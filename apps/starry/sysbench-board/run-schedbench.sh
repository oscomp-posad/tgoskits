#!/bin/sh
sh /home/orangepi/sched-bench.sh 2>&1 | tee /home/orangepi/schedbench-results.txt
chmod 666 /home/orangepi/schedbench-results.txt
sync; sync
echo FULL_RUN_DONE
# `reboot` (busybox, no init) is a NO-OP in StarryOS on this board and strands it
# at the shell (en5 goes down, board unreachable). `reboot -f` actually resets the
# SoC -> U-Boot autoboots SD Linux. The sdboot-run.py driver also sends `reboot -f`
# as a backstop after the sentinel.
sleep 3; sync; reboot -f
