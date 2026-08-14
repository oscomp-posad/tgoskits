//! Full RK3588 HDMI0 cold-init orchestration (Stage 4c): own the HDPTX PHY +
//! DW-HDMI-QP TX + CRU dclk + GRF muxing to light a 1080p60 RGB output *without*
//! reusing any U-Boot display state. Ties together the driver cores:
//! `rockchip-hdptx-phy` (PHY), `rockchip-soc` (CRU dclk), `rockchip-vop2`
//! (modeset) and `dw-hdmi-qp` (TX), plus raw GRF pokes.
//!
//! ## Board-validation status
//! Nothing here has run on hardware. The MMIO addresses, reset ids and GRF
//! writes are hardcoded for the OrangePi-5-Plus / RK3588 (from the board DTB +
//! mainline), and the *ordering* is a best-effort mirror of mainline — both are
//! expected to need tuning against the board (the PHY lock-poll bits and a
//! register dump are the oracle). This compiles + links so the board session is
//! "debug against lock bits", not "write from scratch". It is opt-in
//! (`rockchip-vop2-coldinit` feature) and never on a default boot path.
//!
//! Runs at device-probe time (PostKernel), not from an interrupt: the shared CRU
//! handle it drives (reset/clock pokes) is guarded by a non-IRQ-safe spinlock, so
//! this must stay on a normal-context path.

use core::time::Duration;

use log::{info, warn};
use rockchip_hdptx_phy::{
    PhyEnv, PhyResets, ResetLine, mmio::MmioRegs as PhyMmio, power_on_1080p60,
};
use rockchip_vop2::{mmio::MmioRegs as VopMmio, mode::DisplayMode, modeset::modeset_vp0};

use crate::mmio::iomap;

// --- Board-fixed MMIO regions (RK3588 / OrangePi-5-Plus, from the DTB) ---
const PHY_BASE: usize = 0xfed6_0000;
const PHY_SIZE: usize = 0x2000;
const HDPTX_GRF_BASE: usize = 0xfd5e_0000;
const TX_BASE: usize = 0xfde8_0000;
const TX_SIZE: usize = 0x1_0000;
const VOP_GRF_BASE: usize = 0xfd5a_4000;
const VO1_GRF_BASE: usize = 0xfd5a_8000;
const SYS_GRF_BASE: usize = 0xfd58_c000;
const GRF_SIZE: usize = 0x1000;

// --- Power-domain ids (rdif) ---
const PD_VOP: u32 = 24;
const PD_VO1: u32 = 26;

// --- HDPTX PHY CRU reset ids (DT `resets` of phy@fed60000: apb/init/cmn/lane) ---
// These are the vendor register-encoded reset ids (`id = con_word*16 + bit`,
// spanning CRU sub-blocks). The generic `ResetRockchip` linear decode
// (`addr = softrst_base + (id/16)*4`, `bit = id%16`) reproduces the sub-block
// offset by construction: `apb=0x485` -> SOFTRST_CON72 bit5, and
// `init/cmn/lane=0xc003b..d` -> the PHP-CRU window (`(0xc0000/16)*4 == 0x30000
// == RK3588_PHP_CRU_BASE`) at php_softrst_con(3) bits 11/12/13. The whole
// 0x5c000 CRU reg range is mapped, so all four land on real softrst registers.
const RST_HDPTX_APB: usize = 0x485;
const RST_HDPTX_INIT: usize = 0xc003b;
const RST_HDPTX_CMN: usize = 0xc003c;
const RST_HDPTX_LANE: usize = 0xc003d;

// --- HDPTX PHY source clocks (DT `clocks` of phy@fed60000: ref 24MHz + apb) ---
// The PHY sequence assumes these are running before its first APB access; ungate
// them explicitly since a cold boot may have left them gated (idempotent).
const CLK_HDPTX0_REF: usize = 0x2b5;
const PCLK_HDPTX0_APB: usize = 0x267;

