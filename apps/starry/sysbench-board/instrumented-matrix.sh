#!/bin/sh
# Instrumented sysbench matrix. Writes EVERYTHING to the results file (no reliance on
# the lossy serial console). Around each CPU thread-count it snapshots per-CPU
# residency from /proc/stat (now backed by the real per-CPU BUSY_TICKS counter), so
# the t=8 collapse can be root-caused offline: a large per-CPU busy delta on only
# ~2 cores => thread clustering; large deltas on all 8 cores with low throughput =>
# lock/serialization contention. Parsed offline (grep/cut), no on-device awk.
SB=/usr/bin/sysbench
MB=/usr/local/bin/membw
echo IM_BEGIN
echo "IM_UNAME $(uname -r) $(uname -m)"

# Per-CPU residency snapshot: emits the /proc/stat cpuN lines (field 4 = busy ticks).
pcpu() { grep '^cpu[0-9]' /proc/stat; }

# Per-core pinned sysbench (correctness reference: A55 ~370, A76 ~910).
c=0
while [ "$c" -lt 8 ]; do
  ev=$(taskset -c $c $SB cpu --cpu-max-prime=20000 --threads=1 --time=6 run 2>/dev/null | grep 'events per second')
  echo "IM_PSB c=$c $ev"
  c=$((c + 1))
done

# membw first-touch (THP gauge) on an A55 and an A76 core.
if [ -x "$MB" ]; then
  echo "IM_PM c0 $($MB 0 128 2>/dev/null | tr '\n' ' ')"
  echo "IM_PM c4 $($MB 4 128 2>/dev/null | tr '\n' ' ')"
fi

# CPU thread ladder WITH per-CPU residency snapshots bracketing each run.
for t in 1 2 4 8; do
  echo "IM_STAT_BEFORE t=$t"; pcpu
  ev=$($SB cpu --cpu-max-prime=20000 --threads=$t --time=10 run 2>&1 | grep -E 'events per second|Segmentation')
  echo "IM_CPU t=$t $ev"
  echo "IM_STAT_AFTER t=$t"; pcpu
done

# Scheduler-heavy + mutex at 8 threads, also residency-bracketed (does a wake/yield
# storm spread or serialize?).
echo "IM_STAT_BEFORE thr8"; pcpu
echo "IM_THR8 $($SB threads --threads=8 --thread-yields=1000 --thread-locks=8 --time=10 run 2>&1 | grep -E 'total number of events|Segmentation')"
echo "IM_STAT_AFTER thr8"; pcpu
echo "IM_MUTEX8 $($SB mutex --threads=8 --mutex-num=4096 --mutex-locks=50000 run 2>&1 | grep -E 'total time|Segmentation')"

# Memory at 8 threads, residency-bracketed (does the memory workload spread?).
echo "IM_STAT_BEFORE mem8"; pcpu
for bs in 1M 1K; do
  for op in write read; do
    echo "IM_MEM8 bs=$bs op=$op $($SB memory --threads=8 --memory-block-size=$bs --memory-oper=$op --memory-total-size=8G run 2>&1 | grep -E 'transferred|Segmentation')"
  done
done
echo "IM_STAT_AFTER mem8"; pcpu
echo IM_DONE
