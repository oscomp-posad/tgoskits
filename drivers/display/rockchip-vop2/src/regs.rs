//! RK3588 VOP2 register offsets (bytes, relative to the `regs` MMIO base).
//! Transcribed from mainline `drivers/gpu/drm/rockchip/rockchip_drm_vop2.h`.

/// Writing (a mask into) this register latches all shadow-register writes.
pub const REG_CFG_DONE: usize = 0x000;

/// Global config-done enable (`RK3568_REG_CFG_DONE__GLB_CFG_DONE_EN`, BIT 15) —
/// the master "shadow writes take effect" gate. Mainline `vop2_cfg_done` ORs
/// this into every config-done write; per the mainline comment it has **no**
/// write-mask bit, so a raw write that omits it drives bit 15 to 0 and disables
/// the latch entirely (the commit silently never fires).
pub const GLB_CFG_DONE_EN: u32 = 1 << 15;

/// Value to write to [`REG_CFG_DONE`] to latch video port `vp`'s shadow
/// registers. Matches mainline `vop2_cfg_done`:
/// `GLB_CFG_DONE_EN | BIT(vp) | (BIT(vp) << 16)`. The `BIT(vp) << 16` is the
/// per-VP bit's write-enable mask (so a raw `write32` only affects VP `vp`'s
/// commit bit; other VPs' bits, whose mask bits are 0, are left untouched),
/// while `GLB_CFG_DONE_EN` is written directly.
pub const fn cfg_done_value(vp: u8) -> u32 {
    let bit = 1u32 << vp;
    GLB_CFG_DONE_EN | bit | (bit << 16)
}

/// System auto clock-gating control. Mainline clears the master enable (bit 31)
/// at `vop2_enable` — "workaround to avoid display image shift when a window
/// enabled". Cold reset leaves the register all-ones.
pub const SYS_AUTO_GATING_CTRL: usize = 0x008;
pub const AUTO_GATING_EN: u32 = 1 << 31;

/// VOP2-internal window-memory power domains (`RK3588_SYS_PD_CTRL`): a SET bit
/// = domain powered DOWN. Cold reset = 0xff (all down); a powered-down window
/// fetches zeros — a full-screen black window with every config register
/// looking correct. Mainline `rk3588_vop2_power_domain_enable_all` clears
/// Cluster0-3 (bits 0-3) + Esmart (bit 7) at enable, before any window config.
pub const SYS_PD_CTRL: usize = 0x034;
pub const PD_ALL_WINDOWS: u32 = 0x8f;

/// Window base offsets (RK3588).
pub mod win_base {
    pub const CLUSTER0: usize = 0x1000;
    pub const CLUSTER1: usize = 0x1200;
    pub const CLUSTER2: usize = 0x1400;
    pub const CLUSTER3: usize = 0x1600;
    pub const ESMART0: usize = 0x1800;
    pub const ESMART1: usize = 0x1a00;
    pub const ESMART2: usize = 0x1c00;
    pub const ESMART3: usize = 0x1e00;
}

/// Cluster-window internal register offsets (added to the window base).
pub mod cluster {
    pub const CTRL0: usize = 0x00;
    pub const CTRL1: usize = 0x04;
    pub const CTRL2: usize = 0x08;
    pub const YRGB_MST: usize = 0x10;
    pub const VIR: usize = 0x18;
    pub const ACT_INFO: usize = 0x20;
    pub const DSP_INFO: usize = 0x24;
    pub const DSP_ST: usize = 0x28;
    pub const CLUSTER_CTRL: usize = 0x100;
}

