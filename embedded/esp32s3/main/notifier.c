// Notifier port (CRYPT-15): Wi-Fi + SNTP + MQTT to AWS IoT Core. See notifier.h.
//
// The published message is already in SNS's JSON message structure, because the IoT rule hands it
// straight to SNS (no Lambda):
//   {"default": "...", "APNS_SANDBOX": "<the APNs payload as a JSON string>"}
// tc.sentAt (ms since the epoch, from SNTP) lets the watch time the trip and refuse stale requests.
// The push carries no authority: approval only counts once the watch signs the key's BLE challenge.
//
// Fleet (CRYPT-14): the key also subscribes to tinycrypt/<thing>/commands and answers on
// tinycrypt/<thing>/responses; ota.c handles the commands. Its settings and certificate come from
// the prov partition (prov.h), not the image.
//
// CRYPT-30: Wi-Fi set from the iPhone app over BLE (notifier_set_wifi) is kept in NVS and wins over
// prov's; a key can be provisioned with none. Devices enrolled over BLE are registered for that key's
// pushes by relaying their APNs tokens to tinycrypt/{env}/register/{thing} (notifier_register).
#include "notifier.h"
#include "led_status.h"
#include "ota.h"
#include "presence.h"
#include "prov.h"
#include "se_tls.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "nvs.h"
#include "sdkconfig.h"
#ifdef CONFIG_TINYCRYPT_WATCH_BLE
#include "ble_presence.h"
#endif

static const char *TAG = "notifier";

extern const char root_ca_start[] asm("_binary_AmazonRootCA1_pem_start");

// Wi-Fi reconnect backoff, so a wrong password or a missing AP doesn't spin the radio.
#define WIFI_RETRY_MIN_MS 1000
#define WIFI_RETRY_MAX_MS 30000
// Any time after this (Nov 2023) means SNTP has set the clock; before it, it's counting from 1970.
#define CLOCK_SET_AFTER_S 1700000000

// Fleet's commands can carry a presigned URL of ~1.5 KB (longer ones arrive in chunks).
#define MQTT_BUFFER_SIZE 4096

static prov_t prov;
static char commands_topic[sizeof(prov.thing_name) + 32];
static char responses_topic[sizeof(prov.thing_name) + 32];
static char register_topic[sizeof(prov.presence_topic)];
static int subscribe_msg_id = -1;
static esp_mqtt_client_handle_t mqtt;
static volatile bool mqtt_connected;

// A request made while not connected (e.g. a sign-in seconds after the key is plugged in) is held
// and sent as soon as MQTT connects, if its wait is still running.
static portMUX_TYPE pending_lock = portMUX_INITIALIZER_UNLOCKED;
static bool pending;
// The request a push is for, copied in the CTAP task when its wait starts (under pending_lock):
// the MQTT task may send a held push while the next wait is rewriting presence's own copy.
static char held_rp[PRESENCE_RP_MAX];
static char held_label[PRESENCE_LABEL_MAX];
static uint8_t held_hash[PRESENCE_REQUEST_HASH_LEN];
// Before the first connection the LED says "connecting", and "offline" once it has connected
// before or CONNECT_DEADLINE_MS has passed without a connection (bad credentials, AP down).
static volatile bool ever_connected;
static volatile bool connect_overdue;
static esp_timer_handle_t connect_deadline;
#define CONNECT_DEADLINE_MS 20000
static esp_timer_handle_t wifi_retry;
static uint32_t wifi_retry_ms = WIFI_RETRY_MIN_MS;
static bool started;
static bool prov_loaded;  // prov is valid (metrics can run even if MQTT setup failed)

