#include "portal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "dns_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_controller.h"
#include "network_manager.h"
#include "nightscout.h"

#define REQUEST_BODY_MAX 1024

extern const uint8_t portal_html_start[] asm("_binary_portal_html_start");
extern const uint8_t portal_html_end[] asm("_binary_portal_html_end");

static const char *TAG = "portal";
static httpd_handle_t s_server;
static app_config_t s_current_config;
static app_config_t s_pending_config;
static bool s_pending_valid;
static char s_validation_nonce[16];

static esp_err_t send_json(httpd_req_t *request, cJSON *json)
{
    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (body == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON allocation failed");
    }
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(request, body);
    free(body);
    return err;
}

typedef struct {
    const char *message;
    const char *code;
} api_error_mapping_t;

static const char *api_error_code(const char *message)
{
    static const api_error_mapping_t mappings[] = {
        {"请求内容格式错误", "invalid_request"},
        {"请完整填写配网信息", "incomplete_config"},
        {"Wi-Fi 名称不能为空或超过 32 字节", "ssid_invalid"},
        {"Wi-Fi 密码太长", "wifi_password_too_long"},
        {"灯珠数量必须在 1 到 64 之间", "led_count_invalid"},
        {"灯效参数不完整", "effect_params_incomplete"},
        {"未知的灯效", "effect_unknown"},
        {"模拟血糖必须在 20 到 600 mg/dL 之间", "preview_glucose_invalid"},
        {"速度必须在 1 到 100 之间", "effect_speed_invalid"},
        {"配置已经变化，请重新验证", "validation_expired"},
        {"保存配置失败", "save_failed"},
        {"Wi-Fi 参数无效", "wifi_invalid"},
        {"无法开始连接 Wi-Fi", "wifi_start_failed"},
        {"Wi-Fi 密码错误", "wifi_password_wrong"},
        {"找不到这个 Wi-Fi", "wifi_not_found"},
        {"连接 Wi-Fi 超时", "wifi_timeout"},
        {"无法创建 HTTPS 请求", "nightscout_request_create_failed"},
        {"Nightscout 连接失败，请检查域名和 HTTPS 证书", "nightscout_connect_failed"},
        {"Nightscout 请求超时或网络异常", "nightscout_network_failed"},
        {"这个 Nightscout 不允许匿名读取（HTTP 401/403）", "nightscout_anonymous_denied"},
        {"地址可以访问，但没有找到 Nightscout API", "nightscout_api_not_found"},
        {"Nightscout 地址太长", "nightscout_address_too_long"},
        {"Nightscout 状态接口没有返回 serverTimeEpoch", "nightscout_status_time_missing"},
        {"Nightscout 返回的服务器时间无效", "nightscout_server_time_invalid"},
        {"请输入 Nightscout 地址", "nightscout_address_required"},
        {"当前版本只支持 HTTPS 地址", "nightscout_https_only"},
        {"地址中不能包含参数、锚点或账号信息", "nightscout_address_parts_invalid"},
        {"Nightscout 地址格式不正确", "nightscout_address_invalid"},
        {"Nightscout 域名不能为空", "nightscout_host_required"},
        {"Nightscout 请求失败", "nightscout_request_failed"},
        {"Nightscout 返回的数据过大", "nightscout_response_too_large"},
        {"返回内容不是有效的 Nightscout 数据", "nightscout_data_invalid"},
        {"Nightscout 暂时没有血糖数据", "nightscout_no_data"},
        {"血糖数据缺少有效的 sgv 或 date 字段", "nightscout_fields_missing"},
        {"Nightscout 数据时间无效", "nightscout_data_time_invalid"},
        {"血糖记录时间晚于 Nightscout 服务器时间", "nightscout_record_in_future"},
    };

    if (message == NULL) {
        return "request_failed";
    }
    const char *http_prefix = "Nightscout 返回 HTTP ";
    if (strncmp(message, http_prefix, strlen(http_prefix)) == 0) {
        return "nightscout_http";
    }
    for (size_t i = 0; i < sizeof(mappings) / sizeof(mappings[0]); ++i) {
        if (strcmp(message, mappings[i].message) == 0) {
            return mappings[i].code;
        }
    }
    return "request_failed";
}

