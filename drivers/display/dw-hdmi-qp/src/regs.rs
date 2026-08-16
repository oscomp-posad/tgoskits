//! DW-HDMI-QP register offsets (bytes, from TX base `0xfde80000`) and bit
//! fields used for a 1080p60 RGB TMDS enable. Names from mainline
//! `drivers/gpu/drm/bridge/synopsys/dw-hdmi-qp.h`.

/// HDCP2 logic config: `HDCP2_BYPASS` (bit 0) bypasses HDCP.
pub const HDCP2LOGIC_CONFIG0: usize = 0x8e0;
pub const HDCP2_BYPASS: u32 = 1 << 0;

/// Link config: `OPMODE_DVI` (bit 4) selects DVI (set) vs HDMI/TMDS (clear).
pub const LINK_CONFIG0: usize = 0x968;
pub const OPMODE_DVI: u32 = 1 << 4;

/// AVI infoframe packet contents (5 consecutive 32-bit words from here).
pub const PKT_AVI_CONTENTS0: usize = 0xbe0;

/// Packet-scheduler config1: `AVI_FIELDRATE` (bit 12) — cleared for a fixed
/// per-frame AVI.
pub const PKTSCHED_PKT_CONFIG1: usize = 0xa9c;
pub const PKTSCHED_AVI_FIELDRATE: u32 = 1 << 12;

/// Packet-scheduler enable: schedules the AVI + (auto) GCP transmit.
pub const PKTSCHED_PKT_EN: usize = 0xaa8;
pub const PKTSCHED_AVI_TX_EN: u32 = 1 << 13;
pub const PKTSCHED_GCP_TX_EN: u32 = 1 << 3;

// --- I2C master (DDC) block, for EDID reads ---

/// Timer base: the TX "ref" clock rate in Hz; times the I2CM SCL generator.
pub const TIMER_BASE_CONFIG0: usize = 0x80;
/// Fast-mode SCL low/high period config (mainline init value `0x085c085c`).
pub const I2CM_FM_SCL_CONFIG0: usize = 0xe4;
/// Writing 1 soft-resets the I2C master (also the error-recovery path).
pub const I2CM_CONTROL0: usize = 0xec;
/// Main I2CM control: slave address, byte address, op select, fast-mode en.
pub const I2CM_INTERFACE_CONTROL0: usize = 0xf4;
pub const I2CM_ADDR_MASK: u32 = 0xff << 12;
pub const I2CM_ADDR_SHIFT: u32 = 12;
pub const I2CM_SLVADDR_MASK: u32 = 0x7f << 5;
pub const I2CM_SLVADDR_SHIFT: u32 = 5;
/// Op-select field [4:1]; writing `FM_READ` starts one addressed byte read,
/// clearing the field returns the master to idle.
pub const I2CM_WR_MASK: u32 = 0x1e;
pub const I2CM_FM_READ: u32 = 1 << 2;
pub const I2CM_FM_EN: u32 = 1 << 0;
/// Read data (byte 0 = the transferred byte for single-byte reads).
pub const I2CM_INTERFACE_RDDATA_0_3: usize = 0x10c;
/// Main-unit interrupt group 1: raw latched status + write-1-to-clear. The
/// I2CM completion bits latch here regardless of the (unused) irq mask.
pub const MAINUNIT_1_INT_STATUS: usize = 0x3020;
pub const MAINUNIT_1_INT_CLEAR: usize = 0x3028;
pub const I2CM_NACK_RCVD: u32 = 1 << 2;
pub const I2CM_OP_DONE: u32 = 1 << 0;
