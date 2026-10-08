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
  `third_party/`); the IoT/SNS CDK will go in `infra/`, the watch app in its own folder.
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
- Watch signs with CryptoKit, which hashes its input with SHA-256 — verifiers must compare against
  the digest, not the raw nonce.

## Architecture diagrams

Source: `docs/architecture/tinycrypt.drawio` — SVG auto-regenerated on commit by `.githooks/pre-commit`.

To activate the hook after cloning:
```bash
git config core.hooksPath .githooks
```
