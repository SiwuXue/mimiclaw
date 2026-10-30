#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Initialize the servo (LEDC PWM) tool.
 */
esp_err_t tool_servo_init(void);

/**
 * Save the default servo pin and pulse width range to NVS.
 */
esp_err_t servo_set_config(int gpio, int min_us, int max_us);

/**
 * Read the configured servo pin and pulse width range.
 * gpio is 0 when no servo has been configured yet.
 */
void servo_get_config(int *gpio, int *min_us, int *max_us);

/**
 * Tool entry point. input_json: {"angle":90,"gpio":18}
 */
esp_err_t tool_servo_set_execute(const char *input_json, char *output, size_t output_size);

/**
 * Direct helper (also used by the serial CLI).
 * angle is 0-180 degrees; gpio < 0 means "use the configured pin".
 */
esp_err_t servo_set_angle(int gpio, int angle, char *output, size_t output_size);
