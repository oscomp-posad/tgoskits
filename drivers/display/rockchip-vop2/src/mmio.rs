//! MMIO accessor abstraction so the core is testable off-hardware.

/// A 32-bit little-endian MMIO register window. `off` is a byte offset.
pub trait Regs {
    /// Read a 32-bit register at byte offset `off`.
    fn read32(&self, off: usize) -> u32;
    /// Write a 32-bit register at byte offset `off`.
    fn write32(&mut self, off: usize, val: u32);
}

/// A real hardware accessor over a mapped register base. `unsafe` to construct:
/// the caller guarantees `base` points at `len` bytes of mapped VOP2 MMIO.
pub struct MmioRegs {
    base: *mut u8,
    len: usize,
}

impl MmioRegs {
    /// # Safety
    /// `base` must be a valid mapping of at least `len` bytes of device memory.
    pub unsafe fn new(base: *mut u8, len: usize) -> Self {
        Self { base, len }
    }
}

impl Regs for MmioRegs {
    fn read32(&self, off: usize) -> u32 {
        assert!(off + 4 <= self.len, "vop2 read OOB: {off:#x}");
        unsafe { core::ptr::read_volatile(self.base.add(off) as *const u32) }
    }
    fn write32(&mut self, off: usize, val: u32) {
        assert!(off + 4 <= self.len, "vop2 write OOB: {off:#x}");
        unsafe { core::ptr::write_volatile(self.base.add(off) as *mut u32, val) }
    }
}

#[cfg(test)]
pub(crate) mod tests {
    extern crate std;
    use std::vec::Vec;

    use super::*;

    /// Host fake: a flat register file addressable by byte offset.
    pub struct FakeRegs {
        pub words: Vec<u32>,
    }
    impl FakeRegs {
        pub fn new(bytes: usize) -> Self {
            Self {
                words: std::vec![0u32; bytes / 4],
            }
        }
    }
    impl Regs for FakeRegs {
        fn read32(&self, off: usize) -> u32 {
            self.words[off / 4]
        }
        fn write32(&mut self, off: usize, val: u32) {
            self.words[off / 4] = val;
        }
    }

    #[test]
    fn fake_regs_roundtrip() {
        let mut r = FakeRegs::new(0x2000);
        r.write32(0x1814, 0xdead_beef);
        assert_eq!(r.read32(0x1814), 0xdead_beef);
    }
}
