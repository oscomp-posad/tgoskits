//! HDPTX GRF (syscon `rockchip,rk3588-hdptxphy-grf`, base `0xfd5e0000`) register
//! offsets, bits, and the pre-composed hiword-masked CON0 write values used by
//! the TMDS bring-up. Values from mainline `phy-rockchip-samsung-hdptx.c`.

/// GRF control register 0 (offset 0x00). Written hiword-masked (high 16 =
/// write-enable mask, low 16 = data).
pub const CON0: usize = 0x00;
/// GRF status register (offset 0x80, read-only).
pub const STATUS: usize = 0x80;

// CON0 field bits.
pub const LC_REF_CLK_SEL: u32 = 1 << 11;
pub const I_PLL_EN: u32 = 1 << 7;
pub const I_BIAS_EN: u32 = 1 << 6;
pub const I_BGR_EN: u32 = 1 << 5;
pub const MODE_SEL: u32 = 1 << 0;

// STATUS field bits.
pub const O_PLL_LOCK_DONE: u32 = 1 << 3;
pub const O_PHY_CLK_RDY: u32 = 1 << 2;
pub const O_PHY_RDY: u32 = 1 << 1;
pub const O_SB_RDY: u32 = 1 << 0;

/// Compose a hiword-masked write: `(mask << 16) | (val & mask)`.
pub const fn wmask(mask: u32, val: u32) -> u32 {
    (mask << 16) | (val & mask)
}

// Pre-composed CON0 writes for the TMDS power-up (verify against mainline):
/// pre_power_up: clear PLL/BIAS/BGR enables -> `(PLL_EN|BIAS_EN|BGR_EN) << 16`.
pub const CON0_PRE_POWER_UP: u32 = (I_PLL_EN | I_BIAS_EN | I_BGR_EN) << 16; // 0x00E00000
/// cmn_config: select the local 24M ref (clear LC_REF_CLK_SEL).
pub const CON0_LC_REF_CLK: u32 = LC_REF_CLK_SEL << 16; // 0x08000000
/// post_enable_pll: set BIAS + BGR.
pub const CON0_BIAS_BGR: u32 = wmask(I_BIAS_EN | I_BGR_EN, I_BIAS_EN | I_BGR_EN); // 0x00600060
/// post_enable_pll: set PLL_EN.
pub const CON0_PLL_EN: u32 = wmask(I_PLL_EN, I_PLL_EN); // 0x00800080
/// power_on: select HDMI mode (clear MODE_SEL).
pub const CON0_MODE_HDMI: u32 = MODE_SEL << 16; // 0x00010000

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn con0_write_values_match_mainline() {
        assert_eq!(CON0_PRE_POWER_UP, 0x00E0_0000);
        assert_eq!(CON0_LC_REF_CLK, 0x0800_0000);
        assert_eq!(CON0_BIAS_BGR, 0x0060_0060);
        assert_eq!(CON0_PLL_EN, 0x0080_0080);
        assert_eq!(CON0_MODE_HDMI, 0x0001_0000);
    }
}
