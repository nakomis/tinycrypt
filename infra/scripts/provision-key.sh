#!/usr/bin/env bash
# CRYPT-15: give a key its IoT certificate and the settings its firmware needs. Safe to re-run.
#
#   infra/scripts/provision-key.sh [key-name] --csr FILE     the key's own key (CRYPT-22)
#   infra/scripts/provision-key.sh [key-name] [--new]        legacy: a key made here (INSECURE)
#   ... --no-wifi     no Wi-Fi in prov (CRYPT-30): the key's owner sets it from the iPhone app
#
# --csr: the IoT private key lives in the key's secure element (ATECC608B slot 0, or the soft
# stand-in), and only its CSR comes here (esp32s3/tools/atecc_provision.py PORT csr THING FILE). Its
# CN must be the Thing and its signature must verify. IoT Core issues the certificate from it
# (create-certificate-from-csr); then the Thing's other certificates are retired, since the key they
# belong to is no longer used. No private key is written anywhere.
#
# Without --csr (the old way, until a key is reprovisioned): each run makes sure the key's
# certificate is active and attached to its Thing and the policy. --new retires the Thing's existing
# certificates (detach, deactivate, delete) and makes a fresh one; use it if a key's private key may
# have leaked. INSECURE, TEST ONLY: that private key is made here and flashed in plain text.
#
# Writes, never committed, under embedded/esp32s3/certs/<thing>/:
#   device.pem.crt, certificate-arn (and device.pem.key without --csr)
#   prov.bin   the key's prov partition (CRYPT-14): Wi-Fi, IoT endpoint, Thing name, topic, APNs
#              platform and certificate (and the private key without --csr). flash-manual.sh writes
#              it alongside the app; none of it is in the firmware image, so every key runs the same
#              (published) image.
#
# The Wi-Fi password sits in prov.bin and the key's flash either way.
#
# Metrics (CRYPT-28): the key sends its health to the house VictoriaMetrics through vmauth on Cal
# (HOME-437). METRICS_URL (default http://172.29.0.33:8427; "none" for no metrics). vmauth has a user
# per device, named after the Thing, whose password is SSM /vmauth/device-<thing>-password (read via
# METRICS_PROFILE, default nakom.is-prod, where vmauth's credentials live); vmauth pins host_name to
# the user, so a leaked password can only write metrics as this key. A new key needs its user added
# to vmauth_devices in home-servers' cal/ansible/playbook.yml first. If the password can't be read,
# the key is provisioned without metrics (a warning, not a failure).
#
# Needs AWS_PROFILE (default nakom.is-sandbox) and NPM_ENVIRONMENT (default sandbox). The house
# Wi-Fi credentials are read from /Nakomis/Wifi/* (as catcam does), via WIFI_PROFILE (default nakom.is-sandbox: they live only in the sandbox account,
# so a prod key reads them from there too).
set -euo pipefail
umask 077

key=key-1
new=false
wifi=true
csr=""
while [ $# -gt 0 ]; do
    case "$1" in
        --new) new=true ;;
        --no-wifi) wifi=false ;;
        --csr) csr=${2:?--csr needs a file}; shift ;;
        -*) echo "unknown option: $1" >&2; exit 2 ;;
        *) key=$1 ;;
    esac
    shift
done
if [ -n "$csr" ]; then
    [ -s "$csr" ] || { echo "no CSR at $csr" >&2; exit 1; }
    csr=$(cd "$(dirname "$csr")" && pwd)/$(basename "$csr")
fi
env=${NPM_ENVIRONMENT:-sandbox}
export AWS_PROFILE=${AWS_PROFILE:-nakom.is-sandbox}
export AWS_REGION=${AWS_REGION:-eu-west-2}
wifi_profile=${WIFI_PROFILE:-nakom.is-sandbox}
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

if [ -n "$csr" ]; then
    cn=$(openssl req -in "$csr" -noout -subject -nameopt multiline | awk -F' = ' '/commonName/ { print $2 }')
    [ "$cn" = "$thing" ] || { echo "the CSR is for '${cn}', not ${thing}" >&2; exit 1; }
    openssl req -in "$csr" -noout -verify 2> /dev/null || { echo "the CSR's signature doesn't verify" >&2; exit 1; }
    old=$(aws iot list-thing-principals --thing-name "$thing" --query 'principals' --output text)
    aws iot create-certificate-from-csr --set-as-active --certificate-signing-request "file://${csr}" \
        --query '[certificateArn,certificatePem]' --output json > "${dir}/from-csr.json"
    python3 - "$dir" <<'PY'
import json, os, sys
d = sys.argv[1]
arn, pem = json.load(open(os.path.join(d, "from-csr.json")))
open(os.path.join(d, "device.pem.crt"), "w").write(pem)
open(os.path.join(d, "certificate-arn"), "w").write(arn + "\n")
PY
    rm -f "${dir}/from-csr.json" "${dir}/device.pem.key"
    echo "new certificate $(cut -d/ -f2 < "${dir}/certificate-arn") for ${thing}, from its own key's CSR"
    # Only now that the new one exists: the old certificates belong to a key that's no longer used.
    for arn in $old; do
        retire "$arn"
    done
