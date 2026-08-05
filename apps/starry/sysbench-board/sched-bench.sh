#!/bin/sh
HB=/home/orangepi/hackbench
SB=/home/orangepi/schbench
echo SCHEDBENCH_BEGIN
echo "SBB_UNAME $(uname -sr) $(uname -m)"
echo "=== HACKBENCH (Time in s, LOWER=better) ==="
for mode in -P -T; do
  for g in 2 5 10; do
    echo "### hackbench -p -g $g $mode"
    timeout 120 $HB -p -g $g $mode 2>&1 || echo "TIMEOUT/err"
  done
done
echo "=== SCHBENCH (latency us lower / RPS higher; 30s cap) ==="
for cfg in "-m 1 -t 4" "-m 2 -t 8"; do
  echo "### schbench $cfg -r 5"
  timeout 30 $SB $cfg -r 5 2>&1 || echo "TIMEOUT/err"
done
echo SCHEDBENCH_DONE
