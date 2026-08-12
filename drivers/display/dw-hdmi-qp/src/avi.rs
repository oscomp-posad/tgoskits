//! CEA-861 AVI infoframe construction + packing into the DW-HDMI-QP packet
//! registers. Byte layout from `hdmi_avi_infoframe_pack_only` (mainline
//! `drivers/video/hdmi.c`); register packing from `dw_hdmi_qp_write_infoframe`.

/// Pixel-encoding (AVI byte PB1 `Y` field).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Encoding {
    Rgb,
}

/// RGB quantization range (AVI byte PB3 `Q` field).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum QuantRange {
    Default,
    Limited,
    Full,
}

impl QuantRange {
    const fn pb3(self) -> u8 {
        match self {
            // Q occupies bits [3:2] of PB3.
            QuantRange::Default => 0x00,
            QuantRange::Limited => 0x04,
            QuantRange::Full => 0x08,
        }
    }
}

/// A minimal AVI infoframe (version 2, length 13) for a fixed video mode.
#[derive(Debug, Clone, Copy)]
pub struct AviInfoframe {
    pub vic: u8,
    pub encoding: Encoding,
    pub quant: QuantRange,
}

impl AviInfoframe {
    /// AVI for 1920x1080p60 RGB (VIC 16), 16:9, default range.
    pub const fn fhd60_rgb() -> Self {
        Self {
            vic: 16,
            encoding: Encoding::Rgb,
            quant: QuantRange::Default,
        }
    }

    /// The 14 payload bytes PB0..PB13 (PB0 = checksum). Header is HB0=0x82,
    /// HB1=0x02, HB2=0x0D.
    pub const fn payload(&self) -> [u8; 14] {
        // PB1: Y (encoding) in [6:5] = 0 for RGB; A0 (active-format present) = 1.
        let pb1: u8 = 0x10;
        // PB2: C (colorimetry) [7:6]=0; M (picture aspect) [5:4]=2 (16:9);
        // R (active aspect) [3:0]=8 (same as picture).
        let pb2: u8 = 0x28;
        let pb3: u8 = self.quant.pb3();
        let pb4: u8 = self.vic & 0x7f;
        let pb5: u8 = 0;

        // Checksum: PB0 = -(sum of header + PB1..PB13) mod 256.
        let sum: u32 = 0x82
            + 0x02
            + 0x0d
            + pb1 as u32
            + pb2 as u32
            + pb3 as u32
            + pb4 as u32
            + pb5 as u32;
        let pb0 = (0x100u32 - (sum & 0xff)) as u8;

        [
            pb0, pb1, pb2, pb3, pb4, pb5, 0, 0, 0, 0, 0, 0, 0, 0,
        ]
    }

    /// The 5 little-endian 32-bit words written to `PKT_AVI_CONTENTS0..+0x10`.
    /// Word 0 carries HB1/HB2 (the type byte HB0=0x82 is inserted by hardware);
    /// words 1..4 carry PB0..PB13.
    pub fn pack_words(&self) -> [u32; 5] {
        let pb = self.payload();
        fn w(a: u8, b: u8, c: u8, d: u8) -> u32 {
            (a as u32) | ((b as u32) << 8) | ((c as u32) << 16) | ((d as u32) << 24)
        }
        [
            (0x02u32 << 8) | (0x0du32 << 16), // HB1<<8 | HB2<<16
            w(pb[0], pb[1], pb[2], pb[3]),
            w(pb[4], pb[5], pb[6], pb[7]),
            w(pb[8], pb[9], pb[10], pb[11]),
            w(pb[12], pb[13], 0, 0),
        ]
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fhd60_rgb_payload_matches_mainline() {
        let f = AviInfoframe::fhd60_rgb();
        // 82 02 0D | 27 10 28 00 10 00 00 00 00 00 00 00 00 00
        assert_eq!(
            f.payload(),
            [0x27, 0x10, 0x28, 0x00, 0x10, 0x00, 0, 0, 0, 0, 0, 0, 0, 0]
        );
    }

    #[test]
    fn checksum_makes_frame_sum_zero() {
        let f = AviInfoframe::fhd60_rgb();
        let pb = f.payload();
        let total: u32 =
            0x82 + 0x02 + 0x0d + pb.iter().map(|&b| b as u32).sum::<u32>();
        assert_eq!(total & 0xff, 0);
    }

    #[test]
    fn packed_words_match_expected() {
        let f = AviInfoframe::fhd60_rgb();
        assert_eq!(
            f.pack_words(),
            [0x000D_0200, 0x0028_1027, 0x0000_0010, 0x0000_0000, 0x0000_0000]
        );
    }

    #[test]
    fn full_range_changes_pb3_and_checksum() {
        let f = AviInfoframe {
            quant: QuantRange::Full,
            ..AviInfoframe::fhd60_rgb()
        };
        let pb = f.payload();
        assert_eq!(pb[3], 0x08); // PB3 Q=full
        // checksum still balances the frame
        let total: u32 = 0x82 + 0x02 + 0x0d + pb.iter().map(|&b| b as u32).sum::<u32>();
        assert_eq!(total & 0xff, 0);
    }
}
