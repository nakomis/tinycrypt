// Fleet's command protocol (CRYPT-14). See ota_cmd.h.
#include "ota_cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "sha256.h"

#define APPROVAL_DOMAIN "tinycrypt-ota-v1"

void ota_cmd_reset(ota_chunks *chunks)
{
    for (int i = 0; i < OTA_MAX_CHUNKS; i++)
        free(chunks->chunk[i]);
    memset(chunks, 0, sizeof(*chunks));
}

bool ota_version_valid(const char *v)
{
    int parts = 0, digits = 0;
    for (const char *p = v;; p++)
    {
        if (*p >= '0' && *p <= '9')
        {
            if (++digits > 4)
                return false;
        }
        else if (*p == '.' || *p == 0)
        {
            if (digits == 0)
                return false;
            parts++;
            digits = 0;
            if (*p == 0)
                return parts == 3;
        }
        else
            return false;
    }
}

int ota_version_cmp(const char *a, const char *b)
{
    for (int i = 0; i < 3; i++)
    {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y)
            return x < y ? -1 : 1;
        if (*a == '.')
            a++;
        if (*b == '.')
            b++;
    }
    return 0;
}

void ota_approval_hash(const uint8_t image_sha256[OTA_SHA256_LEN], const char *version, uint8_t out[32])
{
    SHA256_CTX sha;
    sha256_init(&sha);
    sha256_update(&sha, (const uint8_t *)APPROVAL_DOMAIN, strlen(APPROVAL_DOMAIN));
    sha256_update(&sha, image_sha256, OTA_SHA256_LEN);
    sha256_update(&sha, (const uint8_t *)version, strlen(version));
    sha256_final(&sha, out);
}

