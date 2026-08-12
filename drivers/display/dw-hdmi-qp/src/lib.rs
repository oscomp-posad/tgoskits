//! OS-independent Synopsys DesignWare HDMI 2.1 "QP" transmitter core for RK3588.
//!
//! Scope: bring the TX up for a **fixed 1080p60 RGB 8bpc TMDS** output with no
//! EDID/DDC, no scrambling/SCDC (148.5 MHz < 340 MHz), no audio, HDCP bypassed.
//! On RK3588 the QP core is a near-passthrough: VOP2 supplies the pixel stream +
//! H/V timing, the HDPTX PHY generates the TMDS bit clock, and the RK GRF selects
//! RGB/8bpc — so a "TX enable" is only a handful of register writes plus the AVI
//! infoframe. Everything here is a `Regs` accessor over the mapped TX MMIO
//! (base `0xfde80000`); MMIO mapping, GRF, PHY and clocks live in the OS glue.
//!
//! Register offsets/values are transcribed from mainline
//! `drivers/gpu/drm/bridge/synopsys/dw-hdmi-qp.{c,h}`. Host-tested; the actual
//! TMDS output is board-validated (needs the PHY + VOP2 + GRF up).
#![no_std]

pub mod avi;
pub mod mmio;
pub mod regs;
pub mod tx;

pub use avi::AviInfoframe;
pub use mmio::Regs;
pub use tx::{OpMode, enable};
