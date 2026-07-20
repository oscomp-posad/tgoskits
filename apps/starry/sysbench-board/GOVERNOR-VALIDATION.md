# Ondemand DVFS governor — board validation (OrangePi-5-Plus RK3588, 2026-07-18)

The `rk3588-cpufreq` driver's dynamic ondemand governor (see
`drivers/ax-driver/src/soc/rockchip/cpufreq.rs` + the `cpufreq-gov` task in
`os/StarryOS/kernel/src/entry.rs`) was validated on the board.

## How freq is read

`cpuprobe <cpu>` (harness) pins to a core, self-loads ~1 s, then times a fixed
compute loop → `ips`. `pmc_ok=0` on StarryOS, so freq is derived from ips vs a
same-binary baseline: **A55 58.26M ips @ 816 MHz, A76 77.69M ips @ 816 MHz**.
cpuprobe's own ~1 s self-load means the 100 ms governor has already scaled the
probed core's cluster **up** by the time it measures — so ips reads the
governor-chosen (loaded) frequency.

## Results

| build | A55 cpu0–3 | A76 cpu4–7 | note |
|---|---|---|---|
| cluster-**average** busy (bug) | ~408 MHz | ~408 MHz | one busy core = 25%/50% of cluster, never crosses 80% up-threshold → never boosts; sits at floor |
| **per-core** + 1608/1416 caps | — | ~1733 MHz | overshoot: ring=1608 @ 762.5 mV over-delivers (undervolt) |
| **per-core** + 675 mV caps (final) | **~1018 MHz** (tgt 1008) | **~1186 MHz** (tgt 1200) | exact, voltage-safe |

- **Up-scaling**: per-core scoring boosts a cluster from its busiest CPU (like
  Linux schedutil/ondemand), so a single CPU-bound thread lifts its cluster.
  After the fix, cpuprobe ips jumped **4.4×** (A76 37.7M → 165M) and sysbench
  `cpu --threads=2` throughput **77 → 295 eps**.
- **Down-scaling**: at idle every cluster decays to the 408 MHz floor (measured
  directly in the average-logic run, where up-scaling never masked it).
- **Exactness**: capping every OPP on the 675 mV rail — the only voltage
  board-proven to make the PVTPLL deliver its SCMI target exactly — removed the
  overshoot. Governor now scales clock 408↔1200 (A76) / 408↔1008 (A55) at fixed
  675 mV; no undervolt, no PMIC voltage changes during scaling.
- **PSU**: `threads=8` all-core ran to completion with **no brownout** (1200/1008
  @ 675 mV draws less than the ~1490 MHz @ 800 mV overshoot the board already
  survived).

## Known limitations / follow-ups

- **>1200 MHz OPPs need calibration.** The PVTPLL coupling over-delivers at the
  higher OPPs, so pairing e.g. the 1608 ring with its DT 762.5 mV under-volts the
  ~1733 MHz it actually produces. Using them safely needs a per-OPP
  delivered-frequency → voltage calibration (measure freq at each rail, pick a
  voltage ≥ the delivered freq's nominal).
- **`gov:` transition logs aren't capturable** post-boot (StarryOS kernel `info!`
  stops reaching the serial once the shell owns the console; during boot the
  cores are busy so no transition fires). cpuprobe freq is the evidence.
- **sysbench eps is flat** t=2 vs t=8 (~201) — a separate StarryOS SMP-scaling
  limitation, orthogonal to DVFS.
