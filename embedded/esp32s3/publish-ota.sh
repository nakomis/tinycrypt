#!/usr/bin/env bash
# CRYPT-14: build, sign and publish a firmware version for Fleet to offer the keys.
#
#   esp32s3/publish-ota.sh <x.y.z> [--test-rollback]
#
# Uploads s3://nakomis-firmware-{env}/tinycrypt/<x.y.z>/firmware.bin with metadata signed=true and
# its sha256, which Fleet's manifest picks up; then update a key from Fleet's UI. The version must be
# esp32s3/version.txt, the tree clean (the image says which commit it is), and the version new:
# a published version is never replaced. --test-rollback allows an image built with
# CONFIG_TINYCRYPT_OTA_FAIL_SELFTEST (it rolls back after installing); never otherwise.
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

"$here/build.sh"
bin="$here/build/tinycrypt.bin"

if grep -q '^CONFIG_TINYCRYPT_OTA_FAIL_SELFTEST=y' "$here/sdkconfig" && ! $test_rollback; then
    echo "this build fails its self-test on purpose (CONFIG_TINYCRYPT_OTA_FAIL_SELFTEST): not publishing" >&2
    exit 1
fi

# No secrets or per-key settings in a published image (they live in the prov partition): no Thing
# name, endpoint or Wi-Fi SSID, and no line of any provisioned key's certificate or private key.
# (Not a bare "PRIVATE KEY": mbedTLS's PEM parser has that string in every image.)
needles=("tinycrypt-${env}-key" "-ats.iot.")
shopt -s nullglob
for pem in "$here"/certs/*/device.pem.key "$here"/certs/*/device.pem.crt; do
    needles+=("$(sed -n 2p "$pem")")
done
for needle in "${needles[@]}"; do
    if LC_ALL=C grep -qa -- "$needle" "$bin"; then
        echo "the image contains \"$needle\": not publishing" >&2
        exit 1
    fi
done
ssid=$(aws ssm get-parameter --name /Nakomis/Wifi/SSID --query Parameter.Value --output text 2> /dev/null || true)
if [ -n "$ssid" ] && LC_ALL=C grep -qaF -- "$ssid" "$bin"; then
    echo "the image contains the Wi-Fi SSID: not publishing" >&2
    exit 1
fi

sha=$(shasum -a 256 "$bin" | cut -d' ' -f1)
aws s3 cp "$bin" "s3://${bucket}/${object}" --content-type application/octet-stream \
    --metadata "signed=true,sha256=${sha}" > /dev/null
echo "published tinycrypt ${version} (sha256 ${sha}) to s3://${bucket}/${object}"
echo "now update a key from Fleet: https://app.${env}.nakomis.com/fleet"
