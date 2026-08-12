//! HDPTX PHY power-up state machine for a fixed 1080p60 HDMI TMDS output.
//! Mirrors mainline `rk_hdptx_tmds_ropll_cmn_config` -> `rk_hdptx_post_enable_pll`
//! -> (HDMI mode select) -> `rk_hdptx_tmds_ropll_mode_config` ->
//! `rk_hdptx_post_enable_lane`, in order.
//!
//! The CRU resets and the `usleep`/poll timing are host-abstracted: the caller
//! provides [`PhyResets`] (the four CRU reset lines) and [`PhyEnv`] (a `delay_us`
//! that also paces the status polls). All register/GRF values come from `seqs`,
//! `ropll` and `grf`. Board-validation pending — see the crate docs.

use crate::{
    grf,
    mmio::Regs,
    ropll::{RopllConfig, apply_cmn_dividers},
    seqs,
};

/// The four CRU reset lines the PHY toggles (by mainline reset name).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ResetLine {
    Apb,
    Init,
    Cmn,
    Lane,
}

/// The CRU reset lines feeding the PHY (assert/deassert). Implemented by the OS
/// glue over the CRU driver.
pub trait PhyResets {
    fn assert(&mut self, line: ResetLine);
    fn deassert(&mut self, line: ResetLine);
}

/// Timing environment: a microsecond delay used for the `usleep` gaps and as the
/// poll interval.
pub trait PhyEnv {
    fn delay_us(&self, us: u32);
}

/// PHY bring-up failure.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PhyError {
    /// `PHY_CLK_RDY` never asserted after `post_enable_pll` (400 us).
    PllClkTimeout,
    /// `PHY_RDY & PLL_LOCK_DONE` never asserted after `post_enable_lane` (5 ms).
    PllLockTimeout,
}

/// PHY register byte offset for LNTOP index `n` (`LNTOP_REG(n) = n*4`).
const fn lntop(n: u16) -> usize {
    (n as usize) * 4
}

fn apply_seq<R: Regs>(regs: &mut R, seq: seqs::Seq) {
    for &(idx, val) in seq {
        regs.write32((idx as usize) * 4, val as u32);
    }
}

/// Poll `grf`'s STATUS register until `ready(status)` is true, sleeping
/// `interval_us` between reads, up to `timeout_us`.
fn poll_status<G: Regs, E: PhyEnv>(
    grf: &G,
    env: &E,
    interval_us: u32,
    timeout_us: u32,
    ready: impl Fn(u32) -> bool,
) -> bool {
    let mut elapsed = 0u32;
    loop {
        if ready(grf.read32(grf::STATUS)) {
            return true;
        }
        if elapsed >= timeout_us {
            return false;
        }
        env.delay_us(interval_us);
        elapsed += interval_us;
    }
}

