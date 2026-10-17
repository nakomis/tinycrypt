// The key's BLE setup messages (CRYPT-30), as the iPhone app (and the watch app) send them. Portable,
// so the host tests them.
//
// All JSON, on characteristics only an enrolled watch or phone can use (ble_presence.h):
//   wifi      write  {"ssid": "...", "password": "..."}   join this network (password "" = open)
//             write  {"scan": true}                      look for networks
//             read   the last scan: [["ssid", rssi, secured 0|1], ...], strongest first
//   register  write  {"kind": "watch"|"phone", "token": "<APNs device token, hex>"}
//   info      read   {"thing", "version", "wifi": {"ssid", "state"}, "iot", "watch", "phone"}
//
// Reads are at most SETUP_READ_MAX bytes, the most one ATT attribute can hold.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SETUP_SSID_MAX 32       // bytes, as 802.11 allows
#define SETUP_PASSWORD_MAX 64   // a WPA2 passphrase is 8-63 bytes, or 64 hex digits of PSK
#define SETUP_TOKEN_MIN 32      // hex digits; APNs tokens are 64 today
#define SETUP_TOKEN_MAX 200
#define SETUP_READ_MAX 512
#define SETUP_SCAN_MAX 12       // networks kept from a scan

typedef struct
{
    bool scan;  // a scan request; ssid and password are empty
    char ssid[SETUP_SSID_MAX + 1];
    char password[SETUP_PASSWORD_MAX + 1];
} setup_wifi_t;

typedef enum
{
    SETUP_KIND_WATCH,
    SETUP_KIND_PHONE,
} setup_kind_t;

typedef struct
{
    setup_kind_t kind;
    char token[SETUP_TOKEN_MAX + 1];  // lower-case hex
} setup_register_t;

typedef enum
{
    SETUP_WIFI_UNSET,           // no network configured
    SETUP_WIFI_CONNECTING,
    SETUP_WIFI_CONNECTED,       // has an IP address
    SETUP_WIFI_WRONG_PASSWORD,
    SETUP_WIFI_NOT_FOUND,       // no access point with that name in range
    SETUP_WIFI_FAILED,          // anything else; it keeps retrying
} setup_wifi_state_t;

typedef struct
{
    char ssid[SETUP_SSID_MAX + 1];
    int8_t rssi;
    bool secured;
} setup_network_t;

typedef struct
{
    const char *thing;
    const char *version;
    const char *ssid;
    setup_wifi_state_t wifi;
    bool iot;      // MQTT connected to IoT Core
    bool watch;    // a watch is enrolled
    bool phone;    // a phone is enrolled
} setup_info_t;

// Parse a write. On failure they return false and set *err to a short reason for the log.
bool setup_parse_wifi(const char *json, size_t len, setup_wifi_t *out, const char **err);
bool setup_parse_register(const char *json, size_t len, setup_register_t *out, const char **err);

const char *setup_kind_name(setup_kind_t kind);
const char *setup_wifi_state_name(setup_wifi_state_t state);

// Build a read into out (at most SETUP_READ_MAX bytes, NUL-terminated). Return the length, or -1.
// The scan drops the weakest networks until it fits; `nets` must be strongest first.
int setup_info_json(const setup_info_t *info, char out[SETUP_READ_MAX + 1]);
int setup_scan_json(const setup_network_t *nets, size_t n, char out[SETUP_READ_MAX + 1]);

// Sort a scan strongest first and keep each SSID once (its strongest), hidden ones and ones that
// aren't UTF-8 dropped. Returns the new count.
size_t setup_scan_tidy(setup_network_t *nets, size_t n);

// The IoT payload that relays a registration to the register Lambda (malloc'd, or NULL).
char *setup_register_payload(const setup_register_t *reg);
