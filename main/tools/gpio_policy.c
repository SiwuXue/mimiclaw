#include "tools/gpio_policy.h"
#include "mimi_config.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "gpio_policy";

#define GPIO_ALLOW_MAX_LEN 160

static char s_allow_csv[GPIO_ALLOW_MAX_LEN] = {0};
static bool s_loaded = false;

static void policy_load(void)
{
    if (s_loaded) {
        return;
    }
    s_loaded = true;

    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_HW, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    char tmp[GPIO_ALLOW_MAX_LEN] = {0};
    size_t len = sizeof(tmp);
    if (nvs_get_str(nvs, MIMI_NVS_KEY_GPIO_ALLOW, tmp, &len) == ESP_OK) {
        strncpy(s_allow_csv, tmp, sizeof(s_allow_csv) - 1);
        s_allow_csv[sizeof(s_allow_csv) - 1] = '\0';
        if (s_allow_csv[0]) {
            ESP_LOGI(TAG, "Runtime GPIO allowlist: %s", s_allow_csv);
        }
    }
    nvs_close(nvs);
}

esp_err_t gpio_policy_init(void)
{
    s_loaded = false;
    policy_load();
    return ESP_OK;
}

esp_err_t gpio_policy_set_allowlist(const char *csv)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_HW, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    if (csv && csv[0] != '\0') {
        err = nvs_set_str(nvs, MIMI_NVS_KEY_GPIO_ALLOW, csv);
        if (err == ESP_OK) {
            strncpy(s_allow_csv, csv, sizeof(s_allow_csv) - 1);
            s_allow_csv[sizeof(s_allow_csv) - 1] = '\0';
        }
    } else {
        nvs_erase_key(nvs, MIMI_NVS_KEY_GPIO_ALLOW);
        s_allow_csv[0] = '\0';
        err = ESP_OK;
    }

    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    s_loaded = true;
    return err;
}

bool gpio_policy_get_allowlist(char *buf, size_t buf_len)
{
    policy_load();
    const char *effective = s_allow_csv[0] ? s_allow_csv : MIMI_GPIO_ALLOWED_CSV;
    if (!buf || buf_len == 0) {
        return false;
    }
    strncpy(buf, effective, buf_len - 1);
    buf[buf_len - 1] = '\0';
    return effective[0] != '\0';
}

#ifndef GPIO_IS_VALID_GPIO
#define GPIO_IS_VALID_GPIO(pin) ((pin) >= 0)
#endif

static bool pin_in_allowlist(int pin, const char *csv)
{
    const char *cursor;

    if (!csv || csv[0] == '\0') {
        return false;
    }

    cursor = csv;
    while (*cursor != '\0') {
        char *endptr = NULL;
        long value;

        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }

        value = strtol(cursor, &endptr, 10);
        if (endptr == cursor) {
            while (*cursor != '\0' && *cursor != ',') {
                cursor++;
            }
            continue;
        }

        if ((int)value == pin) {
            return true;
        }
        cursor = endptr;
    }

    return false;
}

static bool pin_is_allowed_impl(int pin,
                                const char *allowlist_csv,
                                int min_pin,
                                int max_pin,
                                bool block_esp32_flash_pins,
                                bool block_esp32s3_usb_pins)
{
    bool in_policy;

    if (pin < 0) {
        return false;
    }

    /* Block ESP32 flash/PSRAM pins (GPIO 6-11) */
    if (block_esp32_flash_pins && pin >= 6 && pin <= 11) {
        return false;
    }

    /* USB Serial/JTAG uses GPIO19/20 on ESP32-S3 */
    if (block_esp32s3_usb_pins && (pin == 19 || pin == 20)) {
        return false;
    }

    if (allowlist_csv && allowlist_csv[0] != '\0') {
        in_policy = pin_in_allowlist(pin, allowlist_csv);
    } else {
        in_policy = pin >= min_pin && pin <= max_pin;
    }

    if (!in_policy) {
        return false;
    }

    return GPIO_IS_VALID_GPIO((gpio_num_t)pin);
}

bool gpio_policy_pin_is_allowed(int pin)
{
    const char *csv;

    policy_load();
    csv = s_allow_csv[0] ? s_allow_csv : MIMI_GPIO_ALLOWED_CSV;

#if defined(CONFIG_IDF_TARGET_ESP32)
    return pin_is_allowed_impl(pin, csv,
                               MIMI_GPIO_MIN_PIN, MIMI_GPIO_MAX_PIN, true, false);
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
    return pin_is_allowed_impl(pin, csv,
                               MIMI_GPIO_MIN_PIN, MIMI_GPIO_MAX_PIN, false, true);
#else
    return pin_is_allowed_impl(pin, csv,
                               MIMI_GPIO_MIN_PIN, MIMI_GPIO_MAX_PIN, false, false);
#endif
}

bool gpio_policy_pin_forbidden_hint(int pin, char *result, size_t result_len)
{
#if defined(CONFIG_IDF_TARGET_ESP32)
    if (pin >= 6 && pin <= 11) {
        snprintf(result, result_len,
                 "Error: pin %d is reserved for ESP32 flash/PSRAM (GPIO6-11); choose a different pin",
                 pin);
        return true;
    }
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
    if (pin == 19 || pin == 20) {
        snprintf(result, result_len,
                 "Error: pin %d is reserved for ESP32-S3 USB Serial/JTAG (GPIO19/20); choose a different pin",
                 pin);
        return true;
    }
#else
    (void)pin;
    (void)result;
    (void)result_len;
#endif

    return false;
}
