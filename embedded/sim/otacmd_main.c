// tinycrypt-otacmd: run the key's Fleet command parser (port/ota_cmd.c) from the command line, so
// the tests can feed it commands (CRYPT-14).
//
//   tinycrypt-otacmd                        one JSON command per line on stdin; one JSON result per line
//   tinycrypt-otacmd --valid <version>      prints 1 or 0
//   tinycrypt-otacmd --cmp <a> <b>          prints -1, 0 or 1
//   tinycrypt-otacmd --approval <sha256 hex> <version>   prints the approval hash and label
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "ota_cmd.h"

static void print_hex(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        printf("%02x", b[i]);
}

static const char *kind_name(ota_cmd_kind kind)
{
    switch (kind)
    {
    case OTA_CMD_GET_VERSION:
        return "get_version";
    case OTA_CMD_PING:
        return "ping";
    case OTA_CMD_CANCEL:
        return "cancel";
    case OTA_CMD_UPDATE:
        return "update";
    case OTA_CMD_CHUNK:
        return "chunk";
    case OTA_CMD_ERROR:
        return "error";
    }
    return "?";
}

int main(int argc, char *argv[])
{
    if (argc == 3 && strcmp(argv[1], "--valid") == 0)
    {
        printf("%d\n", ota_version_valid(argv[2]));
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "--cmp") == 0)
    {
        int c = ota_version_cmp(argv[2], argv[3]);
        printf("%d\n", c < 0 ? -1 : c > 0);
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "--approval") == 0)
    {
        uint8_t sha[OTA_SHA256_LEN], hash[32];
        if (strlen(argv[2]) != 64)
            return 2;
        for (int i = 0; i < OTA_SHA256_LEN; i++)
            sscanf(&argv[2][2 * i], "%2hhx", &sha[i]);
        ota_approval_hash(sha, argv[3], hash);
        char label[OTA_LABEL_MAX];
        ota_approval_label(argv[3], label);
        print_hex(hash, sizeof(hash));
        printf("\n%s\n", label);
        return 0;
    }
    if (argc != 1)
    {
        fprintf(stderr, "usage: see the comment at the top of otacmd_main.c\n");
        return 2;
    }

    static ota_chunks chunks;
    static ota_cmd cmd;
    static char line[64 * 1024];
    while (fgets(line, sizeof(line), stdin) != NULL)
    {
        ota_cmd_parse(&chunks, line, strlen(line), &cmd);
        cJSON *out = cJSON_CreateObject();
        cJSON_AddStringToObject(out, "kind", kind_name(cmd.kind));
        if (cmd.kind == OTA_CMD_UPDATE)
        {
            char hex[2 * OTA_SHA256_LEN + 1];
            for (int i = 0; i < OTA_SHA256_LEN; i++)
                snprintf(&hex[2 * i], 3, "%02x", cmd.sha256[i]);
            cJSON_AddStringToObject(out, "url", cmd.url);
            cJSON_AddStringToObject(out, "version", cmd.version);
            cJSON_AddStringToObject(out, "sha256", hex);
        }
        else if (cmd.kind == OTA_CMD_CHUNK)
        {
            cJSON_AddNumberToObject(out, "index", cmd.chunk_index);
            cJSON_AddNumberToObject(out, "total", cmd.total_chunks);
        }
        else if (cmd.kind == OTA_CMD_ERROR)
        {
            cJSON_AddBoolToObject(out, "ota_error", cmd.ota_error);
            cJSON_AddStringToObject(out, "message", cmd.message);
        }
        char *text = cJSON_PrintUnformatted(out);
        printf("%s\n", text);
        fflush(stdout);
        cJSON_free(text);
        cJSON_Delete(out);
    }
    ota_cmd_reset(&chunks);
    return 0;
}
