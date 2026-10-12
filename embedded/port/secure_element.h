// The secure element (CRYPT-22): where the key's secrets live and are used.
//
// Two backends: the ATECC608B on the ESP32 (se_atecc.c), and port/se_soft.c, an INSECURE
// software stand-in for the Mac sim and for a board with no chip (its state sits in a file or
// NVS, readable by anyone with the device). crypto_tc.c routes solo1's secret-handling crypto
// through this, so the master secret and the attestation key never reach RAM on the ATECC.
//
// Formats match presence_sig.h and the ATECC: public keys X||Y (64 bytes), signatures r||s
// (64 bytes), over a 32-byte SHA-256 digest. Every call returns 0 on success, -1 on failure.
#ifndef TINYCRYPT_SECURE_ELEMENT_H
#define TINYCRYPT_SECURE_ELEMENT_H

#include <stddef.h>
#include <stdint.h>

#define SE_SERIAL_LEN 9
#define SE_PUB_LEN 64
#define SE_SIG_LEN 64
#define SE_HMAC_LEN 32

// P-256 key slots (atecc_config.h has the ATECC's view of them).
#define SE_SLOT_IOT 0     // IoT Core TLS client key (esp-tls's secure-element path uses slot 0)
#define SE_SLOT_ATTEST 1  // FIDO attestation key

int se_init(void);
const char *se_backend(void);  // "atecc608" or "soft (INSECURE)"
int se_serial(uint8_t serial[SE_SERIAL_LEN]);
int se_random(uint8_t *out, size_t len);

// HMAC-SHA256 keyed with the credential master secret, which never leaves the element. At most
// SE_HMAC_MAX message bytes (the ATECC's SHA command limit).
#define SE_HMAC_MAX 228
int se_hmac(const uint8_t *msg, size_t len, uint8_t mac[SE_HMAC_LEN]);
// Replace the master secret with a new one derived from it and fresh randomness, on the element
// (authenticatorReset). Every credential made before stops working.
int se_master_roll(void);

int se_sign(int slot, const uint8_t digest[32], uint8_t sig[SE_SIG_LEN]);
int se_pubkey(int slot, uint8_t pub[SE_PUB_LEN]);

// The monotonic sign counter: add one and return the new value. It never goes back.
int se_counter_increment(uint32_t *value);

#endif
