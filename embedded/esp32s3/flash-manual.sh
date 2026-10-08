#!/usr/bin/env bash
# Flash a board whose auto-reset doesn't work (e.g. CH343 UART on macOS).
# Put it in download mode first: hold BOOT, tap RST, release BOOT.
#   esp32s3/flash-manual.sh /dev/cu.usbmodemXXXX
set -euo pipefail
cd "$(dirname "$0")/build"
IDF_PATH="${IDF_PATH:-$HOME/esp/v5.5/esp-idf}"
# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" > /dev/null
python -m esptool --chip esp32s3 -p "$1" -b 460800 --before no_reset --after hard_reset write_flash @flash_args
