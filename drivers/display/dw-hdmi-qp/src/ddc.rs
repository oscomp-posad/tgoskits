//! Poll-mode DDC (I2C master) EDID read over the QP core's built-in I2CM.
//! Transcribed from mainline `dw_hdmi_qp_i2c_read` + the I2CM subset of
//! `dw_hdmi_qp_init_hw`, converted from irq-completion to status polling (the
//! completion bits latch in `MAINUNIT_1_INT_STATUS` regardless of the mask).
//!
//! Reads are one addressed byte at a time (the hardware re-addresses per byte),
//! so a 128-byte block costs 128 short transfers — fine for a one-shot probe.

use crate::{
    mmio::Regs,
    regs::{
        I2CM_ADDR_MASK, I2CM_ADDR_SHIFT, I2CM_CONTROL0, I2CM_FM_EN, I2CM_FM_READ,
        I2CM_FM_SCL_CONFIG0, I2CM_INTERFACE_CONTROL0, I2CM_INTERFACE_RDDATA_0_3, I2CM_NACK_RCVD,
        I2CM_OP_DONE, I2CM_SLVADDR_MASK, I2CM_SLVADDR_SHIFT, I2CM_WR_MASK, MAINUNIT_1_INT_CLEAR,
        MAINUNIT_1_INT_STATUS, TIMER_BASE_CONFIG0,
    },
};

/// The DDC EDID slave address.
pub const DDC_EDID_ADDR: u8 = 0x50;

/// Per-byte completion timeout. Generous: SCL may run well below nominal if
/// `ref_clk_hz` overstates the real ref clock (slow SCL is always I2C-legal).
const BYTE_TIMEOUT_US: u32 = 50_000;
const POLL_STEP_US: u32 = 50;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DdcError {
    /// No completion within the timeout — SCL/SDA not toggling (pinmux?) or
    /// nothing driving the bus.
    Timeout { byte: usize },
    /// The sink NACKed — nothing at 0x50 (no sink plugged, or no DDC power).
    Nack { byte: usize },
}

/// One-time I2CM init (the I2CM subset of mainline `dw_hdmi_qp_init_hw`):
/// program the SCL timer base with the TX "ref" clock rate, soft-reset the
/// master, apply the SCL period config, and select standard mode (100 kHz).
pub fn init<R: Regs>(regs: &mut R, ref_clk_hz: u32) {
    regs.write32(TIMER_BASE_CONFIG0, ref_clk_hz);
    regs.write32(I2CM_CONTROL0, 0x01);
    regs.write32(I2CM_FM_SCL_CONFIG0, 0x085c_085c);
    regs.modify(I2CM_INTERFACE_CONTROL0, I2CM_FM_EN, 0);
    regs.write32(MAINUNIT_1_INT_CLEAR, I2CM_OP_DONE | I2CM_NACK_RCVD);
}

/// Read EDID block 0 (128 bytes from DDC address 0x50, byte offsets 0..=127).
/// On error the master is soft-reset and the op field idled, so a retry or a
/// later transfer starts clean.
pub fn read_edid_block0<R: Regs>(
    regs: &mut R,
    mut delay_us: impl FnMut(u32),
) -> Result<[u8; 128], DdcError> {
    let mut out = [0u8; 128];
    regs.modify(
        I2CM_INTERFACE_CONTROL0,
        I2CM_SLVADDR_MASK,
        (DDC_EDID_ADDR as u32) << I2CM_SLVADDR_SHIFT,
    );

    for (i, byte) in out.iter_mut().enumerate() {
        regs.write32(MAINUNIT_1_INT_CLEAR, I2CM_OP_DONE | I2CM_NACK_RCVD);
        regs.modify(
            I2CM_INTERFACE_CONTROL0,
            I2CM_ADDR_MASK,
            (i as u32) << I2CM_ADDR_SHIFT,
        );
        regs.modify(I2CM_INTERFACE_CONTROL0, I2CM_WR_MASK, I2CM_FM_READ);

        let mut waited = 0u32;
        let stat = loop {
            let s = regs.read32(MAINUNIT_1_INT_STATUS);
            if s & (I2CM_OP_DONE | I2CM_NACK_RCVD) != 0 {
                break s;
            }
            if waited >= BYTE_TIMEOUT_US {
                regs.write32(I2CM_CONTROL0, 0x01);
                regs.modify(I2CM_INTERFACE_CONTROL0, I2CM_WR_MASK, 0);
                return Err(DdcError::Timeout { byte: i });
            }
            delay_us(POLL_STEP_US);
            waited += POLL_STEP_US;
        };

        if stat & I2CM_NACK_RCVD != 0 {
            regs.write32(I2CM_CONTROL0, 0x01);
            regs.modify(I2CM_INTERFACE_CONTROL0, I2CM_WR_MASK, 0);
            return Err(DdcError::Nack { byte: i });
        }

        *byte = (regs.read32(I2CM_INTERFACE_RDDATA_0_3) & 0xff) as u8;
        regs.modify(I2CM_INTERFACE_CONTROL0, I2CM_WR_MASK, 0);
    }

    Ok(out)
}

