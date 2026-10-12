# What a published firmware image must pass (CRYPT-14, CRYPT-21), shared by publish-ota.sh (by hand)
# and tools/ci-publish.sh (CI, CRYPT-20) so both apply the same rules. Source it; each check prints
# why and exits on failure. Needs esptool and espsecure on PATH (ESP-IDF's export.sh, or pip).

# The image says this version.
check_image_version() { # bin version
    local built
    built=$(python -m esptool --chip esp32s3 image_info --version 2 "$1" 2> /dev/null | awk -F': *' '/^App version/ { print $2 }')
    [ "$built" = "$2" ] || { echo "the image says version '$built', not $2" >&2; exit 1; }
}

# The build's configuration (its sdkconfig.h) is one that may be published.
check_image_config() { # sdkconfig.h test_rollback(true|false)
    local config=$1 test_rollback=$2 option deadline
    # It can be updated (the notifier), it's checked on probation (rollback), and it checks the
    # next one's signature.
    for option in CONFIG_TINYCRYPT_NOTIFIER CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT; do
        grep -q "^#define ${option} 1$" "$config" || { echo "the build lacks ${option}: not publishing" >&2; exit 1; }
    done
    if grep -q '^#define CONFIG_TINYCRYPT_OTA_FAIL_SELFTEST 1$' "$config" && ! $test_rollback; then
        echo "this build fails its self-test on purpose (CONFIG_TINYCRYPT_OTA_FAIL_SELFTEST): not publishing" >&2
        exit 1
    fi
    # A short download deadline is only for testing it (CRYPT-21): published, it would fail real updates.
    deadline=$(awk '/^#define CONFIG_TINYCRYPT_OTA_DOWNLOAD_DEADLINE_S / { print $3 }' "$config")
    [ "${deadline:-0}" -ge 120 ] || { echo "the download deadline is ${deadline:-unset} s (< 120): not publishing" >&2; exit 1; }
}

# No secrets or per-key settings (they live in the prov partition): no Thing name or endpoint, and
# none of the extra needles (the Wi-Fi SSID, lines of a provisioned key's certificate). Not a bare
# "PRIVATE KEY": mbedTLS's PEM parser has that string in every image.
check_image_strings() { # bin env [needle...]
    local bin=$1 env=$2 needle
    shift 2
    for needle in "tinycrypt-${env}-key" "-ats.iot." "$@"; do
        [ -n "$needle" ] || continue
        if LC_ALL=C grep -qaF -- "$needle" "$bin"; then
            echo "the image contains a per-key string (\"${needle:0:12}...\"): not publishing" >&2
            exit 1
        fi
    done
}

# Signed by this environment's key, or every key in the field refuses it (a key only accepts
# updates signed like the image it runs).
check_image_signed() { # bin env
    local pub
    pub="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/ota-signing/$2.pub.pem"
    espsecure.py verify_signature --version 2 --keyfile "$pub" "$1" > /dev/null 2>&1 || {
        echo "the image isn't signed by the $2 key (ota-signing/$2.pub.pem): not publishing" >&2
        exit 1
    }
}
