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
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "ctap.h"
#include "ctaphid.h"
#include "device.h"
#include "log.h"
#include "storage.h"
#include "util.h"
#include APP_CONFIG

#define RK_NUM 50

typedef enum { PRESENCE_AUTO, PRESENCE_DENY, PRESENCE_PROMPT } presence_mode_t;

static int sock = -1;
static struct sockaddr_in peer;
static bool have_peer = false;
static presence_mode_t presence_mode = PRESENCE_AUTO;
static bool up_disabled = false;
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
    if (!have_peer)
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

// 1 = present, 0 = not present, 2 = presence check disabled by the request.
int ctap_user_presence_test(uint32_t delay_ms)
{
    if (up_disabled)
        return 2;
    switch (presence_mode)
    {
    case PRESENCE_AUTO:
        return 1;
    case PRESENCE_DENY:
        return 0;
    case PRESENCE_PROMPT:
    {
        fprintf(stderr, "\n>>> User presence requested: press Enter within %u s to approve\n",
                (unsigned)(delay_ms / 1000));
        uint32_t start = millis();
        while (millis() - start < delay_ms)
        {
            ctaphid_update_status(CTAPHID_STATUS_UPNEEDED);
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            struct timeval tv = {0, 100 * 1000};
            if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0)
            {
                char line[64];
                if (fgets(line, sizeof(line), stdin) != NULL)
                    return 1;
            }
        }
        fprintf(stderr, ">>> Timed out\n");
        return 0;
    }
    }
    return 0;
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
            "usage: %s [--port N] [--state-dir DIR] [--presence auto|deny|prompt]\n",
            argv0);
    exit(2);
}

int main(int argc, char *argv[])
{
    uint16_t port = 8111;
    const char *state_dir = ".";
    static const struct option opts[] = {
        {"port", required_argument, NULL, 'p'},
        {"state-dir", required_argument, NULL, 's'},
        {"presence", required_argument, NULL, 'u'},
        {NULL, 0, NULL, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "p:s:u:", opts, NULL)) != -1)
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
            else
                usage(argv[0]);
            break;
        default:
            usage(argv[0]);
        }
    }

    fputs(TINYCRYPT_SOFT_KEY_BANNER, stderr);

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
            presence_mode == PRESENCE_AUTO ? "auto" : presence_mode == PRESENCE_DENY ? "deny" : "prompt");
    // Readiness line for test harnesses.
    printf("READY %u\n", port);
    fflush(stdout);

    uint8_t msg[HID_MESSAGE_SIZE];
    for (;;)
    {
        memset(msg, 0, sizeof(msg));
        if (transport_recv(msg, 10) > 0)
            ctaphid_handle_packet(msg);
        ctaphid_check_timeouts();
    }
}
