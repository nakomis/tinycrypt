// TLS client authentication with the secure element's IoT key (CRYPT-22).
//
// esp-mqtt and esp-tls only take a client key as PEM, and esp-tls's own secure-element support
// re-initialises and releases cryptoauthlib behind our back (and routes every ECDSA verify to
// the chip). So instead:
// - se_tls_key_pem() makes a PEM "private key" whose public half is the element's IoT key and
//   whose private scalar is a fixed, public marker. It is not a secret and signs nothing itself.
// - CONFIG_MBEDTLS_ATCA_HW_ECDSA_SIGN makes mbedTLS call mbedtls_ecdsa_sign() below for every
//   ECDSA signature. Given the marker it signs on the element (se_sign, SE_SLOT_IOT, under the
//   element's lock); given anything else it refuses, so no ECDSA signature is ever made in
//   software with a real key either. The firmware makes none: credentials sign with micro-ecc.
//   The one exception is the INSECURE soft-key build with a pre-CRYPT-22 key still in prov
//   (prov.h): that one is signed with micro-ecc, so the key stays online until reprovisioned.
#include "se_tls.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/pk.h"
#include "mbedtls/error.h"
#include "mbedtls/sha256.h"
#include "secure_element.h"
#include "uECC.h"
#include "sdkconfig.h"

#ifndef CONFIG_MBEDTLS_ATCA_HW_ECDSA_SIGN
#error "se_tls.c needs CONFIG_MBEDTLS_ATCA_HW_ECDSA_SIGN (sdkconfig.defaults)"
#endif

static const char *TAG = "se-tls";

#define PEM_MAX 512

static void marker(uint8_t out[32])
{
    static const char label[] = "tinycrypt: the IoT key is in the secure element, slot 0";
    mbedtls_sha256((const unsigned char *)label, sizeof(label) - 1, out, 0);
}

char *se_tls_key_pem(void)
{
    uint8_t d[32], pub[1 + SE_PUB_LEN] = {0x04};
    char *pem = NULL;
    mbedtls_pk_context pk;
    int rc;

    if (se_pubkey(SE_SLOT_IOT, pub + 1) != 0)
        return NULL;
    marker(d);
    mbedtls_pk_init(&pk);
    rc = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    mbedtls_ecp_keypair *kp = mbedtls_pk_ec(pk);
    if (rc == 0)
        rc = mbedtls_ecp_read_key(MBEDTLS_ECP_DP_SECP256R1, kp, d, sizeof(d));
    if (rc == 0)
        rc = mbedtls_ecp_point_read_binary(&kp->MBEDTLS_PRIVATE(grp), &kp->MBEDTLS_PRIVATE(Q), pub, sizeof(pub));
    if (rc == 0 && (pem = malloc(PEM_MAX)) == NULL)
        rc = -1;
    if (rc == 0)
        rc = mbedtls_pk_write_key_pem(&pk, (unsigned char *)pem, PEM_MAX);
    mbedtls_pk_free(&pk);
    if (rc != 0) {
        ESP_LOGE(TAG, "key PEM: -0x%04x", (unsigned)-rc);
        free(pem);
        return NULL;
    }
    return pem;
}

int mbedtls_ecdsa_sign(mbedtls_ecp_group *grp, mbedtls_mpi *r, mbedtls_mpi *s, const mbedtls_mpi *d,
                       const unsigned char *buf, size_t blen,
                       int (*f_rng)(void *, unsigned char *, size_t), void *p_rng)
{
    uint8_t want[32], got[32], digest[32] = {0}, sig[SE_SIG_LEN];
    (void)f_rng;
    (void)p_rng;

    if (grp->id != MBEDTLS_ECP_DP_SECP256R1 || mbedtls_mpi_write_binary(d, got, sizeof(got)) != 0)
        return MBEDTLS_ERR_ECP_FEATURE_UNAVAILABLE;
    // ECDSA uses the leftmost 256 bits of a longer hash (SHA-384 with P-256).
    if (blen >= sizeof(digest))
        memcpy(digest, buf, sizeof(digest));
    else
        memcpy(digest + sizeof(digest) - blen, buf, blen);

    marker(want);
    if (memcmp(got, want, sizeof(want)) == 0) {
        if (se_sign(SE_SLOT_IOT, digest, sig) != 0)
            return MBEDTLS_ERR_PLATFORM_HW_ACCEL_FAILED;
    } else {
#ifdef TINYCRYPT_INSECURE_SOFT_KEY
        int ok = uECC_sign(got, digest, sizeof(digest), sig, uECC_secp256r1());
        memset(got, 0, sizeof(got));
        if (!ok)
            return MBEDTLS_ERR_PLATFORM_HW_ACCEL_FAILED;
#else
        ESP_LOGE(TAG, "refusing an ECDSA signature that isn't the element's IoT key");
        return MBEDTLS_ERR_ECP_FEATURE_UNAVAILABLE;
#endif
    }
    int rc = mbedtls_mpi_read_binary(r, sig, 32);
    if (rc == 0)
        rc = mbedtls_mpi_read_binary(s, sig + 32, 32);
    return rc;
}