static esp_err_t send_api_error(httpd_req_t *request, const char *message)
{
    httpd_resp_set_status(request, "400 Bad Request");
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", false);
    cJSON_AddStringToObject(json, "error", message);
    const char *code = api_error_code(message);
    cJSON_AddStringToObject(json, "errorCode", code);
    if (strcmp(code, "nightscout_http") == 0) {
        int status = 0;
        if (sscanf(message, "Nightscout 返回 HTTP %d", &status) == 1) {
            cJSON_AddNumberToObject(json, "status", status);
        }
    }
    return send_json(request, json);
}

static esp_err_t read_json_body(httpd_req_t *request, cJSON **json)
{
    if (request->content_len <= 0 || request->content_len >= REQUEST_BODY_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    char body[REQUEST_BODY_MAX];
    size_t received_total = 0;
    while (received_total < request->content_len) {
        int received = httpd_req_recv(request, body + received_total,
                                      request->content_len - received_total);
        if (received <= 0) {
            return ESP_FAIL;
        }
        received_total += received;
    }
    body[received_total] = '\0';

    *json = cJSON_Parse(body);
    return *json == NULL ? ESP_ERR_INVALID_ARG : ESP_OK;
}

static esp_err_t send_embedded_html(httpd_req_t *request,
                                    const uint8_t *start, const uint8_t *end)
{
    size_t length = end - start;
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, (const char *)start, length);
}

static esp_err_t index_handler(httpd_req_t *request)
{
    return send_embedded_html(request, portal_html_start, portal_html_end);
}

static esp_err_t setup_page_handler(httpd_req_t *request)
{
    return send_embedded_html(request, portal_html_start, portal_html_end);
}

static esp_err_t effects_page_handler(httpd_req_t *request)
{
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", "/setup#trendInfo");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, NULL, 0);
}

static esp_err_t config_handler(httpd_req_t *request)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "ssid", s_current_config.ssid);
    cJSON_AddStringToObject(json, "nightscout", s_current_config.nightscout_url);
    cJSON_AddNumberToObject(json, "ledCount", s_current_config.led_count);
    cJSON_AddBoolToObject(json, "dynamicTrend",
                          s_current_config.dynamic_trend_enabled);
    cJSON_AddBoolToObject(json, "configured", s_current_config.complete);
    cJSON_AddStringToObject(json, "apSsid", network_manager_ap_ssid());
    return send_json(request, json);
}

