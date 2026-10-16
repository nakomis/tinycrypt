#!/usr/bin/env bash
# Build and sign the firmware (then flash it with flash-manual.sh).
#   esp32s3/build.sh                 build, then sign build/tinycrypt.bin (sign.sh)
#   esp32s3/build.sh menuconfig      any other idf.py command, passed through
#   esp32s3/build.sh provisioner     the provisioner image (CRYPT-22) in build-provisioner/
# KEYSTORE picks the secure element (CRYPT-22): soft (the default, INSECURE, TEST ONLY: its secrets
# sit in plain NVS) or atecc (the ATECC608B). An image is built for one or the other.
# BOARD=cam adds sdkconfig.board-cam.defaults (the camera dev board's pins). Defaults only apply to
# a fresh sdkconfig, so a BOARD build starts from one (sdkconfig / build-provisioner/sdkconfig removed).
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

defaults="sdkconfig.defaults"
board=()
if [ -n "${BOARD:-}" ]; then
    [ -f "sdkconfig.board-$BOARD.defaults" ] || { echo "no sdkconfig.board-$BOARD.defaults" >&2; exit 2; }
    defaults="$defaults;sdkconfig.board-$BOARD.defaults"
    board=("-DSDKCONFIG_DEFAULTS=$defaults")
fi

if [ "${1:-}" = provisioner ]; then
    shift
    # Its own build directory and sdkconfig, so it never mixes with the key's.
    [ -n "${BOARD:-}" ] && rm -f build-provisioner/sdkconfig
    idf.py -B build-provisioner -DSDKCONFIG=build-provisioner/sdkconfig \
        "-DSDKCONFIG_DEFAULTS=$defaults;sdkconfig.provisioner.defaults" "${keystore[@]}" build
    ./sign.sh build-provisioner/tinycrypt.bin
    exit
fi
if [ $# -gt 0 ]; then
    exec idf.py "${keystore[@]}" ${board[@]+"${board[@]}"} "$@"
fi
[ -n "${BOARD:-}" ] && rm -f sdkconfig
idf.py "${keystore[@]}" ${board[@]+"${board[@]}"} build
./sign.sh build/tinycrypt.bin
