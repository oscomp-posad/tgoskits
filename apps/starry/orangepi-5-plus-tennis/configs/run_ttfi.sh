#!/bin/sh
# Time-to-first-inference (TTFI) run: launch the live MJPEG->JPU->RGA->NPU
# pipeline and stop shortly after the first inference. Markers on the console:
#   TENNIS_PROC_START                      (app exec reached run_live)
#   TENNIS_FIRST_INFERENCE ms_since_...    (first completed rknn inference)
#   TENNIS_COLD_START ...                  (in-app cold-start breakdown)
#   TENNIS_TTFI_DONE                       (unique final sentinel)
cd /tennis_app || { echo "MISSING /tennis_app"; exit 1; }
export LD_LIBRARY_PATH=/tennis_app/lib:$LD_LIBRARY_PATH
echo TENNIS_TTFI_BEGIN
# Wait for the UVC camera (1bcf:0b09) to enumerate: at early Linux boot the
# systemd unit can start before USB enumeration finishes and the app then hangs
# in camera init. StarryOS has no /sys/bus/usb (kernel enumerates before the
# shell), so this is a no-op there. The wait is part of measured TTFI on both.
if [ -d /sys/bus/usb/devices ]; then
    i=0
    while [ "$i" -lt 300 ]; do
        grep -qs 1bcf /sys/bus/usb/devices/*/idVendor && break
        i=$((i + 1))
        sleep 0.1
    done
    echo "TENNIS_TTFI_CAMERA_WAIT loops=$i"
fi
# tee the full output to a file on the shared ext4: on Linux the serial getty
# vhangup()s the unit's tty fd mid-run, so anything after login is lost from the
# console. The fflush()ed markers (TENNIS_PROC_START/TENNIS_FIRST_INFERENCE)
# still reach the serial live; the complete log is read back post-hoc.
./tennis_app --mode live --model model/tennis_relu_480x640.rknn \
    --label model/labels.txt --ball-class 0 --device 0 --width 640 --height 480 \
    --fps 30 --duration-sec 15 --core-mask all --min-confidence 25 \
    --log-every 300 --virtual-actuators --profile --infer-affinity 4-7 \
    2>&1 | tee /tennis_app/ttfi_last.log
echo TENNIS_TTFI_DONE
echo TENNIS_TTFI_DONE >> /tennis_app/ttfi_last.log
