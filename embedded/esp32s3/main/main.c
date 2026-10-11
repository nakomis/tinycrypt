// tinycrypt ESP32-S3 port: the solo1 CTAP2 core behind TinyUSB FIDO HID.
//
// - Transport: one HID interface on the FIDO usage page (0xF1D0), 64-byte
//   reports on interrupt IN/OUT endpoints. TinyUSB runs in its own task and
//   queues OUT reports; the CTAP task drains the queue.
// - Presence: the BOOT button (GPIO0, active low) OR the watch, whichever answers first
//   (port/presence.c). With CONFIG_TINYCRYPT_NOTIFIER a push asks the watch; with
//   CONFIG_TINYCRYPT_WATCH_BLE the watch approves by signing a BLE challenge (ble_presence.c).
//   The button always works on its own. Hold it 5 s while idle to open the watch enrol window.
// - Keystore: INSECURE software key. Master secret, resident keys and the
//   sign counter live in plain NVS. TEST ONLY.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bootloader_random.h"
#include "class/hid/hid_device.h"
#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_core_dump.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "tinyusb.h"

#include "ctap.h"
#include "ctaphid.h"
#include "device.h"
#include "log.h"
#include "storage.h"
#include "util.h"
#ifdef TINYCRYPT_SE_SELFTEST
#include "se_selftest.h"
#endif
#include "presence.h"
#include "sdkconfig.h"
#ifdef CONFIG_TINYCRYPT_NOTIFIER
#include "notifier.h"
#endif
#ifdef CONFIG_TINYCRYPT_WATCH_BLE
#include "ble_presence.h"
#endif
#include APP_CONFIG

// A board with secure boot or flash encryption is a release board; the
// INSECURE soft key must never run on one, whatever TINYCRYPT_RELEASE says.
#if defined(TINYCRYPT_INSECURE_SOFT_KEY) && \
    (defined(CONFIG_SECURE_BOOT) || defined(CONFIG_SECURE_FLASH_ENC_ENABLED))
#error "TINYCRYPT_INSECURE_SOFT_KEY must not be built with secure boot or flash encryption enabled"
#endif

static const char *TAG = "tinycrypt";

#define BUTTON_GPIO GPIO_NUM_0
#define RK_NUM 10
#define REPORT_SIZE 64
#define NVS_NAMESPACE "tinycrypt"
#define ENROL_HOLD_MS 5000
#define ENROL_WINDOW_S 60

static QueueHandle_t rx_queue;
static nvs_handle_t nvs;
static bool up_disabled = false;
static CTAP_residentKey rk_store[RK_NUM];

// ---- USB descriptors --------------------------------------------------------

static const uint8_t hid_report_descriptor[] = {TUD_HID_REPORT_DESC_FIDO_U2F(REPORT_SIZE)};

#define EPNUM_HID 0x01
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

static const uint8_t configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, 0, 100),
    TUD_HID_INOUT_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_NONE, sizeof(hid_report_descriptor),
                             EPNUM_HID, 0x80 | EPNUM_HID, REPORT_SIZE, 5),
};

static const tusb_desc_device_t device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,  // Espressif
    .idProduct = 0x4004, // test PID, not allocated
    .bcdDevice = 0x0001,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

// The serial number carries the last reset reason (esp_reset_reason_t), e.g.
// "0001-rr4" after a panic, so crashes are visible from the host (ioreg/lsusb)
// without a UART cable.
static char serial_string[16] = "0001";

static const char *string_descriptor[] = {
    (const char[]){0x09, 0x04}, // English
    "nakomis",
    "tinycrypt INSECURE TEST KEY",
    serial_string,
    "FIDO",
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    return 0;
}

// OUT reports from the host arrive here (TinyUSB task context).
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    uint8_t msg[REPORT_SIZE] = {0};
    memcpy(msg, buffer, bufsize < REPORT_SIZE ? bufsize : REPORT_SIZE);
    // Blocking briefly is safe: TinyUSB only re-arms the OUT endpoint after we
    // return, so the host is NAKed rather than a report being lost mid-message.
    if (xQueueSend(rx_queue, msg, pdMS_TO_TICKS(50)) != pdTRUE)
        ESP_LOGW(TAG, "rx queue full, dropping report");
}

