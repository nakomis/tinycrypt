#!/usr/bin/env bash
# Flash a board whose auto-reset doesn't work (e.g. CH343 UART on macOS).
# Put it in download mode first: hold BOOT, tap RST, release BOOT. Afterwards a watchdog reset
# starts the new firmware, so no RST tap is needed (a plain hard reset over the native USB port
# leaves the S3 in download mode).
#   esp32s3/flash-manual.sh /dev/cu.usbmodemXXXX [thing]
#   esp32s3/flash-manual.sh --provisioner /dev/cu.usbmodemXXXX   the provisioner image (CRYPT-22),
#       from build-provisioner/, without prov; flash the key's own image again afterwards
#   esp32s3/flash-manual.sh --secured /dev/cu.usbmodemXXXX thing   the secured variant (CRYPT-34), from
#       build-secured/, onto a chip whose secure boot isn't on yet: erases ALL of the flash, writes the
#       signed bootloader too, and asks for the chip's MAC to be typed. Its first boot then burns the
#       eFuses (secure boot, flash encryption, release mode) for good, after which this can't flash it
#       again: secured keys are updated only over the air.
# ESPTOOL_AFTER=no_reset leaves the chip in download mode after writing (e.g. to start a serial log first,
# then press RST to watch the first boot).
# ESPTOOL_BEFORE=default_reset skips BOOT+RST when the chip's USB-Serial-JTAG is up (the native port
# unless firmware using TinyUSB is running, as the key image does).
# Writes the bootloader, partition table, OTA data and the (signed, see build.sh) app, plus the
# key's prov partition from certs/<thing>/prov.bin (provision-key.sh; the only one there if no thing
# is named). Never the nvs partition: the sign counter, resident keys and enrolled watch survive.
# Writing OTA data boots ota_0, whichever slot an update had moved to. The app must be signed by
# this environment's key (ota-signing/{env}.pub.pem, env from NPM_ENVIRONMENT, default sandbox): one
# signed by another would boot, then refuse every update from Fleet.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
provisioner=false secured=false
build=build app=tinycrypt.bin partitions=partitions.csv
case "${1:-}" in
    --provisioner) provisioner=true build=build-provisioner; shift ;;
    --secured)
        secured=true build=build-secured app=tinycrypt-secured.bin partitions=partitions-secured.csv; shift
        [ -n "${2:-}" ] || { echo "--secured needs the thing" >&2; exit 2; } ;;
esac
cd "$here/$build"
IDF_PATH="${IDF_PATH:-$HOME/esp/v5.5/esp-idf}"
# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" > /dev/null

env=${NPM_ENVIRONMENT:-sandbox}
espsecure.py verify_signature --version 2 --keyfile "$here/ota-signing/${env}.pub.pem" "$app" > /dev/null 2>&1 || {
    echo "$build/$app isn't signed by the $env key (ota-signing/${env}.pub.pem): run build.sh" >&2
    exit 1
}
if $secured; then
    # The bootloader's signature blocks are what its first boot burns as trusted keys: both must be
    # this environment's (its key and its backup), or the chip would trust the wrong keys for good.
    for name in "$env" "${env}-backup"; do
        espsecure.py verify_signature --version 2 --keyfile "$here/ota-signing/${name}.pub.pem" bootloader/bootloader.bin > /dev/null 2>&1 || {
            echo "$build/bootloader/bootloader.bin isn't signed by the $name key: run VARIANT=secured build.sh" >&2
            exit 1
        }
    done
fi

if $provisioner; then
    python -m esptool --chip esp32s3 -p "$1" -b 460800 --before "${ESPTOOL_BEFORE:-no_reset}" --after "${ESPTOOL_AFTER:-watchdog_reset}" write_flash @flash_args
    echo "provisioner flashed: esp32s3/tools/atecc_provision.py $1 info"
    exit
fi

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
offset=$(awk -F', *' '$1 == "prov" { print $4 }' "$here/$partitions")
echo "prov: ${prov[0]} at ${offset}"
if $secured; then
    summary=$(espefuse.py --chip esp32s3 -p "$1" --before "${ESPTOOL_BEFORE:-no_reset}" summary 2>&1) || {
        echo "couldn't read the eFuses: is it in download mode?" >&2
        exit 1
    }
    if printf '%s\n' "$summary" | grep -E '^SECURE_BOOT_EN .*= True' > /dev/null; then
        echo "secure boot is already on: this chip can only be updated over the air" >&2
        exit 1
    fi
    # Flash encryption on without secure boot is a half-burned chip: a plain write wouldn't boot. Stop.
    if ! printf '%s\n' "$summary" | grep -E '^SPI_BOOT_CRYPT_CNT .*\(0b000\)' > /dev/null; then
        echo "flash encryption has already been turned on (SPI_BOOT_CRYPT_CNT): not flashing" >&2
        exit 1
    fi
    mac=$(python -m esptool --chip esp32s3 -p "$1" --before no_reset --after no_reset read_mac 2>&1 | awk '/^MAC: / && !seen { print $2; seen = 1 }')
    [ -n "$mac" ] || { echo "couldn't read the chip's MAC" >&2; exit 1; }
    echo "Flashing the secured variant ($env keys) ERASES ALL FLASH, and its first boot burns this chip's"
    echo "eFuses for good: secure boot, flash encryption, JTAG off, secure download mode only."
    read -r -p "Type the chip's MAC (${mac}) to go on: " typed
    [ "$typed" = "$mac" ] || { echo "not flashed" >&2; exit 1; }
    # One write: both args files start with the same --flash_mode/--flash_freq/--flash_size line, which
    # esptool won't take twice, so the options once and every offset/file pair after them.
    read -r -a options < flash_args
    images=()
    while read -r address file; do
        images+=("$address" "$file")
    done < <(tail -n +2 flash_bootloader_args; tail -n +2 flash_args)
    python -m esptool --chip esp32s3 -p "$1" -b 460800 --before "${ESPTOOL_BEFORE:-no_reset}" --after "${ESPTOOL_AFTER:-watchdog_reset}" write_flash \
        --erase-all "${options[@]}" "${images[@]}" "$offset" "${prov[0]}"
    exit
fi
python -m esptool --chip esp32s3 -p "$1" -b 460800 --before "${ESPTOOL_BEFORE:-no_reset}" --after "${ESPTOOL_AFTER:-watchdog_reset}" write_flash @flash_args \
    "$offset" "${prov[0]}"
