// tinycrypt ESP32-S3 port: the solo1 CTAP2 core behind TinyUSB FIDO HID.
//
// - Transport: one HID interface on the FIDO usage page (0xF1D0), 64-byte
//   reports on interrupt IN/OUT endpoints. TinyUSB runs in its own task and
//   queues OUT reports; the CTAP task drains the queue.
// - Presence: the BOOT button (GPIO0, active low) until the watch exists.
// - Keystore: INSECURE software key. Master secret, resident keys and the
//   sign counter live in plain NVS. TEST ONLY.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bootloader_random.h"
#include "class/hid/hid_device.h"
#include "driver/gpio.h"
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
#include APP_CONFIG

static const char *TAG = "tinycrypt";

#define BUTTON_GPIO GPIO_NUM_0
#define RK_NUM 10
#define REPORT_SIZE 64
#define NVS_NAMESPACE "tinycrypt"

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

static const char *string_descriptor[] = {
    (const char[]){0x09, 0x04}, // English
    "nakomis",
    "tinycrypt INSECURE TEST KEY",
    "0001",
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
    if (xQueueSend(rx_queue, msg, 0) != pdTRUE)
        ESP_LOGW(TAG, "rx queue full, dropping report");
}

// ---- transport hooks for the core ------------------------------------------

// Multi-packet replies queue reports back to back, so wait for the IN
// endpoint to drain the previous one (time-based: a tick may be longer than 1 ms).
void usbhid_send(uint8_t *msg)
{
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

// Keep servicing CTAPHID while waiting, so the host can CANCEL, and send a
// KEEPALIVE(UPNEEDED) every 100 ms as the spec asks.
static int pump(uint32_t ms)
{
    uint8_t msg[REPORT_SIZE];
    uint32_t until = millis() + ms;
    while ((int32_t)(until - millis()) > 0)
    {
        if (usbhid_recv(msg, pdMS_TO_TICKS(5)) > 0 && ctaphid_handle_packet(msg) == CTAPHID_CANCEL)
            return -1;
    }
    return 0;
}

// 1 = present, 0 = not present/timed out, 2 = check disabled, -1 = cancelled.
int ctap_user_presence_test(uint32_t delay_ms)
{
    if (up_disabled)
        return 2;
    ESP_LOGI(TAG, "user presence requested: press BOOT");
    uint32_t start = millis();
    while (millis() - start < delay_ms)
    {
        ctaphid_update_status(CTAPHID_STATUS_UPNEEDED);
        if (pump(100) < 0)
            return -1;
        if (button_pressed())
        {
            while (button_pressed() && millis() - start < delay_ms)
                pump(20);
            ESP_LOGI(TAG, "presence approved");
            return 1;
        }
    }
    ESP_LOGI(TAG, "presence timed out");
    return 0;
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
    nvs_get_u32(nvs, "counter", &counter);
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

static void ctap_task(void *arg)
{
    ctaphid_init();
    ctap_init();
    ESP_LOGI(TAG, "CTAP core ready");

    uint8_t msg[REPORT_SIZE];
    for (;;)
    {
        if (usbhid_recv(msg, pdMS_TO_TICKS(10)) > 0)
            ctaphid_handle_packet(msg);
        ctaphid_check_timeouts();
    }
}

void app_main(void)
{
    printf("%s", TINYCRYPT_SOFT_KEY_BANNER);

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

    gpio_config_t button = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));

    set_logging_mask(TAG_ERR | TAG_RED | TAG_GREEN);
    rx_queue = xQueueCreate(16, REPORT_SIZE);

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
