// The provisioner (CRYPT-22, CONFIG_TINYCRYPT_PROVISIONER): a separate image that prepares the
// secure element, driven over the console by esp32s3/tools/atecc_provision.py. The console is the
// native USB port (USB-Serial-JTAG, sdkconfig.provisioner.defaults): it's the only port the key's
// PCB has, and flashing over it needs no BOOT+RST. A UART console build still works.
//
// Commands, one per line; every answer is one line, "TCPROV " and a JSON object:
//   info              the chip: serial, revision, lock states, whether its configuration is ours
//   lock <serial>     configure and lock a blank ATECC608B, then make its keys. PERMANENT. The
//                     serial must be this chip's (from info), so it can't happen by accident.
//                     Each step runs only if still needed, so an interrupted run can be repeated.
//   csr <thing>       the IoT key's CSR (CN = the Thing name) and both public keys
//
// Locking: write the configuration (atecc_config.h over the factory bytes), read it back and
// check it, lock it with the CRC of what was read (so the lock fails if the bus corrupted
// anything), write the master secret (slot 9) from the chip's RNG (real only once the
// configuration is locked), lock the data zone, then GenKey slots 0 and 1 and lock each slot.
// A locked slot is how a later run knows its key was made by GenKey (a readable public key
// proves nothing: the slot's factory bytes might pass for one), and it stops any firmware from
// replacing the attestation or IoT key with GenKey later.
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "bootloader_random.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "secure_element.h"
#include "x509_min.h"

#ifdef TINYCRYPT_KEYSTORE_ATECC
#include "atecc_config.h"
#include "cryptoauthlib.h"
#include "se_atecc.h"
#endif

static void hex(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        printf("%02x", p[i]);
}

static void error(const char *what)
{
    printf("TCPROV {\"error\":\"%s\"}\n", what);
}

#ifdef TINYCRYPT_KEYSTORE_ATECC

static const char *TAG = "provisioner";

static bool status_ok(const char *what, ATCA_STATUS s)
{
    if (s == ATCA_SUCCESS)
        return true;
    ESP_LOGE(TAG, "%s: 0x%02x", what, s);
    char msg[64];
    snprintf(msg, sizeof(msg), "%s failed (0x%02x)", what, s);
    error(msg);
    return false;
}

// Is the configuration zone exactly ours (tinycrypt's settings over whatever else is there)?
static bool config_is_ours(const uint8_t zone[128])
{
    uint8_t want[128];
    memcpy(want, zone, sizeof(want));
    atecc_config_apply(want);
    return memcmp(zone, want, sizeof(want)) == 0;
}

static void cmd_info(void)
{
    uint8_t sn[ATCA_SERIAL_NUM_SIZE], rev[4], zone[128];
    bool cl = false, dl = false;
    if (!status_ok("serial", atcab_read_serial_number(sn)) || !status_ok("info", atcab_info(rev)) ||
        !status_ok("config lock", atcab_is_config_locked(&cl)) ||
        !status_ok("data lock", atcab_is_data_locked(&dl)) ||
        !status_ok("read config", atcab_read_config_zone(zone)))
        return;
    printf("TCPROV {\"backend\":\"atecc608\",\"serial\":\"");
    hex(sn, sizeof(sn));
    printf("\",\"revision\":\"");
    hex(rev, sizeof(rev));
    printf("\",\"config_locked\":%s,\"data_locked\":%s,\"config_ours\":%s,\"config\":\"",
           cl ? "true" : "false", dl ? "true" : "false", config_is_ours(zone) ? "true" : "false");
    hex(zone, sizeof(zone));
    printf("\"}\n");
}

static bool lock_config(void)
{
    uint8_t zone[128], back[128], crc_le[2];
    if (!status_ok("read config", atcab_read_config_zone(zone)))
        return false;
    atecc_config_apply(zone);
    if (!status_ok("write config", atcab_write_config_zone(zone)) ||
        !status_ok("read back", atcab_read_config_zone(back)))
        return false;
    if (!config_is_ours(back)) {
        error("configuration read back differs: not locking");
        return false;
    }
    atCRC(sizeof(back), back, crc_le);
    return status_ok("lock config", atcab_lock_config_zone_crc((uint16_t)(crc_le[0] | crc_le[1] << 8)));
}

static bool lock_data(void)
{
    uint8_t secret[32];
    bool ok = status_ok("random", atcab_random(secret)) &&
              status_ok("write master", atcab_write_bytes_zone(ATCA_ZONE_DATA, ATECC_SLOT_MASTER, 0, secret, 32));
    memset(secret, 0, sizeof(secret));
    return ok && status_ok("lock data", atcab_lock_data_zone());
}

