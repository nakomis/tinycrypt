// Boot self-test (CRYPT-10): verify a real Secure Enclave signature on the S3
// with both verifiers we might use: our portable micro-ecc check and ESP-IDF's
// hardware-accelerated mbedTLS. Test firmware only.
#include "se_selftest.h"

#include <string.h>

#include "esp_log.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/sha256.h"
#include "presence_sig.h"
#include "tinycrypt_se_vector.h"

static const char *TAG = "se-vector";

static bool mbedtls_check(void)
{
    uint8_t digest[32];
    uint8_t point[1 + TINYCRYPT_P256_PUB_LEN] = {0x04};
    mbedtls_ecp_group grp;
    mbedtls_ecp_point q;
    mbedtls_mpi r, s;
    int rc;

    memcpy(point + 1, tinycrypt_se_vector_pub, TINYCRYPT_P256_PUB_LEN);
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    // CryptoKit signed SHA-256(message), so verify over the digest.
    rc = mbedtls_sha256(tinycrypt_se_vector_msg, sizeof(tinycrypt_se_vector_msg), digest, 0);
    if (rc == 0) rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (rc == 0) rc = mbedtls_ecp_point_read_binary(&grp, &q, point, sizeof(point));
    if (rc == 0) rc = mbedtls_ecp_check_pubkey(&grp, &q);
    if (rc == 0) rc = mbedtls_mpi_read_binary(&r, tinycrypt_se_vector_sig, 32);
    if (rc == 0) rc = mbedtls_mpi_read_binary(&s, tinycrypt_se_vector_sig + 32, 32);
    if (rc == 0) rc = mbedtls_ecdsa_verify(&grp, digest, sizeof(digest), &q, &r, &s);
    if (rc != 0) ESP_LOGE(TAG, "mbedtls rc=-0x%04x", (unsigned)-rc);

    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&grp);
    return rc == 0;
}

bool se_selftest_run(void)
{
    bool uecc = tinycrypt_presence_sig_verify(tinycrypt_se_vector_pub, tinycrypt_se_vector_msg,
                                              sizeof(tinycrypt_se_vector_msg),
                                              tinycrypt_se_vector_sig) == 1;
    bool mbed = mbedtls_check();
    if (uecc && mbed) {
        ESP_LOGI(TAG, "uECC ok, mbedtls ok (%s)", TINYCRYPT_SE_VECTOR_NAME);
    } else {
        ESP_LOGE(TAG, "uECC %s, mbedtls %s (%s)", uecc ? "ok" : "FAIL", mbed ? "ok" : "FAIL",
                 TINYCRYPT_SE_VECTOR_NAME);
    }
    return uecc && mbed;
}
