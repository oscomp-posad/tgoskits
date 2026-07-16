//! RK3588 VOP2 OS glue: FDT probe, MMIO map, DMA framebuffer, and rdif-display
//! registration. Driver-core logic lives in the `rockchip-vop2` crate; this
//! file is the OS boundary only.
//!
//! Stage 1 (adopt-and-repoint) assumes U-Boot already powered and lit the VOP
//! (it drew the boot logo), so this driver does not enable the VOP power domain
//! itself — matching the current `rknpu` driver, which likewise relies on the
//! bootloader for its power domain. On `upstream/dev` the RK3588 power provider
//! is exposed only through `rdif_power::Interface` (`power_on(PowerDomainId)`),
//! not as a bare `RockchipPM` device; a real cold-init power-on (VOP domain id
//! 24) belongs to Stage 2 and must go through that interface.

use dma_api::{CoherentArray, DeviceDma};
use log::{info, warn};
use rdif_display::{DisplayError, DisplayInfo, FrameBuffer, PixelFormat};
use rdrive::{DriverGeneric, probe::OnProbeError, register::ProbeFdt};
use rockchip_vop2::{
    mmio::MmioRegs,
    mode::DisplayMode,
    window::{detect_live_window, repoint},
};

use crate::{display::PlatformDeviceDisplay, mmio::iomap};

crate::model_register!(
    name: "Rockchip VOP2",
    level: ProbeLevel::PostKernel,
    priority: ProbePriority::DEFAULT,
    probe_kinds: &[
        ProbeKind::Fdt {
            compatibles: &["rockchip,rk3588-vop"],
            on_probe: probe
        }
    ],
);

/// The registered display device: owns the mapped DMA framebuffer + resolved
/// geometry. The framebuffer is a **coherent (uncached)** DMA allocation:
/// VOP2 scans it continuously from DRAM and userspace mmaps `/dev/fb0` uncached,
/// so a cached/streaming buffer would show stale RAM (CPU writes stuck in cache)
/// and create a mismatched-cacheability alias on ARMv8. `CoherentArray<u8>` and
/// `DisplayInfo` are both `Send`, so this derives `Send` automatically.
struct Vop2Display {
    fb: CoherentArray<u8>,
    info: DisplayInfo,
}

impl DriverGeneric for Vop2Display {
    fn name(&self) -> &str {
        "rockchip-vop2"
    }
}

impl rdif_display::Interface for Vop2Display {
    fn info(&self) -> DisplayInfo {
        self.info
    }
    fn framebuffer(&mut self) -> Result<FrameBuffer<'_>, DisplayError> {
        // SAFETY: the DMA buffer is `fb_size` bytes, CPU-mapped, single owner.
        Ok(
            unsafe {
                FrameBuffer::from_raw_parts_mut(self.fb.as_ptr().as_ptr(), self.info.fb_size)
            },
        )
    }
    fn need_flush(&self) -> bool {
        // VOP2 scans continuously from YRGB_MST; no per-frame copy needed.
        false
    }
}

