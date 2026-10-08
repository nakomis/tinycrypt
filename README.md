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

**Status:** feasibility spikes. The earlier ATtiny85 / U2F experiments live on the
[`spike`](https://github.com/nakomis/tinycrypt/tree/spike) branch.

## How it fits together

1. The browser asks the key (USB HID, CTAP2) for a WebAuthn assertion.
2. The key sends an approval request: AWS IoT Core → SNS → APNs → actionable watch notification.
3. You tap **Approve**; the watch connects to the key over BLE and signs a fresh nonce with its Secure Enclave key.
4. The key verifies the watch's signature, then signs the WebAuthn assertion and returns it to the browser.

The firmware is split into a portable CTAP2 core with swappable transport, crypto, presence and
notifier back ends, so most of it also runs as a simulator on macOS.

> **Testing builds** can use a software key instead of the ATECC608. That build is deliberately
> insecure, is for testing only, and must only ever be registered against a read-only test user.

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
