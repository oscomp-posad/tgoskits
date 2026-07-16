//! DW-HDMI-QP transmitter enable sequence for a fixed 1080p60 RGB TMDS output.
//! Transcribed from mainline `dw_hdmi_qp_bridge_atomic_enable` +
//! `dw_hdmi_qp_write_infoframe`. Assumes the PHY (TMDS bit clock), VOP2 (pixel
//! stream) and RK GRF (RGB/8bpc select) are brought up by the OS glue around
//! this call; this module only configures the QP core.

use crate::{
    avi::AviInfoframe,
    mmio::Regs,
    regs::{
        HDCP2_BYPASS, HDCP2LOGIC_CONFIG0, LINK_CONFIG0, OPMODE_DVI, PKT_AVI_CONTENTS0,
        PKTSCHED_AVI_FIELDRATE, PKTSCHED_AVI_TX_EN, PKTSCHED_GCP_TX_EN, PKTSCHED_PKT_CONFIG1,
        PKTSCHED_PKT_EN,
    },
};

/// Output op-mode. HDMI sends packets (incl. the AVI infoframe); DVI is pure
/// TMDS video with no packets — the most bulletproof first-light for an unknown
/// sink.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OpMode {
    Hdmi,
    Dvi,
}

/// Pack + schedule the AVI infoframe for transmission (HDMI mode only).
pub fn write_avi_infoframe<R: Regs>(regs: &mut R, avi: &AviInfoframe) {
    // Stop scheduling the packet while its contents are updated.
    regs.modify(PKTSCHED_PKT_EN, PKTSCHED_AVI_TX_EN | PKTSCHED_GCP_TX_EN, 0);

    for (i, word) in avi.pack_words().iter().enumerate() {
        regs.write32(PKT_AVI_CONTENTS0 + i * 4, *word);
    }

    // Per-frame AVI (not field-rate), then (re)enable AVI + auto-GCP transmit.
    regs.modify(PKTSCHED_PKT_CONFIG1, PKTSCHED_AVI_FIELDRATE, 0);
    regs.modify(
        PKTSCHED_PKT_EN,
        PKTSCHED_AVI_TX_EN | PKTSCHED_GCP_TX_EN,
        PKTSCHED_AVI_TX_EN | PKTSCHED_GCP_TX_EN,
    );
}

/// Enable the QP core for `mode`. Bypasses HDCP, selects the op-mode, and (for
/// HDMI) schedules the AVI infoframe. Output goes live once the PHY is locked,
/// the op-mode is TMDS, and VOP2 is pushing pixels — there is no single
/// TMDS-output-enable bit in the QP core.
pub fn enable<R: Regs>(regs: &mut R, mode: OpMode, avi: &AviInfoframe) {
    // HDCP is never needed for a framebuffer; bypass it.
    regs.modify(HDCP2LOGIC_CONFIG0, HDCP2_BYPASS, HDCP2_BYPASS);

    match mode {
        OpMode::Hdmi => {
            // Clear OPMODE_DVI => HDMI/TMDS.
            regs.modify(LINK_CONFIG0, OPMODE_DVI, 0);
            write_avi_infoframe(regs, avi);
        }
        OpMode::Dvi => {
            // Set OPMODE_DVI => DVI; no infoframe.
            regs.modify(LINK_CONFIG0, OPMODE_DVI, OPMODE_DVI);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mmio::tests::FakeRegs;

    #[test]
    fn hdmi_enable_bypasses_hdcp_sets_tmds_and_schedules_avi() {
        let mut r = FakeRegs::new(0x1000);
        // pretend DVI was set + AVI scheduling was off
        r.write32(LINK_CONFIG0, OPMODE_DVI);
        enable(&mut r, OpMode::Hdmi, &AviInfoframe::fhd60_rgb());

        assert_eq!(r.read32(HDCP2LOGIC_CONFIG0) & HDCP2_BYPASS, HDCP2_BYPASS);
        assert_eq!(r.read32(LINK_CONFIG0) & OPMODE_DVI, 0); // TMDS/HDMI
        // AVI packet words written
        assert_eq!(r.read32(PKT_AVI_CONTENTS0), 0x000D_0200);
        assert_eq!(r.read32(PKT_AVI_CONTENTS0 + 4), 0x0028_1027);
        assert_eq!(r.read32(PKT_AVI_CONTENTS0 + 8), 0x0000_0010);
        // AVI + GCP scheduled, field-rate cleared
        assert_eq!(
            r.read32(PKTSCHED_PKT_EN) & (PKTSCHED_AVI_TX_EN | PKTSCHED_GCP_TX_EN),
            PKTSCHED_AVI_TX_EN | PKTSCHED_GCP_TX_EN
        );
        assert_eq!(r.read32(PKTSCHED_PKT_CONFIG1) & PKTSCHED_AVI_FIELDRATE, 0);
    }

    #[test]
    fn dvi_enable_sets_dvi_and_writes_no_packet() {
        let mut r = FakeRegs::new(0x1000);
        enable(&mut r, OpMode::Dvi, &AviInfoframe::fhd60_rgb());
        assert_eq!(r.read32(LINK_CONFIG0) & OPMODE_DVI, OPMODE_DVI);
        assert_eq!(r.read32(HDCP2LOGIC_CONFIG0) & HDCP2_BYPASS, HDCP2_BYPASS);
        // no AVI packet written
        assert_eq!(r.read32(PKT_AVI_CONTENTS0), 0);
        assert_eq!(r.read32(PKTSCHED_PKT_EN), 0);
    }
}
