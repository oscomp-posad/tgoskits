<!-- from workflow wkgnflo45; safety-review pass pending (session limit) -->

# StarryOS RK3588 DVFS / cpufreq — Phased Safe Implementation Plan

## 0. Reconciliation of the four investigations (what's true)

The four reports agree on the mechanics but **conflict on one safety-critical point** that I resolved by reading the tree:

**CRITICAL CORRECTION — SCMI does NOT raise voltage on this board.** One investigation claimed "SCMI routes to ATF which sets PLL AND regulator voltage atomically." That is **wrong for RK3588**. Verified:
- `drivers/ax-driver/src/soc/scmi.rs:137` `set_clock_rate` calls `scmi.clock_rate_set_direct(clock_id, rate)` — a pure clock rate set.
- DTS `scmi` node (`orangepi5plus.dts:1759`) exposes **only** `protocol@14` (clock) + `protocol@16` (reset). There is **no** `perf@13` and **no** `voltage@17`.

Consequence: **SCMI and raw-CRU are BOTH frequency-only.** Neither is a "free pass" to high OPPs. The undervolt ceiling applies to SCMI exactly as it does to raw PLL writes. This is the single most important thing to get right — it prevents the fatal mistake of `scmi::set_clock_rate(id, 2_256_000_000)` "because SCMI is safe."

**Verified ground truth (this worktree):**
- SCMI CPU clock IDs (DTS `cpu@*` `clocks = <0x0e N>`): **A55 = id 0** (cpu0-3), **A76 big0 = id 2** (cpu4/5), **A76 big1 = id 3** (cpu6/7). All three assigned `0x30a32c00 = 816 MHz` at boot.
- OPP voltage ladders (base worst-case bin, first column = required core µV):
  - **A55 (cluster0):** 816 **& 1008 both 0.675V** (`0xa4cb8`); 1200 → 0.7125V; 1416 → 0.7625V; 1608 → 0.85V; 1800 → 0.95V.
  - **A76 (cluster1/2):** 816, 1008 **& 1200 all 0.675V** (`0xa4cb8`); 1416 → 0.725V; 1608 → 0.7625V; 1800 → 0.85V; 2016 → 0.925V; 2256 → 1.000V (`0xf4240`, rated max).
- Proven SCMI seam to copy: `drivers/ax-driver/src/block/rockchip/clock.rs:129` `scmi::set_clock_rate(self.phandle, self.clock_id, rate)` (phandle is ignored inside scmi.rs — single global agent; SMC id `0x82000010` + shmem `0x10f000` proven working).
- `PLL_RATE_TABLE` already has **1008** (`pll.rs:112`, p2/m336/s2) and **1200** (`pll.rs:109`, p2/m200/s1); it does **not** have 1800/2256.
- **No voltage lever exists anywhere:** grep for `set_voltage|rk806|rk8602|rk860x|spmi` across `drivers/` + `components/` = **zero**. Only `enable_fixed_regulator` (GPIO on/off, no µV). Board rails per DTS: `vdd_cpu_lit_s0` = RK806 DCDC2 on **SPI2** (`feb20000`); `vdd_cpu_big0_s0` = RK8602 @0x42 and `vdd_cpu_big1_s0` = RK8603 @0x43 on **I2C** (`fd880000`). **No SPMI on this board.**
- No cpufreq/governor/dvfs code exists (grep = zero).
- Boot seam (verified line numbers, this tree): `init_scheduler()` `axruntime/src/lib.rs:278` → `devices::probe_all_devices()` **:301** (PostKernel probes → CRU + SCMI become live) → `start_secondary_cpus()` **:324**.

---

## 1. PATH DECISION — **SCMI-to-ATF primary; raw CRU-PLL only as a gated fallback**

**Use SCMI as the CPU-clock mechanism.** Justification (safety first):

