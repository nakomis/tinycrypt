# tinycrypt — a DIY FIDO2 security key you approve from your Apple Watch

## Support

If you find this useful, please consider buying me a coffee:

[![Donate with PayPal](https://www.paypalobjects.com/en_GB/i/btn/btn_donate_SM.gif)](https://www.paypal.com/donate?hosted_button_id=Q3BESC73EWVNN&custom=tinycrypt)

## Table of Contents

<!-- toc -->
<!-- tocstop -->

## Overview

A YubiKey-style FIDO2/WebAuthn USB security key built on an ESP32-S3, where the "touch the key"
step happens on your wrist instead: the key sends a push notification to an Apple Watch, you tap
**Approve**, and the watch proves it over Bluetooth before the key signs you in.

The first target is signing into AWS IAM Identity Center with the "Security key" MFA option.

**Hardware:**
- ESP32-S3 (target board: Waveshare ESP32-S3-Zero) — native USB, Wi-Fi, BLE
- Microchip ATECC608 — ECC P-256 secure element (I²C) for key storage, added after the software prototype
- Apple Watch — user-presence approval

**Status:** feasibility spikes. Spike A passed: an ESP32-S3 running the
[SoloKeys solo1](https://github.com/solokeys/solo1) CTAP2 core, with an insecure software key and a
BOOT-button presence check, registered and signed into IAM Identity Center from Chrome. The earlier
ATtiny85 / U2F experiments live on the [`spike`](https://github.com/nakomis/tinycrypt/tree/spike) branch.

## How it fits together

1. The browser asks the key (USB HID, CTAP2) for a WebAuthn assertion.
2. The key sends an approval request: AWS IoT Core → SNS → APNs → actionable watch notification.
3. You tap **Approve**; the watch connects to the key over BLE and signs a fresh nonce with its Secure Enclave key.
4. The key verifies the watch's signature, then signs the WebAuthn assertion and returns it to the browser.

The firmware is split into a portable CTAP2 core with swappable transport, crypto, presence and
notifier back ends, so most of it also runs as a simulator on macOS.

> **Testing builds** can use a software key instead of the ATECC608. That build is deliberately
> insecure, is for testing only, and must only ever be registered against a read-only test user.

## Building and testing

The CTAP2 core is [SoloKeys solo1](https://github.com/solokeys/solo1) (Apache-2.0 OR MIT), pinned as
a submodule in `third_party/solo1` under its own licence. Fetch it and the libraries it builds:

```bash
git submodule update --init third_party/solo1
git -C third_party/solo1 submodule update --init crypto/cifra crypto/micro-ecc crypto/tiny-AES-c tinycbor
```

| Path | What it is |
|---|---|
| `cmake/solo_sources.cmake` | The core's source list, shared by both builds |
| `port/` | App config and the keystore guard (`TINYCRYPT_INSECURE_SOFT_KEY`, refused in release builds) |
| `sim/` | Mac simulator: CTAPHID over UDP, state in a directory |
| `esp32s3/` | ESP-IDF firmware: TinyUSB FIDO HID, NVS soft key, BOOT-button presence |
| `tests/` | python-fido2 tests against the sim, plus `hw_smoke.py` for a real key |

**Mac sim** (needs `brew install libsodium`):

```bash
cmake -S . -B build -DTINYCRYPT_INSECURE_SOFT_KEY=ON && cmake --build build
python3 -m venv tests/.venv && tests/.venv/bin/pip install -r tests/requirements.txt
tests/.venv/bin/python -m pytest tests
build/tinycrypt-sim --presence prompt     # run it by hand; press Enter to approve
```

**ESP32-S3** (ESP-IDF 5.5 in `~/esp/v5.5/esp-idf`):

```bash
esp32s3/build.sh                                    # build
esp32s3/build.sh -p /dev/cu.usbmodemXXXX flash      # flash over the UART port
esp32s3/flash-manual.sh /dev/cu.usbmodemXXXX        # if auto-reset fails: hold BOOT, tap RST, release BOOT first
tests/.venv/bin/python tests/hw_smoke.py            # register + authenticate; press BOOT when asked
```

The key enumerates on the S3's **native** USB port (not the UART one) as
"tinycrypt INSECURE TEST KEY", USB ID 303a:4004.

## Architecture Diagram

`docs/architecture/tinycrypt.drawio` will be the source for the architecture diagram; the SVG is
auto-regenerated on commit by the pre-commit hook in `.githooks/pre-commit`.

To activate the hook after cloning:

```bash
git config core.hooksPath .githooks
```

## Support

If you find this useful, please consider buying me a coffee:

[![Donate with PayPal](https://www.paypalobjects.com/en_GB/i/btn/btn_donate_SM.gif)](https://www.paypal.com/donate?hosted_button_id=Q3BESC73EWVNN&custom=tinycrypt)
