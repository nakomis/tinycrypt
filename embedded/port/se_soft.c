// INSECURE software secure element (CRYPT-22), TEST ONLY: the Mac sim, and the ESP32 with no
// ATECC608B fitted. It behaves like the chip (a master secret used only for HMAC, two P-256 keys
// that only sign, a counter that only goes up) but keeps everything in one blob that the port
// stores in a file or NVS, readable by anyone with the device.
#include <string.h>

#include "device.h"
#include "secure_element.h"
#include "se_soft.h"
#include "sha256.h"
#include "uECC.h"

#define SE_SOFT_MAGIC 0x74635345u  // "tcSE"

typedef struct {
    uint32_t magic;
    uint8_t serial[SE_SERIAL_LEN];
    uint8_t master[32];
    uint8_t keys[2][32];  // SE_SLOT_IOT, SE_SLOT_ATTEST
    uint32_t counter;
} se_soft_state;

static se_soft_state st;
static bool ready;

static int rng(uint8_t *dst, unsigned size)
{
    return ctap_generate_rng(dst, size);
}

static int save(void)
{
    return se_soft_store_save(&st, sizeof(st));
}

int se_init(void)
{
    if (ready)
        return 0;
    uECC_set_rng(rng);
    if (se_soft_store_load(&st, sizeof(st)) == 0 && st.magic == SE_SOFT_MAGIC) {
        ready = true;
        return 0;
    }
    // First start: make the "chip". A blob that exists but doesn't read is not replaced
    // silently: that would lose every credential.
    if (se_soft_store_exists())
        return -1;
    memset(&st, 0, sizeof(st));
    st.magic = SE_SOFT_MAGIC;
    uint8_t pub[SE_PUB_LEN];
    if (!rng(st.serial, sizeof(st.serial)) || !rng(st.master, sizeof(st.master)) ||
        !uECC_make_key(pub, st.keys[SE_SLOT_IOT], uECC_secp256r1()) ||
        !uECC_make_key(pub, st.keys[SE_SLOT_ATTEST], uECC_secp256r1()))
        return -1;
    if (save() != 0)
        return -1;
    ready = true;
    return 0;
}

const char *se_backend(void)
{
    return "soft (INSECURE)";
}

int se_serial(uint8_t serial[SE_SERIAL_LEN])
{
    if (!ready)
        return -1;
    memcpy(serial, st.serial, SE_SERIAL_LEN);
    return 0;
}

int se_random(uint8_t *out, size_t len)
{
    return ctap_generate_rng(out, len) ? 0 : -1;
}

int se_hmac(const uint8_t *msg, size_t len, uint8_t mac[SE_HMAC_LEN])
{
    uint8_t pad[64];
    SHA256_CTX ctx;
    if (!ready || len > SE_HMAC_MAX)
        return -1;

    memset(pad, 0x36, sizeof(pad));
    for (size_t i = 0; i < sizeof(st.master); i++)
        pad[i] ^= st.master[i];
    sha256_init(&ctx);
    sha256_update(&ctx, pad, sizeof(pad));
    sha256_update(&ctx, msg, len);
    sha256_final(&ctx, mac);

    memset(pad, 0x5c, sizeof(pad));
    for (size_t i = 0; i < sizeof(st.master); i++)
        pad[i] ^= st.master[i];
    sha256_init(&ctx);
    sha256_update(&ctx, pad, sizeof(pad));
    sha256_update(&ctx, mac, SE_HMAC_LEN);
    sha256_final(&ctx, mac);

    memset(pad, 0, sizeof(pad));
    return 0;
}

// As the ATECC's DeriveKey roll: new = SHA-256(old || fresh randomness).
int se_master_roll(void)
{
    uint8_t nonce[32];
    SHA256_CTX ctx;
    if (!ready || se_random(nonce, sizeof(nonce)) != 0)
        return -1;
    sha256_init(&ctx);
    sha256_update(&ctx, st.master, sizeof(st.master));
    sha256_update(&ctx, nonce, sizeof(nonce));
    sha256_final(&ctx, st.master);
    return save();
}

static bool slot_ok(int slot)
{
    return ready && (slot == SE_SLOT_IOT || slot == SE_SLOT_ATTEST);
}

int se_sign(int slot, const uint8_t digest[32], uint8_t sig[SE_SIG_LEN])
{
    if (!slot_ok(slot))
        return -1;
    return uECC_sign(st.keys[slot], digest, 32, sig, uECC_secp256r1()) ? 0 : -1;
}

int se_pubkey(int slot, uint8_t pub[SE_PUB_LEN])
{
    if (!slot_ok(slot))
        return -1;
    return uECC_compute_public_key(st.keys[slot], pub, uECC_secp256r1()) ? 0 : -1;
}

int se_counter_increment(uint32_t *value)
{
    if (!ready || st.counter == UINT32_MAX)
        return -1;
    st.counter++;
    if (save() != 0) {
        st.counter--;
        return -1;
    }
    *value = st.counter;
    return 0;
}
