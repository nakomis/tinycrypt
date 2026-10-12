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
- **Approval is bound to the request** (CRYPT-18): solo1's `ctaphid.c` is compiled with
  `ctap_request`/`u2f_request` renamed to our `tc_*` wrappers (`SOLO_CTAPHID_DEFINES`), so the wait
  hashes the exact buffer solo1 is acting on (SHA-256 of the CBOR command byte and map, or the U2F
  APDU); never mirror solo1's CTAPHID reassembly. It reads the rpId (CTAP2 only; U2F has just a hash).
  The push says "Sign in to <rp>?" and carries `tc.req`; the challenge is protocol **v1**,
  `"tinycrypt-presence-v1" ‖ nonce32 ‖ reqhash32`; the watch signs only if the challenge's hash matches
  the push it was tapped from, and a different request hash is always a new user action.
  **Fails closed**: a wait is *bound* (`presence_request_bound`) only for U2F register/authenticate or
  a makeCredential/getAssertion whose rpId appears once and is a plain lower-case host name (≤ 253);
  otherwise (repeated keys, NUL, odd characters, CTAP2 reset, vendor commands) there's no push and no
  BLE challenge: button only. The watch refuses a remote push without a valid `tc.req`; only the
  app's own local "Arm in 10 s" test notification may approve without one.
