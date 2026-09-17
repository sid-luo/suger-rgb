#include "network_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_AP_STARTED_BIT BIT1
#define WIFI_START_TIMEOUT_MS 5000

static const char *TAG = "network";
static EventGroupHandle_t s_wifi_events;
static bool s_auto_reconnect;
static bool s_wifi_started;
static uint8_t s_last_disconnect_reason;
static char s_ap_ssid[24] = "Suger-RGB";

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = event_data;
        s_last_disconnect_reason = event->reason;
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        if (s_auto_reconnect) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        xEventGroupSetBits(s_wifi_events, WIFI_AP_STARTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
        xEventGroupClearBits(s_wifi_events, WIFI_AP_STARTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi connected; IP=" IPSTR,
                 IP2STR(&event->ip_info.ip));
    }
}

static esp_err_t set_sta_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > APP_SSID_MAX_LEN ||
        password == NULL || strlen(password) > APP_WIFI_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t sta_config = {0};
    strlcpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta_config.sta.pmf_cfg.capable = true;
    sta_config.sta.pmf_cfg.required = false;
    return esp_wifi_set_config(WIFI_IF_STA, &sta_config);
}

esp_err_t network_manager_init(void)
{
    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        return loop_err;
    }

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "Suger-RGB-%02X%02X", mac[4], mac[5]);

    return ESP_OK;
}

esp_err_t network_manager_start_normal(const app_config_t *config)
{
    if (config == NULL || !config->complete) {
        return ESP_ERR_INVALID_STATE;
    }

    s_auto_reconnect = false;
    if (s_wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_stop());
        s_wifi_started = false;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(set_sta_credentials(config->ssid, config->password));
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_AP_STARTED_BIT);
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_started = true;
    s_auto_reconnect = true;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Initial Wi-Fi connect failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t network_manager_start_config_ap(void)
{
    s_auto_reconnect = false;
    if (s_wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_stop());
        s_wifi_started = false;
    }

    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, s_ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(s_ap_ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.ssid_hidden = 0;
    ap_config.ap.beacon_interval = 100;

    // Configure the AP completely before starting the radio, following the
    // ESP-IDF SoftAP lifecycle. Candidate Wi-Fi validation enables the STA
    // interface later without stopping this AP.
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_AP_STARTED_BIT);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_started = true;

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_AP_STARTED_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_START_TIMEOUT_MS));
    if ((bits & WIFI_AP_STARTED_BIT) == 0) {
        ESP_LOGE(TAG, "Configuration AP did not start within %d ms",
                 WIFI_START_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Configuration AP ready: %s", s_ap_ssid);
    return ESP_OK;
}

esp_err_t network_manager_connect_candidate(const char *ssid, const char *password,
                                            uint32_t timeout_ms,
                                            char *error_message, size_t error_message_size)
{
    if (error_message != NULL && error_message_size > 0) {
        error_message[0] = '\0';
    }

    s_auto_reconnect = false;
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    esp_err_t disconnect_err = esp_wifi_disconnect();
    if (disconnect_err != ESP_OK && disconnect_err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "Could not disconnect previous Wi-Fi candidate: %s",
                 esp_err_to_name(disconnect_err));
    }

    esp_err_t err = set_sta_credentials(ssid, password);
    if (err != ESP_OK) {
        if (error_message != NULL) {
            snprintf(error_message, error_message_size, "Wi-Fi 参数无效");
        }
        return err;
    }

    s_last_disconnect_reason = 0;
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        if (error_message != NULL) {
            snprintf(error_message, error_message_size, "无法开始连接 Wi-Fi");
        }
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        if (error_message != NULL) {
            if (s_last_disconnect_reason == WIFI_REASON_AUTH_FAIL ||
                s_last_disconnect_reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                s_last_disconnect_reason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
                snprintf(error_message, error_message_size, "Wi-Fi 密码错误");
            } else if (s_last_disconnect_reason == WIFI_REASON_NO_AP_FOUND) {
                snprintf(error_message, error_message_size, "找不到这个 Wi-Fi");
            } else {
                snprintf(error_message, error_message_size, "连接 Wi-Fi 超时");
            }
        }
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

bool network_manager_is_connected(void)
{
    return (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

esp_err_t network_manager_wait_connected(uint32_t timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

const char *network_manager_ap_ssid(void)
{
    return s_ap_ssid;
}
