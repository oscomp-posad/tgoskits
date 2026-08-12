#!/bin/sh
# ABLATION on-board benchmark harness. Runs the same on board-Linux and StarryOS.
# Usage: abl-bench.sh <profile> [tag]      env: REPS (default 3)
# Each probe runs REPS times (median taken offline) to average out noise. Writes to
# $OUT/<profile>.txt; prints a final sentinel (>>>ABLDONE<<<) for the serial reader.
# Numbers are read back from the file over ssh after reboot (serial is lossy). POSIX sh.
#
# Robustness (learned the hard way):
#   * per-probe capture to a TEMP FILE then grep — long "tool|grep|sed|tee" pipes hit
#     EINTR on StarryOS (sed: Interrupted system call; reads aren't SA_RESTART-retried).
#   * hackbench MUST use -p (pipe mode); socketpair default HANGS StarryOS.
#   * sysbench threads --thread-yields=1000 HARD-HANGS StarryOS (#59) -> opt-in gentle only.
#   * hackbench runs LAST so a hang can't lose other probes.
set -u
BIN=${BIN:-/home/orangepi/abl}
OUT=${OUT:-/home/orangepi/abl/out}
REPS=${REPS:-3}
PROF=${1:-all}
TAG=${2:-run}
T=/tmp/abltmp.$$
mkdir -p "$OUT" 2>/dev/null
RES="$OUT/$PROF.txt"
: > "$RES"
say(){ echo "$*" >> "$RES"; echo "$*"; }
SB="$BIN/sysbench-static-aarch64"
pin(){ c=$1; shift; if command -v taskset >/dev/null 2>&1; then taskset -c "$c" "$@"; else "$@"; fi; }

say "### ABL profile=$PROF tag=$TAG reps=$REPS $(uname -sr 2>/dev/null)"

