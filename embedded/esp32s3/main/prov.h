// This key's own settings and IoT credentials (CRYPT-14), read from the `prov` NVS partition that
// infra/scripts/provision-key.sh writes. Nothing here is in the firmware image, so every key runs
// the same image and a published update holds no secrets.
//
// The IoT private key is the secure element's (CRYPT-22, se_tls.c); only keys provisioned before
// that still have one here, which the soft-key test build uses until they're reprovisioned.
#pragma once

#include <stdbool.h>

typedef struct
{
    char wifi_ssid[33];
    char wifi_password[65];
    char iot_endpoint[128];
    char thing_name[129];       // also the MQTT client id
    char presence_topic[192];   // tinycrypt/{env}/presence/{thing}/request
    char apns_platform[16];     // APNS_SANDBOX or APNS
    char *device_cert;          // PEM, malloc'd
    char *device_key;           // PEM, malloc'd; NULL unless left over from before CRYPT-22
    // Optional (CRYPT-28): where the key sends its metrics, the house VictoriaMetrics through
    // vmauth. Empty url = no metrics. The password is write-only and assumed public (it sits in
    // plain flash), so it grants nothing but writing metrics.
    char metrics_url[128];      // e.g. http://172.29.0.33:8427
    char metrics_user[33];      // vmauth's user for this key: the Thing name
    char metrics_pass[65];
} prov_t;

// Reads the partition. False (and a log line saying what's missing) if the key isn't provisioned.
bool prov_load(prov_t *prov);
