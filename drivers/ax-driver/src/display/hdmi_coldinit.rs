//! Full RK3588 HDMI0 cold-init orchestration (Stage 4c): own the HDPTX PHY +
//! DW-HDMI-QP TX + CRU dclk + GRF muxing to light a 1080p60 RGB output *without*
//! reusing any U-Boot display state. Ties together the driver cores:
//! `rockchip-hdptx-phy` (PHY), `rockchip-soc` (CRU dclk), `rockchip-vop2`
//! (modeset) and `dw-hdmi-qp` (TX), plus raw GRF pokes.
//!
//! ## Board-validation status
//! Nothing here has run on hardware. The MMIO addresses, reset ids and GRF
//! writes are hardcoded for the OrangePi-5-Plus / RK3588 (from the board DTB +
//! mainline), and the *ordering* is a best-effort mirror of mainline — both are
//! expected to need tuning against the board (the PHY lock-poll bits and a
//! register dump are the oracle). This compiles + links so the board session is
//! "debug against lock bits", not "write from scratch". It is opt-in
//! (`rockchip-vop2-coldinit` feature) and never on a default boot path.

use core::time::Duration;

use log::{info, warn};
use rockchip_hdptx_phy::{
    PhyEnv, PhyResets, ResetLine, mmio::MmioRegs as PhyMmio, power_on_1080p60,
};
use rockchip_vop2::{mmio::MmioRegs as VopMmio, mode::DisplayMode, modeset::modeset_vp0};

use crate::mmio::iomap;

// --- Board-fixed MMIO regions (RK3588 / OrangePi-5-Plus, from the DTB) ---
const PHY_BASE: usize = 0xfed6_0000;
const PHY_SIZE: usize = 0x2000;
const HDPTX_GRF_BASE: usize = 0xfd5e_0000;
const TX_BASE: usize = 0xfde8_0000;
const TX_SIZE: usize = 0x1_0000;
const VOP_GRF_BASE: usize = 0xfd5a_4000;
const VO1_GRF_BASE: usize = 0xfd5a_8000;
const SYS_GRF_BASE: usize = 0xfd58_c000;
const GRF_SIZE: usize = 0x1000;

// --- Power-domain ids (rdif) ---
const PD_VOP: u32 = 24;
const PD_VO1: u32 = 26;

// --- HDPTX PHY CRU reset ids (DT `resets` of phy@fed60000: apb/init/cmn/lane) ---
const RST_HDPTX_APB: usize = 0x485;
const RST_HDPTX_INIT: usize = 0xc003b;
const RST_HDPTX_CMN: usize = 0xc003c;
const RST_HDPTX_LANE: usize = 0xc003d;

// --- GRF register offsets + hiword-masked write values (mainline) ---
// VOP_GRF CON2: route VP output to the HDMI0 TX (BIT1).
const VOP_GRF_CON2: usize = 0x08;
const VOP_GRF_CON2_HDMI0: u32 = 0x0002_0002;
// VO1_GRF CON0: HDMI0 sync polarity (from Stage-2 research; board-tunable).
const VO1_GRF_CON0: usize = 0x00;
const VO1_GRF_CON0_HDMI0_POL: u32 = 0x0060_0020;
// VO1_GRF CON3: DDC SDA/SCL pin mux + HDMI mode + I2S sel; COLOR_DEPTH[7:4]=0 (8bpc).
const VO1_GRF_CON3: usize = 0x0c;
const VO1_GRF_CON3_IO: u32 = 0x2e00_2e00;
// VO1_GRF CON9: HDMI0 grant select (BIT10).
const VO1_GRF_CON9: usize = 0x24;
const VO1_GRF_CON9_GRANT: u32 = 0x0400_0400;
// SYS_GRF CON7: HPD IO enable (BIT12|BIT13).
const SYS_GRF_CON7: usize = 0x31c;
const SYS_GRF_CON7_HPD_IO: u32 = 0x3000_3000;

/// CRU-backed PHY reset lines (uses the process-wide CRU handle).
struct CruResets;
impl PhyResets for CruResets {
    fn assert(&mut self, line: ResetLine) {
        crate::soc::rockchip::cru::reset_assert(reset_id(line));
    }
    fn deassert(&mut self, line: ResetLine) {
        crate::soc::rockchip::cru::reset_deassert(reset_id(line));
    }
}

fn reset_id(line: ResetLine) -> usize {
    match line {
        ResetLine::Apb => RST_HDPTX_APB,
        ResetLine::Init => RST_HDPTX_INIT,
        ResetLine::Cmn => RST_HDPTX_CMN,
        ResetLine::Lane => RST_HDPTX_LANE,
    }
}

/// Kernel timing env: busy-wait microsecond delays for the PHY sequence + polls.
struct KEnv;
impl PhyEnv for KEnv {
    fn delay_us(&self, us: u32) {
        axklib::time::busy_wait(Duration::from_micros(us as u64));
    }
}

