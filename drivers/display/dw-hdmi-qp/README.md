# dw-hdmi-qp

OS-independent Synopsys DesignWare HDMI 2.1 "QP" transmitter core for RK3588,
scoped to a fixed **1080p60 RGB 8bpc TMDS** output (no EDID/DDC, no
scrambling/SCDC, no audio, HDCP bypassed).

On RK3588 the QP core is a near-passthrough: VOP2 supplies the pixel stream and
timing, the HDPTX PHY generates the TMDS bit clock, and the RK GRF selects
RGB/8bpc. So a "TX enable" is a handful of register writes + the AVI infoframe.

Register offsets/values transcribed from mainline
`drivers/gpu/drm/bridge/synopsys/dw-hdmi-qp.{c,h}` and host-tested (AVI byte
packing + checksum + write order). Actual TMDS output is board-validated (needs
the PHY + VOP2 + GRF up).
