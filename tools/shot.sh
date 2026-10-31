#!/bin/bash
# usage: shot.sh name
echo "screendump $1.ppm" | socat - UNIX-CONNECT:mon.sock >/dev/null
for i in 1 2 3 4 5; do [ -s $1.ppm ] && break; sleep 0.5; done
convert $1.ppm $1.png && rm $1.ppm
