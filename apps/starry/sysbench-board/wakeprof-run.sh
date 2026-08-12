#!/bin/sh
# Cross-core wake-latency profiling run. The kernel (wakeprof feature, wake_affine
# OFF) exposes /proc/wakeprof: wake-to-run latency split local vs cross-core, plus
# the deferred on_cpu-handshake count. For each workload: zero the counters, run it,
# then dump the profile. Output tee'd to a file that survives the reboot to Linux.
SB=/home/orangepi/schbench
HB=/home/orangepi/hackbench
dump() { echo "--- /proc/wakeprof after $1 ---"; cat /proc/wakeprof; }

echo WAKEPROF_BEGIN
echo "WP_UNAME $(uname -sr) $(uname -m)"

echo "=== schbench -m 1 -t 4 (1:1-ish, should be mostly local) ==="
cat /proc/wakeprof_reset >/dev/null 2>&1
$SB -m 1 -t 4 -r 5 2>&1 | grep -aE '50.0th|average rps' | head -4
dump "schbench_m1t4"

echo "=== schbench -m 2 -t 8 (more threads -> more cross-core) ==="
cat /proc/wakeprof_reset >/dev/null 2>&1
$SB -m 2 -t 8 -r 5 2>&1 | grep -aE '50.0th|average rps' | head -4
dump "schbench_m2t8"

echo "=== hackbench -p -g 5 -P (fan-out, heavy cross-core) ==="
cat /proc/wakeprof_reset >/dev/null 2>&1
$HB -p -g 5 -P 2>&1 | grep -aE 'Time:'
dump "hackbench_g5P"

echo WAKEPROF_DONE
