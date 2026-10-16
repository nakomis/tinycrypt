// The ATECC608B secure element (CRYPT-22, secure_element.h), through cryptoauthlib's atcab_* API.
//
// The chip must already be provisioned (provisioner.c): configuration and data zones locked with
// atecc_config.h's settings, keys generated. se_init refuses a chip that isn't, rather than
// using one whose permissions we haven't checked.
//
// Every call holds one mutex: the CTAP task (HMACs, attestation) and the MQTT task (the TLS
// signature, se_tls.c) both use the chip, and cryptoauthlib has no locking of its own. A
// sequence like Nonce then DeriveKey must not be split by another command.
//
// esp-cryptoauthlib's I2C HAL turns ACK checking off, so with no chip on the bus every transfer
// "succeeds" and reads 0xFF, which cryptoauthlib takes for "busy" and polls for minutes per
// command (atcab_init already reads the chip). So se_atecc_open first wakes the chip and probes
// its address with ACKs checked, on a bus of its own that it frees before cryptoauthlib makes one.
#include "secure_element.h"
#include "se_atecc.h"

#include <stdbool.h>
#include <string.h>

#include "atecc_config.h"
#include "cryptoauthlib.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

static const char *TAG = "se-atecc";

static SemaphoreHandle_t lock;
static bool ready;

#define WITH_LOCK(expr)                          \
    ({                                           \
        xSemaphoreTake(lock, portMAX_DELAY);     \
        ATCA_STATUS _s = (expr);                 \
        xSemaphoreGive(lock);                    \
        _s;                                      \
    })

static int check(const char *what, ATCA_STATUS s)
{
    if (s == ATCA_SUCCESS)
        return 0;
    ESP_LOGE(TAG, "%s: 0x%02x", what, s);
    return -1;
}

// Whether an ATECC answers at our address. It sleeps, and then ACKs nothing, so wake it first:
// SDA low for at least 60 us (an address byte of zeros at 100 kHz holds it low for ~80 us), then
// 1.5 ms for it to start.
static bool chip_present(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,
        .sda_io_num = CONFIG_ATCA_I2C_SDA_PIN,
        .scl_io_num = CONFIG_ATCA_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK)
        return false;
    (void)i2c_master_probe(bus, 0x00, 10);  // the wake pulse; nothing answers it
    esp_rom_delay_us(1500);
    esp_err_t rc = i2c_master_probe(bus, CONFIG_ATCA_I2C_ADDRESS >> 1, 50);
    if (rc == ESP_OK) {
        // Back to sleep (word address 0x01), so cryptoauthlib's own wake finds the chip as it
        // expects: asleep, answering its wake with the 04 11 33 43 token.
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = CONFIG_ATCA_I2C_ADDRESS >> 1,
            .scl_speed_hz = 100000,
        };
        i2c_master_dev_handle_t dev;
        static const uint8_t sleep = 0x01;
        if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) == ESP_OK) {
            (void)i2c_master_transmit(dev, &sleep, 1, 50);
            i2c_master_bus_rm_device(dev);
        }
    }
    i2c_del_master_bus(bus);
    if (rc != ESP_OK)
        ESP_LOGE(TAG, "no ATECC608 answers at 0x%02x (8-bit): %s", CONFIG_ATCA_I2C_ADDRESS, esp_err_to_name(rc));
    return rc == ESP_OK;
}

int se_atecc_open(void)
{
    static ATCAIfaceCfg cfg = {
        .iface_type = ATCA_I2C_IFACE,
        .devtype = ATECC608,
        .atcai2c.address = CONFIG_ATCA_I2C_ADDRESS,
        .atcai2c.bus = 0,
        .atcai2c.baud = CONFIG_ATCA_I2C_BAUD_RATE,
        .wake_delay = 1500,
        .rx_retries = 20,
    };
    if (lock == NULL && (lock = xSemaphoreCreateMutex()) == NULL)
        return -1;
    static bool open;
    if (open)
        return 0;  // cryptoauthlib holds the bus now, so the probe couldn't run again anyway
    if (!chip_present() || check("atcab_init", WITH_LOCK(atcab_init(&cfg))) != 0)
        return -1;
    open = true;
    return 0;
}

