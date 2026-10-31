#!/bin/bash
# Run the Pi 3 test kernel in QEMU (raspi3b) with an SD card image.
#   tools/run.sh build-pi3/kernel8.img [seconds]   (SD=sd.img by default)
# The QEMU monitor listens on mon.sock (use tools/shot.sh for screenshots,
# or: echo "sendkey f12" | socat - UNIX-CONNECT:mon.sock)
rm -f mon.sock serial.log
timeout ${2:-600} qemu-system-aarch64 -M raspi3b -kernel $1 -drive file=${SD:-sd.img},if=sd,format=raw \
  -serial file:serial.log -display none -monitor unix:mon.sock,server,nowait \
  -usb -device usb-kbd -device usb-mouse > qemu.log 2>&1 &
