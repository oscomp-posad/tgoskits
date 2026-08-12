#!/bin/sh
sh /home/orangepi/wakeprof-run.sh 2>&1 | tee /home/orangepi/wakeprof-results.txt
chmod 666 /home/orangepi/wakeprof-results.txt
sync; sync
echo FULL_RUN_DONE
sleep 3; sync; reboot -f
