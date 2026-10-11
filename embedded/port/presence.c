// Shared user-presence wait (CRYPT-13). See presence.h.
#include "presence.h"

#include <string.h>

#include "cbor.h"
#include "ctaphid.h"
#include "sha256.h"

#define KEEPALIVE_MS 100
#define PUMP_MS 5

#define CID_LEN 4
#define CMD_OFFSET 4

// The channel of the report the core is handling (see presence_note_report).
static uint8_t current_cid[CID_LEN];

// Reassembly of the latest CBOR/MSG request, mirroring solo1's single buffer: a new request on
// another channel while one is still arriving is ignored (solo1 answers it CHANNEL_BUSY).
#define HEAD_MAX 1024  // enough of the CBOR to find the rpId
static struct
{
    uint8_t cid[CID_LEN];
    uint16_t bcnt, got;
    uint8_t next_seq;
    bool active, complete;
    SHA256_CTX sha;
    uint8_t head[HEAD_MAX];
    uint16_t head_len;
    uint8_t hash[PRESENCE_REQUEST_HASH_LEN];
} assembly;

// The request the current wait is for.
static uint8_t request_hash[PRESENCE_REQUEST_HASH_LEN];
static char request_rp[PRESENCE_RP_MAX];
static uint8_t last_hash[PRESENCE_REQUEST_HASH_LEN];

// The wait in progress, and the last one (for grouping U2F re-asks into one action).
static uint8_t request_cid[CID_LEN];
static bool new_action;
static bool cancelled;
static bool last_approved = true;  // so the first wait is always a new action
static bool last_cancelled;
static uint32_t last_start_ms, last_end_ms;
static uint8_t last_cid[CID_LEN];

// An INIT that aborted the wait: the core's answer on that channel is dropped until the INIT has
// been replayed.
static bool aborted;
static uint8_t aborted_cid[CID_LEN];
static uint8_t deferred_init[PRESENCE_REPORT_SIZE];

bool presence_is_new_action(void)
{
    return new_action;
}

bool presence_was_cancelled(void)
{
    return cancelled;
}

const uint8_t *presence_request_hash(void)
{
    return request_hash;
}

const char *presence_request_rp(void)
{
    return request_rp;
}

static bool is_init_packet(const uint8_t *report)
{
    return (report[CMD_OFFSET] & TYPE_INIT) != 0;
}

// CTAP2 makeCredential (0x01): rp is key 2, a map with "id". getAssertion (0x02): rpId is key 1.
static void parse_rp(const uint8_t *payload, size_t len, char *out, size_t out_len)
{
    out[0] = 0;
    if (len < 2 || (payload[0] != 0x01 && payload[0] != 0x02))
        return;
    int want = payload[0] == 0x01 ? 2 : 1;
    CborParser parser;
    CborValue map, it;
    if (cbor_parser_init(payload + 1, len - 1, 0, &parser, &map) != CborNoError || !cbor_value_is_map(&map) ||
        cbor_value_enter_container(&map, &it) != CborNoError)
        return;
    while (!cbor_value_at_end(&it))
    {
        int key = -1;
        bool is_key = cbor_value_is_integer(&it) && cbor_value_get_int(&it, &key) == CborNoError;
        if (cbor_value_advance(&it) != CborNoError || cbor_value_at_end(&it))
            return;
        if (is_key && key == want)
        {
            CborValue value = it;
            if (want == 2)
            {
                CborValue found;
                if (!cbor_value_is_map(&it) || cbor_value_map_find_value(&it, "id", &found) != CborNoError)
                    return;
                value = found;
            }
            size_t n = out_len;
            if (cbor_value_is_text_string(&value) && cbor_value_copy_text_string(&value, out, &n, NULL) == CborNoError)
                return;  // copied, NUL-terminated
            out[0] = 0;
            return;
        }
        if (cbor_value_advance(&it) != CborNoError)
            return;
    }
}

static void assembly_add(const uint8_t *data, uint16_t n)
{
    sha256_update(&assembly.sha, data, n);
    uint16_t room = HEAD_MAX - assembly.head_len;
    uint16_t take = n < room ? n : room;
    memcpy(assembly.head + assembly.head_len, data, take);
    assembly.head_len += take;
    assembly.got += n;
    if (assembly.got >= assembly.bcnt)
    {
        sha256_final(&assembly.sha, assembly.hash);
        assembly.complete = true;
        assembly.active = false;
    }
}

static void track_request(const uint8_t *report)
{
    uint8_t cmd = report[CMD_OFFSET];
    if (is_init_packet(report))
    {
        if (cmd != CTAPHID_CBOR && cmd != CTAPHID_MSG)
            return;
        if (assembly.active && memcmp(report, assembly.cid, CID_LEN) != 0)
            return;  // another channel mid-message: solo1 will refuse this one
        memset(&assembly, 0, sizeof(assembly));
        memcpy(assembly.cid, report, CID_LEN);
        assembly.bcnt = (uint16_t)(report[CMD_OFFSET + 1] << 8 | report[CMD_OFFSET + 2]);
        assembly.active = true;
        sha256_init(&assembly.sha);
        uint16_t n = assembly.bcnt < PRESENCE_REPORT_SIZE - 7 ? assembly.bcnt : PRESENCE_REPORT_SIZE - 7;
        assembly_add(report + 7, n);
        return;
    }
    if (!assembly.active || memcmp(report, assembly.cid, CID_LEN) != 0 || cmd != assembly.next_seq)
        return;
    assembly.next_seq++;
    uint16_t left = assembly.bcnt - assembly.got;
    uint16_t n = left < PRESENCE_REPORT_SIZE - 5 ? left : PRESENCE_REPORT_SIZE - 5;
    assembly_add(report + 5, n);
}

