// Metrics to the house VictoriaMetrics (CRYPT-28). See metrics.h.
//
// One InfluxDB line-protocol POST a minute to vmauth on Cal (prov's metrics_url + "/write"), plain
// HTTP with basic auth: no TLS on the LAN (house rule), and the password is write-only and assumed
// public anyway (it sits in plain flash). vmauth (HOME-437) has a user per device and pins
// host_name to it server-side, so the host_name sent here is overridden and a leaked password can
// only write as this key. VictoriaMetrics names the series tinycrypt_<field>.
//
//   tinycrypt,host_name=<thing>,version=<x.y.z> heap_free=…i,heap_largest=…i,heap_min=…i,
//       uptime_s=…i,rssi=…i,mqtt=1i,watch_enrolled=1i,ble_connections=…i
//
// Deliberately NOT sent: anything about sign-ins (when, how many, which site). That would put a
// record of Martin's sign-ins on the LAN.
//
// Never in the way: a minute is skipped while Wi-Fi is down, an update is running, or the largest
// free block is small (an HTTP client needs a few KB and RAM is this board's limit); a failed POST
// is dropped, not retried, with one log line per run of failures. The task runs at the lowest
// priority and every call has a short timeout.
#include "metrics.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_flash_encrypt.h"
#include "esp_secure_boot.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "metrics_line.h"
#include "notifier.h"
#include "ota.h"
#include "sdkconfig.h"
#ifdef CONFIG_TINYCRYPT_WATCH_BLE
#include "ble_presence.h"
#endif

static const char *TAG = "metrics";

#define FIRST_DELAY_MS 30000  // let Wi-Fi, MQTT and the probation self-test settle first
#define MIN_LARGEST_BLOCK 8192
#define TIMEOUT_MS 3000

static const prov_t *prov;
static char url[sizeof(((prov_t *)0)->metrics_url) + 8];
static char auth[8 + 4 * ((sizeof(((prov_t *)0)->metrics_user) + sizeof(((prov_t *)0)->metrics_pass) + 2) / 3) + 1];

static bool post(const char *body, int len, int *status)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = TIMEOUT_MS,
        .disable_auto_redirect = true,  // the credential goes to vmauth and nowhere else
        .buffer_size = 512,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL)
        return false;
    esp_http_client_set_header(client, "Content-Type", "text/plain");
    if (auth[0])
        esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, body, len);
    esp_err_t err = esp_http_client_perform(client);
    *status = err == ESP_OK ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);
    return err == ESP_OK && *status / 100 == 2;
}

static void metrics_task(void *arg)
{
    (void)arg;
    bool failing = false;
    vTaskDelay(pdMS_TO_TICKS(FIRST_DELAY_MS));
    for (;;)
    {
        wifi_ap_record_t ap;
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK && !ota_busy() && largest >= MIN_LARGEST_BLOCK)
        {
            metrics_field_t fields[] = {
                {"heap_free", (int64_t)esp_get_free_heap_size()},
                {"heap_largest", (int64_t)largest},
                {"heap_min", (int64_t)esp_get_minimum_free_heap_size()},
                {"uptime_s", esp_timer_get_time() / 1000000},
                {"rssi", ap.rssi},
                // CRYPT-34: 1 once secure boot and flash encryption are both on (the chip's eFuses, not the build).
                {"secured", esp_secure_boot_enabled() && esp_flash_encryption_enabled()},
                {"mqtt", notifier_mqtt_connected()},
#ifdef CONFIG_TINYCRYPT_WATCH_BLE
                {"watch_enrolled", ble_presence_enrolled()},
                {"ble_connections", ble_presence_connections()},
#endif
            };
            char line[320];
            int len = metrics_line(line, sizeof(line), "tinycrypt", prov->thing_name, esp_app_get_description()->version,
                                   fields, sizeof(fields) / sizeof(fields[0]));
            int status = 0;
            if (len < 0)
                ESP_LOGE(TAG, "metrics line didn't fit");
            else if (post(line, len, &status))
            {
                if (failing)
                    ESP_LOGI(TAG, "sending again");
                failing = false;
            }
            else if (!failing)
            {
                failing = true;  // once per run of failures: Cal reboots, and that's fine
                ESP_LOGW(TAG, "POST %s failed (HTTP %d); dropping metrics until it works", url, status);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_TINYCRYPT_METRICS_INTERVAL_S * 1000));
    }
}

void metrics_start(const prov_t *p)
{
    if (p == NULL || p->metrics_url[0] == 0)
    {
        ESP_LOGI(TAG, "no metrics URL in prov: metrics off");
        return;
    }
    prov = p;
    snprintf(url, sizeof(url), "%s/write", p->metrics_url);
    auth[0] = 0;
    if (p->metrics_user[0])
    {
        char creds[sizeof(p->metrics_user) + sizeof(p->metrics_pass) + 1];
        int n = snprintf(creds, sizeof(creds), "%s:%s", p->metrics_user, p->metrics_pass);
        size_t olen = 0;
        memcpy(auth, "Basic ", 6);
        if (mbedtls_base64_encode((unsigned char *)auth + 6, sizeof(auth) - 6, &olen, (unsigned char *)creds, n) != 0)
            auth[0] = 0;
        memset(creds, 0, sizeof(creds));
    }
    if (xTaskCreate(metrics_task, "metrics", 4096, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS)
        ESP_LOGW(TAG, "no memory for the metrics task: metrics off");
    else
        ESP_LOGI(TAG, "metrics every %d s to %s as %s", CONFIG_TINYCRYPT_METRICS_INTERVAL_S, url,
                 p->metrics_user[0] ? p->metrics_user : "(no user)");
}
