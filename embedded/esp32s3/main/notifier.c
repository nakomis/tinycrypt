// Notifier port (CRYPT-15): Wi-Fi + SNTP + MQTT to AWS IoT Core. See notifier.h.
//
// The published message is already in SNS's JSON message structure, because the IoT rule hands it
// straight to SNS (no Lambda):
//   {"default": "...", "APNS_SANDBOX": "<the APNs payload as a JSON string>"}
// tc.sentAt (ms since the epoch, from SNTP) lets the watch time the trip and refuse stale requests.
#include "notifier.h"

#include <inttypes.h>
#include <string.h>
#include <sys/time.h>

#include "bootloader_random.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

static const char *TAG = "notifier";

extern const char root_ca_start[] asm("_binary_AmazonRootCA1_pem_start");
extern const char device_cert_start[] asm("_binary_device_pem_crt_start");
extern const char device_key_start[] asm("_binary_device_pem_key_start");

static esp_mqtt_client_handle_t mqtt;
static volatile bool mqtt_connected;
static int64_t last_publish_us;  // for timing the PUBACK; one request in flight at a time

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
        esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d), retrying",
                 ((wifi_event_sta_disconnected_t *)data)->reason);
        esp_wifi_connect();
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
        ESP_LOGI(TAG, "Wi-Fi up, IP " IPSTR, IP2STR(&((ip_event_got_ip_t *)data)->ip_info.ip));
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t event = data;
    switch ((esp_mqtt_event_id_t)id)
    {
    case MQTT_EVENT_CONNECTED:
        mqtt_connected = true;
        ESP_LOGI(TAG, "MQTT connected to IoT Core as %s", CONFIG_TINYCRYPT_THING_NAME);
        break;
    case MQTT_EVENT_DISCONNECTED:
        mqtt_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "presence request acknowledged by IoT Core after %" PRId64 " ms",
                 (esp_timer_get_time() - last_publish_us) / 1000);
        break;
    case MQTT_EVENT_ERROR:
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
            ESP_LOGW(TAG, "MQTT transport error: tls 0x%x, stack 0x%x, errno %d",
                     event->error_handle->esp_tls_last_esp_err, event->error_handle->esp_tls_stack_err,
                     event->error_handle->esp_transport_sock_errno);
        else
            ESP_LOGW(TAG, "MQTT error type %d, connect return code %d", event->error_handle->error_type,
                     event->error_handle->connect_return_code);
        break;
    default:
        break;
    }
}

void notifier_start(void)
{
    // The radio becomes the RNG's entropy source; the ADC one must be off before Wi-Fi starts.
    bootloader_random_disable();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));

    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, CONFIG_TINYCRYPT_WIFI_SSID, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, CONFIG_TINYCRYPT_WIFI_PASSWORD, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    // No modem sleep: a held-open connection should answer at once, not on the next DTIM beacon.
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp));

    const esp_mqtt_client_config_t config = {
        .broker = {
            .address.uri = "mqtts://" CONFIG_TINYCRYPT_IOT_ENDPOINT ":8883",
            .verification.certificate = root_ca_start,
        },
        .credentials = {
            .client_id = CONFIG_TINYCRYPT_THING_NAME,
            .authentication = {.certificate = device_cert_start, .key = device_key_start},
        },
        .session.keepalive = 60,
        .network.reconnect_timeout_ms = 5000,
    };
    mqtt = esp_mqtt_client_init(&config);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt, ESP_EVENT_ANY_ID, mqtt_event, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt));
    ESP_LOGI(TAG, "notifier starting: Wi-Fi \"%s\", IoT Core %s", CONFIG_TINYCRYPT_WIFI_SSID,
             CONFIG_TINYCRYPT_IOT_ENDPOINT);
}

static int64_t epoch_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    // Before SNTP has synced the clock starts at 1970; don't send a time the watch would misread.
    return tv.tv_sec > 1700000000 ? (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000 : 0;
}

bool notifier_presence_request(void)
{
    if (!mqtt_connected)
    {
        ESP_LOGW(TAG, "not connected: no presence request sent (the button still works)");
        return false;
    }

    uint8_t raw[8];
    esp_fill_random(raw, sizeof(raw));
    char id[2 * sizeof(raw) + 1];
    for (size_t i = 0; i < sizeof(raw); i++)
        snprintf(&id[2 * i], 3, "%02x", raw[i]);
    int64_t sent_at = epoch_ms();

    // TODO(BLE firmware): add the key id and a hash of the CTAP request, bound into the BLE
    // challenge, so a late or replayed push can't approve a different request.
    cJSON *apns = cJSON_CreateObject();
    cJSON *aps = cJSON_AddObjectToObject(apns, "aps");
    cJSON *alert = cJSON_AddObjectToObject(aps, "alert");
    cJSON_AddStringToObject(alert, "title", "Sign in?");
    cJSON_AddStringToObject(alert, "body", "Your security key wants approval");
    cJSON_AddStringToObject(aps, "category", "PRESENCE");
    cJSON_AddStringToObject(aps, "sound", "default");
    cJSON *tc = cJSON_AddObjectToObject(apns, "tc");
    cJSON_AddStringToObject(tc, "id", id);
    cJSON_AddStringToObject(tc, "key", CONFIG_TINYCRYPT_THING_NAME);
    if (sent_at)
        cJSON_AddNumberToObject(tc, "sentAt", (double)sent_at);
    char *apns_text = cJSON_PrintUnformatted(apns);

    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "default", "tinycrypt presence request");
    cJSON_AddStringToObject(message, CONFIG_TINYCRYPT_APNS_PLATFORM, apns_text);
    char *text = cJSON_PrintUnformatted(message);

    last_publish_us = esp_timer_get_time();
    // Enqueue, not publish: the MQTT task sends it, so the CTAP task never blocks on the network.
    int msg_id = esp_mqtt_client_enqueue(mqtt, CONFIG_TINYCRYPT_PRESENCE_TOPIC, text, 0, 1, 0, true);
    ESP_LOGI(TAG, "presence request %s queued (msg %d, sentAt %" PRId64 ")", id, msg_id, sent_at);

    cJSON_free(text);
    cJSON_Delete(message);
    cJSON_free(apns_text);
    cJSON_Delete(apns);
    return msg_id >= 0;
}
