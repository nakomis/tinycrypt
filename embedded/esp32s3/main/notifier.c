// Notifier port (CRYPT-15): Wi-Fi + SNTP + MQTT to AWS IoT Core. See notifier.h.
//
// The published message is already in SNS's JSON message structure, because the IoT rule hands it
// straight to SNS (no Lambda):
//   {"default": "...", "APNS_SANDBOX": "<the APNs payload as a JSON string>"}
// tc.sentAt (ms since the epoch, from SNTP) lets the watch time the trip and refuse stale requests.
// The push carries no authority: approval only counts once the watch signs the key's BLE challenge.
#include "notifier.h"
#include "led_status.h"
#include "presence.h"

#include <inttypes.h>
#include <stdio.h>
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
#include "freertos/FreeRTOS.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

static const char *TAG = "notifier";

extern const char root_ca_start[] asm("_binary_AmazonRootCA1_pem_start");
extern const char device_cert_start[] asm("_binary_device_pem_crt_start");
extern const char device_key_start[] asm("_binary_device_pem_key_start");

// Wi-Fi reconnect backoff, so a wrong password or a missing AP doesn't spin the radio.
#define WIFI_RETRY_MIN_MS 1000
#define WIFI_RETRY_MAX_MS 30000
// Any time after this (Nov 2023) means SNTP has set the clock; before it, it's counting from 1970.
#define CLOCK_SET_AFTER_S 1700000000

static esp_mqtt_client_handle_t mqtt;
static volatile bool mqtt_connected;

// A request made while not connected (e.g. a sign-in seconds after the key is plugged in) is held
// and sent as soon as MQTT connects, if its wait is still running.
static portMUX_TYPE pending_lock = portMUX_INITIALIZER_UNLOCKED;
static bool pending;
// Before the first connection the LED says "connecting", and "offline" once it has connected
// before or CONNECT_DEADLINE_MS has passed without a connection (bad credentials, AP down).
static volatile bool ever_connected;
static volatile bool connect_overdue;
static esp_timer_handle_t connect_deadline;
#define CONNECT_DEADLINE_MS 20000
static esp_timer_handle_t wifi_retry;
static uint32_t wifi_retry_ms = WIFI_RETRY_MIN_MS;
static bool started;


static void send_request(void);

static void wifi_retry_fire(void *arg)
{
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK)
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
}