1. **BL31 owns B0PLL/B1PLL/LPLL.** These are exactly the PLLs BL31's SCMI CPUL/CPUB01/CPUB23 handlers program. Poking CRU directly from the OS **races the firmware** that thinks it owns them → stale cached rate, corrupt PLL/mux state. SCMI keeps a single owner.
2. **SCMI does the full `armclk` sequence, the raw path does not.** BL31's CPU-clock handler also reprograms the cluster **DBG/ATCLK/GIC/PCLK divisors** so the sub-buses stay in-spec as the core scales. StarryOS's raw path leaves `bigcore0/1_clksel_con` / `dsu_clksel_con` **unused** (zero call sites) — at high freq those derived clocks run out of spec = a *second, independent* instability source that SCMI avoids for free.
3. **Board-proven.** The identical `scmi::set_clock_rate` call already programs the dwmmc clock on this exact board (`block/rockchip/clock.rs:129`). No new transport risk.
4. **Less code, no footguns.** SCMI needs no `PLL_RATE_TABLE` edits and no `find_pll_params` VCO-guard work (the raw path can't even synthesize 1800/2256 today — `find_pll_params` rejects VCO > 4500 MHz).

Raw-CRU (`rk3588_set_clock_rate(id, hz)`) is retained **only** as a fallback for Phase 1a's same-voltage OPPs (1008/1200 are in the table today) **if** the `describe_rates` preflight shows this BL31 does not accept CPU-cluster rate_set. It must never be driven above the boot-voltage ceiling and never concurrently with the SCMI path.

**Both paths raise frequency only. Voltage is a separate, not-yet-existing lever. Every rung below is gated on that fact.**

---

## 2. PHASE 0 (mandatory prerequisite for anything above the same-voltage ceiling) — measure V_boot

The Phase-1b/2 ceiling is a pure function of the **actual boot rail voltage**, which is **unmeasured** today (only frequency = 816 is confirmed). RK806/RK8602/RK8603 are all `regulator-boot-on` + `always-on`, so they sit at their PMIC OTP reset default (plausibly ~0.9V, but unverified; could be 0.75 or 0.675).

- **Fastest:** boot Linux on the board at idle and read `cat /sys/kernel/debug/regulator/regulator_summary` (or `.../vdd_cpu_lit_s0/voltage`, `vdd_cpu_big0_s0`, `vdd_cpu_big1_s0`). One number per rail.
- **Better (self-verifying):** add a tiny **read-only** VSEL probe in StarryOS — RK806 VSEL over SPI2 `feb20000`, RK8602/03 VSEL over I2C `fd880000` @0x42/0x43. Read-only = zero hang risk, and it lets the kernel self-check the ceiling before every raise.

Phase 0 gates Phase 1b and Phase 2 only. **Phase 1a does not need it** (see below).

---

## 3. PHASE 1 — voltage-free frequency raise (no PMIC writes)

### Phase 1a — airtight-safe, needs no measurement (this is the first PR)

**Target frequencies (same 0.675V as the proven-stable 816 boot OPP):**
- **A55 (SCMI id 0): 816 → 1008 MHz** (+23.5%)
- **A76 (SCMI id 2 AND id 3): 816 → 1200 MHz** (+47%) — must set **both** id 2 and id 3 to cover all four A76 cores.

**Why it's safe with zero voltage knowledge:** the board is provably stable at 816 MHz @ 0.675V ⟹ the rail is ≥ 0.675V ⟹ any OPP whose required voltage is ≤ 0.675V is safe **regardless of the actual V_boot**. Verified: A55 1008 and A76 1200 both require exactly 0.675V (`0xa4cb8`), identical to boot. This is airtight; it does not rest on the ~0.9V assumption.

**Expected harness win** (curve: A55 ~0.20 ev/s/MHz, A76 ~0.43; measured baselines A55 ~159, A76 ~347 @ 816):
- A55 sysbench single-thread **~159 → ~196**
- A76 sysbench single-thread **~347 → ~510**

**Code + where:**
- Add `drivers/ax-driver/src/soc/rockchip/cpufreq.rs`, a small rdrive driver: `model_register!(level: PostKernel, priority: ProbePriority::DEFAULT)` (DEFAULT=256 > CLK=6, so CRU + SCMI are already registered), bound to the `arm,scmi-smc` node (or the `operating-points-v2`/cpu node). In `on_probe`, after a preflight (below), call the existing free functions:
  - `scmi::set_clock_rate(phandle, 0, 1_008_000_000)`
  - `scmi::set_clock_rate(phandle, 2, 1_200_000_000)`
  - `scmi::set_clock_rate(phandle, 3, 1_200_000_000)`
- This mirrors the proven in-probe consumer at `block/rockchip/clock.rs:129`, keeps board policy **out** of generic `axruntime`, and runs before `start_secondary_cpus()` (:324) so the A76 clusters are reclocked while **no core is running on them** (primary = cpu0/A55; id0's live-core switch is BL31's glitch-free path).
- Acceptable simpler fallback: a board-feature-gated hook in `axruntime/src/lib.rs` immediately after `devices::probe_all_devices()` (:301) and before `start_secondary_cpus()` (:324) — but gate it behind a board feature so RK3588 policy doesn't leak into the shared runtime.

**Preflight (do this before the first CPU set):** `scmi.describe_rates(0x04)` on ids 0/2/3 to confirm this BL31 accepts CPU-cluster rate_set and to read the discrete rates it permits. Treat `set_clock_rate → None` as a **hard stop, not a retry** (fail-safe: no change occurred). If ids 0/2/3 are rejected, fall back to `rk3588_set_clock_rate(LPLL=3, 1_008_000_000)` / `(B0PLL=1,1_200_000_000)` / `(B1PLL=2,1_200_000_000)` — those rates are already in `PLL_RATE_TABLE`, no table edit needed, still ≤ boot voltage.

**Verify with the harness:** cpuprobe (CNTVCT-timed, frequency-invariant) → per-core ips must rise by the exact ratio (1008/816 = ×1.235, 1200/816 = ×1.47); `sched_getcpu` confirms core identity; sysbench single-thread hits ~196 (A55) / ~510 (A76). If ips does **not** move, read back `scmi::clock_rate(phandle, id)` to see the actually-applied rate and check the SCMI warn log. Snapshot this as **rung R1**.

### Phase 1b — higher OPPs, still no PMIC, **gated on measured V_boot**

Only after Phase 0. Raise one OPP at a time, verifying (and confirming no hang) before advancing, using **base worst-case-bin** voltages (nvmem speed-bin is not read) and a **~25–50 mV guardband** below V_boot:
- If **V_boot ≈ 0.9V** (likely OTP default): A55 → up to **1608 MHz** (0.85V, ×1.97), A76 → up to **1800 MHz** (0.85V, ×2.20). Captures **most** of the ultimate target with zero voltage risk. (A55 1800 @0.95V and A76 2016 @0.925V are excluded — above 0.9V.)
- If **V_boot ≈ 0.75V:** A55 → 1200 (0.7125V), A76 → 1416 (0.725V).
- If **V_boot ≈ 0.675V:** stop at Phase 1a (1008/1200).

---

## 4. PHASE 2 — full max OPP (A55 1.8 GHz / A76 2.256 GHz) — requires a real voltage lever

This is the only way to reach the full **×2.25 (A55) / ×2.82 (A76)** target. Required: A55 1800 @ **0.95V**, A76 2256 @ **1.00V** — both above boot. There is no shortcut; SCMI cannot do it (no perf/voltage domain on this BL31).

**Voltage path to add (write drivers that do not exist today):**
- **RK806 DCDC_REG2 over SPI2** (`feb20000`, spi-max 1 MHz) for `vdd_cpu_lit_s0` — DT-clamped **0.55–0.95V**, ramp **12500 µV/µs**, 6.25 mV VSEL step.
- **RK8602 (I2C `fd880000` @0x42)** and **RK8603 (@0x43)** for `vdd_cpu_big0/1_s0` — DT-clamped **0.55–1.05V**, ramp **2300 µV/µs**, 6.25 mV step. A76 2256 needs 1.00V (inside 1.05V limit); A55 1800 needs 0.95V (= RK806 DT max).
- Add a minimal in-Rust OPP table (freq → required µV, per cluster) and a strict-ordering DVFS applier.

**Gating safety checks (all mandatory):**
1. **Strict ordering:** raise V → **wait ramp-settle** (RK806 12.5 mV/µs; rk860x 2.3 mV/µs → compute settle from ΔµV) → **then** SCMI rate_set up. Reverse when lowering: SCMI rate_set down → then lower V. A missed settle-delay is an undervolt window.
2. **Clamp to DT max in software:** RK806 ≤ 0.95V, rk860x ≤ 1.05V. Reject any OPP whose voltage exceeds the buck's range.
3. **Companion rail (vdd_log / cpu-mem):** the OPP `opp-microvolt` mem column and `vdd_log_s0` (DCDC3) must be co-raised at high OPP or cache/SRAM goes unstable even with a correct core rail.
4. **Thermal:** StarryOS has no thermal governor; only the RK3588 hardware TSADC (~120 °C) protects. Cap sustained all-core max, or add a throttle, before running 4×A76@2.256 + 4×A55@1.8 near 1.0V.
5. **VSEL encoding:** verify the 6.25 mV-step math and the exact SPI/I2C VSEL register per part before the first write — an encoding error is a direct over/under-volt.
6. Keep using base (not binned) voltages until nvmem leakage/PVTM is read.

---

## 5. INTRA-LEVER LADDER (snapshot rungs)

Each rung = a harness snapshot (cpuprobe freq + sysbench single-thread). Advance one rung at a time; never skip.

| Rung | Path / gate | A55 (id0) | A76 (id2+id3) | A55 sysbench | A76 sysbench | Voltage need vs boot |
|------|-------------|-----------|----------------|--------------|--------------|----------------------|
| R0 (now) | boot | 816 | 816 | ~159 | ~347 | 0.675V (at boot) |
| **R1 (PR1, Phase 1a)** | SCMI, no measurement | **1008** | **1200** | ~196 (+23.5%) | ~510 (+47%) | **same 0.675V — airtight** |
| R2 (Phase 1b) | gated V_boot ≥ ~0.76V | 1200 | 1416 | ~240 | ~609 | 0.7125 / 0.725V |
| R3 (Phase 1b) | gated V_boot ≥ ~0.9V | 1416 → 1608 | 1608 → 1800 | ~283 → ~322 | ~692 → ~774 | ≤0.85V |
| R4 (Phase 2) | regulator up first | 1800 | 2016 → 2256 | ~360 (×2.25) | ~867 → ~970 (×2.82) | 0.95V / 1.00V |

---

## 6. SAFETY GATES — exactly what hangs the board, and how each rung avoids it

**Recovery reality:** a hung board needs a **physical power-cycle** (costly here). Every gate below is designed to *never reach* a hang, because there is no software watchdog recovery in this path. Validate one rung at a time with serial capture before advancing.

1. **Undervolt hang (primary risk).** Freq above what the rail supports → lockup.
   - *R1:* impossible by construction — same 0.675V as proven-stable boot.
   - *R2–R3:* gated on measured V_boot minus a 25–50 mV guardband, base worst-case voltages, one OPP step at a time.
   - *R4:* voltage raised **first** with ramp-settle; reject any OPP above the buck clamp.
2. **Racing BL31 on the PLLs.** Avoided by using **SCMI exclusively** for CPU clocks. Never drive CRU B0/B1/LPLL from the OS while the SCMI path is live. Raw-CRU is fallback-only and only for R1's already-safe rates.
3. **Sub-clock (DBG/ATCLK/GIC/PCLK) overclock.** Avoided because SCMI = BL31's full armclk sequence programs those divisors. (This is a *reason not to use* the raw path at high freq, where those divisors are left at 816-sized values.)
4. **BL31 rejects CPU rate_set.** Fail-safe — `set_clock_rate` returns `None`, nothing changes. Preflight `describe_rates` on ids 0/2/3; treat `None` as a hard stop, not "retry higher."
5. **Reclocking the running core.** BL31's SCMI CPU switch is glitch-free; additionally, apply Phase-1 at PostKernel probe (:301) **before** `start_secondary_cpus()` (:324) so the A76 clusters carry no running core and the A55 switch is the only live-core event.
6. **SMP brownout / smp8 boot hang (known board issue).** Higher all-core frequency raises power draw. R1 keeps voltage constant (power scales ~linearly with freq only), so brownout risk is modest; still validate with a **bounded core count** first. R4 (voltage up + full freq) is the real brownout/thermal danger — pair it with the thermal cap.
7. **Phase-2 ramp/settle & VSEL-encoding bugs.** Covered by the strict-ordering + settle-delay + DT-clamp + verified-encoding gates in §4.
8. **Companion-rail (vdd_log/mem) undervolt at high OPP.** Co-raise per the OPP mem column in Phase 2.

---

## 7. Concrete FIRST PR scope

**Title:** `feat(soc/rockchip): fixed-OPP-at-boot CPU DVFS via SCMI (voltage-free rung)`

**Contents (Phase 1a only — airtight-safe, no measurement, no PMIC, no rate-table edits):**
1. New `drivers/ax-driver/src/soc/rockchip/cpufreq.rs`: rdrive `model_register!(level: PostKernel, priority: ProbePriority::DEFAULT)` bound to the `arm,scmi-smc` (or opp-v2/cpu) node, board-feature-gated.
2. In `on_probe`: `describe_rates(0x04)` preflight on SCMI ids 0/2/3 (log permitted rates); then `scmi::set_clock_rate(phandle, 0, 1_008_000_000)`, `(…, 2, 1_200_000_000)`, `(…, 3, 1_200_000_000)`. Treat any `None` as hard stop; on preflight-reject, fall back to `rk3588_set_clock_rate(3,1_008_000_000)/(1,1_200_000_000)/(2,1_200_000_000)` (rates already in `PLL_RATE_TABLE`).
3. Read back with `scmi::clock_rate(phandle, id)` and log applied rate.
4. Wire the feature into `apps/starry/sysbench-board/build-aarch64-*.toml` (already enables `rockchip-soc` + `rockchip-dwmmc`, so both SCMI and CRU are compiled in).

**Explicitly OUT of PR1 (deferred):** any regulator/voltage code, any `PLL_RATE_TABLE` edits, any OPP above 1008 (A55) / 1200 (A76), any governor. Those depend on Phase 0 measurement (1b) or the new RK806/RK860x drivers (2).

**Acceptance:** on-board harness shows A55 core ips ×1.235 (→ sysbench ~196) and all four A76 cores ×1.47 (→ sysbench ~510), `sched_getcpu` correct, board stable through a full sysbench pass and reboot. That snapshot is rung **R1** and the safe foundation the voltage work builds on.

**Verified anchors:** `scmi.rs:137` (set) / `:83` (get) / `:29` (probe); proven caller `block/rockchip/clock.rs:129`; SCMI ids in `orangepi5plus.dts:1767` (`assigned-clocks <0x0e 0 / 2 / 3>`) + `cpu@0/400/600` `clocks`; A55/A76 OPP µV at `dts:785-835` / `dts:~1000-1090`; `PLL_RATE_TABLE` 1008/1200 at `pll.rs:112/109`; boot seam `axruntime/src/lib.rs:301` / `:324`; no voltage/cpufreq code anywhere (grep clean).
