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

// CRYPT-30: setup from the iPhone app, over BLE (ble_presence.c). Safe from any task.
#include "setup_msg.h"
// Keeps the network in NVS (it then wins over prov's) and joins it now. False if Wi-Fi isn't running
// or NVS refused.
bool notifier_set_wifi(const char *ssid, const char *password);
// Starts a scan; notifier_scan_result has it a few seconds later.
bool notifier_scan_wifi(void);
int notifier_scan_result(char out[SETUP_READ_MAX + 1]);
// The network in use and how joining it is going.
setup_wifi_state_t notifier_wifi_info(char ssid[SETUP_SSID_MAX + 1]);
// Keeps an enrolled device's APNs token until IoT Core has it, relaying it to
// tinycrypt/{env}/register/{thing} for the register Lambda (now, or on the next connection).
bool notifier_register(const setup_register_t *reg);
// The key forgot its device of this kind (replaced by a new one, or moved to the other kind): stop
// pushing to it, unless that kind registers again first.
void notifier_deregister(setup_kind_t kind);