// CRYPT-30: the network in use (NVS's, else prov's) and how joining it is going, for the app.
#define WIFI_NVS "wifi"
#define REGISTER_NVS "register"
static portMUX_TYPE wifi_lock = portMUX_INITIALIZER_UNLOCKED;
static bool wifi_started;
static char wifi_ssid[SETUP_SSID_MAX + 1];
static volatile setup_wifi_state_t wifi_state = SETUP_WIFI_UNSET;
static volatile bool reconfiguring;  // our own disconnect, to switch networks: don't back off
static setup_network_t scan_nets[SETUP_SCAN_MAX];
static size_t scan_count;
static volatile bool scanning;
// Registrations being published, so a PUBACK clears the right one (and not a newer token).
static int register_msg_id[2] = {-1, -1};
static char register_sent[2][SETUP_TOKEN_MAX + 1];


static void send_request(void);
static void publish_registrations(void);
static void registration_acked(setup_kind_t kind);

static void wifi_retry_fire(void *arg)
{
    if (wifi_state == SETUP_WIFI_UNSET)
        return;  // no network yet: the app sets one (CRYPT-30)
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

// Keeps the strongest SETUP_SCAN_MAX networks (once each) for the app's wifi read.
static void scan_done(void)
{
    uint16_t n = 20;
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    setup_network_t *nets = calloc(n, sizeof(*nets));
    if (recs == NULL || nets == NULL || esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK)
        n = 0;
    for (uint16_t i = 0; i < n; i++)
    {
        strlcpy(nets[i].ssid, (const char *)recs[i].ssid, sizeof(nets[i].ssid));
        nets[i].rssi = recs[i].rssi;
        nets[i].secured = recs[i].authmode != WIFI_AUTH_OPEN;
    }
    if (recs == NULL || nets == NULL)
        esp_wifi_clear_ap_list();
    size_t kept = nets ? setup_scan_tidy(nets, n) : 0;
    taskENTER_CRITICAL(&wifi_lock);
    if (nets)
        memcpy(scan_nets, nets, kept * sizeof(*nets));
    scan_count = kept;
    taskEXIT_CRITICAL(&wifi_lock);
    scanning = false;
    free(recs);
    free(nets);
    ESP_LOGI(TAG, "Wi-Fi scan: %u networks", (unsigned)kept);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
        wifi_retry_fire(NULL);
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        uint8_t reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        // MQTT only notices a dead socket at its next keepalive; stop queueing into it now.
        mqtt_connected = false;
        if (reconfiguring && reason == WIFI_REASON_ASSOC_LEAVE)
        {
            reconfiguring = false;  // notifier_set_wifi left the old network and is joining the new
            return;
        }
        switch (reason)
        {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_MIC_FAILURE:
            wifi_state = SETUP_WIFI_WRONG_PASSWORD;
            break;
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            wifi_state = SETUP_WIFI_NOT_FOUND;
            break;
        default:
            if (wifi_state != SETUP_WIFI_UNSET)
                wifi_state = SETUP_WIFI_CONNECTING;  // it keeps retrying
            break;
        }
        if (ever_connected || connect_overdue)
            led_status_set(LED_OFFLINE, true);
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d), retrying in %" PRIu32 " ms", reason, wifi_retry_ms);
        esp_timer_start_once(wifi_retry, (uint64_t)wifi_retry_ms * 1000);
        wifi_retry_ms = wifi_retry_ms * 2 > WIFI_RETRY_MAX_MS ? WIFI_RETRY_MAX_MS : wifi_retry_ms * 2;
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE)
        scan_done();
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        wifi_retry_ms = WIFI_RETRY_MIN_MS;
        wifi_state = SETUP_WIFI_CONNECTED;
        reconfiguring = false;
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
        ESP_LOGI(TAG, "MQTT connected to IoT Core as %s", prov.thing_name);
        // A clean session each time, so subscribe on every connect.
        subscribe_msg_id = esp_mqtt_client_subscribe(mqtt, commands_topic, 1);
        led_status_set(LED_CONNECTING, false);
        led_status_set(LED_OFFLINE, false);
        if (!ever_connected)
            led_status_flash(LED_FLASH_ONLINE);
        ever_connected = true;
        publish_registrations();
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
    case MQTT_EVENT_SUBSCRIBED:
        // A SUBACK can refuse (0x80): then the key can't take updates, and a new image isn't kept.
        if (event->msg_id == subscribe_msg_id && event->error_handle->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED)
            ESP_LOGE(TAG, "subscription to %s refused", commands_topic);
        else if (event->msg_id == subscribe_msg_id)
        {
            ESP_LOGI(TAG, "listening for Fleet on %s", commands_topic);
            ota_on_connected();
        }
        break;
    case MQTT_EVENT_DATA:
        // Commands only (the one subscription). One that didn't fit the buffer arrives in pieces:
        // refuse it rather than parse a fragment.
        if (event->current_data_offset != 0)
            break;
        if (event->total_data_len != event->data_len)
        {
            ESP_LOGW(TAG, "command of %d bytes doesn't fit the %d-byte buffer", event->total_data_len, MQTT_BUFFER_SIZE);
            notifier_respond("{\"type\":\"error\",\"message\":\"Command too long\"}");
            break;
        }
        ota_handle_command(event->data, (size_t)event->data_len);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "msg %d acknowledged by IoT Core", event->msg_id);
        for (int kind = 0; kind < 2; kind++)
            if (event->msg_id == register_msg_id[kind])
                registration_acked((setup_kind_t)kind);
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