/// Esmart/Smart-window internal register offsets (added to the window base).
pub mod esmart {
    pub const CTRL0: usize = 0x00;
    /// SMART_CTRL1: carries the RK3588 per-window AXI read-IDs.
    pub const CTRL1: usize = 0x04;
    /// SMART_AXI_CTRL (RK3588): AXI bus-id select (bit 1).
    pub const AXI_CTRL: usize = 0x08;
    pub const REGION0_CTRL: usize = 0x10;
    pub const REGION0_YRGB_MST: usize = 0x14;
    pub const REGION0_VIR: usize = 0x1C;
    pub const REGION0_ACT_INFO: usize = 0x20;
    pub const REGION0_DSP_INFO: usize = 0x24;
    pub const REGION0_DSP_ST: usize = 0x28;

    // REGION0_CTRL fields (mainline rk3568_vop_smart_regs):
    /// Window enable (bit 0).
    pub const REGION0_CTRL_WIN_EN: u32 = 1 << 0;
    /// Data-format field bits [5:1]. XRGB8888/ARGB8888 == 0.
    pub const REGION0_CTRL_FORMAT_SHIFT: u32 = 1;
    pub const FORMAT_ARGB8888: u32 = 0;

    // CTRL1 AXI read-id fields (RK3588): yrgb [8:4], uv [16:12].
    pub const CTRL1_YRGB_R_ID_SHIFT: u32 = 4;
    pub const CTRL1_UV_R_ID_SHIFT: u32 = 12;
    /// Esmart0 default read-ids from mainline rk3588_vop_win_data.
    pub const ESMART0_YRGB_R_ID: u32 = 0x0a;
    pub const ESMART0_UV_R_ID: u32 = 0x0b;
}

/// System interrupt enable; frame-start interrupt bit (Stage 2 use).
pub const SYS0_INT_EN: usize = 0x80;
pub const FS_NEW_INTR: u32 = 1 << 4;

/// Video Port (VP) control blocks (Stage 2 modeset). Base per VP, stride 0x100.
pub mod vp {
    pub const VP0_BASE: usize = 0x0C00;
    pub const STRIDE: usize = 0x100;

    /// Byte offset of video port `vp`'s control block (0..=3).
    pub const fn base(vp: u8) -> usize {
        VP0_BASE + (vp as usize) * STRIDE
    }

    // Offsets within a VP block (add to `base(vp)`):
    pub const DSP_CTRL: usize = 0x00;
    pub const MIPI_CTRL: usize = 0x04;
    /// VP interface clock control (`RK3588_VP_CLK_CTRL`): DCLK_CORE_DIV[1:0] and
    /// DCLK_OUT_DIV[3:2], each holding log2(divisor). For HDMI with DCLK_VOP muxed
    /// straight to the HDPTX pixel clock, the VP core must run at dclk/4.
    pub const CLK_CTRL: usize = 0x0C;
    /// `CLK_CTRL` value for DCLK_CORE_DIV=/4 (log2=2), DCLK_OUT_DIV=/1.
    pub const CLK_CTRL_DCLK_CORE_DIV4: u32 = 2;
    /// VP hardware color-bar test-pattern control (`RK3568_VP_COLOR_BAR_CTRL`).
    /// Bit 0 = enable: when set, the VP emits an internal color-bar pattern that
    /// OVERRIDES the composited window output (the framebuffer is ignored). U-Boot
    /// enables this for its own bring-up and never clears it, so a from-scratch
    /// modeset must write 0 here or the panel shows color bars regardless of fb.
    pub const COLOR_BAR_CTRL: usize = 0x08;
    /// Enable bit of `COLOR_BAR_CTRL`.
    pub const COLOR_BAR_CTRL_EN: u32 = 1 << 0;
    pub const DSP_BG: usize = 0x2C;
    /// Pre-scan horizontal timing (`RK3568_VP_PRE_SCAN_HTIMING`):
    /// `((bg_dly + hactive/2 - 1) << 16) | hsync_len` (mainline
    /// `rk3568_vop2_setup_bg_dly`), written together with the matching
    /// `BG_MIX_CTRL` bg_dly. Left at the cold-reset 0 the post pipeline gets no
    /// per-line pre-fetch lead: the scanout FIFO underruns on EVERY line
    /// (`POST_BUF_EMPTY` latches each frame) and the VP outputs solid black
    /// with an otherwise perfect config — the sink locks, the picture is black.
    pub const PRE_SCAN_HTIMING: usize = 0x30;
    pub const POST_DSP_HACT_INFO: usize = 0x34;
    pub const POST_DSP_VACT_INFO: usize = 0x38;
    pub const DSP_HTOTAL_HS_END: usize = 0x48;
    pub const DSP_HACT_ST_END: usize = 0x4C;
    pub const DSP_VTOTAL_VS_END: usize = 0x50;
    pub const DSP_VACT_ST_END: usize = 0x54;

