// One InfluxDB line-protocol line of integer fields (CRYPT-28), for the key's metrics to the
// house VictoriaMetrics (through vmauth on Cal). Shared by the ESP32 and the host test tool.
//
//   <measurement>,host_name=<host>[,version=<version>] <field>=<n>i[,<field>=<n>i...]\n
//
// Tag values are escaped as line protocol requires (comma, space, equals and backslash take a
// backslash); field names must be plain [a-z0-9_] and are checked. VictoriaMetrics names each
// series <measurement>_<field>. No timestamp: VM stamps the line on receipt.
#ifndef TINYCRYPT_METRICS_LINE_H
#define TINYCRYPT_METRICS_LINE_H

#include <stddef.h>
#include <stdint.h>

typedef struct
{
    const char *name;
    int64_t value;
} metrics_field_t;

// Writes the line (with its newline and a NUL) into out. Returns its length without the NUL, or
// -1 if it doesn't fit, a field name isn't [a-z0-9_]+, a tag value is empty, or there are no fields.
int metrics_line(char *out, size_t cap, const char *measurement, const char *host_name,
                 const char *version, const metrics_field_t *fields, size_t n);

#endif
