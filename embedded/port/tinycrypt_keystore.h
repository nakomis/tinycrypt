// Keystore selection and the release guard.
//
// The only keystore so far is the INSECURE software key: the master secret
// lives in a plain file (Mac sim) or NVS (ESP32-S3), and the attestation key
// is solo1's published development key. Anyone who can read the device's
// storage can clone every credential. TEST ONLY.
#ifndef TINYCRYPT_KEYSTORE_H
#define TINYCRYPT_KEYSTORE_H

#if defined(TINYCRYPT_INSECURE_SOFT_KEY) && defined(TINYCRYPT_RELEASE)
#error "TINYCRYPT_INSECURE_SOFT_KEY must never be built in a release configuration"
#endif

#if !defined(TINYCRYPT_INSECURE_SOFT_KEY)
#error "No secure keystore backend exists yet (ATECC608 is CRYPT-1..3); build with TINYCRYPT_INSECURE_SOFT_KEY for testing"
#endif

#define TINYCRYPT_SOFT_KEY_BANNER                                              \
    "\n"                                                                       \
    "****************************************************************\n"      \
    "*  TINYCRYPT INSECURE SOFTWARE KEY - TEST ONLY                 *\n"      \
    "*  Secrets are stored unprotected and the attestation key is   *\n"      \
    "*  public. Register ONLY against the read-only test user.      *\n"      \
    "****************************************************************\n"

#endif
