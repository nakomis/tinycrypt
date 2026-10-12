// Notifier port (CRYPT-15): tells the watch that the key wants user presence.
//
// Wi-Fi, SNTP and a held-open MQTT connection to AWS IoT Core (mutual TLS with the key's own
// certificate). A presence request is one publish; IoT Core hands it to SNS, which delivers it to
// the watch through APNs. Everything here is best effort: the BOOT button never depends on it.
#pragma once

#include <stdbool.h>

// Starts Wi-Fi, SNTP and MQTT in the background and returns at once. Switches the ADC entropy
// source off first, since the radio replaces it (bootloader_random_disable()). On any setup
// failure it logs and gives up: the key carries on with the button alone. Returns true once Wi-Fi
// (and so the radio's entropy) has started.
bool notifier_start(void);

// Call as each presence wait is armed. Sends a request only when the wait starts a new user action
// (presence_is_new_action()). Doesn't normally block: the publish is queued for the MQTT task.
void notifier_presence_wait_started(void);

// Call as each presence wait ends: drops a request still held for a connection.
void notifier_presence_wait_ended(void);

// Publishes a Fleet response (JSON) on tinycrypt/<thing>/responses at QoS 1. Queued for the MQTT
// task; false (and dropped) if not connected.
bool notifier_respond(const char *json);

// This key's Thing name ("" until notifier_start has read the prov partition).
const char *notifier_thing_name(void);

// The provisioned settings notifier_start read (NULL before that, or if the key isn't provisioned).
#include "prov.h"
const prov_t *notifier_prov(void);

// Whether the MQTT connection to IoT Core is up (CRYPT-28's metrics).
bool notifier_mqtt_connected(void);