run_cpu(){
  [ -x "$SB" ] || { say "SKIP cpu"; return; }
  i=1; while [ $i -le $REPS ]; do
    pin 0 "$SB" cpu --cpu-max-prime=20000 --threads=1 --time=5 run >"$T" 2>/dev/null
    say "A55_1T r$i $(grep -i 'events per second' "$T")"; i=$((i+1)); done
  i=1; while [ $i -le $REPS ]; do
    pin 4 "$SB" cpu --cpu-max-prime=20000 --threads=1 --time=5 run >"$T" 2>/dev/null
    say "A76_1T r$i $(grep -i 'events per second' "$T")"; i=$((i+1)); done
  i=1; while [ $i -le $REPS ]; do
    "$SB" cpu --cpu-max-prime=20000 --threads=8 --time=5 run >"$T" 2>/dev/null
    say "ALL_8T r$i $(grep -i 'events per second' "$T")"; i=$((i+1)); done
}
run_p1extra(){
  [ -x "$SB" ] || { say "SKIP p1extra"; return; }
  i=1; while [ $i -le $REPS ]; do
    "$SB" mutex --threads=8 --mutex-num=4096 --mutex-locks=50000 --mutex-loops=10000 run >"$T" 2>/dev/null
    say "MUTEX8 r$i $(grep -i 'total time' "$T")"; i=$((i+1)); done
  i=1; while [ $i -le $REPS ]; do
    "$SB" memory --threads=8 --memory-block-size=1M --memory-total-size=20G run >"$T" 2>/dev/null
    say "MEM8 r$i $(grep -iE 'transferred' "$T")"; i=$((i+1)); done
}
run_threads(){  # OPT-IN, GENTLE. --thread-yields=1000 HARD-HANGS StarryOS (#59).
  [ -x "$SB" ] || { say "SKIP threads"; return; }
  i=1; while [ $i -le $REPS ]; do
    "$SB" threads --threads=8 --thread-yields=100 --thread-locks=8 --time=5 run >"$T" 2>/dev/null
    say "THREADS8 r$i $(grep -i 'total number of events' "$T")"; i=$((i+1)); done
}
run_sch(){
  [ -x "$BIN/schbench" ] || { say "SKIP schbench"; return; }
  i=1; while [ $i -le $REPS ]; do
    "$BIN/schbench" -m 1 -t 4 -r 10 >"$T" 2>&1
    say "SCH_m1t4 r$i rps=$(grep -i 'average rps' "$T" | grep -oE '[0-9.]+' | head -1) p50wake=$(awk '/Wakeup Latencies/{f=1} f&&/50.0th/{print $2; exit}' "$T")"
    i=$((i+1)); done
}
run_getpid(){
  [ -x "$BIN/syscost" ] || { say "SKIP syscost"; return; }
  i=1; while [ $i -le $REPS ]; do
    "$BIN/syscost" >"$T" 2>&1
    say "SYSCOST r$i $(grep -i 'getpid' "$T" | head -1)"; i=$((i+1)); done
}
run_thp(){
  [ -x "$BIN/thp_narrow" ] || { say "SKIP thp_narrow"; return; }
  i=1; while [ $i -le $REPS ]; do
    "$BIN/thp_narrow" >"$T" 2>&1
    grep -iE 'PHASE|GB/s|MB/s|first' "$T" | sed "s/^/THP r$i /" >> "$RES"; i=$((i+1)); done
  echo "thp done"
}
run_thpbw(){  # THP first-touch + TLB-reach bandwidth (256 MiB region)
  [ -x "$BIN/thp_bw" ] || { say "SKIP thp_bw"; return; }
  "$BIN/thp_bw" 256 "$REPS" >"$T" 2>&1
  grep -i thpbw "$T" >> "$RES"; grep -i thpbw "$T"
}
run_membw(){
  [ -x "$BIN/mem_bw2" ] || { say "SKIP membw"; return; }
  i=1; while [ $i -le $REPS ]; do
    "$BIN/mem_bw2" >"$T" 2>&1
    grep -iE 'aggregate' "$T" | sed "s/^/MEMBW2 r$i /" >> "$RES"; i=$((i+1)); done
  [ -x "$BIN/bar2" ] && { "$BIN/bar2" >"$T" 2>&1; grep -iE 'cpu' "$T" | sed 's/^/BAR2 /' >> "$RES"; }
  echo "membw done"
}
run_hb(){  # LAST — pipe mode (-p); socketpair default hangs StarryOS
  [ -x "$BIN/hackbench" ] || { say "SKIP hackbench"; return; }
  for m in "-P" "-T"; do for g in 2 5 10; do
    i=1; while [ $i -le $REPS ]; do
      "$BIN/hackbench" -p -g $g $m >"$T" 2>&1
      say "HB $m g$g r$i : $(grep -i 'Time:' "$T" | head -1)"; i=$((i+1)); done
  done; done
}
run_hbT(){  # focused -T (CLONE_VM) high-rep — the user-access-fastpath / IPC story
  [ -x "$BIN/hackbench" ] || { say "SKIP hackbench"; return; }
  for g in 5 10; do i=1; while [ $i -le $REPS ]; do
    "$BIN/hackbench" -p -g $g -T >"$T" 2>&1
    say "HB -T g$g r$i : $(grep -i 'Time:' "$T" | head -1)"; i=$((i+1)); done; done
}
run_freq(){  # direct effective-frequency readout per cluster (cpu0=A55, cpu4=A76)
  [ -x "$BIN/cpuprobe" ] || { say "SKIP cpuprobe"; return; }
  i=1; while [ $i -le $REPS ]; do
    say "FREQ_A55 r$i $(pin 0 "$BIN/cpuprobe" 2>/dev/null)"; i=$((i+1)); done
  i=1; while [ $i -le $REPS ]; do
    say "FREQ_A76 r$i $(pin 4 "$BIN/cpuprobe" 2>/dev/null)"; i=$((i+1)); done
}
run_hbP(){  # focused -P (fork) high-rep — the COW-refcount / IPC story
  [ -x "$BIN/hackbench" ] || { say "SKIP hackbench"; return; }
  for g in 5 10; do i=1; while [ $i -le $REPS ]; do
    "$BIN/hackbench" -p -g $g -P >"$T" 2>&1
    say "HB -P g$g r$i : $(grep -i 'Time:' "$T" | head -1)"; i=$((i+1)); done; done
}

case "$PROF" in
  cpu) run_cpu;;
  freq) run_freq;;
  cpufreqoff) run_cpu; run_freq;;
  p1) run_cpu; run_p1extra;;
  threads) run_threads;;
  hb) run_hb;;
  hbT) run_hbT;;
  hbP) run_hbP;;
  sch) run_sch;;
  getpid) run_getpid;;
  thp) run_thp;;
  thpbw) run_thpbw;;
  membw) run_membw;;
  all) run_cpu; run_p1extra; run_sch; run_getpid; run_thp; run_membw; run_hb;;
  *) say "unknown profile $PROF";;
esac
rm -f "$T" 2>/dev/null
say "### ABL_DONE profile=$PROF tag=$TAG >>>ABLDONE<<<"
