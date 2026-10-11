// Watch presence over BLE (CRYPT-13). See ble_presence.h.
#include "ble_presence.h"

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
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble-presence";

#define DOMAIN_TAG "tinycrypt-presence-v0"
#define DOMAIN_LEN (sizeof(DOMAIN_TAG) - 1)
#define NONCE_LEN 32
#define CHALLENGE_LEN (DOMAIN_LEN + NONCE_LEN)
// An unapproved challenge outlives its wait by this long, so a U2F re-ask can still use it.
#define SAME_ACTION_GAP_US (3 * 1000 * 1000)
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

static uint8_t watch_pub[TINYCRYPT_P256_PUB_LEN];
static bool have_watch;

static uint8_t own_addr_type;

static void advertise(void);

// ---- the presence source ----------------------------------------------------

static void arm(void *ctx)
{
    (void)ctx;
    taskENTER_CRITICAL(&lock);
    bool new_action = presence_is_new_action() || !challenge_live;
    if (new_action)
    {
        memcpy(challenge, DOMAIN_TAG, DOMAIN_LEN);
        esp_fill_random(challenge + DOMAIN_LEN, NONCE_LEN);
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
    if (result == 1)
        challenge_live = false;  // approved by the button or the watch: nothing left to sign
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

void ble_presence_open_enrol(uint32_t seconds)
{
    enrol_until_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    ESP_LOGW(TAG, "ENROL window open for %" PRIu32 " s: tap Enrol with key on the watch", seconds);
}

bool ble_presence_enrolled(void)
{
    return have_watch;
}

static int enrol(const uint8_t *pub, size_t len)
{
    if (esp_timer_get_time() > enrol_until_us)
    {
        ESP_LOGW(TAG, "enrol refused: hold the button 5 s to open the enrol window");
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }
    if (len != TINYCRYPT_P256_PUB_LEN)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
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
    enrol_until_us = 0;  // one watch per window
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
        ESP_LOGI(TAG, "watch telemetry: %.*s", (int)len, (const char *)buf);
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

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type)
    {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "watch %s", event->connect.status == 0 ? "connected" : "failed to connect");
        if (event->connect.status != 0)
            advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "watch disconnected (reason 0x%x)", event->disconnect.reason);
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
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
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

void ble_presence_start(void)
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
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(services) != 0 || ble_gatts_add_svcs(services) != 0)
    {
        ESP_LOGE(TAG, "GATT registration failed: watch presence off");
        return;
    }
    ble_svc_gap_device_name_set("tinycrypt");
    nimble_port_freertos_init(host_task);
}