// ---- transport hooks for the core ------------------------------------------

// Multi-packet replies queue reports back to back, so wait for the IN
// endpoint to drain the previous one (time-based: a tick may be longer than 1 ms).
void usbhid_send(uint8_t *msg)
{
    if (!presence_filter_send(msg))
        return;  // answers a transaction the client aborted with INIT (presence.h)
    int64_t until = esp_timer_get_time() + 100 * 1000;
    while (!tud_hid_ready() && esp_timer_get_time() < until)
        vTaskDelay(1);
    if (!tud_hid_report(0, msg, REPORT_SIZE))
        ESP_LOGW(TAG, "IN report dropped");
}

static int usbhid_recv(uint8_t *msg, TickType_t wait)
{
    return xQueueReceive(rx_queue, msg, wait) == pdTRUE ? REPORT_SIZE : 0;
}

uint32_t millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void delay(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void device_read_aaguid(uint8_t *dst)
{
    memmove(dst, TINYCRYPT_AAGUID, 16);
}

void device_reboot(void)
{
    esp_restart();
}

// ---- presence: BOOT button --------------------------------------------------

static bool button_pressed(void)
{
    return gpio_get_level(BUTTON_GPIO) == 0;
}

void device_disable_up(bool disable)
{
    up_disabled = disable;
}

// Debounced: pressed on two reads 10 ms apart.
static bool button_down(void)
{
    if (!button_pressed())
        return false;
    vTaskDelay(pdMS_TO_TICKS(10));
    return button_pressed();
}

// The button as a presence source. Presence needs a fresh press: the button must be seen released
// after the wait starts, so a held or stuck button approves nothing.
static bool seen_released;

static void button_arm(void *ctx)
{
    (void)ctx;
    seen_released = false;
    ESP_LOGI(TAG, "user presence requested: press BOOT%s",
#ifdef CONFIG_TINYCRYPT_WATCH_BLE
             ble_presence_enrolled() ? " or approve on the watch" : "");
#else
             "");
#endif
}

static bool button_approved(void *ctx)
{
    (void)ctx;
    if (!button_pressed())
        seen_released = true;
    else if (seen_released && button_down())
        return true;
    return false;
}

static void report_result(void *ctx, int result)
{
    (void)ctx;
    ESP_LOGI(TAG, "presence %s", result ? "approved" : "not given (timed out or cancelled)");
}

static const presence_source button_source = {"button", NULL, button_arm, button_approved, report_result};

#ifdef CONFIG_TINYCRYPT_NOTIFIER
static void notifier_arm(void *ctx)
{
    (void)ctx;
    notifier_presence_wait_started();
}

static void notifier_disarm(void *ctx, int result)
{
    (void)ctx;
    (void)result;
    notifier_presence_wait_ended();
}

// Sends the push; never approves (the watch's answer comes back over BLE).
static const presence_source notifier_source = {"push", NULL, notifier_arm, NULL, notifier_disarm};
#endif

static int presence_recv(uint8_t report[PRESENCE_REPORT_SIZE], uint32_t timeout_ms)
{
    return usbhid_recv(report, pdMS_TO_TICKS(timeout_ms));
}

static const presence_io usb_io = {presence_recv, usbhid_send, millis};

// Contract with solo1: 1 = present, 0 = not present, 2 = check disabled by
// the request. NEVER return anything else. solo1's U2F code tests only `== 0`
// / `!ret`, so a -1 for "cancelled" would count as present and sign without
// a press. presence_wait() returns only 1 or 0.
int ctap_user_presence_test(uint32_t delay_ms)
{
    if (up_disabled)
        return 2;
    // The button first, so a press is never missed behind slower sources.
    const presence_source *sources[] = {
        &button_source,
#ifdef CONFIG_TINYCRYPT_WATCH_BLE
        &ble_presence_source,
#endif
#ifdef CONFIG_TINYCRYPT_NOTIFIER
        &notifier_source,
#endif
    };
    return presence_wait(delay_ms, sources, sizeof(sources) / sizeof(sources[0]), &usb_io);
}