- **Watch over BLE on the key** (`esp32s3/main/ble_presence.c`, NimBLE, `CONFIG_TINYCRYPT_WATCH_BLE`): the
  key advertises the protocol GATT service; a challenge is
  readable during a wait (and up to 3 s after, for U2F re-asks) and approves once. **Bonded** (CRYPT-19):
  LE Secure Connections, Just Works, bonds in NVS (two at most); the challenge, response and telemetry
  characteristics need an encrypted link, and only the enrolled watch's identity address (`watch_peer`)
  may use it: any other bond is deleted and the link dropped, and repeat pairing is only allowed in the
  enrol window. Connections are still dropped after 10 s and two are allowed (a squatter can't lock the
  watch out). **Enrol:** hold BOOT 5 s while idle (not at power-up: that's download mode) for a 60 s
  window; the watch taps "Enrol with key" and accepts the pairing request (enrolment needs the bonded
  link). A watch enrolled before bonding must enrol again. Its public key is in NVS
  (`watch_pub`), so reflashing keeps it. Wi-Fi runs modem sleep alongside BLE (ESP-IDF requires it).
  Measured: tap on Approve → key verified 1.5–1.9 s, app in the background. If no radio starts, the
  ADC entropy source is switched back on. `sdkconfig.defaults` only applies to a fresh `sdkconfig`:
  after changing it, `rm embedded/esp32s3/sdkconfig` and rebuild. A new option in `Kconfig.projbuild`
  isn't picked up by a plain build either (its `CONFIG_` is undeclared): run `build.sh reconfigure` first.
- **Presence contract** (`ctap_user_presence_test`): return only 1 (present), 0 (absent, cancelled
  or timed out) or 2 (check disabled by the request). solo1's U2F code tests `== 0` / `!ret`, so
  any other value (e.g. -1 for cancel) counts as present and signs without a press. (solo1's
  `extensions/wallet.c` too, even on CTAP2 paths, so a cancelled CTAP2 wait reports ACTION_TIMEOUT 0x3A.)
  `test_u2f_respects_denied_presence` guards this.
- **CI**: the "Protect main" ruleset requires a check named exactly `CI Status`.
- **Secure element** (CRYPT-22, `port/secure_element.h`): the credential master secret, the attestation key,
  the IoT TLS key and the sign counter live in it. Backends: the **ATECC608B** (`esp32s3/main/se_atecc.c`,
  `KEYSTORE=atecc esp32s3/build.sh`) and `port/se_soft.c`, an INSECURE stand-in (sim file, ESP32 NVS) for
  `TINYCRYPT_INSECURE_SOFT_KEY` builds (the default until the chip is proven). **Scheme A** (Martin, 12 Oct):
  per-site keys are HMAC(master, `tc-cred-key` ‖ ctr ‖ credential ID), built in RAM, used once, wiped; so
  until secure boot is on, anyone who can flash the key can clone it (secure boot completes it, later).
  solo1's `crypto.c` is compiled with its secret-handling functions renamed `solo_*` (`SOLO_CRYPTO_DEFINES`)
  and `port/crypto_tc.c` replaces them: HMACs with solo1's MASTER/TRANSPORT keys go to the element with a
  label each (message ≤ 228 bytes, the ATECC's SHA limit), attestation signs on the element, Ed25519 too.
  A CTAP reset rolls the master secret (DeriveKey Roll): solo1 signals it only by new `key_space` bytes,
  so `crypto_load_master_secret` rolls when they change after boot. The attestation certificate is
  self-signed by the element (`port/x509_min.c`, FIDO subject, AAGUID extension; made at first boot,
  kept in NVS); a Nakomis CA version is CRYPT-25. **ATECC configuration** (`atecc_config.h`, checked bit by
  bit by `tests/test_atecc_config.py` against the 608A datasheet's tables): slots 0–7 P-256, external
  sign only, GenKey yes, PrivWrite no (0 IoT, 1 attestation, 2–7 spare for per-site keys later); 9 the
  master secret (never readable, Write never, DeriveKey Roll without MAC, ReqRandom); the rest inert;
  sign counter = Counter[1]. **Provisioning** is a separate image (`build.sh provisioner`,
  `flash-manual.sh --provisioner`, then `esp32s3/tools/atecc_provision.py PORT info|lock|csr`; its console is
  the native USB-Serial-JTAG port, so `ESPTOOL_BEFORE=default_reset` flashes it, and the key image after it,
  with no BOOT+RST):
  `lock` asks for the chip's serial to be typed, writes the configuration, checks it read back, locks it
  by CRC, writes the master secret from the chip's RNG, locks data, GenKeys 0 and 1; `csr` gives the IoT
  CSR for `provision-key.sh key-N --csr FILE` (create-certificate-from-csr; no private key anywhere).
  **TLS:** esp-tls's own secure-element support re-inits and releases cryptoauthlib and routes all ECDSA
  verifies to the chip, so it's off; instead `se_tls.c` hands esp-mqtt a PEM whose scalar is a public
  marker, and `CONFIG_MBEDTLS_ATCA_HW_ECDSA_SIGN` routes every mbedTLS ECDSA signature to our
  `mbedtls_ecdsa_sign`, which signs the marker on the element (slot 0) and refuses anything else (soft
  builds still sign a pre-CRYPT-22 prov key in software, so key-1 stays online until reprovisioned).
  Credential IDs are hashed before the HMAC (a relying party picks their length; the chip takes 228
  bytes at most), and a credential's key is derived inside the sign call, never left loaded (U2F loads
  before asking presence). A missing or corrupt authenticator state rolls the master too
  (`crypto_tc_state_missing`): solo1 then starts with no PIN, so old credentials must not survive.
  Still open until flash encryption: the PIN hash sits in plain NVS, so a flash dump can brute-force a
  short PIN offline. The attestation certificate is checked against the element's key at boot and
  remade after a change of chip or keystore. The provisioner locks slots 0 and 1 after GenKey
  (a locked slot is how a rerun, and `se_init`, know the key is ours; it also stops any firmware
  replacing them). Every chip call holds one mutex (CTAP and MQTT tasks). A board whose ATECC has worked is marked in
  NVS (`ks_atecc`) and refuses a soft-key image, so a wrong update rolls back. Without a working element
  there's no CTAP (LED fault) and, since the IoT TLS key is in the element too, no IoT connection, so no
  updates either: a key whose element fails is reflashed over USB. (A new image without one rolls back.)
- **Soft key is INSECURE, TEST ONLY**: gated behind `TINYCRYPT_INSECURE_SOFT_KEY`, loud boot
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
- **Notifier firmware** (CRYPT-15, `esp32s3/main/notifier.c`, `CONFIG_TINYCRYPT_NOTIFIER`, on by default):
  Wi-Fi (no power saving), SNTP, and esp-mqtt held open to IoT Core with the key's own X.509
  certificate. A presence request is sent once per user action, not per `ctap_user_presence_test` call
  (solo1's U2F path re-asks every 750 ms while the browser retries; without this, one sign-in sent dozens
  of pushes). It is enqueued at QoS 1 for the MQTT task (normally no blocking), and the outbox drops it
  after 10 s unsent, so a reconnect can't deliver a stale prompt. Setup failures switch the notifier off
  rather than aborting; the BOOT button works regardless. `infra/scripts/provision-key.sh [key-N] [--new | --csr FILE]`
  makes (or with `--new` replaces, retiring the old one) the certificate and writes
  `esp32s3/certs/<thing>/` (uncommitted), including `prov.bin`: the key's **`prov` NVS partition**
  (CRYPT-14) with its Wi-Fi (from SSM `/Nakomis/Wifi/*`), endpoint, Thing name, topic, APNs platform,
  certificate and key (`main/prov.c`). `flash-manual.sh <port> [thing]` writes it with the app. None of
  it is in the image, so every key runs the same one; nothing per-key goes in Kconfig any more. Each key
  is an IoT Thing (`TinycryptKeyStack`) whose policy allows connecting only as itself, publishing presence
  requests and Fleet responses, and receiving Fleet commands, all on its own topics. INSECURE, TEST ONLY:
  that private key sits in flash; the ATECC608 replaces it. Measured on the board: key publish → on the
  watch 1.3–1.5 s.
- **OTA through Fleet** (CRYPT-14, `esp32s3/main/ota.c`, `port/ota_cmd.c`): Fleet
  (`app.sandbox.nakomis.com/fleet`, repo `nakomis-fleet`) only triggers; **the key is the trust anchor**.
  Commands on `tinycrypt/<thing>/commands` (`get_version`, `ping`, `ota_update {url, version, sha256}`,
  `url_chunk` 0-based ≤ 10 with `sha256` on one, `ota_cancel`; anything else, e.g. `reboot`/`wipe`, is
  refused), answers on `/responses` (a `version` message on every connect, `ota_progress`, `ota_error`,
  `ota_complete`). The key downloads over HTTPS (Amazon Root CA 1, committed as `esp32s3/AmazonRootCA1.pem`)
  into the spare slot, hashing as it goes, then refuses it unless: SHA-256 = Fleet's; `esp_ota_end` passes
  (signature by the key that signed the **running** image); `esp_app_desc` says `tinycrypt` at that
  version; not a downgrade. Only then a 60 s presence wait, run by the CTAP task while idle
  (`presence_wait_external`: no owning channel, CTAP gets BUSY meanwhile), bound to
  SHA-256(`"tinycrypt-ota-v1"` ‖ image SHA-256 ‖ version) with the push "Install tinycrypt x.y.z?"; the
  watch needs no change. Approved → NVS `ota/target`, switch slot, restart. **Probation:** a new image
  must pass `local_selftest` (NVS, micro-ecc sign/verify) and reach IoT Core within 120 s or it's rolled
  back (bootloader rollback; a crash rolls back too); the old image then reports `rolled back`.
  `CONFIG_TINYCRYPT_OTA_FAIL_SELFTEST` builds a deliberately failing image to test that
  (`publish-ota.sh x.y.z --test-rollback`).
  **RAM is the constraint** (the MINI-1-N8 has no PSRAM): with Wi-Fi, NimBLE and the MQTT TLS session up,
  the largest free block after the S3 handshake was 13 KB, and mbedTLS needs ~16.7 KB for one S3 record.
  So **BLE (host and controller) is paused for the download** (`ble_presence_suspend`, not during a
  presence wait) and resumed before the approval: 109 KB free, largest block 31.7 KB. mbedTLS dynamic
  buffers are on. esp_http_client's TX buffer is sized to the presigned URL (~3 KB with an STS token).
  IoT Core redelivers commands whose PUBACK was slow during a download: a repeated URL is ignored, with
  **no reply on purpose** (it usually lands during the download it started, and an `ota_error` with that
  version would show Fleet a running update as failed; every Fleet trigger presigns a new URL, so a real
  retry never repeats one). **Limits** (CRYPT-21, `ota.h`; RAM only, a reboot clears them), because a
  download pauses BLE and erases the spare slot and an approval blocks CTAP and buzzes the watch:
  the running version or older is refused before downloading (`already running` / `downgrade refused`);
  a download must finish in `CONFIG_TINYCRYPT_OTA_DOWNLOAD_DEADLINE_S` (300; `publish-ota.sh` refuses
  < 120) and move 32 KB every 30 s; a failed one (cancel included) refuses updates for 30 s, doubling
  to 15 min (`busy: retry in N s`); an image whose approval timed out or was rejected is refused for
  10 min (`declined recently`), and no prompt follows an unapproved one within 60 s. These bound what
  updates can cost the user; they are **not** a defence against a hostile publisher, who can keep the
  back-off at its maximum with a junk URL every 15 min and so hold real updates off (it could already
  do that with `ota_cancel`). Approval is the defence that matters. A chunk set is capped at
  `OTA_URL_MAX` as it arrives, and one older than 60 s is freed by the next command. The LED pulses **amber** while an update
  waits for approval (blue is a sign-in). An `ota_cancel` sticks until the approval wait it's for ends, and
  wins over a racing approval. **NVS is never erased** to get past an init error (counter, resident keys,
  watch): a new image that can't read it rolls back. Failures report their cause (TLS error, errno,
  heap) in `ota_error`, since the log is out of reach on the native port: for a serial log, swap to the
  UART port (the OTA path doesn't need USB HID).
- **Flash layout** (`partitions.csv`, fits 4 MB; the boards have 16 MB): `nvs` and `phy` at IDF's
  single-app offsets (so NVS survived the move), `otadata`, `prov`, two 1.875 MB app slots, coredump.
  Moving from the old single-app layout needs one wired flash (bootloader with rollback included).
- **Signed images** (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`, RSA-3072, the secure boot v2 scheme):
  every app image is signed, dev builds included, because an update must be signed like the running
  one. `build.sh` builds then `sign.sh` signs (never `idf.py flash`: unsigned). The key is in SSM
  `/tinycrypt/{env}/ota/signing-key` (master) and the Keychain (`tinycrypt-ota-signing`/`{env}`, base64);
  made once by `infra/scripts/ota-signing-key.sh` (sandbox public key digest `2d1d16f2…`). Replacing it
  strands every key in the field. Without secure boot this protects updates, not a USB cable. The
  public half is committed (`esp32s3/ota-signing/{env}.pub.pem`): `sign.sh`, `flash-manual.sh` and
  `publish-ota.sh` check images against it, so one signed by another key (which would boot, then refuse
  every update) is refused, and an already-signed image needs no Keychain read. The private key never
  goes on a command line: the Keychain item is added through `security -i` (stdin), ACL
  `-T /usr/bin/security` (explicit default; `-T ""` would prompt on every read and block unattended builds).
- **Publishing is CI's** (CRYPT-20, `.github/workflows/ci.yml`, the `firmware-*` jobs). A PR that touches
  `embedded/` gets the next version from the shared deployment tracker (`nakomis-infra`'s
  `compute-version`, project `tinycrypt`, bumped from sandbox's latest; `--bump-minor` or `--bump-major`
  alone on a line of the PR description), is built **once, unsigned** (in `FIRMWARE_KEYSTORE`'s keystore),
  then signed with the sandbox key and published to sandbox (`tools/ci-publish.sh`), and recorded. Every push
  to a PR is a new version. **The merge** publishes the same unsigned bytes, signed with the prod key, to
  prod: the job finds the PR's last successful run, and refuses unless main's tree is the one that build
  came from (if main moved on, re-run the PR's CI). **Prod takes only ATECC builds**, so while
  `FIRMWARE_KEYSTORE` is `soft` a merge publishes nothing to prod. The signing keys come from SSM through
  `TinycryptGithubCiStack`'s roles (`nakomis-tinycrypt-github-ci-{env}`, repo variables
  `AWS_{SANDBOX,PROD}_ROLE_ARN`): sandbox trusts any ref of this repo, prod only `main` (only Martin merges).
  RSA-PSS signatures are salted, so a re-run compares the `unsigned-sha256` metadata. `version.txt` only
  seeds the tracker and labels local builds. **By hand**, only when CI can't: bump `version.txt` above the
  tracker, commit, `esp32s3/publish-ota.sh x.y.z`. Both paths run `tools/image-checks.sh` (version,
  configuration, no per-key strings, signature against the committed public key). Then update from Fleet's
  UI. The Thing's attributes (`capabilities=ota,ota-signed,ota-approval`) make Fleet refuse unsigned builds
  and expect approval.
- **Watch app to TestFlight is CI's** (CRYPT-27, the `watch-testflight` job): a PR touching `watch/TinycryptWatch/`
  or `watch/ci/` gets the next version from the tracker (project `tinycrypt-watch`; build number = run number
  and attempt), is archived with **manual** distribution signing (`watch/ci/ci-signing.xcconfig`: CI's own
  Apple Distribution certificate, and the profiles "tinycrypt CI iOS stub App Store" / "tinycrypt CI watch App
  Store", told apart by SDK) and uploaded by `xcodebuild -exportArchive` (`watch/ci/ExportOptions.plist`).
  Secrets in sandbox SSM `/tinycrypt/sandbox/apple/*`: `asc-key-id`, `asc-issuer-id`, `asc-key` (the App
  Manager API key's `.p8`) and `dist-key` (the certificate's private key; the certificate itself is fetched
  from the API by public key, `watch/ci/asc.py`, so a renewal needs no CI change). Your own "Distribution
  Managed" certificate is Xcode's cloud signing and can't be exported, hence CI's own. Certificate and
  profiles expire **12 Oct 2027**. A TestFlight build is distribution-signed (`aps-environment` production),
  so it gets **prod** APNs pushes: register its token with `sns-platform.sh` in prod, not sandbox; Xcode
  development builds stay on the APNs sandbox. App Store Connect app id 6821087095.
- **Metrics** (CRYPT-28, `esp32s3/main/metrics.c`, `CONFIG_TINYCRYPT_METRICS`): every 60 s the key POSTs one
  InfluxDB line (`port/metrics_line.c`) to vmauth on Cal (HOME-437, `http://172.29.0.33:8427/write`, plain
  HTTP + basic auth, no TLS on the LAN), landing in the house VictoriaMetrics as `tinycrypt_heap_free`,
  `_heap_largest`, `_heap_min`, `_uptime_s`, `_rssi`, `_mqtt`, `_watch_enrolled`, `_ble_connections` (label
  `version`). **Nothing about sign-ins is sent.** vmauth has a user per device named after the Thing, password
  in prod SSM `/vmauth/device-<thing>-password`, and pins `host_name` server-side, so the password (in plain
  flash, `prov`'s `metrics_*`, written by `provision-key.sh`; `METRICS_URL=none` for none) can only write as
  that key. A new key needs its user in home-servers' `vmauth_devices` first. Best effort: a minute is skipped
  while Wi-Fi is down, an update runs, or the largest free block is under 8 KB; failures are dropped. Query
  it read-only with `ssh cal.local "docker exec victoriametrics wget -qO- 'http://127.0.0.1:8428/api/v1/query?query=…'"`.
  Measured on key-1 at boot with BLE and MQTT up: 66.8 KB free, largest block 31.7 KB.
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
- **Infra deploys**: `cd infra && pnpm run deploy-sandbox` (profile `nakom.is-sandbox`), or `diff-prod` /
  `deploy-prod` (profile `nakom.is-prod`; asks before IAM changes, and only with Martin's OK). The account
  comes from the profile, never a literal. **Keys are listed per environment in `infra/keys.json`**
  (CRYPT-26): a deploy declares every key, so add one there before `provision-key.sh`; a key missing from
  the list has its Thing deleted. **Prod** (CRYPT-26) is set up but unused: no key goes there until the
  ATECC, the PCB, secure boot and flash encryption are done. Prod pushes need a distribution-signed
  (TestFlight) watch build, since a development build only gets APNs sandbox pushes (CRYPT-27); until then
  prod's platform app has no endpoint (`sns-platform.sh --app-only`). The house Wi-Fi is only in sandbox SSM.

## Architecture diagrams

Source: `docs/architecture/tinycrypt.drawio` — SVG auto-regenerated on commit by `.githooks/pre-commit`.

To activate the hook after cloning:
```bash
git config core.hooksPath .githooks
```
