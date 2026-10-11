#!/usr/bin/env bash
# Flash a board whose auto-reset doesn't work (e.g. CH343 UART on macOS).
# Put it in download mode first: hold BOOT, tap RST, release BOOT. Afterwards a watchdog reset
# starts the new firmware, so no RST tap is needed (a plain hard reset over the native USB port
# leaves the S3 in download mode).
#   esp32s3/flash-manual.sh /dev/cu.usbmodemXXXX [thing]
# Writes the bootloader, partition table, OTA data and the (signed, see build.sh) app, plus the
# key's prov partition from certs/<thing>/prov.bin (provision-key.sh; the only one there if no thing
# is named). Never the nvs partition: the sign counter, resident keys and enrolled watch survive.
# Writing OTA data boots ota_0, whichever slot an update had moved to. The app must be signed by
# this environment's key (ota-signing/{env}.pub.pem, env from NPM_ENVIRONMENT, default sandbox): one
# signed by another would boot, then refuse every update from Fleet.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
cd "$here/build"
IDF_PATH="${IDF_PATH:-$HOME/esp/v5.5/esp-idf}"
# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" > /dev/null

env=${NPM_ENVIRONMENT:-sandbox}
espsecure.py verify_signature --version 2 --keyfile "$here/ota-signing/${env}.pub.pem" tinycrypt.bin > /dev/null 2>&1 || {
    echo "build/tinycrypt.bin isn't signed by the $env key (ota-signing/${env}.pub.pem): run build.sh" >&2
    exit 1
}

prov=()
if [ -n "${2:-}" ]; then
    prov=("$here/certs/$2/prov.bin")
else
    shopt -s nullglob
    prov=("$here"/certs/*/prov.bin)
fi
if [ "${#prov[@]}" -ne 1 ] || [ ! -s "${prov[0]}" ]; then
    echo "need exactly one prov.bin (have ${#prov[@]}): name the thing, or run infra/scripts/provision-key.sh" >&2
    exit 1
fi
offset=$(awk -F', *' '$1 == "prov" { print $4 }' "$here/partitions.csv")
echo "prov: ${prov[0]} at ${offset}"
python -m esptool --chip esp32s3 -p "$1" -b 460800 --before no_reset --after watchdog_reset write_flash @flash_args \
    "$offset" "${prov[0]}"
