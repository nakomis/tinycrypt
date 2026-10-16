// Storage for the INSECURE software secure element on an ESP32 with no ATECC608B (se_soft.h):
// one blob in NVS, readable by anyone with the board. TEST ONLY.
#include "se_soft.h"

#include "nvs.h"

#define NS "tinycrypt"
#define KEY "se_soft"

int se_soft_store_load(void *buf, size_t len)
{
    nvs_handle_t h;
    size_t got = len;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK)
        return -1;
    esp_err_t err = nvs_get_blob(h, KEY, buf, &got);
    nvs_close(h);
    return err == ESP_OK && got == len ? 0 : -1;
}

int se_soft_store_save(const void *buf, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK)
        return -1;
    esp_err_t err = nvs_set_blob(h, KEY, buf, len);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK ? 0 : -1;
}

bool se_soft_store_exists(void)
{
    nvs_handle_t h;
    size_t len = 0;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK)
        return false;
    esp_err_t err = nvs_get_blob(h, KEY, NULL, &len);
    nvs_close(h);
    return err == ESP_OK;
}
