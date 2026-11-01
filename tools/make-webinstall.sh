#!/bin/sh
# Build the ESP Web Tools installer site from the release binaries.
#
# usage: tools/make-webinstall.sh <version> <binary dir> <output dir>
#   <binary dir> holds SomfyController.onboard.<chip>.bin and SomfyController.ino.<chip>.bin
#   for esp32, esp32s2, esp32s3 and esp32c3.
#
# manifest-install.json writes the complete image at 0x0, which erases everything.
# manifest-repair.json writes only the first 64 KB of that image (bootloader and partition
# table) and the firmware at 0x10000, so the file system with the shades stays.
set -e
version=${1:?version, e.g. 2.4.10}
bins=${2:?binary directory}
out=${3:?output directory}

rm -rf "$out"
mkdir -p "$out/firmware"
sed "s/@VERSION@/$version/g" "$(dirname "$0")/../webinstall/index.html" > "$out/index.html"
# Serve the files as they are, without Jekyll.
touch "$out/.nojekyll"

install=""
repair=""
for pair in ESP32:esp32 ESP32-S2:esp32s2 ESP32-S3:esp32s3 ESP32-C3:esp32c3; do
  family=${pair%%:*}
  chip=${pair#*:}
  cp "$bins/SomfyController.onboard.$chip.bin" "$out/firmware/"
  cp "$bins/SomfyController.ino.$chip.bin" "$out/firmware/"
  head -c 65536 "$bins/SomfyController.onboard.$chip.bin" > "$out/firmware/SomfyController.bootarea.$chip.bin"
  install="$install${install:+,}
    {\"chipFamily\": \"$family\", \"parts\": [
      {\"path\": \"firmware/SomfyController.onboard.$chip.bin\", \"offset\": 0}]}"
  repair="$repair${repair:+,}
    {\"chipFamily\": \"$family\", \"parts\": [
      {\"path\": \"firmware/SomfyController.bootarea.$chip.bin\", \"offset\": 0},
      {\"path\": \"firmware/SomfyController.ino.$chip.bin\", \"offset\": 65536}]}"
done

cat > "$out/manifest-install.json" <<EOF
{
  "name": "ESPSomfy RTS",
  "version": "$version",
  "new_install_prompt_erase": true,
  "builds": [$install
  ]
}
EOF
cat > "$out/manifest-repair.json" <<EOF
{
  "name": "ESPSomfy RTS (repair)",
  "version": "$version",
  "new_install_prompt_erase": false,
  "builds": [$repair
  ]
}
EOF
ls -l "$out" "$out/firmware"
