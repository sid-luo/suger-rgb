#include "nightscout.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#define HTTP_RESPONSE_CAPACITY 4096
#define NIGHTSCOUT_ENTRIES_SUFFIX "/api/v1/entries/sgv.json?count=2"
#define NIGHTSCOUT_STATUS_SUFFIX  "/api/v1/status.json"
#define SERVER_TIME_REFRESH_US    (15LL * 60LL * 1000000LL)

typedef struct {
    char *data;
    size_t capacity;
    size_t length;
    bool overflow;
} http_response_t;

static const char *TAG = "nightscout";
static char s_server_time_base_url[APP_NIGHTSCOUT_URL_MAX_LEN + 1];
static int64_t s_server_epoch_at_sync_ms;
static int64_t s_server_sync_us;

static void set_error(char *output, size_t output_size, const char *message)
{
    if (output != NULL && output_size > 0) {
        strlcpy(output, message, output_size);
    }
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }
    http_response_t *response = event->user_data;
    if (response == NULL) {
        return ESP_OK;
    }
    if (response->data == NULL || response->capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t available = response->capacity - 1 - response->length;
    size_t copy_length = (size_t)event->data_len < available
                             ? (size_t)event->data_len
                             : available;
    if (copy_length > 0) {
        memcpy(response->data + response->length, event->data, copy_length);
        response->length += copy_length;
    }
    response->data[response->length] = '\0';
    if (copy_length < (size_t)event->data_len) {
        response->overflow = true;
    }
    return ESP_OK;
}

static esp_err_t https_get(const char *url, http_response_t *response, int *status,
                           char *error_message, size_t error_message_size)
{
    if (response == NULL || response->data == NULL || response->capacity < 2 ||
        status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    response->length = 0;
    response->overflow = false;
    response->data[0] = '\0';
    esp_http_client_config_t client_config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = response,
        .timeout_ms = 10000,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .max_redirection_count = 3,
    };

    esp_http_client_handle_t client = esp_http_client_init(&client_config);
    if (client == NULL) {
        set_error(error_message, error_message_size, "无法创建 HTTPS 请求");
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_perform(client);
    *status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTPS request failed: %s", esp_err_to_name(err));
        set_error(error_message, error_message_size,
                  err == ESP_ERR_HTTP_CONNECT ? "Nightscout 连接失败，请检查域名和 HTTPS 证书" :
                                                "Nightscout 请求超时或网络异常");
    }
    return err;
}

