// Watch presence over BLE (CRYPT-13). See ble_presence.h.
#include "ble_presence.h"
#include "led_status.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "presence_sig.h"
#include "uECC.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble-presence";

// Protocol v1 (CRYPT-18): the challenge carries the request hash, so a signature approves only the
// request it was made for.
#define DOMAIN_TAG "tinycrypt-presence-v1"
#define DOMAIN_LEN (sizeof(DOMAIN_TAG) - 1)
#define NONCE_LEN 32
#define CHALLENGE_LEN (DOMAIN_LEN + NONCE_LEN + PRESENCE_REQUEST_HASH_LEN)
// An unapproved challenge outlives its wait by this long, so a U2F re-ask can still use it.
#define SAME_ACTION_GAP_US ((int64_t)PRESENCE_SAME_ACTION_GAP_MS * 1000)
// Any connection is dropped after this long: an approval takes under 2 s.
#define CONNECTION_LIMIT_US (10 * 1000 * 1000)
#define TELEMETRY_LOG_MAX 160
#define NVS_NAMESPACE "tinycrypt"
#define NVS_WATCH_KEY "watch_pub"

static ble_uuid_any_t svc_uuid, challenge_uuid, response_uuid, enrol_uuid, telemetry_uuid;

// Shared between the NimBLE host task and the CTAP task.
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t challenge[CHALLENGE_LEN];
static bool challenge_live;      // readable and answerable
static bool approved;            // a valid response arrived for the live challenge
static bool waiting;             // a presence wait is in progress
static int64_t last_wait_end_us;
static int64_t enrol_until_us;
static esp_timer_handle_t enrol_timer;

static uint8_t watch_pub[TINYCRYPT_P256_PUB_LEN];
static bool have_watch;

static uint8_t own_addr_type;

static void advertise(void);

// ---- the presence source ----------------------------------------------------

static void arm(void *ctx)
{
    (void)ctx;
    uint8_t nonce[NONCE_LEN];
    esp_fill_random(nonce, sizeof(nonce));  // outside the critical section
    taskENTER_CRITICAL(&lock);
    bool new_action = presence_is_new_action() || !challenge_live;
    if (new_action)
    {
        memcpy(challenge, DOMAIN_TAG, DOMAIN_LEN);
        memcpy(challenge + DOMAIN_LEN, nonce, NONCE_LEN);
        memcpy(challenge + DOMAIN_LEN + NONCE_LEN, presence_request_hash(), PRESENCE_REQUEST_HASH_LEN);
        challenge_live = true;
        approved = false;
    }
    waiting = true;
    taskEXIT_CRITICAL(&lock);
    if (new_action)
        ESP_LOGI(TAG, "challenge armed%s", have_watch ? "" : " (no watch enrolled)");
}

static bool is_approved(void *ctx)
{
    (void)ctx;
    taskENTER_CRITICAL(&lock);
    bool yes = approved;
    if (yes)
    {
        approved = false;
        challenge_live = false;  // single use
    }
    taskEXIT_CRITICAL(&lock);
    return yes;
}

static void disarm(void *ctx, int result)
{
    (void)ctx;
    taskENTER_CRITICAL(&lock);
    waiting = false;
    last_wait_end_us = esp_timer_get_time();
    // Approved (by either source) or cancelled: this challenge, and any approval not yet used, are
    // finished, so nothing can carry over to another request.
    if (result == 1 || presence_was_cancelled())
    {
        challenge_live = false;
        approved = false;
    }
    taskEXIT_CRITICAL(&lock);
}

const presence_source ble_presence_source = {"watch", NULL, arm, is_approved, disarm};

// Called on each NimBLE access: retires a challenge whose user action has ended.
static void expire_challenge(void)
{
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&lock);
    if (challenge_live && !waiting && now - last_wait_end_us > SAME_ACTION_GAP_US)
        challenge_live = false;
    taskEXIT_CRITICAL(&lock);
}

// ---- enrolment ---------------------------------------------------------------

static void enrol_window_closed(void *arg)
{
    (void)arg;
    led_status_set(LED_ENROL_OPEN, false);
}

void ble_presence_open_enrol(uint32_t seconds)
{
    if (enrol_timer == NULL)
    {
        const esp_timer_create_args_t args = {.callback = enrol_window_closed, .name = "enrol"};
        esp_timer_create(&args, &enrol_timer);
    }
    if (enrol_timer != NULL)
    {
        esp_timer_stop(enrol_timer);
        esp_timer_start_once(enrol_timer, (uint64_t)seconds * 1000000);
    }
    led_status_set(LED_ENROL_OPEN, true);
    int64_t until = esp_timer_get_time() + (int64_t)seconds * 1000000;
    taskENTER_CRITICAL(&lock);
    enrol_until_us = until;
    taskEXIT_CRITICAL(&lock);
    ESP_LOGW(TAG, "ENROL window open for %" PRIu32 " s: tap Enrol with key on the watch", seconds);
}

