# tinycrypt privacy policy

tinycrypt is a home-made security key and its iPhone, iPad and Apple Watch app, made by Martin Harris
for himself and his family. This page says what the app and the key handle, and where it goes.

## What the app collects

Nothing about you. There are no accounts, no analytics, no advertising and no tracking.

## What the app handles, and where it goes

- **A signing key on your device.** The app makes a key in your iPhone's, iPad's or Apple Watch's
  Secure Enclave. Its private half never leaves the device; the public half is given to your security
  key over Bluetooth when you add it.
- **Your Wi-Fi password.** When you set up your security key's Wi-Fi, the network name and password
  go over an encrypted Bluetooth connection to the key, and are kept on the key. They are never sent
  anywhere else.
- **A notification token.** So your security key can ask you to approve a sign-in, your device's
  Apple Push Notification token goes, through the key, to Amazon Web Services (Amazon SNS, in London,
  in Martin Harris's account), which uses it to send you notifications through Apple. It is deleted
  when you add a different device of the same kind to the key.
- **The site you are signing in to.** A sign-in request names the website (for example
  "github.com"), so the notification can say what you are approving. The request passes through
  Amazon Web Services and Apple's notification service to your device. It is not stored, apart from
  Amazon SNS's delivery logs (kept for diagnostics, in Martin Harris's AWS account).
- **The security key's health.** The key reports its own health (memory, uptime, signal strength,
  whether it is online) to Martin Harris's home monitoring. Nothing about your sign-ins is included.

## Your choices

You can turn notifications off in Settings, or remove the app; the key's button always works without
either. To have your notification token removed from Amazon SNS, contact Martin Harris (below).

## Contact

Martin Harris: [open an issue](https://github.com/nakomis/tinycrypt/issues) on GitHub.

Last updated 17 October 2026.
