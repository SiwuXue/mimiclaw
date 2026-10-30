#pragma once

#include "esp_err.h"
#include <stddef.h>

esp_err_t tool_uart_init(void);

/**
 * Tool entry point.
 * input_json: {"op":"setup","port":1,"tx":17,"rx":18,"baud":9600}
 * input_json: {"op":"write","port":1,"data":"AT+NAME\\r\\n"}
 * input_json: {"op":"read","port":1,"timeout_ms":500,"len":64}
 */
esp_err_t tool_uart_execute(const char *input_json, char *output, size_t output_size);
