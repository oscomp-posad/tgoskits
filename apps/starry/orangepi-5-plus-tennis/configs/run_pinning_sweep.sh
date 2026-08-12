#!/bin/sh
# Staged affinity sweep on StarryOS for the dequant-on-demand (want_float=0)
# build. One binary, all variants via flags, so a single power cycle measures:
#   - the want_float=0 dequant win on its own (config with no pinning), and
#   - stacking A76 big-core placement on top, in stages.
#
# In this app decode + inference + the on-demand dequant all run on the MAIN
# consumer thread, which --infer-affinity pins. RK3588: cpu0-3 = A55 little,
# cpu4-7 = A76 big. NOTE: the libuvc capture/stash thread does USB work and MUST
# stay on the boot core (cpu0) on StarryOS -- pinning it off cpu0 via
# UVC_CAPTURE_AFFINITY HANGS the board (same class as "USB/rknn_init hangs on a
# non-boot core"), so the capture-pin configs are removed. The only safe lever is
# main-thread placement via --infer-affinity (applied after init).
#
# Each tennis_app run prints its own TENNIS_BENCH_DONE; the board config gates on
# the unique final marker TENNIS_SWEEP_DONE so all configs run.
cd /tennis_app || { echo "MISSING /tennis_app"; exit 1; }
export LD_LIBRARY_PATH=/tennis_app/lib:$LD_LIBRARY_PATH
echo TENNIS_SWEEP_BEGIN

run() {
    m="$1"; ia="$2"; ca="$3"
    echo "TENNIS_SWEEP_CONFIG model=$m infer=${ia:-none} capture=${ca:-none}"
    aff=""
    [ -n "$ia" ] && aff="--infer-affinity $ia"
    # UVC_CAPTURE_AFFINITY is read once by the libuvc receive thread (no-op if empty).
    UVC_CAPTURE_AFFINITY="$ca" ./tennis_app --mode live --model "model/$m.rknn" \
        --label model/labels.txt --ball-class 0 --device 0 --width 640 --height 480 \
        --fps 30 --duration-sec 20 --core-mask all --min-confidence 25 \
        --log-every 600 --virtual-actuators --profile $aff
}

# DISCRIMINATING TEST: reorder to isolate the cross-process hang trigger.
# Observed pattern across 3 boots: configs 1+2 complete, config 3 hangs.
# Sequence none -> 4-7 -> none tests: does an UNPINNED config 3 hang after
# an A76-pinned config 2? If yes -> "after-any-pinned" (global state poison).
# If it completes -> "2nd pinned hangs" or "3rd invocation regardless."
run tennis_relu_480x640 ""    ""      # A: baseline, no pinning
run tennis_relu_480x640 "4-7" ""      # B: A76 big cores
run tennis_relu_480x640 ""    ""      # C: UNPINNED after a pinned run (the test)
run tennis_relu_480x640 "4-7" ""      # D: another A76 (tests "2nd pin" theory if C ok)
run tennis_relu_480x640 ""    ""      # E: one more unpinned

echo TENNIS_SWEEP_DONE
