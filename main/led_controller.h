#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "esp_err.h"

#define LED_DATA_GPIO 4

typedef enum {
    LED_EFFECT_SOLID = 0,
    LED_EFFECT_SOFT_FADE,
    LED_EFFECT_SOFT_BREATHE,
    LED_EFFECT_DOUBLE_PULSE,
    LED_EFFECT_CHASE,
    LED_EFFECT_COMET,
    LED_EFFECT_SCANNER,
    LED_EFFECT_WAVE,
    LED_EFFECT_WIPE,
    LED_EFFECT_TWINKLE,
    LED_EFFECT_WLED_COLOR_TWINKLES,
    LED_EFFECT_WLED_TWINKLEFOX,
    LED_EFFECT_WLED_CHASE_RAINBOW,
    LED_EFFECT_WLED_THEATER_RAINBOW,
    LED_EFFECT_COUNT,
} led_effect_t;

typedef struct {
    bool preview_active;
    led_effect_t preview_effect;
    int preview_glucose_mgdl;
    uint8_t preview_speed;
    bool preview_reverse;
    bool has_live_glucose;
    int live_glucose_mgdl;
    char live_direction[24];
} led_effect_lab_status_t;

esp_err_t led_controller_init(uint16_t led_count);
void led_controller_set_count(uint16_t led_count);
void led_controller_set_dynamic_trend(bool enabled);
void led_controller_show_config(void);
void led_controller_show_no_data(void);
void led_controller_show_glucose(int glucose_mgdl, int64_t sample_timestamp_ms,
                                 const char *direction, bool reported_stale);

/** Apply a RAM-only effect preview. It is never written to NVS. */
void led_controller_preview_effect(led_effect_t effect, int glucose_mgdl,
                                   uint8_t speed, bool reverse);

/** Stop the RAM-only preview and immediately return to Nightscout automation. */
void led_controller_stop_preview(void);

void led_controller_get_effect_lab_status(led_effect_lab_status_t *status);

/** Start three slow white pulses used to acknowledge a forced reset. */
void led_controller_start_reset_feedback(void);

/** Wait until the reset feedback animation has completed. */
bool led_controller_wait_reset_feedback(uint32_t timeout_ms);
