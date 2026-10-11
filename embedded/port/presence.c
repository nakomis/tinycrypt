// Shared user-presence wait (CRYPT-13). See presence.h.
#include "presence.h"

#include <string.h>

#include "ctaphid.h"

#define KEEPALIVE_MS 100
#define PUMP_MS 5

#define CID_LEN 4
#define CMD_OFFSET 4

static uint8_t request_cid[CID_LEN];
static bool have_request_cid;

static bool is_init_packet(const uint8_t *report)
{
    return (report[CMD_OFFSET] & TYPE_INIT) != 0;
}

void presence_note_report(const uint8_t report[PRESENCE_REPORT_SIZE])
{
    // Only requests that can ask for presence; continuation packets carry no command.
    uint8_t cmd = report[CMD_OFFSET];
    if (is_init_packet(report) && (cmd == CTAPHID_CBOR || cmd == CTAPHID_MSG))
    {
        memcpy(request_cid, report, CID_LEN);
        have_request_cid = true;
    }
}

static void send_error(const presence_io *io, const uint8_t *cid, uint8_t error)
{
    uint8_t frame[PRESENCE_REPORT_SIZE] = {0};
    memcpy(frame, cid, CID_LEN);
    frame[CMD_OFFSET] = CTAPHID_ERROR;
    frame[CMD_OFFSET + 1] = 0;  // BCNT, big-endian: one byte of payload
    frame[CMD_OFFSET + 2] = 1;
    frame[CMD_OFFSET + 3] = error;
    io->send(frame);
}

// Returns false if the requesting channel cancelled the request.
static bool handle_report(const presence_io *io, const uint8_t *report)
{
    if (!is_init_packet(report))
        return true;  // a stray continuation: nothing to answer
    bool same_channel = have_request_cid && memcmp(report, request_cid, CID_LEN) == 0;
    uint8_t cmd = report[CMD_OFFSET];
    if (same_channel && (cmd == CTAPHID_CANCEL || cmd == CTAPHID_INIT))
        return false;
    if (cmd != CTAPHID_CANCEL)  // a CANCEL from another channel has nothing to cancel
        send_error(io, report, CTAP1_ERR_CHANNEL_BUSY);
    return true;
}

int presence_wait(uint32_t timeout_ms, const presence_source *const *sources, size_t n_sources,
                  const presence_io *io)
{
    for (size_t i = 0; i < n_sources; i++)
        if (sources[i]->arm)
            sources[i]->arm(sources[i]->ctx);

    int result = 0;
    uint32_t start = io->now_ms();
    uint32_t next_keepalive = start;
    uint8_t report[PRESENCE_REPORT_SIZE];
    while (io->now_ms() - start < timeout_ms)
    {
        if ((int32_t)(io->now_ms() - next_keepalive) >= 0)
        {
            ctaphid_update_status(CTAPHID_STATUS_UPNEEDED);
            next_keepalive += KEEPALIVE_MS;
        }
        if (io->recv(report, PUMP_MS) > 0 && !handle_report(io, report))
            break;  // cancelled: not present
        bool approved = false;
        for (size_t i = 0; i < n_sources && !approved; i++)
            approved = sources[i]->approved && sources[i]->approved(sources[i]->ctx);
        if (approved)
        {
            result = 1;
            break;
        }
    }

    for (size_t i = 0; i < n_sources; i++)
        if (sources[i]->disarm)
            sources[i]->disarm(sources[i]->ctx, result);
    return result;
}
