// tinycrypt Mac simulator: the solo1 CTAP2 core behind a UDP transport.
//
// Each UDP datagram is one 64-byte CTAPHID report. Replies go back to the
// address of the most recent request, so a test client can bind any port.
// State (master secret, sign counter, resident keys) lives in --state-dir.
//
// Port shape follows solo1's pc/ target (Apache-2.0 OR MIT).
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "ctap.h"
#include "presence.h"
#include "ctaphid.h"
#include "device.h"
#include "log.h"
#include "storage.h"
#include "util.h"
#include APP_CONFIG

#define RK_NUM 50

// prompt = the button alone (Enter on stdin); button+watch = the button or a scripted watch
// that approves --watch-after ms into each wait (0: never), whichever comes first.
typedef enum { PRESENCE_AUTO, PRESENCE_DENY, PRESENCE_PROMPT, PRESENCE_BUTTON_WATCH } presence_mode_t;

static int sock = -1;
static struct sockaddr_in peer;
static bool have_peer = false;
static presence_mode_t presence_mode = PRESENCE_AUTO;
static bool up_disabled = false;
static uint32_t watch_after_ms = 0;
static uint32_t up_timeout_ms = 0;  // 0: the core's own timeout
// An external wait (CRYPT-14, standing in for an OTA install approval): starts external_at_ms after
// startup, once the loop is idle, and is cancelled from another thread external_cancel_ms into it.
static uint32_t external_at_ms = 0;
static uint32_t external_cancel_ms = 0;
static bool external_cancel_first = false;  // cancelled before the wait even starts
static char state_path[1024];
static char rk_path[1024];
static char counter_path[1024];
static CTAP_residentKey rk_store[RK_NUM];

static void die(const char *what)
{
    perror(what);
    exit(1);
}

// ---- time -----------------------------------------------------------------

uint32_t millis(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000ULL + tv.tv_usec / 1000);
}

void delay(uint32_t ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

// ---- transport ------------------------------------------------------------

static void transport_init(uint16_t port)
{
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        die("socket");
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        die("bind");
}

// Wait up to timeout_ms for one report. Returns its length, or 0.
static int transport_recv(uint8_t *msg, int timeout_ms)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    if (select(sock + 1, &fds, NULL, NULL, &tv) <= 0)
        return 0;
    socklen_t len = sizeof(peer);
    ssize_t n = recvfrom(sock, msg, HID_MESSAGE_SIZE, 0, (struct sockaddr *)&peer, &len);
    if (n < 0)
        die("recvfrom");
    have_peer = true;
    return (int)n;
}

void usbhid_send(uint8_t *msg)
{
    if (!have_peer || !presence_filter_send(msg))
        return;
    if (sendto(sock, msg, HID_MESSAGE_SIZE, 0, (struct sockaddr *)&peer, sizeof(peer)) < 0)
        perror("sendto");
}

void usbhid_close(void)
{
    if (sock >= 0)
        close(sock);
}

// ---- presence -------------------------------------------------------------

void device_disable_up(bool disable)
{
    up_disabled = disable;
}

// The button: a line on stdin. If stdin closes, nobody can ever press it.
static bool stdin_closed = false;

static bool button_approved(void *ctx)
{
    (void)ctx;
    if (stdin_closed)
        return false;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = {0, 0};
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0)
        return false;
    char line[64];
    if (fgets(line, sizeof(line), stdin) != NULL)
    {
        fprintf(stderr, ">>> button pressed\n");
        return true;
    }
    fprintf(stderr, ">>> stdin closed, the button can't be pressed\n");
    stdin_closed = true;
    return false;
}

// The watch, scripted: approves watch_after_ms into the wait, if the wait is bound to its request
// (presence.h); an unbound wait is button only, as on the device.
static uint32_t watch_armed_at;
static bool watch_armed;

