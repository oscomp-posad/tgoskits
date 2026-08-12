#!/bin/sh
# MJPEG -> JPU -> RGA -> NPU zero-copy pipeline bench.
# The camera now negotiates MJPEG first; each frame is decoded on the RK3588 JPU
# (librockchip_mpp via /dev/mpp_service), then RGA does NV12->RGB888 straight into
# the NPU input (Path A zero-copy). Watch for:
#   TENNIS_JPU decoder ready ...            (JPU MJPEG decoder up)
#   TENNIS_RGA setup: ... (zero-copy)       (RGA dst = NPU phys)
#   <camera> ctrl format ... (MJPEG b_format_index)
#   decode_ms (JPU+RGA preprocess), detections, captured/processed fps
cd /tennis_app || { echo "MISSING /tennis_app"; exit 1; }
export LD_LIBRARY_PATH=/tennis_app/lib:$LD_LIBRARY_PATH
echo TENNIS_MJPEG_BEGIN
./tennis_app --mode live --model model/tennis_relu_480x640.rknn \
    --label model/labels.txt --ball-class 0 --device 0 --width 640 --height 480 \
    --fps 30 --duration-sec 20 --core-mask all --min-confidence 25 \
    --log-every 300 --virtual-actuators --profile --infer-affinity 4-7
echo TENNIS_MJPEG_DONE
