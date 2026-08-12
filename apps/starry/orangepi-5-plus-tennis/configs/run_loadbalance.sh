#!/bin/sh
# Scheduler REINVESTIGATION (sched-loadbalance kernel feature ON; xHCI IRQ stays
# on cpu0 = USB_IRQ_TARGET_CPU=None). Two questions in one boot:
#   1. THROUGHPUT: does the work-stealing + big.LITTLE-aware balancer lift
#      processed fps by spreading capture/decode/infer across the A76 cluster,
#      vs the ~14fps single-core-supply ceiling?
#   2. DEADLOCK: does the balancer trigger the cross-core on_cpu spin-under-lock
#      hazard (run_queue.rs:633) even WITHOUT IRQ routing? With NO manual affinity
#      every task carries an all-cores cpumask, so `select_wake_run_queue` reroutes
#      wakeups to the least-loaded core -> cross-core wakes become routine, the
#      suspected mutual-spin trigger. A freeze here (total serial silence) confirms
#      the scheduler-wake path is the culprit (the only new variable vs the working
#      cpu0-default baseline is the balancer's cross-core wake routing).
# NO --infer-affinity / NO UVC_CAPTURE_AFFINITY: the balancer does all placement.
# Run 3x to probe the probabilistic hang and get repeatable fps. (<=3 UNPINNED
# launches/boot stays clear of the cross-process USB-reuse hang, which only bit
# after an A76-PINNED run; a freeze with no app output + both cores silent is the
# scheduler deadlock, not the USB-reuse hang.)
cd /tennis_app || { echo "MISSING /tennis_app"; exit 1; }
export LD_LIBRARY_PATH=/tennis_app/lib:$LD_LIBRARY_PATH
echo TENNIS_SWEEP_BEGIN

run() {
    n="$1"
    echo "TENNIS_SWEEP_CONFIG loadbalance run=$n model=tennis_relu_480x640 affinity=none-LB-placed"
    ./tennis_app --mode live --model model/tennis_relu_480x640.rknn \
        --label model/labels.txt --ball-class 0 --device 0 --width 640 --height 480 \
        --fps 30 --duration-sec 20 --core-mask all --min-confidence 25 \
        --log-every 600 --virtual-actuators --profile
}

run 1
run 2
run 3

echo TENNIS_SWEEP_DONE