// ---- rng, counter, state (INSECURE: plain NVS) ------------------------------

int ctap_generate_rng(uint8_t *dst, size_t num)
{
    // A true RNG needs an entropy source: the radio, or the SAR ADC noise that
    // bootloader_random_enable() switches on in app_main.
    esp_fill_random(dst, num);
    return 1;
}

static bool nvs_read(const char *key, void *data, size_t len)
{
    size_t got = len;
    return nvs_get_blob(nvs, key, data, &got) == ESP_OK && got == len;
}

static void nvs_write(const char *key, const void *data, size_t len)
{
    ESP_ERROR_CHECK(nvs_set_blob(nvs, key, data, len));
    ESP_ERROR_CHECK(nvs_commit(nvs));
}

uint32_t ctap_atomic_count(uint32_t amount)
{
    uint32_t counter = 0;
    esp_err_t err = nvs_get_u32(nvs, "counter", &counter);
    // Only a missing counter may start from 0. On any other read error, fail
    // closed (abort and reboot) rather than let the sign count go backwards.
    if (err != ESP_ERR_NVS_NOT_FOUND)
        ESP_ERROR_CHECK(err);
    counter += amount + 1;
    ESP_ERROR_CHECK(nvs_set_u32(nvs, "counter", counter));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    return counter;
}

int authenticator_read_state(AuthenticatorState *state)
{
    if (!nvs_read("state", state, sizeof(*state)))
        return 0;
    return state->is_initialized == INITIALIZED_MARKER;
}

void authenticator_write_state(AuthenticatorState *state)
{
    nvs_write("state", state, sizeof(*state));
}

static void sync_rk(void)
{
    nvs_write("rk", rk_store, sizeof(rk_store));
}

void ctap_reset_rk(void)
{
    memset(rk_store, 0xff, sizeof(rk_store));
    sync_rk();
}

uint32_t ctap_rk_size(void)
{
    return RK_NUM;
}

void ctap_store_rk(int index, CTAP_residentKey *rk)
{
    if (index < 0 || index >= RK_NUM)
        return;
    memmove(&rk_store[index], rk, sizeof(*rk));
    sync_rk();
}

void ctap_delete_rk(int index)
{
    if (index < 0 || index >= RK_NUM)
        return;
    memset(&rk_store[index], 0xff, sizeof(rk_store[index]));
    sync_rk();
}

void ctap_load_rk(int index, CTAP_residentKey *rk)
{
    if (index < 0 || index >= RK_NUM)
        return;
    memmove(rk, &rk_store[index], sizeof(*rk));
}

void ctap_overwrite_rk(int index, CTAP_residentKey *rk)
{
    ctap_store_rk(index, rk);
}

// ---- main -------------------------------------------------------------------

// Print the saved core dump's summary (task, PC, backtrace) at boot, so a crash
// can be read over the UART console without download mode. Decode with
// xtensa-esp32s3-elf-addr2line -pfiaC -e build/tinycrypt.elf <addresses>.
static void report_last_crash(void)
{
    if (esp_core_dump_image_check() != ESP_OK)
        return;
    esp_core_dump_summary_t summary;
    if (esp_core_dump_get_summary(&summary) != ESP_OK)
        return;
    printf("LAST CRASH: task '%s' pc 0x%08" PRIx32 " cause %" PRIu32 " vaddr 0x%08" PRIx32 "\nBacktrace:",
           summary.exc_task, summary.exc_pc, summary.ex_info.exc_cause, summary.ex_info.exc_vaddr);
    for (int i = 0; i < summary.exc_bt_info.depth; i++)
        printf(" 0x%08" PRIx32, summary.exc_bt_info.bt[i]);
    printf("%s\n", summary.exc_bt_info.corrupted ? " (corrupted)" : "");
}

