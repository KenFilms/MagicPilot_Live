#!/bin/sh
# Desktop preview of the overlay UI: renders UI states over a background picture into PNGs.
#   tools/preview/build.sh <background.rgb565> <out_dir>
# The background is 1280x720 RGB565 (little endian); tools/preview/render.py makes one from any image.
set -e
cd "$(dirname "$0")"
M=../../main
cc -O2 -std=gnu11 -Wall -Wno-unused-function -Istubs -I$M -o preview preview.c \
   $M/overlay.c $M/gfx.c $M/fonts.c $M/ui_draw.c $M/cam_fmt.c -lm
./preview "$1" "$2"