/// One-shot hiword-masked write to a GRF syscon register.
fn grf_write(base: usize, off: usize, val: u32) -> Result<(), &'static str> {
    let p = iomap(base, GRF_SIZE).map_err(|_| "grf iomap failed")?;
    // SAFETY: `off` is within the mapped GRF_SIZE page; GRF regs are 32-bit.
    unsafe { core::ptr::write_volatile(p.as_ptr().add(off) as *mut u32, val) };
    Ok(())
}

/// Cold-init HDMI0 for a 1080p60 RGB output displaying `fb_phys`, from scratch
/// (no U-Boot reuse). Programs, in order: power domains -> HDPTX PHY (blocks on
/// PLL/lane lock) -> CRU dclk mux + VOP gates -> GRF muxes -> VOP2 modeset ->
/// TX enable. Returns the first failing step; the caller keeps the fb registered
/// regardless so /dev/fb0 exists.
pub fn cold_init_hdmi0_1080p60(vop_mmio: &mut VopMmio, fb_phys: u64) -> Result<(), &'static str> {
    let mode = DisplayMode::fhd_vp0();

    // 1. Power the VOP + VO1 domains (cold boot: U-Boot did not).
    match crate::soc::rockchip::pm::power_on_domain(PD_VOP) {
        Ok(true) => info!("coldinit: VOP power domain on"),
        Ok(false) => warn!("coldinit: no power provider; assuming VOP powered"),
        Err(e) => warn!("coldinit: VOP power_on failed: {e:?}"),
    }
    let _ = crate::soc::rockchip::pm::power_on_domain(PD_VO1);

    // 2. HDPTX PHY: generate the 148.5 MHz TMDS + pixel clock. Blocks on the
    //    two GRF lock polls (the on-board pass/fail oracle).
    let phy_ptr = iomap(PHY_BASE, PHY_SIZE).map_err(|_| "phy iomap failed")?;
    let grf_ptr = iomap(HDPTX_GRF_BASE, GRF_SIZE).map_err(|_| "hdptx-grf iomap failed")?;
    // SAFETY: freshly-mapped device regions of the given sizes.
    let mut phy = unsafe { PhyMmio::new(phy_ptr.as_ptr(), PHY_SIZE) };
    let mut phy_grf = unsafe { PhyMmio::new(grf_ptr.as_ptr(), GRF_SIZE) };
    let mut resets = CruResets;
    power_on_1080p60(&mut phy, &mut phy_grf, &mut resets, &KEnv).map_err(|e| match e {
        rockchip_hdptx_phy::PhyError::PllClkTimeout => "phy PLL clk timeout",
        rockchip_hdptx_phy::PhyError::PllLockTimeout => "phy PLL/lane lock timeout",
    })?;
    info!("coldinit: HDPTX PHY locked @ 148.5MHz");

    // 3. CRU: mux DCLK_VOP0 to the now-running PHY pixel clock + ungate VOP.
    if crate::soc::rockchip::cru::with_cru(|c| c.vop_hdmi0_clocks_setup()).is_none() {
        warn!("coldinit: no CRU handle; VOP dclk not muxed");
    }

    // 4. GRF: route VP0->HDMI0 + polarity + DDC/HPD io + color depth (8bpc RGB).
    grf_write(VOP_GRF_BASE, VOP_GRF_CON2, VOP_GRF_CON2_HDMI0)?;
    grf_write(VO1_GRF_BASE, VO1_GRF_CON0, VO1_GRF_CON0_HDMI0_POL)?;
    grf_write(VO1_GRF_BASE, VO1_GRF_CON3, VO1_GRF_CON3_IO)?;
    grf_write(VO1_GRF_BASE, VO1_GRF_CON9, VO1_GRF_CON9_GRANT)?;
    grf_write(SYS_GRF_BASE, SYS_GRF_CON7, SYS_GRF_CON7_HPD_IO)?;

    // 5. VOP2 modeset (timing + Esmart0 window + overlay + DSP_IF_EN mux).
    modeset_vp0(vop_mmio, fb_phys, &mode).map_err(|_| "vop2 modeset failed")?;
    info!("coldinit: VOP2 VP0 modeset done");

    // 6. HDMI-QP TX enable (HDMI op-mode + AVI infoframe).
    let tx_ptr = iomap(TX_BASE, TX_SIZE).map_err(|_| "tx iomap failed")?;
    // SAFETY: freshly-mapped device region.
    let mut tx = unsafe { dw_hdmi_qp::mmio::MmioRegs::new(tx_ptr.as_ptr(), TX_SIZE) };
    dw_hdmi_qp::tx::enable(
        &mut tx,
        dw_hdmi_qp::tx::OpMode::Hdmi,
        &dw_hdmi_qp::avi::AviInfoframe::fhd60_rgb(),
    );
    info!("coldinit: HDMI-QP TX enabled -> 1080p60 RGB should be live");

    Ok(())
}
