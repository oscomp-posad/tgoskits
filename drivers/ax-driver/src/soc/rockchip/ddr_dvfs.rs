// Copyright 2025 The Axvisor Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//! RK3588 DDR/DMC dynamic frequency ramp via the **Rockchip SIP DRAM interface**.
//!
//! StarryOS boots with the DDR controller at whatever rate the DDR-bin / BL31 left
//! it (a conservative mid/low rung). A single A76 core nearly saturates that low
//! bandwidth ceiling, so multi-threaded memory workloads cannot scale — measured
//! ~13 GB/s aggregate vs Linux's ~55 GB/s, even though single-core memcpy is at
//! parity. Linux closes the gap with a `dmc` devfreq driver that ramps the DDR to
//! its top rung (2112 MHz on this LPDDR4X board). This module does the same ramp
//! once, at boot.
//!
//! ## Why SIP, not SCMI
//!
//! The RK3588 DDR clock is NOT driven through SCMI, despite `SCMI_CLK_DDR = 4`
//! existing in the device tree: in TF-A that clock is a NULL-ops placeholder (no
//! rate table), and Rockchip's own kernel `dmc` driver sets
//! `is_set_rate_direct = true` to bypass `clk_set_rate` and call the SIP DRAM
//! service directly. The actual frequency switch — including LPDDR retraining on a
//! dedicated DDR MCU that hardware-stalls all AXI masters for ~1-5 ms — lives in
//! the closed rkbin BL31 behind SMC function `0x82000008`. Parameters that don't
//! fit in registers (the target Hz) are passed via a shared page obtained from a
//! second SMC (`0x82000009`).
//!
//! ## Safety
//!
//! Ramping UP requires the DDR/logic rail to already supply the target rung's
//! voltage — an undervolted switch corrupts memory. On boards where that rail is a
//! fixed always-on regulator at max, no action is needed; otherwise the rail must
//! be raised first (reusing the cpufreq PMIC lever). Until that voltage precondition
//! is verified for this board, this module runs in **probe-only** mode: it exercises
//! the read-only half of the interface (version, shared page, available-frequency
//! query) and logs it, but does NOT issue SET_RATE. That validates the closed BL31
//! actually answers the SIP DRAM calls on real hardware — the one fact desk research
//! could not confirm — at zero risk, and can share a board run with other tests.

use log::{info, warn};

use crate::mmio::iomap;

// ---- SIP function IDs (SMC #0, EL1 -> EL3) --------------------------------------
const SIP_DRAM_CONFIG: u32 = 0x8200_0008; // ROCKCHIP_SIP_DRAM_FREQ
const SIP_SHARE_MEM: u32 = 0x8200_0009;

// ---- DRAM_CONFIG subcommands (passed in x3) ------------------------------------
const CFG_DRAM_INIT: usize = 0x00;
const CFG_DRAM_SET_RATE: usize = 0x01;
const CFG_DRAM_GET_RATE: usize = 0x05;
const CFG_DRAM_GET_VERSION: usize = 0x08;
const CFG_DRAM_POST_SET_RATE: usize = 0x09;
const CFG_MCU_START: usize = 0x0c;
const CFG_DRAM_GET_FREQ_INFO: usize = 0x0e;

/// `share_mem` page type selector (x2) and `set_rate` page selector (x1).
const SHARE_PAGE_TYPE_DDR: usize = 2;

/// SET_RATE returns this in a1 when the switch is handed to the DDR MCU and is not
/// yet complete — an *expected* async status on RK3588, NOT a failure.
const SIP_RET_SET_RATE_TIMEOUT: i64 = -6;

// ---- `struct share_params` field offsets (u32 LE, first 4 KiB of the page) ------
const SP_HZ: usize = 0;
const SP_LCDC_TYPE: usize = 4;
const SP_FREQ_COUNT: usize = 44;
const SP_FREQ_INFO_MHZ: usize = 48; // [6] u32

/// `lcdc_type = 0` (SCREEN_NULL): headless, so BL31 will not wait on a VOP scanout
/// window before switching.
const SCREEN_NULL: u32 = 0;

/// Shared page is 8 KiB (params in the first 4 KiB).
const SHARE_PAGE_SIZE: usize = 0x2000;

/// Master switch. `false` = probe-only (read the interface, never SET_RATE): safe on
/// any board, used to first confirm the closed BL31 answers these calls on-board.
/// Flip to `true` only once the DDR-rail voltage precondition is verified for this
/// board (see the module-level Safety note) so the ramp cannot undervolt the DRAM.
const ENABLE_SET_RATE: bool = false;

