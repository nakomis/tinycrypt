#!/usr/bin/env bash
# CRYPT-14: make the RSA-3072 key that signs tinycrypt firmware (the secure boot v2 scheme), once
# per environment. Refuses if one exists: replacing it would strand every key in the field, since a
# key only accepts updates signed like the image it's running.
#
#   infra/scripts/ota-signing-key.sh
#   infra/scripts/ota-signing-key.sh --backup    the backup key (CRYPT-34)
#
# --backup: a second key that secured keys' bootloaders also trust (sign.sh --bootloader signs with
# both, and the first boot burns both digests). Never used for routine signing: it's there so an
# update signed with it can revoke the main key if that leaks or is lost. Same storage, named
# signing-key-backup (SSM), {env}-backup (Keychain account) and {env}-backup.pub.pem.
#
# The key lives in SSM Parameter Store, /tinycrypt/{env}/ota/signing-key (SecureString, the master
# copy), and in the login Keychain (service tinycrypt-ota-signing, account {env}, base64) for
# embedded/esp32s3/sign.sh. Only its public half goes in the repo
# (embedded/esp32s3/ota-signing/{env}.pub.pem: commit it); the private PEM made here is deleted, and
# never passed on a command line.
#
# Needs AWS_PROFILE (default nakom.is-sandbox) and NPM_ENVIRONMENT (default sandbox).
set -euo pipefail
umask 077

env=${NPM_ENVIRONMENT:-sandbox}
name=$env suffix=
case "${1:-}" in
    "") ;;
    --backup) name=${env}-backup suffix=-backup ;;
    *) echo "usage: ota-signing-key.sh [--backup]" >&2; exit 2 ;;
esac
export AWS_PROFILE=${AWS_PROFILE:-nakom.is-sandbox}
export AWS_REGION=${AWS_REGION:-eu-west-2}
param="/tinycrypt/${env}/ota/signing-key${suffix}"
service=tinycrypt-ota-signing

if aws ssm get-parameter --name "$param" > /dev/null 2>&1; then
    echo "$param already exists; not replacing it" >&2
    exit 1
fi
if security find-generic-password -s "$service" -a "$name" > /dev/null 2>&1; then
    echo "Keychain already has $service/$name; not replacing it" >&2
    exit 1
fi

IDF_PATH="${IDF_PATH:-$HOME/esp/v5.5/esp-idf}"
# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" > /dev/null

dir=$(mktemp -d)
trap 'rm -P "$dir"/* 2>/dev/null; rmdir "$dir"' EXIT
espsecure.py generate_signing_key --version 2 --scheme rsa3072 "$dir/key.pem" > /dev/null

# The public half first: if anything after SSM fails, sign.sh can still restore the Keychain from SSM.
pub="$(cd "$(dirname "$0")/../.." && pwd)/embedded/esp32s3/ota-signing/${name}.pub.pem"
mkdir -p "$(dirname "$pub")"
espsecure.py extract_public_key --version 2 --keyfile "$dir/key.pem" "$pub" > /dev/null
chmod 644 "$pub"
aws ssm put-parameter --name "$param" --type SecureString --value "file://$dir/key.pem" \
    --description "tinycrypt firmware signing key${suffix:+ (backup)} (RSA-3072, secure boot v2 scheme)" > /dev/null
# Through security's stdin, not its argv (where ps would show it). Base64 needs no quoting.
printf 'add-generic-password -s %s -a %s -T /usr/bin/security -j "%s" -w %s\n' "$service" "$name" \
    "tinycrypt firmware signing key ($name); master copy in SSM $param" "$(base64 < "$dir/key.pem" | tr -d '\n')" |
    security -i

digest=$(espsecure.py digest_sbv2_public_key --keyfile "$dir/key.pem" --output "$dir/digest.bin" > /dev/null && xxd -p -c 64 "$dir/digest.bin")
echo "made $param and Keychain $service/$name; public key digest $digest"
echo "commit $pub"
