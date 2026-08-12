//! OS-independent RK3588 Samsung HDPTX combo-PHY bring-up, scoped to a **fixed
//! 1080p60 HDMI TMDS output** (148.5 MHz char rate = 1.485 Gbps/lane, 8bpc, 1/10
//! bit-rate path, no FRL/DP/scrambling). From-scratch port of the TMDS path of
//! mainline `drivers/phy/rockchip/phy-rockchip-samsung-hdptx.c`.
//!
//! The same ROPLL that drives the TMDS serializer also feeds the VOP pixel clock
//! (`clk_hdmiphy_pixel0`), so bringing this PHY up is what makes a coherent
//! 148.5 MHz dclk available to VOP2 (the CRU only muxes it in — see
//! `rockchip-soc`'s `vop_hdmi0_clocks_setup`).
//!
//! ## Board-validation status
//! The ~193 fixed init-sequence writes (`seqs`) and the ROPLL divider values are
//! **undocumented analog/bias/serializer trims transcribed verbatim from
//! mainline**. Host tests here cover the register-offset arithmetic, the divider
//! encoding, and the write ordering — but whether the PLL and lanes actually
//! LOCK at 148.5 MHz is physical and can only be confirmed on real silicon (the
//! two `GRF_HDPTX_STATUS` poll gates, `PHY_CLK_RDY` then `PHY_RDY & PLL_LOCK_DONE`,
//! are the on-board pass/fail oracle). Nothing here has run on hardware yet.
#![no_std]

pub mod grf;
pub mod mmio;
pub mod phy;
pub mod ropll;
pub mod seqs;

pub use mmio::Regs;
pub use phy::{PhyEnv, PhyError, PhyResets, ResetLine, power_on_1080p60};
pub use ropll::RopllConfig;
