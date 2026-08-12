#!/bin/sh
# Throughput test for the USERSPACE capture-affinity sidestep (kernel keeps the
# xHCI completion IRQ on cpu0 — USB_IRQ_TARGET_CPU=None — because routing it to a
# big core cross-core-deadlocks the xHCI completion path). The
# bursty capture supply caps processed fps at ~14 on StarryOS because the whole
# capture path (USB IRQ + URB worker + libuvc 614KB memcpy) piles on the single
# loaded boot core cpu0. This test moves only the EXPENSIVE userspace half — the
# libuvc receive thread (614KB/frame stash + decode) — to an A76 via
# UVC_CAPTURE_AFFINITY, leaving the cheap IRQ ack on cpu0 (zero cross-core driver
# lock contention), with inference on a DIFFERENT A76 (cpu5). Measures whether
# that alone lifts processed fps above the ~14fps cpu0-supply ceiling.
# Only 2 launches (the cross-process USB hang bites the 3rd), both complete.
cd /tennis_app || { echo "MISSING /tennis_app"; exit 1; }
export LD_LIBRARY_PATH=/tennis_app/lib:$LD_LIBRARY_PATH
echo TENNIS_SWEEP_BEGIN

run() {
    m="$1"; ia="$2"; ca="$3"
    echo "TENNIS_SWEEP_CONFIG model=$m infer=${ia:-none} capture=${ca:-none}"
    aff=""
    [ -n "$ia" ] && aff="--infer-affinity $ia"
    UVC_CAPTURE_AFFINITY="$ca" ./tennis_app --mode live --model "model/$m.rknn" \
        --label model/labels.txt --ball-class 0 --device 0 --width 640 --height 480 \
        --fps 30 --duration-sec 20 --core-mask all --min-confidence 25 \
        --log-every 600 --virtual-actuators --profile $aff
}

# IRQ stays on cpu0 (kernel). Vary only the userspace capture-thread placement:
run tennis_relu_480x640 "5" ""     # baseline: infer cpu5, libuvc thread unplaced (scheduler default)
run tennis_relu_480x640 "5" "4"    # sidestep: infer cpu5, libuvc receive thread -> cpu4 (A76)

echo TENNIS_SWEEP_DONE