/// Issue a Rockchip SIP `smc #0`. Args land in x1..x3; returns (x0, x1) — x0 is the
/// status (0 = ok, negative = error), x1 the secondary result (version / phys addr /
/// async status). Self-contained asm: ax-driver sits below ax-hal in the dependency
/// graph, so it cannot use the HAL's SMCCC helper.
#[cfg(target_arch = "aarch64")]
#[inline]
fn sip_smc(fid: u32, a1: usize, a2: usize, a3: usize) -> (i64, i64) {
    let mut x0 = fid as usize;
    let mut x1 = a1;
    // SAFETY: a plain SMCCC call into resident EL3 firmware; clobbers x0..x3 only.
    unsafe {
        core::arch::asm!(
            "smc #0",
            inout("x0") x0,
            inout("x1") x1,
            inout("x2") a2 => _,
            inout("x3") a3 => _,
            options(nomem, nostack),
        );
    }
    (x0 as i64, x1 as i64)
}

#[cfg(not(target_arch = "aarch64"))]
#[inline]
fn sip_smc(_fid: u32, _a1: usize, _a2: usize, _a3: usize) -> (i64, i64) {
    (-1, 0)
}

/// Busy-wait `ms` milliseconds off the ARM generic timer (CNTPCT_EL0 / CNTFRQ_EL0).
/// Used to bound the DDR-MCU retrain before POST_SET_RATE. Direct system-register
/// reads keep this free of any ax-hal dependency.
#[cfg(target_arch = "aarch64")]
fn busy_wait_ms(ms: u64) {
    #[inline]
    fn cntpct() -> u64 {
        let v: u64;
        // SAFETY: reads a read-only architectural counter.
        unsafe { core::arch::asm!("mrs {}, cntpct_el0", out(reg) v, options(nomem, nostack)) };
        v
    }
    #[inline]
    fn cntfrq() -> u64 {
        let v: u64;
        // SAFETY: reads a read-only architectural register.
        unsafe { core::arch::asm!("mrs {}, cntfrq_el0", out(reg) v, options(nomem, nostack)) };
        v
    }
    let freq = cntfrq().max(1);
    let deadline = cntpct().wrapping_add(freq.saturating_mul(ms) / 1000);
    while cntpct() < deadline {
        core::hint::spin_loop();
    }
}

#[cfg(not(target_arch = "aarch64"))]
fn busy_wait_ms(_ms: u64) {}

