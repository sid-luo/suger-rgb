#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"
#include "esp_err.h"

esp_err_t network_manager_init(void);
esp_err_t network_manager_start_normal(const app_config_t *config);
esp_err_t network_manager_start_config_ap(void);

/** Connect using candidate credentials without storing them in flash. */
esp_err_t network_manager_connect_candidate(const char *ssid, const char *password,
                                            uint32_t timeout_ms,
                                            char *error_message, size_t error_message_size);

bool network_manager_is_connected(void);
esp_err_t network_manager_wait_connected(uint32_t timeout_ms);
const char *network_manager_ap_ssid(void);
