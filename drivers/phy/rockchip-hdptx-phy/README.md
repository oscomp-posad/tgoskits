# rockchip-hdptx-phy

OS-independent RK3588 Samsung HDPTX combo-PHY bring-up, scoped to a **fixed
1080p60 HDMI TMDS output** (148.5 MHz char rate, 8bpc, 1/10 bit-rate path). A
from-scratch port of the TMDS path of mainline
`drivers/phy/rockchip/phy-rockchip-samsung-hdptx.c`.

The same ROPLL that drives the TMDS serializer feeds the VOP pixel clock
(`clk_hdmiphy_pixel0`), so this PHY is what makes a coherent 148.5 MHz dclk
available to VOP2.

## Board-validation status

The ~193 fixed init-sequence writes (`seqs.rs`) and the ROPLL dividers are
undocumented analog/bias/serializer trims transcribed **verbatim** from mainline.
Host tests cover the register-offset arithmetic, the divider encoding, the reset
ordering and the full write sequence — but whether the PLL/lanes actually LOCK at
148.5 MHz is physical and can only be confirmed on silicon. The two
`GRF_HDPTX_STATUS` poll gates (`PHY_CLK_RDY`, then `PHY_RDY & PLL_LOCK_DONE`) are
the on-board pass/fail oracle. **Nothing here has run on hardware.**