static void watch_arm(void *ctx)
{
    (void)ctx;
    watch_armed_at = millis();
    watch_armed = presence_request_bound();
    fprintf(stderr, ">>> %s action\n", presence_is_new_action() ? "new" : "same");
    const uint8_t *h = presence_request_hash();
    if (presence_request_label()[0])
        fprintf(stderr, ">>> label %s\n", presence_request_label());
    fprintf(stderr, ">>> request %s rp=%s hash=", watch_armed ? "bound" : "unbound", presence_request_rp());
    for (int i = 0; i < PRESENCE_REQUEST_HASH_LEN; i++)
        fprintf(stderr, "%02x", h[i]);
    fprintf(stderr, "\n");
}

static bool watch_approved(void *ctx)
{
    (void)ctx;
    if (!watch_armed || watch_after_ms == 0 || millis() - watch_armed_at < watch_after_ms)
        return false;
    fprintf(stderr, ">>> watch approved\n");
    return true;
}

static void announce(void *ctx)
{
    (void)ctx;
    // A press only counts during a wait: throw away lines typed before it started.
    for (;;)
    {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = {0, 0};
        char line[64];
        if (stdin_closed || select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0 || fgets(line, sizeof(line), stdin) == NULL)
            break;
    }
    fprintf(stderr, "\n>>> User presence requested: press Enter to approve\n");
}

static void report_result(void *ctx, int result)
{
    (void)ctx;
    fprintf(stderr, ">>> presence %s\n", result ? "approved" : "not given");
}

static const presence_source button_source = {"button", NULL, announce, button_approved, report_result};
static const presence_source watch_source = {"watch", NULL, watch_arm, watch_approved, NULL};

static int sim_recv(uint8_t report[PRESENCE_REPORT_SIZE], uint32_t timeout_ms)
{
    memset(report, 0, PRESENCE_REPORT_SIZE);
    return transport_recv(report, (int)timeout_ms);
}

static const presence_io sim_io = {sim_recv, usbhid_send, millis};

// Contract with solo1: 1 = present, 0 = not present, 2 = check disabled by
// the request. NEVER return anything else: solo1's U2F code tests only
// `== 0` / `!ret`, so any other value counts as present.
int ctap_user_presence_test(uint32_t delay_ms)
{
    if (up_disabled)
        return 2;
    uint32_t timeout = up_timeout_ms ? up_timeout_ms : delay_ms;
    switch (presence_mode)
    {
    case PRESENCE_AUTO:
        return 1;
    case PRESENCE_DENY:
        return 0;
    case PRESENCE_PROMPT:
    {
        const presence_source *sources[] = {&button_source};
        return presence_wait(timeout, sources, 1, &sim_io);
    }
    case PRESENCE_BUTTON_WATCH:
    {
        const presence_source *sources[] = {&button_source, &watch_source};
        return presence_wait(timeout, sources, 2, &sim_io);
    }
    }
    return 0;
}

// The external wait: the button or the watch, as for a CTAP request, under the same presence mode.
static void *cancel_later(void *arg)
{
    (void)arg;
    usleep(external_cancel_ms * 1000);
    fprintf(stderr, ">>> cancelling the external wait\n");
    presence_cancel_external();
    return NULL;
}