void presence_note_report(const uint8_t report[PRESENCE_REPORT_SIZE])
{
    memcpy(current_cid, report, CID_LEN);
    track_request(report);
}

bool presence_filter_send(const uint8_t report[PRESENCE_REPORT_SIZE])
{
    return !(aborted && memcmp(report, aborted_cid, CID_LEN) == 0);
}

bool presence_take_deferred(uint8_t report[PRESENCE_REPORT_SIZE])
{
    if (!aborted)
        return false;
    aborted = false;  // the core's answer to the aborted request has gone (dropped)
    memcpy(report, deferred_init, PRESENCE_REPORT_SIZE);
    return true;
}

static void send_frame(const presence_io *io, const uint8_t *cid, uint8_t cmd, uint8_t payload)
{
    uint8_t frame[PRESENCE_REPORT_SIZE] = {0};
    memcpy(frame, cid, CID_LEN);
    frame[CMD_OFFSET] = cmd;
    frame[CMD_OFFSET + 1] = 0;  // BCNT, big-endian: one byte of payload
    frame[CMD_OFFSET + 2] = 1;
    frame[CMD_OFFSET + 3] = payload;
    io->send(frame);
}

// Returns false if the requesting channel cancelled the request.
static bool handle_report(const presence_io *io, const uint8_t *report)
{
    if (!is_init_packet(report))
        return true;  // a stray continuation: nothing to answer
    bool same_channel = memcmp(report, request_cid, CID_LEN) == 0;
    uint8_t cmd = report[CMD_OFFSET];
    if (same_channel && cmd == CTAPHID_CANCEL)
        return false;
    if (same_channel && cmd == CTAPHID_INIT)
    {
        // Abort: drop the core's answer to this request and replay the INIT after it.
        memcpy(deferred_init, report, PRESENCE_REPORT_SIZE);
        memcpy(aborted_cid, request_cid, CID_LEN);
        aborted = true;
        return false;
    }
    if (cmd != CTAPHID_CANCEL)  // a CANCEL from another channel has nothing to cancel
        send_frame(io, report, CTAPHID_ERROR, CTAP1_ERR_CHANNEL_BUSY);
    return true;
}

int presence_wait(uint32_t timeout_ms, const presence_source *const *sources, size_t n_sources,
                  const presence_io *io)
{
    uint32_t start = io->now_ms();
    memcpy(request_cid, current_cid, CID_LEN);
    if (assembly.complete && memcmp(assembly.cid, request_cid, CID_LEN) == 0)
    {
        memcpy(request_hash, assembly.hash, sizeof(request_hash));
        parse_rp(assembly.head, assembly.head_len < assembly.bcnt ? assembly.head_len : assembly.bcnt, request_rp,
                 sizeof(request_rp));
    }
    else
    {
        memset(request_hash, 0, sizeof(request_hash));
        request_rp[0] = 0;
    }
    new_action = last_approved || last_cancelled || memcmp(request_cid, last_cid, CID_LEN) != 0 ||
                 memcmp(request_hash, last_hash, sizeof(request_hash)) != 0 ||
                 start - last_end_ms >= PRESENCE_SAME_ACTION_GAP_MS ||
                 last_end_ms - last_start_ms >= PRESENCE_SHORT_WAIT_MS;
    cancelled = false;
    for (size_t i = 0; i < n_sources; i++)
        if (sources[i]->arm)
            sources[i]->arm(sources[i]->ctx);

    int result = 0;
    uint32_t next_keepalive = start;
    uint8_t report[PRESENCE_REPORT_SIZE];
    while (io->now_ms() - start < timeout_ms)
    {
        if ((int32_t)(io->now_ms() - next_keepalive) >= 0)
        {
            send_frame(io, request_cid, CTAPHID_KEEPALIVE, CTAPHID_STATUS_UPNEEDED);
            next_keepalive += KEEPALIVE_MS;
        }
        if (io->recv(report, PUMP_MS) > 0 && !handle_report(io, report))
        {
            cancelled = true;
            break;
        }
        bool approved = false;
        for (size_t i = 0; i < n_sources && !approved; i++)
            approved = sources[i]->approved && sources[i]->approved(sources[i]->ctx);
        if (approved)
        {
            result = 1;
            break;
        }
    }

    last_start_ms = start;
    last_end_ms = io->now_ms();
    last_approved = result == 1;
    last_cancelled = cancelled;
    memcpy(last_cid, request_cid, CID_LEN);
    memcpy(last_hash, request_hash, sizeof(last_hash));
    for (size_t i = 0; i < n_sources; i++)
        if (sources[i]->disarm)
            sources[i]->disarm(sources[i]->ctx, result);
    return result;
}
