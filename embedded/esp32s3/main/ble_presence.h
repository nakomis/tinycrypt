// Watch presence over BLE (CRYPT-13): the key is the GATT peripheral, the watch the central.
//
// GATT protocol v0, as the watch app speaks it (watch/TinycryptWatch/Sources/PresenceProtocol.swift):
//   service    30CCC4F1-350D-4FDF-89C5-6120E2CC5044
//   challenge  read   "tinycrypt-presence-v0" || nonce(32); only while a presence wait is armed
//   response   write  r||s (64): the watch Secure Enclave's signature over SHA-256(challenge)
//   enrol      write  X||Y (64): the watch's public key; only while the enrol window is open
//   telemetry  write  timing JSON from the watch, logged
//
// A challenge lives for one user action (solo1's U2F path re-asks every 750 ms; the challenge
// survives that) and is single-use: one valid signature approves once, then the challenge is gone.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "presence.h"

// Starts NimBLE and advertises. Call after notifier_start() (or bootloader_random_disable()):
// the radio replaces the ADC as the RNG's entropy source.
void ble_presence_start(void);

// The watch as a presence source for presence_wait().
extern const presence_source ble_presence_source;

// Opens the enrol window: for `seconds`, the next watch to write its public key is enrolled,
// replacing any previous one.
void ble_presence_open_enrol(uint32_t seconds);

bool ble_presence_enrolled(void);
