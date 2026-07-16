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

//! RK3588 fixed-OPP-at-boot CPU DVFS — voltage-free rung (Phase 1a).
//!
//! Raises the three RK3588 CPU cluster clocks via the board-proven SCMI seam to
//! the highest OPP that still shares the 816 MHz boot OPP's voltage row, so no
//! PMIC/voltage change is needed and an undervolt hang is impossible **by
//! construction**:
//!
//! | cluster        | SCMI clock id | rate (MHz)  | core voltage row     |
//! |----------------|---------------|-------------|----------------------|
//! | A55 (little)   | 0             | 816 → 1008  | same as 816 boot row |
//! | A76 big pair 0 | 2             | 816 → 1200  | same as 816 boot row |
//! | A76 big pair 1 | 3             | 816 → 1200  | same as 816 boot row |
//!
//! Ground truth (`orangepi5plus.dts`): `cpu@0..300` carry `clocks = <scmi 0>`,
//! `cpu@400/500` carry `<scmi 2>`, `cpu@600/700` carry `<scmi 3>`. Every target
//! OPP shares the 816 MHz boot OPP's `opp-microvolt` core row: the standard SKU
//! puts that row at 0.675 V, while the industrial RK3588J/M SKU (selected by the
//! `specification_serial_number` nvmem cell) puts it at 0.75 V. The safety of
//! this rung is **"shares the boot OPP's voltage row"**, NOT "needs 0.675 V" —
//! because the board is provably stable at its boot rate on that row, any OPP on
//! the SAME row is safe regardless of the (unmeasured) absolute boot voltage,
//! under either SKU. A higher OPP that steps to a new row needs a real voltage
//! lever and is out of scope here.
//!
//! Registration and ordering: a `PostKernel` / `DEFAULT` rdrive probe, so the
//! CRU + SCMI providers (registered at `CLK` priority) are already live, and it
//! runs inside `devices::probe_all_devices()` — **before**
//! `start_secondary_cpus()` — so the A76 clusters are reclocked while no core is
//! scheduled on them (the live A55 id-0 switch is BL31's glitch-free path). It
//! binds to the CPU nodes rather than `arm,scmi-smc` (which the SCMI driver
//! already owns; a second driver on that node would never get an `on_probe`),
//! and applies exactly once via a one-shot guard because several `cpu@*` nodes
//! match.

use core::sync::atomic::{AtomicBool, Ordering};

use fdt_edit::Phandle;
use log::{info, warn};

use crate::{probe::OnProbeError, register::ProbeFdt, soc::scmi};

/// SCMI clock id of the A55 (little) cluster — cpu0..3.
const A55_CLK_ID: u32 = 0;
/// SCMI clock ids of the two A76 (big) cluster pairs — cpu4/5 and cpu6/7. Both
/// must be set; they cover different core pairs.
const A76_CLK_IDS: [u32; 2] = [2, 3];

/// A55 target and hard ceiling: the top OPP still on the 816 MHz boot voltage
/// row. `set_clock_rate` must never be driven above this for the A55 cluster.
const A55_MAX_HZ: u64 = 1_008_000_000;
/// A76 target and hard ceiling: the top OPP still on the 816 MHz boot voltage
/// row. `set_clock_rate` must never be driven above this for an A76 cluster.
const A76_MAX_HZ: u64 = 1_200_000_000;

/// One-shot guard: several `cpu@*` nodes match, but the reclock runs once.
static APPLIED: AtomicBool = AtomicBool::new(false);

crate::model_register!(
    name: "RK3588 CPU DVFS SCMI",
    level: ProbeLevel::PostKernel,
    priority: ProbePriority::DEFAULT,
    probe_kinds: &[
        ProbeKind::Fdt {
            compatibles: &["arm,cortex-a55", "arm,cortex-a76"],
            on_probe: probe
        }
    ],
);

