# Ablation board campaign — running log

## Session 2026-08-12

### Setup done (no board needed)
- All 24-lever variants **build-confirmed COMPILE** (feature-flips + `abl/*` revert branches). See ABLATION.md run-matrix.
- Static aarch64 benchmark binaries ready (non-PIE, run on board-Linux AND StarryOS):
  `sysbench-static-aarch64` (musl, non-PIE — took 4 tries: `-all-static` at make-time + luajit-first + `-fno-pie`),
  plus `hackbench schbench syscost mem_bw2 bar2 thp_narrow`. Deployed to board `/home/orangepi/abl`.
- On-board harness `abl-bench.sh` (profiles: cpu, p1, threads, hb, sch, getpid, thp, membw, all).

### Board wiring (this rig)
- Board Linux `orangepi@169.254.50.2` (enP3p49s0, carrier), host bind en5 `169.254.99.39`, key `id_ed25519`.
- Console: direct pyserial `/dev/cu.usbserial-AQ03MLX2 @ 1.5M` (ostool-server DOWN). boardctl at MAIN repo path.
- StarryOS driven by `scripts/board/sdboot-run.py --config <toml> --uboot-config uboot-abl-<prof>.toml`.

### LINUX baseline column — CAPTURED ✓ (`results/ablation/linux/all.txt`)
| probe | Linux |
|---|---|
| sysbench cpu A55(cpu0) 1T | 344.6 ev/s |
| sysbench cpu A76(cpu4) 1T | 964.1 ev/s |
| sysbench cpu 8T | 5227.3 ev/s |
| sysbench mutex 8T | 0.483 s |
| sysbench memory 8T | 56236 MiB/s |
| hackbench -P g2/g5/g10 (**-p pipe mode**) | 0.023 / 0.038 / 0.070 s |
| hackbench -T g2/g5/g10 | 0.029 / 0.043 / 0.079 s |
| schbench m1t4 RPS | ~198 |
| getpid | 175.1 ns ; pipe_wr 589.4 ns |
| mem_bw2 1/2/4/8T | 34.1 / 67.9 / 139.2 / 189.0 GB/s |

### SHIP (full placement config) StarryOS — PARTIAL (2 boots, both cut short)
| probe | StarryOS ship | vs Linux |
|---|---|---|
| sysbench cpu A55 1T | **361.5 ev/s** | **1.05× (StarryOS FASTER)** |
| sysbench cpu A76 1T | 888–900 ev/s | ~0.93× |
| sysbench cpu 8T | 4994–5008 ev/s | ~0.96× |
| sysbench mutex 8T | 0.85 s | ~0.57× (1.75× slower) |
| sysbench memory 8T | 27051 MiB/s | ~0.48× |
| (sch/getpid/thp/membw/hb) | not yet captured for ship | |
- **DDR-DVFS no-op CONFIRMED from the boot log**: `ddr-dvfs: SIP DRAM DVFS not supported (GET_VERSION status=-1); leaving DDR at boot rate` — validates the ABLATION §5 flat-ceiling-row prediction directly.

### ⚠ OPERATIONAL FINDING — board hard-hang (needs power-cycle)
- `sysbench threads --threads=8 --thread-yields=1000` **HARD-HANGS StarryOS** on smp8 (#59 extreme
  wake-storm): all 8 cores deadlock, **console AND network go dead**, Ctrl-C does not recover, no remote
  reset on this rig → requires a physical power-cycle. It completed once (43580 ev, run 1) then hung (run 2)
  = flaky/catastrophic.
- **FIX applied to `abl-bench.sh`**: removed the aggressive threads stressor from `all`/`p1`; `threads` is now
  an opt-in profile with gentle params (`--thread-yields=100 --time=5`). Redeploy the harness after the board
  is back, then the campaign is safe to resume.
- **Harness hardening already in place**: hackbench uses `-p` (socketpair default hangs StarryOS); per-probe
  capture is file-based (long pipes hit EINTR on StarryOS); `hb` runs last so a hang can't lose other probes.

### Resume plan (after power-cycle → board autoboots Linux)
1. Redeploy hardened `abl-bench.sh`; re-prime ARP (board→host ping over serial), ssh check.
2. Finish SHIP: run `all` (now safe) → sch/getpid/thp/membw/hb + re-confirm cpu/mutex/memory.
3. Table 1 ladder (5 boots): `abl/p1floor`(floor) → `abl-p1-l1` → `abl-p1-l1l3` → `abl-p1-l1l3l4` → ship, profile `cpu`.
4. Orthogonal + micro per pillar (feature-flip tomls + `abl/*` revert branches), each a focused profile boot.