static esp_err_t validate_handler(httpd_req_t *request)
{
    cJSON *json = NULL;
    if (read_json_body(request, &json) != ESP_OK) {
        return send_api_error(request, "请求内容格式错误");
    }

    cJSON *ssid_json = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    cJSON *password_json = cJSON_GetObjectItemCaseSensitive(json, "password");
    cJSON *nightscout_json = cJSON_GetObjectItemCaseSensitive(json, "nightscout");
    cJSON *led_count_json = cJSON_GetObjectItemCaseSensitive(json, "ledCount");
    cJSON *dynamic_trend_json =
        cJSON_GetObjectItemCaseSensitive(json, "dynamicTrend");

    if (!cJSON_IsString(ssid_json) || !cJSON_IsString(password_json) ||
        !cJSON_IsString(nightscout_json) || !cJSON_IsNumber(led_count_json) ||
        !cJSON_IsBool(dynamic_trend_json)) {
        cJSON_Delete(json);
        return send_api_error(request, "请完整填写配网信息");
    }

    const char *ssid = ssid_json->valuestring;
    const char *submitted_password = password_json->valuestring;
    const char *nightscout_input = nightscout_json->valuestring;
    int led_count = led_count_json->valueint;
    if (ssid[0] == '\0' || strlen(ssid) > APP_SSID_MAX_LEN) {
        cJSON_Delete(json);
        return send_api_error(request, "Wi-Fi 名称不能为空或超过 32 字节");
    }
    if (strlen(submitted_password) > APP_WIFI_PASSWORD_MAX_LEN) {
        cJSON_Delete(json);
        return send_api_error(request, "Wi-Fi 密码太长");
    }
    if (led_count < 1 || led_count > APP_MAX_LED_COUNT) {
        cJSON_Delete(json);
        return send_api_error(request, "灯珠数量必须在 1 到 128 之间");
    }
    char password[APP_WIFI_PASSWORD_MAX_LEN + 1];
    if (submitted_password[0] == '\0' && s_current_config.complete &&
        strcmp(ssid, s_current_config.ssid) == 0) {
        strlcpy(password, s_current_config.password, sizeof(password));
    } else {
        strlcpy(password, submitted_password, sizeof(password));
    }

    char canonical_url[APP_NIGHTSCOUT_URL_MAX_LEN + 1];
    char error_message[160];
    esp_err_t err = nightscout_normalize_base_url(nightscout_input, canonical_url,
                                                   sizeof(canonical_url),
                                                   error_message, sizeof(error_message));
    if (err != ESP_OK) {
        cJSON_Delete(json);
        return send_api_error(request, error_message);
    }

    led_controller_set_count(led_count);
    err = network_manager_connect_candidate(ssid, password, 15000,
                                            error_message, sizeof(error_message));
    if (err != ESP_OK) {
        cJSON_Delete(json);
        return send_api_error(request, error_message);
    }

    nightscout_reading_t reading;
    err = nightscout_fetch_latest(canonical_url, &reading,
                                  error_message, sizeof(error_message));
    if (err != ESP_OK) {
        cJSON_Delete(json);
        return send_api_error(request, error_message);
    }

    memset(&s_pending_config, 0, sizeof(s_pending_config));
    s_pending_config.complete = true;
    strlcpy(s_pending_config.ssid, ssid, sizeof(s_pending_config.ssid));
    strlcpy(s_pending_config.password, password, sizeof(s_pending_config.password));
    strlcpy(s_pending_config.nightscout_url, canonical_url,
            sizeof(s_pending_config.nightscout_url));
    s_pending_config.led_count = led_count;
    s_pending_config.dynamic_trend_enabled = cJSON_IsTrue(dynamic_trend_json);
    snprintf(s_validation_nonce, sizeof(s_validation_nonce), "%08" PRIx32, esp_random());
    s_pending_valid = true;
    cJSON_Delete(json);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "ok", true);
    cJSON_AddStringToObject(response, "nonce", s_validation_nonce);
    cJSON_AddStringToObject(response, "baseUrl", canonical_url);
    cJSON_AddNumberToObject(response, "sgv", reading.glucose_mgdl);
    cJSON_AddBoolToObject(response, "ageKnown", reading.age_known);
    cJSON_AddNumberToObject(response, "ageMinutes", reading.age_minutes);
    cJSON_AddBoolToObject(response, "stale", reading.stale);
    cJSON_AddStringToObject(response, "direction", reading.direction);
    return send_json(request, response);
}

static cJSON *effect_lab_status_json(void)
{
    led_effect_lab_status_t status;
    led_controller_get_effect_lab_status(&status);

    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", true);
    cJSON_AddBoolToObject(json, "active", status.preview_active);
    cJSON_AddNumberToObject(json, "effect", status.preview_effect);
    cJSON_AddNumberToObject(json, "glucose", status.preview_glucose_mgdl);
    cJSON_AddNumberToObject(json, "speed", status.preview_speed);
    cJSON_AddBoolToObject(json, "reverse", status.preview_reverse);
    cJSON_AddNumberToObject(json, "effectCount", LED_EFFECT_COUNT);
    cJSON_AddBoolToObject(json, "hasLiveGlucose", status.has_live_glucose);
    if (status.has_live_glucose) {
        cJSON_AddNumberToObject(json, "liveGlucose", status.live_glucose_mgdl);
        cJSON_AddStringToObject(json, "liveDirection", status.live_direction);
    }
    return json;
}

static esp_err_t effect_lab_get_handler(httpd_req_t *request)
{
    return send_json(request, effect_lab_status_json());
}

