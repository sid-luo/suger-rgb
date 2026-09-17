#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_controller.h"
#include "network_manager.h"
#include "nightscout.h"
#include "nvs_flash.h"
#include "portal.h"

#define BOOT_BUTTON_GPIO       GPIO_NUM_9
#define BOOT_HOLD_MS           5000
#define BUTTON_DEBOUNCE_MS     40
#define NIGHTSCOUT_POLL_MS     60000

static const char *TAG = "suger_rgb";
static app_config_t s_config;
static volatile bool s_config_mode;

static esp_err_t enter_config_mode(void)
{
    if (s_config_mode) {
        return ESP_OK;
    }

    s_config_mode = true;
    led_controller_show_config();
    esp_err_t err = network_manager_start_config_ap();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start configuration AP: %s", esp_err_to_name(err));
        s_config_mode = false;
        return err;
    }
    err = portal_start(&s_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start configuration page: %s", esp_err_to_name(err));
        s_config_mode = false;
        return err;
    }

    ESP_LOGI(TAG, "Configuration mode: connect to open network %s",
             network_manager_ap_ssid());
    return ESP_OK;
}

static void forced_restore_after_release(void)
{
    ESP_LOGW(TAG, "Forced restore requested; waiting for feedback animation");
    led_controller_wait_reset_feedback(6000);

    esp_err_t err = app_config_clear_connection();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not clear connection settings: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Wi-Fi and Nightscout settings cleared; LED count preserved");
    }

    // GPIO9 is an ESP32-C3 strapping pin. Reboot only after the button has
    // actually been released, otherwise the chip can enter download mode.
    while (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

static void boot_button_task(void *arg)
{
    (void)arg;
    bool last_raw_pressed = false;
    bool stable_pressed = false;
    bool hold_triggered = false;
    bool reset_armed = false;
    int64_t raw_changed_us = esp_timer_get_time();
    int64_t pressed_since_us = 0;

    while (true) {
        bool raw_pressed = gpio_get_level(BOOT_BUTTON_GPIO) == 0;
        int64_t now_us = esp_timer_get_time();

        if (raw_pressed != last_raw_pressed) {
            last_raw_pressed = raw_pressed;
            raw_changed_us = now_us;
        }

        if (raw_pressed != stable_pressed &&
            now_us - raw_changed_us >= BUTTON_DEBOUNCE_MS * 1000LL) {
            stable_pressed = raw_pressed;
            if (stable_pressed) {
                pressed_since_us = now_us;
                hold_triggered = false;
                reset_armed = false;
            } else {
                if (reset_armed) {
                    forced_restore_after_release();
                }
                // A complete release is mandatory before another long press
                // can be recognized. One continuous hold can never trigger
                // both configuration mode and forced restore.
                pressed_since_us = 0;
                hold_triggered = false;
                reset_armed = false;
            }
        }

        if (stable_pressed && !hold_triggered && pressed_since_us > 0 &&
            now_us - pressed_since_us >= BOOT_HOLD_MS * 1000LL) {
            hold_triggered = true;
            if (!s_config_mode) {
                ESP_LOGI(TAG, "BOOT long press: entering configuration mode");
                enter_config_mode();
            } else {
                ESP_LOGW(TAG, "BOOT long press in configuration mode: forced restore armed");
                reset_armed = true;
                led_controller_start_reset_feedback();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void nightscout_poll_task(void *arg)
{
    (void)arg;
    bool have_reading = false;

    while (true) {
        if (!s_config_mode && network_manager_wait_connected(10000) == ESP_OK) {
            nightscout_reading_t reading;
            char error_message[160];
            esp_err_t err = nightscout_fetch_latest(s_config.nightscout_url, &reading,
                                                    error_message, sizeof(error_message));
            if (err == ESP_OK) {
                have_reading = true;
                led_controller_show_glucose(reading.glucose_mgdl, reading.timestamp_ms,
                                            reading.direction, reading.stale);
                ESP_LOGI(TAG, "Nightscout: %d mg/dL, direction=%s%s",
                         reading.glucose_mgdl, reading.direction,
                         reading.stale ? " (stale)" : "");
            } else {
                ESP_LOGW(TAG, "Nightscout update failed: %s", error_message);
                if (!have_reading) {
                    led_controller_show_no_data();
                }
            }
        } else if (!s_config_mode && !have_reading) {
            led_controller_show_no_data();
        }

        for (int elapsed = 0; elapsed < NIGHTSCOUT_POLL_MS / 250; ++elapsed) {
            vTaskDelay(pdMS_TO_TICKS(250));
            if (s_config_mode) {
                break;
            }
        }
    }
}

void app_main(void)
{
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    ESP_ERROR_CHECK(app_config_load(&s_config));

    ESP_LOGI(TAG, "Starting Suger RGB; LED data GPIO=%d, count=%u",
             LED_DATA_GPIO, s_config.led_count);
    ESP_ERROR_CHECK(led_controller_init(s_config.led_count));
    led_controller_set_dynamic_trend(s_config.dynamic_trend_enabled);
    led_controller_show_no_data();
    ESP_ERROR_CHECK(network_manager_init());

    gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button_config));
    xTaskCreate(boot_button_task, "boot_button", 3072, NULL, 6, NULL);

    if (s_config.complete) {
        ESP_LOGI(TAG, "Stored configuration found; connecting to Wi-Fi");
        ESP_ERROR_CHECK(network_manager_start_normal(&s_config));
        xTaskCreate(nightscout_poll_task, "nightscout_poll", 10240, NULL, 4, NULL);
    } else {
        ESP_LOGI(TAG, "No complete configuration found; starting pairing mode");
        s_config_mode = false;
        ESP_ERROR_CHECK(enter_config_mode());
    }
}
