#include "tools/tool_adc.h"
#include "tools/gpio_policy.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "adc";

static adc_oneshot_unit_handle_t s_units[ADC_UNIT_2 + 1];
static bool s_chan_cfg[2][10];   /* channel configured per unit */
static adc_atten_t s_atten = ADC_ATTEN_DB_12;

esp_err_t tool_adc_init(void)
{
    memset(s_units, 0, sizeof(s_units));
    memset(s_chan_cfg, 0, sizeof(s_chan_cfg));
    return ESP_OK;
}

static adc_oneshot_unit_handle_t adc_unit_handle(adc_unit_t unit, char *out, size_t out_size)
{
    int idx = (unit == ADC_UNIT_1) ? 0 : 1;
    if (s_units[unit]) {
        return s_units[unit];
    }

    adc_oneshot_unit_init_cfg_t cfg = {
        .unit_id = unit,
    };
    if (adc_oneshot_new_unit(&cfg, &s_units[unit]) != ESP_OK) {
        snprintf(out, out_size, "Error: failed to init ADC unit %d", (int)unit);
        s_units[unit] = NULL;
        return NULL;
    }
    (void)idx;
    ESP_LOGI(TAG, "ADC unit %d ready", (int)unit);
    return s_units[unit];
}

static bool adc_channel_ensure(adc_oneshot_unit_handle_t handle, adc_unit_t unit,
                               adc_channel_t chan, char *out, size_t out_size)
{
    int u = (unit == ADC_UNIT_1) ? 0 : 1;
    int c = (int)chan;
    if (c < 0 || c > 9) {
        snprintf(out, out_size, "Error: invalid ADC channel");
        return false;
    }
    if (s_chan_cfg[u][c]) {
        return true;
    }
    adc_oneshot_chan_cfg_t cfg = {
        .atten = s_atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(handle, chan, &cfg) != ESP_OK) {
        snprintf(out, out_size, "Error: failed to configure ADC channel");
        return false;
    }
    s_chan_cfg[u][c] = true;
    return true;
}

esp_err_t tool_adc_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    int pin = 4;
    int samples = 1;
    cJSON *obj;
    if (cJSON_IsNumber(obj = cJSON_GetObjectItem(root, "pin"))) pin = (int)obj->valuedouble;
    if (cJSON_IsNumber(obj = cJSON_GetObjectItem(root, "samples"))) samples = (int)obj->valuedouble;
    if (samples < 1) samples = 1;
    if (samples > 32) samples = 32;

    if (!gpio_policy_pin_is_allowed(pin)) {
        if (!gpio_policy_pin_forbidden_hint(pin, output, output_size)) {
            snprintf(output, output_size, "Error: pin %d is not allowed (see set_gpio_allow)", pin);
        }
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    adc_unit_t unit;
    adc_channel_t chan;
    if (adc_oneshot_io_to_channel(pin, &unit, &chan) != ESP_OK) {
        snprintf(output, output_size,
                 "Error: GPIO%d is not an ADC pin (ESP32-S3 ADC1: GPIO1-10, ADC2: GPIO11-14)",
                 pin);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    adc_oneshot_unit_handle_t handle = adc_unit_handle(unit, output, output_size);
    if (!handle || !adc_channel_ensure(handle, unit, chan, output, output_size)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    int raw = 0;
    long total = 0;
    for (int i = 0; i < samples; i++) {
        if (adc_oneshot_read(handle, chan, &raw) != ESP_OK) {
            snprintf(output, output_size, "Error: ADC read failed on GPIO%d", pin);
            cJSON_Delete(root);
            return ESP_FAIL;
        }
        total += raw;
    }
    int avg = (int)(total / samples);
    int mv = (int)((long)avg * 3300 / 4095);

    snprintf(output, output_size, "GPIO%d ADC raw=%d (~%d mV, %d sample(s), 12dB atten)",
             pin, avg, mv, samples);
    ESP_LOGI(TAG, "adc: pin=%d raw=%d mv=%d", pin, avg, mv);

    cJSON_Delete(root);
    return ESP_OK;
}
