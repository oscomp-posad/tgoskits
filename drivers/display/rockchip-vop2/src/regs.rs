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
    pub const DSP_BG: usize = 0x2C;
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
    /// Per-VP background-mix control: `0x6E0 + vp*4`.
    pub const fn vp_bg_mix_ctrl(vp: u8) -> usize {
        0x6E0 + (vp as usize) * 4
    }
    /// Esmart-window delay-number register (holds ESMART0..3 delays).
    pub const SMART_DLY_NUM: usize = 0x6F8;

    /// One `LAYER_SEL` nibble is 4 bits at `layer*4`; it holds the window's
    /// `layer_sel_id` occupying mixer `layer`.
    pub const fn layer_sel_shift(layer: u8) -> u32 {
        (layer as u32) * 4
    }
    /// Esmart0's `layer_sel_id` on RK3588 (mainline win data) == 2.
    pub const ESMART0_LAYER_SEL_ID: u32 = 2;

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
