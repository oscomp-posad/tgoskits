#!/bin/sh
# Baseline model comparison on StarryOS: run each of the 4 tennis RKNN models
# through the live pipeline (UVC camera -> detect -> state machine -> virtual
# actuators) with full profiling. Kept on the rootfs so the board config's
# shell_init_cmd is one short line (long inline commands garble at 1.5M baud).
# Each tennis_app run prints its own TENNIS_BENCH_DONE; the unique final marker
# TENNIS_MULTI_DONE is what the board config gates success on, so all 4 run.
cd /tennis_app || { echo "MISSING /tennis_app"; exit 1; }
export LD_LIBRARY_PATH=/tennis_app/lib:$LD_LIBRARY_PATH
echo TENNIS_MULTI_BEGIN
for m in tennis_relu_480x640 tennis_relu_640x640 tennis_silu_480x640 tennis_silu_640x640; do
    echo "TENNIS_MODEL model=$m"
    ./tennis_app --mode live --model "model/$m.rknn" --label model/labels.txt \
        --ball-class 0 --device 0 --width 640 --height 480 --fps 30 \
        --duration-sec 20 --core-mask all --min-confidence 25 --log-every 600 \
        --virtual-actuators --profile
done
echo TENNIS_MULTI_DONE
