#pragma once

#include "esp_err.h"
#include <stddef.h>

esp_err_t tool_spi_init(void);

/**
 * Tool entry point.
 * input_json: {"op":"transfer","mosi":11,"miso":13,"sclk":12,"cs":10,"mode":0,
 *              "freq":1000000,"tx":"01 02 03","bits":24}
 */
esp_err_t tool_spi_execute(const char *input_json, char *output, size_t output_size);
