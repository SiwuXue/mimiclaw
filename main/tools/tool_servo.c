#include "tools/tool_servo.h"
#include "tools/gpio_policy.h"
#include "mimi_config.h"

#include "driver/ledc.h"
#include "esp_log.h"
#include "nvs.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "servo";

#define SERVO_FREQ_HZ       50          /* standard hobby servo: 20 ms period */
#define SERVO_PERIOD_US     20000
#define SERVO_TIMER         LEDC_TIMER_0
#define SERVO_SPEED_MODE    LEDC_LOW_SPEED_MODE
#define SERVO_RES_BITS      LEDC_TIMER_14_BIT
#define SERVO_MAX_CHANNELS  4
#define SERVO_DEFAULT_MIN_US 500
#define SERVO_DEFAULT_MAX_US 2500

typedef struct {
    int gpio;
    ledc_channel_t channel;
} servo_slot_t;

static servo_slot_t s_slots[SERVO_MAX_CHANNELS];
static int s_slot_count = 0;
static bool s_timer_ready = false;

/* Defaults, overridable through NVS (set_servo CLI) */
static int s_cfg_gpio = 0;
static int s_cfg_min_us = SERVO_DEFAULT_MIN_US;
static int s_cfg_max_us = SERVO_DEFAULT_MAX_US;

static void servo_load_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_HW, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    uint32_t value = 0;
    if (nvs_get_u32(nvs, MIMI_NVS_KEY_SERVO_GPIO, &value) == ESP_OK) {
        s_cfg_gpio = (int)value;
    }
    if (nvs_get_u32(nvs, MIMI_NVS_KEY_SERVO_MIN_US, &value) == ESP_OK && value > 0) {
        s_cfg_min_us = (int)value;
    }
    if (nvs_get_u32(nvs, MIMI_NVS_KEY_SERVO_MAX_US, &value) == ESP_OK && value > 0) {
        s_cfg_max_us = (int)value;
    }
    nvs_close(nvs);
}

esp_err_t servo_set_config(int gpio, int min_us, int max_us)
{
    if (!gpio_policy_pin_is_allowed(gpio)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (min_us <= 0) min_us = SERVO_DEFAULT_MIN_US;
    if (max_us <= 0) max_us = SERVO_DEFAULT_MAX_US;
    if (min_us >= max_us || max_us > SERVO_PERIOD_US) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_HW, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u32(nvs, MIMI_NVS_KEY_SERVO_GPIO, (uint32_t)gpio);
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, MIMI_NVS_KEY_SERVO_MIN_US, (uint32_t)min_us);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, MIMI_NVS_KEY_SERVO_MAX_US, (uint32_t)max_us);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err == ESP_OK) {
        s_cfg_gpio = gpio;
        s_cfg_min_us = min_us;
        s_cfg_max_us = max_us;
    }
    return err;
}

void servo_get_config(int *gpio, int *min_us, int *max_us)
{
    if (gpio) *gpio = s_cfg_gpio;
    if (min_us) *min_us = s_cfg_min_us;
    if (max_us) *max_us = s_cfg_max_us;
}

static esp_err_t servo_timer_ensure(void)
{
    if (s_timer_ready) {
        return ESP_OK;
    }

    ledc_timer_config_t timer = {
        .speed_mode = SERVO_SPEED_MODE,
        .duty_resolution = SERVO_RES_BITS,
        .timer_num = SERVO_TIMER,
        .freq_hz = SERVO_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err == ESP_OK) {
        s_timer_ready = true;
    }
    return err;
}

