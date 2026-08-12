//! VOP2 window model + the "detect live window and repoint" operation.

use crate::{
    Vop2Error,
    mmio::Regs,
    mode::DisplayMode,
    regs::{REG_CFG_DONE, cfg_done_value, cluster, esmart, intr, vp, win_base},
};

/// The two window families VOP2 exposes. U-Boot logos use Cluster0 or Esmart0.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WindowKind {
    Cluster,
    Esmart,
}

/// A concrete window: its family + MMIO base offset + its key register offsets.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Window {
    pub kind: WindowKind,
    pub base: usize,
    pub yrgb_mst: usize,
    pub vir: usize,
    pub act_info: usize,
}

impl Window {
    pub const fn cluster0() -> Self {
        Self {
            kind: WindowKind::Cluster,
            base: win_base::CLUSTER0,
            yrgb_mst: win_base::CLUSTER0 + cluster::YRGB_MST,
            vir: win_base::CLUSTER0 + cluster::VIR,
            act_info: win_base::CLUSTER0 + cluster::ACT_INFO,
        }
    }
    pub const fn esmart0() -> Self {
        Self {
            kind: WindowKind::Esmart,
            base: win_base::ESMART0,
            yrgb_mst: win_base::ESMART0 + esmart::REGION0_YRGB_MST,
            vir: win_base::ESMART0 + esmart::REGION0_VIR,
            act_info: win_base::ESMART0 + esmart::REGION0_ACT_INFO,
        }
    }

    /// Read the window's current scanout address (0 == not scanning).
    pub fn current_mst<R: Regs>(&self, regs: &R) -> u32 {
        regs.read32(self.yrgb_mst)
    }
}

/// The candidate windows we probe for "who is U-Boot scanning", in priority
/// order (Cluster0 first — vendor U-Boot default — then Esmart0).
pub const CANDIDATES: [Window; 2] = [Window::cluster0(), Window::esmart0()];

/// Find the window whose YRGB_MST is non-zero (i.e. actively scanning a fb).
pub fn detect_live_window<R: Regs>(regs: &R) -> Option<Window> {
    CANDIDATES.into_iter().find(|w| w.current_mst(regs) != 0)
}

/// Repoint `win` to `fb_phys` and latch it via config-done for `mode.vp`.
///
/// Stage 1 keeps every other register as U-Boot left it (timings, enables,
/// overlay routing) and changes only the scanout address + stride/geometry —
/// the minimal, reversible change that proves our fb reaches the panel.
pub fn repoint<R: Regs>(
    regs: &mut R,
    win: Window,
    fb_phys: u64,
    mode: &DisplayMode,
) -> Result<(), Vop2Error> {
    if !fb_phys.is_multiple_of(4) {
        return Err(Vop2Error::MisalignedFramebuffer);
    }
    // YRGB_MST is a 32-bit byte address; VOP2 can only scan the low 4 GiB. The
    // caller's DMA mask should already guarantee this, but reject explicitly
    // rather than silently truncate `fb_phys as u32` (which would point the
    // scanout engine at the wrong physical page).
    if fb_phys > u32::MAX as u64 {
        return Err(Vop2Error::FramebufferAbove4Gib);
    }
    regs.write32(win.yrgb_mst, fb_phys as u32);
    regs.write32(win.vir, mode.vir_words());
    regs.write32(win.act_info, mode.act_info());
    regs.write32(REG_CFG_DONE, cfg_done_value(mode.vp));
    Ok(())
}

/// Program `mode.vp`'s display-timing shadow registers from `mode.timing`
/// (Stage 2 modeset). Writes only the timing/active-window registers — the
/// values are transcribed from mainline `vop2_crtc_atomic_enable` — and does
/// **not** clear standby, route the output interface, or enable the window;
/// those steps must be reconciled against the board golden dump before use.
/// The caller latches with a `cfg_done` write.
pub fn program_vp_timing<R: Regs>(regs: &mut R, mode: &DisplayMode) {
    let t = &mode.timing;
    let base = vp::base(mode.vp);
    regs.write32(base + vp::DSP_HTOTAL_HS_END, t.htotal_hs_end());
    regs.write32(base + vp::DSP_HACT_ST_END, t.hact_st_end());
    regs.write32(base + vp::DSP_VTOTAL_VS_END, t.vtotal_vs_end());
    regs.write32(base + vp::DSP_VACT_ST_END, t.vact_st_end());
    regs.write32(base + vp::POST_DSP_HACT_INFO, t.post_hact_info());
    regs.write32(base + vp::POST_DSP_VACT_INFO, t.post_vact_info());
}

/// Enable the frame-start interrupt for video port `vp` (read-modify-write, so
/// other enabled interrupt sources are preserved).
pub fn enable_frame_irq<R: Regs>(regs: &mut R, vp: u8) {
    let reg = intr::vp_int_en(vp);
    let cur = regs.read32(reg);
    regs.write32(reg, cur | intr::FS_NEW_INTR);
}

