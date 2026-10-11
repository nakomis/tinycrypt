# tinycrypt

DIY FIDO2/WebAuthn USB security key on an ESP32-S3, with user presence approved from an Apple Watch
(push notification + BLE challenge signed by the watch's Secure Enclave). Plane project: **CRYPT** (Tiny Crypt).

## Current phase

Feasibility spikes — see `PLAN.md` (gitignored) and the CRYPT epic in Plane. Spike code is throwaway.
The old ATtiny85/U2F scaffold is on the `spike` branch, not `main`.

## Key architectural decisions

- **Portable CTAP2 core + ports**: transport (USB HID | socket for the Mac sim), crypto/keystore
  (ATECC608 | software key), presence, notifier (IoT Core → SNS → APNs). The same core builds for
  the ESP32-S3 and natively on macOS.
- **Presence is button OR watch** (CRYPT-13): a physical button wired to the device and the watch
  approval are both armed, and the first to respond wins. The button must always work on its own;
  the watch is a convenience, not a stronger factor. Tests use auto-approve or deny.
- **Layout** follows Martin's standard: code in top-level component folders, only docs and config at
  the root. `embedded/` holds all firmware (CMake sim build, `port/`, `sim/`, `esp32s3/`, `tests/`,
  `third_party/`); the AWS CDK is in `infra/`, the watch app in its own folder.
- **API domain** (house style): `api.tinycrypt.sandbox.nakomis.com` (sandbox) and
  `api.tinycrypt.nakomis.com` (prod). Derive them from the deploy environment in CDK; don't hard-code.
- **CTAP2 core is SoloKeys solo1** (Apache-2.0 OR MIT), a submodule in `embedded/third_party/solo1`; never
  copy its code into our CC0 tree. Ports override its `__attribute__((weak))` device hooks; the
  ESP32 `main` component is `WHOLE_ARCHIVE` so the strong ones always win.
- **ESP32-S3 gotchas**: the key is on the *native* USB port (the **left** one), not the UART one
  (CH343, right). **The board only connects one USB port at a time**: whichever wins at power-up
  enumerates and the other stays dark, which on 8 Oct looked like a string of charge-only cables.
  To swap, unplug the one you don't want and power-cycle the board. Use UART for flashing and logs
  (`flash-manual.sh`, after BOOT+RST; auto-reset doesn't work on macOS), native for CTAP tests. Short FreeRTOS waits
  need `CONFIG_FREERTOS_HZ=1000` (at 100 Hz `pdMS_TO_TICKS(1)` is 0, which once dropped every
  multi-packet CTAPHID reply). After flashing over the native port, press RST; a USB reset leaves
  it in download mode. `bootloader_random_enable()` feeds the RNG while no radio is on; call
  `bootloader_random_disable()` before starting BLE/Wi-Fi or using the ADC.
- **Presence wait** (CRYPT-13, `port/presence.c`, shared by the sim and the ESP32): every source (button,
  watch over BLE, push notifier) is armed; the first to approve wins. During the wait CTAPHID is handled
  there, never by re-entering solo1 (whose static buffer holds the request's channel): KEEPALIVE to the
  requesting CID (the last report noted before the wait began), CANCEL from that CID ends it, an INIT
  from it aborts the transaction (the core's answer is dropped and the INIT replayed afterwards, so it
  gets its reply), other channels get CHANNEL_BUSY. Ports wire in `presence_note_report`,
  `presence_filter_send` and `presence_take_deferred` (see `presence.h`).
  `presence_is_new_action()` groups U2F's 750 ms re-asks on one channel into one action (one push, one
  challenge); a new channel, a cancel, an approval, a 3 s gap or a retry after a full timeout starts a
  new one, and a cancel also discards the challenge and any unused approval. Sim: `--presence button+watch --watch-after MS --up-timeout MS`.
- **Approval is bound to the request** (CRYPT-18): the wait reassembles the requesting channel's CTAP
  message, hashes it (SHA-256 of the CTAPHID payload) and reads the rpId (CTAP2 only; U2F has just a
  hash). The push says "Sign in to <rp>?" and carries `tc.req`; the challenge is protocol **v1**,
  `"tinycrypt-presence-v1" ‖ nonce32 ‖ reqhash32`; the watch signs only if the challenge's hash matches
  the push it was tapped from, and a different request hash is always a new user action. The app's
  own "Arm in 10 s" test notification has no `tc.req`, so it accepts any v1 challenge.
- **Watch over BLE on the key** (`esp32s3/main/ble_presence.c`, NimBLE, `CONFIG_TINYCRYPT_WATCH_BLE`): the
  key advertises the protocol GATT service; a challenge is
  readable during a wait (and up to 3 s after, for U2F re-asks) and approves once. No pairing yet, so
  every connection is dropped after 10 s and two are allowed (a squatter can't lock the watch out). **Enrol:** hold BOOT 5 s while idle (not at power-up:
  that's download mode) for a 60 s window; the watch taps "Enrol with key". Its public key is in NVS
  (`watch_pub`), so reflashing keeps it. Wi-Fi runs modem sleep alongside BLE (ESP-IDF requires it).
  Measured: tap on Approve → key verified 1.5–1.9 s, app in the background. If no radio starts, the
  ADC entropy source is switched back on. `sdkconfig.defaults` only applies to a fresh `sdkconfig`:
  after changing it, `rm embedded/esp32s3/sdkconfig` and rebuild.
- **Presence contract** (`ctap_user_presence_test`): return only 1 (present), 0 (absent, cancelled
  or timed out) or 2 (check disabled by the request). solo1's U2F code tests `== 0` / `!ret`, so
  any other value (e.g. -1 for cancel) counts as present and signs without a press. (solo1's
  `extensions/wallet.c` too, even on CTAP2 paths, so a cancelled CTAP2 wait reports ACTION_TIMEOUT 0x3A.)
  `test_u2f_respects_denied_presence` guards this.
- **CI**: the "Protect main" ruleset requires a check named exactly `CI Status`.
- **Software key is INSECURE, TEST ONLY**: gated behind `TINYCRYPT_INSECURE_SOFT_KEY`, loud boot
  banner, must not build in release config. Only ever register it against the read-only Identity Center test user.
- **Watch signature convention** (CRYPT-10, proven with a real Secure Enclave key): the watch signs
  with `SecureEnclave.P256.Signing.PrivateKey.signature(for: message)`, which ECDSA-signs
  **SHA-256(message)**, never the message itself. The key side hashes, then verifies the digest
  (`port/presence_sig.c`: micro-ecc; mbedTLS `mbedtls_ecdsa_verify`; ATECC `atcab_verify_extern`/`_stored`).
  Formats are raw, no DER: public key `X‖Y` (64 bytes, `x963Representation` minus the `04`),
  signature `r‖s` (64 bytes, `rawRepresentation`). High-S signatures are accepted, as CryptoKit does.
  The frozen vector is `embedded/tests/vectors/se_p256.json`, made by `watch/tools/se-vector`
  (`swift run --package-path watch/tools/se-vector se-vector`; regenerate only on purpose).
  `se_p256_watch.json` holds two signatures from Martin's real watch over BLE challenges (CRYPT-11).
- **BLE roles** (CRYPT-11): `CBPeripheralManager` is unavailable on watchOS, so the **key is the GATT
  peripheral and the watch the central**. Proximity is the point of BLE: the cloud only sends the
  notification, and the approval itself must come over the air from a watch in range. Reconnect by
  **retrieving the peripheral by identifier** (saved at enrolment): it works from the foreground, the
  background and a cold launch, ~1.4 s median (n=4, worst 1.7 s) from the Approve action handler
  starting to the key verifying. That excludes notification delivery and app launch time, so it is
  a lower bound on what the user feels. **Scanning doesn't work from the background** on watchOS 27
  (never discovered in 20 s, n=2; ~150 ms in the foreground), so scan only while the app is open
  (enrolment, or as a fallback). Key-side timeout must allow ~2 s plus launch time plus slack.
- **Open security questions for the real design** (the spike deliberately ignores them):
  - **Relay**: the watch trusts any peripheral with the service UUID (scan) or the saved identifier
    (retrieve). A relay near the watch can forward the real key's challenge, so proximity to the relay
    is not proximity to the key. Bind the signed message to the key's identity and the pending CTAP
    request (key id plus request hash), and consider requiring a bonded, encrypted link.
  - **Enrolment trust**: enrolment is first-come-first-served, with no confirmation on either side.
    It needs pairing confirmation (e.g. a code shown on the watch and confirmed with the key's button).
  - **Challenge lifetime**: the challenge must be single-use, tied to one CTAP request and expire
    with it; the stand-in only re-arms after a valid response.
  - **Watch-side gate**: the Secure Enclave key has no access control flags, so anyone who can tap
    Approve on the unlocked watch approves. That's the intended presence check, not a second factor.

- **Push path** (CRYPT-12): each key publishes to its own `tinycrypt/{env}/presence/{thing}/request`
  (CRYPT-15: its IoT policy allows no other topic, so one key can't pose as another); an IoT rule's SNS
  action sends it to the `tinycrypt-presence-{env}` topic, and SNS delivers it to the watch's APNs
  platform endpoint. **No Lambda** (Martin's call): sign-ins are rare, so a Lambda would nearly always be
  cold (+~0.9 s measured). The key publishes SNS's JSON message structure itself:
  `{"default": "...", "APNS_SANDBOX": "<APNs payload as a JSON string>"}` (`APNS` in prod), with
  category `PRESENCE`. Measured on a real watch: publish → on the watch 1.3–2.4 s; SNS holds a
  message for 25–44 ms (`dwellTimeMs` in its delivery logs).
- **Notifier firmware** (CRYPT-15, `esp32s3/main/notifier.c`, off unless `CONFIG_TINYCRYPT_NOTIFIER`):
  Wi-Fi (no power saving), SNTP, and esp-mqtt held open to IoT Core with the key's own X.509
  certificate. A presence request is sent once per user action, not per `ctap_user_presence_test` call
  (solo1's U2F path re-asks every 750 ms while the browser retries; without this, one sign-in sent dozens
  of pushes). It is enqueued at QoS 1 for the MQTT task (normally no blocking), and the outbox drops it
  after 10 s unsent, so a reconnect can't deliver a stale prompt. Setup failures switch the notifier off
  rather than aborting; the BOOT button works regardless. `infra/scripts/provision-key.sh [key-N] [--new]`
  makes (or with `--new` replaces, retiring the old one) the certificate and writes
  `esp32s3/certs/<thing>/` and `esp32s3/sdkconfig.secrets` (Wi-Fi from SSM
  `/Nakomis/Wifi/*`), both uncommitted; then `rm esp32s3/sdkconfig` and rebuild. Each key is an IoT
  Thing (`TinycryptKeyStack`) whose policy allows connecting only as itself and publishing only presence
  requests. INSECURE, TEST ONLY: that private key sits in flash; the ATECC608 replaces it. Measured on the
  board: key publish → on the watch 1.3–1.5 s. The app partition is 1.75 MB for the network stack.
- **Push content is device-supplied**: the key writes the alert text, which goes to APNs verbatim.
  Fine while keys are ours; the push carries no authority anyway (approval needs the signed BLE challenge).
- **Stale prompts**: an IoT SNS action can't set message attributes, so there's no
  `AWS.SNS.MOBILE.APNS.TTL`, and APNs may store a push for an offline watch and deliver it late. The
  real guard is the key: its BLE challenge is single-use and expires with the CTAP request, so a late
  Approve finds nothing to sign. The watch should also refuse requests older than ~60 s.
- **SNS setup** CloudFormation can't do: `infra/scripts/sns-platform.sh <device-token>` creates or updates
  the platform application `tinycrypt-watch-{env}` (key from SSM, delivery logging on), the watch's
  endpoint and its subscription. Safe to re-run. The device token comes from the watch app's run log
  (`push token …`). A development-signed watch build only gets pushes from the APNs sandbox.
- **APNs key**: the live team-scoped key is **`25Z7VVWUQW`** (sandbox and production). tinycrypt keeps
  its own copy in SSM at `/tinycrypt/{env}/apns/{team-id,key-id,private-key}`, put there by hand.
  `VH26CFZ5GD`, still named in the cert portal's `/home-certs/*/apns/key-id`, is revoked (APNs says
  `InvalidProviderToken`).
- **Infra deploys**: `cd infra && pnpm run deploy-sandbox` (profile `nakom.is-sandbox`). The account
  comes from the profile, never a literal.

## Architecture diagrams

Source: `docs/architecture/tinycrypt.drawio` — SVG auto-regenerated on commit by `.githooks/pre-commit`.

To activate the hook after cloning:
```bash
git config core.hooksPath .githooks
```