/// True iff `edid` starts with the fixed 8-byte EDID header.
pub fn header_ok(edid: &[u8; 128]) -> bool {
    edid[..8] == [0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00]
}

/// True iff the block checksum (all 128 bytes summing to 0 mod 256) holds.
pub fn checksum_ok(edid: &[u8; 128]) -> bool {
    edid.iter().fold(0u8, |a, &b| a.wrapping_add(b)) == 0
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mmio::tests::FakeRegs;

    #[test]
    fn init_programs_timer_scl_and_standard_mode() {
        let mut r = FakeRegs::new(0x4000);
        r.write32(I2CM_INTERFACE_CONTROL0, I2CM_FM_EN); // pretend fast mode on
        init(&mut r, 428_571_429);
        assert_eq!(r.read32(TIMER_BASE_CONFIG0), 428_571_429);
        assert_eq!(r.read32(I2CM_FM_SCL_CONFIG0), 0x085c_085c);
        assert_eq!(r.read32(I2CM_INTERFACE_CONTROL0) & I2CM_FM_EN, 0);
    }

    #[test]
    fn read_reads_all_bytes_and_addresses_last_byte() {
        let mut r = FakeRegs::new(0x4000);
        // Completion permanently asserted; data byte fixed. (FakeRegs's CLEAR
        // write is a distinct word, so STATUS stays set across all 128 reads.)
        r.write32(MAINUNIT_1_INT_STATUS, I2CM_OP_DONE);
        r.write32(I2CM_INTERFACE_RDDATA_0_3, 0xA5);
        let edid = read_edid_block0(&mut r, |_| {}).unwrap();
        assert!(edid.iter().all(|&b| b == 0xA5));
        let ctl = r.read32(I2CM_INTERFACE_CONTROL0);
        // slave 0x50, last byte address 127, op field idled after completion
        assert_eq!((ctl & I2CM_SLVADDR_MASK) >> I2CM_SLVADDR_SHIFT, 0x50);
        assert_eq!((ctl & I2CM_ADDR_MASK) >> I2CM_ADDR_SHIFT, 127);
        assert_eq!(ctl & I2CM_WR_MASK, 0);
    }

    #[test]
    fn nack_aborts_with_reset() {
        let mut r = FakeRegs::new(0x4000);
        r.write32(MAINUNIT_1_INT_STATUS, I2CM_NACK_RCVD);
        assert_eq!(
            read_edid_block0(&mut r, |_| {}),
            Err(DdcError::Nack { byte: 0 })
        );
        assert_eq!(r.read32(I2CM_CONTROL0), 0x01); // soft reset issued
    }

    #[test]
    fn timeout_aborts_after_budget() {
        let mut r = FakeRegs::new(0x4000);
        let mut slept = 0u32;
        assert_eq!(
            read_edid_block0(&mut r, |us| slept += us),
            Err(DdcError::Timeout { byte: 0 })
        );
        assert!(slept >= BYTE_TIMEOUT_US);
        assert_eq!(r.read32(I2CM_CONTROL0), 0x01);
    }

    #[test]
    fn header_and_checksum_helpers() {
        let mut e = [0u8; 128];
        e[..8].copy_from_slice(&[0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00]);
        assert!(header_ok(&e));
        // make the sum 0 mod 256 via the last byte
        let sum: u8 = e.iter().fold(0u8, |a, &b| a.wrapping_add(b));
        e[127] = 0u8.wrapping_sub(sum);
        assert!(checksum_ok(&e));
        e[0] = 1;
        assert!(!header_ok(&e));
        assert!(!checksum_ok(&e));
    }
}