static esp_err_t effect_lab_post_handler(httpd_req_t *request)
{
    cJSON *json = NULL;
    if (read_json_body(request, &json) != ESP_OK) {
        return send_api_error(request, "请求内容格式错误");
    }

    cJSON *active_json = cJSON_GetObjectItemCaseSensitive(json, "active");
    if (cJSON_IsFalse(active_json)) {
        led_controller_stop_preview();
        cJSON_Delete(json);
        return send_json(request, effect_lab_status_json());
    }

    cJSON *effect_json = cJSON_GetObjectItemCaseSensitive(json, "effect");
    cJSON *glucose_json = cJSON_GetObjectItemCaseSensitive(json, "glucose");
    cJSON *speed_json = cJSON_GetObjectItemCaseSensitive(json, "speed");
    cJSON *reverse_json = cJSON_GetObjectItemCaseSensitive(json, "reverse");
    if (!cJSON_IsNumber(effect_json) || !cJSON_IsNumber(glucose_json) ||
        !cJSON_IsNumber(speed_json) || !cJSON_IsBool(reverse_json)) {
        cJSON_Delete(json);
        return send_api_error(request, "灯效参数不完整");
    }

    int effect = effect_json->valueint;
    int glucose = glucose_json->valueint;
    int speed = speed_json->valueint;
    if (effect < 0 || effect >= LED_EFFECT_COUNT) {
        cJSON_Delete(json);
        return send_api_error(request, "未知的灯效");
    }
    if (glucose < 20 || glucose > 600) {
        cJSON_Delete(json);
        return send_api_error(request, "模拟血糖必须在 20 到 600 mg/dL 之间");
    }
    if (speed < 1 || speed > 100) {
        cJSON_Delete(json);
        return send_api_error(request, "速度必须在 1 到 100 之间");
    }

    led_controller_preview_effect((led_effect_t)effect, glucose, (uint8_t)speed,
                                  cJSON_IsTrue(reverse_json));
    cJSON_Delete(json);
    return send_json(request, effect_lab_status_json());
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

static esp_err_t save_handler(httpd_req_t *request)
{
    cJSON *json = NULL;
    if (read_json_body(request, &json) != ESP_OK) {
        return send_api_error(request, "请求内容格式错误");
    }
    cJSON *nonce = cJSON_GetObjectItemCaseSensitive(json, "nonce");
    bool valid_nonce = s_pending_valid && cJSON_IsString(nonce) &&
                       strcmp(nonce->valuestring, s_validation_nonce) == 0;
    cJSON_Delete(json);
    if (!valid_nonce) {
        return send_api_error(request, "配置已经变化，请重新验证");
    }

    esp_err_t err = app_config_save(&s_pending_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not save configuration: %s", esp_err_to_name(err));
        return send_api_error(request, "保存配置失败");
    }
    s_current_config = s_pending_config;
    s_pending_valid = false;

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "ok", true);
    cJSON_AddStringToObject(response, "message", "配置已保存，设备正在重启");
    esp_err_t response_err = send_json(request, response);
    xTaskCreate(restart_task, "restart", 2048, NULL, 4, NULL);
    return response_err;
}

static esp_err_t redirect_404(httpd_req_t *request, httpd_err_code_t error)
{
    (void)error;
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, NULL, 0);
}

esp_err_t portal_start(const app_config_t *current_config)
{
    if (current_config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_current_config = *current_config;
    s_pending_valid = false;

    ESP_ERROR_CHECK(dns_server_start());
    if (s_server != NULL) {
        ESP_LOGI(TAG, "Suger RGB configuration page selected at /");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;
    config.stack_size = 10240;
    config.lru_purge_enable = true;

    ESP_ERROR_CHECK(httpd_start(&s_server, &config));

    const httpd_uri_t index = {.uri = "/", .method = HTTP_GET, .handler = index_handler};
    const httpd_uri_t setup = {.uri = "/setup", .method = HTTP_GET, .handler = setup_page_handler};
    const httpd_uri_t effects = {.uri = "/effects", .method = HTTP_GET, .handler = effects_page_handler};
    const httpd_uri_t get_config = {.uri = "/api/config", .method = HTTP_GET, .handler = config_handler};
    const httpd_uri_t validate = {.uri = "/api/validate", .method = HTTP_POST, .handler = validate_handler};
    const httpd_uri_t save = {.uri = "/api/save", .method = HTTP_POST, .handler = save_handler};
    const httpd_uri_t effect_lab_get = {
        .uri = "/api/effects", .method = HTTP_GET, .handler = effect_lab_get_handler};
    const httpd_uri_t effect_lab_post = {
        .uri = "/api/effects", .method = HTTP_POST, .handler = effect_lab_post_handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &index));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &setup));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &effects));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &get_config));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &validate));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &save));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &effect_lab_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &effect_lab_post));
    ESP_ERROR_CHECK(httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, redirect_404));

    ESP_LOGI(TAG, "Suger RGB setup available at http://192.168.4.1/");
    return ESP_OK;
}

bool portal_is_running(void)
{
    return s_server != NULL;
}
