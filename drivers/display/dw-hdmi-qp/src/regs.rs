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