fn probe(_probe: ProbeFdt<'_>) -> Result<(), OnProbeError> {
    // Several CPU nodes match this driver; only the first invocation reclocks.
    if APPLIED.swap(true, Ordering::AcqRel) {
        return Ok(());
    }

    // The `scmi::*` helpers ignore the phandle (single global agent); pass a
    // dummy so we do not depend on parsing the node's clock specifier.
    let phandle = Phandle::from(0u32);

    // Safety preflight (read-only): confirm this firmware actually services the
    // CPU-cluster clocks before touching any of them. `describe_rates` changes
    // no state, so this cannot hang or perturb the clocks; treat its result as
    // accept/reject only. If any target id is rejected, leave every cluster at
    // its boot rate and bail — there is no raw-CRU fallback in this phase.
    for id in [A55_CLK_ID, A76_CLK_IDS[0], A76_CLK_IDS[1]] {
        if scmi::describe_rates(phandle, id).is_none() {
            warn!(
                "cpufreq: SCMI does not service CPU cluster clock id {id}; leaving all CPU \
                 clusters at their boot rate (no DVFS applied)"
            );
            return Ok(());
        }
    }

    let a55_before = read_mhz(phandle, A55_CLK_ID);
    if !set_and_verify(phandle, A55_CLK_ID, A55_MAX_HZ, A55_MAX_HZ) {
        warn!("cpufreq: A55 reclock did not verify; stopping (A76 left at boot rate)");
        return Ok(());
    }
    let a55_after = read_mhz(phandle, A55_CLK_ID);

    let a76_before = read_mhz(phandle, A76_CLK_IDS[0]);
    for id in A76_CLK_IDS {
        if !set_and_verify(phandle, id, A76_MAX_HZ, A76_MAX_HZ) {
            warn!("cpufreq: A76 cluster clock id {id} reclock did not verify; stopping");
            return Ok(());
        }
    }
    let a76_after = read_mhz(phandle, A76_CLK_IDS[0]);

    info!("cpufreq: A55 {a55_before}->{a55_after}, A76 {a76_before}->{a76_after} MHz");

    Ok(())
}

/// Programs `clock_id` to `target`, but never above `ceiling` (the hard cap on
/// the boot voltage row), then verifies the platform actually applied it.
/// Returns `true` only when the read-back matches the request.
///
/// `target == ceiling` in Phase 1a; the clamp is defense in depth so a future
/// edit can never push a cluster past its boot-voltage-safe ceiling. A rejected
/// set or a deviating read-back is reported and returns `false` so the caller
/// stops rather than leaving a partial/phantom state — the board stays on
/// whatever rate the firmware last confirmed.
fn set_and_verify(phandle: Phandle, clock_id: u32, target: u64, ceiling: u64) -> bool {
    if target > ceiling {
        warn!(
            "cpufreq: refusing to set clock id {clock_id} to {target} Hz (above boot-safe \
             ceiling {ceiling} Hz)"
        );
        return false;
    }
    if scmi::set_clock_rate(phandle, clock_id, target).is_none() {
        warn!("cpufreq: SCMI rejected clock id {clock_id} set to {target} Hz; left unchanged");
        return false;
    }
    match scmi::clock_rate(phandle, clock_id) {
        Some(applied) if applied == target => true,
        Some(applied) if applied > ceiling => {
            warn!(
                "cpufreq: clock id {clock_id} read back {applied} Hz ABOVE boot-safe ceiling \
                 {ceiling} Hz (requested {target} Hz); stopping"
            );
            false
        }
        Some(applied) => {
            warn!(
                "cpufreq: clock id {clock_id} read back {applied} Hz, requested {target} Hz; \
                 stopping"
            );
            false
        }
        None => {
            warn!("cpufreq: could not read back clock id {clock_id} after set; stopping");
            false
        }
    }
}

/// Current rate of `clock_id` in MHz, or 0 if it cannot be read (logging only).
fn read_mhz(phandle: Phandle, clock_id: u32) -> u64 {
    scmi::clock_rate(phandle, clock_id).unwrap_or(0) / 1_000_000
}