    /// Standby bit in `DSP_CTRL` (`RK3568_VP_DSP_CTRL__STANDBY`, BIT 31): set =
    /// VP blanked. Clearing it (once timing is programmed) starts scanout.
    pub const DSP_CTRL_STANDBY: u32 = 1 << 31;
    /// `OUT_MODE` field [3:0] of `DSP_CTRL`. RGB/AAAA (used for HDMI on a VP with
    /// the 10-bit output feature, which RK3588 VP0 has) == 0xF.
    pub const DSP_CTRL_OUT_MODE_AAAA: u32 = 0xF;
}

/// Global overlay/mixer block (absolute VOP2-base offsets). Binds windows to
/// video ports and configures the per-VP layer mux.
pub mod ovl {
    pub const CTRL: usize = 0x600;
    pub const LAYER_SEL: usize = 0x604;
    pub const PORT_SEL: usize = 0x608;
    /// Per-VP background-mix control: `0x6E0 + vp*4`. BG_DLY field [31:24].
    pub const fn vp_bg_mix_ctrl(vp: u8) -> usize {
        0x6E0 + (vp as usize) * 4
    }
    pub const BG_MIX_BG_DLY_SHIFT: u32 = 24;
    /// RK3588 VP0/VP1 background delay (mainline `pre_scan_max_dly[3]` of
    /// `rk3588_vop_video_ports`); must match the delay baked into the VP's
    /// `PRE_SCAN_HTIMING` value.
    pub const VP_BG_DLY: u32 = 54;
    /// Esmart-window delay-number register (holds ESMART0..3 delays).
    pub const SMART_DLY_NUM: usize = 0x6F8;

    /// One `LAYER_SEL` nibble is 4 bits at `layer*4`; it holds the window's
    /// `layer_sel_id` occupying mixer `layer`.
    pub const fn layer_sel_shift(layer: u8) -> u32 {
        (layer as u32) * 4
    }
    /// Esmart0's `layer_sel_id` on RK3588 (mainline `rk3588_vop_win_data`:
    /// Cluster0..3 = 0..3, Esmart0..3 = 4..7). 2 is the RK3568 id — using it
    /// here routes Cluster2 into the mixer instead and the VP scans background.
    pub const ESMART0_LAYER_SEL_ID: u32 = 4;

    /// `PORT_SEL` window→VP field for Esmart0: bits [25:24].
    pub const PORT_SEL_ESMART0_SHIFT: u32 = 24;
    pub const PORT_SEL_ESMART0_MASK: u32 = 0b11 << 24;

    /// `SMART_DLY_NUM` Esmart0 field: bits [7:0]. Esmart0 default delay == 23.
    pub const SMART_DLY_ESMART0_MASK: u32 = 0xFF;
    pub const ESMART0_DLY: u32 = 23;
}

/// System display-output-interface routing/enable (absolute offsets).
pub mod dsp_if {
    pub const EN: usize = 0x028;
    pub const CTRL: usize = 0x02C;
    pub const POL: usize = 0x030;

