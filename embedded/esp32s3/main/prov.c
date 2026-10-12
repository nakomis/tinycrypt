// The key's provisioned settings (CRYPT-14). See prov.h.
#include "prov.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "prov";

#define PARTITION "prov"
#define NAMESPACE "tinycrypt"

static bool get_fixed(nvs_handle_t h, const char *key, char *out, size_t size)
{
    size_t len = size;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "%s: %s", key, esp_err_to_name(err));
    return err == ESP_OK;
}

// An optional setting: absent leaves it empty, quietly.
static void get_optional(nvs_handle_t h, const char *key, char *out, size_t size)
{
    size_t len = size;
    if (nvs_get_str(h, key, out, &len) != ESP_OK)
        out[0] = 0;
}

static char *get_alloc(nvs_handle_t h, const char *key)
{
    size_t len = 0;
    esp_err_t err = nvs_get_str(h, key, NULL, &len);
    char *out = err == ESP_OK ? malloc(len) : NULL;
    if (out != NULL && nvs_get_str(h, key, out, &len) != ESP_OK)
    {
        free(out);
        out = NULL;
    }
    if (out == NULL)
        ESP_LOGE(TAG, "%s: %s", key, err == ESP_OK ? "out of memory" : esp_err_to_name(err));
    return out;
}

bool prov_load(prov_t *prov)
{
    memset(prov, 0, sizeof(*prov));
    esp_err_t err = nvs_flash_init_partition(PARTITION);
    nvs_handle_t h;
    if (err == ESP_OK)
        err = nvs_open_from_partition(PARTITION, NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "not provisioned (%s): run infra/scripts/provision-key.sh", esp_err_to_name(err));
        return false;
    }
    bool ok = get_fixed(h, "wifi_ssid", prov->wifi_ssid, sizeof(prov->wifi_ssid));
    ok &= get_fixed(h, "wifi_pass", prov->wifi_password, sizeof(prov->wifi_password));
    ok &= get_fixed(h, "endpoint", prov->iot_endpoint, sizeof(prov->iot_endpoint));
    ok &= get_fixed(h, "thing", prov->thing_name, sizeof(prov->thing_name));
    ok &= get_fixed(h, "topic", prov->presence_topic, sizeof(prov->presence_topic));
    ok &= get_fixed(h, "apns", prov->apns_platform, sizeof(prov->apns_platform));
    prov->device_cert = get_alloc(h, "cert");
    get_optional(h, "metrics_url", prov->metrics_url, sizeof(prov->metrics_url));
    get_optional(h, "metrics_user", prov->metrics_user, sizeof(prov->metrics_user));
    get_optional(h, "metrics_pass", prov->metrics_pass, sizeof(prov->metrics_pass));
    // The IoT key lives in the secure element (CRYPT-22, se_tls.c). Keys provisioned before that
    // have it here, in plain flash: the soft-key test build still uses it until the key is
    // reprovisioned with provision-key.sh --csr; an ATECC build never does.
    size_t key_len = 0;
    if (nvs_get_str(h, "key", NULL, &key_len) == ESP_OK)
    {
#ifdef TINYCRYPT_INSECURE_SOFT_KEY
        prov->device_key = get_alloc(h, "key");
        ESP_LOGW(TAG, "IoT private key in prov (INSECURE): reprovision with provision-key.sh --csr");
#else
        ESP_LOGW(TAG, "ignoring the IoT private key in prov: the secure element's is used");
#endif
    }
    nvs_close(h);
    if (!ok || prov->device_cert == NULL)
    {
        free(prov->device_cert);
        free(prov->device_key);
        memset(prov, 0, sizeof(*prov));
        ESP_LOGE(TAG, "incomplete provisioning: run infra/scripts/provision-key.sh");
        return false;
    }
    ESP_LOGI(TAG, "provisioned as %s", prov->thing_name);
    return true;
}
