//! RK3588 VOP2 register offsets (bytes, relative to the `regs` MMIO base).
//! Transcribed from mainline `drivers/gpu/drm/rockchip/rockchip_drm_vop2.h`.

/// Writing (a mask into) this register latches all shadow-register writes.
pub const REG_CFG_DONE: usize = 0x000;

/// On RK3588 config-done is per-VP maskable. Bit `vp` enables that VP's latch;
/// the high half is a write-enable mask mirroring the low half.
pub const fn cfg_done_value(vp: u8) -> u32 {
    let bit = 1u32 << vp;
    bit | (bit << 16)
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