static void connect_deadline_fire(void *arg)
{
    if (mqtt_connected)
        return;
    connect_overdue = true;
    ESP_LOGW(TAG, "not connected after %d s: showing offline (the button works)", CONNECT_DEADLINE_MS / 1000);
    led_status_set(LED_CONNECTING, false);
    led_status_set(LED_OFFLINE, true);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
        wifi_retry_fire(NULL);
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        // MQTT only notices a dead socket at its next keepalive; stop queueing into it now.
        mqtt_connected = false;
        if (ever_connected || connect_overdue)
            led_status_set(LED_OFFLINE, true);
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d), retrying in %" PRIu32 " ms",
                 ((wifi_event_sta_disconnected_t *)data)->reason, wifi_retry_ms);
        esp_timer_start_once(wifi_retry, (uint64_t)wifi_retry_ms * 1000);
        wifi_retry_ms = wifi_retry_ms * 2 > WIFI_RETRY_MAX_MS ? WIFI_RETRY_MAX_MS : wifi_retry_ms * 2;
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        wifi_retry_ms = WIFI_RETRY_MIN_MS;
        ESP_LOGI(TAG, "Wi-Fi up, IP " IPSTR, IP2STR(&((ip_event_got_ip_t *)data)->ip_info.ip));
    }
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t event = data;
    switch ((esp_mqtt_event_id_t)id)
    {
    case MQTT_EVENT_CONNECTED:
    {
        mqtt_connected = true;
        ESP_LOGI(TAG, "MQTT connected to IoT Core as %s", CONFIG_TINYCRYPT_THING_NAME);
        led_status_set(LED_CONNECTING, false);
        led_status_set(LED_OFFLINE, false);
        if (!ever_connected)
            led_status_flash(LED_FLASH_ONLINE);
        ever_connected = true;
        taskENTER_CRITICAL(&pending_lock);
        bool send_now = pending;
        pending = false;
        taskEXIT_CRITICAL(&pending_lock);
        if (send_now)
        {
            ESP_LOGI(TAG, "sending the presence request held while offline");
            send_request();
        }
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        mqtt_connected = false;
        if (ever_connected || connect_overdue)
            led_status_set(LED_OFFLINE, true);
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "presence request (msg %d) acknowledged by IoT Core", event->msg_id);
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

// Logs and gives up rather than aborting: the button must keep working without the notifier.
#define TRY(x)                                                                         \
    do                                                                                 \
    {                                                                                  \
        esp_err_t err_ = (x);                                                          \
        if (err_ != ESP_OK)                                                            \
        {                                                                              \
            ESP_LOGE(TAG, "%s failed: %s; notifier off", #x, esp_err_to_name(err_)); \
            return radio_on;                                                           \
        }                                                                              \
    } while (0)

bool notifier_start(void)
{
    bool radio_on = false;
    // The radio becomes the RNG's entropy source; the ADC one must be off before Wi-Fi starts.
    bootloader_random_disable();

    TRY(esp_netif_init());
    TRY(esp_event_loop_create_default());
    if (esp_netif_create_default_wifi_sta() == NULL)
    {
        ESP_LOGE(TAG, "no Wi-Fi station interface; notifier off");
        return false;
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    TRY(esp_wifi_init(&init));
    const esp_timer_create_args_t retry = {.callback = wifi_retry_fire, .name = "wifi_retry"};
    TRY(esp_timer_create(&retry, &wifi_retry));
    const esp_timer_create_args_t deadline = {.callback = connect_deadline_fire, .name = "connect_deadline"};
    TRY(esp_timer_create(&deadline, &connect_deadline));
    TRY(esp_timer_start_once(connect_deadline, (uint64_t)CONNECT_DEADLINE_MS * 1000));
    TRY(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    TRY(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));

    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, CONFIG_TINYCRYPT_WIFI_SSID, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, CONFIG_TINYCRYPT_WIFI_PASSWORD, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    TRY(esp_wifi_set_mode(WIFI_MODE_STA));
    TRY(esp_wifi_set_config(WIFI_IF_STA, &sta));
    TRY(esp_wifi_start());
    radio_on = true;  // from here on the radio is the entropy source
#ifdef CONFIG_BT_ENABLED
    // Wi-Fi and BLE share the radio, and ESP-IDF requires modem sleep for that. A push then waits
    // for the next beacon at most (~100 ms with DTIM 1).
    TRY(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
#else
    // No modem sleep: a held-open connection should answer at once, not on the next DTIM beacon.
    TRY(esp_wifi_set_ps(WIFI_PS_NONE));
#endif

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    TRY(esp_netif_sntp_init(&sntp));

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
    if (mqtt == NULL)
    {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed; notifier off");
        return radio_on;
    }
    TRY(esp_mqtt_client_register_event(mqtt, ESP_EVENT_ANY_ID, mqtt_event, NULL));
    TRY(esp_mqtt_client_start(mqtt));
    started = true;
    ESP_LOGI(TAG, "notifier starting: Wi-Fi \"%s\", IoT Core %s, topic %s", CONFIG_TINYCRYPT_WIFI_SSID,
             CONFIG_TINYCRYPT_IOT_ENDPOINT, CONFIG_TINYCRYPT_PRESENCE_TOPIC);
    return radio_on;
}

static int64_t epoch_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    // Before SNTP has set the clock, send no time rather than one the watch would misread.
    return tv.tv_sec > CLOCK_SET_AFTER_S ? (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000 : 0;
}

static void send_request(void)
{
    if (!started)
        return;
    if (!mqtt_connected)
    {
        taskENTER_CRITICAL(&pending_lock);
        pending = true;
        taskEXIT_CRITICAL(&pending_lock);
        ESP_LOGW(TAG, "not connected yet: presence request held until MQTT connects (the button works)");
        return;
    }

    uint8_t raw[8];
    esp_fill_random(raw, sizeof(raw));
    char id[2 * sizeof(raw) + 1];
    for (size_t i = 0; i < sizeof(raw); i++)
        snprintf(&id[2 * i], 3, "%02x", raw[i]);
    int64_t sent_at = epoch_ms();

    // The request this push is for (CRYPT-18): the watch only signs a BLE challenge carrying the
    // same hash, and shows the site so you know what you're approving.
    const char *rp = presence_request_rp();
    const uint8_t *hash = presence_request_hash();
    char req[2 * PRESENCE_REQUEST_HASH_LEN + 1];
    for (int i = 0; i < PRESENCE_REQUEST_HASH_LEN; i++)
        snprintf(&req[2 * i], 3, "%02x", hash[i]);
    char body[PRESENCE_RP_MAX + 32];
    if (rp[0])
        snprintf(body, sizeof(body), "Sign in to %s?", rp);
    else
        snprintf(body, sizeof(body), "Your security key wants approval");

    cJSON *apns = cJSON_CreateObject();
    cJSON *aps = cJSON_AddObjectToObject(apns, "aps");
    cJSON *alert = cJSON_AddObjectToObject(aps, "alert");
    cJSON_AddStringToObject(alert, "title", "Sign in?");
    cJSON_AddStringToObject(alert, "body", body);
    cJSON_AddStringToObject(aps, "category", "PRESENCE");
    cJSON_AddStringToObject(aps, "sound", "default");
    cJSON *tc = cJSON_AddObjectToObject(apns, "tc");
    cJSON_AddStringToObject(tc, "id", id);
    cJSON_AddStringToObject(tc, "key", CONFIG_TINYCRYPT_THING_NAME);
    cJSON_AddStringToObject(tc, "req", req);
    if (rp[0])
        cJSON_AddStringToObject(tc, "rp", rp);
    if (sent_at)
        cJSON_AddNumberToObject(tc, "sentAt", (double)sent_at);
    char *apns_text = cJSON_PrintUnformatted(apns);

    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "default", "tinycrypt presence request");
    if (apns_text)
        cJSON_AddStringToObject(message, CONFIG_TINYCRYPT_APNS_PLATFORM, apns_text);
    char *text = apns_text ? cJSON_PrintUnformatted(message) : NULL;

    if (text == NULL)
        ESP_LOGE(TAG, "out of memory building the presence request");
    else
    {
        // Enqueued for the MQTT task to send, so the CTAP task doesn't wait on the network. QoS 1
        // for the PUBACK in the log; the outbox drops anything unsent after 10 s
        // (CONFIG_MQTT_OUTBOX_EXPIRED_TIMEOUT_MS), so a reconnect can't deliver a stale prompt.
        int msg_id = esp_mqtt_client_enqueue(mqtt, CONFIG_TINYCRYPT_PRESENCE_TOPIC, text, 0, 1, 0, true);
        ESP_LOGI(TAG, "presence request %s queued (msg %d, sentAt %" PRId64 ")", id, msg_id, sent_at);
    }

    cJSON_free(text);
    cJSON_Delete(message);
    cJSON_free(apns_text);
    cJSON_Delete(apns);
}

void notifier_presence_wait_started(void)
{
    // One push per user action (presence.h): U2F's 750 ms re-asks don't each buzz the watch.
    if (presence_is_new_action())
        send_request();
}

void notifier_presence_wait_ended(void)
{
    // A request still held for a connection is stale once its wait is over.
    taskENTER_CRITICAL(&pending_lock);
    pending = false;
    taskEXIT_CRITICAL(&pending_lock);
}
