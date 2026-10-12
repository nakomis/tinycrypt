// The two X.509 documents the key makes for itself (CRYPT-22), signed by the secure element so
// the private key never leaves it. Shared by the sim and the ESP32 provisioner, and checked by
// tests/test_x509_min.py with the cryptography package.
//
// - The FIDO attestation certificate: self-signed by SE_SLOT_ATTEST, with the subject FIDO's
//   packed attestation asks for (C, O, OU "Authenticator Attestation", CN), CA:FALSE and the
//   AAGUID extension. A Nakomis CA version is CRYPT-25.
// - The IoT Core CSR for SE_SLOT_IOT, CN = the Thing name, for create-certificate-from-csr.
//
// Both return the DER length, or -1 (element error, or cap too small).
#ifndef TINYCRYPT_X509_MIN_H
#define TINYCRYPT_X509_MIN_H

#include <stddef.h>
#include <stdint.h>

#define X509_MIN_MAX 768  // either document fits easily

int x509_attestation_cert(const uint8_t aaguid[16], uint8_t *out, size_t cap);
int x509_csr(const char *cn, uint8_t *out, size_t cap);

#endif