static esp_err_t configure_wifi(const char *ssid, const char *password)
{
    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, password, sizeof(sta.sta.password));
    // An open network only when the app set one with no password; otherwise WPA2 or better.
    sta.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    memset(&sta, 0, sizeof(sta));
    return err;
}

// The network the app set (CRYPT-30), if any.
static bool load_wifi(char ssid[SETUP_SSID_MAX + 1], char password[SETUP_PASSWORD_MAX + 1])
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS, NVS_READONLY, &h) != ESP_OK)
        return false;
    size_t len = SETUP_SSID_MAX + 1;
    bool ok = nvs_get_str(h, "ssid", ssid, &len) == ESP_OK && ssid[0];
    len = SETUP_PASSWORD_MAX + 1;
    ok = ok && nvs_get_str(h, "pass", password, &len) == ESP_OK;
    nvs_close(h);
    if (!ok)
        ssid[0] = password[0] = 0;
    return ok;
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
    if (!prov_load(&prov))
    {
        ESP_LOGE(TAG, "notifier off: the key isn't provisioned (the button works)");
        return false;
    }
    prov_loaded = true;
    snprintf(commands_topic, sizeof(commands_topic), "tinycrypt/%s/commands", prov.thing_name);
    snprintf(responses_topic, sizeof(responses_topic), "tinycrypt/%s/responses", prov.thing_name);
    // tinycrypt/{env}/presence/{thing}/request -> tinycrypt/{env}/register/{thing}
    const char *presence = strstr(prov.presence_topic, "/presence/");
    if (presence != NULL)
        snprintf(register_topic, sizeof(register_topic), "%.*s/register/%s", (int)(presence - prov.presence_topic),
                 prov.presence_topic, prov.thing_name);

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

    // The app's network (CRYPT-30), else prov's, else none yet: the radio still starts, for scanning.
    char password[SETUP_PASSWORD_MAX + 1] = "";
    if (!load_wifi(wifi_ssid, password))
    {
        strlcpy(wifi_ssid, prov.wifi_ssid, sizeof(wifi_ssid));
        strlcpy(password, prov.wifi_password, sizeof(password));
    }
    wifi_state = wifi_ssid[0] ? SETUP_WIFI_CONNECTING : SETUP_WIFI_UNSET;
    TRY(esp_wifi_set_mode(WIFI_MODE_STA));
    esp_err_t config_err = configure_wifi(wifi_ssid, password);
    memset(password, 0, sizeof(password));
    TRY(config_err);
    TRY(esp_wifi_start());
    wifi_started = true;
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

    // The TLS client key: the secure element's (a PEM that signs on the element), unless this
    // soft-key key still has a pre-CRYPT-22 one in prov.
    char *client_key = prov.device_key ? prov.device_key : se_tls_key_pem();
    if (client_key == NULL)
    {
        ESP_LOGE(TAG, "no TLS client key from the secure element; notifier off");
        return radio_on;
    }

    static char uri[sizeof(prov.iot_endpoint) + 16];
    snprintf(uri, sizeof(uri), "mqtts://%s:8883", prov.iot_endpoint);
    const esp_mqtt_client_config_t config = {
        .broker = {
            .address.uri = uri,
            .verification.certificate = root_ca_start,
        },
        .credentials = {
            .client_id = prov.thing_name,
            .authentication = {.certificate = prov.device_cert, .key = client_key},
        },
        .session.keepalive = 60,
        .network.reconnect_timeout_ms = 5000,
        .buffer.size = MQTT_BUFFER_SIZE,
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
    ESP_LOGI(TAG, "notifier starting: Wi-Fi \"%s\", IoT Core %s, topic %s", wifi_ssid[0] ? wifi_ssid : "(none yet)",
             prov.iot_endpoint, prov.presence_topic);
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
    // same hash, and shows the site so you know what you're approving. presence only binds a site
    // that is a plain host name, so it can go into the alert as it is.
    char rp[PRESENCE_RP_MAX];
    char label[PRESENCE_LABEL_MAX];
    uint8_t hash[PRESENCE_REQUEST_HASH_LEN];
    taskENTER_CRITICAL(&pending_lock);
    memcpy(rp, held_rp, sizeof(rp));
    memcpy(label, held_label, sizeof(label));
    memcpy(hash, held_hash, sizeof(hash));
    taskEXIT_CRITICAL(&pending_lock);
    char req[2 * PRESENCE_REQUEST_HASH_LEN + 1];
    for (int i = 0; i < PRESENCE_REQUEST_HASH_LEN; i++)
        snprintf(&req[2 * i], 3, "%02x", hash[i]);
    // An update (CRYPT-14) brings its own wording: "Install tinycrypt x.y.z?".
    char body[PRESENCE_RP_MAX + 32];
    if (label[0])
        snprintf(body, sizeof(body), "%s", label);
    else if (rp[0])
        snprintf(body, sizeof(body), "Sign in to %s?", rp);
    else
        snprintf(body, sizeof(body), "Your security key wants approval");

    cJSON *apns = cJSON_CreateObject();
    cJSON *aps = cJSON_AddObjectToObject(apns, "aps");
    cJSON *alert = cJSON_AddObjectToObject(aps, "alert");
    cJSON_AddStringToObject(alert, "title", label[0] ? "Update?" : "Sign in?");
    cJSON_AddStringToObject(alert, "body", body);
    cJSON_AddStringToObject(aps, "category", "PRESENCE");
    cJSON_AddStringToObject(aps, "sound", "default");
    cJSON *tc = cJSON_AddObjectToObject(apns, "tc");
    cJSON_AddStringToObject(tc, "id", id);
    cJSON_AddStringToObject(tc, "key", prov.thing_name);
    cJSON_AddStringToObject(tc, "req", req);
    if (rp[0])
        cJSON_AddStringToObject(tc, "rp", rp);
    if (sent_at)
        cJSON_AddNumberToObject(tc, "sentAt", (double)sent_at);
    char *apns_text = cJSON_PrintUnformatted(apns);

    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "default", "tinycrypt presence request");
    if (apns_text)
        cJSON_AddStringToObject(message, prov.apns_platform, apns_text);
    char *text = apns_text ? cJSON_PrintUnformatted(message) : NULL;

    if (text == NULL)
        ESP_LOGE(TAG, "out of memory building the presence request");
    else
    {
        // Enqueued for the MQTT task to send, so the CTAP task doesn't wait on the network. QoS 1
        // for the PUBACK in the log; the outbox drops anything unsent after 10 s
        // (CONFIG_MQTT_OUTBOX_EXPIRED_TIMEOUT_MS), so a reconnect can't deliver a stale prompt.
        int msg_id = esp_mqtt_client_enqueue(mqtt, prov.presence_topic, text, 0, 1, 0, true);
        ESP_LOGI(TAG, "presence request %s queued (msg %d, sentAt %" PRId64 ")", id, msg_id, sent_at);
    }

    cJSON_free(text);
    cJSON_Delete(message);
    cJSON_free(apns_text);
    cJSON_Delete(apns);
}

