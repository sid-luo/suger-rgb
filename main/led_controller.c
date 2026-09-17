#include "led_controller.h"

#include <string.h>
#include <strings.h>

#include "app_config.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define RMT_RESOLUTION_HZ        10000000
#define RMT_TX_TIMEOUT_MS        100
#define RMT_RECOVERY_DELAY_MS    10
#define DYNAMIC_REFRESH_MS       23
#define EFFECT_FRAME_MS          40
#define STALE_AFTER_US           (10LL * 60LL * 1000000LL)
#define RESET_PULSE_COUNT        3
#define RESET_PULSE_MS           1400
#define CONFIG_CHASE_WHITE       80
#define COLOR_EFFECT_FLOOR       5U
#define COLOR_EFFECT_PEAK        38U

// Approximate dynamic LED-current ceiling. A WS2812B color channel can draw
// about 20 mA at value 255. This is intentionally fixed and not user-facing.
// It leaves more USB-power headroom for the ESP32-C3 and Wi-Fi current peaks.
#define LED_OUTPUT_BUDGET_MA     250U
#define WS2812_CHANNEL_MA        20U
#define MAX_CHANNEL_BUDGET       (LED_OUTPUT_BUDGET_MA * 255U / WS2812_CHANNEL_MA)

typedef enum {
    DISPLAY_NO_DATA,
    DISPLAY_GLUCOSE,
    DISPLAY_CONFIG,
    DISPLAY_RESET_FEEDBACK,
} display_mode_t;

typedef struct {
    display_mode_t mode;
    uint16_t led_count;
    bool dynamic_trend_enabled;
    int glucose_mgdl;
    char direction[24];
    bool has_live_glucose;
    int64_t sample_timestamp_ms;
    int64_t sample_observed_us;
    bool sample_reported_stale;
    int64_t reset_started_us;

    // Development effect-lab override. These fields are RAM-only and are
    // deliberately absent from app_config/NVS.
    bool preview_active;
    led_effect_t preview_effect;
    int preview_glucose_mgdl;
    uint8_t preview_speed;
    bool preview_reverse;
} display_state_t;

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} rgb_t;

typedef struct {
    led_effect_t effect;
    uint8_t speed;
    bool reverse;
} effect_selection_t;

static const char *TAG = "led";
static uint8_t s_pixels[APP_MAX_LED_COUNT * 3];
static rgb_t s_color_twinkles[APP_MAX_LED_COUNT];
static uint64_t s_color_twinkles_fade_up;
static bool s_color_twinkles_initialized;
static uint32_t s_effect_rng = 0x5A17C3E9U;
static rmt_channel_handle_t s_led_channel;
static rmt_encoder_handle_t s_led_encoder;
static SemaphoreHandle_t s_reset_done;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static display_state_t s_state = {
    .mode = DISPLAY_NO_DATA,
    .led_count = APP_DEFAULT_LED_COUNT,
    .dynamic_trend_enabled = true,
    .preview_effect = LED_EFFECT_SOLID,
    .preview_glucose_mgdl = 100,
    .preview_speed = 50,
    .direction = "Unknown",
};

static const rmt_symbol_word_t WS2812_ZERO = {
    .level0 = 1,
    .duration0 = 3,
    .level1 = 0,
    .duration1 = 9,
};

static const rmt_symbol_word_t WS2812_ONE = {
    .level0 = 1,
    .duration0 = 9,
    .level1 = 0,
    .duration1 = 3,
};

static const rmt_symbol_word_t WS2812_RESET = {
    .level0 = 0,
    .duration0 = 250,
    .level1 = 0,
    .duration1 = 250,
};

static size_t ws2812_encoder_callback(const void *data, size_t data_size,
                                      size_t symbols_written, size_t symbols_free,
                                      rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    (void)arg;
    if (symbols_free < 8) {
        return 0;
    }

    size_t data_pos = symbols_written / 8;
    const uint8_t *bytes = data;
    if (data_pos < data_size) {
        size_t symbol_pos = 0;
        for (uint8_t mask = 0x80; mask != 0; mask >>= 1) {
            symbols[symbol_pos++] = (bytes[data_pos] & mask) ? WS2812_ONE : WS2812_ZERO;
        }
        return symbol_pos;
    }

    symbols[0] = WS2812_RESET;
    *done = true;
    return 1;
}

static void set_pixel(uint16_t index, rgb_t color, uint8_t scale)
{
    if (index >= APP_MAX_LED_COUNT) {
        return;
    }
    // WS2812B byte order is GRB.
    s_pixels[index * 3 + 0] = (uint16_t)color.green * scale / 255;
    s_pixels[index * 3 + 1] = (uint16_t)color.red * scale / 255;
    s_pixels[index * 3 + 2] = (uint16_t)color.blue * scale / 255;
}

static uint8_t saturating_add8(uint8_t left, uint8_t right)
{
    uint16_t sum = (uint16_t)left + right;
    return sum > 255U ? 255U : (uint8_t)sum;
}

