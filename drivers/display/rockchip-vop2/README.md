# rockchip-vop2

OS-independent driver core for the RK3588 VOP2 display controller.

Layer 1 (Driver Core) of the cross-kernel driver model: register model + the
"adopt an already-scanning window and repoint it to our framebuffer" logic. No
MMIO mapping, no DMA, no OS calls — callers supply a `Regs` accessor and a
physical framebuffer address. Host-unit-tested.

Register offsets are transcribed from mainline
`drivers/gpu/drm/rockchip/rockchip_drm_vop2.h` and validated against a live
board register dump (Stage 1 board bring-up).
