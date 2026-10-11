// Watch presence over BLE (CRYPT-13): the key is the GATT peripheral, the watch the central.
//
// GATT protocol v1, as the watch app speaks it (watch/TinycryptWatch/Sources/PresenceProtocol.swift):
//   service    30CCC4F1-350D-4FDF-89C5-6120E2CC5044
//   challenge  read   "tinycrypt-presence-v1" || nonce(32) || request hash(32) (CRYPT-18; v0 had no hash)
//   response   write  r||s (64): the watch Secure Enclave's signature over SHA-256(challenge)
//   enrol      write  X||Y (64): the watch's public key; only while the enrol window is open
//   telemetry  write  timing JSON from the watch, logged
//
// A challenge lives for one user action (presence_is_new_action(): solo1's U2F path re-asks every
// 750 ms, and the challenge survives that, readable for up to PRESENCE_SAME_ACTION_GAP_MS after a
// wait ends). It is single-use: one valid signature approves once. A cancel retires it.
//
// Bonding (CRYPT-19): the watch pairs (LE Secure Connections, Just Works) and bonds while enrolling,
// and its identity address is kept as the one allowed peer. Every presence characteristic needs an
// encrypted link, and the encrypted link must be that watch's: any other device that encrypts is
// disconnected and its bond deleted. A connection is still dropped after 10 s, and two are allowed,
// so a squatter can't lock the watch out.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "presence.h"

// Starts NimBLE and advertises. Call after bootloader_random_disable(): the radio replaces the ADC
// as the RNG's entropy source. Returns false if BLE failed to start (then nothing feeds the RNG).
bool ble_presence_start(void);

// The watch as a presence source for presence_wait().
extern const presence_source ble_presence_source;

// Opens the enrol window: for `seconds`, the next watch to write its public key is enrolled,
// replacing any previous one.
void ble_presence_open_enrol(uint32_t seconds);

bool ble_presence_enrolled(void);

// CRYPT-14: stops NimBLE and the controller to free their RAM while an update downloads (a second
// TLS session needs a contiguous ~17 KB record buffer, on a module with no PSRAM). The button keeps
// working; the watch can't approve until ble_presence_resume(). Returns 1 if BLE was running and
// is now paused (call resume afterwards), 0 if it wasn't running (or couldn't be stopped), and -1
// if a presence wait is in progress, which the watch may still approve: try again once it's over.
int ble_presence_suspend(void);
void ble_presence_resume(void);
