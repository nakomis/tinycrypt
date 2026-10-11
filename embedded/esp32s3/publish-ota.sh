#!/usr/bin/env bash
# CRYPT-14: build, sign and publish a firmware version for Fleet to offer the keys.
#
#   esp32s3/publish-ota.sh <x.y.z> [--test-rollback]
#
# Uploads s3://nakomis-firmware-{env}/tinycrypt/<x.y.z>/firmware.bin with metadata signed=true and
# its sha256, which Fleet's manifest picks up; then update a key from Fleet's UI. The version must be
# esp32s3/version.txt, the tree clean (the image says which commit it is), and the version new:
# a published version is never replaced. The build comes from sdkconfig.defaults alone (the local
# sdkconfig is replaced). --test-rollback builds with CONFIG_TINYCRYPT_OTA_FAIL_SELFTEST, an image
# that rolls back after installing, to test that; never otherwise.
#
# Needs AWS_PROFILE (default nakom.is-sandbox) and NPM_ENVIRONMENT (default sandbox).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
version=${1:?usage: publish-ota.sh <x.y.z> [--test-rollback]}
test_rollback=false
[ "${2:-}" = --test-rollback ] && test_rollback=true
env=${NPM_ENVIRONMENT:-sandbox}
export AWS_PROFILE=${AWS_PROFILE:-nakom.is-sandbox}
export AWS_REGION=${AWS_REGION:-eu-west-2}
IDF_PATH="${IDF_PATH:-$HOME/esp/v5.5/esp-idf}"
# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" > /dev/null 2>&1   # for esptool
bucket="nakomis-firmware-${env}"
object="tinycrypt/${version}/firmware.bin"

[[ "$version" =~ ^[0-9]{1,4}\.[0-9]{1,4}\.[0-9]{1,4}$ ]] || { echo "version must be x.y.z" >&2; exit 1; }
[ "$(tr -d '[:space:]' < "$here/version.txt")" = "$version" ] || {
    echo "esp32s3/version.txt says $(cat "$here/version.txt"), not $version" >&2
    exit 1
}
[ -z "$(git -C "$here" status --porcelain)" ] || { echo "the tree has uncommitted changes" >&2; exit 1; }
if aws s3api head-object --bucket "$bucket" --key "$object" > /dev/null 2>&1; then
    echo "s3://${bucket}/${object} already exists: publish a new version" >&2
    exit 1
fi
# Read before building, so a missing credential fails early; the image is checked for it below.
ssid=$(aws ssm get-parameter --name /Nakomis/Wifi/SSID --query Parameter.Value --output text) || {
    echo "can't read the Wi-Fi SSID to check the image for it: not publishing" >&2
    exit 1
}

# From sdkconfig.defaults alone, never whatever local sdkconfig is lying around: a build without the
# notifier, say, would be kept at once and could never be updated again. (IDF also reads
# version.txt only at configure time.)
rm -f "$here/sdkconfig"
defaults="sdkconfig.defaults"
if $test_rollback; then
    mkdir -p "$here/build"
    printf 'CONFIG_TINYCRYPT_OTA_FAIL_SELFTEST=y\n' > "$here/build/rollback-test.defaults"
    defaults="sdkconfig.defaults;build/rollback-test.defaults"
fi
"$here/build.sh" "-DSDKCONFIG_DEFAULTS=${defaults}" reconfigure > /dev/null
"$here/build.sh"
bin="$here/build/tinycrypt.bin"
if $test_rollback; then
    # Don't leave the failing self-test in the dev config (or in CMake's cached defaults).
    cp "$bin" "$here/build/rollback-test.bin"
    rm -f "$here/sdkconfig" "$here/build/rollback-test.defaults"
    "$here/build.sh" "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults" reconfigure > /dev/null
    bin="$here/build/rollback-test.bin"
fi
config="$here/build/config/sdkconfig.h"
built=$(python -m esptool --chip esp32s3 image_info --version 2 "$bin" 2> /dev/null | awk -F': *' '/^App version/ { print $2 }')
[ "$built" = "$version" ] || { echo "the image says version '$built', not $version" >&2; exit 1; }

# What every published image needs: it can be updated (the notifier), it's checked on probation
# (rollback), and it checks the next one's signature.
for option in CONFIG_TINYCRYPT_NOTIFIER CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT; do
    grep -q "^#define ${option} 1$" "$config" || { echo "the build lacks ${option}: not publishing" >&2; exit 1; }
done
if grep -q '^#define CONFIG_TINYCRYPT_OTA_FAIL_SELFTEST 1$' "$config" && ! $test_rollback; then
    echo "this build fails its self-test on purpose (CONFIG_TINYCRYPT_OTA_FAIL_SELFTEST): not publishing" >&2
    exit 1
fi

# No secrets or per-key settings in a published image (they live in the prov partition): no Thing
# name, endpoint or Wi-Fi SSID, and no line of any provisioned key's certificate or private key.
# (Not a bare "PRIVATE KEY": mbedTLS's PEM parser has that string in every image.)
needles=("tinycrypt-${env}-key" "-ats.iot." "$ssid")
shopt -s nullglob
for pem in "$here"/certs/*/device.pem.key "$here"/certs/*/device.pem.crt; do
    needles+=("$(sed -n 2p "$pem")")
done
for needle in "${needles[@]}"; do
    if LC_ALL=C grep -qaF -- "$needle" "$bin"; then
        echo "the image contains a per-key string (\"${needle:0:12}...\"): not publishing" >&2
        exit 1
    fi
done

sha=$(shasum -a 256 "$bin" | cut -d' ' -f1)
# --if-none-match: a published version is never replaced, even by a publish racing this one.
aws s3api put-object --bucket "$bucket" --key "$object" --body "$bin" --if-none-match '*' \
    --content-type application/octet-stream --metadata "signed=true,sha256=${sha}" > /dev/null
echo "published tinycrypt ${version} (sha256 ${sha}) to s3://${bucket}/${object}"
echo "now update a key from Fleet: https://app.${env}.nakomis.com/fleet"
