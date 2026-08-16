//! Full from-scratch VP→HDMI0 modeset (Stage 2), the "adopt-and-reprogram"
//! path. Reprograms VOP2's video-port timing, DSP control, the Esmart0 window,
//! the overlay window→VP routing, and the VP→HDMI0 output-interface enable,
//! while **reusing U-Boot's already-running** clock tree (dclk), GRF interface
//! mux, HDMI TX and PHY. This extends the Stage-1 "adopt-and-repoint" idea to a
//! full VP+window config without touching the CRU or the separate GRF syscons.
//!
//! Every register value is transcribed from mainline (`vop2_crtc_atomic_enable`,
//! `vop2_plane_atomic_update`, `rk3568_vop2_setup_layer_mixer`,
//! `rk3588_set_intf_mux`) for RK3588 1080p60 XRGB8888 on VP0→HDMI0, and is
//! host-tested. The *values* are mainline-derived; the *effect on hardware* is
//! pending board validation (a dclk must already be running for STANDBY-clear to
//! take effect at the next frame — which the adopt strategy guarantees).

use crate::{
    Vop2Error,
    mmio::Regs,
    mode::DisplayMode,
    regs::{REG_CFG_DONE, cfg_done_value, dsp_if, esmart, ovl, vp, win_base},
    window::program_vp_timing,
};

/// `DSP_CTRL` value for RGB (AAAA) output with scanout enabled (STANDBY = 0).
pub const fn vp_dsp_ctrl_rgb() -> u32 {
    vp::DSP_CTRL_OUT_MODE_AAAA
}

/// Esmart `REGION0_CTRL`: window enabled, XRGB8888 format, no swaps.
pub const fn esmart_region0_ctrl_xrgb8888() -> u32 {
    esmart::REGION0_CTRL_WIN_EN | (esmart::FORMAT_ARGB8888 << esmart::REGION0_CTRL_FORMAT_SHIFT)
}

/// Esmart0 `CTRL1` carrying the RK3588 per-window AXI read-ids (required on
/// RK3588 so windows on one AXI bus don't collide on the default id).
pub const fn esmart0_ctrl1_axi_ids() -> u32 {
    (esmart::ESMART0_UV_R_ID << esmart::CTRL1_UV_R_ID_SHIFT)
        | (esmart::ESMART0_YRGB_R_ID << esmart::CTRL1_YRGB_R_ID_SHIFT)
}

/// Program the Esmart0 window to display `fb_phys` fullscreen for `mode`.
pub fn setup_esmart0<R: Regs>(regs: &mut R, fb_phys: u32, mode: &DisplayMode) {
    let b = win_base::ESMART0;
    regs.write32(b + esmart::CTRL1, esmart0_ctrl1_axi_ids());
    regs.write32(b + esmart::AXI_CTRL, 0); // axi_bus_id = 0 (Esmart0 on AXI0)
    regs.write32(b + esmart::REGION0_YRGB_MST, fb_phys);
    regs.write32(b + esmart::REGION0_VIR, mode.vir_words());
    let act = mode.act_info(); // (h-1)<<16 | (w-1)
    regs.write32(b + esmart::REGION0_ACT_INFO, act);
    regs.write32(b + esmart::REGION0_DSP_INFO, act);
    // Window position within the VP active area: (0,0) for a fullscreen plane.
    regs.write32(b + esmart::REGION0_DSP_ST, 0);
    regs.write32(b + esmart::REGION0_CTRL, esmart_region0_ctrl_xrgb8888());
}

