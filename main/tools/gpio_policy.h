#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* GPIO defaults for ESP32-S3-LCD-1.47B safe user-accessible pins */
#define MIMI_GPIO_MIN_PIN       1
#define MIMI_GPIO_MAX_PIN       21
#define MIMI_GPIO_ALLOWED_CSV   "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,21,38,46,48"

/**
 * Load the runtime allowlist override from NVS (falls back to MIMI_GPIO_ALLOWED_CSV).
 * Called once during tool initialization; safe to call multiple times.
 */
esp_err_t gpio_policy_init(void);

/**
 * Save a runtime allowlist to NVS. Pass an empty string to clear the override.
 * CSV format, e.g. "1,2,3,4,48".
 */
esp_err_t gpio_policy_set_allowlist(const char *csv);

/**
 * Copy the effective allowlist (runtime override or compile-time default) into buf.
 * Returns false if the effective list is empty.
 */
bool gpio_policy_get_allowlist(char *buf, size_t buf_len);

/**
 * Check if a pin is allowed for user GPIO operations.
 * Validates against the allowlist or default range, and blocks
 * pins reserved for flash/PSRAM on ESP32.
 */
bool gpio_policy_pin_is_allowed(int pin);

/**
 * Write a human-readable hint if the pin is forbidden for a known reason.
 * Returns true if a hint was written (and the caller should return the error).
 */
bool gpio_policy_pin_forbidden_hint(int pin, char *result, size_t result_len);
