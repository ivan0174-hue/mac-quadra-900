#!/bin/bash
# Build a Raspberry Pi 3 (AArch64) variant of QuadraPi for testing in QEMU.
# Needs a second circle-stdlib configured with "-r 3" (see docs/COMPILARE.txt).
#   STDLIB3=/path/to/circle-stdlib-pi3 tools/build-pi3.sh
set -e
: "${STDLIB3:?set STDLIB3 to a circle-stdlib configured with -r 3}"
cd "$(dirname "$0")/.."
OUT=build-pi3
mkdir -p $OUT
cp -r src basilisk Makefile $OUT/
cd $OUT && make STDLIBHOME="$STDLIB3" TARGET=kernel8 -j4
echo "Kernel: $OUT/kernel8.img"