void notifier_presence_wait_started(void)
{
    // Only a request the watch can approve gets a push (presence.h); anything else is button only.
    if (!presence_request_bound())
        return;
    // CRYPT-30: and only if a watch or phone is enrolled to approve it: otherwise no one is told.
#ifdef CONFIG_TINYCRYPT_WATCH_BLE
    if (!ble_presence_enrolled())
        return;
#else
    return;
#endif
    // One push per user action (presence.h): U2F's 750 ms re-asks don't each buzz the watch.
    if (presence_is_new_action())
    {
        taskENTER_CRITICAL(&pending_lock);
        memcpy(held_rp, presence_request_rp(), sizeof(held_rp));
        memcpy(held_label, presence_request_label(), sizeof(held_label));
        memcpy(held_hash, presence_request_hash(), sizeof(held_hash));
        taskEXIT_CRITICAL(&pending_lock);
        send_request();
    }
}

void notifier_presence_wait_ended(void)
{
    // A request still held for a connection is stale once its wait is over.
    taskENTER_CRITICAL(&pending_lock);
    pending = false;
    taskEXIT_CRITICAL(&pending_lock);
}

bool notifier_respond(const char *json)
{
    if (!started || !mqtt_connected)
    {
        ESP_LOGW(TAG, "not connected: response dropped: %s", json);
        return false;
    }
    int msg_id = esp_mqtt_client_enqueue(mqtt, responses_topic, json, 0, 1, 0, true);
    ESP_LOGI(TAG, "response queued (msg %d): %s", msg_id, json);
    return msg_id >= 0;
}

