// Notifier port (CRYPT-15): tells the watch that the key wants user presence.
//
// Wi-Fi, SNTP and a held-open MQTT connection to AWS IoT Core (mutual TLS with the key's own
// certificate). A presence request is one publish; IoT Core hands it to SNS, which delivers it to
// the watch through APNs. Everything here is best effort: the BOOT button never depends on it.
#pragma once

#include <stdbool.h>

// Starts Wi-Fi, SNTP and MQTT in the background and returns at once. Switches the ADC entropy
// source off first, since the radio replaces it (bootloader_random_disable()). On any setup
// failure it logs and gives up: the key carries on with the button alone.
void notifier_start(void);

// Bracket every ctap_user_presence_test() call. solo1 asks again and again for one user action
// (U2F polls every 750 ms while the browser retries), so a request is only sent when a wait
// starts a new action: the previous wait was approved, or ended more than a few seconds ago.
// Neither call normally blocks: the publish is queued for the MQTT task.
void notifier_presence_wait_started(void);
void notifier_presence_wait_ended(int presence_result);
