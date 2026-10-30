#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Initialize the I2C primitive tool (no bus is opened until first use).
 */
esp_err_t tool_i2c_init(void);

/**
 * Tool entry point.
 * input_json: {"op":"scan","port":0,"sda":8,"scl":9,"freq":100000,"addr":118,"reg":15,"data":"01 02","len":4}
 * ops: scan | read | write
 */
esp_err_t tool_i2c_execute(const char *input_json, char *output, size_t output_size);
