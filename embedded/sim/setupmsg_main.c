// tinycrypt-setupmsg: run the key's BLE setup message code (port/setup_msg.c) from the command line,
// so the tests can feed it messages (CRYPT-30). One JSON input per line on stdin, one result per line.
//
//   tinycrypt-setupmsg wifi       a wifi write -> {"ok", "scan", "ssid", "password"} or {"ok": false, "err"}
//   tinycrypt-setupmsg register   a register write -> {"ok", "kind", "token", "payload"} or {"ok": false, "err"}
//   tinycrypt-setupmsg info       {"thing", "version", "ssid", "wifi" (0-5), "iot", "watch", "phone"} -> the info read
//   tinycrypt-setupmsg scan       [["ssid", rssi, secured], ...] in any order -> the scan read
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "setup_msg.h"

static void print_and_free(cJSON *out)
{
    char *text = cJSON_PrintUnformatted(out);
    puts(text ? text : "null");
    cJSON_free(text);
    cJSON_Delete(out);
}

static cJSON *failure(const char *err)
{
    cJSON *out = cJSON_CreateObject();
    cJSON_AddFalseToObject(out, "ok");
    cJSON_AddStringToObject(out, "err", err);
    return out;
}

static void one(const char *mode, const char *line, size_t len)
{
    const char *err = "?";
    if (strcmp(mode, "wifi") == 0)
    {
        setup_wifi_t w;
        if (!setup_parse_wifi(line, len, &w, &err))
            return print_and_free(failure(err));
        cJSON *out = cJSON_CreateObject();
        cJSON_AddTrueToObject(out, "ok");
        cJSON_AddBoolToObject(out, "scan", w.scan);
        cJSON_AddStringToObject(out, "ssid", w.ssid);
        cJSON_AddStringToObject(out, "password", w.password);
        return print_and_free(out);
    }
    if (strcmp(mode, "register") == 0)
    {
        setup_register_t r;
        if (!setup_parse_register(line, len, &r, &err))
            return print_and_free(failure(err));
        char *payload = setup_register_payload(&r);
        cJSON *out = cJSON_CreateObject();
        cJSON_AddTrueToObject(out, "ok");
        cJSON_AddStringToObject(out, "kind", setup_kind_name(r.kind));
        cJSON_AddStringToObject(out, "token", r.token);
        cJSON_AddStringToObject(out, "payload", payload ? payload : "");
        cJSON_free(payload);
        return print_and_free(out);
    }
    char buf[SETUP_READ_MAX + 1];
    cJSON *in = cJSON_ParseWithLength(line, len);
    int n = -1;
    if (strcmp(mode, "info") == 0 && cJSON_IsObject(in))
    {
        setup_info_t info = {
            .thing = cJSON_GetStringValue(cJSON_GetObjectItem(in, "thing")),
            .version = cJSON_GetStringValue(cJSON_GetObjectItem(in, "version")),
            .ssid = cJSON_GetStringValue(cJSON_GetObjectItem(in, "ssid")),
            .wifi = (setup_wifi_state_t)cJSON_GetNumberValue(cJSON_GetObjectItem(in, "wifi")),
            .iot = cJSON_IsTrue(cJSON_GetObjectItem(in, "iot")),
            .watch = cJSON_IsTrue(cJSON_GetObjectItem(in, "watch")),
            .phone = cJSON_IsTrue(cJSON_GetObjectItem(in, "phone")),
        };
        n = setup_info_json(&info, buf);
    }
    else if (strcmp(mode, "scan") == 0 && cJSON_IsArray(in))
    {
        setup_network_t nets[64];
        size_t count = 0;
        const cJSON *net;
        cJSON_ArrayForEach(net, in)
        {
            if (count == sizeof(nets) / sizeof(nets[0]))
                break;
            const char *ssid = cJSON_GetStringValue(cJSON_GetArrayItem(net, 0));
            snprintf(nets[count].ssid, sizeof(nets[count].ssid), "%s", ssid ? ssid : "");
            nets[count].rssi = (int8_t)cJSON_GetNumberValue(cJSON_GetArrayItem(net, 1));
            nets[count].secured = cJSON_GetNumberValue(cJSON_GetArrayItem(net, 2)) != 0;
            count++;
        }
        n = setup_scan_json(nets, setup_scan_tidy(nets, count), buf);
    }
    cJSON_Delete(in);
    puts(n >= 0 ? buf : "null");
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: tinycrypt-setupmsg wifi|register|info|scan < lines\n");
        return 2;
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, stdin)) > 0)
    {
        if (line[len - 1] == '\n')
            line[--len] = 0;
        one(argv[1], line, (size_t)len);
        fflush(stdout);
    }
    free(line);
    return 0;
}
