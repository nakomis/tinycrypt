// The key's BLE setup messages (CRYPT-30). See setup_msg.h.
#include "setup_msg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static cJSON *parse(const char *json, size_t len, const char **err)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL || !cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        *err = "not a JSON object";
        return NULL;
    }
    return root;
}

// A string member of at most `max` bytes, with no control characters.
static bool get_string(const cJSON *root, const char *name, char *out, size_t max, const char **err)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item))
    {
        *err = "missing field";
        return false;
    }
    size_t len = strlen(item->valuestring);
    if (len > max)
    {
        *err = "field too long";
        return false;
    }
    for (size_t i = 0; i < len; i++)
        if ((unsigned char)item->valuestring[i] < 0x20)
        {
            *err = "control character";
            return false;
        }
    memcpy(out, item->valuestring, len + 1);
    return true;
}

bool setup_parse_wifi(const char *json, size_t len, setup_wifi_t *out, const char **err)
{
    memset(out, 0, sizeof(*out));
    cJSON *root = parse(json, len, err);
    if (root == NULL)
        return false;
    bool ok;
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "scan")))
    {
        out->scan = true;
        ok = true;
    }
    else
    {
        ok = get_string(root, "ssid", out->ssid, SETUP_SSID_MAX, err) &&
             get_string(root, "password", out->password, SETUP_PASSWORD_MAX, err);
        size_t pw = strlen(out->password);
        if (ok && out->ssid[0] == 0)
        {
            *err = "empty SSID";
            ok = false;
        }
        else if (ok && pw != 0 && pw < 8)
        {
            *err = "password too short";  // WPA2 needs 8; "" is an open network
            ok = false;
        }
        else if (ok && pw == 64 && strspn(out->password, "0123456789abcdefABCDEF") != 64)
        {
            *err = "64 characters must be a hex PSK";
            ok = false;
        }
    }
    cJSON_Delete(root);
    if (!ok)
        memset(out, 0, sizeof(*out));  // no password left lying about
    return ok;
}

bool setup_parse_register(const char *json, size_t len, setup_register_t *out, const char **err)
{
    memset(out, 0, sizeof(*out));
    cJSON *root = parse(json, len, err);
    if (root == NULL)
        return false;
    char kind[8];
    bool ok = get_string(root, "kind", kind, sizeof(kind) - 1, err) &&
              get_string(root, "token", out->token, SETUP_TOKEN_MAX, err);
    cJSON_Delete(root);
    if (!ok)
        return false;
    if (strcmp(kind, "watch") == 0)
        out->kind = SETUP_KIND_WATCH;
    else if (strcmp(kind, "phone") == 0)
        out->kind = SETUP_KIND_PHONE;
    else
    {
        *err = "unknown kind";
        return false;
    }
    size_t n = strlen(out->token);
    for (size_t i = 0; i < n; i++)
        if (out->token[i] >= 'A' && out->token[i] <= 'F')
            out->token[i] += 'a' - 'A';
    if (n < SETUP_TOKEN_MIN || strspn(out->token, "0123456789abcdef") != n)
    {
        *err = "token must be hex";
        return false;
    }
    return true;
}

const char *setup_kind_name(setup_kind_t kind)
{
    return kind == SETUP_KIND_PHONE ? "phone" : "watch";
}

const char *setup_wifi_state_name(setup_wifi_state_t state)
{
    switch (state)
    {
    case SETUP_WIFI_UNSET:
        return "unset";
    case SETUP_WIFI_CONNECTING:
        return "connecting";
    case SETUP_WIFI_CONNECTED:
        return "connected";
    case SETUP_WIFI_WRONG_PASSWORD:
        return "wrong-password";
    case SETUP_WIFI_NOT_FOUND:
        return "not-found";
    case SETUP_WIFI_FAILED:
        return "failed";
    }
    return "failed";
}

static int print_into(cJSON *root, char out[SETUP_READ_MAX + 1])
{
    bool ok = root != NULL && cJSON_PrintPreallocated(root, out, SETUP_READ_MAX + 1, false);
    cJSON_Delete(root);
    return ok ? (int)strlen(out) : -1;
}

int setup_info_json(const setup_info_t *info, char out[SETUP_READ_MAX + 1])
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "thing", info->thing ? info->thing : "");
    cJSON_AddStringToObject(root, "version", info->version ? info->version : "");
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wifi, "ssid", info->ssid ? info->ssid : "");
    cJSON_AddStringToObject(wifi, "state", setup_wifi_state_name(info->wifi));
    cJSON_AddBoolToObject(root, "iot", info->iot);
    cJSON_AddBoolToObject(root, "watch", info->watch);
    cJSON_AddBoolToObject(root, "phone", info->phone);
    return print_into(root, out);
}

int setup_scan_json(const setup_network_t *nets, size_t n, char out[SETUP_READ_MAX + 1])
{
    for (size_t keep = n;; keep--)
    {
        cJSON *root = cJSON_CreateArray();
        for (size_t i = 0; i < keep; i++)
        {
            cJSON *net = cJSON_CreateArray();
            cJSON_AddItemToArray(net, cJSON_CreateString(nets[i].ssid));
            cJSON_AddItemToArray(net, cJSON_CreateNumber(nets[i].rssi));
            cJSON_AddItemToArray(net, cJSON_CreateNumber(nets[i].secured ? 1 : 0));
            cJSON_AddItemToArray(root, net);
        }
        int len = print_into(root, out);
        if (len >= 0 || keep == 0)
            return len;
    }
}

static int stronger_first(const void *a, const void *b)
{
    return ((const setup_network_t *)b)->rssi - ((const setup_network_t *)a)->rssi;
}

size_t setup_scan_tidy(setup_network_t *nets, size_t n)
{
    qsort(nets, n, sizeof(*nets), stronger_first);
    size_t kept = 0;
    for (size_t i = 0; i < n; i++)
    {
        if (nets[i].ssid[0] == 0)
            continue;
        bool seen = false;
        for (size_t j = 0; j < kept && !seen; j++)
            seen = strcmp(nets[j].ssid, nets[i].ssid) == 0;
        if (!seen && kept < SETUP_SCAN_MAX)
            nets[kept++] = nets[i];
    }
    return kept;
}

char *setup_register_payload(const setup_register_t *reg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "kind", setup_kind_name(reg->kind));
    cJSON_AddStringToObject(root, "token", reg->token);
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}