static uint8_t scale8_video(uint8_t value, uint8_t scale)
{
    if (value == 0 || scale == 0) {
        return 0;
    }
    return ((uint16_t)value * scale >> 8) + 1U;
}

static uint32_t effect_random32(void)
{
    // Small deterministic xorshift generator. The LED task is its only caller.
    uint32_t value = s_effect_rng;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    s_effect_rng = value;
    return value;
}

static uint8_t effect_random8(void)
{
    return (uint8_t)(effect_random32() >> 24);
}

static uint16_t effect_random_pixel(uint16_t led_count)
{
    return ((uint64_t)effect_random32() * led_count) >> 32;
}

static rgb_t rainbow_wheel(uint8_t position)
{
    position = 255U - position;
    if (position < 85U) {
        return (rgb_t){
            .red = 255U - position * 3U,
            .green = 0,
            .blue = position * 3U,
        };
    }
    if (position < 170U) {
        position -= 85U;
        return (rgb_t){
            .red = 0,
            .green = position * 3U,
            .blue = 255U - position * 3U,
        };
    }
    position -= 170U;
    return (rgb_t){
        .red = position * 3U,
        .green = 255U - position * 3U,
        .blue = 0,
    };
}

static void apply_fixed_power_budget(uint16_t led_count)
{
    uint32_t sum = 0;
    const size_t byte_count = (size_t)led_count * 3;
    for (size_t i = 0; i < byte_count; ++i) {
        sum += s_pixels[i];
    }
    if (sum <= MAX_CHANNEL_BUDGET || sum == 0) {
        return;
    }

    for (size_t i = 0; i < byte_count; ++i) {
        s_pixels[i] = (uint32_t)s_pixels[i] * MAX_CHANNEL_BUDGET / sum;
    }
}

static esp_err_t transmit_pixels_once(uint16_t led_count)
{
    rmt_transmit_config_t tx_config = {.loop_count = 0};
    esp_err_t err = rmt_transmit(s_led_channel, s_led_encoder, s_pixels,
                                 (size_t)led_count * 3, &tx_config);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(s_led_channel, RMT_TX_TIMEOUT_MS);
    }
    return err;
}

static esp_err_t recover_led_channel(void)
{
    esp_err_t err = rmt_disable(s_led_channel);
    if (err != ESP_OK) {
        return err;
    }

    // rmt_disable() terminates and recycles an interrupted transaction. Flush
    // its completed descriptor before enabling the channel again.
    err = rmt_tx_wait_all_done(s_led_channel, 0);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(RMT_RECOVERY_DELAY_MS));
    return rmt_enable(s_led_channel);
}

