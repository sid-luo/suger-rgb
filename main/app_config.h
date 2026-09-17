#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define APP_SSID_MAX_LEN             32
#define APP_WIFI_PASSWORD_MAX_LEN    64
#define APP_NIGHTSCOUT_URL_MAX_LEN   192
#define APP_DEFAULT_LED_COUNT        30
#define APP_MAX_LED_COUNT            128

typedef struct {
    bool complete;
    char ssid[APP_SSID_MAX_LEN + 1];
    char password[APP_WIFI_PASSWORD_MAX_LEN + 1];
    char nightscout_url[APP_NIGHTSCOUT_URL_MAX_LEN + 1];
    uint16_t led_count;
    bool dynamic_trend_enabled;
} app_config_t;

esp_err_t app_config_load(app_config_t *config);
esp_err_t app_config_save(const app_config_t *config);

/**
 * Clear Wi-Fi and Nightscout settings while preserving hardware/preferences.
 */
esp_err_t app_config_clear_connection(void);
