# BENCHMARKING.md — StarryOS RK3588 optimization ladder

**Scope.** How we measure, snapshot, and present every optimization to StarryOS on the OrangePi‑5‑Plus (RK3588: 4× A76 + 4× A55) against native Linux (Ubuntu Jammy `6.1.43-rockchip`) using the *same board and the same binaries*. This is the process the team follows for every rung.

**The thesis we are proving, one rung at a time.** Per‑core compute is already at parity (same core + same clock ⇒ same throughput — verified: StarryOS A55 sysbench 160 ≈ Linux A55@816 161; A76 351 ≈ Linux A76@816 347). The entire ~24–33× gap is **four independent policy/subsystem levers**, each separately measured and each independently fixable:

| # | Lever | Measured signal today | Headline gap |
|---|---|---|---|
| 1 | **DVFS/cpufreq** | all 8 cores stuck at ~800 MHz boot OPP; no cpufreq driver | 2.24× (A55) … 2.79× (A76), global |
| 2 | **big.LITTLE placement** | affinity works (`landed==req`), default scheduler never uses A76 | 2.2× at 800 MHz (351/159) |
| 3 | **Load balancer** | 4 threads masked to 4 A76 cores still = 1‑core (351); ceiling @800 MHz = 2046 | up to 12.9× |
| 4 | **Fault/first‑touch path** | first‑touch 256 MB 0.8–1.3 s vs Linux 0.086 s; warm BW fine | ~11× per fault |

Levers stack: single‑thread latency path `159 →(DVFS)→ 360 →(A76)→ 977` and throughput path `159 →(balancer)→ 2046 →(DVFS)→ ~5348` both land on Linux (8‑thread 5320).

**Hard environment constraints (recap — every benchmark must obey these).**
- Runs **glibc‑dynamic aarch64** binaries against the board's Ubuntu ext4 rootfs (`mmcblk1p2`). musl‑static is HARD here — do not attempt it. Build path is proven: `harness/build-harness.sh` → `docker run --platform linux/arm64 ubuntu:22.04 … gcc -O2` → one file, deps = libc[+libm] only.
- StarryOS `/bin/sh` is **dash/busybox** — POSIX sh only in on‑board scripts, no bashisms.
- **Works** on StarryOS: `sched_setaffinity`/`taskset`, `sched_getcpu`, `clone`/`futex`/pthreads, `fork`, `mmap`/`mprotect(PROT_EXEC)`, `CNTVCT_EL0`/`CNTFRQ_EL0` (EL0 generic timer, base 24 MHz).
- **Does NOT work**: EL0 `MIDR_EL1`/`PMCCNTR_EL0` (`midr_ok=0`, `pmc_ok=0` — confirmed on StarryOS *and* Linux for PMU), cpufreq/DVFS, auto load‑balancing. ⇒ **core‑type and frequency are never read on‑device; they are recovered from the Linux MIDR map + the Linux OPP curves.** Every StarryOS snapshot therefore MUST carry a paired Linux reference.
- A StarryOS board run = serial FIT `loady` (~4 min) via `env -u RUSTUP_TOOLCHAIN cargo xtask starry uboot -c build-…toml --uboot-config uboot-…toml`, which runs `starry-harness.sh` and captures the serial console to the `SYSBENCH_BOARD_DONE` sentinel. Linux reference = `ssh … 'bash -s' < linux-harness.sh`.

---

## Part 1 — The snapshot + ladder system

### 1.1 What a snapshot (one measured run) contains

A snapshot is an **immutable, append‑only** directory holding *everything needed to reproduce and defend one number*. Files, in two tiers:

**Contracts (machine‑read, stable across tool refactors):**
- `manifest.json` — provenance sidecar (schema §1.4). The contract that pins snapshot ⇄ commit ⇄ tag ⇄ Linux ref.
- `metrics.json` — the canonical flat scalar table the renderer + regression gate read. Decoupled from decompose so it survives decompose refactors. Carries median+spread over N=3 repeats.
- `lever-config.json` — which levers are ON and each one's intra‑lever knob (machine‑visible so `10-dvfs-perf` and `11-dvfs-opp-sweep` are never confused).

