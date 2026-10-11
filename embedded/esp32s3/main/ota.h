// Over-the-air updates through Fleet (CRYPT-14), with the key as the trust anchor.
//
// Fleet only says "please update": ota_update {url, version, sha256} (or url_chunks) on
// tinycrypt/<thing>/commands. The key then
//   1. downloads the image over HTTPS (Amazon Root CA 1) into the spare slot, hashing as it goes;
//   2. refuses it unless its SHA-256 is Fleet's, its RSA-3072 signature is by the key that signed
//      the running app (esp_ota_end, CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT), it says it is
//      tinycrypt at that version, and that version isn't older than the running one;
//   3. asks for presence like a sign-in (button, or the watch showing "Install tinycrypt x.y.z?"),
//      bound to SHA-256("tinycrypt-ota-v1" || image SHA-256 || version) (port/ota_cmd.h);
//   4. only then switches slots and restarts.
// The new image runs a self-test and must reach IoT Core within OTA_VERIFY_DEADLINE_S; until then
// it's on probation, and a failure or crash rolls back to the old one, which reports it.
// Progress goes back on tinycrypt/<thing>/responses as Fleet expects (ota_progress, ota_error,
// ota_complete, and a version message on every connect).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ota_cmd.h"

#define OTA_APPROVAL_MS 60000
#define OTA_VERIFY_DEADLINE_S 120

// At boot, after NVS is up and before the radios start, with the result of the local self-test
// (NVS readable, crypto working). A new image that fails it is rolled back at once.
void ota_boot(bool local_selftest_ok);

// From the notifier (MQTT task): subscribed to commands, so this image can be updated and may be
// kept; reports the version and the outcome of the last update.
void ota_on_connected(void);
// From the notifier (MQTT task): one command from Fleet.
void ota_handle_command(const char *data, size_t len);

// For the CTAP task while idle: true (and what to show) if an update is waiting for approval. Run
// presence_wait_external() with them, then hand the result to ota_approval_done().
bool ota_take_approval(uint8_t hash[32], char label[OTA_LABEL_MAX]);
void ota_approval_done(int result);