const char *notifier_thing_name(void)
{
    return prov.thing_name;
}

const prov_t *notifier_prov(void)
{
    return prov_loaded ? &prov : NULL;
}

bool notifier_mqtt_connected(void)
{
    return mqtt_connected;
}

// ---- CRYPT-30: set up from the iPhone app over BLE ------------------------------------------

bool notifier_set_wifi(const char *ssid, const char *password)
{
    if (!wifi_started)
        return false;
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS, NVS_READWRITE, &h);
    if (err == ESP_OK)
    {
        err = nvs_set_str(h, "ssid", ssid);
        if (err == ESP_OK)
            err = nvs_set_str(h, "pass", password);
        if (err == ESP_OK)
            err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "can't keep the new Wi-Fi network: %s", esp_err_to_name(err));
        return false;
    }
    // Leave the old network (its disconnect event is ours, so no back-off) and join the new one now.
    esp_timer_stop(wifi_retry);
    reconfiguring = true;
    esp_wifi_disconnect();
    taskENTER_CRITICAL(&wifi_lock);
    strlcpy(wifi_ssid, ssid, sizeof(wifi_ssid));
    taskEXIT_CRITICAL(&wifi_lock);
    wifi_state = SETUP_WIFI_CONNECTING;
    wifi_retry_ms = WIFI_RETRY_MIN_MS;
    if (configure_wifi(ssid, password) != ESP_OK || esp_wifi_connect() != ESP_OK)
        esp_timer_start_once(wifi_retry, (uint64_t)WIFI_RETRY_MIN_MS * 1000);
    ESP_LOGI(TAG, "Wi-Fi set from the app: joining \"%s\"", ssid);
    return true;
}

