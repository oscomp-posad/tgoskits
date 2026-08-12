//! MMIO accessor for the PHY register block (base `0xfed60000`) and the HDPTX
//! GRF (a separate syscon). Both use the same 32-bit `Regs` trait; GRF writes
//! carry the Rockchip hiword write-enable mask in the value itself.

/// A 32-bit little-endian MMIO register window. `off` is a byte offset from the
/// window base.
pub trait Regs {
    fn read32(&self, off: usize) -> u32;
    fn write32(&mut self, off: usize, val: u32);

    /// Read-modify-write: set the bits of `mask` to `val & mask`.
    fn modify(&mut self, off: usize, mask: u32, val: u32) {
        let cur = self.read32(off);
        self.write32(off, (cur & !mask) | (val & mask));
    }
}

/// Real hardware accessor over a mapped register base.
pub struct MmioRegs {
    base: *mut u8,
    len: usize,
}

impl MmioRegs {
    /// # Safety
    /// `base` must map at least `len` bytes of the device register memory.
    pub unsafe fn new(base: *mut u8, len: usize) -> Self {
        Self { base, len }
    }
}

impl Regs for MmioRegs {
    fn read32(&self, off: usize) -> u32 {
        assert!(off + 4 <= self.len, "hdptx read OOB: {off:#x}");
        unsafe { core::ptr::read_volatile(self.base.add(off) as *const u32) }
    }
    fn write32(&mut self, off: usize, val: u32) {
        assert!(off + 4 <= self.len, "hdptx write OOB: {off:#x}");
        unsafe { core::ptr::write_volatile(self.base.add(off) as *mut u32, val) }
    }
}

#[cfg(test)]
pub(crate) mod tests {
    extern crate std;
    use std::vec::Vec;

    use super::*;

    /// Byte-addressable fake register file that also records the write order.
    pub struct FakeRegs {
        pub words: Vec<u32>,
        pub writes: Vec<(usize, u32)>,
    }
    impl FakeRegs {
        pub fn new(bytes: usize) -> Self {
            Self {
                words: std::vec![0u32; bytes / 4],
                writes: Vec::new(),
            }
        }
    }
    impl Regs for FakeRegs {
        fn read32(&self, off: usize) -> u32 {
            self.words[off / 4]
        }
        fn write32(&mut self, off: usize, val: u32) {
            self.words[off / 4] = val;
            self.writes.push((off, val));
        }
    }
}