static void run_external_wait(void)
{
    // SHA-256("tinycrypt-sim-external"): any fixed hash will do.
    static const uint8_t hash[PRESENCE_REQUEST_HASH_LEN] = {
        0x5e, 0x1d, 0x0a, 0x8c, 0x3b, 0x77, 0x42, 0x19, 0xa6, 0x0f, 0xd2, 0x91, 0x4c, 0xe8, 0x23, 0x6b,
        0x90, 0x11, 0x7d, 0xc4, 0x58, 0x2e, 0xb3, 0x06, 0xf9, 0x6a, 0x35, 0xd0, 0x87, 0x4f, 0xe1, 0x2c};
    if (external_cancel_ms)
    {
        pthread_t t;
        pthread_create(&t, NULL, cancel_later, NULL);
        pthread_detach(t);
    }
    if (external_cancel_first)
    {
        fprintf(stderr, ">>> cancelling the external wait before it starts\n");
        presence_cancel_external();
    }
    fprintf(stderr, ">>> external wait\n");
    uint32_t timeout = up_timeout_ms ? up_timeout_ms : 10000;
    int result;
    if (presence_mode == PRESENCE_AUTO || presence_mode == PRESENCE_DENY)
        result = presence_mode == PRESENCE_AUTO;
    else if (presence_mode == PRESENCE_PROMPT)
    {
        const presence_source *sources[] = {&button_source};
        result = presence_wait_external(timeout, hash, "Install tinycrypt 9.9.9?", sources, 1, &sim_io);
    }
    else
    {
        const presence_source *sources[] = {&button_source, &watch_source};
        result = presence_wait_external(timeout, hash, "Install tinycrypt 9.9.9?", sources, 2, &sim_io);
    }
    fprintf(stderr, ">>> external result %d\n", result);
}

// ---- rng, counter, state --------------------------------------------------

int ctap_generate_rng(uint8_t *dst, size_t num)
{
    arc4random_buf(dst, num);
    return 1;
}

static void write_file(const char *path, const void *data, size_t len)
{
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (f == NULL)
        die(tmp);
    if (fwrite(data, 1, len, f) != len)
        die("fwrite");
    fclose(f);
    if (rename(tmp, path) != 0)
        die("rename");
}

static bool read_file(const char *path, void *data, size_t len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return false;
    size_t n = fread(data, 1, len, f);
    fclose(f);
    return n == len;
}

// The default counter lives in RAM; persist it so the sign count never goes
// backwards across restarts (relying parties treat that as a cloned key).
uint32_t ctap_atomic_count(uint32_t amount)
{
    static uint32_t counter = 0;
    static bool loaded = false;
    if (!loaded)
    {
        read_file(counter_path, &counter, sizeof(counter));
        loaded = true;
    }
    counter += amount + 1;
    write_file(counter_path, &counter, sizeof(counter));
    return counter;
}

int authenticator_read_state(AuthenticatorState *state)
{
    if (!read_file(state_path, state, sizeof(*state)))
        return 0;
    return state->is_initialized == INITIALIZED_MARKER;
}

void authenticator_write_state(AuthenticatorState *state)
{
    write_file(state_path, state, sizeof(*state));
}

static void sync_rk(void)
{
    write_file(rk_path, rk_store, sizeof(rk_store));
}

void ctap_reset_rk(void)
{
    memset(rk_store, 0xff, sizeof(rk_store));
    sync_rk();
}

uint32_t ctap_rk_size(void)
{
    return RK_NUM;
}

void ctap_store_rk(int index, CTAP_residentKey *rk)
{
    if (index < 0 || index >= RK_NUM)
        return;
    memmove(&rk_store[index], rk, sizeof(*rk));
    sync_rk();
}

void ctap_delete_rk(int index)
{
    if (index < 0 || index >= RK_NUM)
        return;
    memset(&rk_store[index], 0xff, sizeof(rk_store[index]));
    sync_rk();
}

void ctap_load_rk(int index, CTAP_residentKey *rk)
{
    if (index < 0 || index >= RK_NUM)
        return;
    memmove(rk, &rk_store[index], sizeof(*rk));
}

void ctap_overwrite_rk(int index, CTAP_residentKey *rk)
{
    ctap_store_rk(index, rk);
}

void device_read_aaguid(uint8_t *dst)
{
    memmove(dst, TINYCRYPT_AAGUID, 16);
}

void device_reboot(void)
{
    fprintf(stderr, "reboot requested, exiting\n");
    exit(100);
}

// ---- main -----------------------------------------------------------------

static void on_signal(int sig)
{
    (void)sig;
    usbhid_close();
    _exit(0);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--port N] [--state-dir DIR] [--presence auto|deny|prompt|button+watch]\n"
            "          [--watch-after MS] [--up-timeout MS] [--external-at MS [--external-cancel MS | --external-cancel-first]]\n",
            argv0);
    exit(2);
}

