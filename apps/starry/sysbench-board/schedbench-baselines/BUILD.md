# hackbench + schbench (static aarch64, run on StarryOS + Linux)

Cross-compiled on the Mac with `aarch64-linux-musl-gcc` (homebrew), `-static` so the
same binary runs on StarryOS (musl-ish) and the board's Linux:

    # schbench (Chris Mason, RPS + wakeup-latency percentiles)
    curl -sSLO https://raw.githubusercontent.com/masoncl/schbench/master/schbench.c
    aarch64-linux-musl-gcc -O2 -static -o schbench schbench.c -lpthread

    # hackbench (mingo/rt-tests standalone; getopt args: -p pipe, -g groups, -P/-T)
    curl -sSLo hackbench.c https://raw.githubusercontent.com/jlelli/rt-tests/master/src/hackbench/hackbench.c
    aarch64-linux-musl-gcc -O2 -static -o hackbench hackbench.c -lpthread

Deploy both + `sched-bench.sh` to /home/orangepi/ and run via `run-schedbench.sh`
(StarryOS) or directly over ssh (Linux baseline).
