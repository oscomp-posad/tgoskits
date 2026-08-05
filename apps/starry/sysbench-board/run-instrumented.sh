#!/bin/sh
sh /home/orangepi/instrumented-matrix.sh 2>&1 | tee /home/orangepi/instrumented-results.txt
chmod 666 /home/orangepi/instrumented-results.txt
sync; sync
echo FULL_RUN_DONE
sleep 3; sync; reboot
