# tinycrypt

DIY FIDO2/WebAuthn USB security key on an ESP32-S3, with user presence approved from an Apple Watch
(push notification + BLE challenge signed by the watch's Secure Enclave). Plane project: **CRYPT** (Tiny Crypt).

## Current phase

Feasibility spikes — see `PLAN.md` (gitignored) and the CRYPT epic in Plane. Spike code is throwaway.
The old ATtiny85/U2F scaffold is on the `spike` branch, not `main`.

## Key architectural decisions

- **Portable CTAP2 core + ports**: transport (USB HID | socket for the Mac sim), crypto/keystore
  (ATECC608 | software key), presence (watch approval | button | auto-approve in tests), notifier
  (IoT Core → SNS → APNs). The same core builds for the ESP32-S3 and natively on macOS.
- **Don't write CTAP2 from scratch** — port an existing core. Check the licence first: this repo is
  CC0, so AGPL code (pico-fido, I believe) can't be copied in; SoloKeys solo1 (Apache-2.0/MIT) is the candidate.
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
