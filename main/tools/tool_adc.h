#pragma once

#include "esp_err.h"
#include <stddef.h>

esp_err_t tool_adc_init(void);

/**
 * Tool entry point.
 * input_json: {"op":"read","pin":4,"samples":1}
 */
esp_err_t tool_adc_execute(const char *input_json, char *output, size_t output_size);
