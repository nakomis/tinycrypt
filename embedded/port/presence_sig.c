#include "presence_sig.h"

#include "sha256.h"
#include "uECC.h"

int tinycrypt_presence_sig_verify(const uint8_t pub[TINYCRYPT_P256_PUB_LEN], const uint8_t *msg,
                                  size_t msg_len, const uint8_t sig[TINYCRYPT_P256_SIG_LEN])
{
    if (pub == NULL || sig == NULL || (msg == NULL && msg_len != 0)) {
        return 0;
    }
    const struct uECC_Curve_t *curve = uECC_secp256r1();
    // uECC_verify assumes a valid point; check it rather than trust the caller.
    if (!uECC_valid_public_key(pub, curve)) {
        return 0;
    }

    uint8_t digest[32];
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, msg, msg_len);
    sha256_final(&ctx, digest);

    return uECC_verify(pub, digest, sizeof(digest), sig, curve) == 1;
}
