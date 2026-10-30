#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Initialize the WS2812 (addressable RGB LED) tool.
 */
esp_err_t tool_ws2812_init(void);

/**
 * Save the default data pin / LED count to NVS so every call uses it.
 */
esp_err_t ws2812_set_config(int gpio, int count);

/**
 * Read the configured default data pin / LED count.
 */
void ws2812_get_config(int *gpio, int *count);

/**
 * Tool entry point (called by the agent loop through the tool registry).
 * input_json: {"gpio":48,"count":1,"color":"#ffffff","brightness":255}
 */
esp_err_t tool_ws2812_set_execute(const char *input_json, char *output, size_t output_size);

/**
 * Tool entry point: start a background on/off blink.
 * input_json: {"gpio":48,"count":1,"color":"white","brightness":255,"interval_ms":500,"cycles":0}
 * interval_ms is the on time and the off time (each). cycles=0 means blink forever.
 */
esp_err_t tool_ws2812_blink_execute(const char *input_json, char *output, size_t output_size);

/**
 * Tool entry point: stop the background blink and turn the LEDs off.
 */
esp_err_t tool_ws2812_stop_execute(const char *input_json, char *output, size_t output_size);

/**
 * Stop the background blink task (also called when a fixed color is set).
 */
void ws2812_blink_stop(void);

/**
 * Start a background blink task directly (used by the tool and the serial CLI).
 */
esp_err_t ws2812_blink_start(int gpio, int count, const char *color, int brightness,
                             int interval_ms, int cycles,
                             char *output, size_t output_size);

/**
 * Direct helper (also used by the serial CLI for quick testing).
 * color accepts "#RRGGBB"/"RRGGBB" or a name such as white/red/green/blue/off.
 */
esp_err_t ws2812_set_pixels(int gpio, int count, const char *color, int brightness,
                            char *output, size_t output_size);