    /// RK3588: HDMI0 output enable (`RK3588_SYS_DSP_INFACE_EN_HDMI0`, BIT 3).
    pub const EN_HDMI0: u32 = 1 << 3;
    /// RK3588: EDP0/HDMI0 shared VP-source mux, bits [17:16] (0=VP0..3=VP3).
    pub const EN_EDP_HDMI0_MUX_SHIFT: u32 = 16;
    pub const EN_EDP_HDMI0_MUX_MASK: u32 = 0b11 << 16;
    /// `CFG_DONE_IMD` (BIT 28) of `POL`: make the DSP_IF bank latch immediately.
    pub const POL_CFG_DONE_IMD: u32 = 1 << 28;

    /// `CTRL` EDP0/HDMI0 interface dividers: DCLK_DIV[17:16] + PCLK_DIV[18], each
    /// log2(divisor). Without these the HDMI0 TX gets an ill-timed pixel stream
    /// (VP0 scans, but the transmitter outputs black).
    pub const CTRL_HDMI0_DIV_MASK: u32 = (0b11 << 16) | (1 << 18);
    /// HDMI0 interface dclk=/4 (DCLK_DIV[17:16]=2), pixclk=/2 (PCLK_DIV[18]=1) —
    /// the exact value mainline `rk3588_set_intf_mux` writes for 1080p60 8bpc
    /// (`rk3588_calc_cru_cfg`: if_dclk_div=ilog2(4)=2, if_pixclk_div=ilog2(2)=1).
    /// == 0x00060000.
    pub const CTRL_HDMI0_DCLK4_PCLK2: u32 = (2 << 16) | (1 << 18);
}

/// Interrupt registers (Stage 2 vsync). Per-VP block at `0xA0 + vp*0x10`.
pub mod intr {
    /// Per-VP interrupt-enable register.
    pub const fn vp_int_en(vp: u8) -> usize {
        0xA0 + (vp as usize) * 0x10
    }
    /// Per-VP interrupt-clear register.
    pub const fn vp_int_clr(vp: u8) -> usize {
        0xA4 + (vp as usize) * 0x10
    }
    /// Per-VP interrupt-status register.
    pub const fn vp_int_status(vp: u8) -> usize {
        0xA8 + (vp as usize) * 0x10
    }
    /// Per-VP raw (unmasked) interrupt-status register.
    pub const fn vp_int_raw_status(vp: u8) -> usize {
        0xAC + (vp as usize) * 0x10
    }

    /// Frame-start interrupt bit (shared position across the INT registers).
    pub const FS_NEW_INTR: u32 = 1 << 4;
    /// Line-flag 1 interrupt bit.
    pub const LINE_FLAG1_INTR: u32 = 1 << 6;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cfg_done_includes_global_enable() {
        // GLB_CFG_DONE_EN (BIT 15) must always be present, plus VP0's commit bit
        // (BIT 0) and its write-enable mask (BIT 16).
        assert_eq!(cfg_done_value(0), (1 << 15) | (1 << 0) | (1 << 16));
        assert_eq!(cfg_done_value(1), (1 << 15) | (1 << 1) | (1 << 17));
    }

    #[test]
    fn vp_offsets() {
        assert_eq!(vp::base(0), 0x0C00);
        assert_eq!(vp::base(1), 0x0D00);
        assert_eq!(vp::base(3), 0x0F00);
        assert_eq!(vp::base(0) + vp::DSP_HTOTAL_HS_END, 0x0C48);
        assert_eq!(vp::base(0) + vp::DSP_VACT_ST_END, 0x0C54);
    }

    #[test]
    fn intr_offsets() {
        assert_eq!(intr::vp_int_en(0), 0xA0);
        assert_eq!(intr::vp_int_clr(0), 0xA4);
        assert_eq!(intr::vp_int_status(0), 0xA8);
        assert_eq!(intr::vp_int_raw_status(0), 0xAC);
        assert_eq!(intr::vp_int_en(1), 0xB0);
        assert_eq!(intr::FS_NEW_INTR, 0x10);
    }
}
