// solo1's secret-handling crypto, routed through the secure element (CRYPT-22).
//
// solo1's crypto.c keeps a master secret and a transport secret in RAM, loaded from the
// authenticator state (NVS), and its development attestation key is published. Here none of
// that exists: solo_sources.cmake compiles crypto.c with the functions below renamed to solo_*
// (SOLO_CRYPTO_DEFINES), and these take their place.
//
// - HMACs keyed with CRYPTO_MASTER_KEY, CRYPTO_TRANSPORT_KEY or CRYPTO_TRANSPORT_KEY2 run on the
//   element (se_hmac), each with its own label in front of the message. The message is buffered
//   here and sent in one go at the end. HMACs with ordinary keys (the PIN token, ECDH secrets) go
//   to solo1 unchanged.
// - A credential's private key is HMAC(master, "tc-cred-p256" || counter || SHA-256(ID)) (its
//   own label for Ed25519): built in RAM inside the sign call, used once, wiped. That is scheme A: whoever can run code on the key can
//   derive them, so it relies on secure boot (Martin's later call).
// - The attestation key never leaves the element (se_sign on SE_SLOT_ATTEST).
// - The authenticator state's key_space no longer holds secrets, but solo1 still uses it to
//   signal a reset: ctap_reset() writes fresh random key_space and loads it. So the first
//   crypto_load_master_secret after boot only notes key_space's digest, and any later load with
//   different bytes rolls the master secret on the element, which kills every credential as a
//   reset should. A port whose state was missing at boot (erased, unreadable) says so with
//   crypto_tc_state_missing(), and then the first load rolls too: solo1 starts a fresh state with
//   no PIN, and the old credentials must not survive into it (that would bypass the PIN). (A power cut between the state write and the roll would let credentials
//   survive that one reset.) crypto_reset_master_secret rolls too.
//
// Anything unexpected (a secret key where none belongs, a sign with nothing loaded, an element
// error) aborts: failing closed beats signing with the wrong key.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sodium/crypto_sign_ed25519.h>

#include "crypto.h"
#include "crypto_tc.h"
#include "storage.h"
#include "secure_element.h"
#include "sha256.h"
#include "uECC.h"

void solo_crypto_sha256_init(void);
void solo_crypto_sha256_update(uint8_t *data, size_t len);
void solo_crypto_sha256_final(uint8_t *hash);
void solo_crypto_sha256_hmac_init(uint8_t *key, uint32_t klen, uint8_t *hmac);
void solo_crypto_sha256_hmac_final(uint8_t *key, uint32_t klen, uint8_t *hmac);
void solo_crypto_aes256_init(uint8_t *key, uint8_t *nonce);

// Labels: one per use of the master secret, so no two uses can be confused. Fixed length.
#define LABEL_LEN 16
static const uint8_t LABEL_CRED_P256[LABEL_LEN] = "tc-cred-p256\0\0\0";
static const uint8_t LABEL_CRED_ED25519[LABEL_LEN] = "tc-cred-ed25519";
static const uint8_t LABEL_MASTER[LABEL_LEN] = "tc-hmac-master\0";
static const uint8_t LABEL_TRANSPORT[LABEL_LEN] = "tc-hmac-transp1";
static const uint8_t LABEL_TRANSPORT2[LABEL_LEN] = "tc-hmac-transp2";

// The ATECC's SHA command takes at most 228 message bytes (SE_HMAC_MAX). The MACs solo1 asks for
// are fixed-size and well under that; credential IDs, which a relying party chooses (solo1 takes
// "custom" ones up to 255 bytes), are hashed first (derive()), never put in whole.
#define HMAC_BUF_MAX SE_HMAC_MAX

static struct {
    bool active;
    size_t len;
    uint8_t buf[HMAC_BUF_MAX];
} hmac;

// What the next signature uses. For a credential only the SHA-256 of its ID is kept: the private
// key is derived inside the sign call and wiped there, so it is never left in RAM (U2F loads the
// key before asking for presence, and a denied request never signs).
static enum { KEY_NONE, KEY_ATTEST, KEY_P256, KEY_ED25519 } loaded = KEY_NONE;
static uint8_t loaded_id[32];

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--)
        *v++ = 0;
}

static void fail(const char *why)
{
    fprintf(stderr, "crypto_tc: %s\n", why);
    abort();
}

