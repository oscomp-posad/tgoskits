use alloc::sync::Arc;

use ax_kspin::SpinRaw as Mutex;
use rdrive::{DriverGeneric, KError};
use rockchip_soc::{ClkId, ClockOp, Cru, ResetOp, RstId};

mod rk3568;
mod rk3588;

type SharedCru = Arc<Mutex<Cru>>;

/// A process-wide handle to the registered CRU, stashed at probe time so
/// non-rdif callers (e.g. the display cold-init) can reach CRU operations that
/// aren't exposed through the `rdif_clk`/`rdif_reset` wrappers (like the VOP
/// display-clock setup and the HDPTX PHY resets).
static SHARED_CRU: Mutex<Option<SharedCru>> = Mutex::new(None);

pub(crate) fn set_shared_cru(cru: SharedCru) {
    *SHARED_CRU.lock() = Some(cru);
}

/// Run `f` with the registered CRU, if one has been probed.
pub fn with_cru<R>(f: impl FnOnce(&mut Cru) -> R) -> Option<R> {
    let guard = SHARED_CRU.lock();
    let cru = guard.as_ref()?;
    Some(f(&mut cru.lock()))
}

/// Assert a CRU reset line by its DT reset id. Returns `false` if no CRU exists.
pub fn reset_assert(id: usize) -> bool {
    with_cru(|c| c.reset_assert(RstId::from(id))).is_some()
}
/// Deassert a CRU reset line by its DT reset id.
pub fn reset_deassert(id: usize) -> bool {
    with_cru(|c| c.reset_deassert(RstId::from(id))).is_some()
}

/// Enable (ungate) a CRU clock by its DT clock id. Returns `false` if no CRU
/// exists or the id is unknown to the gate table. Ungating an already-running
/// clock is idempotent, so this is safe to call unconditionally on a cold path.
pub fn clk_enable(id: usize) -> bool {
    with_cru(|c| c.clk_enable(ClkId::from(id)).is_ok()).unwrap_or(false)
}

pub struct ClkDrv {
    name: &'static str,
    inner: SharedCru,
}

impl ClkDrv {
    pub fn new(name: &'static str, cru: SharedCru) -> Self {
        Self { name, inner: cru }
    }
}

pub struct ResetDrv {
    name: &'static str,
    inner: SharedCru,
}

impl ResetDrv {
    pub fn new(name: &'static str, cru: SharedCru) -> Self {
        Self { name, inner: cru }
    }
}

impl DriverGeneric for ResetDrv {
    fn name(&self) -> &str {
        self.name
    }
}

unsafe impl Send for ClkDrv {}
unsafe impl Send for ResetDrv {}

impl DriverGeneric for ClkDrv {
    fn name(&self) -> &str {
        self.name
    }
}

impl rdif_clk::Interface for ClkDrv {
    fn perper_enable(&mut self) {}

    fn enable(&mut self, id: rdif_clk::ClockId) -> Result<(), KError> {
        self.inner
            .lock()
            .clk_enable(clock_id(id))
            .map_err(|_| KError::InvalidArg { name: "clock_id" })
    }

    fn get_rate(&self, id: rdif_clk::ClockId) -> Result<u64, KError> {
        self.inner
            .lock()
            .clk_get_rate(clock_id(id))
            .map_err(|_| KError::InvalidArg { name: "clock_id" })
    }

    fn set_rate(&mut self, id: rdif_clk::ClockId, rate: u64) -> Result<(), KError> {
        self.inner
            .lock()
            .clk_set_rate(clock_id(id), rate)
            .map_err(|_| KError::InvalidArg { name: "clock_id" })?;
        Ok(())
    }
}

impl rdif_reset::Interface for ResetDrv {
    fn assert(&mut self, id: rdif_reset::ResetId) -> Result<(), rdif_reset::ResetError> {
        self.inner.lock().reset_assert(reset_id(id));
        Ok(())
    }

    fn deassert(&mut self, id: rdif_reset::ResetId) -> Result<(), rdif_reset::ResetError> {
        self.inner.lock().reset_deassert(reset_id(id));
        Ok(())
    }
}

fn clock_id(id: rdif_clk::ClockId) -> ClkId {
    let id: usize = id.into();
    ClkId::from(id)
}

fn reset_id(id: rdif_reset::ResetId) -> RstId {
    RstId::from(id.raw())
}
