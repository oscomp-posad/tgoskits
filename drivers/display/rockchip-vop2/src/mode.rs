//! Display mode + pixel-format math. Stage 1 supports one fixed mode.

/// Pixel layout. Stage 1 targets XRGB8888 only (4 bytes/pixel, linear).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PixelFormat {
    Xrgb8888,
}

impl PixelFormat {
    /// Bytes per pixel.
    pub const fn bytes_per_pixel(self) -> u32 {
        match self {
            PixelFormat::Xrgb8888 => 4,
        }
    }

    /// VOP2 window format-field encoding for the Esmart REGION0_CTRL / Cluster
    /// CTRL0 `data_format` field. XRGB8888 == 0 on RK3568/RK3588 win format.
    pub const fn vop2_format_code(self) -> u32 {
        match self {
            PixelFormat::Xrgb8888 => 0,
        }
    }
}

/// A resolved display mode (active geometry only — Stage 1 does not touch VP
/// blanking timings; U-Boot already programmed them).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DisplayMode {
    pub width: u32,
    pub height: u32,
    pub format: PixelFormat,
    /// Which VP drives the active connector (0..=3). Confirmed by dump in glue.
    pub vp: u8,
}

impl DisplayMode {
    /// Canonical Stage-1 fallback: 1080p, XRGB8888, VP0.
    pub const fn fhd_vp0() -> Self {
        Self {
            width: 1920,
            height: 1080,
            format: PixelFormat::Xrgb8888,
            vp: 0,
        }
    }

    /// Bytes per scanline (no extra padding; VOP2 VIR is in 32-bit words).
    pub const fn stride_bytes(&self) -> u32 {
        self.width * self.format.bytes_per_pixel()
    }

    /// VIR register value: stride expressed in 32-bit words.
    pub const fn vir_words(&self) -> u32 {
        self.stride_bytes() / 4
    }

    /// Total framebuffer byte size.
    pub const fn fb_size(&self) -> usize {
        (self.stride_bytes() * self.height) as usize
    }

    /// ACT_INFO register value: ((height-1) << 16) | (width-1).
    pub const fn act_info(&self) -> u32 {
        ((self.height - 1) << 16) | (self.width - 1)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fhd_geometry() {
        let m = DisplayMode::fhd_vp0();
        assert_eq!(m.stride_bytes(), 1920 * 4);
        assert_eq!(m.vir_words(), 1920);
        assert_eq!(m.fb_size(), 1920 * 1080 * 4);
        assert_eq!(m.act_info(), (1079 << 16) | 1919);
    }

    #[test]
    fn format_codes() {
        assert_eq!(PixelFormat::Xrgb8888.bytes_per_pixel(), 4);
        assert_eq!(PixelFormat::Xrgb8888.vop2_format_code(), 0);
    }
}
