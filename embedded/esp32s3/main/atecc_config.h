// The ATECC608B configuration tinycrypt writes and locks (CRYPT-22). Locking is permanent, so
// every bit here is checked by tests/test_atecc_config.py, and the first chip locked is a
// sacrificial one.
//
// Source: the ATECC608A datasheet (2017 preliminary, the full one), tables 2-5 to 2-11; the
// 608B keeps the 608A's configuration zone. Bytes not listed (serial, revision, I2C address,
// ChipMode, the counters' initial values, UserExtra) keep their factory values: the provisioner
// reads the zone, overlays these and writes it back.
//
// | Slot | Holds                     | SlotConfig | KeyConfig |
// |------|---------------------------|------------|-----------|
// | 0    | IoT Core TLS key (P-256)  | 0x2081     | 0x0033    |
// | 1    | FIDO attestation key      | 0x2081     | 0x0033    |
// | 2-7  | spare P-256 keys          | 0x2081     | 0x0033    |
// | 8    | unused data               | 0x808F     | 0x001C    |
// | 9    | credential master secret  | 0x208F     | 0x005C    |
// | 10-15| unused                    | 0x808F     | 0x001C    |
//
// P-256 slots, SlotConfig 0x2081: ReadKey bit 0 only (signs external digests; no internal sign,
// no ECDH), IsSecret (GenKey and Sign need it), WriteConfig 0010 (GenKey may make a key here,
// PrivWrite forbidden, so a private key can only ever be generated on the chip). KeyConfig
// 0x0033: Private, PubInfo (the public key can be read back), KeyType 4 (P-256), Lockable: the
// provisioner locks slots 0 and 1 right after GenKey, so those keys can never be replaced. The spares are configured the same so per-site keys (scheme C)
// stay possible: the configuration can never change once locked.
//
// Master secret, SlotConfig 0x208F: IsSecret with EncryptRead 0 (never readable), NoMac 0 (the
// SHA command's HMAC mode needs a key legal for MAC operations), WriteConfig 0010 (Write: never;
// DeriveKey Roll without an authorising MAC, which is how a CTAP reset replaces it), ReadKey 0xF
// (unused, but the datasheet warns against 0). KeyConfig 0x005C: KeyType 7 (SHA key),
// ReqRandom (a roll must mix in the chip's own random nonce). Its first value is written in the
// clear before the data zone is locked, from the chip's RNG (real only once the configuration is
// locked).
//
// Unused slots, SlotConfig 0x808F: IsSecret, WriteConfig 1000 (Write never, not a DeriveKey
// target, no GenKey). KeyConfig 0x001C: KeyType 7.
//
// The sign counter is Counter[1], which is attached to no key.
#ifndef TINYCRYPT_ATECC_CONFIG_H
#define TINYCRYPT_ATECC_CONFIG_H

#include <stdint.h>

#define ATECC_SLOT_IOT 0
#define ATECC_SLOT_ATTEST 1
#define ATECC_SLOT_MASTER 9
#define ATECC_SIGN_COUNTER 1

#define ATECC_SLOT_CONFIG_OFFSET 20
#define ATECC_KEY_CONFIG_OFFSET 96

static const uint16_t ATECC_SLOT_CONFIG[16] = {
    0x2081, 0x2081, 0x2081, 0x2081, 0x2081, 0x2081, 0x2081, 0x2081,
    0x808F, 0x208F, 0x808F, 0x808F, 0x808F, 0x808F, 0x808F, 0x808F,
};

static const uint16_t ATECC_KEY_CONFIG[16] = {
    0x0033, 0x0033, 0x0033, 0x0033, 0x0033, 0x0033, 0x0033, 0x0033,
    0x001C, 0x005C, 0x001C, 0x001C, 0x001C, 0x001C, 0x001C, 0x001C,
};

// Other bytes we set (offset, value): no transport lock, no volatile key, no secure boot mode,
// no X.509 format restrictions. ChipOptions (90-91) stays at its factory value.
static const struct {
    uint8_t offset, value;
} ATECC_CONFIG_BYTES[] = {
    {68, 0x00},                                      // UseLock
    {69, 0x00},                                      // VolatileKeyPermission
    {70, 0x00}, {71, 0x00},                          // SecureBoot
    {92, 0x00}, {93, 0x00}, {94, 0x00}, {95, 0x00},  // X509format
};

// Overlay tinycrypt's settings onto a configuration zone read from the chip (128 bytes).
static inline void atecc_config_apply(uint8_t zone[128])
{
    for (int i = 0; i < 16; i++) {
        zone[ATECC_SLOT_CONFIG_OFFSET + 2 * i] = (uint8_t)ATECC_SLOT_CONFIG[i];
        zone[ATECC_SLOT_CONFIG_OFFSET + 2 * i + 1] = (uint8_t)(ATECC_SLOT_CONFIG[i] >> 8);
        zone[ATECC_KEY_CONFIG_OFFSET + 2 * i] = (uint8_t)ATECC_KEY_CONFIG[i];
        zone[ATECC_KEY_CONFIG_OFFSET + 2 * i + 1] = (uint8_t)(ATECC_KEY_CONFIG[i] >> 8);
    }
    for (unsigned i = 0; i < sizeof(ATECC_CONFIG_BYTES) / sizeof(ATECC_CONFIG_BYTES[0]); i++)
        zone[ATECC_CONFIG_BYTES[i].offset] = ATECC_CONFIG_BYTES[i].value;
}

#endif
