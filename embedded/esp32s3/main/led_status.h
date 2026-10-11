// Status LED (CRYPT-17): one addressable RGB LED (WS2812-class) tells the user what the key is
// doing. Calm when idle, busy only when it needs you. Always dim: CONFIG_TINYCRYPT_LED_BRIGHTNESS
// percent of full, because these LEDs are blinding at 100 %.
//
//   connecting          dim white, breathing        (until Wi-Fi + MQTT first come up)
//   online, idle        off                         (one green blink when it first connects)
//   offline             short amber blink every 3 s (the button still works)
//   presence wanted     blue, pulsing               ("touch me, or approve on the watch")
//   watch signing       cyan, fast flicker          (a watch read the challenge)
//   approved            green, 1 s                  (one-shot)
//   not given           red, 1 s                    (timed out, cancelled; one-shot)
//   enrol window open   magenta, pulsing; three green flashes when a watch enrols
//   fault               red double blink every 2 s  (e.g. no radio started)
//
// States are flags that can be set together; the highest-priority one shows (presence beats
// offline beats idle). One-shot flashes show over everything, then the state resumes.
#pragma once

#include <stdbool.h>

typedef enum
{
    LED_CONNECTING,
    LED_OFFLINE,
    LED_PRESENCE,
    LED_WATCH_SIGNING,
    LED_ENROL_OPEN,
    LED_FAULT,
    LED_STATE_COUNT,
} led_state_t;

typedef enum
{
    LED_FLASH_ONLINE,
    LED_FLASH_APPROVED,
    LED_FLASH_NOT_GIVEN,
    LED_FLASH_ENROLLED,
} led_flash_t;

// Starts the LED task. Without CONFIG_TINYCRYPT_LED every call here does nothing.
void led_status_start(void);
void led_status_set(led_state_t state, bool on);
void led_status_flash(led_flash_t flash);