// --- GRF register offsets + hiword-masked write values (mainline) ---
// VOP_GRF CON2: route VP output to the HDMI0 TX (BIT1).
const VOP_GRF_CON2: usize = 0x08;
const VOP_GRF_CON2_HDMI0: u32 = 0x0002_0002;
// VO1_GRF CON0: HDMI0 sync polarity (from Stage-2 research; board-tunable).
const VO1_GRF_CON0: usize = 0x00;
const VO1_GRF_CON0_HDMI0_POL: u32 = 0x0060_0020;
// VO1_GRF CON3: DDC SDA/SCL pin mux + HDMI mode + I2S sel; COLOR_DEPTH[7:4]=0 (8bpc).
const VO1_GRF_CON3: usize = 0x0c;
const VO1_GRF_CON3_IO: u32 = 0x2e00_2e00;
// VO1_GRF CON9: HDMI0 grant select (BIT10).
const VO1_GRF_CON9: usize = 0x24;
const VO1_GRF_CON9_GRANT: u32 = 0x0400_0400;
// SYS_GRF CON7: HPD IO enable (BIT12|BIT13).
const SYS_GRF_CON7: usize = 0x31c;
const SYS_GRF_CON7_HPD_IO: u32 = 0x3000_3000;

/// CRU-backed PHY reset lines (uses the process-wide CRU handle).
struct CruResets;
impl PhyResets for CruResets {
    fn assert(&mut self, line: ResetLine) {
        crate::soc::rockchip::cru::reset_assert(reset_id(line));
    }
    fn deassert(&mut self, line: ResetLine) {
        crate::soc::rockchip::cru::reset_deassert(reset_id(line));
    }
}

fn reset_id(line: ResetLine) -> usize {
    match line {
        ResetLine::Apb => RST_HDPTX_APB,
        ResetLine::Init => RST_HDPTX_INIT,
        ResetLine::Cmn => RST_HDPTX_CMN,
        ResetLine::Lane => RST_HDPTX_LANE,
    }
}

/// Kernel timing env: busy-wait microsecond delays for the PHY sequence + polls.
struct KEnv;
impl PhyEnv for KEnv {
    fn delay_us(&self, us: u32) {
        axklib::time::busy_wait(Duration::from_micros(us as u64));
    }
}

/// Map a GRF syscon page once and apply a batch of hiword-masked writes. Batched
/// (rather than one map per register) so a multi-register GRF is only mapped once.
fn grf_write_batch(base: usize, writes: &[(usize, u32)]) -> Result<(), &'static str> {
    let p = iomap(base, GRF_SIZE).map_err(|_| "grf iomap failed")?;
    for &(off, val) in writes {
        // SAFETY: `off` is within the mapped GRF_SIZE page; GRF regs are 32-bit.
        unsafe { core::ptr::write_volatile(p.as_ptr().add(off) as *mut u32, val) };
    }
    Ok(())
}