static bool ensure_key(uint16_t slot)
{
    uint8_t pub[64];
    bool locked = false;
    if (!status_ok("slot lock", atcab_is_slot_locked(slot, &locked)))
        return false;
    if (locked)
        return true;
    return status_ok("genkey", atcab_genkey(slot, pub)) && status_ok("lock slot", atcab_lock_data_slot(slot));
}

static void cmd_lock(const char *arg)
{
    uint8_t sn[ATCA_SERIAL_NUM_SIZE], zone[128];
    char want[2 * ATCA_SERIAL_NUM_SIZE + 1];
    bool cl = false, dl = false;
    if (!status_ok("serial", atcab_read_serial_number(sn)))
        return;
    for (size_t i = 0; i < sizeof(sn); i++)
        snprintf(want + 2 * i, 3, "%02x", sn[i]);
    if (strcasecmp(arg, want) != 0) {
        error("serial doesn't match this chip: nothing done");
        return;
    }
    if (!status_ok("config lock", atcab_is_config_locked(&cl)))
        return;
    if (cl) {
        if (!status_ok("read config", atcab_read_config_zone(zone)))
            return;
        if (!config_is_ours(zone)) {
            error("configuration already locked, and not tinycrypt's: this chip can't be used");
            return;
        }
    } else if (!lock_config()) {
        return;
    }
    if (!status_ok("data lock", atcab_is_data_locked(&dl)))
        return;
    if (!dl && !lock_data())
        return;
    if (!ensure_key(ATECC_SLOT_IOT) || !ensure_key(ATECC_SLOT_ATTEST))
        return;
    printf("TCPROV {\"locked\":true}\n");
}

#else

static void cmd_info(void)
{
    uint8_t sn[SE_SERIAL_LEN];
    if (se_init() != 0 || se_serial(sn) != 0) {
        error("soft element failed");
        return;
    }
    printf("TCPROV {\"backend\":\"%s\",\"serial\":\"", se_backend());
    hex(sn, sizeof(sn));
    printf("\",\"config_locked\":true,\"data_locked\":true,\"config_ours\":true}\n");
}

static void cmd_lock(const char *arg)
{
    (void)arg;
    error("the soft element has nothing to lock");
}

#endif

static bool valid_thing(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 128)
        return false;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && !strchr("_-:", *s))
            return false;
    return true;
}

static void cmd_csr(const char *thing)
{
    uint8_t csr[X509_MIN_MAX], iot[SE_PUB_LEN], attest[SE_PUB_LEN];
    if (!valid_thing(thing)) {
        error("bad Thing name");
        return;
    }
    if (se_init() != 0) {
        error("secure element not ready (provisioned and locked?)");
        return;
    }
    int n = x509_csr(thing, csr, sizeof(csr));
    if (n < 0 || se_pubkey(SE_SLOT_IOT, iot) != 0 || se_pubkey(SE_SLOT_ATTEST, attest) != 0) {
        error("CSR failed");
        return;
    }
    printf("TCPROV {\"thing\":\"%s\",\"csr\":\"", thing);
    hex(csr, (size_t)n);
    printf("\",\"iot_pub\":\"");
    hex(iot, sizeof(iot));
    printf("\",\"attest_pub\":\"");
    hex(attest, sizeof(attest));
    printf("\"}\n");
}

int ctap_generate_rng(uint8_t *dst, size_t num)
{
    esp_fill_random(dst, num);
    return 1;
}

void app_main(void)
{
    // No radio: the ADC noise feeds esp_fill_random (the soft element's keys, the CSR serial).
    bootloader_random_enable();
    // The soft element lives in NVS; never erase it to get past an error.
    ESP_ERROR_CHECK(nvs_flash_init());
    // Blocking reads on stdin need the console's driver, not the boot-time polling one.
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t usj = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usj));
    usb_serial_jtag_vfs_use_driver();
#else
    ESP_ERROR_CHECK(uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, NULL, 0));
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
#endif
    setvbuf(stdin, NULL, _IONBF, 0);

#ifdef TINYCRYPT_KEYSTORE_ATECC
    if (se_atecc_open() != 0) {
        error("no ATECC608 on the bus (wiring, address?)");
    }
#endif
    printf("TCPROV {\"ready\":true,\"backend\":\"%s\"}\n", se_backend());

    char line[200];
    for (;;) {
        if (fgets(line, sizeof(line), stdin) == NULL)
            continue;
        line[strcspn(line, "\r\n")] = 0;
        if (strcmp(line, "info") == 0)
            cmd_info();
        else if (strncmp(line, "lock ", 5) == 0)
            cmd_lock(line + 5);
        else if (strncmp(line, "csr ", 4) == 0)
            cmd_csr(line + 4);
        else if (line[0])
            error("commands: info | lock <serial> | csr <thing>");
        fflush(stdout);
    }
}
