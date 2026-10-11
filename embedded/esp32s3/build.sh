#!/usr/bin/env bash
# Build and sign the INSECURE soft-key test firmware (then flash it with flash-manual.sh).
#   esp32s3/build.sh                 build, then sign build/tinycrypt.bin (sign.sh)
#   esp32s3/build.sh menuconfig      any other idf.py command, passed through
# Images must be signed (CRYPT-14), so don't flash with `idf.py flash`: it would write the unsigned one.
set -euo pipefail
cd "$(dirname "$0")"
IDF_PATH="${IDF_PATH:-$HOME/esp/v5.5/esp-idf}"
# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" > /dev/null
if [ $# -gt 0 ]; then
    exec idf.py -DTINYCRYPT_INSECURE_SOFT_KEY=ON "$@"
fi
# Always a fresh, unsigned image to sign (signing one that's already signed would add a second block).
rm -f build/tinycrypt.bin
idf.py -DTINYCRYPT_INSECURE_SOFT_KEY=ON build
./sign.sh build/tinycrypt.bin
