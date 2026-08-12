//! ROPLL configuration for the fixed 1080p60 TMDS rate + the CMN divider writes
//! it produces. Transcribed from mainline `rk_hdptx_tmds_ropll_cfg[]` (the
//! 148.5 MHz row) and `rk_hdptx_tmds_ropll_cmn_config`.

use crate::mmio::Regs;

/// PHY register byte offset for CMN index `n` (`CMN_REG(n) = n*4`).
pub const fn cmn(n: u16) -> usize {
    (n as usize) * 4
}

// CMN field masks (from mainline).
const ROPLL_SDM_EN_MASK: u32 = 1 << 6;
const ROPLL_SDM_NUM_SIGN_RBR_MASK: u32 = 1 << 3;
const ROPLL_SDC_N_RBR_MASK: u32 = 0b111; // GENMASK(2,0)
const PLL_PCG_POSTDIV_SEL_MASK: u32 = 0xF0; // GENMASK(7,4)
const PLL_PCG_CLK_SEL_MASK: u32 = 0b1110; // GENMASK(3,1)

/// ROPLL divider/SDM config for one TMDS rate.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RopllConfig {
    pub rate: u64,
    pub mdiv: u8,
    pub mdiv_afc: u8,
    pub pdiv: u8,
    pub refdiv: u8,
    pub sdiv: u8,
    pub sdm_en: u8,
    pub sdm_deno: u8,
    pub sdm_num_sign: u8,
    pub sdm_num: u8,
    pub sdc_n: u8,
    pub sdc_num: u8,
    pub sdc_deno: u8,
}

impl RopllConfig {
    /// The 148.5 MHz (1080p60) row from `rk_hdptx_tmds_ropll_cfg[]`.
    pub const fn fhd60() -> Self {
        Self {
            rate: 148_500_000,
            mdiv: 123,
            mdiv_afc: 123,
            pdiv: 1,
            refdiv: 1,
            sdiv: 3,
            sdm_en: 1,
            sdm_deno: 4,
            sdm_num_sign: 0,
            sdm_num: 3,
            sdc_n: 5,
            sdc_num: 5,
            sdc_deno: 16,
        }
    }
}

/// Apply the ROPLL divider writes for `cfg` (mirrors the tail of
/// `rk_hdptx_tmds_ropll_cmn_config`; assumes 8bpc). Run after the fixed CMN init
/// sequences, before `post_enable_pll`.
pub fn apply_cmn_dividers<R: Regs>(regs: &mut R, cfg: &RopllConfig) {
    regs.write32(cmn(0x0051), cfg.mdiv as u32);
    regs.write32(cmn(0x0055), cfg.mdiv_afc as u32);
    regs.write32(cmn(0x0059), ((cfg.pdiv as u32) << 4) | cfg.refdiv as u32);
    regs.write32(cmn(0x005a), (cfg.sdiv as u32) << 4);

    // SDM enable (bit 6); mainline only clears the low nibble when sdm_en == 0.
    regs.modify(
        cmn(0x005e),
        ROPLL_SDM_EN_MASK,
        if cfg.sdm_en != 0 {
            ROPLL_SDM_EN_MASK
        } else {
            0
        },
    );
    if cfg.sdm_en == 0 {
        regs.modify(cmn(0x005e), 0xf, 0);
    }

    regs.modify(
        cmn(0x0064),
        ROPLL_SDM_NUM_SIGN_RBR_MASK,
        (cfg.sdm_num_sign as u32) << 3,
    );

    regs.write32(cmn(0x0060), cfg.sdm_deno as u32);
    regs.write32(cmn(0x0065), cfg.sdm_num as u32);

    regs.modify(cmn(0x0069), ROPLL_SDC_N_RBR_MASK, cfg.sdc_n as u32);

    regs.write32(cmn(0x006c), cfg.sdc_num as u32);
    regs.write32(cmn(0x0070), cfg.sdc_deno as u32);

    // 0086: postdiv_sel[7:4] = sdiv; clk_sel[3:1] = (bpc-8)>>1 = 0 for 8bpc.
    regs.modify(
        cmn(0x0086),
        PLL_PCG_POSTDIV_SEL_MASK,
        (cfg.sdiv as u32) << 4,
    );
    regs.modify(cmn(0x0086), PLL_PCG_CLK_SEL_MASK, 0);
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mmio::tests::FakeRegs;

    #[test]
    fn fhd60_config_matches_mainline_row() {
        let c = RopllConfig::fhd60();
        assert_eq!(
            (c.mdiv, c.mdiv_afc, c.pdiv, c.refdiv, c.sdiv),
            (123, 123, 1, 1, 3)
        );
        assert_eq!(
            (c.sdm_en, c.sdm_deno, c.sdm_num_sign, c.sdm_num),
            (1, 4, 0, 3)
        );
        assert_eq!((c.sdc_n, c.sdc_num, c.sdc_deno), (5, 5, 16));
    }

    #[test]
    fn cmn_offsets() {
        assert_eq!(cmn(0x0051), 0x144);
        assert_eq!(cmn(0x0086), 0x218);
    }

    #[test]
    fn divider_writes_produce_expected_values() {
        let mut r = FakeRegs::new(0x400);
        // pretend the fixed seq left 005e=0x4f and 0086=0x01 (their init values).
        r.write32(cmn(0x005e), 0x4f);
        r.write32(cmn(0x0086), 0x01);
        apply_cmn_dividers(&mut r, &RopllConfig::fhd60());

        assert_eq!(r.read32(cmn(0x0051)), 0x7b); // mdiv 123
        assert_eq!(r.read32(cmn(0x0055)), 0x7b);
        assert_eq!(r.read32(cmn(0x0059)), 0x11); // (1<<4)|1
        assert_eq!(r.read32(cmn(0x005a)), 0x30); // 3<<4
        assert_eq!(r.read32(cmn(0x005e)) & ROPLL_SDM_EN_MASK, ROPLL_SDM_EN_MASK);
        assert_eq!(r.read32(cmn(0x0060)), 4);
        assert_eq!(r.read32(cmn(0x0065)), 3);
        assert_eq!(r.read32(cmn(0x0069)) & ROPLL_SDC_N_RBR_MASK, 5);
        assert_eq!(r.read32(cmn(0x006c)), 5);
        assert_eq!(r.read32(cmn(0x0070)), 0x10); // 16
        // 0086: base 0x01 (clk_en) | postdiv 3<<4 => 0x31, clk_sel 0.
        assert_eq!(r.read32(cmn(0x0086)), 0x31);
    }
}