fn probe(probe: ProbeFdt<'_>) -> Result<(), OnProbeError> {
    let (info_fdt, plat_dev) = probe.into_parts();
    let regs = info_fdt.node.regs();

    // reg[0] = control registers (0xfdd90000, 0x4200). Map page-aligned.
    let reg0 = regs
        .first()
        .ok_or_else(|| OnProbeError::other("vop2: no reg"))?;
    let start_raw = reg0.address as usize;
    let size_raw = reg0.size.unwrap_or(0x4200) as usize;
    let page = 0x1000usize;
    let start = start_raw & !(page - 1);
    let offset = start_raw - start;
    let end = (start_raw + size_raw + page - 1) & !(page - 1);
    let mapped = unsafe { iomap(start, end - start)?.add(offset) };
    // Bound the accessor by the bytes actually mapped from `mapped` (page-aligned
    // region minus the intra-page offset), which is always >= size_raw. Using
    // size_raw directly would let a malformed/undersized FDT `reg` size make the
    // dump/repoint reads trip MmioRegs' bounds assert and panic the boot probe.
    let mapped_len = (end - start) - offset;
    let mut mmio = unsafe { MmioRegs::new(mapped.as_ptr(), mapped_len) };

    // NOTE: Stage 1 relies on U-Boot having powered the VOP domain (see module
    // docs). We do not enable it here; if a future dump shows the block is
    // unpowered (garbage/zero registers), that is the trigger to add a real
    // `rdif_power` power-on in the Stage-2 cold-init path.

    // --- DUMP: golden reference. Read-only; safe on first bring-up. ---
    dump_state(&mmio);

    let live = detect_live_window(&mmio);
    match live {
        Some(w) => info!(
            "vop2: live window {:?} base={:#x} mst={:#x}",
            w.kind,
            w.base,
            w.current_mst(&mmio)
        ),
        None => warn!(
            "vop2: NO live window (all candidate YRGB_MST == 0). U-Boot did not light the \
             display; registering fb anyway, full modeset is Stage 2+."
        ),
    }

    // Allocate our framebuffer: COHERENT (uncached), contiguous, low-4GiB,
    // zeroed. Coherent — not streaming/contiguous — because VOP2 scans it in
    // place with no per-frame flush and userspace mmaps it uncached; a cached
    // buffer would leave CPU writes in cache and the panel scanning stale RAM.
    // `device_with_mask(u32::MAX)` keeps it in the low 4 GiB YRGB_MST can reach.
    let mode = DisplayMode::fhd_vp0();
    let dma: DeviceDma = axklib::dma::device_with_mask(u32::MAX as u64);
    let fb = dma
        .coherent_array_zero_with_align::<u8>(mode.fb_size(), 0x1000)
        .map_err(|_| OnProbeError::other("vop2: framebuffer DMA alloc failed"))?;
    let fb_phys = fb.dma_addr().as_u64();
    info!("vop2: fb {} bytes @ phys {:#x}", mode.fb_size(), fb_phys);

    // Repoint the live window (if any) at our framebuffer. If U-Boot lit no
    // window we still register the fb; modeset is Stage 2.
    if let Some(win) = live {
        match repoint(&mut mmio, win, fb_phys, &mode) {
            Ok(()) => info!("vop2: repointed {:?} -> our fb {:#x}", win.kind, fb_phys),
            Err(e) => warn!("vop2: repoint failed: {e:?} (registering fb anyway)"),
        }
    }

    let info = DisplayInfo {
        width: mode.width,
        height: mode.height,
        stride: mode.stride_bytes() as usize,
        format: PixelFormat::Xrgb8888,
        fb_size: mode.fb_size(),
    };
    let dev = Vop2Display { fb, info };
    // `PlatformDeviceDisplay::register_display` returns `Option<usize>` (the IRQ
    // if the binding carried one) — infallible at this layer, not a Result.
    let irq = plat_dev.register_display(dev);
    info!("vop2: registered display irq={irq:?}");
    Ok(())
}

/// Dump the registers we care about at `Info` level for the golden reference.
fn dump_state(mmio: &MmioRegs) {
    use rockchip_vop2::regs::{REG_CFG_DONE, cluster, esmart, win_base};
    let rd = |off: usize| rockchip_vop2::mmio::Regs::read32(mmio, off);
    info!("vop2 DUMP cfg_done={:#010x}", rd(REG_CFG_DONE));
    info!(
        "vop2 DUMP cluster0 ctrl0={:#010x} mst={:#010x} vir={:#010x} act={:#010x} dsp={:#010x}",
        rd(win_base::CLUSTER0 + cluster::CTRL0),
        rd(win_base::CLUSTER0 + cluster::YRGB_MST),
        rd(win_base::CLUSTER0 + cluster::VIR),
        rd(win_base::CLUSTER0 + cluster::ACT_INFO),
        rd(win_base::CLUSTER0 + cluster::DSP_INFO),
    );
    info!(
        "vop2 DUMP esmart0 ctrl0={:#010x} rgn0_ctrl={:#010x} mst={:#010x} vir={:#010x} \
         act={:#010x}",
        rd(win_base::ESMART0 + esmart::CTRL0),
        rd(win_base::ESMART0 + esmart::REGION0_CTRL),
        rd(win_base::ESMART0 + esmart::REGION0_YRGB_MST),
        rd(win_base::ESMART0 + esmart::REGION0_VIR),
        rd(win_base::ESMART0 + esmart::REGION0_ACT_INFO),
    );
}
