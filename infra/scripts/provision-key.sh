#!/usr/bin/env bash
# CRYPT-15: give a key its IoT certificate and the settings its firmware needs. Safe to re-run.
#
#   infra/scripts/provision-key.sh [key-name] [--new]       (default key-1)
#
# Each run makes sure the key's certificate is active and attached to its Thing and the policy.
# --new retires the Thing's existing certificates (detach, deactivate, delete) and makes a fresh one;
# use it if a key's private key may have leaked.
#
# Writes, never committed, under embedded/esp32s3/:
#   certs/<thing>/device.pem.crt, device.pem.key, certificate-arn; certs/AmazonRootCA1.pem
#   sdkconfig.secrets   Wi-Fi, IoT endpoint, Thing name, topic and APNs platform for the firmware
#
# INSECURE, TEST ONLY: the private key is made here and flashed in plain text, and the Wi-Fi password
# ends up in the build's sdkconfig and firmware image. The real key keeps its private key on the
# ATECC608 and only ever sends AWS a CSR.
#
# Needs AWS_PROFILE (default nakom.is-sandbox) and NPM_ENVIRONMENT (default sandbox). The house
# Wi-Fi credentials are read from /Nakomis/Wifi/* (as catcam does), via WIFI_PROFILE (default AWS_PROFILE).
set -euo pipefail
umask 077

key=key-1
new=false
for arg in "$@"; do
    case "$arg" in
        --new) new=true ;;
        -*) echo "unknown option: $arg" >&2; exit 2 ;;
        *) key=$arg ;;
    esac
done
env=${NPM_ENVIRONMENT:-sandbox}
export AWS_PROFILE=${AWS_PROFILE:-nakom.is-sandbox}
export AWS_REGION=${AWS_REGION:-eu-west-2}
wifi_profile=${WIFI_PROFILE:-$AWS_PROFILE}
platform=$([ "$env" = prod ] && echo APNS || echo APNS_SANDBOX)

here=$(cd "$(dirname "$0")" && pwd)
fw="${here}/../../embedded/esp32s3"
thing="tinycrypt-${env}-${key}"
policy="tinycrypt-key-${env}"
dir="${fw}/certs/${thing}"
mkdir -p "$dir"

aws iot describe-thing --thing-name "$thing" > /dev/null   # fails here if TinycryptKeyStack lacks this key

retire() {
    local arn=$1 id=${1##*/}
    aws iot detach-policy --policy-name "$policy" --target "$arn" 2>/dev/null || true
    aws iot detach-thing-principal --thing-name "$thing" --principal "$arn"
    aws iot update-certificate --certificate-id "$id" --new-status INACTIVE
    # Detaching from a Thing is eventually consistent; deletion can lag a few seconds behind.
    for _ in 1 2 3 4 5 6; do aws iot delete-certificate --certificate-id "$id" 2> /dev/null && break; sleep 5; done
    echo "retired certificate ${id}"
}

if $new; then
    for arn in $(aws iot list-thing-principals --thing-name "$thing" --query 'principals' --output text); do
        retire "$arn"
    done
    rm -f "${dir}/device.pem.crt" "${dir}/device.pem.key" "${dir}/certificate-arn"
fi

if [ ! -s "${dir}/device.pem.key" ] || [ ! -s "${dir}/certificate-arn" ]; then
    aws iot create-keys-and-certificate --set-as-active \
        --certificate-pem-outfile "${dir}/device.pem.crt" \
        --private-key-outfile "${dir}/device.pem.key" \
        --query certificateArn --output text > "${dir}/certificate-arn"
    echo "new certificate $(cut -d/ -f2 < "${dir}/certificate-arn") for ${thing}"
fi
cert_arn=$(cat "${dir}/certificate-arn")

# Both attachments are idempotent, so every run repairs a half-finished earlier one.
aws iot attach-policy --policy-name "$policy" --target "$cert_arn"
aws iot attach-thing-principal --thing-name "$thing" --principal "$cert_arn"
status=$(aws iot describe-certificate --certificate-id "${cert_arn##*/}" --query certificateDescription.status --output text)
[ "$status" = ACTIVE ] || { echo "certificate ${cert_arn##*/} is ${status}: rerun with --new" >&2; exit 1; }

ca="${fw}/certs/AmazonRootCA1.pem"
[ -s "$ca" ] || curl -fsS -o "$ca" https://www.amazontrust.com/repository/AmazonRootCA1.pem

endpoint=$(aws iot describe-endpoint --endpoint-type iot:Data-ATS --query endpointAddress --output text)
# Through the environment, not argv, so the password never shows up in `ps`.
export TC_SSID TC_PASSWORD
TC_SSID=$(aws ssm get-parameter --profile "$wifi_profile" --name /Nakomis/Wifi/SSID --query Parameter.Value --output text)
TC_PASSWORD=$(aws ssm get-parameter --profile "$wifi_profile" --name /Nakomis/Wifi/Password --with-decryption \
    --query Parameter.Value --output text)

python3 - "$fw/sdkconfig.secrets" "$endpoint" "$thing" "$env" "$platform" <<'PY'
import json, os, sys
path, endpoint, thing, env, platform = sys.argv[1:]
ssid, password = os.environ["TC_SSID"], os.environ["TC_PASSWORD"]
if len(ssid.encode()) > 32 or len(password.encode()) > 63:
    sys.exit("Wi-Fi SSID or password too long for ESP-IDF (32 / 63 bytes)")

def q(value):
    # Kconfig strings take C escapes (\" and \\), which JSON's match; keep UTF-8 as is, since Kconfig
    # has no \u escapes.
    return json.dumps(value, ensure_ascii=False)

lines = [
    "# Written by infra/scripts/provision-key.sh. Not committed: holds the Wi-Fi password.",
    "CONFIG_TINYCRYPT_NOTIFIER=y",
    f"CONFIG_TINYCRYPT_WIFI_SSID={q(ssid)}",
    f"CONFIG_TINYCRYPT_WIFI_PASSWORD={q(password)}",
    f"CONFIG_TINYCRYPT_IOT_ENDPOINT={q(endpoint)}",
    f"CONFIG_TINYCRYPT_THING_NAME={q(thing)}",
    f"CONFIG_TINYCRYPT_PRESENCE_TOPIC={q(f'tinycrypt/{env}/presence/{thing}/request')}",
    f"CONFIG_TINYCRYPT_APNS_PLATFORM={q(platform)}",
]
open(path, "w", encoding="utf-8").write("\n".join(lines) + "\n")
PY
echo "wrote ${fw}/sdkconfig.secrets for ${thing} (${endpoint})"
echo "rebuild with a clean sdkconfig so the new values apply: rm embedded/esp32s3/sdkconfig"