/// Acknowledge a frame-start interrupt for video port `vp`. Returns `true` if
/// the frame-start bit was set (and clears it), `false` otherwise.
pub fn ack_frame_irq<R: Regs>(regs: &mut R, vp: u8) -> bool {
    let status = regs.read32(intr::vp_int_status(vp));
    if status & intr::FS_NEW_INTR != 0 {
        regs.write32(intr::vp_int_clr(vp), intr::FS_NEW_INTR);
        true
    } else {
        false
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{mmio::tests::FakeRegs, mode::DisplayMode};

    #[test]
    fn detect_prefers_cluster0_when_scanning() {
        let mut r = FakeRegs::new(0x2000);
        r.write32(Window::cluster0().yrgb_mst, 0x1000_0000);
        assert_eq!(detect_live_window(&r).unwrap(), Window::cluster0());
    }

    #[test]
    fn detect_falls_back_to_esmart0() {
        let mut r = FakeRegs::new(0x2000);
        r.write32(Window::esmart0().yrgb_mst, 0x2000_0000);
        assert_eq!(detect_live_window(&r).unwrap(), Window::esmart0());
    }

    #[test]
    fn detect_none_when_idle() {
        let r = FakeRegs::new(0x2000);
        assert!(detect_live_window(&r).is_none());
    }

    #[test]
    fn repoint_writes_addr_stride_actinfo_and_latches() {
        let mut r = FakeRegs::new(0x2000);
        let m = DisplayMode::fhd_vp0();
        let win = Window::esmart0();
        repoint(&mut r, win, 0x8000_0000, &m).unwrap();
        assert_eq!(r.read32(win.yrgb_mst), 0x8000_0000);
        assert_eq!(r.read32(win.vir), 1920);
        assert_eq!(r.read32(win.act_info), (1079 << 16) | 1919);
        assert_eq!(r.read32(REG_CFG_DONE), cfg_done_value(0));
    }

    #[test]
    fn repoint_rejects_misaligned_fb() {
        let mut r = FakeRegs::new(0x2000);
        let m = DisplayMode::fhd_vp0();
        assert_eq!(
            repoint(&mut r, Window::esmart0(), 0x8000_0001, &m),
            Err(Vop2Error::MisalignedFramebuffer)
        );
    }

    #[test]
    fn repoint_rejects_fb_above_4gib() {
        let mut r = FakeRegs::new(0x2000);
        let m = DisplayMode::fhd_vp0();
        // 4 GiB, 4-byte aligned but unrepresentable in the 32-bit YRGB_MST.
        assert_eq!(
            repoint(&mut r, Window::esmart0(), 0x1_0000_0000, &m),
            Err(Vop2Error::FramebufferAbove4Gib)
        );
        // and it must not have written a truncated address.
        assert_eq!(r.read32(Window::esmart0().yrgb_mst), 0);
    }

    #[test]
    fn program_vp_timing_writes_mainline_values() {
        let mut r = FakeRegs::new(0x2000);
        let m = DisplayMode::fhd_vp0();
        program_vp_timing(&mut r, &m);
        let base = vp::base(0);
        assert_eq!(r.read32(base + vp::DSP_HTOTAL_HS_END), (2200 << 16) | 44);
        assert_eq!(r.read32(base + vp::DSP_HACT_ST_END), (192 << 16) | 2112);
        assert_eq!(r.read32(base + vp::DSP_VTOTAL_VS_END), (1125 << 16) | 5);
        assert_eq!(r.read32(base + vp::DSP_VACT_ST_END), (41 << 16) | 1121);
        assert_eq!(r.read32(base + vp::POST_DSP_HACT_INFO), (192 << 16) | 2112);
        assert_eq!(r.read32(base + vp::POST_DSP_VACT_INFO), (41 << 16) | 1121);
    }

    #[test]
    fn enable_frame_irq_sets_bit_preserving_others() {
        let mut r = FakeRegs::new(0x2000);
        r.write32(intr::vp_int_en(0), intr::LINE_FLAG1_INTR); // pre-existing source
        enable_frame_irq(&mut r, 0);
        assert_eq!(
            r.read32(intr::vp_int_en(0)),
            intr::LINE_FLAG1_INTR | intr::FS_NEW_INTR
        );
    }

    #[test]
    fn ack_frame_irq_clears_only_when_set() {
        let mut r = FakeRegs::new(0x2000);
        // No FS pending → returns false, writes nothing to CLR.
        assert!(!ack_frame_irq(&mut r, 0));
        assert_eq!(r.read32(intr::vp_int_clr(0)), 0);
        // FS pending → returns true and clears it.
        r.write32(intr::vp_int_status(0), intr::FS_NEW_INTR);
        assert!(ack_frame_irq(&mut r, 0));
        assert_eq!(r.read32(intr::vp_int_clr(0)), intr::FS_NEW_INTR);
    }
}