void ota_approval_label(const char *version, char out[OTA_LABEL_MAX])
{
    snprintf(out, OTA_LABEL_MAX, "Install tinycrypt %s?", version);
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool parse_sha256(const char *hex, uint8_t out[OTA_SHA256_LEN])
{
    if (strlen(hex) != 2 * OTA_SHA256_LEN)
        return false;
    for (int i = 0; i < OTA_SHA256_LEN; i++)
    {
        int hi = hex_nibble(hex[2 * i]), lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

// HTTPS only, and nothing a URL never needs (spaces, control characters, non-ASCII).
static bool url_ok(const char *url)
{
    if (strncmp(url, "https://", 8) != 0 || url[8] == 0)
        return false;
    for (const char *p = url; *p; p++)
        if (*p <= 0x20 || *p >= 0x7f)
            return false;
    return true;
}

static const char *string_field(const cJSON *root, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static void fail(ota_cmd *out, bool ota_error, const char *message)
{
    out->kind = OTA_CMD_ERROR;
    out->ota_error = ota_error;
    snprintf(out->message, sizeof(out->message), "%s", message);
}

// Fills in an update from its parts, or fails. `sha256` may be NULL (missing).
static void make_update(ota_cmd *out, const char *url, const char *version, const uint8_t *sha256)
{
    if (url == NULL || !url_ok(url) || strlen(url) >= sizeof(out->url))
        return fail(out, true, "Invalid firmware URL");
    if (version == NULL || !ota_version_valid(version))
        return fail(out, true, "Invalid version");
    if (sha256 == NULL)
        return fail(out, true, "sha256 required");
    out->kind = OTA_CMD_UPDATE;
    snprintf(out->url, sizeof(out->url), "%s", url);
    snprintf(out->version, sizeof(out->version), "%s", version);
    memcpy(out->sha256, sha256, OTA_SHA256_LEN);
}

static void chunk(ota_chunks *chunks, const cJSON *root, ota_cmd *out)
{
    const cJSON *index = cJSON_GetObjectItemCaseSensitive(root, "chunk_index");
    const cJSON *total = cJSON_GetObjectItemCaseSensitive(root, "total_chunks");
    const char *data = string_field(root, "chunk_data");
    const char *version = string_field(root, "version");
    if (!cJSON_IsNumber(index) || !cJSON_IsNumber(total) || data == NULL || version == NULL ||
        total->valuedouble < 1 || total->valuedouble > OTA_MAX_CHUNKS || index->valuedouble < 0 ||
        index->valuedouble >= total->valuedouble || index->valuedouble != (int)index->valuedouble ||
        total->valuedouble != (int)total->valuedouble || strlen(version) >= OTA_VERSION_MAX)
        return fail(out, true, "Invalid chunk parameters");
    int i = (int)index->valuedouble, n = (int)total->valuedouble;
    // A chunk from a different update (count or version) starts again.
    if (chunks->total != n || strcmp(chunks->version, version) != 0)
    {
        ota_cmd_reset(chunks);
        chunks->total = n;
        snprintf(chunks->version, sizeof(chunks->version), "%s", version);
    }
    const char *hex = string_field(root, "sha256");
    if (hex != NULL)
    {
        if (!parse_sha256(hex, chunks->sha256))
            return fail(out, true, "Invalid sha256");
        chunks->have_sha256 = true;
    }
    free(chunks->chunk[i]);
    chunks->chunk[i] = strdup(data);
    if (chunks->chunk[i] == NULL)
    {
        ota_cmd_reset(chunks);
        return fail(out, true, "Out of memory");
    }

    out->kind = OTA_CMD_CHUNK;
    out->chunk_index = i;
    out->total_chunks = n;
    size_t len = 0;
    for (int k = 0; k < n; k++)
    {
        if (chunks->chunk[k] == NULL)
            return;  // more to come
        len += strlen(chunks->chunk[k]);
    }
    if (len >= sizeof(out->url))
    {
        ota_cmd_reset(chunks);
        return fail(out, true, "Invalid firmware URL");
    }
    // All here: assemble the URL in place, then check it like a single ota_update.
    out->url[0] = 0;
    for (int k = 0; k < n; k++)
        strcat(out->url, chunks->chunk[k]);
    bool have_sha = chunks->have_sha256;
    if (!url_ok(out->url))
        fail(out, true, "Invalid firmware URL");
    else if (!ota_version_valid(chunks->version))
        fail(out, true, "Invalid version");
    else if (!have_sha)
        fail(out, true, "sha256 required");
    else
    {
        out->kind = OTA_CMD_UPDATE;
        snprintf(out->version, sizeof(out->version), "%s", chunks->version);
        memcpy(out->sha256, chunks->sha256, OTA_SHA256_LEN);
    }
    ota_cmd_reset(chunks);
}

void ota_cmd_parse(ota_chunks *chunks, const char *json, size_t len, ota_cmd *out)
{
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_ParseWithLength(json, len);
    const char *command = root ? string_field(root, "command") : NULL;
    if (root == NULL || !cJSON_IsObject(root))
        fail(out, false, "Invalid JSON command");
    else if (command == NULL)
        fail(out, false, "Missing command");
    else if (strcmp(command, "get_version") == 0)
        out->kind = OTA_CMD_GET_VERSION;
    else if (strcmp(command, "ping") == 0)
        out->kind = OTA_CMD_PING;
    else if (strcmp(command, "ota_cancel") == 0)
    {
        ota_cmd_reset(chunks);
        out->kind = OTA_CMD_CANCEL;
    }
    else if (strcmp(command, "ota_update") == 0)
    {
        ota_cmd_reset(chunks);
        const char *hex = string_field(root, "sha256");
        uint8_t sha[OTA_SHA256_LEN];
        if (hex != NULL && !parse_sha256(hex, sha))
            fail(out, true, "Invalid sha256");
        else
            make_update(out, string_field(root, "url"), string_field(root, "version"), hex ? sha : NULL);
    }
    else if (strcmp(command, "url_chunk") == 0)
        chunk(chunks, root, out);
    else
    {
        char message[sizeof(out->message)];
        snprintf(message, sizeof(message), "Unknown command: %.60s", command);
        fail(out, false, message);
    }
    cJSON_Delete(root);
}
