#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Initialize the WS2812 (addressable RGB LED) tool.
 */
esp_err_t tool_ws2812_init(void);

/**
 * Tool entry point (called by the agent loop through the tool registry).
 * input_json: {"gpio":48,"count":1,"color":"#ffffff","brightness":255}
 */
esp_err_t tool_ws2812_set_execute(const char *input_json, char *output, size_t output_size);

/**
 * Direct helper (also used by the serial CLI for quick testing).
 * color accepts "#RRGGBB"/"RRGGBB" or a name such as white/red/green/blue/off.
 */
esp_err_t ws2812_set_pixels(int gpio, int count, const char *color, int brightness,
                            char *output, size_t output_size);
