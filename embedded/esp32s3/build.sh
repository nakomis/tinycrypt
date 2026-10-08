#!/usr/bin/env bash
# Build (and optionally flash) the INSECURE soft-key test firmware.
#   esp32s3/build.sh                                   build only
#   esp32s3/build.sh -p /dev/cu.usbmodemXXXX flash     build and flash over the UART port
set -euo pipefail
cd "$(dirname "$0")"
IDF_PATH="${IDF_PATH:-$HOME/esp/v5.5/esp-idf}"
# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" > /dev/null
idf.py -DTINYCRYPT_INSECURE_SOFT_KEY=ON "${@:-build}"
