#!/usr/bin/env bash
# Build and sign the firmware (then flash it with flash-manual.sh).
#   esp32s3/build.sh                 build, then sign build/tinycrypt.bin (sign.sh)
#   esp32s3/build.sh menuconfig      any other idf.py command, passed through
#   esp32s3/build.sh provisioner     the provisioner image (CRYPT-22) in build-provisioner/
# KEYSTORE picks the secure element (CRYPT-22): soft (the default, INSECURE, TEST ONLY: its secrets
# sit in plain NVS) or atecc (the ATECC608B). An image is built for one or the other.
# BOARD=cam adds sdkconfig.board-cam.defaults (the camera dev board's pins); no BOARD is the PCB.
# Defaults only apply to a fresh sdkconfig, so when the board differs from the last build's
# (remembered next to its sdkconfig, in .board) that sdkconfig is removed first.
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
if [ -n "${BOARD:-}" ]; then
    [ -f "sdkconfig.board-$BOARD.defaults" ] || { echo "no sdkconfig.board-$BOARD.defaults" >&2; exit 2; }
    defaults="$defaults;sdkconfig.board-$BOARD.defaults"
fi
# switch_board SDKCONFIG: start from a fresh sdkconfig if it was made for another board
switch_board() {
    local marker
    marker="$(dirname "$1")/.board"
    [ "$(cat "$marker" 2>/dev/null || true)" = "${BOARD:-pcb}" ] && return 0
    rm -f "$1"
    mkdir -p "$(dirname "$1")"
    echo "${BOARD:-pcb}" > "$marker"
}

if [ "${1:-}" = provisioner ]; then
    shift
    # Its own build directory and sdkconfig, so it never mixes with the key's.
    switch_board build-provisioner/sdkconfig
    idf.py -B build-provisioner -DSDKCONFIG=build-provisioner/sdkconfig \
        "-DSDKCONFIG_DEFAULTS=$defaults;sdkconfig.provisioner.defaults" "${keystore[@]}" build
    ./sign.sh build-provisioner/tinycrypt.bin
    exit
fi
switch_board sdkconfig
if [ $# -gt 0 ]; then
    exec idf.py "${keystore[@]}" "-DSDKCONFIG_DEFAULTS=$defaults" "$@"
fi
idf.py "${keystore[@]}" "-DSDKCONFIG_DEFAULTS=$defaults" build
./sign.sh build/tinycrypt.bin
