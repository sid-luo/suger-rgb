#pragma once

#include "app_config.h"
#include "esp_err.h"

/**
 * Start the captive configuration HTTP server.
 *
 * Normal operation intentionally does not expose an HTTP service. This must
 * only be called after a physical BOOT long-press enters configuration mode.
 */
esp_err_t portal_start(const app_config_t *current_config);
bool portal_is_running(void);
