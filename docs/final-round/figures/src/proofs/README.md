# Proof panels (真机证据图)

Dark terminal-panel figures built from **real captures**, used on the deck's
evidence slides. Run each from the repo root; all write into `figures/out/`.

| Script | Output | Source of the data |
|--------|--------|--------------------|
| `gen_perf_proof.py` | `Fperf_proof.png` | Board perf captures (perf stat/record/report/top, kprobe, ftrace) + the 39/0 big.LITTLE PMU and 56/0 feature matrices |
| `gen_pg_proof.py` | `Fpostgres_proof.png` | PostgreSQL-17-on-StarryOS run: apk add → initdb → 14-stage SQL workload → `POSTGRESQL_TEST_PASSED` |
| `gen_perf_flame.py` | `Fperf_flame.png` | `flame_perf.svg` — real `perf script \| flamegraph.pl` output (6735 samples) of the tennis inference; renders + smart-crops to the flame-bars band |

```sh
python3 figures/src/proofs/gen_perf_proof.py
python3 figures/src/proofs/gen_pg_proof.py
python3 figures/src/proofs/gen_perf_flame.py   # needs macOS qlmanage + Pillow
```

`flame_perf.svg` is vendored here so the flame strip is self-contained
(original lives at `tgoskits/scripts/profile/results/svg/flame_perf.svg`).
The two matplotlib scripts use the Menlo + Songti SC fonts (macOS paths).
