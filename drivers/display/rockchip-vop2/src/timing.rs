//! CEA-861 video timing → VOP2 Video-Port timing register values.
//!
//! Pure/host-tested. The register packing is transcribed from mainline
//! `vop2_crtc_atomic_enable` (`drivers/gpu/drm/rockchip/rockchip_drm_vop2.c`):
//! with DRM crtc semantics `hsync_start = hactive + hfront`,
//! `htotal = hactive + hfront + hsync + hback`, mainline computes
//! `hact_st = htotal - hsync_start` (= `hsync + hback`) and
//! `hact_end = hact_st + hactive`, then writes
//! `HTOTAL_HS_END = htotal<<16 | hsync`, `HACT_ST_END = hact_st<<16 | hact_end`,
//! `VTOTAL_VS_END = vtotal<<16 | vsync`, `VACT_ST_END = vact_st<<16 | vact_end`,
//! and `POST_DSP_{H,V}ACT_INFO` with the same active start/end (no underscan
//! margin). Progressive scan only (1080p60); interlaced fields are out of scope.

/// Full display timing in pixels/lines. Sync polarity is positive for 1080p60.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct VideoTiming {
    pub hactive: u32,
    pub hfront: u32,
    pub hsync: u32,
    pub hback: u32,
    pub vactive: u32,
    pub vfront: u32,
    pub vsync: u32,
    pub vback: u32,
}

impl VideoTiming {
    /// 1920x1080p60: htotal 2200, vtotal 1125, pixel clock 148.5 MHz.
    pub const fn fhd60() -> Self {
        Self {
            hactive: 1920,
            hfront: 88,
            hsync: 44,
            hback: 148,
            vactive: 1080,
            vfront: 4,
            vsync: 5,
            vback: 36,
        }
    }

    pub const fn htotal(&self) -> u32 {
        self.hactive + self.hfront + self.hsync + self.hback
    }
    pub const fn vtotal(&self) -> u32 {
        self.vactive + self.vfront + self.vsync + self.vback
    }

    /// Active-region horizontal start = `htotal - hsync_start` = `hsync + hback`.
    const fn hact_st(&self) -> u32 {
        self.hsync + self.hback
    }
    /// Active-region vertical start = `vsync + vback`.
    const fn vact_st(&self) -> u32 {
        self.vsync + self.vback
    }

    /// `DSP_HTOTAL_HS_END = (htotal << 16) | hsync_len`.
    pub const fn htotal_hs_end(&self) -> u32 {
        (self.htotal() << 16) | self.hsync
    }
    /// `DSP_HACT_ST_END = (hact_st << 16) | hact_end`.
    pub const fn hact_st_end(&self) -> u32 {
        let st = self.hact_st();
        (st << 16) | (st + self.hactive)
    }
    /// `DSP_VTOTAL_VS_END = (vtotal << 16) | vsync_len`.
    pub const fn vtotal_vs_end(&self) -> u32 {
        (self.vtotal() << 16) | self.vsync
    }
    /// `DSP_VACT_ST_END = (vact_st << 16) | vact_end`.
    pub const fn vact_st_end(&self) -> u32 {
        let st = self.vact_st();
        (st << 16) | (st + self.vactive)
    }
    /// `POST_DSP_HACT_INFO` — same active start/end as the display timing when
    /// there is no post-scaler underscan margin (our case).
    pub const fn post_hact_info(&self) -> u32 {
        self.hact_st_end()
    }
    /// `POST_DSP_VACT_INFO` — same active start/end (no underscan margin).
    pub const fn post_vact_info(&self) -> u32 {
        self.vact_st_end()
    }
    /// `PRE_SCAN_HTIMING = ((bg_dly + hactive/2 - 1) << 16) | hsync_len`
    /// (mainline `rk3568_vop2_setup_bg_dly`). `bg_dly` must equal the value
    /// programmed into the VP's `BG_MIX_CTRL` BG_DLY field.
    pub const fn pre_scan_htiming(&self, bg_dly: u32) -> u32 {
        ((bg_dly + self.hactive / 2 - 1) << 16) | self.hsync
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fhd60_totals() {
        let t = VideoTiming::fhd60();
        assert_eq!(t.htotal(), 2200);
        assert_eq!(t.vtotal(), 1125);
    }

    #[test]
    fn fhd60_register_values_match_mainline() {
        let t = VideoTiming::fhd60();
        // htotal 2200, hsync 44
        assert_eq!(t.htotal_hs_end(), (2200 << 16) | 44);
        // hact_st = 44+148 = 192; hact_end = 192+1920 = 2112
        assert_eq!(t.hact_st_end(), (192 << 16) | 2112);
        // vtotal 1125, vsync 5
        assert_eq!(t.vtotal_vs_end(), (1125 << 16) | 5);
        // vact_st = 5+36 = 41; vact_end = 41+1080 = 1121
        assert_eq!(t.vact_st_end(), (41 << 16) | 1121);
        // POST info mirrors the display active window (no underscan)
        assert_eq!(t.post_hact_info(), (192 << 16) | 2112);
        assert_eq!(t.post_vact_info(), (41 << 16) | 1121);
        // pre-scan with the RK3588 VP0 bg_dly of 54: (54+960-1)<<16 | 44
        // (board-proven 2026-08-17: this exact value ended the per-line
        // POST_BUF_EMPTY underrun and produced first light)
        assert_eq!(t.pre_scan_htiming(54), (1013 << 16) | 44);
    }
}