/// Route Esmart0 into video port `vp_id` as its layer-0 and select RGB overlay
/// output. Read-modify-write throughout, to preserve U-Boot's routing of any
/// other windows/ports.
pub fn route_esmart0_to_vp<R: Regs>(regs: &mut R, vp_id: u8) {
    // LAYER_SEL: place Esmart0's layer_sel_id (4) into mixer layer 0. Clear only
    // the low 3 bits of the nibble (mainline `LAYER(id, 0x7)`) — layer_sel_ids
    // are 0..=7, so the 4th bit is reserved and preserved.
    let mut layer_sel = regs.read32(ovl::LAYER_SEL);
    let shift = ovl::layer_sel_shift(0);
    layer_sel &= !(0x7 << shift);
    layer_sel |= ovl::ESMART0_LAYER_SEL_ID << shift;
    regs.write32(ovl::LAYER_SEL, layer_sel);

    // NOTE: PORT_SEL's PORT0_MUX field (bits[3:0] = VP0's mixer layer count - 1)
    // is intentionally left as U-Boot programmed it. The adopt strategy assumes
    // U-Boot lit VP0->HDMI0 (so PORT0_MUX, dclk, TX and PHY are all up); if the
    // board shows no output despite a correct-looking modeset, an unset
    // PORT0_MUX is the first thing to program (value is DT/VP-count dependent:
    // win_size(8)/nvps - 1).

    // PORT_SEL: window→VP field for Esmart0 = vp_id.
    let mut port_sel = regs.read32(ovl::PORT_SEL);
    port_sel &= !ovl::PORT_SEL_ESMART0_MASK;
    port_sel |= (vp_id as u32) << ovl::PORT_SEL_ESMART0_SHIFT;
    regs.write32(ovl::PORT_SEL, port_sel);

    // OVL_CTRL: clear YUV_MODE(vp) so the mixer emits RGB.
    let mut ovl_ctrl = regs.read32(ovl::CTRL);
    ovl_ctrl &= !(1 << vp_id);
    regs.write32(ovl::CTRL, ovl_ctrl);

    // SMART_DLY_NUM: Esmart0 delay (bits [7:0]).
    let mut dly = regs.read32(ovl::SMART_DLY_NUM);
    dly &= !ovl::SMART_DLY_ESMART0_MASK;
    dly |= ovl::ESMART0_DLY;
    regs.write32(ovl::SMART_DLY_NUM, dly);
}

/// Enable video port `vp_id` → HDMI0 at the output interface (RMW: preserves the
/// other interfaces), and force the DSP_IF bank to latch immediately.
pub fn route_vp_to_hdmi0<R: Regs>(regs: &mut R, vp_id: u8) {
    // Interface clock dividers for VP{vp_id} -> HDMI0 (RK3588's `set_intf_mux`
    // does these; a from-scratch modeset that omits them makes the TX transmit a
    // valid signal but BLACK video because VP0's pixels clock out at the wrong
    // rate). Our DCLK_VOP0 is muxed directly to the HDPTX pixel clock, so the VP
    // core runs at dclk/4 and the HDMI0 interface pixclk at dclk/1.
    regs.write32(vp::base(vp_id) + vp::CLK_CTRL, vp::CLK_CTRL_DCLK_CORE_DIV4);
    let mut ctrl = regs.read32(dsp_if::CTRL);
    ctrl &= !dsp_if::CTRL_HDMI0_DIV_MASK;
    ctrl |= dsp_if::CTRL_HDMI0_DCLK4_PCLK2; // 0x60000 = mainline (dclk/4, pixclk/2)
    regs.write32(dsp_if::CTRL, ctrl);

    let mut en = regs.read32(dsp_if::EN);
    en &= !dsp_if::EN_EDP_HDMI0_MUX_MASK;
    en |= dsp_if::EN_HDMI0 | ((vp_id as u32) << dsp_if::EN_EDP_HDMI0_MUX_SHIFT);
    regs.write32(dsp_if::EN, en);

    let pol = regs.read32(dsp_if::POL) | dsp_if::POL_CFG_DONE_IMD;
    regs.write32(dsp_if::POL, pol);
}

