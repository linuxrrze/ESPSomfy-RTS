#!/bin/sh
# Stage the web application for the LittleFS image with the text assets
# gzipped. The firmware serves <file>.gz with Content-Encoding: gzip and
# falls back to the uncompressed file, so data/ can still be uploaded as is.
#
# usage: tools/gzip-data.sh <data dir> <output dir>
#   e.g. tools/gzip-data.sh data build/data
#        mklittlefs --create build/data --size 1441792 SomfyController.littlefs.bin
set -e
src=${1:?source directory}
dst=${2:?output directory}

rm -rf "$dst"
mkdir -p "$dst"
cp -R "$src"/. "$dst"/
for f in "$dst"/*.html "$dst"/*.js "$dst"/*.css "$dst"/*.svg; do
  [ -f "$f" ] || continue
  # -n leaves the name and timestamp out of the header so the image is reproducible.
  gzip -9 -n "$f"
done
ls -l "$dst"