#ifdef CONFIG_TINYCRYPT_WATCH_BLE
// Holding BOOT for 5 s while idle opens the watch enrol window. (Held at power-up, BOOT means
// download mode instead; during a presence wait, a press is presence.)
static void check_enrol_hold(void)
{
    static uint32_t held_since;
    static bool opened;
    if (!button_pressed())
    {
        held_since = 0;
        opened = false;
        return;
    }
    if (held_since == 0)
        held_since = millis() | 1;
    else if (!opened && millis() - held_since >= ENROL_HOLD_MS)
    {
        ble_presence_open_enrol(ENROL_WINDOW_S);
        opened = true;
    }
}
#endif

static void ctap_task(void *arg)
{
#ifdef TINYCRYPT_SE_SELFTEST
    // Here rather than app_main: two ECDSA verifies need more than the main task's 3.5 KB stack.
    se_selftest_run();
#endif
    ctaphid_init();
    ctap_init();
    ESP_LOGI(TAG, "CTAP core ready");

    uint8_t msg[REPORT_SIZE];
    UBaseType_t low_water = UINT32_MAX;
    for (;;)
    {
        if (usbhid_recv(msg, pdMS_TO_TICKS(10)) > 0)
        {
            presence_note_report(msg);
            ctaphid_handle_packet(msg);
            while (presence_take_deferred(msg))
            {
                presence_note_report(msg);
                ctaphid_handle_packet(msg);
            }
            // The presence wait runs inside ctaphid_handle_packet(); watch how close it gets
            // to the stack limit.
            UBaseType_t free_now = uxTaskGetStackHighWaterMark(NULL);
            if (free_now < low_water)
            {
                low_water = free_now;
                ESP_LOGI(TAG, "ctap task stack: %u bytes never used", (unsigned)low_water);
            }
        }
        ctaphid_check_timeouts();
#ifdef CONFIG_TINYCRYPT_WATCH_BLE
        check_enrol_hold();
#endif
    }
}

void app_main(void)
{
    printf("%s", TINYCRYPT_SOFT_KEY_BANNER);
    snprintf(serial_string, sizeof(serial_string), "0001-rr%d", (int)esp_reset_reason());
    report_last_crash();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs));
    if (!nvs_read("rk", rk_store, sizeof(rk_store)))
        ctap_reset_rk();

    // No radio is on, so use the ADC noise source for esp_fill_random().
    // Must be disabled again before Wi-Fi/BLE or the ADC are used.
    bootloader_random_enable();
    // The radio replaces the ADC as the RNG's entropy source. If no radio comes up, put the ADC
    // source back: credential keys and ECDSA nonces must never come from a bare PRNG.
    bool radio = false;
#ifdef CONFIG_TINYCRYPT_NOTIFIER
    radio |= notifier_start();
#endif
#ifdef CONFIG_TINYCRYPT_WATCH_BLE
    bootloader_random_disable();  // (again, if the notifier is off) before the radio starts
    radio |= ble_presence_start();
#endif
#if defined(CONFIG_TINYCRYPT_NOTIFIER) || defined(CONFIG_TINYCRYPT_WATCH_BLE)
    if (!radio)
    {
        ESP_LOGW(TAG, "no radio started: ADC entropy back on");
        bootloader_random_enable();
    }
#endif

    gpio_config_t button = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));

    set_logging_mask(TAG_ERR | TAG_RED | TAG_GREEN);
    rx_queue = xQueueCreate(16, REPORT_SIZE);
    if (rx_queue == NULL)
        abort();

    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = &device_descriptor,
        .string_descriptor = string_descriptor,
        .string_descriptor_count = sizeof(string_descriptor) / sizeof(string_descriptor[0]),
        .external_phy = false,
        .configuration_descriptor = configuration_descriptor,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    xTaskCreate(ctap_task, "ctap", 32 * 1024, NULL, 5, NULL);
}
