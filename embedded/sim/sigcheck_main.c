// tinycrypt-sigcheck: run the key-side presence signature check from the
// command line, so the tests can feed it Secure Enclave vectors (CRYPT-10).
//
//   tinycrypt-sigcheck <pub hex, 64 bytes> <message hex> <sig hex, 64 bytes>
//
// Exits 0 if the signature verifies, 1 if it doesn't, 2 on bad arguments.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "presence_sig.h"

// Strict: sscanf("%2x") would also accept spaces and signs.
static int nibble(char c)
{
    if (!isxdigit((unsigned char)c)) {
        return -1;
    }
    return isdigit((unsigned char)c) ? c - '0' : tolower((unsigned char)c) - 'a' + 10;
}

// Decodes exactly `want` bytes of hex (or any length if want < 0). Returns the
// length, or -1 on malformed input.
static long unhex(const char *s, uint8_t *out, size_t cap, long want)
{
    size_t n = strlen(s);
    if (n % 2 != 0 || n / 2 > cap || (want >= 0 && (long)(n / 2) != want)) {
        return -1;
    }
    for (size_t i = 0; i < n / 2; i++) {
        int hi = nibble(s[2 * i]), lo = nibble(s[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return (long)(n / 2);
}

int main(int argc, char **argv)
{
    static uint8_t msg[4096];
    uint8_t pub[TINYCRYPT_P256_PUB_LEN], sig[TINYCRYPT_P256_SIG_LEN];
    long msg_len;

    if (argc != 4 || unhex(argv[1], pub, sizeof(pub), sizeof(pub)) < 0 ||
        (msg_len = unhex(argv[2], msg, sizeof(msg), -1)) < 0 ||
        unhex(argv[3], sig, sizeof(sig), sizeof(sig)) < 0) {
        fprintf(stderr, "usage: %s <pub hex, 64 bytes> <message hex> <sig hex, 64 bytes>\n", argv[0]);
        return 2;
    }
    int ok = tinycrypt_presence_sig_verify(pub, msg, (size_t)msg_len, sig);
    puts(ok ? "valid" : "invalid");
    return ok ? 0 : 1;
}