**Raw evidence (verbatim, human‑ and re‑parse‑able):**
- `starry-harness.out` — verbatim StarryOS serial capture: the `HS_*` tag stream (`HS_UNAME`, `HS_PC`, `HS_PSB`, `HS_PM`, `HS_MX`, `HS_MEMSW`, plus the new `HS_PLACE`/`HS_LBSCALE`/`HS_FTFAULT` lines) ending at `SYSBENCH_BOARD_DONE`. **Primary raw StarryOS measurement.**
- `linux-harness.out` (or a symlink into `refs/linux/<id>/`) — the `HL_*` reference stream: `HL_CORE` MIDR map, `HL_REF` per‑cluster ev‑vs‑freq curves (the calibration), `HL_PC`/`HL_PM`/`HL_MX`. Same binary, same board, captured close in time.
- `decompose.txt` — human‑readable `decompose.py` report (the narrative artifact for the write‑up).
- `decompose.json` — machine‑readable decompose output (NEW `--json` mode, §1.4/Appendix): per‑core `{type,freq_mhz,ev,landed,affinity_ok}`, lever multipliers, full matrix, memory sweep, headline `pct_of_linux`.
- `boot.log` — full StarryOS serial boot capture. **A rung's headline is only valid if `boot.log` shows the claimed core count** (`nproc`, smp8 vs silent smp1 fallback, PSU brownout, panics). Catches "labeled smp8, actually smp1".
- `build-config.toml` + `uboot-config.toml` — verbatim copies (not references) of the exact `build-aarch64-unknown-none-softfloat.toml` (`features`, `max_cpu_num`, `log`) and `uboot-orangepi-5-plus.toml` (serial/dtb/`shell_init_cmd`) frozen INTO the snapshot.
- `notes.md` — free‑form operator notes: thermal state, PSU events, retries, deviations.
- `perf/` (optional) — flamegraph.svg / perf capture (ties into the perf‑multicore effort) for rungs where a callchain explains *why* a lever moved (or didn't) the number.

### 1.2 On‑disk layout

Root: `apps/starry/sysbench-board/snapshots/`

```
snapshots/
  README.md                      # scheme + "how to add a rung" + capture command
  refs/
    linux/<board>-<lkver>-<date>-<refsha>/    # SHARED Linux reference snapshots
        linux-harness.out  manifest.json  curves.json   # A55/A76 ev-vs-freq extracted
  rungs/
    00-baseline/  10-dvfs-perf/  11-dvfs-opp-sweep/  12-dvfs-ondemand/
    20-placement-affinity/  21-placement-heuristic/
    30-balancer-spread/  31-balancer-steal/
    40-faultpath-prefault/  41-faultpath-thp/
        <runid>/               # ONE immutable measured run — all §1.1 files live here
        latest -> <runid>      # symlink to the ACCEPTED run (the only mutable pointer)
  iso/                         # attribution: each single lever measured vs baseline
    dvfs/<runid>/  placement/<runid>/  balancer/<runid>/  faultpath/<runid>/
  ladder.json  ladder.md  ladder.html  ladder.csv     # GENERATED, mutable
```

**Rules.**
- A run dir is **append‑only / immutable** once written. Re‑measuring never overwrites — it creates a new `<runid>` and, if accepted, repoints `latest`. Only `latest` symlinks and generated `ladder.*` mutate.
- Linux refs are **shared** (many rungs point at one ref) so `% of Linux` stays comparable across rungs. Changing the ref is an explicit re‑baseline.
- Tens‑digit rung prefixes (`00,10,20,30,40`) leave gaps so intra‑lever sub‑rungs (`11,12,21,31`) and inserted rungs slot in without renumbering.
- **Migration note (do this first):** the existing `snapshots/0000-baseline-dbbe0e065/` becomes `rungs/00-baseline/20260716-HHMMSS-dbbe0e065-smp8/` with a `latest` symlink; its `linux-harness.out` is promoted to `refs/linux/opi5p-6.1.43-20260716-<refsha>/`. Keep the old dir as a symlink for one cycle if any tooling references it.

### 1.3 Naming & versioning

- **Rung dir:** `<NN>-<lever-slug>` — `NN` = 2‑digit order key with gaps; slug = short human lever name (`dvfs-perf`, `placement-affinity`, `balancer-steal`). `manifest` carries the semantic stage id (`"R2"`) + `order` (20).
- **Run id:** `<YYYYMMDD-HHMMSS>-<starry-short-sha>-smp<N>` — sortable, unique, self‑describing; ties the measurement to code + build variant. Many runs per rung; `latest` → accepted run.
- **Linux ref id:** `<board>-<lkver>-<date>-<refsha>`, e.g. `opi5p-6.1.43-20260716-ab12cd`. A ladder pins to ONE ref id.
- **Tags:** on acceptance, tag immutably `snap/R<n>-<lever>-<date>`; `manifest.starry_sha` MUST equal the tag commit.
- **`schema_version`** field in BOTH `manifest.json` and `metrics.json`; renderer/regression tools assert a supported version. `rebased_from` links lineage when the stack is rebased.

### 1.4 Canonical contracts (schemas)

**`manifest.json`** (provenance — the thing that makes a delta attributable):
```json
{
  "schema_version": 1,
  "rung": "R1", "order": 10, "name": "DVFS performance governor",
  "levers_on": ["dvfs"],
  "starry": { "sha": "…full…", "short_sha": "…", "branch": "ladder/10-dvfs",
              "tag": "snap/R1-dvfs-20260801", "rebased_from": null },
  "toolchain": "nightly-2026-04-27",
  "build": { "target": "aarch64-unknown-none-softfloat", "max_cpu_num": 8,
             "features": ["ax-driver/rockchip-soc","ax-driver/rockchip-sdhci",
                          "ax-driver/rockchip-dwmmc","ax-driver/rockchip-cpufreq"],
             "log": "Info" },
  "lever_config": { "dvfs": { "mode": "performance", "opp_khz": 1800000 } },
  "harness": { "cpuprobe_sha256":"…","membw_sha256":"…","ftfault_sha256":"…",
               "lbscale_sha256":"…","placeprobe_sha256":"…","sysbench_sha256":"…",
               "PRIME": 20000, "time_windows_s": { "psb":5, "mx":5, "probe":5 } },
  "board": { "soc":"RK3588", "model":"OrangePi-5-Plus",
             "serial":"/dev/cu.usbserial-AQ03MLX2", "dtb":"orangepi-5-plus.dtb",
             "linux_uname":"6.1.43-rockchip", "rootfs":"mmcblk1p2" },
  "linux_ref": "opi5p-6.1.43-20260716-ab12cd",
  "decompose_sha256": "…",
  "operator": "…", "start_utc": "…", "end_utc": "…", "retry_count": 0
}
```

**`metrics.json`** (the stable flat contract; each scalar is `{median, spread, n}`):
```json
{
  "schema_version": 1, "rung": "R1", "runid": "…", "starry_sha": "…",
  "linux_ref": "opi5p-6.1.43-20260716-ab12cd",
  "cpu_ev":   { "t1":{"median":..,"spread":..,"n":3}, "t2":{…}, "t4":{…}, "t8":{…} },
  "thr_ev_t4": {…}, "mutex_t4_s": {…},
  "mem_MiBps": { "bs1k_read":{…},"bs1k_write":{…},"bs1m_read":{…},"bs1m_write":{…} },
  "firsttouch_s": { "A55":{…}, "A76":{…} },
  "membw_GBps":  { "A55_memcpy":{…}, "A76_memcpy":{…} },
  "percore": { "0":{"type":"A55","ev":160,"landed":0,"affinity_ok":true}, "…":{…} },
  "levers_multiplier": { "dvfs":{…}, "placement":{…}, "balancing":{…} },
  "pct_of_linux": { "cpu_t1":{…}, "cpu_t8":{…}, "thr_t4":{…} },
  "gauges": { "unpinned_landed_type":"A55", "max_freq_mhz":816,
              "scaling_t8_over_t1":1.0, "firsttouch_A76_s":0.80 }
}
```
The `gauges` block is what attribution + regression **key on** (each lever owns one gauge — §1.7).

**`lever-config.json`** — explicit ON/knob record, e.g.
`{"dvfs":{"mode":"performance","opp_khz":1800000},"placement":{"mode":"off"},"balancer":{"mode":"off"},"faultpath":{"mode":"off"}}`.

### 1.5 Rendering the ladder for a presentation

`render_ladder.py` reads every `rungs/<NN>/latest/metrics.json` + the pinned `refs/linux/…/curves.json` and emits `ladder.{md,html,csv,json}`. Uses the **dataviz skill** for the HTML (light/dark, accessible). Five surfaces:

1. **Ladder table** — one row per rung ordered by `NN`: `Stage | lever added | headline (sysbench cpu ev/s, BOTH single‑thread and 8‑thread aggregate) | absolute value | ×‑vs‑baseline | Δ‑from‑previous | % of Linux parity`. Mirrors the roadmap `159 → 360 → 977 → 2046 → ~5348` but auto‑filled from measured `metrics.json`.
2. **Waterfall (the money slide)** — starts at the baseline bar (159) and each lever adds a labeled step climbing toward the Linux‑parity line (drawn from the *paired Linux ref*, NOT hardcoded). Waterfall because levers stack multiplicatively; it shows each lever's marginal contribution and where the remaining gap is.
3. **Two tracks, rendered separately** (they combine differently for serial vs parallel products): **LATENCY/product** path (baseline→placement→DVFS, single‑thread ev, the RKNN/tennis pipeline) and **THROUGHPUT** path (baseline→balancer→DVFS, 8‑thread aggregate). One waterfall per track.
4. **Parity gauge per rung** — `starry_metric / linux_same-binary_metric × 100`, headline like "R3 reaches 38% of Linux 8‑thread throughput".
5. **Projected vs measured** — rungs not yet implemented render as dashed/projected bars computed from `decompose.py`'s model multipliers; implemented rungs render solid/measured. The full target ladder shows NOW and fills in as levers land. Every rendered value carries its rung's `starry_sha` + `linux_ref` in a tooltip/footnote.

Sub‑rungs (`11`,`12`) collapse under their lever header by default; `--expand` shows intra‑lever progression.

### 1.6 Git branch / worktree strategy

Pin a known‑good upstream base as tag **`ladder-base`** on upstream `dev` (the roadmap warns of stale‑base artifacts: `oscomp-posad` missing axtask SMP‑wake #1495/#1426 and block‑IRQ #1512 — do **not** base on it).

**Cumulative ladder stack** — exactly ONE lever per branch, each branched from the previous so its tree = base + all lower levers + this one:
```
ladder/00-baseline   # harness + snapshot tooling only, no kernel change
ladder/10-dvfs       # from 00; ONLY cpufreq/DVFS driver + governor
ladder/20-placement  # from 10; ONLY big.LITTLE placement
ladder/30-balancer   # from 20; ONLY load-balancer / work-stealing
ladder/40-faultpath  # from 30; ONLY fault-around / prefault
```
Rung N's snapshot records `starry_sha = git rev-parse ladder/N0-…`. On acceptance, tag `snap/R<n>-<lever>-<date>`; `manifest.starry_sha` MUST equal the tag commit.

**Isolation branches** for attribution cross‑check: `iso/dvfs`, `iso/placement`, `iso/balancer`, `iso/faultpath` — each branched **directly from `ladder/00-baseline`** with only that one lever, measured into `snapshots/iso/`, to get each lever's SOLO delta (detects interaction / non‑additivity).

Use **git worktrees** (project convention) — one per rung branch — so rungs build/measure without churn. **Rebase discipline:** when the base advances or a lower lever is fixed, rebase the stack upward and RE‑MEASURE affected rungs (new SHA → new `<runid>`; old snapshot retained; `manifest.rebased_from` links lineage). Levers ultimately land upstream as independent Conventional‑Commits PRs (`feat(cpufreq): …`, `feat(axtask): …`); the stack is the integration/measurement vehicle and the snapshots are the evidence attached to each PR.

### 1.7 Attribution controls + regression detection

**Five anti‑miscredit controls** (implemented in `check_ladder.py`):

1. **One lever per rung, enforced by the stack.** A rung's delta vs its parent is attributable to the declared lever *only if* `manifest` asserts build‑config + harness SHAs + `linux_ref` are IDENTICAL to the parent except the one lever. If more than the declared lever differs → attribution `INVALID`.
2. **Lever‑specific gauge must move, or credit is rejected** (headline moving is not enough):
   - **DVFS** → `gauges.max_freq_mhz` rises AND all `percore` pinned `ev` rise proportionally on ALL cores.
   - **Placement** → `gauges.unpinned_landed_type` moves `A55→A76` and unpinned `cpu_t1` jumps to A76‑class.
   - **Balancer** → `gauges.scaling_t8_over_t1` rises above 1.0 and `cpu_t8` climbs toward the aggregate ceiling.
   - **Fault‑path** → `gauges.firsttouch_A76_s` drops and `mem_MiBps.bs1k_*` rises.
   *Headline moved but the gauge didn't ⇒ it was NOT that lever* → flagged `INEFFECTIVE`.
3. **Model cross‑check.** Attribute two ways — (a) empirical rung‑to‑rung delta, (b) `decompose.py` model multiplier (StarryOS ev mapped onto the Linux ev‑vs‑freq curve). They must AGREE within band; disagreement flags a confound (e.g. a DVFS rung that accidentally improved placement).
4. **Isolation matrix.** The `iso/` solo runs give each lever's independent contribution; comparing the solo product to the cumulative stack detects interaction (levers multiply: DVFS scales placement).
5. **Provenance assertion.** `check_ladder.py` diffs each rung's `build-config.toml` + `lever-config.json` vs parent and refuses to credit a delta if more than the declared lever changed.

**`check_ladder.py --assert`** (CI‑style gate, non‑zero exit on failure), run after every capture, compares `latest/metrics.json` against (a) its **parent rung** and (b) its **own previous accepted run** (re‑measure drift):
1. **Monotonicity** — a lever‑adding rung's headline ≥ parent by a threshold AND its specific gauge moved the right direction.
2. **Non‑regression on other gauges** — adding lever X must not degrade a landed lever's gauge beyond the noise band (balancer must not tank single‑thread ev; fault‑path must not tank compute).
3. **Noise band** — N=3 repeats, store median + spread; regression = delta OUTSIDE ±3–5 % ev (wider for memory) AND outside the parent's spread.
4. **Baseline‑drift guard** — warn if `linux_ref` differs from the ladder's canonical ref (parity % incomparable) → prompt explicit re‑baseline.
5. **Provenance assertion** — control 5 above.
6. **Boot sanity** — assert `boot.log` core count matches `manifest.max_cpu_num` (guards "labeled smp8, actually smp1").

---

## Part 2 — Per‑lever benchmark suites

Each suite below is the **reconciled survivor set** after applying both adversarial lenses (feasibility + confound‑isolation). Dropped items and *why* are stated so nobody re‑adds them. Legend: **[PORT]** = port an existing standard benchmark; **[CUSTOM]** = build our own; **[ANCHOR]** = already deployed/board‑proven; **[LINUX‑ONLY]** = reference/cross‑check on Linux, never a StarryOS rung.

> Cross‑cutting rule enforced everywhere: single‑thread + single‑pinned‑core kills levers 2/3; register/L1‑resident + warmup kills lever 4; same binary on the same physical core cancels microarch/compiler/libc; **always `assert sched_getcpu()==req` and ABORT on mismatch** (an A76 rung silently becoming an A55 number is the #1 trap). Core‑type comes from the **requested index** (cpu0–3 = A55, cpu4–7 = A76) mapped through the Linux `HL_CORE` MIDR map — **never** from an on‑device MIDR read (`midr_ok=0`).

### 2.1 Lever 1 — DVFS / cpufreq

**Rung metric:** effective sustained single‑core clock in **MHz per cluster** (one A76 number, one A55 number), recovered by mapping a pinned single‑thread cache‑resident throughput onto the board‑Linux OPP curve. Report three ways: (i) effective MHz; (ii) DVFS fraction = StarryOS/Linux‑at‑max‑OPP (today ≈ 0.36 A76, 0.45 A55); (iii) reciprocal gap (≈ 2.79× A76, 2.24× A55). Current rung ≈ 800 MHz; target ≈ 2256 (A76)/1800 (A55), gap → 1.0×.

| Benchmark | verdict | role |
|---|---|---|
| **sysbench cpu `--cpu-max-prime=20000 --threads=1` pinned** | KEEP | **[ANCHOR]** continuity — the 24–33× baseline is already in its units (`HS_PSB`). |
| **freqprobe** (harden `cpuprobe.c`) | KEEP | **[CUSTOM]** primary IPC‑invariant probe — ~90 % already exists in `cpuprobe.c`. |
| **CoreMark (EEMBC)** | KEEP *after fixes* | **[PORT]** the citable industry standard. |
| **Dhrystone 2.1** | KEEP (optional) | **[PORT]** independent integer cross‑check. |
| **Whetstone** | KEEP (low pri) | **[PORT]** cheap FP sanity only — RK3588 has one clock domain per cluster, so it tracks the integer clock 1:1 and adds little independent DVFS signal. |
| ~~stress‑ng~~ | **DROP** | autotools multi‑lib binary that probes `/proc`+`/sys` and disclaims bogo‑ops; breaks the one‑file deploy. Linux‑side sanity only. |

**freqprobe [CUSTOM] spec** (add as a mode to `cpuprobe.c`, do NOT fork a new file): keep the existing `work()` splitmix64 kernel as the "heavy‑pipeline" variant; add (a) an **IPC=1 dependent‑ADD chain** (each op consumes the previous result) so ops/s ≈ effective_MHz × IPC and reads out near‑MHz; (b) an **FP variant** mirroring Whetstone. Working set = registers only. Reuse the existing `isb;mrs cntvct_el0` window + `"+r"` result barrier (already prevents `-O2` hoisting). Auto‑calibrate to ≥0.35 s (already done). **Add: median of N≥5 repeats.** Emit `iters, sec, ips, cntfrq, landed`. Do NOT present raw ops/s as absolute MHz — always map onto the Linux curve.

**Required fixes before CoreMark/Dhrystone/Whetstone count as rungs** (from the feasibility lens):
1. **Timer‑source contamination is the biggest hazard.** Stock CoreMark/Dhrystone/Whetstone time with `clock()`/`times()`/`CLOCK_PROCESS_CPUTIME_ID`/HZ — unverified on StarryOS; a silently‑zero process‑cputime clock makes a real DVFS deficit vanish or invert while looking plausible. **Force `CNTVCT_EL0` or `CLOCK_MONOTONIC` and always print raw iters + seconds** so a broken clock is visible.
2. **Dhrystone reads run‑count from stdin via `scanf`** — the non‑interactive serial run hits EOF → divide‑by‑zero/bogus exit. **Hardcode the run count.**
3. **Auto‑calibrate work to a target duration** (double iters to ≥0.35 s) instead of a fixed `ITERATIONS` — at 800 MHz a 2.256 GHz‑tuned budget runs ~2.8× longer and blows the serial timeout.
4. **Verify `landed==req` and abort** on every per‑cluster leg.
5. CoreMark: use only the **same‑core same‑binary ABSOLUTE ratio** to read clock; never compare CoreMark/MHz across A55 vs A76.

**Isolation recipe.** Pin to one core (A55 idx 0, A76 idx 4), verify `landed==req`; single thread; register/L1‑resident + warmup; same binary both OSes. On Linux build the curve with `governor=userspace` sweeping `scaling_available_frequencies` (already done by `linux-harness.sh` → `HL_REF`) and **validate the anchor with `scaling_cur_freq`/`cpuinfo_cur_freq` during the run** (throttling shrinks the apparent gap). Feed both logs to `decompose.py`.

**Confound fixes in `decompose.py`** (from the confound lens):
- Map by **local interpolation between bracketing measured OPPs** (600/816/1008), not the global median‑slope zero‑intercept fit — the curve is 2–5 % sub‑linear at the low‑freq end where StarryOS sits. A real 816 MHz sample already brackets the operating point, so this is interpolation, not extrapolation.
- **Report A55 (2.24×, real) and A76 (2.79×, placement‑gated) separately** — the 2.79× A76 number only materializes once placement also moves work to A76, so never quote one blended "DVFS gap".
- **Re‑verify on‑curve co‑location every snapshot** (StarryOS splitmix64/prime must still land within ~2 % of the 816 MHz OPP point) so a future StarryOS compute regression can't masquerade as a DVFS change.

**Intra‑lever sub‑rungs:** `10-dvfs-perf` (fixed performance governor = pin max OPP; most of the win, no scheduler change) → `11-dvfs-opp-sweep` (mirror the Linux OPP loop on StarryOS to prove the StarryOS DVFS curve matches the Linux reference **point‑for‑point**, validating the driver not just the endpoint) → `12-dvfs-ondemand` (dynamic governor).

### 2.2 Lever 2 — big.LITTLE placement

**Rung metric:** placement efficiency @ fixed clock = `unpinned_single_thread_ev / pinned_A76_ev` (0..1), equivalently the big‑cluster residency fraction of one unpinned hot thread. Baseline ≈ 0.45 (lands on A55; residency 0). Target = 1.0. DVFS‑free companion in roadmap units: unpinned single‑thread sysbench `159 → ~351 ev/s @800 MHz` (2.2×) purely from this rung, before any DVFS — **label it "assuming ~800 MHz"** since `pmc_ok=0` means the clock is asserted, not measured.

| Benchmark | verdict | role |
|---|---|---|
| **sysbench cpu single‑thread A/B** (unpinned vs `taskset -c 4` vs `taskset -c 0`) | KEEP | **[ANCHOR/primary]** board‑proven (`HS_PSB` pinned legs exist; add the unpinned leg). |
| **placeprobe** (extend `cpuprobe.c`) | KEEP | **[CUSTOM]** the mechanistic proof — directly measures *where* the thread ran. |
| **schbench** (classic masoncl / srikard ARMv8 fork) | KEEP (secondary) | **[PORT]** latency angle only, not the isolator. |
| ~~rt‑app~~ | **DROP** (Linux ref only) | **[LINUX‑ONLY]** placement readout needs ftrace sched events StarryOS lacks; json‑c/autotools + `SCHED_FIFO`/`mlockall` unverified. Cite as the EAS reference methodology. |
| ~~stress‑ng~~ | **DROP** | fragile startup `/proc`+`/sys` surface; fault behavior can shift between snapshots (breaks comparability); redundant with the sysbench A/B. |
| ~~hackbench~~ | **DROP** (→ balancer) | measures distribution (lever 3), not core‑type. Move to §2.3. |

**placeprobe [CUSTOM] spec** (extend `cpuprobe.c`, reuse `work()`): spawn ONE CPU‑hot worker; run T=5 s; **the worker itself** stores its `sched_getcpu()` into a shared slot every N iterations (do NOT sample from a separate 25 ms‑sleeping monitor thread — a second runnable task perturbs placement, especially on the Linux EAS leg). Accumulate a per‑core residency histogram; record the first A76 (cpu4–7) timestamp. Emit `ips` over the window. Two modes:
- **P1 (default):** no affinity, all 8 cores allowed — observes real default placement (initial core, big‑cluster residency, ips). Uniquely records the **initial pre‑affinity core** — this resolves the spawn‑core confound the sysbench leg alone cannot.
- **P2 (tight):** mask = exactly `{cpu0 (A55), cpu4 (A76)}` (2‑bit `0x11`). One thread, two allowed cores ⇒ zero spreading is possible and the ONLY scheduler decision is core‑type. Report which it parks on + A76 residency + ips vs the pinned‑A76 ceiling. **Confirm StarryOS honors and constrains to a 2‑bit set before trusting the leg** (cpuprobe only ever set single‑bit masks; the balancer work masked 4/8 so multi‑bit appears accepted, but verify).

Deploys as `HS_PLACE` rows from `starry-harness.sh`. Uses only `sched_getcpu` + `sched_setaffinity` + pthreads + CNTVCT — all proven, no `/proc`/`/sys`/ftrace.

**Required fixes** (confound lens):
1. **Control/record the initial spawn core on the unpinned sysbench leg.** On a no‑migration OS, "unpinned lands on A55" can be a *fork artifact*, not a scheduler decision. Launch sysbench pinned to a known core, or record `sched_getcpu` of the sysbench PID. placeprobe already resolves this; the sysbench leg must too.
2. **Label P2 as initial‑placement, not misfit up‑migration latency.** StarryOS never migrates in place, so P2 measures "does the scheduler ever pick A76 when allowed?" — a valid lever readout, but *not* migration latency. Up‑migration latency is mechanism‑shared with lever 3 (Linux implements misfit up‑migration inside the balancer) — lean on P2 static park‑core + residency as the airtight cut; treat up‑mig latency as the soft, shared‑mechanism metric.
3. **Add the cross‑OS microarch sanity check.** Pinned‑A76/pinned‑A55 ev ratio at matched clock (≈ 351/160 = 2.19 @800 MHz on StarryOS) must agree with Linux at the same interpolated frequency (from `HL_REF`). Divergence flags a hidden confound (cache warm‑up, thermal clamp, or an unnoticed real‑frequency difference StarryOS can't self‑measure).
4. On the Linux reference leg, **pin both clusters to a matched interpolated frequency** (their OPP tables differ, so an exact common setpoint may not exist — interpolate both to a common frequency from the `HL_REF` curves). Never compare StarryOS‑default against Linux‑turbo for this rung.

**Intra‑lever sub‑rungs:** `20-placement-affinity` (app pins itself / affinity exposed; gauge = `HS_PSB` A76 reachability) vs `21-placement-heuristic` (kernel auto‑prefers big cores for CPU‑heavy unpinned tasks; gauge = unpinned `HS_PLACE` landed core‑type). Different gauges because one is explicit, one is default policy.

### 2.3 Lever 3 — load balancer / cross‑core distribution

**Rung metric:** load‑balance efficiency `E = agg_ops/s(AUTO: K threads sharing the K‑core cluster mask) / agg_ops/s(MANUAL: K threads each pinned 1‑per‑core on the same cluster)`, measured within ONE homogeneous cluster at fixed 800 MHz. Ideal 1.0; baseline ≈ 1/K (≈ 0.25 at K=4). Secondary diagnostic: `cores_used` = distinct `sched_getcpu` values across AUTO threads (baseline ≈ 1, healthy ≈ K). Equivalent presentation: parallel speedup `S = auto_agg / single_thread` (baseline ~1×, ideal 4× per cluster / ~8× via the process control).

| Benchmark | verdict | role |
|---|---|---|
| **lbscale** (spread‑vs‑pin differential) | KEEP | **[CUSTOM/primary]** the ONLY instrument that cancels all three other levers by construction. |
| **sysbench cpu `--threads=N` scaling, `taskset -c 4-7` masked, reported as ratio S(N)** | KEEP | **[ANCHOR]** zero‑port anchor — but see fixes: NOT the existing unmasked absolute line. |
| ~~schbench~~ | **DROP** (from isolation) | RPS/p99 require all‑CPU saturation → dominated by the missing balancer + wakeup/timer subsystem (a new confounder) + frequency. Instead add an **lbscale `wake` mode** to recover its one unique signal without the contamination. |
| ~~hackbench~~ | **DROP** | wall‑clock is IPC/context‑switch/futex + first‑touch + frequency bound; every other lever leaks in. |
| ~~stress‑ng~~ | **DROP** | won't deploy (autotools) + bogo‑ops non‑comparable + redundant with lbscale. |

**lbscale [CUSTOM] spec** (new one file, reuse `cpuprobe.c`'s `work()` verbatim as the per‑thread hot loop — no heap, no shared memory, no syscalls, no locks in the timed region). CLI: `lbscale <cpu_list> <mode> <seconds>` (`cpu_list` homogeneous, e.g. `4,5,6,7`; `mode` = `auto|pin`). Spawn K pthreads; `pin` → each `sched_setaffinity` to one distinct core; `auto` → every thread sets the same K‑core mask. Driver runs pin@A76(K=4), auto@A76(K=4), pin@A55, auto@A55, computes E. Also a **process‑parallel control**: `fork` K copies each pinned 1‑per‑core (mirrors the measured 2046‑ev/s ceiling) to prove threads‑vs‑processes is the balancer axis, not a threading bug. Emits `HS_LBSCALE mode=… K=… agg_ips=… per_thread_ips=[…] cores_used=… landed=[…]`. Runs bit‑identically on Linux (Linux E≈1.0, StarryOS E≈0.25 today).

**Required fixes — ALL bias E optimistically if unfixed** (confound lens; these are mandatory before first run):
1. **Zero shared memory in the timed region.** A shared stop‑flag is L1‑local in AUTO (all threads on one core) but bounces between cores in MANUAL (spread), depressing the MANUAL denominator and inflating E toward 1.0 (the dangerous direction for an improvement ladder). **Each worker self‑terminates on a precomputed CNTVCT end‑count** and reports its own ips; main just sums. This also fixes the timing‑robustness bug (a main‑thread timer can be starved onto the overloaded AUTO core, delaying the stop signal).
2. **Cache‑line‑pad per‑thread counters** (64 B, written once at the end) — else MANUAL pays false‑sharing that AUTO doesn't, again inflating E.
3. **`pthread_barrier` after spawn + a settle spin** before BOTH the `cores_used` sample and the timed window — auto‑placement settles lazily (cpuprobe spins ~2 M iters for exactly this reason); measuring mid‑migration corrupts E.
4. **A76 (cpus 4–7, IRQ‑free) is the headline rung.** For A55 exclude **cpu0** (StarryOS lands IRQs there) → use `1,2,3` (K=3) or move IRQ affinity off cpu0 first; E@A55 is inherently less trustworthy.
5. **`cores_used` is necessary‑not‑sufficient** — it's the UNION of cores touched, not simultaneity (a scheduler round‑robining one thread across 4 cores sequentially reads `cores_used=4` at true parallelism 1). Present it *with* E, never alone; E (throughput) is ground truth.

**sysbench anchor fix:** the existing `HS_MX cpu t=1/2/4` line runs **unmasked** (lands on the cpu0 A55 → the flat ~160), and reports absolute ev/s — it is **not** a clean lever‑3 rung. Replace with `taskset -c 4-7 sysbench cpu --threads=N` and report the **dimensionless speedup ratio S(N)=ev(N)/ev(1)** only (absolute ev/s becomes frequency‑contaminated the moment DVFS lands). The "8 threads on all cores = 160" number spans A76+A55 and mixes lever‑2 with lever‑3 — never a clean balancer rung.

**Intra‑lever sub‑rungs:** `30-balancer-spread` (initial‑placement spread) vs `31-balancer-steal` (runtime work‑stealing/migration; gauge = `scaling_t8_over_t1`). Note: lbscale's forever‑spinning AUTO threads test *initial* placement; a mode that starts threads unbalanced then waits for a pull is needed to isolate periodic rebalance vs fork/wake placement.

### 2.4 Lever 4 — page‑fault / first‑touch path

**Rung metric:** anonymous minor page‑fault latency, **µs/fault**, single‑thread, pinned to a verified A76 core (cpu4) at ~800 MHz. **Headline = the RAW `firsttouch_ns_per_fault` StarryOS/Linux ratio** (not the first‑minus‑warm subtraction — that is near‑zero and unstable on the fast Linux side, ~1.3 vs ~1.0 µs, and can go negative). Baseline ≈ 12 µs/fault (StarryOS A76, from `0.8028 s / 65536 faults`) vs Linux ≈ 1.3 µs/fault ⇒ ~9–11×. Secondary: page_fault1‑style mmap+touch+munmap faults/s (VMA setup included) for citeable comparison. A55 (cpu0) as a second series.

| Benchmark | verdict | role |
|---|---|---|
| **ftfault** (evolve `membw.c`) | KEEP | **[CUSTOM/primary]** anon minor‑fault probe; `membw.c` already runs this exact path and produces the ~12 µs number, so build risk ≈ 0. |
| **pft** (Christoph Lameter) | KEEP (optional) | **[PORT]** external‑credibility corroborator + source of a **ground‑truth fault counter** (`getrusage.ru_minflt`). |
| **will‑it‑scale page_fault1** | KEEP as loop‑mode | **[PORT‑lite]** lift only the ~30‑line mmap/touch/munmap loop (= ftfault loop‑mode); do NOT build the upstream hwloc+Python runner. The citeable name. |
| ~~lmbench lat_pagefault~~ | **DROP** (Linux ref only) | faults a FILE mapping → measures ext4/page‑cache/block path (a *different*, separately‑broken subsystem), not anon zero‑page first‑touch; also needs lmbench's `lib_timing` scaffold. |
| ~~stress‑ng --mmap/--fault/--page-in~~ | **DROP** as metric | `--fault` = userfaultfd (user‑space handling, different path), `--page-in` = mincore/madvise; bogo‑ops non‑citeable; heavy ENOSYS surface. Keep ONLY as a non‑metric VM‑syscall‑availability probe + soak. |

**ftfault [CUSTOM] spec** (evolve `membw.c`; keep the CNTVCT timing + affinity+`landed` guard). Use **`mmap`, not `malloc`** (glibc arenas may already be faulted): `mmap(NULL,N,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0)`. `pagesize=sysconf(_SC_PAGESIZE)`, `nfaults=N/pagesize` (printed, so silent THP is visible). Steps: (1) affinity to `[core]`, settle, `assert sched_getcpu()==core`, print core‑type from the **index** (not MIDR). (2) FIRST TOUCH: one byte per page‑aligned offset, CNTVCT‑timed → `firsttouch_ns_per_fault`. (3) WARM RE‑TOUCH → `warm_ns_per_page`. (4) `fault_overhead_ns = firsttouch − warm` (StarryOS‑side attribution only). Report `firsttouch_ns_per_fault, warm_ns_per_page, fault_overhead_ns, faults_per_s`. **Median over K≥5** (not best‑of — best‑of is a bandwidth idiom that biases a latency number low). Modes to localize the bottleneck: (b) **loop** `{mmap; touch; munmap}×K` → page_fault1‑comparable faults/s; (c) **populate** `MAP_POPULATE` vs lazy; (d) **dontneed** `madvise(MADV_DONTNEED)` then re‑fault the SAME VMA (isolates the handler from VMA setup).

**Required fixes** (both lenses):
1. **Identical region size AND fault count on BOTH OSes.** `membw` currently uses mb=128 on StarryOS but 256 on Linux and memsets two buffers — fix a single canonical size (e.g. 256 MB, 65536 faults) so the rung is comparable across snapshots.
2. **Force 4K granularity** (`MADV_NOHUGEPAGE` / `transparent_hugepage=never` on Linux; StarryOS has no THP) so both do 4K minor faults.
3. **Ground‑truth fault counter** (`getrusage(RUSAGE_SELF).ru_minflt`, fallback `/proc/self/stat` minflt) on BOTH sides — `sysconf` `nfaults` is the *assumed* count; a silent THP/large‑page backing changes the real count while the printed value stays, silently mis‑scaling per‑fault cost. This is exactly what pft provides. If neither counter works on StarryOS, **state explicitly that the 4K‑per‑fault attribution is an unverified assumption.**
4. **populate/dontneed must self‑VALIDATE the EFFECT, not the errno.** `MAP_POPULATE`/`MADV_DONTNEED` can return success while doing nothing on StarryOS: a no‑op `MADV_DONTNEED` makes the "re‑fault" a warm touch (false "fault path is fast"); a no‑op `MAP_POPULATE` mislabels lazy faulting as "populate". Require the timing/fault‑count to actually move (post‑populate first touch must be warm‑fast; post‑dontneed re‑touch must be first‑touch‑slow) or **fall back to the guaranteed‑supported `munmap`+`mmap` loop.**
5. **Pin Linux to a REAL OPP** — `echo 800000 > scaling_setspeed` is not an RK3588 OPP (steps are 408/600/816/1008…; the boot OPP is ~816). Pick from `scaling_available_frequencies` and verify `cpuinfo_cur_freq`.
6. **Note the out‑of‑model 5th confound: DDR/DMC DVFS.** Servicing an anon fault zeroes a fresh frame (a DDR write) that the warm re‑touch does NOT re‑zero, so the subtraction can't cancel it. Warm memcpy parity (7.3 GB/s) suggests it's small, but **record/verify dmc freq**, don't assume.
7. The first‑minus‑warm subtraction cancels the store/cache‑fill (bandwidth) component but **NOT** the handler's own clock dependence — so equal‑OPP matching is mandatory; don't skip it on the strength of the subtraction.

**Intra‑lever sub‑rungs:** `40-faultpath-prefault` (`MAP_POPULATE`/fault‑around) vs `41-faultpath-thp` (huge pages). The N‑thread scaling variant of any fault bench is **dominated by levers 2/3** (no A76 placement, no balancer) — attribute it to lever 3, never present it as a StarryOS "page‑fault scalability" property.

---

## Part 3 — The implement → measure → snapshot → compare loop

Per lever (and per intra‑lever sub‑rung):

1. **Branch.** `git worktree add` on `ladder/N0-<lever>` (from the parent rung) — see §1.6. One lever only.
2. **Implement** the lever (kernel change) OR the benchmark (harness change on `ladder/00-baseline`, then rebased up).
3. **Build instruments.** `bash harness/build-harness.sh` (arm64 ubuntu:22.04 container → glibc‑dynamic single files). Record each binary's SHA256 into the pending manifest.
4. **Refresh the Linux ref if needed.** If none pinned for this ladder, capture once: `ssh … 'bash -s' < linux-harness.sh` → `refs/linux/<id>/`. Otherwise reuse the pinned ref (parity % stays comparable).
5. **Deploy + capture** via the one‑command wrapper:
   `capture_snapshot.sh <rung-id>` which: reads `git rev-parse`, copies `build-…toml`/`uboot-…toml` into the run dir, runs `deploy-harness.sh`, boots StarryOS `env -u RUSTUP_TOOLCHAIN cargo xtask starry uboot -c build-…toml --uboot-config uboot-…toml`, **tees `starry-harness.out` + `boot.log`** to a fresh `rungs/<NN>/<runid>/`, repeats **N=3** (idle‑thermal gaps between; require idle thermal before capture; log ambient/retries to `notes.md`), runs `decompose.py --json`, extracts `metrics.json` (median+spread), writes `manifest.json` + `lever-config.json`.
6. **Gate.** `check_ladder.py --assert <rung-id>` (§1.7). If it fails (monotonicity, gauge direction, provenance, boot core count, noise band) → do NOT accept; investigate.
7. **Accept.** Repoint `latest -> <runid>`; tag `snap/R<n>-<lever>-<date>`.
8. **Attribution cross‑check.** For the lever, also measure `iso/<lever>` (branched from baseline) into `snapshots/iso/` and confirm empirical Δ ≈ decompose model multiplier ≈ solo iso delta.
9. **Render.** `render_ladder.py` → `ladder.{md,html,csv,json}` (waterfall + two tracks + parity gauges; projected bars fill in as levers land).
10. **Rebase discipline.** When the base advances or a lower lever is fixed, rebase the stack, RE‑MEASURE affected rungs (new `<runid>`, `manifest.rebased_from` set), re‑gate, re‑render.

**Determinism aids baked into the snapshot:** verbatim `build/uboot-config.toml`, harness SHA256s, the pinned build recipe (arm64 ubuntu:22.04, glibc‑dynamic, `PRIME=20000`, `--time` windows baked into the harness SHA), `linux_ref` id, `nightly-2026-04-27`. `--dry-run` schema validator + `decompose.py --selftest` exercise the whole pipeline with no board.

---

## Part 4 — The concrete first step

**Build `lbscale` first** (the balancer spread‑vs‑pin microbench), concurrently with the snapshot‑tooling scaffold. Rationale:

- **It fills the one real measurement GAP.** DVFS, placement, and fault‑path each already have a working measurement path in the current harness (DVFS via `cpuprobe.ips` + the Linux `HL_REF` curve; placement via `HS_PSB` pinned legs; fault via `membw firsttouch`). The **balancer is the only lever whose current harness measurement is confounded/broken** — `HS_MX cpu t=1/2/4` runs *unmasked* (lands on the cpu0 A55, reports the flat ~160 absolute ev/s), which both adversarial lenses reject as a non‑rung.
- **It is the #1 measured bottleneck and the money‑slide number.** `RESULTS.md` finding #2: smp8 == smp4 exactly because nothing spreads; the ceiling is 12.9× (2046 vs 159). The waterfall's largest step needs a clean isolator.
- **Lowest build risk of the four customs.** It reuses `cpuprobe.c`'s `work()` kernel verbatim and only proven syscalls (pthreads, `sched_setaffinity`, `sched_getcpu`, `CNTVCT`, `fork`); one glibc‑dynamic file via the existing `build-harness.sh` path.
- **Its rung metric is confound‑clean by construction** — the AUTO/MANUAL ratio `E` cancels DVFS (dimensionless, both at 800 MHz), big.LITTLE (one homogeneous cluster in numerator and denominator), and faults (register‑only timed loop) — and `cores_used` is an undeniable diagnostic needing no Linux curve.

Ship it with all §2.3 hardening from day one (self‑timed CNTVCT end‑count, cache‑line‑padded counters, barrier+settle, A76 headline / A55 excludes cpu0, `cores_used` reported only alongside E).

**What to add to existing files vs build new:**

| File | Action |
|---|---|
| `starry-harness.sh` | **ADD** an unpinned single‑thread sysbench leg (placement companion); **ADD** `taskset -c 4-7 sysbench --threads=N` masked scaling legs reported as ratio S(N) (balancer anchor); **ADD** `HS_LBSCALE`/`HS_PLACE`/`HS_FTFAULT` invocation rows; **harmonize** `--time` (HS_PSB=3 s vs HS_MX=5 s today → pick one). |
| `cpuprobe.c` | **ADD** the `placeprobe` P1/P2 residency modes and (lower priority) the freqprobe IPC=1 add‑chain + FP variant + median‑of‑N. No new syscalls. |
| `membw.c` | **EVOLVE** into `ftfault`: mmap‑not‑malloc, single canonical region size on both OSes, 4K forcing, first‑minus‑warm, ground‑truth fault counter, self‑validating populate/dontneed. |
| `decompose.py` | **ADD** `--json` mode (→ `metrics.json`/`decompose.json`), local OPP interpolation (replace `curve_k()` median‑slope), per‑cluster DVFS split, and lever‑multiplier emission. |
| **NEW** | `lbscale.c` (build first); `placeprobe`/`ftfault` (as above); tooling: `capture_snapshot.sh`, `check_ladder.py`, `render_ladder.py`, and a tiny fork+signal‑guarded VM‑syscall availability probe (replaces stress‑ng's "which syscalls exist" role). |

---

## Appendix — measured reference numbers (baseline rung `dbbe0e065`, 2026‑07‑16)

Grounding for the projected bars and the gauge thresholds (all from the frozen `starry-harness.clean.out` / `linux-harness.clean.out`):

- **Per‑core sysbench @800 MHz:** StarryOS A55 (cpu0–3) 155–161, A76 (cpu4–7) 350.8–351.3; Linux @816 MHz A55 160.91, A76 346.86 → **per‑core parity confirmed.**
- **cpuprobe ips @800 MHz:** StarryOS A55 ~58.3 M, A76 ~77.7 M; Linux @816 A55 58.04 M, A76 76.43 M.
- **Linux curves:** A55 ~0.199 ev/s/MHz (77.75@408 → 359.24@1800); A76 ~0.434 ev/s/MHz (170.73@408 → 978.74@2256).
- **Linux matrix:** cpu t1 977.61, t2 1955.61, t4 3892.11, **t8 5320.20**; thr t4 49492; mutex t8 0.463 s; mem 8039.9 MiB/s.
- **StarryOS matrix:** cpu t1/2/4 = 159.4/159.9/160.5 (flat = balancer gap); thr t4 1076; mutex t4 7.44 s; mem@1M 2925.8 MiB/s, mem@1K 42.8 MiB/s.
- **First‑touch:** StarryOS A76 0.8028 s/128 MB (~12.25 µs/fault), A55 1.295 s/128 MB; Linux A76 0.1733 s/256 MB (~1.32 µs/fault), A55 0.3006 s/256 MB. (Size mismatch is the `membw` comparability defect ftfault fixes.)
- **Derived gap decomposition:** `159 (A55@~800) ×2.25 (DVFS) ×2.72 (A55→A76 @max) ×4.0 (1→4 cores) ≈ 24×`; full‑board ceiling @800 MHz = 2046 (12.9×).