static const uint8_t *label_for(const uint8_t *key)
{
    if (key == CRYPTO_MASTER_KEY)
        return LABEL_MASTER;
    if (key == CRYPTO_TRANSPORT_KEY)
        return LABEL_TRANSPORT;
    if (key == CRYPTO_TRANSPORT_KEY2)
        return LABEL_TRANSPORT2;
    return NULL;
}

static void hmac_add(const uint8_t *data, size_t len)
{
    if (len > HMAC_BUF_MAX - hmac.len)
        fail("HMAC message too long");
    memcpy(hmac.buf + hmac.len, data, len);
    hmac.len += len;
}

static void hmac_begin(const uint8_t *label)
{
    if (hmac.active)
        fail("nested secure-element HMAC");
    hmac.active = true;
    hmac.len = 0;
    hmac_add(label, LABEL_LEN);
}

static void hmac_end(uint8_t out[SE_HMAC_LEN])
{
    int rc = se_hmac(hmac.buf, hmac.len, out);
    wipe(&hmac, sizeof(hmac));
    if (rc != 0)
        fail("secure-element HMAC failed");
}

// ---- SHA-256, with the secure-element HMAC session in mind ------------------------------------

void crypto_sha256_init(void)
{
    if (hmac.active)
        fail("SHA-256 restarted inside a secure-element HMAC");
    solo_crypto_sha256_init();
}

void crypto_sha256_update(uint8_t *data, size_t len)
{
    if (hmac.active)
        hmac_add(data, len);
    else
        solo_crypto_sha256_update(data, len);
}

void crypto_sha256_final(uint8_t *hash)
{
    if (hmac.active)
        fail("SHA-256 finished inside a secure-element HMAC");
    solo_crypto_sha256_final(hash);
}

void crypto_sha256_update_secret(void)
{
    fail("crypto_sha256_update_secret: the master secret is not readable");
}

void crypto_sha256_hmac_init(uint8_t *key, uint32_t klen, uint8_t *out)
{
    const uint8_t *label = label_for(key);
    if (label)
        hmac_begin(label);
    else
        solo_crypto_sha256_hmac_init(key, klen, out);
}

void crypto_sha256_hmac_final(uint8_t *key, uint32_t klen, uint8_t *out)
{
    if (hmac.active) {
        // solo1 sometimes names a different secret key in final than in init (TRANSPORT_KEY2);
        // the session started with init decides. An ordinary key here is a mismatch.
        if (!label_for(key))
            fail("HMAC started with a secret key, finished with another");
        hmac_end(out);
    } else {
        if (label_for(key))
            fail("HMAC finished with a secret key it didn't start with");
        solo_crypto_sha256_hmac_final(key, klen, out);
    }
}

void crypto_aes256_init(uint8_t *key, uint8_t *nonce)
{
    if (label_for(key))
        fail("AES with a secret key is not supported");
    solo_crypto_aes256_init(key, nonce);
}

// ---- master secret ----------------------------------------------------------------------------

static bool roll_on_first_load;

void crypto_tc_state_missing(void)
{
    roll_on_first_load = true;
}

void crypto_load_master_secret(uint8_t *key)
{
    static bool seen;
    static uint8_t seen_digest[32];
    uint8_t digest[32];
    SHA256_CTX ctx;

    sha256_init(&ctx);
    sha256_update(&ctx, key, KEY_SPACE_BYTES);
    sha256_final(&ctx, digest);
    bool roll = seen ? memcmp(digest, seen_digest, sizeof(digest)) != 0 : roll_on_first_load;
    if (roll && se_master_roll() != 0)
        fail("could not roll the master secret");
    roll_on_first_load = false;
    memcpy(seen_digest, digest, sizeof(digest));
    seen = true;
}

void crypto_reset_master_secret(void)
{
    if (se_master_roll() != 0)
        fail("could not roll the master secret");
}

// ---- credential keys --------------------------------------------------------------------------

// SHA-256 of the credential ID (data || data2): a fixed-size input to the element, whatever length
// the relying party chose.
static void id_digest(const uint8_t *data, int len, const uint8_t *data2, int len2, uint8_t out[32])
{
    SHA256_CTX ctx;
    if (len < 0 || len2 < 0 || (len > 0 && !data) || (len2 > 0 && !data2))
        fail("bad credential ID");
    sha256_init(&ctx);
    if (len > 0)
        sha256_update(&ctx, data, (size_t)len);
    if (len2 > 0)
        sha256_update(&ctx, data2, (size_t)len2);
    sha256_final(&ctx, out);
}