// The configuration zone must hold exactly atecc_config.h's settings, and both zones be locked.
static int verify_provisioned(void)
{
    uint8_t zone[128], want[128];
    bool config_locked = false, data_locked = false;
    if (check("config lock", WITH_LOCK(atcab_is_config_locked(&config_locked))) != 0 ||
        check("data lock", WITH_LOCK(atcab_is_data_locked(&data_locked))) != 0 ||
        check("read config", WITH_LOCK(atcab_read_config_zone(zone))) != 0)
        return -1;
    if (!config_locked || !data_locked) {
        ESP_LOGE(TAG, "not provisioned (config %s, data %s): run the provisioner",
                 config_locked ? "locked" : "unlocked", data_locked ? "locked" : "unlocked");
        return -1;
    }
    memcpy(want, zone, sizeof(want));
    atecc_config_apply(want);
    if (memcmp(zone, want, sizeof(zone)) != 0) {
        ESP_LOGE(TAG, "configuration is not tinycrypt's: refusing the chip");
        return -1;
    }
    return 0;
}

int se_init(void)
{
    if (ready)
        return 0;
    if (se_atecc_open() != 0 || verify_provisioned() != 0)
        return -1;
    // The provisioner locks each key slot right after GenKey: unlocked means no key of ours.
    for (uint16_t slot = ATECC_SLOT_IOT; slot <= ATECC_SLOT_ATTEST; slot++) {
        bool locked = false;
        if (check("slot lock", WITH_LOCK(atcab_is_slot_locked(slot, &locked))) != 0)
            return -1;
        if (!locked) {
            ESP_LOGE(TAG, "slot %u has no key yet: run the provisioner's lock again", slot);
            return -1;
        }
    }
    uint8_t pub[SE_PUB_LEN];
    if (se_pubkey(SE_SLOT_IOT, pub) != 0 || se_pubkey(SE_SLOT_ATTEST, pub) != 0)
        return -1;
    ready = true;
    return 0;
}

const char *se_backend(void)
{
    return "atecc608";
}

int se_serial(uint8_t serial[SE_SERIAL_LEN])
{
    return check("serial", WITH_LOCK(atcab_read_serial_number(serial)));
}

int se_random(uint8_t *out, size_t len)
{
    uint8_t block[32];
    while (len > 0) {
        if (check("random", WITH_LOCK(atcab_random(block))) != 0)
            return -1;
        size_t n = len < sizeof(block) ? len : sizeof(block);
        memcpy(out, block, n);
        out += n;
        len -= n;
    }
    memset(block, 0, sizeof(block));
    return 0;
}

// Slot 9's KeyConfig.ReqRandom covers the SHA command's HMAC mode too (found on the sacrificial
// chip: without a fresh random nonce in TempKey the HMAC fails with an execution error). The nonce
// only unlocks the command: the HMAC is of the slot's key and the message, so it doesn't change.
int se_hmac(const uint8_t *msg, size_t len, uint8_t mac[SE_HMAC_LEN])
{
    uint8_t num_in[20] = {0}, rand_out[32];
    if (!ready || len > SE_HMAC_MAX)
        return -1;
    xSemaphoreTake(lock, portMAX_DELAY);
    ATCA_STATUS s = atcab_nonce_rand(num_in, rand_out);
    if (s == ATCA_SUCCESS)
        s = atcab_sha_hmac(msg, len, ATECC_SLOT_MASTER, mac, SHA_MODE_TARGET_OUT_ONLY);
    xSemaphoreGive(lock);
    return check("hmac", s);
}

// DeriveKey(Roll): slot 9 = SHA-256(slot 9, opcode, mode, slot, SN, TempKey), TempKey being a
// random nonce from the chip (KeyConfig.ReqRandom). Mode 0: TempKey.SourceFlag is Rand.
int se_master_roll(void)
{
    uint8_t num_in[20] = {0}, rand_out[32];
    if (!ready)
        return -1;
    xSemaphoreTake(lock, portMAX_DELAY);
    ATCA_STATUS s = atcab_nonce_rand(num_in, rand_out);
    if (s == ATCA_SUCCESS)
        s = atcab_derivekey(0, ATECC_SLOT_MASTER, NULL);
    xSemaphoreGive(lock);
    return check("master roll", s);
}

static bool slot_ok(int slot)
{
    return slot == SE_SLOT_IOT || slot == SE_SLOT_ATTEST;
}

int se_sign(int slot, const uint8_t digest[32], uint8_t sig[SE_SIG_LEN])
{
    if (!slot_ok(slot))
        return -1;
    return check("sign", WITH_LOCK(atcab_sign((uint16_t)slot, digest, sig)));
}

int se_pubkey(int slot, uint8_t pub[SE_PUB_LEN])
{
    if (!slot_ok(slot))
        return -1;
    return check("pubkey", WITH_LOCK(atcab_get_pubkey((uint16_t)slot, pub)));
}

int se_counter_increment(uint32_t *value)
{
    if (!ready)
        return -1;
    return check("counter", WITH_LOCK(atcab_counter_increment(ATECC_SIGN_COUNTER, value)));
}
