#include "app_config.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#define CONFIG_NAMESPACE "t1dlamp"

static const char *TAG = "app_config";

static esp_err_t read_string(nvs_handle_t handle, const char *key, char *output, size_t output_size)
{
    size_t required = output_size;
    esp_err_t err = nvs_get_str(handle, key, output, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        output[0] = '\0';
        return ESP_OK;
    }
    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGW(TAG, "Stored value for %s is too long; ignoring it", key);
        output[0] = '\0';
        return ESP_OK;
    }
    return err;
}

esp_err_t app_config_load(app_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));
    config->led_count = APP_DEFAULT_LED_COUNT;
    config->dynamic_trend_enabled = true;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t complete = 0;
    err = nvs_get_u8(handle, "configured", &complete);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = read_string(handle, "ssid", config->ssid, sizeof(config->ssid));
    }
    if (err == ESP_OK) {
        err = read_string(handle, "wifi_pass", config->password, sizeof(config->password));
    }
    if (err == ESP_OK) {
        err = read_string(handle, "ns_url", config->nightscout_url, sizeof(config->nightscout_url));
    }

    uint16_t led_count = APP_DEFAULT_LED_COUNT;
    esp_err_t led_err = nvs_get_u16(handle, "led_count", &led_count);
    if (led_err == ESP_OK && led_count >= 1 && led_count <= APP_MAX_LED_COUNT) {
        config->led_count = led_count;
    }

    uint8_t dynamic_trend = 1;
    esp_err_t trend_err = nvs_get_u8(handle, "trend_fx", &dynamic_trend);
    if (trend_err == ESP_OK && dynamic_trend <= 1U) {
        config->dynamic_trend_enabled = dynamic_trend == 1U;
    }

    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }

    config->complete = complete == 1 && config->ssid[0] != '\0' && config->nightscout_url[0] != '\0';
    return ESP_OK;
}

esp_err_t app_config_save(const app_config_t *config)
{
    if (config == NULL || config->ssid[0] == '\0' || config->nightscout_url[0] == '\0' ||
        config->led_count < 1 || config->led_count > APP_MAX_LED_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    // Remove the legacy user-selected effect when an older installation is
    // configured again. Lighting behavior is no longer a saved preference.
    esp_err_t erase_effect_err = nvs_erase_key(handle, "effect");
    if (erase_effect_err != ESP_OK && erase_effect_err != ESP_ERR_NVS_NOT_FOUND) {
        err = erase_effect_err;
    }

    if (err == ESP_OK &&
        (err = nvs_set_str(handle, "ssid", config->ssid)) == ESP_OK &&
        (err = nvs_set_str(handle, "wifi_pass", config->password)) == ESP_OK &&
        (err = nvs_set_str(handle, "ns_url", config->nightscout_url)) == ESP_OK &&
        (err = nvs_set_u16(handle, "led_count", config->led_count)) == ESP_OK &&
        (err = nvs_set_u8(handle, "trend_fx",
                          config->dynamic_trend_enabled ? 1U : 0U)) == ESP_OK &&
        (err = nvs_set_u8(handle, "configured", 1)) == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

esp_err_t app_config_clear_connection(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    const char *keys[] = {"configured", "ssid", "wifi_pass", "ns_url", "effect"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        esp_err_t erase_err = nvs_erase_key(handle, keys[i]);
        if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
            err = erase_err;
            break;
        }
    }

    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
