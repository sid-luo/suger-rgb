#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    int glucose_mgdl;
    int64_t timestamp_ms;
    int age_minutes;
    bool age_known;
    bool stale;
    char direction[24];
} nightscout_reading_t;

esp_err_t nightscout_normalize_base_url(const char *input, char *output, size_t output_size,
                                        char *error_message, size_t error_message_size);

esp_err_t nightscout_fetch_latest(const char *base_url, nightscout_reading_t *reading,
                                  char *error_message, size_t error_message_size);
