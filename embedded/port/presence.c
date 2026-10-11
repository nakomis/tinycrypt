// Shared user-presence wait (CRYPT-13). See presence.h.
#include "presence.h"

#include <string.h>

#include "ctaphid.h"

#define KEEPALIVE_MS 100
#define PUMP_MS 5

#define CID_LEN 4
#define CMD_OFFSET 4

// The channel of the report the core is handling (see presence_note_report).
static uint8_t current_cid[CID_LEN];

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

static bool is_init_packet(const uint8_t *report)
{
    return (report[CMD_OFFSET] & TYPE_INIT) != 0;
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
    new_action = last_approved || last_cancelled || memcmp(request_cid, last_cid, CID_LEN) != 0 ||
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
    for (size_t i = 0; i < n_sources; i++)
        if (sources[i]->disarm)
            sources[i]->disarm(sources[i]->ctx, result);
    return result;
}
