#!/bin/sh
sh /home/orangepi/sched-bench.sh 2>&1 | tee /home/orangepi/schedbench-results.txt
chmod 666 /home/orangepi/schedbench-results.txt
sync; sync
echo FULL_RUN_DONE
sleep 3; sync; reboot
