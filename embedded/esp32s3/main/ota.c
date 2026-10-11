// OTA through Fleet (CRYPT-14). See ota.h.
#include "ota.h"
#include "notifier.h"
#include "presence.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "ota";

extern const char root_ca_start[] asm("_binary_AmazonRootCA1_pem_start");

#define PROJECT "tinycrypt"
#define NVS_NAMESPACE "ota"
#define NVS_TARGET "target"  // the version an approved install restarted into
#define READ_CHUNK 4096
#define HTTP_TIMEOUT_MS 15000
#define RESTART_DELAY_MS 2000  // for "approved" to reach IoT Core first

typedef enum
{
    IDLE,
    DOWNLOADING,
    APPROVAL_REQUESTED,  // waiting for the CTAP task to pick it up
    APPROVAL_RUNNING,
    INSTALLING,
} phase_t;

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static volatile phase_t phase = IDLE;
static volatile bool cancel;
static TaskHandle_t task;
static SemaphoreHandle_t approval_done;
static int approval_result;

// The update being worked on (written by the MQTT task while IDLE, then owned by the OTA task).
static char *job_url;
static char job_version[OTA_VERSION_MAX];
static uint8_t job_sha256[OTA_SHA256_LEN];
static int64_t job_finished_us;  // when the last update ended (in failure: success restarts)
#define REDELIVERY_WINDOW_US (30 * 1000000LL)
static uint8_t approval_hash[32];
static char approval_label[OTA_LABEL_MAX];

static nvs_handle_t nvs;
static bool nvs_ok;
static volatile bool on_probation;  // a new image that hasn't yet passed its self-test
static esp_timer_handle_t verify_deadline;

static const char *running_version(void)
{
    return esp_app_get_description()->version;
}

// ---- responses ----------------------------------------------------------------

static void respond(cJSON *msg)
{
    char *text = cJSON_PrintUnformatted(msg);
#ifdef CONFIG_TINYCRYPT_NOTIFIER
    if (text != NULL)
        notifier_respond(text);
#endif
    cJSON_free(text);
    cJSON_Delete(msg);
}

static cJSON *message(const char *type)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", type);
    return msg;
}

static void respond_progress(const char *status, int progress, const char *version)
{
    cJSON *msg = message("ota_progress");
    cJSON_AddStringToObject(msg, "status", status);
    cJSON_AddNumberToObject(msg, "progress", progress);
    if (version)
        cJSON_AddStringToObject(msg, "version", version);
    respond(msg);
}

static void respond_error(const char *type, const char *text, const char *version)
{
    ESP_LOGW(TAG, "%s: %s", type, text);
    cJSON *msg = message(type);
    cJSON_AddStringToObject(msg, "message", text);
    if (version)
        cJSON_AddStringToObject(msg, "version", version);
    respond(msg);
}

static void respond_version(void)
{
    cJSON *msg = message("version");
    cJSON_AddStringToObject(msg, "version", running_version());
    cJSON_AddStringToObject(msg, "project", PROJECT);
    respond(msg);
}

// ---- boot: probation and rollback ------------------------------------------------

