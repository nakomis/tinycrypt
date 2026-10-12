// Keystore selection and the release guard (CRYPT-22).
//
// Exactly one secure element backend (secure_element.h):
// - TINYCRYPT_KEYSTORE_ATECC: the ATECC608B. The master secret, the attestation key, the IoT key
//   and the sign counter live on the chip.
// - TINYCRYPT_INSECURE_SOFT_KEY: port/se_soft.c, the same secrets in a plain file (Mac sim) or
//   NVS (ESP32-S3 without a chip). Anyone who can read the device's storage can clone every
//   credential. TEST ONLY, never in a release.
#ifndef TINYCRYPT_KEYSTORE_H
#define TINYCRYPT_KEYSTORE_H

#if defined(TINYCRYPT_INSECURE_SOFT_KEY) && defined(TINYCRYPT_RELEASE)
#error "TINYCRYPT_INSECURE_SOFT_KEY must never be built in a release configuration"
#endif

#if defined(TINYCRYPT_INSECURE_SOFT_KEY) && defined(TINYCRYPT_KEYSTORE_ATECC)
#error "Choose one keystore: TINYCRYPT_KEYSTORE_ATECC or TINYCRYPT_INSECURE_SOFT_KEY"
#endif

#if !defined(TINYCRYPT_INSECURE_SOFT_KEY) && !defined(TINYCRYPT_KEYSTORE_ATECC)
#error "No keystore chosen: build with TINYCRYPT_KEYSTORE_ATECC (or TINYCRYPT_INSECURE_SOFT_KEY for testing)"
#endif

#define TINYCRYPT_SOFT_KEY_BANNER                                              \
    "\n"                                                                       \
    "****************************************************************\n"      \
    "*  TINYCRYPT INSECURE SOFTWARE KEY - TEST ONLY                 *\n"      \
    "*  The secure element is simulated: its secrets are stored     *\n"      \
    "*  unprotected. Register ONLY against the read-only test user. *\n"      \
    "****************************************************************\n"

#endif
