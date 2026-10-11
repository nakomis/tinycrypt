// Shared user-presence wait (CRYPT-13), used by every port's ctap_user_presence_test().
//
// Presence is the physical button OR the watch, whichever answers first. Each way of getting
// presence is a source: the wait arms them all, polls them, and the first to approve wins.
// Sources that only have side effects (the push notifier) never approve.
//
// While waiting, CTAPHID traffic is handled here and never handed back to the core: solo1 keeps
// the request's channel in a static buffer for the whole wait, and re-entering it (an INIT from
// another client resets that buffer) sends later KEEPALIVEs to CID 0 and lets any channel CANCEL.
//   - KEEPALIVE(UPNEEDED) every 100 ms, sent here to the requesting channel.
//   - CANCEL on the requesting channel ends the wait: not present. The core then answers the
//     request (CTAP2: ACTION_TIMEOUT, see below).
//   - INIT on the requesting channel aborts the transaction, as CTAPHID specifies: the wait ends,
//     the core's answer to the aborted request is dropped (presence_filter_send), and the INIT is
//     replayed to the core afterwards (presence_take_deferred) so the client gets its INIT reply.
//   - Anything that starts a request on another channel gets ERROR(CHANNEL_BUSY).
//
// Binding (CRYPT-18): the wait knows which request it is for. The requesting channel's CTAP message
// is reassembled from the reports noted, hashed (SHA-256 of the CTAPHID payload: the CBOR command
// byte and map, or the U2F APDU) and, for CTAP2 makeCredential/getAssertion, its rpId read. Sources
// get both while armed: the push shows the site and carries the hash, and the watch's challenge
// includes the hash, so an approval can only ever apply to this request.
//
// Contract with solo1: the result is only ever 1 (present) or 0 (not present). A cancelled CTAP2
// request therefore reports ACTION_TIMEOUT rather than KEEPALIVE_CANCEL: -1 can't be returned,
// because solo1's U2F and wallet code treat any non-zero result as present.
//
// Port integration, in the CTAPHID receive loop:
//     presence_note_report(report);       // before ctaphid_handle_packet()
//     ctaphid_handle_packet(report);
//     while (presence_take_deferred(report))
//         ctaphid_handle_packet(report);  // an INIT that aborted a wait
// and in the core's usbhid_send(): `if (!presence_filter_send(report)) return;`.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PRESENCE_REPORT_SIZE 64
#define PRESENCE_REQUEST_HASH_LEN 32
#define PRESENCE_RP_MAX 128

// A short wait (U2F's 750 ms poll) followed within this long by another on the same channel is
// the same user action: it shares one push and one watch challenge.
#define PRESENCE_SAME_ACTION_GAP_MS 3000
#define PRESENCE_SHORT_WAIT_MS 2000

typedef struct presence_source
{
    const char *name;
    void *ctx;
    void (*arm)(void *ctx);                 // the wait is starting (optional)
    bool (*approved)(void *ctx);            // polled every few ms; true = present (optional)
    void (*disarm)(void *ctx, int result);  // the wait has ended with this result (optional)
} presence_source;

typedef struct presence_io
{
    // Waits up to timeout_ms for one 64-byte report. Returns > 0 if one arrived.
    int (*recv)(uint8_t report[PRESENCE_REPORT_SIZE], uint32_t timeout_ms);
    void (*send)(uint8_t report[PRESENCE_REPORT_SIZE]);
    uint32_t (*now_ms)(void);
} presence_io;

// Ports call this with every report, just before handing it to ctaphid_handle_packet(). A wait
// can only start while the core handles the final packet of a request, so the last report noted
// is the requesting channel's.
void presence_note_report(const uint8_t report[PRESENCE_REPORT_SIZE]);

// The core's usbhid_send() asks this first; false means drop the report (it answers a
// transaction the client aborted with INIT).
bool presence_filter_send(const uint8_t report[PRESENCE_REPORT_SIZE]);

// After ctaphid_handle_packet() returns: true (and the report) if a deferred INIT must now be
// handed to the core.
bool presence_take_deferred(uint8_t report[PRESENCE_REPORT_SIZE]);

// While sources are being armed: true if this wait starts a new user action, false if it repeats
// the last one (see PRESENCE_SAME_ACTION_GAP_MS). A new action: the first wait; one after an
// approval or a cancel; one on a different channel or for a different request; one after a gap of
// 3 s or more; or one after a long wait (a CTAP2 wait that timed out and was retried).
bool presence_is_new_action(void);

// During a wait: SHA-256 of the request being served (all zero if it couldn't be reassembled), and
// its rpId ("" for U2F, or if unknown).
const uint8_t *presence_request_hash(void);
const char *presence_request_rp(void);

// While sources are being disarmed: true if the requesting channel cancelled (CANCEL or INIT).
bool presence_was_cancelled(void);

// Arms every source and waits up to timeout_ms. Returns 1 if a source approved, else 0
// (timed out, or cancelled by the requesting channel).
int presence_wait(uint32_t timeout_ms, const presence_source *const *sources, size_t n_sources,
                  const presence_io *io);
