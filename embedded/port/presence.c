// Shared user-presence wait (CRYPT-13). See presence.h.
#include "presence.h"

#include <string.h>

#include "cbor.h"
#include "ctap.h"
#include "ctaphid.h"
#include "sha256.h"
#include "u2f.h"

#define KEEPALIVE_MS 100
#define PUMP_MS 5

#define CID_LEN 4
#define CMD_OFFSET 4

// The channel of the report the core is handling (see presence_note_report).
static uint8_t current_cid[CID_LEN];

// The request solo1 is processing: its buffer, exactly as solo1 dispatched it (see
// tc_ctap_request below). NULL outside a request.
static const uint8_t *serving;
static size_t serving_len;
static bool serving_u2f;

// The request the current wait is for.
static bool request_bound;
static uint8_t request_hash[PRESENCE_REQUEST_HASH_LEN];
static char request_rp[PRESENCE_RP_MAX];
static char request_label[PRESENCE_LABEL_MAX];
static uint8_t last_hash[PRESENCE_REQUEST_HASH_LEN];

// The wait in progress, and the last one (for grouping U2F re-asks into one action). An external
// wait has no requesting channel (its CID is all zeros, which no channel is ever given).
static uint8_t request_cid[CID_LEN];
static bool external;
static volatile bool cancel_external;
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

bool presence_request_bound(void)
{
    return request_bound;
}

const uint8_t *presence_request_hash(void)
{
    return request_hash;
}

const char *presence_request_rp(void)
{
    return request_rp;
}

const char *presence_request_label(void)
{
    return request_label;
}

void presence_cancel_external(void)
{
    cancel_external = true;
}

void presence_clear_external_cancel(void)
{
    cancel_external = false;
}

static bool is_init_packet(const uint8_t *report)
{
    return (report[CMD_OFFSET] & TYPE_INIT) != 0;
}

// An rpId we're willing to show: a plain lower-case host name (browsers send A-labels).
static bool is_host_name(const char *s, size_t n)
{
    if (n == 0 || n > PRESENCE_RP_MAX - 1)
        return false;
    for (size_t i = 0; i < n; i++)
    {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-'))
            return false;  // also rejects an embedded NUL, so what's shown is all solo1 sees
    }
    return true;
}

// Finds the value of the one key matching `want_int` (or the text key `want_text`) in a map. False
// if it's missing or repeated: solo1 keeps the last of a repeated key, and a reader might show the
// first, so a repeat is refused rather than guessed at.
static bool find_unique(CborValue *map, int want_int, const char *want_text, CborValue *found)
{
    CborValue it;
    int seen = 0;
    if (!cbor_value_is_map(map) || cbor_value_enter_container(map, &it) != CborNoError)
        return false;
    while (!cbor_value_at_end(&it))
    {
        bool match = false;
        if (want_text)
        {
            bool eq = false;
            match = cbor_value_is_text_string(&it) &&
                    cbor_value_text_string_equals(&it, want_text, &eq) == CborNoError && eq;
        }
        else
        {
            int key;
            match = cbor_value_is_integer(&it) && cbor_value_get_int(&it, &key) == CborNoError && key == want_int;
        }
        if (cbor_value_advance(&it) != CborNoError || cbor_value_at_end(&it))
            return false;
        if (match)
        {
            *found = it;
            seen++;
        }
        if (cbor_value_advance(&it) != CborNoError)
            return false;
    }
    return seen == 1;
}

// CTAP2 makeCredential (0x01): rp is key 2, a map with "id". getAssertion (0x02): rpId is key 1.
// Runs only once solo1 has parsed the same bytes successfully (it's asking for presence), so the
// CBOR is well formed and no deeper than solo1 already walked. Fails closed: false means the site
// can't be shown unambiguously, and the watch isn't offered.
static bool parse_rp(const uint8_t *payload, size_t len, char *out, size_t out_len)
{
    out[0] = 0;
    if (len < 2 || (payload[0] != 0x01 && payload[0] != 0x02))
        return false;
    CborParser parser;
    CborValue map, value;
    if (cbor_parser_init(payload + 1, len - 1, 0, &parser, &map) != CborNoError)
        return false;
    if (!find_unique(&map, payload[0] == 0x01 ? 2 : 1, NULL, &value))
        return false;
    if (payload[0] == 0x01)
    {
        CborValue rp = value;
        if (!find_unique(&rp, 0, "id", &value))
            return false;
    }
    size_t n = 0;
    if (!cbor_value_is_text_string(&value) || cbor_value_get_string_length(&value, &n) != CborNoError ||
        n >= out_len)
        return false;
    size_t copied = out_len;
    if (cbor_value_copy_text_string(&value, out, &copied, NULL) != CborNoError || copied != n ||
        !is_host_name(out, n))
    {
        out[0] = 0;
        return false;
    }
    return true;
}