int main(int argc, char *argv[])
{
    uint16_t port = 8111;
    // Holds the master secret: keep it out of wherever the sim is run from.
    const char *state_dir = "sim-state";
    static const struct option opts[] = {
        {"port", required_argument, NULL, 'p'},
        {"state-dir", required_argument, NULL, 's'},
        {"presence", required_argument, NULL, 'u'},
        {"watch-after", required_argument, NULL, 'w'},
        {"up-timeout", required_argument, NULL, 't'},
        {"external-at", required_argument, NULL, 'x'},
        {"external-cancel", required_argument, NULL, 'c'},
        {"external-cancel-first", no_argument, NULL, 'f'},
        {NULL, 0, NULL, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "p:s:u:w:t:x:c:f", opts, NULL)) != -1)
    {
        switch (c)
        {
        case 'p':
            port = (uint16_t)atoi(optarg);
            break;
        case 's':
            state_dir = optarg;
            break;
        case 'u':
            if (strcmp(optarg, "auto") == 0)
                presence_mode = PRESENCE_AUTO;
            else if (strcmp(optarg, "deny") == 0)
                presence_mode = PRESENCE_DENY;
            else if (strcmp(optarg, "prompt") == 0)
                presence_mode = PRESENCE_PROMPT;
            else if (strcmp(optarg, "button+watch") == 0)
                presence_mode = PRESENCE_BUTTON_WATCH;
            else
                usage(argv[0]);
            break;
        case 'w':
            watch_after_ms = (uint32_t)atoi(optarg);
            break;
        case 't':
            up_timeout_ms = (uint32_t)atoi(optarg);
            break;
        case 'x':
            external_at_ms = (uint32_t)atoi(optarg);
            break;
        case 'c':
            external_cancel_ms = (uint32_t)atoi(optarg);
            break;
        case 'f':
            external_cancel_first = true;
            break;
        default:
            usage(argv[0]);
        }
    }

    fputs(TINYCRYPT_SOFT_KEY_BANNER, stderr);

    if (mkdir(state_dir, 0700) != 0 && errno != EEXIST)
        die(state_dir);

    snprintf(state_path, sizeof(state_path), "%s/authenticator_state.bin", state_dir);
    snprintf(rk_path, sizeof(rk_path), "%s/resident_keys.bin", state_dir);
    snprintf(counter_path, sizeof(counter_path), "%s/sign_counter.bin", state_dir);
    if (!read_file(rk_path, rk_store, sizeof(rk_store)))
        ctap_reset_rk();

    set_logging_mask(TAG_ERR | TAG_RED | TAG_GREEN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    transport_init(port);
    ctaphid_init();
    ctap_init();
    fprintf(stderr, "tinycrypt sim listening on udp://127.0.0.1:%u (presence=%s)\n", port,
            presence_mode == PRESENCE_AUTO     ? "auto"
            : presence_mode == PRESENCE_DENY   ? "deny"
            : presence_mode == PRESENCE_PROMPT ? "prompt"
                                               : "button+watch");
    // Readiness line for test harnesses.
    printf("READY %u\n", port);
    fflush(stdout);

    uint8_t msg[HID_MESSAGE_SIZE];
    uint32_t started = millis();
    bool external_done = external_at_ms == 0;
    for (;;)
    {
        if (!external_done && millis() - started >= external_at_ms)
        {
            external_done = true;
            run_external_wait();
        }
        memset(msg, 0, sizeof(msg));
        if (transport_recv(msg, 10) > 0)
        {
            presence_note_report(msg);
            ctaphid_handle_packet(msg);
            while (presence_take_deferred(msg))
            {
                presence_note_report(msg);
                ctaphid_handle_packet(msg);
            }
        }
        ctaphid_check_timeouts();
    }
}