/// Ramp the DDR controller to its top supported frequency. Called once, early in
/// kernel init on the boot CPU, before any memory-heavy workload. Best-effort: any
/// failure leaves the DDR at its boot rate and logs why (never panics).
pub fn ramp_to_max() {
    // 1. Probe: does this firmware expose the SIP DRAM DVFS service at all?
    let (st, ver) = sip_smc(SIP_DRAM_CONFIG, 0, 0, CFG_DRAM_GET_VERSION);
    if st != 0 {
        warn!(
            "ddr-dvfs: SIP DRAM DVFS not supported (GET_VERSION status={st}); leaving DDR at boot \
             rate"
        );
        return;
    }
    info!("ddr-dvfs: SIP DRAM DVFS present (ATF DDR-DVFS version={ver})");

    // 2. Obtain the 8 KiB shared parameter page (its own physical region, distinct
    //    from the SCMI shmem) and map it.
    let (st, phys) = sip_smc(SIP_SHARE_MEM, 2, SHARE_PAGE_TYPE_DDR, 0);
    if st != 0 || phys <= 0 {
        warn!("ddr-dvfs: SHARE_MEM failed (status={st}, phys={phys:#x}); aborting");
        return;
    }
    let page = match iomap(phys as usize, SHARE_PAGE_SIZE) {
        Ok(p) => p.as_ptr(),
        Err(e) => {
            warn!("ddr-dvfs: iomap of share page {phys:#x} failed: {e:?}");
            return;
        }
    };
    // SAFETY: `page` maps SHARE_PAGE_SIZE bytes of the firmware's shared region.
    let rd =
        |off: usize| -> u32 { unsafe { core::ptr::read_volatile(page.add(off).cast::<u32>()) } };
    let wr =
        |off: usize, v: u32| unsafe { core::ptr::write_volatile(page.add(off).cast::<u32>(), v) };
    // Start from a clean page (the firmware reads several fields we must zero).
    // SAFETY: same mapped region.
    unsafe { core::ptr::write_bytes(page, 0, SHARE_PAGE_SIZE) };

    // 3. Initialize the DFS bookkeeping in firmware.
    let (st, _) = sip_smc(SIP_DRAM_CONFIG, SHARE_PAGE_TYPE_DDR, 0, CFG_DRAM_INIT);
    if st != 0 {
        warn!("ddr-dvfs: DRAM_INIT failed (status={st}); aborting");
        return;
    }

    // 4. Query the available frequency table into the shared page.
    let (st, _) = sip_smc(
        SIP_DRAM_CONFIG,
        SHARE_PAGE_TYPE_DDR,
        0,
        CFG_DRAM_GET_FREQ_INFO,
    );
    if st != 0 {
        warn!("ddr-dvfs: GET_FREQ_INFO failed (status={st}); aborting");
        return;
    }
    let count = rd(SP_FREQ_COUNT).min(6);
    let mut max_mhz = 0u32;
    for i in 0..count as usize {
        max_mhz = max_mhz.max(rd(SP_FREQ_INFO_MHZ + i * 4));
    }
    // Current rate, best-effort (BSP RK3588 may answer NOT_SUPPORTED here — do not
    // gate on it, it is only for a before/after log).
    let (get_st, cur) = sip_smc(SIP_DRAM_CONFIG, 0, 0, CFG_DRAM_GET_RATE);
    let cur_str = if get_st == 0 { cur } else { -1 };
    info!(
        "ddr-dvfs: DDR freq table count={count} max={max_mhz}MHz, current≈{cur_str} (GET_RATE \
         status={get_st})"
    );
    if max_mhz == 0 {
        warn!("ddr-dvfs: firmware reported no DDR frequencies; aborting");
        return;
    }

    if !ENABLE_SET_RATE {
        info!(
            "ddr-dvfs: probe-only (ENABLE_SET_RATE=false) — SIP DRAM interface confirmed \
             on-board, top rung {max_mhz}MHz available; NOT ramping until DDR-rail voltage is \
             verified"
        );
        return;
    }

    // 5. Voltage precondition. Ramping up below the rung's rail voltage corrupts
    //    DRAM; ensure it is satisfied before SET_RATE. (Filled in once the board's
    //    DDR rail is characterized; conservative default refuses to ramp.)
    if !ddr_rail_voltage_ready(max_mhz) {
        warn!("ddr-dvfs: DDR-rail voltage not confirmed for {max_mhz}MHz; refusing to ramp");
        return;
    }

    // 6. Request the top rung. Headless (lcdc_type = SCREEN_NULL).
    wr(SP_HZ, max_mhz.saturating_mul(1_000_000));
    wr(SP_LCDC_TYPE, SCREEN_NULL);
    let (st, async_st) = sip_smc(SIP_DRAM_CONFIG, SHARE_PAGE_TYPE_DDR, 0, CFG_DRAM_SET_RATE);

    // 7. On RK3588 the switch runs on the DDR MCU and SET_RATE returns the async
    //    "timeout" status; kick the MCU, allow a bounded retrain window, then
    //    finalize. (A synchronous st==0 with no async status means it already
    //    completed.)
    if async_st == SIP_RET_SET_RATE_TIMEOUT || st == SIP_RET_SET_RATE_TIMEOUT {
        info!("ddr-dvfs: SET_RATE handed to DDR MCU (async); starting MCU + finalizing");
        sip_smc(SIP_DRAM_CONFIG, 0, 0, CFG_MCU_START);
        // >> real retrain time (~1-5 ms); Linux's own timeout finalizer uses ~85 ms.
        busy_wait_ms(100);
        let (post_st, _) = sip_smc(
            SIP_DRAM_CONFIG,
            SHARE_PAGE_TYPE_DDR,
            0,
            CFG_DRAM_POST_SET_RATE,
        );
        if post_st != 0 {
            warn!("ddr-dvfs: POST_SET_RATE status={post_st} (DDR may be mid-switch)");
            return;
        }
    } else if st != 0 {
        warn!("ddr-dvfs: SET_RATE failed (status={st}); DDR left at boot rate");
        return;
    }
    info!("ddr-dvfs: DDR ramped to {max_mhz}MHz");
}

/// Whether the DDR/logic rail already supplies the voltage the target rung needs.
///
/// TODO(board): characterize the OrangePi-5-Plus DDR rail. If it is a fixed
/// always-on regulator at/above the 2112 MHz OPP voltage, return `true`
/// unconditionally. Otherwise raise it via the cpufreq PMIC lever (RK806 SPI /
/// RK8602 I2C) to the DMC OPP `opp-microvolt` for `_target_mhz` before returning
/// `true`. Conservative default (`false`) refuses to ramp until this is known.
fn ddr_rail_voltage_ready(_target_mhz: u32) -> bool {
    false
}