/// Cold-init HDMI0 for a 1080p60 RGB output displaying `fb_phys`, from scratch
/// (no U-Boot reuse). Programs, in order: power domains -> ungate PHY ref/apb
/// clocks -> HDPTX PHY (blocks on PLL/lane lock) -> CRU dclk mux + VOP gates ->
/// GRF muxes -> VOP2 modeset -> TX enable. Returns the first failing step; the
/// caller keeps the fb registered regardless so /dev/fb0 exists.
pub fn cold_init_hdmi0_1080p60(vop_mmio: &mut VopMmio, fb_phys: u64) -> Result<(), &'static str> {
    let mode = DisplayMode::fhd_vp0();

    // 1. Power the VOP + VO1 domains (cold boot: U-Boot did not).
    match crate::soc::rockchip::pm::power_on_domain(PD_VOP) {
        Ok(true) => info!("coldinit: VOP power domain on"),
        Ok(false) => warn!("coldinit: no power provider; assuming VOP powered"),
        Err(e) => warn!("coldinit: VOP power_on failed: {e:?}"),
    }
    let _ = crate::soc::rockchip::pm::power_on_domain(PD_VO1);

    // 2. Ungate the PHY's own ref (24 MHz) + apb source clocks before touching
    //    it — the PHY sequence's first APB access assumes they are running, and a
    //    cold boot may have left them gated. Best-effort (idempotent if already on).
    if !crate::soc::rockchip::cru::clk_enable(CLK_HDPTX0_REF) {
        warn!("coldinit: HDPTX0 ref clock ungate skipped (no CRU / unknown id)");
    }
    let _ = crate::soc::rockchip::cru::clk_enable(PCLK_HDPTX0_APB);

    // 3. HDPTX PHY: generate the 148.5 MHz TMDS + pixel clock. Blocks on the
    //    two GRF lock polls (the on-board pass/fail oracle).
    let phy_ptr = iomap(PHY_BASE, PHY_SIZE).map_err(|_| "phy iomap failed")?;
    let grf_ptr = iomap(HDPTX_GRF_BASE, GRF_SIZE).map_err(|_| "hdptx-grf iomap failed")?;
    // SAFETY: freshly-mapped device regions of the given sizes.
    let mut phy = unsafe { PhyMmio::new(phy_ptr.as_ptr(), PHY_SIZE) };
    let mut phy_grf = unsafe { PhyMmio::new(grf_ptr.as_ptr(), GRF_SIZE) };
    let mut resets = CruResets;
    power_on_1080p60(&mut phy, &mut phy_grf, &mut resets, &KEnv).map_err(|e| match e {
        rockchip_hdptx_phy::PhyError::PllClkTimeout => "phy PLL clk timeout",
        rockchip_hdptx_phy::PhyError::PllLockTimeout => "phy PLL/lane lock timeout",
    })?;
    info!("coldinit: HDPTX PHY locked @ 148.5MHz");

    // 4. CRU: mux DCLK_VOP0 to the now-running PHY pixel clock + ungate VOP.
    if crate::soc::rockchip::cru::with_cru(|c| c.vop_hdmi0_clocks_setup()).is_none() {
        warn!("coldinit: no CRU handle; VOP dclk not muxed");
    }

    // 5. GRF: route VP0->HDMI0 + polarity + DDC/HPD io + color depth (8bpc RGB).
    //    Each GRF syscon is mapped once; VO1_GRF carries three of the writes.
    grf_write_batch(VOP_GRF_BASE, &[(VOP_GRF_CON2, VOP_GRF_CON2_HDMI0)])?;
    grf_write_batch(
        VO1_GRF_BASE,
        &[
            (VO1_GRF_CON0, VO1_GRF_CON0_HDMI0_POL),
            (VO1_GRF_CON3, VO1_GRF_CON3_IO),
            (VO1_GRF_CON9, VO1_GRF_CON9_GRANT),
        ],
    )?;
    grf_write_batch(SYS_GRF_BASE, &[(SYS_GRF_CON7, SYS_GRF_CON7_HPD_IO)])?;

    // 6. VOP2 modeset (timing + Esmart0 window + overlay + DSP_IF_EN mux).
    modeset_vp0(vop_mmio, fb_phys, &mode).map_err(|_| "vop2 modeset failed")?;
    info!("coldinit: VOP2 VP0 modeset done");

    // 7. HDMI-QP TX enable (HDMI op-mode + AVI infoframe).
    let tx_ptr = iomap(TX_BASE, TX_SIZE).map_err(|_| "tx iomap failed")?;
    // SAFETY: freshly-mapped device region.
    let mut tx = unsafe { dw_hdmi_qp::mmio::MmioRegs::new(tx_ptr.as_ptr(), TX_SIZE) };
    dw_hdmi_qp::tx::enable(
        &mut tx,
        dw_hdmi_qp::tx::OpMode::Hdmi,
        &dw_hdmi_qp::avi::AviInfoframe::fhd60_rgb(),
    );
    info!("coldinit: HDMI-QP TX enabled -> 1080p60 RGB should be live");

    // 8. Monitor-free liveness readback: prove the raster is actually scanning
    //    (frame-start interrupts advancing), the window is fetching our fb, and
    //    the PHY is still locked — everything verifiable short of a physical sink.
    probe_pipeline_liveness(vop_mmio, fb_phys, &KEnv);

    Ok(())
}