static void verify_deadline_fire(void *arg)
{
    (void)arg;
    if (!on_probation)
        return;
    ESP_LOGE(TAG, "new image didn't reach IoT Core within %d s: rolling back", OTA_VERIFY_DEADLINE_S);
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

void ota_boot(bool local_selftest_ok)
{
    nvs_ok = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK;
    approval_done = xSemaphoreCreateBinary();
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    bool pending = esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
    ESP_LOGI(TAG, "running %s %s from %s%s", PROJECT, running_version(), running->label,
             pending ? " (new image, on probation)" : "");
    if (!pending)
        return;
    if (!local_selftest_ok)
    {
        ESP_LOGE(TAG, "new image failed its self-test: rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
#ifdef CONFIG_TINYCRYPT_NOTIFIER
    // Kept only once it can be updated again (ota_on_connected).
    on_probation = true;
    const esp_timer_create_args_t args = {.callback = verify_deadline_fire, .name = "ota_verify"};
    if (esp_timer_create(&args, &verify_deadline) != ESP_OK ||
        esp_timer_start_once(verify_deadline, (uint64_t)OTA_VERIFY_DEADLINE_S * 1000000) != ESP_OK)
    {
        ESP_LOGE(TAG, "no probation timer: rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
#else
    esp_ota_mark_app_valid_cancel_rollback();
#endif
}

void ota_on_connected(void)
{
    if (on_probation)
    {
        on_probation = false;
        esp_timer_stop(verify_deadline);
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new image passed its self-test: kept (%s)", esp_err_to_name(err));
    }
    respond_version();

    // The outcome of an install this key approved, reported once.
    char target[OTA_VERSION_MAX];
    size_t len = sizeof(target);
    if (!nvs_ok || nvs_get_str(nvs, NVS_TARGET, target, &len) != ESP_OK)
        return;
    if (strcmp(target, running_version()) == 0)
    {
        cJSON *msg = message("ota_complete");
        cJSON_AddStringToObject(msg, "version", target);
        respond(msg);
    }
    else
        respond_error("ota_error", "rolled back", target);
    nvs_erase_key(nvs, NVS_TARGET);
    nvs_commit(nvs);
}

// ---- download and checks ------------------------------------------------------------

// "<what> (tls ... errno ... heap free/largest)": the key's log is usually out of reach (the native
// USB port is the key), so a failure says why itself: the TLS stack's error and certificate-verify
// flags, the socket's errno, and how much internal RAM there was.
static const char *explain(esp_http_client_handle_t http, const char *what, char *why, size_t why_len)
{
    int tls_code = 0, tls_flags = 0;
    esp_err_t tls = esp_http_client_get_and_clear_last_tls_error(http, &tls_code, &tls_flags);
    snprintf(why, why_len, "download failed: %s (tls %s -0x%x flags 0x%x, errno %d, heap %u/%u)", what,
             esp_err_to_name(tls), (unsigned)-tls_code, (unsigned)tls_flags, esp_http_client_get_errno(http),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ESP_LOGE(TAG, "%s", why);
    return why;
}

static void log_heap(const char *when)
{
    ESP_LOGI(TAG, "%s: heap %u free, largest block %u", when, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

// Returns NULL on success, else the reason (for ota_error).
static const char *download(const esp_partition_t *slot, char *why, size_t why_len)
{
    const char *err_text = NULL;
    uint8_t *buf = malloc(READ_CHUNK);
    esp_http_client_config_t config = {
        .url = job_url,
        .cert_pem = root_ca_start,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .keep_alive_enable = false,
        // The request line carries the whole presigned URL (~1.5-3 KB with an STS token); the
        // default 512-byte TX buffer can't hold it, and the request fails with a bare ESP_FAIL.
        .buffer_size_tx = (int)strlen(job_url) + 1024,
    };
    esp_http_client_handle_t http = buf ? esp_http_client_init(&config) : NULL;
    esp_ota_handle_t ota = 0;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    if (http == NULL)
    {
        err_text = "download failed: out of memory";
        goto out;
    }
    log_heap("before connecting");
    esp_err_t err = esp_http_client_open(http, 0);
    if (err != ESP_OK)
    {
        err_text = explain(http, esp_err_to_name(err), why, why_len);
        goto out;
    }
    log_heap("connected");
    int64_t length = esp_http_client_fetch_headers(http);
    int status = esp_http_client_get_status_code(http);
    ESP_LOGI(TAG, "HTTP %d, %" PRId64 " bytes", status, length);
    if (status != 200)
    {
        snprintf(why, why_len, "download failed: HTTP %d", status);
        err_text = why;
        goto out;
    }
    if (length > (int64_t)slot->size)
    {
        err_text = "image too large";
        goto out;
    }
    err = esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK)
    {
        snprintf(why, why_len, "download failed: %s", esp_err_to_name(err));
        err_text = why;
        ota = 0;
        goto out;
    }
    mbedtls_sha256_starts(&sha, 0);
    size_t total = 0;
    int last_tenth = -1;
    int stalls = 0;
    for (;;)
    {
        if (cancel)
        {
            err_text = "cancelled";
            goto out;
        }
        int n = esp_http_client_read(http, (char *)buf, READ_CHUNK);
        if (n == -ESP_ERR_HTTP_EAGAIN && ++stalls <= 3)
        {
            ESP_LOGW(TAG, "read timed out after %u bytes: retrying", (unsigned)total);
            continue;
        }
        if (n < 0)
        {
            char what[48];
            snprintf(what, sizeof(what), "read %d after %u bytes", n, (unsigned)total);
            err_text = explain(http, what, why, why_len);
            goto out;
        }
        stalls = 0;
        if (n == 0)
        {
            if (!esp_http_client_is_complete_data_received(http))
            {
                err_text = "download failed: connection closed early";
                goto out;
            }
            break;
        }
        total += (size_t)n;
        if (total > slot->size)
        {
            err_text = "image too large";
            goto out;
        }
        mbedtls_sha256_update(&sha, buf, (size_t)n);
        err = esp_ota_write(ota, buf, (size_t)n);
        if (err != ESP_OK)
        {
            snprintf(why, why_len, "download failed: %s", esp_err_to_name(err));
            err_text = why;
            goto out;
        }
        if (length > 0 && (int)(total * 10 / (size_t)length) != last_tenth)
        {
            last_tenth = (int)(total * 10 / (size_t)length);
            respond_progress("downloading", last_tenth * 10, job_version);
        }
    }
    ESP_LOGI(TAG, "downloaded %u bytes", (unsigned)total);

    uint8_t got[OTA_SHA256_LEN];
    mbedtls_sha256_finish(&sha, got);
    if (memcmp(got, job_sha256, sizeof(got)) != 0)
    {
        err_text = "sha256 mismatch";
        goto out;
    }
    // Checks the image's own integrity and its signature against the running app's signing key.
    err = esp_ota_end(ota);
    ota = 0;
    if (err != ESP_OK)
    {
        err_text = err == ESP_ERR_OTA_VALIDATE_FAILED ? "bad signature" : "verify failed";
        goto out;
    }
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(slot, &desc) != ESP_OK || strcmp(desc.project_name, PROJECT) != 0 ||
        strcmp(desc.version, job_version) != 0)
    {
        err_text = "wrong image";
        goto out;
    }
    if (ota_version_valid(running_version()) && ota_version_cmp(job_version, running_version()) < 0)
        err_text = "downgrade refused";

out:
    if (ota)
        esp_ota_abort(ota);
    mbedtls_sha256_free(&sha);
    if (http)
        esp_http_client_cleanup(http);
    free(buf);
    return err_text;
}

static void finish(void)
{
    taskENTER_CRITICAL(&lock);
    phase = IDLE;
    cancel = false;
    taskEXIT_CRITICAL(&lock);
    free(job_url);
    job_url = NULL;
    job_finished_us = esp_timer_get_time();
}

static void ota_task(void *arg)
{
    (void)arg;
    static char why[160];
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
        ESP_LOGI(TAG, "update to %s: downloading into %s", job_version, slot ? slot->label : "(none)");
        const char *failed = slot ? download(slot, why, sizeof(why)) : "no update slot";
        if (failed != NULL)
        {
            if (strcmp(failed, "cancelled") == 0)
                respond_progress("Update cancelled", 0, NULL);
            else
                respond_error("ota_error", failed, job_version);
            finish();
            continue;
        }

        // Verified: now the user decides.
        ota_approval_hash(job_sha256, job_version, approval_hash);
        ota_approval_label(job_version, approval_label);
        xSemaphoreTake(approval_done, 0);  // nothing stale
        taskENTER_CRITICAL(&lock);
        bool cancelled = cancel;
        if (!cancelled)
            phase = APPROVAL_REQUESTED;
        taskEXIT_CRITICAL(&lock);
        if (cancelled)
        {
            respond_progress("Update cancelled", 0, NULL);
            finish();
            continue;
        }
        ESP_LOGI(TAG, "%s verified: waiting for approval", job_version);
        respond_progress("awaiting_approval", 100, job_version);
        // The CTAP task runs the wait (OTA_APPROVAL_MS) when it's next idle; allow for a sign-in first.
        if (xSemaphoreTake(approval_done, pdMS_TO_TICKS(OTA_APPROVAL_MS + 60000)) != pdTRUE)
        {
            taskENTER_CRITICAL(&lock);
            bool running = phase == APPROVAL_RUNNING;
            if (!running)
                phase = INSTALLING;  // so the CTAP task can't pick it up now
            taskEXIT_CRITICAL(&lock);
            if (running)
                xSemaphoreTake(approval_done, portMAX_DELAY);  // it ends at OTA_APPROVAL_MS
            else
                approval_result = 0;
        }
        taskENTER_CRITICAL(&lock);
        cancelled = cancel;
        phase = INSTALLING;
        taskEXIT_CRITICAL(&lock);
        if (approval_result != 1)
        {
            respond_error("ota_error", cancelled ? "rejected" : "approval timed out", job_version);
            finish();
            continue;
        }

        ESP_LOGI(TAG, "%s approved: installing", job_version);
        esp_err_t err = nvs_ok ? nvs_set_str(nvs, NVS_TARGET, job_version) : ESP_ERR_INVALID_STATE;
        if (err == ESP_OK)
            err = nvs_commit(nvs);
        if (err == ESP_OK)
            err = esp_ota_set_boot_partition(slot);
        if (err != ESP_OK)
        {
            if (nvs_ok)
                nvs_erase_key(nvs, NVS_TARGET);
            respond_error("ota_error", "install failed", job_version);
            finish();
            continue;
        }
        respond_progress("approved", 100, job_version);
        vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
        ESP_LOGI(TAG, "restarting into %s", job_version);
        esp_restart();
    }
}

// ---- commands (MQTT task) -------------------------------------------------------------

static void start_update(const ota_cmd *cmd)
{
    if (task == NULL && xTaskCreate(ota_task, "ota", 10 * 1024, NULL, 4, &task) != pdPASS)
    {
        task = NULL;
        respond_error("ota_error", "out of memory", cmd->version);
        return;
    }
    char *url = strdup(cmd->url);
    if (url == NULL)
    {
        respond_error("ota_error", "out of memory", cmd->version);
        return;
    }
    // IoT Core redelivers a command whose PUBACK was slow (the MQTT task is starved while a download
    // runs), sometimes after the update has already failed: don't start it all over again.
    bool same = strcmp(cmd->version, job_version) == 0 && memcmp(cmd->sha256, job_sha256, sizeof(job_sha256)) == 0;
    if (same && phase == IDLE && job_finished_us != 0 && esp_timer_get_time() - job_finished_us < REDELIVERY_WINDOW_US)
    {
        ESP_LOGI(TAG, "update to %s again within %lld s of the last: a redelivery, ignored", cmd->version,
                 REDELIVERY_WINDOW_US / 1000000);
        free(url);
        return;
    }
    taskENTER_CRITICAL(&lock);
    bool busy = phase != IDLE;
    if (!busy)
    {
        phase = DOWNLOADING;
        cancel = false;
    }
    taskEXIT_CRITICAL(&lock);
    if (busy)
    {
        free(url);
        // IoT Core redelivers a command whose PUBACK was slow (the MQTT task is starved while a
        // download runs): the same update again is that, not a second request.
        if (same)
            ESP_LOGI(TAG, "update to %s again: already under way", cmd->version);
        else
            respond_error("ota_error", "busy", cmd->version);
        return;
    }
    job_url = url;
    snprintf(job_version, sizeof(job_version), "%s", cmd->version);
    memcpy(job_sha256, cmd->sha256, sizeof(job_sha256));
    xTaskNotifyGive(task);
}

static void cancel_update(void)
{
    taskENTER_CRITICAL(&lock);
    phase_t now = phase;
    if (now == DOWNLOADING || now == APPROVAL_RUNNING)
        cancel = true;
    else if (now == APPROVAL_REQUESTED)
    {
        // Not started yet: it never will be.
        cancel = true;
        approval_result = 0;
        phase = INSTALLING;
    }
    taskEXIT_CRITICAL(&lock);
    if (now == APPROVAL_RUNNING)
        presence_cancel_external();
    else if (now == APPROVAL_REQUESTED)
        xSemaphoreGive(approval_done);
    else if (now == INSTALLING)
        respond_progress("Installing; too late to cancel", 100, NULL);
    else if (now == IDLE)
        respond_progress("Update cancelled", 0, NULL);
}

void ota_handle_command(const char *data, size_t len)
{
    static ota_chunks chunks;
    static ota_cmd cmd;  // 8 KB: not on the MQTT task's stack
    ota_cmd_parse(&chunks, data, len, &cmd);
    switch (cmd.kind)
    {
    case OTA_CMD_GET_VERSION:
        respond_version();
        break;
    case OTA_CMD_PING:
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        cJSON *msg = message("pong");
        cJSON_AddNumberToObject(msg, "timestamp", (double)tv.tv_sec * 1000 + tv.tv_usec / 1000);
        respond(msg);
        break;
    }
    case OTA_CMD_CANCEL:
        cancel_update();
        break;
    case OTA_CMD_CHUNK:
    {
        char status[48];
        snprintf(status, sizeof(status), "Chunk %d/%d received", cmd.chunk_index + 1, cmd.total_chunks);
        respond_progress(status, 0, NULL);  // no version: an ack can't reopen a finished update
        break;
    }
    case OTA_CMD_UPDATE:
        ESP_LOGI(TAG, "update to %s requested", cmd.version);
        start_update(&cmd);
        break;
    case OTA_CMD_ERROR:
        respond_error(cmd.ota_error ? "ota_error" : "error", cmd.message, NULL);
        break;
    }
}

// ---- the approval wait (CTAP task) ------------------------------------------------------

bool ota_take_approval(uint8_t hash[32], char label[OTA_LABEL_MAX])
{
    if (phase != APPROVAL_REQUESTED)
        return false;
    taskENTER_CRITICAL(&lock);
    bool take = phase == APPROVAL_REQUESTED;
    if (take)
        phase = APPROVAL_RUNNING;
    taskEXIT_CRITICAL(&lock);
    if (take)
    {
        memcpy(hash, approval_hash, sizeof(approval_hash));
        memcpy(label, approval_label, OTA_LABEL_MAX);
    }
    return take;
}

void ota_approval_done(int result)
{
    approval_result = result;
    xSemaphoreGive(approval_done);
}
