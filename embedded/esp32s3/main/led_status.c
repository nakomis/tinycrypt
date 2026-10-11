// Status LED (CRYPT-17). See led_status.h.
#include "led_status.h"

#include "sdkconfig.h"

#ifndef CONFIG_TINYCRYPT_LED

void led_status_start(void) {}
void led_status_set(led_state_t state, bool on) { (void)state; (void)on; }
void led_status_flash(led_flash_t flash) { (void)flash; }

#else

#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "led";

#define FRAME_MS 20

typedef struct
{
    uint8_t r, g, b;
} rgb_t;

static const rgb_t OFF = {0, 0, 0};
static const rgb_t WHITE = {255, 255, 255};
static const rgb_t AMBER = {255, 110, 0};
static const rgb_t BLUE = {0, 40, 255};
static const rgb_t GREEN = {0, 255, 30};
static const rgb_t RED = {255, 0, 0};
static const rgb_t MAGENTA = {255, 0, 200};

static led_strip_handle_t strip;
static volatile uint32_t states;  // bit per led_state_t
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static led_flash_t flash_kind;
static int64_t flash_started_us = -1;

void led_status_set(led_state_t state, bool on)
{
    taskENTER_CRITICAL(&lock);
    if (on)
        states |= 1u << state;
    else
        states &= ~(1u << state);
    taskEXIT_CRITICAL(&lock);
}

void led_status_flash(led_flash_t flash)
{
    taskENTER_CRITICAL(&lock);
    flash_kind = flash;
    flash_started_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&lock);
}

// 0..255 triangle wave with the given period.
static uint32_t pulse(int64_t ms, uint32_t period_ms)
{
    uint32_t phase = (uint32_t)(ms % period_ms) * 510 / period_ms;
    return phase < 255 ? phase : 510 - phase;
}

static rgb_t scale(rgb_t c, uint32_t level)
{
    return (rgb_t){c.r * level / 255, c.g * level / 255, c.b * level / 255};
}

// What to show at time `ms`, given the states and any flash in progress.
static rgb_t frame(int64_t ms, uint32_t on, bool flashing, led_flash_t kind, int64_t flash_ms)
{
    if (flashing)
    {
        switch (kind)
        {
        case LED_FLASH_ONLINE:
            return flash_ms < 300 ? GREEN : OFF;
        case LED_FLASH_APPROVED:
            return GREEN;
        case LED_FLASH_NOT_GIVEN:
            return RED;
        case LED_FLASH_ENROLLED:
            return (flash_ms / 200) % 2 == 0 ? GREEN : OFF;  // three flashes in 1.2 s
        }
    }
    if (on & (1u << LED_WATCH_SIGNING))
        return (ms / 60) % 2 ? WHITE : OFF;  // fast flicker: blue and cyan look alike when dim
    if (on & (1u << LED_PRESENCE))
        return scale(BLUE, 40 + pulse(ms, 1200) * 215 / 255);
    if (on & (1u << LED_ENROL_OPEN))
        return scale(MAGENTA, 40 + pulse(ms, 1600) * 215 / 255);
    if (on & (1u << LED_FAULT))
    {
        int64_t t = ms % 2000;
        return (t < 120 || (t >= 240 && t < 360)) ? RED : OFF;
    }
    if (on & (1u << LED_CONNECTING))
        return scale(WHITE, 20 + pulse(ms, 2000) * 100 / 255);
    if (on & (1u << LED_OFFLINE))
        return ms % 3000 < 120 ? AMBER : OFF;
    return OFF;
}

static int64_t flash_length_ms(led_flash_t kind)
{
    return kind == LED_FLASH_ONLINE ? 300 : kind == LED_FLASH_ENROLLED ? 1200 : 1000;
}

static void led_task(void *arg)
{
    (void)arg;
    rgb_t last = {1, 1, 1};
    for (;;)
    {
        int64_t now_us = esp_timer_get_time();
        taskENTER_CRITICAL(&lock);
        uint32_t on = states;
        led_flash_t kind = flash_kind;
        int64_t started = flash_started_us;
        taskEXIT_CRITICAL(&lock);
        int64_t flash_ms = started < 0 ? 0 : (now_us - started) / 1000;
        bool flashing = started >= 0 && flash_ms < flash_length_ms(kind);

        rgb_t c = scale(frame(now_us / 1000, on, flashing, kind, flash_ms),
                        CONFIG_TINYCRYPT_LED_BRIGHTNESS * 255 / 100);
        if (c.r != last.r || c.g != last.g || c.b != last.b)
        {
            led_strip_set_pixel(strip, 0, c.r, c.g, c.b);
            led_strip_refresh(strip);
            last = c;
        }
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
    }
}

void led_status_start(void)
{
    const led_strip_config_t config = {
        .strip_gpio_num = CONFIG_TINYCRYPT_LED_GPIO,
        .max_leds = 1,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model = LED_MODEL_WS2812,
    };
    const led_strip_rmt_config_t rmt = {.resolution_hz = 10 * 1000 * 1000};
    if (led_strip_new_rmt_device(&config, &rmt, &strip) != ESP_OK)
    {
        ESP_LOGW(TAG, "no status LED on GPIO%d", CONFIG_TINYCRYPT_LED_GPIO);
        return;
    }
    led_strip_clear(strip);
    xTaskCreate(led_task, "led", 3072, NULL, 2, NULL);
    ESP_LOGI(TAG, "status LED on GPIO%d at %d%%", CONFIG_TINYCRYPT_LED_GPIO, CONFIG_TINYCRYPT_LED_BRIGHTNESS);
}

#endif
