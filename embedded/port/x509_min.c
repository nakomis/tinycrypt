// Minimal DER writer for the attestation certificate and the IoT CSR (x509_min.h).
//
// Each constructed value is written as its content first, then wrapped: wrap() slides the
// content along to make room for the tag and length in front of it.
#include "x509_min.h"

#include <stdbool.h>
#include <string.h>

#include "secure_element.h"
#include "sha256.h"

typedef struct {
    uint8_t *p;
    size_t len, cap;
    bool err;
} der;

static void put(der *d, const void *data, size_t n)
{
    if (n == 0)
        return;
    if (d->err || n > d->cap - d->len) {
        d->err = true;
        return;
    }
    memcpy(d->p + d->len, data, n);
    d->len += n;
}

static void put_byte(der *d, uint8_t b)
{
    put(d, &b, 1);
}

// Wrap everything written since `start` in a tag.
static void wrap(der *d, size_t start, uint8_t tag)
{
    if (d->err)
        return;
    size_t n = d->len - start;
    uint8_t hdr[4];
    size_t h = 0;
    hdr[h++] = tag;
    if (n < 0x80) {
        hdr[h++] = (uint8_t)n;
    } else if (n < 0x100) {
        hdr[h++] = 0x81;
        hdr[h++] = (uint8_t)n;
    } else {
        hdr[h++] = 0x82;
        hdr[h++] = (uint8_t)(n >> 8);
        hdr[h++] = (uint8_t)n;
    }
    if (h > d->cap - d->len) {
        d->err = true;
        return;
    }
    memmove(d->p + start + h, d->p + start, n);
    memcpy(d->p + start, hdr, h);
    d->len += h;
}

static void tlv(der *d, uint8_t tag, const void *data, size_t n)
{
    size_t start = d->len;
    put(d, data, n);
    wrap(d, start, tag);
}

// A non-negative INTEGER from big-endian bytes: no leading zeros, a 0x00 before a high bit.
static void integer(der *d, const uint8_t *be, size_t n)
{
    size_t start = d->len;
    while (n > 1 && be[0] == 0) {
        be++;
        n--;
    }
    if (be[0] & 0x80)
        put_byte(d, 0);
    put(d, be, n);
    wrap(d, start, 0x02);
}

static const uint8_t OID_ECDSA_SHA256[] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02};
static const uint8_t OID_EC_PUBLIC_KEY[] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01};
static const uint8_t OID_PRIME256V1[] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07};
static const uint8_t OID_C[] = {0x55, 0x04, 0x06};
static const uint8_t OID_O[] = {0x55, 0x04, 0x0a};
static const uint8_t OID_OU[] = {0x55, 0x04, 0x0b};
static const uint8_t OID_CN[] = {0x55, 0x04, 0x03};
static const uint8_t OID_BASIC_CONSTRAINTS[] = {0x55, 0x1d, 0x13};
// id-fido-gen-ce-aaguid, 1.3.6.1.4.1.45724.1.1.4
static const uint8_t OID_FIDO_AAGUID[] = {0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0xe5, 0x1c, 0x01, 0x01, 0x04};

static void sig_alg(der *d)
{
    size_t start = d->len;
    tlv(d, 0x06, OID_ECDSA_SHA256, sizeof(OID_ECDSA_SHA256));
    wrap(d, start, 0x30);
}

// One RelativeDistinguishedName: SET { SEQUENCE { type, value } }.
static void rdn(der *d, const uint8_t *oid, size_t oid_len, uint8_t str_tag, const char *value)
{
    size_t set = d->len;
    size_t seq = d->len;
    tlv(d, 0x06, oid, oid_len);
    tlv(d, str_tag, value, strlen(value));
    wrap(d, seq, 0x30);
    wrap(d, set, 0x31);
}

static void spki(der *d, const uint8_t pub[SE_PUB_LEN])
{
    size_t start = d->len, alg = d->len;
    tlv(d, 0x06, OID_EC_PUBLIC_KEY, sizeof(OID_EC_PUBLIC_KEY));
    tlv(d, 0x06, OID_PRIME256V1, sizeof(OID_PRIME256V1));
    wrap(d, alg, 0x30);
    size_t bits = d->len;
    put_byte(d, 0x00);  // no unused bits
    put_byte(d, 0x04);  // uncompressed point
    put(d, pub, SE_PUB_LEN);
    wrap(d, bits, 0x03);
    wrap(d, start, 0x30);
}