static esp_err_t transmit_pixels(uint16_t led_count)
{
    esp_err_t err = transmit_pixels_once(led_count);
    if (err != ESP_ERR_TIMEOUT && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    ESP_LOGW(TAG, "LED channel stalled during startup; recovering");
    esp_err_t recovery_err = recover_led_channel();
    if (recovery_err != ESP_OK) {
        ESP_LOGE(TAG, "LED channel recovery failed: %s",
                 esp_err_to_name(recovery_err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(RMT_RECOVERY_DELAY_MS));
    return transmit_pixels_once(led_count);
}

static rgb_t glucose_color(int glucose_mgdl)
{
    if (glucose_mgdl < 63) {
        return (rgb_t){.red = 150, .green = 0, .blue = 220};
    }
    if (glucose_mgdl < 80) {
        return (rgb_t){.red = 0, .green = 35, .blue = 255};
    }
    if (glucose_mgdl < 153) {
        return (rgb_t){.red = 0, .green = 255, .blue = 0};
    }
    if (glucose_mgdl < 180) {
        return (rgb_t){.red = 255, .green = 75, .blue = 0};
    }
    return (rgb_t){.red = 255, .green = 0, .blue = 0};
}

static bool sample_is_stale(const display_state_t *state)
{
    if (state->sample_reported_stale || state->sample_observed_us <= 0) {
        return true;
    }
    int64_t elapsed_us = esp_timer_get_time() - state->sample_observed_us;
    return elapsed_us < 0 || elapsed_us >= STALE_AFTER_US;
}

static uint8_t clamp_speed(uint8_t speed)
{
    if (speed < 1) {
        return 1;
    }
    if (speed > 100) {
        return 100;
    }
    return speed;
}

static uint32_t effect_period_ms(uint8_t speed, uint32_t slow_ms, uint32_t fast_ms)
{
    speed = clamp_speed(speed);
    return slow_ms - (uint64_t)(slow_ms - fast_ms) * (speed - 1U) / 99U;
}

static uint16_t effect_phase(uint32_t now_ms, uint8_t speed,
                             uint32_t slow_ms, uint32_t fast_ms, bool reverse)
{
    uint32_t period_ms = effect_period_ms(speed, slow_ms, fast_ms);
    uint16_t phase = (uint32_t)(now_ms % period_ms) * 65536U / period_ms;
    return reverse ? (uint16_t)(0U - phase) : phase;
}

static uint8_t triangle8(uint16_t phase)
{
    if (phase & 0x8000U) {
        phase = 0xFFFFU - phase;
    }
    return (uint8_t)(phase >> 7);
}

static uint8_t smoothstep8(uint8_t value)
{
    uint32_t x = value;
    return (uint8_t)((x * x * (765U - 2U * x) + 32512U) / 65025U);
}

static void render_solid(uint16_t led_count, rgb_t color)
{
    for (uint16_t i = 0; i < led_count; ++i) {
        set_pixel(i, color, 255);
    }
}

static uint16_t wled_triwave16(uint16_t value)
{
    if (value & 0x8000U) {
        value = 0xFFFFU - value;
    }
    return value << 1;
}

static void render_soft_fade(uint16_t led_count, rgb_t color, uint32_t now_ms,
                             uint8_t speed)
{
    // The triangular envelope is adapted from WLED v0.14.4's MIT-licensed
    // Fade effect. See THIRD_PARTY_NOTICES.md. It is deliberately mapped to a
    // low, non-zero range so the lamp never disappears at the dark end.
    uint16_t phase = effect_phase(now_ms, speed, 6000, 1200, false);
    uint8_t luminance = (uint8_t)(wled_triwave16(phase) >> 8);
    uint8_t level = COLOR_EFFECT_FLOOR +
                    (uint32_t)luminance * (COLOR_EFFECT_PEAK - COLOR_EFFECT_FLOOR) / 255U;

    for (uint16_t i = 0; i < led_count; ++i) {
        set_pixel(i, color, level);
    }
}

static void render_soft_breathe(uint16_t led_count, rgb_t color, uint32_t now_ms,
                                uint8_t speed)
{
    uint16_t phase = effect_phase(now_ms, speed, 7000, 1400, false);
    uint8_t eased = smoothstep8(triangle8(phase));
    uint8_t level = COLOR_EFFECT_FLOOR +
                    (uint32_t)eased * (COLOR_EFFECT_PEAK - COLOR_EFFECT_FLOOR) / 255U;
    for (uint16_t i = 0; i < led_count; ++i) {
        set_pixel(i, color, level);
    }
}

static uint8_t pulse_envelope(uint16_t position, uint16_t start, uint16_t end)
{
    if (position < start || position >= end || end <= start + 1U) {
        return 0;
    }
    uint16_t local = (uint32_t)(position - start) * 65535U / (end - start - 1U);
    return smoothstep8(triangle8(local));
}

static void render_double_pulse(uint16_t led_count, rgb_t color, uint32_t now_ms,
                                uint8_t speed)
{
    uint32_t period_ms = effect_period_ms(speed, 4200, 1100);
    uint16_t position = (uint32_t)(now_ms % period_ms) * 1000U / period_ms;
    uint8_t first = pulse_envelope(position, 0, 235);
    uint8_t second = pulse_envelope(position, 285, 535);
    uint8_t peak = first > second ? first : second;
    uint8_t level = 6U + (uint32_t)peak * 42U / 255U;
    for (uint16_t i = 0; i < led_count; ++i) {
        set_pixel(i, color, level);
    }
}

static uint16_t wrapped_pixel(int32_t pixel, uint16_t led_count)
{
    while (pixel < 0) {
        pixel += led_count;
    }
    while (pixel >= led_count) {
        pixel -= led_count;
    }
    return (uint16_t)pixel;
}

static void render_chase(uint16_t led_count, rgb_t color, uint32_t now_ms,
                         uint8_t speed, bool reverse)
{
    memset(s_pixels, 0, (size_t)led_count * 3);
    uint16_t phase = effect_phase(now_ms, speed, 12000, 1200, reverse);
    uint16_t head = ((uint32_t)phase * led_count) >> 16;
    static const uint8_t tail_scale[] = {112, 58, 28, 12, 5};
    size_t tail_length = led_count < sizeof(tail_scale) ? led_count : sizeof(tail_scale);
    for (size_t tail = 0; tail < tail_length; ++tail) {
        int32_t pixel = reverse ? (int32_t)head + tail : (int32_t)head - tail;
        set_pixel(wrapped_pixel(pixel, led_count), color, tail_scale[tail]);
    }
}

static void render_comet(uint16_t led_count, rgb_t color, uint32_t now_ms,
                         uint8_t speed, bool reverse)
{
    memset(s_pixels, 0, (size_t)led_count * 3);
    uint16_t phase = effect_phase(now_ms, speed, 10000, 1000, reverse);
    uint16_t head = ((uint32_t)phase * led_count) >> 16;
    uint16_t tail_length = led_count / 4U;
    if (tail_length < 6U) {
        tail_length = led_count < 6U ? led_count : 6U;
    }
    if (tail_length > 18U) {
        tail_length = 18U;
    }

    for (uint16_t tail = 0; tail < tail_length; ++tail) {
        uint32_t remaining = tail_length - tail;
        uint8_t level = 3U + remaining * remaining * 123U /
                               ((uint32_t)tail_length * tail_length);
        int32_t pixel = reverse ? (int32_t)head + tail : (int32_t)head - tail;
        set_pixel(wrapped_pixel(pixel, led_count), color, level);
    }
}

static void render_scanner(uint16_t led_count, rgb_t color, uint32_t now_ms,
                           uint8_t speed, bool reverse)
{
    for (uint16_t i = 0; i < led_count; ++i) {
        set_pixel(i, color, 2);
    }
    if (led_count == 1U) {
        set_pixel(0, color, 100);
        return;
    }

    uint16_t phase = effect_phase(now_ms, speed, 9000, 1200, false);
    uint32_t travel = (uint32_t)(led_count - 1U) * 2U;
    uint32_t step = ((uint32_t)phase * travel) >> 16;
    uint16_t head = step < led_count ? step : travel - step;
    if (reverse) {
        head = led_count - 1U - head;
    }

    static const uint8_t levels[] = {108, 48, 18, 7};
    for (size_t distance = 0; distance < sizeof(levels); ++distance) {
        if (head >= distance) {
            set_pixel(head - distance, color, levels[distance]);
        }
        if (distance > 0 && head + distance < led_count) {
            set_pixel(head + distance, color, levels[distance]);
        }
    }
}

static void render_wave(uint16_t led_count, rgb_t color, uint32_t now_ms,
                        uint8_t speed, bool reverse)
{
    uint16_t phase = effect_phase(now_ms, speed, 10000, 1000, reverse);
    for (uint16_t i = 0; i < led_count; ++i) {
        // Two broad waves travel through the pixel index order.
        uint16_t local = phase + (uint32_t)i * 131072U / led_count;
        uint8_t wave = smoothstep8(triangle8(local));
        uint8_t level = 4U + (uint32_t)wave * 34U / 255U;
        set_pixel(i, color, level);
    }
}

static void render_wipe(uint16_t led_count, rgb_t color, uint32_t now_ms,
                        uint8_t speed, bool reverse)
{
    uint16_t phase = effect_phase(now_ms, speed, 10000, 1600, false);
    uint32_t edge = (uint32_t)triangle8(phase) * (led_count * 256U + 255U) / 255U;
    for (uint16_t i = 0; i < led_count; ++i) {
        uint16_t position = reverse ? led_count - 1U - i : i;
        uint32_t pixel_start = (uint32_t)position * 256U;
        uint8_t level = 4;
        if (edge >= pixel_start + 256U) {
            level = 36;
        } else if (edge > pixel_start) {
            level = 4U + (edge - pixel_start) * 32U / 256U;
        }
        set_pixel(i, color, level);
    }
}

static uint16_t pixel_phase_offset(uint16_t index)
{
    uint32_t value = (uint32_t)(index + 1U) * 0x9E3779B1U;
    value ^= value >> 16;
    value *= 0x85EBCA6BU;
    return (uint16_t)(value >> 16);
}

static void render_twinkle(uint16_t led_count, rgb_t color, uint32_t now_ms,
                           uint8_t speed, bool reverse)
{
    uint16_t phase = effect_phase(now_ms, speed, 7000, 1100, reverse);
    for (uint16_t i = 0; i < led_count; ++i) {
        uint8_t wave = smoothstep8(triangle8(phase + pixel_phase_offset(i)));
        uint8_t level = 3;
        if (wave > 150U) {
            uint8_t sparkle = (uint32_t)(wave - 150U) * 255U / 105U;
            level += (uint32_t)sparkle * 39U / 255U;
        }
        set_pixel(i, color, level);
    }
}

static void initialize_color_twinkles(uint16_t led_count)
{
    memset(s_color_twinkles, 0, sizeof(s_color_twinkles));
    s_color_twinkles_fade_up = 0;
    uint16_t seeds = led_count / 4U;
    if (seeds < 4U) {
        seeds = led_count < 4U ? led_count : 4U;
    }
    for (uint16_t seed = 0; seed < seeds; ++seed) {
        uint16_t pixel = effect_random_pixel(led_count);
        rgb_t color = rainbow_wheel(effect_random8());
        s_color_twinkles[pixel] = (rgb_t){
            .red = (uint16_t)color.red * 64U / 255U,
            .green = (uint16_t)color.green * 64U / 255U,
            .blue = (uint16_t)color.blue * 64U / 255U,
        };
        s_color_twinkles_fade_up |= 1ULL << pixel;
    }
    s_color_twinkles_initialized = true;
}

static void render_wled_color_twinkles(uint16_t led_count, uint8_t speed)
{
    // Adapted from WLED v0.14.4 mode_colortwinkle. Each pixel owns a color and
    // independently grows to full brightness before fading back to black.
    if (!s_color_twinkles_initialized) {
        initialize_color_twinkles(led_count);
    }

    uint8_t wled_speed = (uint16_t)clamp_speed(speed) * 255U / 100U;
    uint8_t fade_up_amount = 8U + (wled_speed >> 2);
    uint8_t fade_down_amount = 8U + (wled_speed >> 3);

    for (uint16_t i = 0; i < led_count; ++i) {
        rgb_t *pixel = &s_color_twinkles[i];
        bool fade_up = (s_color_twinkles_fade_up & (1ULL << i)) != 0;
        if (fade_up) {
            pixel->red = saturating_add8(pixel->red,
                                         scale8_video(pixel->red, fade_up_amount));
            pixel->green = saturating_add8(pixel->green,
                                           scale8_video(pixel->green, fade_up_amount));
            pixel->blue = saturating_add8(pixel->blue,
                                          scale8_video(pixel->blue, fade_up_amount));
            if (pixel->red == 255U || pixel->green == 255U || pixel->blue == 255U) {
                s_color_twinkles_fade_up &= ~(1ULL << i);
            }
        } else {
            uint8_t scale = 255U - fade_down_amount;
            pixel->red = (uint16_t)pixel->red * scale >> 8;
            pixel->green = (uint16_t)pixel->green * scale >> 8;
            pixel->blue = (uint16_t)pixel->blue * scale >> 8;
            if ((uint16_t)pixel->red + pixel->green + pixel->blue <= 3U) {
                *pixel = (rgb_t){0};
            }
        }
        set_pixel(i, *pixel, 255);
    }

    // WLED spawns at most one new point per pass. Two passes on a 64-LED
    // installation make this deliberately dense and conspicuous without
    // resorting to a full-strip strobe.
    uint8_t spawn_chance = 100U + clamp_speed(speed);
    uint16_t passes = 1U + led_count / 50U;
    for (uint16_t pass = 0; pass < passes; ++pass) {
        if (effect_random8() > spawn_chance) {
            continue;
        }
        for (uint8_t attempt = 0; attempt < 5U; ++attempt) {
            uint16_t i = effect_random_pixel(led_count);
            rgb_t *pixel = &s_color_twinkles[i];
            if ((uint16_t)pixel->red + pixel->green + pixel->blue > 3U) {
                continue;
            }
            rgb_t color = rainbow_wheel(effect_random8());
            *pixel = (rgb_t){
                .red = (uint16_t)color.red * 64U / 255U,
                .green = (uint16_t)color.green * 64U / 255U,
                .blue = (uint16_t)color.blue * 64U / 255U,
            };
            s_color_twinkles_fade_up |= 1ULL << i;
            break;
        }
    }
}

static uint8_t sin8_approx(uint8_t phase)
{
    uint8_t shifted = phase + 64U;
    return smoothstep8(triangle8((uint16_t)shifted << 8));
}

static rgb_t wled_twinklefox_one(uint32_t now_ms, uint8_t salt, uint8_t speed)
{
    uint8_t wled_speed = (uint16_t)clamp_speed(speed) * 255U / 100U;
    uint16_t divisor = wled_speed > 100U
                           ? 3U + ((255U - wled_speed) >> 3)
                           : 22U + ((100U - wled_speed) >> 1);
    uint16_t ticks = now_ms / divisor;
    uint8_t fast_cycle = ticks;
    uint16_t slow_cycle = (ticks >> 8) + salt;
    slow_cycle += sin8_approx((uint8_t)slow_cycle);
    slow_cycle = slow_cycle * 2053U + 1384U;
    uint8_t slow_cycle8 = (slow_cycle & 0xFFU) + (slow_cycle >> 8);

    // Six out of eight density slots keeps the original holiday-light motion
    // but makes this alert candidate intentionally busy.
    uint8_t brightness = 0;
    if (((slow_cycle8 & 0x0EU) / 2U) < 6U) {
        uint8_t phase = fast_cycle;
        if (phase < 86U) {
            brightness = phase * 3U;
        } else {
            phase -= 86U;
            brightness = 255U - (phase + phase / 2U);
        }
    }

    rgb_t color = rainbow_wheel(slow_cycle8 - salt);
    color.red = (uint16_t)color.red * brightness / 255U;
    color.green = (uint16_t)color.green * brightness / 255U;
    color.blue = (uint16_t)color.blue * brightness / 255U;
    if (fast_cycle >= 128U) {
        uint8_t cooling = (fast_cycle - 128U) >> 4;
        color.green = color.green > cooling ? color.green - cooling : 0;
        uint8_t blue_cooling = cooling * 2U;
        color.blue = color.blue > blue_cooling ? color.blue - blue_cooling : 0;
    }
    return color;
}

static void render_wled_twinklefox(uint16_t led_count, uint32_t now_ms,
                                   uint8_t speed, bool reverse)
{
    // Deterministic per-pixel clock offsets follow WLED v0.14.4 Twinklefox,
    // itself based on Mark Kriegsman's TwinkleFOX.
    uint16_t prng = 11337U;
    for (uint16_t i = 0; i < led_count; ++i) {
        prng = (uint16_t)(prng * 2053U) + 1384U;
        uint16_t clock_offset = prng;
        prng = (uint16_t)(prng * 2053U) + 1384U;
        uint8_t multiplier = ((((prng & 0xFFU) >> 4) + (prng & 0x0FU)) & 0x0FU) + 8U;
        uint32_t pixel_clock = ((uint64_t)now_ms * multiplier >> 3) + clock_offset;
        rgb_t color = wled_twinklefox_one(pixel_clock, prng >> 8, speed);
        uint16_t target = reverse ? led_count - 1U - i : i;
        set_pixel(target, color, 255);
    }
}

static void render_wled_chase_rainbow(uint16_t led_count, uint32_t now_ms,
                                      uint8_t speed, bool reverse)
{
    uint16_t phase = effect_phase(now_ms, speed, 9000, 900, reverse);
    uint16_t head = ((uint32_t)phase * led_count) >> 16;
    uint8_t hue_offset = now_ms / 18U;
    for (uint16_t i = 0; i < led_count; ++i) {
        rgb_t background = rainbow_wheel(hue_offset + (uint32_t)i * 256U / led_count);
        set_pixel(i, background, 7);
    }

    static const uint8_t tail_scale[] = {190, 125, 76, 42, 20, 8};
    size_t tail_length = led_count < sizeof(tail_scale) ? led_count : sizeof(tail_scale);
    for (size_t tail = 0; tail < tail_length; ++tail) {
        int32_t pixel = reverse ? (int32_t)head + tail : (int32_t)head - tail;
        rgb_t color = rainbow_wheel(hue_offset - tail * 18U);
        set_pixel(wrapped_pixel(pixel, led_count), color, tail_scale[tail]);
    }
}

static void render_wled_theater_rainbow(uint16_t led_count, uint32_t now_ms,
                                        uint8_t speed, bool reverse)
{
    uint32_t step_ms = effect_period_ms(speed, 360, 55);
    uint8_t offset = (now_ms / step_ms) % 3U;
    uint8_t hue_offset = now_ms / 16U;
    memset(s_pixels, 0, (size_t)led_count * 3);
    for (uint16_t i = 0; i < led_count; ++i) {
        uint16_t position = reverse ? led_count - 1U - i : i;
        if ((position + offset) % 3U == 0) {
            rgb_t color = rainbow_wheel(hue_offset +
                                        (uint32_t)position * 256U / led_count);
            set_pixel(i, color, 165);
        }
    }
}

static void render_effect(uint16_t led_count, rgb_t color, led_effect_t effect,
                          uint32_t now_ms, uint8_t speed, bool reverse)
{
    switch (effect) {
    case LED_EFFECT_SOFT_FADE:
        render_soft_fade(led_count, color, now_ms, speed);
        break;
    case LED_EFFECT_SOFT_BREATHE:
        render_soft_breathe(led_count, color, now_ms, speed);
        break;
    case LED_EFFECT_DOUBLE_PULSE:
        render_double_pulse(led_count, color, now_ms, speed);
        break;
    case LED_EFFECT_CHASE:
        render_chase(led_count, color, now_ms, speed, reverse);
        break;
    case LED_EFFECT_COMET:
        render_comet(led_count, color, now_ms, speed, reverse);
        break;
    case LED_EFFECT_SCANNER:
        render_scanner(led_count, color, now_ms, speed, reverse);
        break;
    case LED_EFFECT_WAVE:
        render_wave(led_count, color, now_ms, speed, reverse);
        break;
    case LED_EFFECT_WIPE:
        render_wipe(led_count, color, now_ms, speed, reverse);
        break;
    case LED_EFFECT_TWINKLE:
        render_twinkle(led_count, color, now_ms, speed, reverse);
        break;
    case LED_EFFECT_WLED_COLOR_TWINKLES:
        render_wled_color_twinkles(led_count, speed);
        break;
    case LED_EFFECT_WLED_TWINKLEFOX:
        render_wled_twinklefox(led_count, now_ms, speed, reverse);
        break;
    case LED_EFFECT_WLED_CHASE_RAINBOW:
        render_wled_chase_rainbow(led_count, now_ms, speed, reverse);
        break;
    case LED_EFFECT_WLED_THEATER_RAINBOW:
        render_wled_theater_rainbow(led_count, now_ms, speed, reverse);
        break;
    case LED_EFFECT_SOLID:
    default:
        render_solid(led_count, color);
        break;
    }
}

static effect_selection_t automatic_effect_for_reading(int glucose_mgdl,
                                                        const char *direction,
                                                        bool dynamic_trend_enabled)
{
    effect_selection_t selection = {
        .effect = LED_EFFECT_SOLID,
        .speed = 50,
        .reverse = false,
    };

    // Critical glucose overrides always take priority over the trend arrow and
    // remain active even if the optional dynamic trend hint is disabled.
    if (glucose_mgdl >= 270) {
        selection.effect = LED_EFFECT_WLED_TWINKLEFOX;
        selection.speed = 100;
        return selection;
    }
    if (glucose_mgdl < 60) {
        selection.effect = LED_EFFECT_DOUBLE_PULSE;
        selection.speed = 100;
        return selection;
    }

    if (!dynamic_trend_enabled || direction == NULL) {
        return selection;
    }
    if (strcasecmp(direction, "DoubleUp") == 0) {
        selection.effect = LED_EFFECT_SOFT_BREATHE;
        selection.speed = 100;
    } else if (strcasecmp(direction, "SingleUp") == 0) {
        selection.effect = LED_EFFECT_SOFT_BREATHE;
        selection.speed = 80;
    } else if (strcasecmp(direction, "SingleDown") == 0) {
        selection.effect = LED_EFFECT_WIPE;
        selection.speed = 50;
        selection.reverse = true;
    } else if (strcasecmp(direction, "DoubleDown") == 0) {
        selection.effect = LED_EFFECT_COMET;
        selection.speed = 80;
        selection.reverse = true;
    }
    return selection;
}

static void render_config_chase(uint16_t led_count, uint32_t now_ms)
{
    memset(s_pixels, 0, (size_t)led_count * 3);
    uint16_t head = (now_ms / 80U) % led_count;
    static const uint8_t tail_scale[] = {CONFIG_CHASE_WHITE, 40, 18, 7};
    size_t tail_length = led_count < sizeof(tail_scale) ? led_count : sizeof(tail_scale);
    rgb_t white = {.red = 255, .green = 255, .blue = 255};
    for (size_t tail = 0; tail < tail_length; ++tail) {
        uint16_t pixel = (head + led_count - tail) % led_count;
        set_pixel(pixel, white, tail_scale[tail]);
    }
}

static void render_no_data_breathing(uint16_t led_count, uint32_t now_ms)
{
    uint16_t phase = effect_phase(now_ms, 42, 6500, 1800, false);
    uint8_t level = 2U + (uint32_t)smoothstep8(triangle8(phase)) * 18U / 255U;
    rgb_t white = {.red = 255, .green = 255, .blue = 255};
    for (uint16_t i = 0; i < led_count; ++i) {
        set_pixel(i, white, level);
    }
}

static bool render_reset_feedback(uint16_t led_count, int64_t started_us)
{
    int64_t elapsed_ms = (esp_timer_get_time() - started_us) / 1000;
    int64_t total_ms = RESET_PULSE_COUNT * RESET_PULSE_MS;
    if (elapsed_ms >= total_ms) {
        rgb_t white = {.red = 255, .green = 255, .blue = 255};
        for (uint16_t i = 0; i < led_count; ++i) {
            set_pixel(i, white, 4);
        }
        return true;
    }

    int32_t within = elapsed_ms % RESET_PULSE_MS;
    int32_t half = RESET_PULSE_MS / 2;
    int32_t ramp = within < half ? within : RESET_PULSE_MS - within;
    uint8_t level = 2 + (uint32_t)ramp * 26 / half;
    rgb_t white = {.red = 255, .green = 255, .blue = 255};
    for (uint16_t i = 0; i < led_count; ++i) {
        set_pixel(i, white, level);
    }
    return false;
}

static void led_task(void *arg)
{
    (void)arg;
    bool reset_notified = false;

    // On a true USB cold start, some ESP32-C3 boards need one scheduler turn
    // before their first RMT transaction. Keep LED startup best-effort so a
    // transient output timeout can never prevent Wi-Fi pairing from starting.
    vTaskDelay(pdMS_TO_TICKS(20));
    memset(s_pixels, 0, sizeof(s_pixels));
    esp_err_t clear_err = transmit_pixels(APP_MAX_LED_COUNT);
    if (clear_err != ESP_OK) {
        ESP_LOGW(TAG, "Initial LED clear skipped: %s", esp_err_to_name(clear_err));
    }

    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        display_state_t state;
        portENTER_CRITICAL(&s_state_lock);
        state = s_state;
        portEXIT_CRITICAL(&s_state_lock);

        uint16_t count = state.led_count;
        if (count < 1 || count > APP_MAX_LED_COUNT) {
            count = APP_DEFAULT_LED_COUNT;
        }

        bool dynamic_effect = false;
        bool feedback_done = false;
        if (state.mode == DISPLAY_RESET_FEEDBACK) {
            memset(s_pixels, 0, (size_t)count * 3);
            feedback_done = render_reset_feedback(count, state.reset_started_us);
            if (feedback_done && !reset_notified) {
                xSemaphoreGive(s_reset_done);
                reset_notified = true;
            }
        } else if (state.preview_active) {
            render_effect(count, glucose_color(state.preview_glucose_mgdl),
                          state.preview_effect, now_ms, state.preview_speed,
                          state.preview_reverse);
            dynamic_effect = state.preview_effect != LED_EFFECT_SOLID;
            reset_notified = false;
        } else {
            switch (state.mode) {
            case DISPLAY_CONFIG:
                render_config_chase(count, now_ms);
                dynamic_effect = true;
                reset_notified = false;
                break;
            case DISPLAY_GLUCOSE:
                if (sample_is_stale(&state)) {
                    render_no_data_breathing(count, now_ms);
                    dynamic_effect = true;
                } else {
                    effect_selection_t selection =
                        automatic_effect_for_reading(state.glucose_mgdl,
                                                     state.direction,
                                                     state.dynamic_trend_enabled);
                    render_effect(count, glucose_color(state.glucose_mgdl),
                                  selection.effect, now_ms, selection.speed,
                                  selection.reverse);
                    dynamic_effect = selection.effect != LED_EFFECT_SOLID;
                }
                reset_notified = false;
                break;
            case DISPLAY_NO_DATA:
            default:
                render_no_data_breathing(count, now_ms);
                dynamic_effect = true;
                reset_notified = false;
                break;
            }
        }

        apply_fixed_power_budget(count);
        esp_err_t err = transmit_pixels(count);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "LED transmit failed: %s", esp_err_to_name(err));
        }

        uint32_t refresh_ms = dynamic_effect ? DYNAMIC_REFRESH_MS : EFFECT_FRAME_MS;
        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(refresh_ms));
    }
}