# A certificate already made (from a CSR, so no private key file here, or the old way) is kept:
# a plain re-run only refreshes prov. Only a key with no certificate at all gets an AWS-made key
# pair (INSECURE, the pre-CRYPT-22 way); never recreate one just because device.pem.key is absent,
# or a re-run would quietly put an AWS-generated private key back into prov.
elif [ ! -s "${dir}/certificate-arn" ]; then
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

endpoint=$(aws iot describe-endpoint --endpoint-type iot:Data-ATS --query endpointAddress --output text)
# Through the environment, not argv, so the password never shows up in `ps`.
export TC_SSID= TC_PASSWORD=
$wifi && TC_SSID=$(aws ssm get-parameter --profile "$wifi_profile" --name /Nakomis/Wifi/SSID --query Parameter.Value --output text)
$wifi && TC_PASSWORD=$(aws ssm get-parameter --profile "$wifi_profile" --name /Nakomis/Wifi/Password --with-decryption \
    --query Parameter.Value --output text)

export TC_METRICS_URL=${METRICS_URL:-http://172.29.0.33:8427} TC_METRICS_USER=$thing TC_METRICS_PASS=
if [ "$TC_METRICS_URL" = none ]; then
    TC_METRICS_URL=
elif ! TC_METRICS_PASS=$(aws ssm get-parameter --profile "${METRICS_PROFILE:-nakom.is-prod}" \
        --name "/vmauth/device-${thing}-password" --with-decryption --query Parameter.Value --output text 2> /dev/null); then
    echo "warning: can't read /vmauth/device-${thing}-password (profile ${METRICS_PROFILE:-nakom.is-prod}): no metrics for ${thing}" >&2
    TC_METRICS_URL= TC_METRICS_PASS=
fi

IDF_PATH="${IDF_PATH:-$HOME/esp/v5.5/esp-idf}"
# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" > /dev/null 2>&1   # for its Python, which has the NVS partition generator
trap 'rm -P "${dir}/prov.csv" 2> /dev/null || true' EXIT   # it holds the Wi-Fi password
prov_size=$(awk -F', *' '$1 == "prov" { print $5 }' "${fw}/partitions.csv")
python3 - "$dir" "$endpoint" "$thing" "$env" "$platform" <<'PY'
import csv, os, sys
d, endpoint, thing, env, platform = sys.argv[1:]
ssid, password = os.environ["TC_SSID"], os.environ["TC_PASSWORD"]
if len(ssid.encode()) > 32 or len(password.encode()) > 63:
    sys.exit("Wi-Fi SSID or password too long for ESP-IDF (32 / 63 bytes)")
# Keys and sizes as embedded/esp32s3/main/prov.c reads them.
rows = [("tinycrypt", "namespace", "", "")]
# Without Wi-Fi (--no-wifi), the owner sets it over BLE from the iPhone app (CRYPT-30).
if ssid:
    rows += [("wifi_ssid", "data", "string", ssid), ("wifi_pass", "data", "string", password)]
rows += [
    ("endpoint", "data", "string", endpoint),
    ("thing", "data", "string", thing),
    ("topic", "data", "string", f"tinycrypt/{env}/presence/{thing}/request"),
    ("apns", "data", "string", platform),
    ("cert", "file", "string", os.path.join(d, "device.pem.crt")),
]
# Metrics (CRYPT-28), sizes as prov.h's.
if os.environ["TC_METRICS_URL"]:
    url, user, pw = os.environ["TC_METRICS_URL"], os.environ["TC_METRICS_USER"], os.environ["TC_METRICS_PASS"]
    if len(url.encode()) > 127 or len(user.encode()) > 32 or len(pw.encode()) > 64:
        sys.exit("metrics URL, user or password too long for prov (127 / 32 / 64 bytes)")
    rows += [("metrics_url", "data", "string", url), ("metrics_user", "data", "string", user),
             ("metrics_pass", "data", "string", pw)]
# Only a key provisioned the old way has its private key here (CRYPT-22).
if os.path.exists(os.path.join(d, "device.pem.key")):
    rows.append(("key", "file", "string", os.path.join(d, "device.pem.key")))
with open(os.path.join(d, "prov.csv"), "w", newline="", encoding="utf-8") as f:
    w = csv.writer(f)
    w.writerow(("key", "type", "encoding", "value"))
    w.writerows(rows)
PY
python -m esp_idf_nvs_partition_gen generate "${dir}/prov.csv" "${dir}/prov.bin" "$prov_size" > /dev/null
# Settings once went into the build (CRYPT-15); nothing reads this any more, and it holds the password.
[ -f "${fw}/sdkconfig.secrets" ] && rm -P "${fw}/sdkconfig.secrets"
echo "wrote ${dir}/prov.bin for ${thing} (${endpoint}${TC_METRICS_URL:+; metrics to ${TC_METRICS_URL}}); flash it with esp32s3/flash-manual.sh <port> ${thing}"