// HMAC(master, label || counter || SHA-256(credential ID)). Each algorithm has its own label, so
// one ID never gives the same secret to both. The counter only moves past 0 for a P-256 scalar
// that is 0 or >= n, about one credential in 2^32.
static void derive(const uint8_t *label, uint8_t ctr, const uint8_t id[32], uint8_t out[32])
{
    hmac_begin(label);
    hmac_add(&ctr, 1);
    hmac_add(id, 32);
    hmac_end(out);
}

static void derive_p256(const uint8_t id[32], uint8_t priv[32], uint8_t pub[64])
{
    for (unsigned ctr = 0; ctr < 256; ctr++) {
        derive(LABEL_CRED_P256, (uint8_t)ctr, id, priv);
        if (uECC_compute_public_key(priv, pub, uECC_secp256r1()))
            return;
    }
    fail("no valid P-256 key");
}

void generate_private_key(uint8_t *data, int len, uint8_t *data2, int len2, uint8_t *privkey)
{
    uint8_t id[32], pub[64];
    id_digest(data, len, data2, len2, id);
    derive_p256(id, privkey, pub);
}

void crypto_ecc256_derive_public_key(uint8_t *data, int len, uint8_t *x, uint8_t *y)
{
    uint8_t id[32], priv[32], pub[64];
    id_digest(data, len, NULL, 0, id);
    derive_p256(id, priv, pub);
    wipe(priv, sizeof(priv));
    memcpy(x, pub, 32);
    memcpy(y, pub + 32, 32);
}

void crypto_ecc256_load_key(uint8_t *data, int len, uint8_t *data2, int len2)
{
    id_digest(data, len, data2, len2, loaded_id);
    loaded = KEY_P256;
}

void crypto_ecc256_load_attestation_key(void)
{
    loaded = KEY_ATTEST;
}

// One signature per load.
void crypto_ecc256_sign(uint8_t *data, int len, uint8_t *sig)
{
    if (loaded == KEY_ATTEST) {
        if (len != 32)
            fail("attestation signs a 32-byte digest only");
        if (se_sign(SE_SLOT_ATTEST, data, sig) != 0)
            fail("attestation signature failed");
    } else if (loaded == KEY_P256) {
        uint8_t priv[32], pub[64];
        derive_p256(loaded_id, priv, pub);
        int ok = uECC_sign(priv, data, (unsigned)len, sig, uECC_secp256r1());
        wipe(priv, sizeof(priv));
        if (!ok)
            fail("uECC_sign failed");
    } else {
        fail("ES256 signature with no key loaded");
    }
    loaded = KEY_NONE;
}

// ---- Ed25519 credentials ----------------------------------------------------------------------

static void derive_ed25519(const uint8_t id[32], uint8_t pk[32], uint8_t sk[64])
{
    uint8_t seed[crypto_sign_ed25519_SEEDBYTES];
    derive(LABEL_CRED_ED25519, 0, id, seed);
    crypto_sign_ed25519_seed_keypair(pk, sk, seed);
    wipe(seed, sizeof(seed));
}

void crypto_ed25519_derive_public_key(uint8_t *data, int len, uint8_t *x)
{
    uint8_t id[32], sk[crypto_sign_ed25519_SECRETKEYBYTES];
    id_digest(data, len, NULL, 0, id);
    derive_ed25519(id, x, sk);
    wipe(sk, sizeof(sk));
}

void crypto_ed25519_load_key(uint8_t *data, int len)
{
    id_digest(data, len, NULL, 0, loaded_id);
    loaded = KEY_ED25519;
}

void crypto_ed25519_sign(uint8_t *data1, int len1, uint8_t *data2, int len2, uint8_t *sig)
{
    // Ed25519 signs the whole message at once; solo1 caps each part at 512 bytes.
    uint8_t msg[1024], pk[crypto_sign_ed25519_PUBLICKEYBYTES], sk[crypto_sign_ed25519_SECRETKEYBYTES];
    if (loaded != KEY_ED25519)
        fail("Ed25519 signature with no key loaded");
    if (len1 < 0 || len2 < 0 || len1 > 512 || len2 > 512)
        fail("Ed25519 message too long");
    memcpy(msg, data1, (size_t)len1);
    memcpy(msg + len1, data2, (size_t)len2);
    derive_ed25519(loaded_id, pk, sk);
    crypto_sign_ed25519_detached(sig, NULL, msg, (unsigned long long)(len1 + len2), sk);
    wipe(sk, sizeof(sk));
    loaded = KEY_NONE;
}
