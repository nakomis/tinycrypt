// Verifies a presence approval signed by the watch's Secure Enclave (CRYPT-10).
//
// The watch signs with CryptoKit `SecureEnclave.P256.Signing.PrivateKey
// .signature(for: message)`, which ECDSA-signs SHA-256(message), never the
// message itself. So this hashes `msg` and verifies the signature over the digest.
//
// Formats are raw, no DER, matching ATECC608 atcab_verify_extern/_stored:
//   pub: X || Y, 64 bytes (CryptoKit x963Representation without the leading 04)
//   sig: r || s, 64 bytes (CryptoKit rawRepresentation)
//
// High-S signatures are accepted, as CryptoKit accepts them too. That is
// harmless while every challenge uses a fresh nonce.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define TINYCRYPT_P256_PUB_LEN 64
#define TINYCRYPT_P256_SIG_LEN 64

// Returns 1 if `sig` is a valid signature by `pub` over SHA-256(msg), else 0.
int tinycrypt_presence_sig_verify(const uint8_t pub[TINYCRYPT_P256_PUB_LEN], const uint8_t *msg,
                                  size_t msg_len, const uint8_t sig[TINYCRYPT_P256_SIG_LEN]);