// solo1's ctaphid.c is built with ctap_request/u2f_request renamed to these (solo_sources.cmake),
// so the request a wait binds to is the very buffer solo1 is acting on: there's no copy to
// reassemble or keep in step with its CTAPHID state machine.
uint8_t tc_ctap_request(uint8_t *pkt_raw, int length, CTAP_RESPONSE *resp);
void tc_u2f_request(struct u2f_request_apdu *req, CTAP_RESPONSE *resp);

uint8_t tc_ctap_request(uint8_t *pkt_raw, int length, CTAP_RESPONSE *resp)
{
    serving = pkt_raw;
    serving_len = length > 0 ? (size_t)length : 0;
    serving_u2f = false;
    uint8_t status = ctap_request(pkt_raw, length, resp);
    serving = NULL;
    return status;
}

void tc_u2f_request(struct u2f_request_apdu *req, CTAP_RESPONSE *resp)
{
    // The APDU as u2f_request reads it: the header and LC bytes plus LC bytes of payload.
    size_t lc = (size_t)req->LC1 << 16 | (size_t)req->LC2 << 8 | req->LC3;
    size_t max = CTAPHID_BUFFER_SIZE - 7;
    serving = (const uint8_t *)req;
    serving_len = 7 + (lc < max ? lc : max);
    serving_u2f = true;
    u2f_request(req, resp);
    serving = NULL;
}

// Binds the wait to the request being served. Bound only if the watch can be told exactly what
// it's approving: U2F register/authenticate (by hash; U2F carries no site name), or a CTAP2
// makeCredential/getAssertion whose rpId is unambiguous. Anything else (CTAP2 reset, solo1's
// vendor commands, a site name we won't show) is button only.
static void bind_request(void)
{
    request_bound = false;
    request_rp[0] = 0;
    memset(request_hash, 0, sizeof(request_hash));
    if (serving == NULL || serving_len == 0)
        return;
    SHA256_CTX sha;
    sha256_init(&sha);
    sha256_update(&sha, serving, serving_len);
    sha256_final(&sha, request_hash);
    if (serving_u2f)
        request_bound = serving_len >= 2 && (serving[1] == U2F_REGISTER || serving[1] == U2F_AUTHENTICATE);
    else
        request_bound = parse_rp(serving, serving_len, request_rp, sizeof(request_rp));
}

void presence_note_report(const uint8_t report[PRESENCE_REPORT_SIZE])
{
    memcpy(current_cid, report, CID_LEN);
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
    bool same_channel = !external && memcmp(report, request_cid, CID_LEN) == 0;
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

static int wait(uint32_t timeout_ms, const presence_source *const *sources, size_t n_sources,
                const presence_io *io)
{
    uint32_t start = io->now_ms();
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
        if (external && cancel_external)
        {
            cancelled = true;
            break;
        }
        if (!external && (int32_t)(io->now_ms() - next_keepalive) >= 0)
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

int presence_wait(uint32_t timeout_ms, const presence_source *const *sources, size_t n_sources,
                  const presence_io *io)
{
    external = false;
    memcpy(request_cid, current_cid, CID_LEN);
    request_label[0] = 0;
    bind_request();
    return wait(timeout_ms, sources, n_sources, io);
}

int presence_wait_external(uint32_t timeout_ms, const uint8_t hash[PRESENCE_REQUEST_HASH_LEN], const char *label,
                           const presence_source *const *sources, size_t n_sources, const presence_io *io)
{
    external = true;
    memset(request_cid, 0, CID_LEN);
    memcpy(request_hash, hash, PRESENCE_REQUEST_HASH_LEN);
    request_rp[0] = 0;
    // Printable ASCII only: it goes into the push as it is.
    size_t n = 0;
    for (; label[n] && n < PRESENCE_LABEL_MAX - 1; n++)
        request_label[n] = label[n] >= 0x20 && label[n] < 0x7f ? label[n] : '?';
    request_label[n] = 0;
    request_bound = true;
    int result = wait(timeout_ms, sources, n_sources, io);
    cancel_external = false;
    external = false;
    request_bound = false;
    return result;
}
