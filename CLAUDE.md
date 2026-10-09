# tinycrypt

DIY FIDO2/WebAuthn USB security key on an ESP32-S3, with user presence approved from an Apple Watch
(push notification + BLE challenge signed by the watch's Secure Enclave). Plane project: **CRYPT** (Tiny Crypt).

## Current phase

Feasibility spikes — see `PLAN.md` (gitignored) and the CRYPT epic in Plane. Spike code is throwaway.
The old ATtiny85/U2F scaffold is on the `spike` branch, not `main`.

## Key architectural decisions

- **Portable CTAP2 core + ports**: transport (USB HID | socket for the Mac sim), crypto/keystore
  (ATECC608 | software key), presence, notifier (IoT Core → Lambda → APNs). The same core builds for
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
- **ESP32-S3 gotchas**: the key is on the *native* USB port, not the UART one. Short FreeRTOS waits
  need `CONFIG_FREERTOS_HZ=1000` (at 100 Hz `pdMS_TO_TICKS(1)` is 0, which once dropped every
  multi-packet CTAPHID reply). After flashing over the native port, press RST; a USB reset leaves
  it in download mode. `bootloader_random_enable()` feeds the RNG while no radio is on; call
  `bootloader_random_disable()` before starting BLE/Wi-Fi or using the ADC.
- **Presence contract** (`ctap_user_presence_test`): return only 1 (present), 0 (absent, cancelled
  or timed out) or 2 (check disabled by the request). solo1's U2F code tests `== 0` / `!ret`, so
  any other value (e.g. -1 for cancel) counts as present and signs without a press.
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

- **Push path** (CRYPT-12): the key publishes to `tinycrypt/{env}/presence/request`; an IoT rule (adding
  `iotAt`) invokes the relay Lambda (`infra/lambda/push-relay.ts`), which sends an actionable alert
  (category `PRESENCE`) **straight to APNs** over HTTP/2 with a JWT. No SNS: it needs a platform
  application per bundle, holding the key, with no CloudFormation resource, for no gain. The Lambda
  **never retries** (`retryAttempts: 0`, 60 s max event age): a late prompt could be approved for a
  stale request. Measured on a real watch: publish → on the watch ~1.4–2.2 s; publish → APNs accepted
  ~0.53 s warm, ~0.77–1.55 s cold.
- **APNs key**: the live team-scoped key is **`25Z7VVWUQW`** (sandbox and production). tinycrypt keeps
  its own copy in SSM at `/tinycrypt/{env}/apns/{team-id,key-id,private-key}`, put there by hand, so
  the key never passes through CloudFormation. `VH26CFZ5GD`, still named in the cert portal's
  `/home-certs/*/apns/key-id`, is revoked (APNs says `InvalidProviderToken`). The watch's device token is
  `/tinycrypt/{env}/watch/device-token`, read from the app's run log (`push token …`). A
  development-signed watch build only gets pushes from `api.sandbox.push.apple.com`.
- **Infra deploys**: `cd infra && pnpm run deploy-sandbox` (profile `nakom.is-sandbox`). The account
  comes from the profile, never a literal. pnpm 11's `exec esbuild` runs the native binary through node,
  so the Lambda is bundled with the esbuild API in a local bundling hook, not `NodejsFunction`.

## Architecture diagrams

Source: `docs/architecture/tinycrypt.drawio` — SVG auto-regenerated on commit by `.githooks/pre-commit`.

To activate the hook after cloning:
```bash
git config core.hooksPath .githooks
```
