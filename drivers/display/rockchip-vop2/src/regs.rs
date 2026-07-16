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
    pub const REGION0_CTRL: usize = 0x10;
    pub const REGION0_YRGB_MST: usize = 0x14;
    pub const REGION0_VIR: usize = 0x1C;
    pub const REGION0_ACT_INFO: usize = 0x20;
    pub const REGION0_DSP_INFO: usize = 0x24;
    pub const REGION0_DSP_ST: usize = 0x28;
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
    pub const POST_DSP_HACT_INFO: usize = 0x34;
    pub const POST_DSP_VACT_INFO: usize = 0x38;
    pub const DSP_HTOTAL_HS_END: usize = 0x48;
    pub const DSP_HACT_ST_END: usize = 0x4C;
    pub const DSP_VTOTAL_VS_END: usize = 0x50;
    pub const DSP_VACT_ST_END: usize = 0x54;

    /// Standby bit in `DSP_CTRL` (`RK3568_VP_DSP_CTRL__STANDBY`, BIT 31): set =
    /// VP blanked. Clearing it (once timing is programmed) starts scanout.
    pub const DSP_CTRL_STANDBY: u32 = 1 << 31;
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
