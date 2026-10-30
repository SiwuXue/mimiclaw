#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Device table: lets the agent talk to hardware by name instead of by pin.
 * Table lives in /spiffs/devices.json.
 */
esp_err_t tool_device_init(void);

/**
 * input_json: {"name":"led","type":"ws2812","pin":48,"count":1}
 * Upserts the entry, then returns the whole table.
 */
esp_err_t tool_device_define_execute(const char *input_json, char *output, size_t output_size);

/**
 * List all defined devices.
 */
esp_err_t tool_device_list_execute(const char *input_json, char *output, size_t output_size);

/**
 * Run an action on a named device.
 * input_json: {"name":"led","action":"blink","interval_ms":500}
 */
esp_err_t tool_device_do_execute(const char *input_json, char *output, size_t output_size);
