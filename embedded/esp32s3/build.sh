#!/usr/bin/env bash
# Build and sign the firmware (then flash it with flash-manual.sh).
#   esp32s3/build.sh                 build, then sign build/tinycrypt.bin (sign.sh)
#   esp32s3/build.sh menuconfig      any other idf.py command, passed through
#   esp32s3/build.sh provisioner     the provisioner image (CRYPT-22) in build-provisioner/
# KEYSTORE picks the secure element (CRYPT-22): soft (the default, INSECURE, TEST ONLY: its secrets
# sit in plain NVS) or atecc (the ATECC608B). An image is built for one or the other.
# Images must be signed (CRYPT-14), so don't flash with `idf.py flash`: it would write the unsigned one.
set -euo pipefail
cd "$(dirname "$0")"
IDF_PATH="${IDF_PATH:-$HOME/esp/v5.5/esp-idf}"
# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" > /dev/null

case "${KEYSTORE:-soft}" in
    soft) keystore=(-DTINYCRYPT_INSECURE_SOFT_KEY=ON -DTINYCRYPT_KEYSTORE_ATECC=OFF) ;;
    atecc) keystore=(-DTINYCRYPT_INSECURE_SOFT_KEY=OFF -DTINYCRYPT_KEYSTORE_ATECC=ON) ;;
    *) echo "KEYSTORE must be soft or atecc" >&2; exit 2 ;;
esac

if [ "${1:-}" = provisioner ]; then
    shift
    # Its own build directory and sdkconfig, so it never mixes with the key's.
    idf.py -B build-provisioner -DSDKCONFIG=build-provisioner/sdkconfig \
        "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.provisioner.defaults" "${keystore[@]}" build
    ./sign.sh build-provisioner/tinycrypt.bin
    exit
fi
if [ $# -gt 0 ]; then
    exec idf.py "${keystore[@]}" "$@"
fi
idf.py "${keystore[@]}" build
./sign.sh build/tinycrypt.bin