/// Bring the HDPTX0 PHY up for a 1080p60 (148.5 MHz) HDMI TMDS output.
///
/// `phy` = the PHY register block (`0xfed60000`), `grf` = the HDPTX GRF syscon
/// (`0xfd5e0000`), `resets` = the four CRU reset lines, `env` = timing. Assumes
/// the `ref` (24 MHz) and `apb` clocks are already enabled and the block is out
/// of APB/CMN/INIT reset from probe (this fn re-cycles them as mainline does).
pub fn power_on_1080p60<R: Regs, G: Regs, RS: PhyResets, E: PhyEnv>(
    phy: &mut R,
    grf: &mut G,
    resets: &mut RS,
    env: &E,
) -> Result<(), PhyError> {
    let cfg = RopllConfig::fhd60();

    // --- pre_power_up ---
    resets.assert(ResetLine::Apb);
    env.delay_us(25);
    resets.deassert(ResetLine::Apb);
    resets.assert(ResetLine::Lane);
    resets.assert(ResetLine::Cmn);
    resets.assert(ResetLine::Init);
    grf.write32(grf::CON0, grf::CON0_PRE_POWER_UP);

    // --- cmn_config ---
    grf.write32(grf::CON0, grf::CON0_LC_REF_CLK);
    apply_seq(phy, seqs::COMMON_CMN_INIT);
    apply_seq(phy, seqs::TMDS_CMN_INIT);
    apply_cmn_dividers(phy, &cfg);

    // --- post_enable_pll ---
    grf.write32(grf::CON0, grf::CON0_BIAS_BGR);
    env.delay_us(15);
    resets.deassert(ResetLine::Init);
    env.delay_us(15);
    grf.write32(grf::CON0, grf::CON0_PLL_EN);
    env.delay_us(15);
    resets.deassert(ResetLine::Cmn);
    if !poll_status(grf, env, 20, 400, |s| s & grf::O_PHY_CLK_RDY != 0) {
        // Unwind the still-asserted Lane reset so a retry starts from a clean
        // state (Apb/Init/Cmn are already deasserted by this point).
        resets.deassert(ResetLine::Lane);
        return Err(PhyError::PllClkTimeout);
    }

    // --- HDMI mode select ---
    grf.write32(grf::CON0, grf::CON0_MODE_HDMI);

    // --- mode_config (1/10 bit-rate / lowbr path, 148.5 < 340 MHz) ---
    apply_seq(phy, seqs::COMMON_SB_INIT);
    phy.write32(lntop(0x0200), 0x06);
    apply_seq(phy, seqs::TMDS_LNTOP_LOWBR);
    phy.write32(lntop(0x0206), 0x07);
    apply_seq(phy, seqs::COMMON_LANE_INIT);
    apply_seq(phy, seqs::TMDS_LANE_INIT);

    // --- post_enable_lane ---
    resets.deassert(ResetLine::Lane);
    grf.write32(grf::CON0, grf::CON0_BIAS_BGR);
    phy.write32(lntop(0x0207), 0x0f); // all 4 lanes enabled
    if !poll_status(grf, env, 100, 5000, |s| {
        s & grf::O_PHY_RDY != 0 && s & grf::O_PLL_LOCK_DONE != 0
    }) {
        return Err(PhyError::PllLockTimeout);
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mmio::tests::FakeRegs;

    struct NoDelay;
    impl PhyEnv for NoDelay {
        fn delay_us(&self, _us: u32) {}
    }

    #[derive(Default)]
    struct RecordResets {
        events: std::vec::Vec<(ResetLine, bool)>,
    }
    impl PhyResets for RecordResets {
        fn assert(&mut self, line: ResetLine) {
            self.events.push((line, true));
        }
        fn deassert(&mut self, line: ResetLine) {
            self.events.push((line, false));
        }
    }
    extern crate std;

    /// A GRF fake whose STATUS becomes "locked" after N reads (simulating the PLL
    /// eventually locking), or never (timeout).
    struct LockingGrf {
        words: std::vec::Vec<u32>,
        reads_until_ready: core::cell::Cell<i32>,
    }
    impl LockingGrf {
        fn new(reads_until_ready: i32) -> Self {
            Self {
                words: std::vec![0u32; 0x40],
                reads_until_ready: core::cell::Cell::new(reads_until_ready),
            }
        }
    }
    impl Regs for LockingGrf {
        fn read32(&self, off: usize) -> u32 {
            if off == grf::STATUS {
                let n = self.reads_until_ready.get();
                self.reads_until_ready.set(n - 1);
                if n <= 0 {
                    // both clk-ready and lane-lock bits set once "locked"
                    return grf::O_PHY_CLK_RDY | grf::O_PHY_RDY | grf::O_PLL_LOCK_DONE;
                }
                return 0;
            }
            self.words[off / 4]
        }
        fn write32(&mut self, off: usize, val: u32) {
            self.words[off / 4] = val;
        }
    }

    #[test]
    fn power_on_applies_all_193_seq_writes_and_latches_lntop() {
        let mut phy = FakeRegs::new(0x2000);
        let mut grf = LockingGrf::new(0); // ready immediately
        let mut rs = RecordResets::default();
        power_on_1080p60(&mut phy, &mut grf, &mut rs, &NoDelay).unwrap();

        // Every fixed-seq write landed (69+43+4+5+52+20 = 193) plus dividers +
        // the 3 explicit LNTOP writes; assert the LNTOP enable + a seq value.
        assert_eq!(phy.read32(0x0200 * 4), 0x06);
        assert_eq!(phy.read32(0x0206 * 4), 0x07);
        assert_eq!(phy.read32(0x0207 * 4), 0x0f); // all lanes
        // first common_cmn write: CMN(0009)=0x0c -> offset 0x24
        assert_eq!(phy.read32(0x24), 0x0c);
        // divider applied
        assert_eq!(phy.read32(0x0051 * 4), 0x7b);
    }

    #[test]
    fn reset_order_matches_mainline() {
        let mut phy = FakeRegs::new(0x2000);
        let mut grf = LockingGrf::new(0);
        let mut rs = RecordResets::default();
        power_on_1080p60(&mut phy, &mut grf, &mut rs, &NoDelay).unwrap();
        use ResetLine::*;
        // APB cycle, then assert LANE/CMN/INIT, later deassert INIT, CMN, LANE.
        assert_eq!(
            rs.events,
            std::vec![
                (Apb, true),
                (Apb, false),
                (Lane, true),
                (Cmn, true),
                (Init, true),
                (Init, false),
                (Cmn, false),
                (Lane, false),
            ]
        );
    }

    #[test]
    fn pll_clk_timeout_when_never_ready() {
        let mut phy = FakeRegs::new(0x2000);
        // never ready within the clk-ready poll window (400us/20us = 20 reads);
        // set the threshold high so the first poll times out.
        let mut grf = LockingGrf::new(10_000);
        let mut rs = RecordResets::default();
        assert_eq!(
            power_on_1080p60(&mut phy, &mut grf, &mut rs, &NoDelay),
            Err(PhyError::PllClkTimeout)
        );
    }
}
