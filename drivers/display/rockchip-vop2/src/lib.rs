//! OS-independent RK3588 VOP2 display-controller driver core.
//!
//! This crate holds only the register model and the "adopt-and-repoint" logic:
//! it does not map MMIO, allocate DMA, or talk to any OS. Callers provide a
//! [`mmio::Regs`] accessor over the mapped VOP2 register window and a physical
//! framebuffer address; the core computes and applies the register writes.
#![no_std]

pub mod mmio;
pub mod mode;
pub mod regs;
pub mod window;

pub use mode::{DisplayMode, PixelFormat};
pub use window::{Window, WindowKind};

/// Errors from VOP2 core operations.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Vop2Error {
    /// No window appears to be actively scanning (all candidate YRGB_MST are 0).
    NoActiveWindow,
    /// The framebuffer physical address is not 4-byte aligned (VOP2 requires it).
    MisalignedFramebuffer,
    /// The requested mode is not supported by this driver core.
    UnsupportedMode,
}