/// Read back live hardware state to prove the display pipeline is running
/// **without a monitor**. Reports: PHY still locked (HDPTX-GRF STATUS), VP0 out
/// of standby, the Esmart0 window enabled and fetching our `fb_phys`, and — the
/// decisive dynamic signal — VOP2 frame-start interrupts advancing, which means
/// the VP is scanning a raster off the live PHY pixel clock (not merely
/// configured). A physical sink is the only thing this cannot confirm.
pub fn probe_pipeline_liveness<E: PhyEnv>(vop_mmio: &mut VopMmio, fb_phys: u64, env: &E) {
    use rockchip_vop2::{
        mmio::Regs,
        regs::{esmart, intr, vp, win_base},
    };

    // PHY lock bits (HDPTX-GRF STATUS @ +0x80): bit3 PLL_LOCK_DONE, bit2 CLK_RDY,
    // bit1 PHY_RDY.
    if let Ok(p) = iomap(HDPTX_GRF_BASE, GRF_SIZE) {
        // SAFETY: freshly-mapped device page; STATUS is a 32-bit reg at 0x80.
        let s = unsafe { core::ptr::read_volatile(p.as_ptr().add(0x80) as *const u32) };
        info!(
            "liveness: HDPTX-GRF STATUS={s:#010x} pll_lock={} clk_rdy={} phy_rdy={}",
            s & (1 << 3) != 0,
            s & (1 << 2) != 0,
            s & (1 << 1) != 0
        );
    }

    // VP0 active (not in standby) + window enabled + fetching our fb.
    let dsp_ctrl = vop_mmio.read32(vp::base(0) + vp::DSP_CTRL);
    let win_ctrl = vop_mmio.read32(win_base::ESMART0 + esmart::REGION0_CTRL);
    let mst = vop_mmio.read32(win_base::ESMART0 + esmart::REGION0_YRGB_MST);
    info!(
        "liveness: VP0 DSP_CTRL={dsp_ctrl:#010x} standby={} | Esmart0 win_en={} \
         YRGB_MST={mst:#010x} (fb={:#010x} match={})",
        dsp_ctrl & vp::DSP_CTRL_STANDBY != 0,
        win_ctrl & esmart::REGION0_CTRL_WIN_EN != 0,
        fb_phys as u32,
        mst == fb_phys as u32
    );

    // Dynamic proof: enable + clear the VP0 frame-start latch, then poll the raw
    // status for ~50 ms (>3 frames @60Hz). If FS pulses/latches, the VOP is
    // actively scanning — the pixel clock is live and the timing is running.
    let en = vop_mmio.read32(intr::vp_int_en(0));
    vop_mmio.write32(intr::vp_int_en(0), en | intr::FS_NEW_INTR);
    vop_mmio.write32(intr::vp_int_clr(0), intr::FS_NEW_INTR);
    let mut seen_pulse = false;
    for _ in 0..500 {
        if vop_mmio.read32(intr::vp_int_raw_status(0)) & intr::FS_NEW_INTR != 0 {
            seen_pulse = true;
        }
        env.delay_us(100);
    }
    let latched = vop_mmio.read32(intr::vp_int_status(0)) & intr::FS_NEW_INTR != 0;
    let live = seen_pulse || latched;
    info!(
        "liveness: frame-start pulse={seen_pulse} latched={latched} => raster {}",
        if live {
            "RUNNING — VOP scanning off a live pixel clock (valid HDMI timing generated)"
        } else {
            "NOT advancing — no dclk / VOP idle (check PHY->CRU dclk mux)"
        }
    );
}

/// Paint eight vertical SMPTE-style color bars into an XRGB8888 framebuffer.
///
/// Bring-up **visual oracle** only: a freshly-allocated scanout buffer is zeroed
/// (solid black on the panel), which is ambiguous — "black" could mean "no
/// signal", "wrong fb address", or "working but empty". Color bars are an
/// unmistakable, self-generated pattern, so a correct end-to-end chain
/// (PHY lock -> CRU mux -> GRF -> VOP2 scanout -> TX) shows *our* image with no
/// userspace involved. `stride` is the row stride in bytes.
pub fn fill_color_bars(fb: *mut u8, width: u32, height: u32, stride: usize) {
    // 0x00RRGGBB — white, yellow, cyan, green, magenta, red, blue, black.
    const BARS: [u32; 8] = [
        0x00FF_FFFF,
        0x00FF_FF00,
        0x0000_FFFF,
        0x0000_FF00,
        0x00FF_00FF,
        0x00FF_0000,
        0x0000_00FF,
        0x0000_0000,
    ];
    let (w, h) = (width as usize, height as usize);
    let bar_w = (w / 8).max(1);
    for y in 0..h {
        // SAFETY: caller guarantees `fb` is a `height*stride`-byte XRGB8888
        // buffer; each row write stays within `w*4 <= stride` bytes.
        let row = unsafe { fb.add(y * stride) } as *mut u32;
        for x in 0..w {
            let c = BARS[(x / bar_w).min(7)];
            unsafe { core::ptr::write_volatile(row.add(x), c) };
        }
    }
}
