// Notifier port (CRYPT-15): tells the watch that the key wants user presence.
//
// Wi-Fi, SNTP and a held-open MQTT connection to AWS IoT Core (mutual TLS with the key's own
// certificate). A presence request is one publish; IoT Core hands it to SNS, which delivers it to
// the watch through APNs. Everything here is best effort: the BOOT button never depends on it.
#pragma once

#include <stdbool.h>

// Starts Wi-Fi, SNTP and MQTT in the background and returns at once. Switches the ADC entropy
// source off first, since the radio replaces it (bootloader_random_disable()).
void notifier_start(void);

// Publishes a presence request if MQTT is connected. Returns false, without blocking, if not.
bool notifier_presence_request(void);