bool ble_presence_enrolled(void)
{
    return have_watch;
}

static int enrol(const uint8_t *pub, size_t len)
{
    taskENTER_CRITICAL(&lock);
    bool open = esp_timer_get_time() <= enrol_until_us;
    taskEXIT_CRITICAL(&lock);
    if (!open)
    {
        ESP_LOGW(TAG, "enrol refused: hold the button 5 s to open the enrol window");
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }
    if (len != TINYCRYPT_P256_PUB_LEN)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (!uECC_valid_public_key(pub, uECC_secp256r1()))
    {
        ESP_LOGW(TAG, "enrol refused: not a point on P-256");
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK)
        return BLE_ATT_ERR_UNLIKELY;
    esp_err_t err = nvs_set_blob(nvs, NVS_WATCH_KEY, pub, len);
    if (err == ESP_OK)
        err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK)
        return BLE_ATT_ERR_UNLIKELY;
    memcpy(watch_pub, pub, len);
    have_watch = true;
    taskENTER_CRITICAL(&lock);
    enrol_until_us = 0;  // one watch per window
    taskEXIT_CRITICAL(&lock);
    if (enrol_timer != NULL)
        esp_timer_stop(enrol_timer);
    led_status_set(LED_ENROL_OPEN, false);
    led_status_flash(LED_FLASH_ENROLLED);
    ESP_LOGW(TAG, "ENROLLED watch key %02x%02x%02x%02x...", pub[0], pub[1], pub[2], pub[3]);
    return 0;
}

// ---- GATT --------------------------------------------------------------------

static int respond(const uint8_t *sig, size_t len)
{
    if (!have_watch)
    {
        ESP_LOGW(TAG, "response refused: no watch enrolled");
        return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    }
    if (len != TINYCRYPT_P256_SIG_LEN)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint8_t msg[CHALLENGE_LEN];
    taskENTER_CRITICAL(&lock);
    bool live = challenge_live && !approved;
    memcpy(msg, challenge, sizeof(msg));
    taskEXIT_CRITICAL(&lock);
    if (!live)
    {
        ESP_LOGW(TAG, "response refused: no presence request waiting");
        return BLE_ATT_ERR_UNLIKELY;
    }
    int64_t t0 = esp_timer_get_time();
    if (!tinycrypt_presence_sig_verify(watch_pub, msg, sizeof(msg), sig))
    {
        ESP_LOGW(TAG, "INVALID watch signature");
        return BLE_ATT_ERR_UNLIKELY;
    }
    taskENTER_CRITICAL(&lock);
    // Still the same challenge? (The user action could have ended while we verified.)
    bool ok = challenge_live && memcmp(msg, challenge, sizeof(msg)) == 0;
    if (ok)
        approved = true;
    taskEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG, "%s watch signature (verified in %" PRId64 " ms)", ok ? "VALID" : "late",
             (esp_timer_get_time() - t0) / 1000);
    return ok ? 0 : BLE_ATT_ERR_UNLIKELY;
}

static int gatt_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    expire_challenge();
    const ble_uuid_t *uuid = ctxt->chr->uuid;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
    {
        if (ble_uuid_cmp(uuid, &challenge_uuid.u) != 0)
            return BLE_ATT_ERR_UNLIKELY;
        uint8_t msg[CHALLENGE_LEN];
        taskENTER_CRITICAL(&lock);
        bool live = challenge_live;
        memcpy(msg, challenge, sizeof(msg));
        taskEXIT_CRITICAL(&lock);
        if (!live)
        {
            ESP_LOGW(TAG, "challenge read refused: no presence request waiting");
            return BLE_ATT_ERR_READ_NOT_PERMITTED;
        }
        ESP_LOGI(TAG, "challenge read");
        led_status_set(LED_WATCH_SIGNING, true);
        return os_mbuf_append(ctxt->om, msg, sizeof(msg)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_UNLIKELY;
    uint8_t buf[256];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (ble_uuid_cmp(uuid, &response_uuid.u) == 0)
        return respond(buf, len);
    if (ble_uuid_cmp(uuid, &enrol_uuid.u) == 0)
        return enrol(buf, len);
    if (ble_uuid_cmp(uuid, &telemetry_uuid.u) == 0)
    {
        // Unauthenticated: log it truncated and printable only, clearly marked as the peer's words.
        char text[TELEMETRY_LOG_MAX + 1];
        size_t n = len < TELEMETRY_LOG_MAX ? len : TELEMETRY_LOG_MAX;
        for (size_t i = 0; i < n; i++)
            text[i] = buf[i] >= 0x20 && buf[i] < 0x7f ? (char)buf[i] : '?';
        text[n] = 0;
        ESP_LOGI(TAG, "peer telemetry (unverified): %s", text);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static struct ble_gatt_chr_def characteristics[] = {
    {.uuid = &challenge_uuid.u, .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_READ},
    {.uuid = &response_uuid.u, .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_WRITE},
    {.uuid = &enrol_uuid.u, .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_WRITE},
    {.uuid = &telemetry_uuid.u, .access_cb = gatt_access, .flags = BLE_GATT_CHR_F_WRITE},
    {0},
};

static const struct ble_gatt_svc_def services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &svc_uuid.u, .characteristics = characteristics},
    {0},
};

// ---- GAP ---------------------------------------------------------------------

// One slot per allowed connection; each has a one-shot timer that drops it after the limit.
#define SLOTS CONFIG_BT_NIMBLE_MAX_CONNECTIONS
#define NO_CONN 0xffff
static esp_timer_handle_t slot_timer[SLOTS];
static uint16_t slot_conn[SLOTS] = {[0 ... SLOTS - 1] = NO_CONN};

static void drop_connection(void *arg)
{
    uint16_t handle = slot_conn[(uintptr_t)arg];
    if (handle != NO_CONN && ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM) == 0)
        ESP_LOGI(TAG, "connection %u dropped after %d s", handle, (int)(CONNECTION_LIMIT_US / 1000000));
}