bool notifier_scan_wifi(void)
{
    if (!wifi_started || scanning)
        return wifi_started;
    const wifi_scan_config_t config = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&config, false);  // SCAN_DONE arrives in wifi_event
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Wi-Fi scan: %s", esp_err_to_name(err));
        return false;
    }
    scanning = true;
    return true;
}

int notifier_scan_result(char out[SETUP_READ_MAX + 1])
{
    setup_network_t nets[SETUP_SCAN_MAX];
    taskENTER_CRITICAL(&wifi_lock);
    size_t n = scan_count;
    memcpy(nets, scan_nets, n * sizeof(*nets));
    taskEXIT_CRITICAL(&wifi_lock);
    return setup_scan_json(nets, n, out);
}

setup_wifi_state_t notifier_wifi_info(char ssid[SETUP_SSID_MAX + 1])
{
    taskENTER_CRITICAL(&wifi_lock);
    strlcpy(ssid, wifi_ssid, SETUP_SSID_MAX + 1);
    taskEXIT_CRITICAL(&wifi_lock);
    return wifi_state;
}

// Publishes one kind's pending registration, if there is one and MQTT is up.
static void publish_registration(setup_kind_t kind)
{
    if (!started || !mqtt_connected || register_topic[0] == 0)
        return;
    setup_register_t reg = {.kind = kind};
    nvs_handle_t h;
    if (nvs_open(REGISTER_NVS, NVS_READONLY, &h) != ESP_OK)
        return;
    size_t len = sizeof(reg.token);
    bool have = nvs_get_str(h, setup_kind_name(kind), reg.token, &len) == ESP_OK;
    nvs_close(h);
    char *payload = have ? setup_register_payload(&reg) : NULL;
    if (payload == NULL)
        return;
    strlcpy(register_sent[kind], reg.token, sizeof(register_sent[kind]));
    register_msg_id[kind] = esp_mqtt_client_enqueue(mqtt, register_topic, payload, 0, 1, 0, true);
    ESP_LOGI(TAG, "%s registration queued (msg %d)", setup_kind_name(kind), register_msg_id[kind]);
    cJSON_free(payload);
}

static void publish_registrations(void)
{
    publish_registration(SETUP_KIND_WATCH);
    publish_registration(SETUP_KIND_PHONE);
}

// IoT Core has it: forget it, unless a newer token has arrived meanwhile.
static void registration_acked(setup_kind_t kind)
{
    register_msg_id[kind] = -1;
    nvs_handle_t h;
    if (nvs_open(REGISTER_NVS, NVS_READWRITE, &h) != ESP_OK)
        return;
    char token[SETUP_TOKEN_MAX + 1];
    size_t len = sizeof(token);
    if (nvs_get_str(h, setup_kind_name(kind), token, &len) == ESP_OK && strcmp(token, register_sent[kind]) == 0)
    {
        nvs_erase_key(h, setup_kind_name(kind));
        nvs_commit(h);
        ESP_LOGI(TAG, "%s registered for this key's pushes", setup_kind_name(kind));
    }
    nvs_close(h);
}

bool notifier_register(const setup_register_t *reg)
{
    // Kept until IoT Core acknowledges it: a new key usually has no Wi-Fi when it's enrolled.
    nvs_handle_t h;
    esp_err_t err = nvs_open(REGISTER_NVS, NVS_READWRITE, &h);
    if (err == ESP_OK)
    {
        err = nvs_set_str(h, setup_kind_name(reg->kind), reg->token);
        if (err == ESP_OK)
            err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "can't keep the %s registration: %s", setup_kind_name(reg->kind), esp_err_to_name(err));
        return false;
    }
    publish_registration(reg->kind);
    return true;
}
