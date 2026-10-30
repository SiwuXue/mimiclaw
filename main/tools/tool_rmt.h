#pragma once

#include "esp_err.h"
#include <stddef.h>

esp_err_t tool_rmt_init(void);

/**
 * Generic RMT primitive.
 * input_json (pulse):   {"op":"pulse","gpio":4,"symbols":[{"level":1,"us":500},{"level":0,"us":500}]}
 * input_json (capture): {"op":"capture","gpio":4,"idle_us":5000,"max_symbols":64,"timeout_ms":200}
 */
esp_err_t tool_rmt_execute(const char *input_json, char *output, size_t output_size);