static void connection_opened(uint16_t handle)
{
    for (uintptr_t i = 0; i < SLOTS; i++)
    {
        if (slot_conn[i] != NO_CONN)
            continue;
        if (slot_timer[i] == NULL)
        {
            const esp_timer_create_args_t args = {.callback = drop_connection, .arg = (void *)i, .name = "ble_limit"};
            if (esp_timer_create(&args, &slot_timer[i]) != ESP_OK)
                return;
        }
        slot_conn[i] = handle;
        esp_timer_start_once(slot_timer[i], CONNECTION_LIMIT_US);
        return;
    }
}

static void connection_closed(uint16_t handle)
{
    for (int i = 0; i < SLOTS; i++)
        if (slot_conn[i] == handle)
        {
            esp_timer_stop(slot_timer[i]);
            slot_conn[i] = NO_CONN;
        }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type)
    {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "central %s", event->connect.status == 0 ? "connected" : "failed to connect");
        if (event->connect.status == 0)
            connection_opened(event->connect.conn_handle);
        // Keep advertising while there's room for another connection, so a squatter can't
        // lock the watch out.
        advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "central disconnected (reason 0x%x)", event->disconnect.reason);
        led_status_set(LED_WATCH_SIGNING, false);
        connection_closed(event->disconnect.conn.conn_handle);
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    // The service UUID goes in the advert itself (watchOS scans filter on it); the name in the
    // scan response, as both don't fit in 31 bytes.
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &svc_uuid.u128;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc == 0)
    {
        struct ble_hs_adv_fields rsp = {0};
        const char *name = ble_svc_gap_device_name();
        rsp.name = (const uint8_t *)name;
        rsp.name_len = strlen(name);
        rsp.name_is_complete = 1;
        rc = ble_gap_adv_rsp_set_fields(&rsp);
    }
    struct ble_gap_adv_params params = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN};
    if (rc == 0)
        rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY)
        ESP_LOGE(TAG, "advertising failed: %d", rc);
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &own_addr_type) != 0)
    {
        ESP_LOGE(TAG, "no usable BLE address: watch presence off (the button still works)");
        return;
    }
    uint8_t addr[6] = {0};
    ble_hs_id_copy_addr(own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "advertising as tinycrypt (%02x:%02x:%02x:%02x:%02x:%02x)%s", addr[5], addr[4], addr[3],
             addr[2], addr[1], addr[0], have_watch ? ", watch enrolled" : ", no watch enrolled");
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset (reason %d)", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

bool ble_presence_start(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK)
    {
        size_t len = sizeof(watch_pub);
        have_watch = nvs_get_blob(nvs, NVS_WATCH_KEY, watch_pub, &len) == ESP_OK && len == sizeof(watch_pub);
        nvs_close(nvs);
    }

    ble_uuid_from_str(&svc_uuid, "30ccc4f1-350d-4fdf-89c5-6120e2cc5044");
    ble_uuid_from_str(&challenge_uuid, "1c88eb1a-4d1e-4e79-afc7-637c28e91503");
    ble_uuid_from_str(&response_uuid, "78dc86b2-6f57-478e-979c-22ed6e135a98");
    ble_uuid_from_str(&enrol_uuid, "a40698d1-315e-496d-8855-05283e3369a0");
    ble_uuid_from_str(&telemetry_uuid, "c88c0afb-1519-4b4f-9ca1-e5f9eeffd259");

    if (nimble_port_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "nimble_port_init failed: watch presence off (the button still works)");
        return false;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(services) != 0 || ble_gatts_add_svcs(services) != 0)
    {
        ESP_LOGE(TAG, "GATT registration failed: watch presence off");
        return true;  // the controller (and so the radio's entropy) is up
    }
    ble_svc_gap_device_name_set("tinycrypt");
    nimble_port_freertos_init(host_task);
    return true;
}