/// Full VP modeset for `mode` displaying `fb_phys`, adopting U-Boot's running
/// clock/GRF/TX/PHY. Programs timing → DSP control → the Esmart0 window →
/// overlay routing → output-interface enable, then latches with one config-done.
/// `fb_phys` must be < 4 GiB (VOP2's YRGB_MST is a 32-bit byte address); rejected
/// before any register write otherwise.
pub fn modeset_vp0<R: Regs>(
    regs: &mut R,
    fb_phys: u64,
    mode: &DisplayMode,
) -> Result<(), Vop2Error> {
    if !fb_phys.is_multiple_of(4) {
        return Err(Vop2Error::MisalignedFramebuffer);
    }
    if fb_phys > u32::MAX as u64 {
        return Err(Vop2Error::FramebufferAbove4Gib);
    }
    let fb = fb_phys as u32;
    let vp_id = mode.vp;
    let vbase = vp::base(vp_id);

    // 0. Power the VOP2-internal window-memory domains and disable auto
    //    clock-gating BEFORE any window/VP configuration (mainline
    //    `vop2_enable` order). Cold reset leaves SYS_PD_CTRL=0xff — a
    //    powered-down window fetches zeros, i.e. a full-screen black window.
    let pd = regs.read32(crate::regs::SYS_PD_CTRL);
    regs.write32(crate::regs::SYS_PD_CTRL, pd & !crate::regs::PD_ALL_WINDOWS);
    let ag = regs.read32(crate::regs::SYS_AUTO_GATING_CTRL);
    regs.write32(
        crate::regs::SYS_AUTO_GATING_CTRL,
        ag & !crate::regs::AUTO_GATING_EN,
    );

    // 1. VP display timing (values verified against mainline in `timing`).
    program_vp_timing(regs, mode);

    // 2. VP control: RGB (AAAA) out, MIPI off, black background, STANDBY cleared.
    //    Also disable the VP hardware color-bar test pattern: U-Boot leaves it
    //    enabled for its own bring-up, and while set the VP emits an internal
    //    color-bar pattern that overrides the composited window (the framebuffer
    //    is ignored). Clearing it makes the VP scan our Esmart0 window/fb.
    regs.write32(vbase + vp::COLOR_BAR_CTRL, 0);
    regs.write32(vbase + vp::MIPI_CTRL, 0);
    regs.write32(vbase + vp::DSP_BG, 0);
    regs.write32(vbase + vp::DSP_CTRL, vp_dsp_ctrl_rgb());

    // 3. The Esmart0 source window.
    setup_esmart0(regs, fb, mode);

    // 4. Overlay window→VP routing + VP→HDMI0 output enable.
    route_esmart0_to_vp(regs, vp_id);
    route_vp_to_hdmi0(regs, vp_id);

    // 5. Latch this VP's shadow registers.
    regs.write32(REG_CFG_DONE, cfg_done_value(vp_id));
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mmio::tests::FakeRegs;

    #[test]
    fn value_builders_match_mainline() {
        assert_eq!(vp_dsp_ctrl_rgb(), 0x0000_000F);
        assert_eq!(esmart_region0_ctrl_xrgb8888(), 0x0000_0001);
        // yrgb_r_id 0x0a at [8:4], uv_r_id 0x0b at [16:12] -> 0xB0A0
        assert_eq!(esmart0_ctrl1_axi_ids(), (0x0b << 12) | (0x0a << 4));
        assert_eq!(esmart0_ctrl1_axi_ids(), 0x0000_B0A0);
    }

    #[test]
    fn setup_esmart0_programs_window() {
        let mut r = FakeRegs::new(0x2000);
        let m = DisplayMode::fhd_vp0();
        setup_esmart0(&mut r, 0x8000_0000, &m);
        let b = win_base::ESMART0;
        assert_eq!(r.read32(b + esmart::REGION0_YRGB_MST), 0x8000_0000);
        assert_eq!(r.read32(b + esmart::REGION0_VIR), 1920); // 0x780
        assert_eq!(r.read32(b + esmart::REGION0_ACT_INFO), (1079 << 16) | 1919); // 0x0437077F
        assert_eq!(r.read32(b + esmart::REGION0_DSP_INFO), (1079 << 16) | 1919);
        assert_eq!(r.read32(b + esmart::REGION0_DSP_ST), 0);
        assert_eq!(r.read32(b + esmart::REGION0_CTRL), 1);
        assert_eq!(r.read32(b + esmart::CTRL1), 0xB0A0);
    }

    #[test]
    fn route_esmart0_sets_layer_and_port_preserving_others() {
        let mut r = FakeRegs::new(0x2000);
        // Pre-existing unrelated routing that must survive the RMW.
        r.write32(ovl::LAYER_SEL, 0x3300_0000); // upper nibbles occupied
        r.write32(ovl::PORT_SEL, 0x00FF_0000); // other window→VP bits set
        route_esmart0_to_vp(&mut r, 0);
        // layer0 nibble == Esmart0 id (4), upper nibbles preserved.
        assert_eq!(r.read32(ovl::LAYER_SEL) & 0xF, 4);
        assert_eq!(r.read32(ovl::LAYER_SEL) & 0xFF00_0000, 0x3300_0000);
        // Esmart0 field [25:24] == 0 (VP0); other window bits preserved.
        assert_eq!(r.read32(ovl::PORT_SEL) & (0b11 << 24), 0);
        assert_eq!(r.read32(ovl::PORT_SEL) & 0x00FC_0000, 0x00FC_0000);
        // Esmart0 delay set.
        assert_eq!(r.read32(ovl::SMART_DLY_NUM) & 0xFF, 23);
    }

    #[test]
    fn route_vp_to_hdmi0_enables_and_muxes() {
        let mut r = FakeRegs::new(0x2000);
        r.write32(dsp_if::EN, 0x0003_0000); // mux field pre-set to VP3, must clear
        route_vp_to_hdmi0(&mut r, 0);
        assert_eq!(r.read32(dsp_if::EN) & dsp_if::EN_HDMI0, dsp_if::EN_HDMI0);
        assert_eq!(r.read32(dsp_if::EN) & dsp_if::EN_EDP_HDMI0_MUX_MASK, 0); // VP0
        assert_eq!(
            r.read32(dsp_if::POL) & dsp_if::POL_CFG_DONE_IMD,
            dsp_if::POL_CFG_DONE_IMD
        );
    }

    #[test]
    fn modeset_vp0_full_sequence_latches() {
        let mut r = FakeRegs::new(0x2000);
        let m = DisplayMode::fhd_vp0();
        modeset_vp0(&mut r, 0x8000_0000, &m).unwrap();
        let vbase = vp::base(0);
        // timing
        assert_eq!(r.read32(vbase + vp::DSP_HTOTAL_HS_END), (2200 << 16) | 44);
        // control
        assert_eq!(r.read32(vbase + vp::DSP_CTRL), 0xF);
        assert_eq!(r.read32(vbase + vp::MIPI_CTRL), 0);
        // window enabled at our fb
        assert_eq!(
            r.read32(win_base::ESMART0 + esmart::REGION0_YRGB_MST),
            0x8000_0000
        );
        assert_eq!(r.read32(win_base::ESMART0 + esmart::REGION0_CTRL), 1);
        // output route
        assert_eq!(r.read32(dsp_if::EN) & dsp_if::EN_HDMI0, dsp_if::EN_HDMI0);
        // latched
        assert_eq!(r.read32(REG_CFG_DONE), cfg_done_value(0));
    }

    #[test]
    fn modeset_rejects_bad_fb() {
        let mut r = FakeRegs::new(0x2000);
        let m = DisplayMode::fhd_vp0();
        assert_eq!(
            modeset_vp0(&mut r, 0x1_0000_0000, &m),
            Err(Vop2Error::FramebufferAbove4Gib)
        );
        assert_eq!(
            modeset_vp0(&mut r, 0x8000_0001, &m),
            Err(Vop2Error::MisalignedFramebuffer)
        );
    }
}