static ledc_channel_t servo_acquire_channel(int gpio)
{
    for (int i = 0; i < s_slot_count; i++) {
        if (s_slots[i].gpio == gpio) {
            return s_slots[i].channel;
        }
    }
    if (s_slot_count >= SERVO_MAX_CHANNELS) {
        return LEDC_CHANNEL_MAX;
    }

    ledc_channel_t channel = (ledc_channel_t)(LEDC_CHANNEL_0 + s_slot_count);
    ledc_channel_config_t cfg = {
        .gpio_num = gpio,
        .speed_mode = SERVO_SPEED_MODE,
        .channel = channel,
        .timer_sel = SERVO_TIMER,
        .duty = 0,
        .hpoint = 0,
        .intr_type = LEDC_INTR_DISABLE,
    };
    if (ledc_channel_config(&cfg) != ESP_OK) {
        return LEDC_CHANNEL_MAX;
    }

    s_slots[s_slot_count].gpio = gpio;
    s_slots[s_slot_count].channel = channel;
    s_slot_count++;
    ESP_LOGI(TAG, "Servo attached to GPIO%d on LEDC channel %d", gpio, (int)channel);
    return channel;
}

esp_err_t servo_set_angle(int gpio, int angle, char *output, size_t output_size)
{
    if (gpio < 0) {
        gpio = s_cfg_gpio;
    }
    if (gpio <= 0) {
        snprintf(output, output_size,
                 "Error: no servo pin configured. Use CLI: set_servo <gpio> [min_us] [max_us]");
        return ESP_ERR_INVALID_STATE;
    }
    if (!gpio_policy_pin_is_allowed(gpio)) {
        if (gpio_policy_pin_forbidden_hint(gpio, output, output_size)) {
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(output, output_size, "Error: pin %d is not allowed for servo output", gpio);
        return ESP_ERR_INVALID_ARG;
    }
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;

    esp_err_t err = servo_timer_ensure();
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to init LEDC timer (%d)", (int)err);
        return ESP_FAIL;
    }

    ledc_channel_t channel = servo_acquire_channel(gpio);
    if (channel == LEDC_CHANNEL_MAX) {
        snprintf(output, output_size, "Error: no free LEDC channel (max %d servos)", SERVO_MAX_CHANNELS);
        return ESP_FAIL;
    }

    int pulse_us = s_cfg_min_us + (s_cfg_max_us - s_cfg_min_us) * angle / 180;
    uint32_t max_duty = (1u << 14) - 1;
    uint32_t duty = (uint32_t)pulse_us * ((1u << 14) - 1) / SERVO_PERIOD_US;
    if (duty > max_duty) {
        duty = max_duty;
    }

    err = ledc_set_duty(SERVO_SPEED_MODE, channel, duty);
    if (err == ESP_OK) {
        err = ledc_update_duty(SERVO_SPEED_MODE, channel);
    }
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to set servo duty (%d)", (int)err);
        return ESP_FAIL;
    }

    snprintf(output, output_size, "Servo on GPIO%d set to %d deg (%d us pulse)",
             gpio, angle, pulse_us);
    ESP_LOGI(TAG, "gpio=%d angle=%d pulse=%dus duty=%u", gpio, angle, pulse_us, (unsigned)duty);
    return ESP_OK;
}

esp_err_t tool_servo_set_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *angle_obj = cJSON_GetObjectItem(root, "angle");
    if (!cJSON_IsNumber(angle_obj)) {
        snprintf(output, output_size, "Error: 'angle' required (0-180 degrees)");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    int angle = (int)angle_obj->valuedouble;
    int gpio = -1;
    cJSON *pin_obj = cJSON_GetObjectItem(root, "gpio");
    if (cJSON_IsNumber(pin_obj)) {
        gpio = (int)pin_obj->valuedouble;
    }

    esp_err_t err = servo_set_angle(gpio, angle, output, output_size);
    cJSON_Delete(root);
    return err;
}

esp_err_t tool_servo_init(void)
{
    servo_load_config();
    if (s_cfg_gpio > 0) {
        ESP_LOGI(TAG, "Servo tool initialized (GPIO%d, %d-%d us)",
                 s_cfg_gpio, s_cfg_min_us, s_cfg_max_us);
    } else {
        ESP_LOGW(TAG, "Servo tool initialized, no pin configured (use CLI: set_servo <gpio>)");
    }
    return ESP_OK;
}