static esp_err_t check_http_status(int status, char *error_message,
                                   size_t error_message_size)
{
    if (status == 401 || status == 403) {
        set_error(error_message, error_message_size,
                  "这个 Nightscout 不允许匿名读取（HTTP 401/403）");
        return ESP_ERR_NOT_ALLOWED;
    }
    if (status == 404) {
        set_error(error_message, error_message_size, "地址可以访问，但没有找到 Nightscout API");
        return ESP_ERR_NOT_FOUND;
    }
    if (status < 200 || status >= 300) {
        if (error_message != NULL && error_message_size > 0) {
            snprintf(error_message, error_message_size, "Nightscout 返回 HTTP %d", status);
        }
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t nightscout_server_now_ms(const char *base_url, int64_t *server_now_ms,
                                          char *error_message, size_t error_message_size)
{
    int64_t monotonic_us = esp_timer_get_time();
    int64_t elapsed_us = monotonic_us - s_server_sync_us;
    if (s_server_epoch_at_sync_ms > 0 && elapsed_us >= 0 &&
        elapsed_us < SERVER_TIME_REFRESH_US &&
        strcmp(base_url, s_server_time_base_url) == 0) {
        *server_now_ms = s_server_epoch_at_sync_ms + elapsed_us / 1000;
        return ESP_OK;
    }

    char url[APP_NIGHTSCOUT_URL_MAX_LEN + sizeof(NIGHTSCOUT_STATUS_SUFFIX) + 4];
    int written = snprintf(url, sizeof(url), "%s%s", base_url, NIGHTSCOUT_STATUS_SUFFIX);
    if (written < 0 || written >= sizeof(url)) {
        set_error(error_message, error_message_size, "Nightscout 地址太长");
        return ESP_ERR_INVALID_SIZE;
    }

    char response_data[1024];
    http_response_t response = {
        .data = response_data,
        .capacity = sizeof(response_data),
    };
    int status = 0;
    esp_err_t err = https_get(url, &response, &status, error_message, error_message_size);
    if (err != ESP_OK) {
        return err;
    }
    err = check_http_status(status, error_message, error_message_size);
    if (err != ESP_OK) {
        return err;
    }

    // The official Nightscout v1 status response places serverTimeEpoch near
    // the start of the JSON object. The full status payload can be larger than
    // our buffer because it also contains settings, so parse this one field
    // directly from the captured prefix.
    const char *key = strstr(response.data, "\"serverTimeEpoch\"");
    const char *colon = key == NULL ? NULL : strchr(key, ':');
    if (colon == NULL) {
        set_error(error_message, error_message_size,
                  "Nightscout 状态接口没有返回 serverTimeEpoch");
        return ESP_ERR_INVALID_RESPONSE;
    }
    char *end = NULL;
    double parsed_epoch = strtod(colon + 1, &end);
    if (end == colon + 1 || parsed_epoch <= 0) {
        set_error(error_message, error_message_size,
                  "Nightscout 返回的服务器时间无效");
        return ESP_ERR_INVALID_RESPONSE;
    }

    int64_t epoch_ms = (int64_t)parsed_epoch;
    if (epoch_ms < 100000000000LL) {
        epoch_ms *= 1000;
    }
    if (epoch_ms < 100000000000LL) {
        set_error(error_message, error_message_size,
                  "Nightscout 返回的服务器时间无效");
        return ESP_ERR_INVALID_RESPONSE;
    }

    monotonic_us = esp_timer_get_time();
    strlcpy(s_server_time_base_url, base_url, sizeof(s_server_time_base_url));
    s_server_epoch_at_sync_ms = epoch_ms;
    s_server_sync_us = monotonic_us;
    *server_now_ms = epoch_ms;
    return ESP_OK;
}

esp_err_t nightscout_normalize_base_url(const char *input, char *output, size_t output_size,
                                        char *error_message, size_t error_message_size)
{
    if (input == NULL || output == NULL || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    while (isspace((unsigned char)*input)) {
        ++input;
    }
    size_t input_len = strlen(input);
    while (input_len > 0 && isspace((unsigned char)input[input_len - 1])) {
        --input_len;
    }
    if (input_len == 0 || input_len > APP_NIGHTSCOUT_URL_MAX_LEN) {
        set_error(error_message, error_message_size, "请输入 Nightscout 地址");
        return ESP_ERR_INVALID_ARG;
    }

    char work[APP_NIGHTSCOUT_URL_MAX_LEN + 16];
    if (input_len >= sizeof(work)) {
        set_error(error_message, error_message_size, "Nightscout 地址太长");
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(work, input, input_len);
    work[input_len] = '\0';

    if (strncasecmp(work, "http://", 7) == 0) {
        set_error(error_message, error_message_size, "当前版本只支持 HTTPS 地址");
        return ESP_ERR_NOT_SUPPORTED;
    }

    char canonical[APP_NIGHTSCOUT_URL_MAX_LEN + 1];
    const char *address_part = strncasecmp(work, "https://", 8) == 0 ? work + 8 : work;
    size_t address_length = strlen(address_part);
    if (address_length + 8 > APP_NIGHTSCOUT_URL_MAX_LEN) {
        set_error(error_message, error_message_size, "Nightscout 地址太长");
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(canonical, "https://", 8);
    memcpy(canonical + 8, address_part, address_length + 1);

    char *api_suffix = strstr(canonical + 8, "/api/v1");
    if (api_suffix != NULL) {
        *api_suffix = '\0';
    }

    if (strchr(canonical + 8, '?') != NULL || strchr(canonical + 8, '#') != NULL ||
        strchr(canonical + 8, '@') != NULL) {
        set_error(error_message, error_message_size, "地址中不能包含参数、锚点或账号信息");
        return ESP_ERR_INVALID_ARG;
    }
    for (const char *p = canonical + 8; *p != '\0'; ++p) {
        if (isspace((unsigned char)*p) || (unsigned char)*p < 0x20) {
            set_error(error_message, error_message_size, "Nightscout 地址格式不正确");
            return ESP_ERR_INVALID_ARG;
        }
    }

    char *host = canonical + 8;
    if (*host == '\0' || *host == '/') {
        set_error(error_message, error_message_size, "Nightscout 域名不能为空");
        return ESP_ERR_INVALID_ARG;
    }

    size_t length = strlen(canonical);
    while (length > 8 && canonical[length - 1] == '/') {
        canonical[--length] = '\0';
    }
    if (length + 1 > output_size) {
        set_error(error_message, error_message_size, "Nightscout 地址太长");
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(output, canonical, output_size);
    return ESP_OK;
}

esp_err_t nightscout_fetch_latest(const char *base_url, nightscout_reading_t *reading,
                                  char *error_message, size_t error_message_size)
{
    if (base_url == NULL || reading == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(reading, 0, sizeof(*reading));
    set_error(error_message, error_message_size, "Nightscout 请求失败");

    int64_t server_now_ms = 0;
    char status_error[160];
    esp_err_t time_err = nightscout_server_now_ms(base_url, &server_now_ms,
                                                  status_error, sizeof(status_error));
    reading->age_known = time_err == ESP_OK;
    if (time_err != ESP_OK) {
        // Reading glucose is still useful. The LED controller independently
        // detects a record that remains unchanged for ten minutes.
        ESP_LOGW(TAG, "Nightscout server time unavailable: %s", status_error);
    }

    char url[APP_NIGHTSCOUT_URL_MAX_LEN + sizeof(NIGHTSCOUT_ENTRIES_SUFFIX) + 4];
    int written = snprintf(url, sizeof(url), "%s%s", base_url, NIGHTSCOUT_ENTRIES_SUFFIX);
    if (written < 0 || written >= sizeof(url)) {
        set_error(error_message, error_message_size, "Nightscout 地址太长");
        return ESP_ERR_INVALID_SIZE;
    }

    char response_data[HTTP_RESPONSE_CAPACITY];
    http_response_t response = {
        .data = response_data,
        .capacity = sizeof(response_data),
    };
    int status = 0;
    esp_err_t err = https_get(url, &response, &status,
                              error_message, error_message_size);
    if (err != ESP_OK) {
        return err;
    }
    err = check_http_status(status, error_message, error_message_size);
    if (err != ESP_OK) {
        return err;
    }
    if (response.overflow) {
        set_error(error_message, error_message_size, "Nightscout 返回的数据过大");
        return ESP_ERR_INVALID_SIZE;
    }

    cJSON *root = cJSON_Parse(response.data);
    if (root == NULL || !cJSON_IsArray(root)) {
        cJSON_Delete(root);
        set_error(error_message, error_message_size, "返回内容不是有效的 Nightscout 数据");
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *entry = cJSON_GetArrayItem(root, 0);
    if (entry == NULL) {
        cJSON_Delete(root);
        set_error(error_message, error_message_size, "Nightscout 暂时没有血糖数据");
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *sgv = cJSON_GetObjectItemCaseSensitive(entry, "sgv");
    cJSON *date = cJSON_GetObjectItemCaseSensitive(entry, "date");
    cJSON *direction = cJSON_GetObjectItemCaseSensitive(entry, "direction");
    if (!cJSON_IsNumber(sgv) || !cJSON_IsNumber(date) || sgv->valuedouble <= 0) {
        cJSON_Delete(root);
        set_error(error_message, error_message_size, "血糖数据缺少有效的 sgv 或 date 字段");
        return ESP_ERR_INVALID_RESPONSE;
    }

    reading->glucose_mgdl = (int)(sgv->valuedouble + 0.5);
    reading->timestamp_ms = (int64_t)date->valuedouble;
    if (reading->timestamp_ms > 0 && reading->timestamp_ms < 100000000000LL) {
        // Be tolerant of non-standard instances returning Unix seconds.
        reading->timestamp_ms *= 1000;
    }
    if (reading->timestamp_ms <= 0) {
        cJSON_Delete(root);
        set_error(error_message, error_message_size, "Nightscout 数据时间无效");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (cJSON_IsString(direction) && direction->valuestring != NULL) {
        strlcpy(reading->direction, direction->valuestring, sizeof(reading->direction));
    } else {
        strlcpy(reading->direction, "Unknown", sizeof(reading->direction));
    }
    cJSON_Delete(root);

    if (reading->age_known) {
        int64_t age_ms = server_now_ms - reading->timestamp_ms;
        if (age_ms < -5LL * 60LL * 1000LL) {
            set_error(error_message, error_message_size,
                      "血糖记录时间晚于 Nightscout 服务器时间");
            return ESP_ERR_INVALID_RESPONSE;
        }
        reading->age_minutes = age_ms <= 0 ? 0 : (int)(age_ms / 60000);
        reading->stale = age_ms >= 10LL * 60LL * 1000LL;
    }
    set_error(error_message, error_message_size, "");
    return ESP_OK;
}
