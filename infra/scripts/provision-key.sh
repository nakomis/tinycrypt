#!/usr/bin/env bash
# CRYPT-15: give a key its IoT certificate and the settings its firmware needs. Safe to re-run:
# an existing certificate is reused unless --new is passed.
#
#   infra/scripts/provision-key.sh [key-name] [--new]       (default key-1)
#
# Writes, all gitignored, under embedded/esp32s3/:
#   certs/device.pem.crt, certs/device.pem.key, certs/AmazonRootCA1.pem
#   sdkconfig.secrets   Wi-Fi, IoT endpoint, Thing name and APNs platform for the firmware
#
# INSECURE, TEST ONLY: the private key is made here and flashed in plain text. The real key keeps
# its private key on the ATECC608 and only ever sends AWS a CSR.
#
# Needs AWS_PROFILE (default nakom.is-sandbox) and NPM_ENVIRONMENT (default sandbox). The house
# Wi-Fi credentials are read from /Nakomis/Wifi/* (as catcam does), via WIFI_PROFILE (default AWS_PROFILE).
set -euo pipefail

key=${1:-key-1}
new=${2:-}
env=${NPM_ENVIRONMENT:-sandbox}
export AWS_PROFILE=${AWS_PROFILE:-nakom.is-sandbox}
export AWS_REGION=${AWS_REGION:-eu-west-2}
wifi_profile=${WIFI_PROFILE:-$AWS_PROFILE}
platform=$([ "$env" = prod ] && echo APNS || echo APNS_SANDBOX)

here=$(cd "$(dirname "$0")" && pwd)
fw="${here}/../../embedded/esp32s3"
certs="${fw}/certs"
mkdir -p "$certs"
chmod 700 "$certs"

thing="tinycrypt-${env}-${key}"
policy="tinycrypt-key-${env}"
aws iot describe-thing --thing-name "$thing" > /dev/null   # fails here if TinycryptKeyStack isn't deployed

if [ "$new" = --new ] || [ ! -s "${certs}/device.pem.key" ]; then
    cert_arn=$(aws iot create-keys-and-certificate --set-as-active \
        --certificate-pem-outfile "${certs}/device.pem.crt" \
        --private-key-outfile "${certs}/device.pem.key" \
        --query certificateArn --output text)
    chmod 600 "${certs}/device.pem.key"
    aws iot attach-policy --policy-name "$policy" --target "$cert_arn"
    aws iot attach-thing-principal --thing-name "$thing" --principal "$cert_arn"
    echo "new certificate ${cert_arn##*/} attached to ${thing}"
else
    echo "reusing the certificate in ${certs}"
fi

[ -s "${certs}/AmazonRootCA1.pem" ] || curl -fsS -o "${certs}/AmazonRootCA1.pem" https://www.amazontrust.com/repository/AmazonRootCA1.pem

endpoint=$(aws iot describe-endpoint --endpoint-type iot:Data-ATS --query endpointAddress --output text)
ssid=$(aws ssm get-parameter --profile "$wifi_profile" --name /Nakomis/Wifi/SSID --query Parameter.Value --output text)
password=$(aws ssm get-parameter --profile "$wifi_profile" --name /Nakomis/Wifi/Password --with-decryption \
    --query Parameter.Value --output text)

umask 077
python3 - "$fw/sdkconfig.secrets" "$ssid" "$password" "$endpoint" "$thing" "$env" "$platform" <<'PY'
import json, sys
path, ssid, password, endpoint, thing, env, platform = sys.argv[1:]
q = json.dumps  # Kconfig strings use C-style escaping, which JSON's quoting matches for these values
lines = [
    "# Written by infra/scripts/provision-key.sh. Gitignored: holds the Wi-Fi password.",
    "CONFIG_TINYCRYPT_NOTIFIER=y",
    f"CONFIG_TINYCRYPT_WIFI_SSID={q(ssid)}",
    f"CONFIG_TINYCRYPT_WIFI_PASSWORD={q(password)}",
    f"CONFIG_TINYCRYPT_IOT_ENDPOINT={q(endpoint)}",
    f"CONFIG_TINYCRYPT_THING_NAME={q(thing)}",
    f"CONFIG_TINYCRYPT_PRESENCE_TOPIC={q(f'tinycrypt/{env}/presence/request')}",
    f"CONFIG_TINYCRYPT_APNS_PLATFORM={q(platform)}",
]
open(path, "w").write("\n".join(lines) + "\n")
PY
echo "wrote ${fw}/sdkconfig.secrets for ${thing} (${endpoint})"
echo "rebuild with a clean sdkconfig so the new values apply: rm embedded/esp32s3/sdkconfig"
