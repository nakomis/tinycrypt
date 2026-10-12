// tinycrypt-metricsline: run port/metrics_line.c from the command line for the tests (CRYPT-28).
//
//   tinycrypt-metricsline <cap> <measurement> <host_name> <version|-> [name=value ...]
//
// Prints the line exactly (it ends in a newline), or "ERROR\n" when metrics_line refuses.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "metrics_line.h"

int main(int argc, char **argv)
{
    if (argc < 5)
    {
        fprintf(stderr, "usage: %s <cap> <measurement> <host_name> <version|-> [name=value ...]\n", argv[0]);
        return 2;
    }
    size_t cap = (size_t)strtoul(argv[1], NULL, 10);
    metrics_field_t fields[16];
    size_t n = 0;
    for (int i = 5; i < argc && n < 16; i++, n++)
    {
        char *eq = strchr(argv[i], '=');
        if (eq == NULL)
            return 2;
        *eq = 0;
        fields[n].name = argv[i];
        fields[n].value = strtoll(eq + 1, NULL, 10);
    }
    char *out = malloc(cap ? cap : 1);
    int len = metrics_line(out, cap, argv[2], argv[3], strcmp(argv[4], "-") ? argv[4] : NULL, fields, n);
    if (len < 0)
        fputs("ERROR\n", stdout);
    else
        fwrite(out, 1, (size_t)len, stdout);
    free(out);
    return 0;
}