esp_err_t led_controller_init(uint16_t led_count)
{
    if (led_count < 1 || led_count > APP_MAX_LED_COUNT) {
        led_count = APP_DEFAULT_LED_COUNT;
    }

    rmt_tx_channel_config_t channel_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = LED_DATA_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .trans_queue_depth = 2,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&channel_config, &s_led_channel), TAG,
                        "Could not create RMT channel");

    rmt_simple_encoder_config_t encoder_config = {
        .callback = ws2812_encoder_callback,
        .min_chunk_size = 64,
    };
    ESP_RETURN_ON_ERROR(rmt_new_simple_encoder(&encoder_config, &s_led_encoder), TAG,
                        "Could not create WS2812 encoder");
    ESP_RETURN_ON_ERROR(rmt_enable(s_led_channel), TAG, "Could not enable RMT channel");

    s_reset_done = xSemaphoreCreateBinary();
    if (s_reset_done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_state.led_count = led_count;

    // The LED task clears the supported maximum asynchronously. In particular,
    // a transient RMT timeout during a cold start must not abort app_main before
    // the configuration hotspot can be created.
    memset(s_pixels, 0, sizeof(s_pixels));

    if (xTaskCreate(led_task, "led_effects", 5120, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void led_controller_set_count(uint16_t led_count)
{
    if (led_count < 1 || led_count > APP_MAX_LED_COUNT) {
        return;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_state.led_count = led_count;
    portEXIT_CRITICAL(&s_state_lock);
}

void led_controller_set_dynamic_trend(bool enabled)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.dynamic_trend_enabled = enabled;
    portEXIT_CRITICAL(&s_state_lock);
}

void led_controller_show_config(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.preview_active = false;
    s_state.mode = DISPLAY_CONFIG;
    portEXIT_CRITICAL(&s_state_lock);
}

void led_controller_show_no_data(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.mode = DISPLAY_NO_DATA;
    portEXIT_CRITICAL(&s_state_lock);
}

void led_controller_show_glucose(int glucose_mgdl, int64_t sample_timestamp_ms,
                                 const char *direction, bool reported_stale)
{
    int64_t observed_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    if (s_state.sample_timestamp_ms != sample_timestamp_ms) {
        s_state.sample_timestamp_ms = sample_timestamp_ms;
        s_state.sample_observed_us = observed_us;
        s_state.sample_reported_stale = reported_stale;
    } else if (reported_stale) {
        // Once Nightscout reports this exact record as stale, do not make it
        // look fresh again if the status endpoint is temporarily unavailable.
        s_state.sample_reported_stale = true;
    }
    s_state.mode = DISPLAY_GLUCOSE;
    s_state.glucose_mgdl = glucose_mgdl;
    s_state.has_live_glucose = true;
    strlcpy(s_state.direction, direction != NULL ? direction : "Unknown",
            sizeof(s_state.direction));
    portEXIT_CRITICAL(&s_state_lock);
}

void led_controller_preview_effect(led_effect_t effect, int glucose_mgdl,
                                   uint8_t speed, bool reverse)
{
    if (effect >= LED_EFFECT_COUNT) {
        return;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_state.preview_active = true;
    s_state.preview_effect = effect;
    s_state.preview_glucose_mgdl = glucose_mgdl;
    s_state.preview_speed = clamp_speed(speed);
    s_state.preview_reverse = reverse;
    portEXIT_CRITICAL(&s_state_lock);
}

void led_controller_stop_preview(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state.preview_active = false;
    portEXIT_CRITICAL(&s_state_lock);
}

void led_controller_get_effect_lab_status(led_effect_lab_status_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    portENTER_CRITICAL(&s_state_lock);
    status->preview_active = s_state.preview_active;
    status->preview_effect = s_state.preview_effect;
    status->preview_glucose_mgdl = s_state.preview_glucose_mgdl;
    status->preview_speed = s_state.preview_speed;
    status->preview_reverse = s_state.preview_reverse;
    status->has_live_glucose = s_state.has_live_glucose;
    status->live_glucose_mgdl = s_state.glucose_mgdl;
    strlcpy(status->live_direction, s_state.direction, sizeof(status->live_direction));
    portEXIT_CRITICAL(&s_state_lock);
}

void led_controller_start_reset_feedback(void)
{
    while (xSemaphoreTake(s_reset_done, 0) == pdTRUE) {
    }
    portENTER_CRITICAL(&s_state_lock);
    s_state.preview_active = false;
    s_state.mode = DISPLAY_RESET_FEEDBACK;
    s_state.reset_started_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_state_lock);
}

bool led_controller_wait_reset_feedback(uint32_t timeout_ms)
{
    return xSemaphoreTake(s_reset_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
