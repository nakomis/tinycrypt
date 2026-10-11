// Shared user-presence wait (CRYPT-13), used by every port's ctap_user_presence_test().
//
// Presence is the physical button OR the watch, whichever answers first. Each way of getting
// presence is a source: the wait arms them all, polls them, and the first to approve wins.
// Sources that only have side effects (the push notifier) never approve.
//
// While waiting, CTAPHID traffic is handled here and never handed back to the core: solo1 keeps
// the request's channel in a static buffer for the whole wait, and re-entering it (an INIT from
// another client resets that buffer) sends later KEEPALIVEs to CID 0 and lets any channel CANCEL.
//   - KEEPALIVE(UPNEEDED) every 100 ms, on the requesting channel.
//   - CANCEL (or INIT, which resynchronises) on the requesting channel ends the wait: not present.
//   - Anything that starts a request on another channel gets ERROR(CHANNEL_BUSY).
//
// Contract with solo1: the result is only ever 1 (present) or 0 (not present). A cancelled CTAP2
// request therefore reports ACTION_TIMEOUT rather than KEEPALIVE_CANCEL: -1 can't be returned,
// because solo1's U2F and wallet code treat any non-zero result as present.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PRESENCE_REPORT_SIZE 64

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

// Ports call this with every report before passing it to ctaphid_handle_packet(), so the wait
// knows which channel made the request.
void presence_note_report(const uint8_t report[PRESENCE_REPORT_SIZE]);

// True while sources are being armed if this wait starts a new user action, false if it repeats
// the last one. solo1's U2F path re-asks for presence every 750 ms while the browser retries; those
// short, back-to-back waits are one action, so they share one push and one watch challenge. A new
// action: the first wait, one after an approval, one after a gap of 3 s or more, or one after a
// long wait (a CTAP2 wait that timed out, retried by the client).
bool presence_is_new_action(void);

// Arms every source and waits up to timeout_ms. Returns 1 if a source approved, else 0
// (timed out, or cancelled by the requesting channel).
int presence_wait(uint32_t timeout_ms, const presence_source *const *sources, size_t n_sources,
                  const presence_io *io);