// Sign SHA-256 of [from, d->len) with `slot`, then wrap it all as SEQUENCE { tbs, alg, sig }.
static void sign_and_close(der *d, size_t from, int slot)
{
    uint8_t digest[32], sig[SE_SIG_LEN];
    SHA256_CTX ctx;
    if (d->err)
        return;
    sha256_init(&ctx);
    sha256_update(&ctx, d->p + from, d->len - from);
    sha256_final(&ctx, digest);
    if (se_sign(slot, digest, sig) != 0) {
        d->err = true;
        return;
    }
    sig_alg(d);
    size_t bits = d->len;
    put_byte(d, 0x00);
    size_t seq = d->len;
    integer(d, sig, 32);
    integer(d, sig + 32, 32);
    wrap(d, seq, 0x30);
    wrap(d, bits, 0x03);
    wrap(d, from, 0x30);
}

static void attestation_name(der *d)
{
    size_t start = d->len;
    rdn(d, OID_C, sizeof(OID_C), 0x13, "GB");  // PrintableString
    rdn(d, OID_O, sizeof(OID_O), 0x0c, "Nakomis");  // UTF8String
    rdn(d, OID_OU, sizeof(OID_OU), 0x0c, "Authenticator Attestation");
    rdn(d, OID_CN, sizeof(OID_CN), 0x0c, "tinycrypt");
    wrap(d, start, 0x30);
}

int x509_attestation_cert(const uint8_t aaguid[16], uint8_t *out, size_t cap)
{
    der d = {out, 0, cap, false};
    uint8_t pub[SE_PUB_LEN], serial[16];
    static const uint8_t v3[] = {0x02, 0x01, 0x02};
    static const uint8_t yes[] = {0xff};

    if (se_pubkey(SE_SLOT_ATTEST, pub) != 0 || se_random(serial, sizeof(serial)) != 0)
        return -1;
    serial[0] = (serial[0] & 0x7f) | 0x40;  // positive, and a full 16 bytes

    size_t tbs = d.len;
    tlv(&d, 0xa0, v3, sizeof(v3));  // [0] EXPLICIT version v3
    integer(&d, serial, sizeof(serial));
    sig_alg(&d);
    attestation_name(&d);  // issuer: self-signed
    size_t validity = d.len;
    tlv(&d, 0x17, "260101000000Z", 13);    // UTCTime
    tlv(&d, 0x18, "99991231235959Z", 15);  // GeneralizedTime: no well-defined expiry (RFC 5280)
    wrap(&d, validity, 0x30);
    attestation_name(&d);  // subject
    spki(&d, pub);

    size_t exts = d.len, seq = d.len;
    {
        size_t ext = d.len;  // basicConstraints, critical, CA:FALSE (an empty SEQUENCE)
        tlv(&d, 0x06, OID_BASIC_CONSTRAINTS, sizeof(OID_BASIC_CONSTRAINTS));
        tlv(&d, 0x01, yes, sizeof(yes));
        tlv(&d, 0x04, "\x30\x00", 2);
        wrap(&d, ext, 0x30);
    }
    {
        size_t ext = d.len;  // the AAGUID, non-critical, an OCTET STRING inside the extension's
        tlv(&d, 0x06, OID_FIDO_AAGUID, sizeof(OID_FIDO_AAGUID));
        size_t value = d.len;
        tlv(&d, 0x04, aaguid, 16);
        wrap(&d, value, 0x04);
        wrap(&d, ext, 0x30);
    }
    wrap(&d, seq, 0x30);
    wrap(&d, exts, 0xa3);  // [3] EXPLICIT extensions
    wrap(&d, tbs, 0x30);

    sign_and_close(&d, tbs, SE_SLOT_ATTEST);
    return d.err ? -1 : (int)d.len;
}

int x509_csr(const char *cn, uint8_t *out, size_t cap)
{
    der d = {out, 0, cap, false};
    uint8_t pub[SE_PUB_LEN];
    static const uint8_t v1[] = {0x00};

    if (se_pubkey(SE_SLOT_IOT, pub) != 0)
        return -1;

    size_t info = d.len;
    integer(&d, v1, sizeof(v1));
    size_t name = d.len;
    rdn(&d, OID_CN, sizeof(OID_CN), 0x0c, cn);
    wrap(&d, name, 0x30);
    spki(&d, pub);
    tlv(&d, 0xa0, NULL, 0);  // [0] attributes: none
    wrap(&d, info, 0x30);

    sign_and_close(&d, info, SE_SLOT_IOT);
    return d.err ? -1 : (int)d.len;
}
