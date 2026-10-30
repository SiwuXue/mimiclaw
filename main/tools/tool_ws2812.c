#include "tools/tool_ws2812.h"
#include "tools/gpio_policy.h"
#include "mimi_config.h"

#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "ws2812";

/* RMT resolution 10 MHz -> 1 tick = 0.1 us */
#define WS2812_RESOLUTION_HZ   10000000
#define WS2812_T0H_TICKS       4      /* 0.40 us */
#define WS2812_T0L_TICKS       8      /* 0.80 us */
#define WS2812_T1H_TICKS       8      /* 0.80 us */
#define WS2812_T1L_TICKS       4      /* 0.40 us */
#define WS2812_RESET_TICKS     800    /* 80 us low = latch */
#define WS2812_MAX_PIXELS      64

static rmt_channel_handle_t s_chan = NULL;
static rmt_encoder_handle_t s_copy_encoder = NULL;
static int s_chan_gpio = -1;

/* Reused bitstream buffer (PSRAM) so blinking does not churn the heap */
static rmt_symbol_word_t *s_symbols = NULL;
static size_t s_symbols_cap = 0;

/* Serializes RMT access between the tool calls and the blink task */
static SemaphoreHandle_t s_rmt_lock = NULL;

/* Background blink task state */
static TaskHandle_t s_blink_task = NULL;
static volatile bool s_blink_active = false;
static int s_blink_gpio = 48;
static int s_blink_count = 1;
static int s_blink_interval_ms = 500;   /* on time and off time, each */
static int s_blink_cycles = 0;      /* 0 = forever */
static uint8_t s_blink_r = 255;
static uint8_t s_blink_g = 255;
static uint8_t s_blink_b = 255;

/* Defaults, overridable through NVS (set_led / set_led in CLI) */
static int s_cfg_gpio = 48;
static int s_cfg_count = 1;

static void ws2812_load_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_HW, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    uint32_t value = 0;
    if (nvs_get_u32(nvs, MIMI_NVS_KEY_LED_GPIO, &value) == ESP_OK && value > 0) {
        s_cfg_gpio = (int)value;
    }
    if (nvs_get_u32(nvs, MIMI_NVS_KEY_LED_COUNT, &value) == ESP_OK && value > 0) {
        s_cfg_count = (int)value;
    }
    nvs_close(nvs);
}

esp_err_t ws2812_set_config(int gpio, int count)
{
    if (!gpio_policy_pin_is_allowed(gpio)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (count < 1 || count > WS2812_MAX_PIXELS) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_HW, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u32(nvs, MIMI_NVS_KEY_LED_GPIO, (uint32_t)gpio);
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, MIMI_NVS_KEY_LED_COUNT, (uint32_t)count);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err == ESP_OK) {
        s_cfg_gpio = gpio;
        s_cfg_count = count;
    }
    return err;
}

void ws2812_get_config(int *gpio, int *count)
{
    if (gpio) *gpio = s_cfg_gpio;
    if (count) *count = s_cfg_count;
}

static void ws2812_close(void)
{
    if (s_chan) {
        rmt_disable(s_chan);
        rmt_del_channel(s_chan);
        s_chan = NULL;
    }
    if (s_copy_encoder) {
        rmt_del_encoder(s_copy_encoder);
        s_copy_encoder = NULL;
    }
    s_chan_gpio = -1;
}

static bool ws2812_open(int gpio)
{
    if (s_chan && s_chan_gpio == gpio) {
        return true;
    }

    ws2812_close();

    rmt_tx_channel_config_t tx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = gpio,
        .mem_block_symbols = 64,
        .resolution_hz = WS2812_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    if (rmt_new_tx_channel(&tx_cfg, &s_chan) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed on GPIO%d", gpio);
        s_chan = NULL;
        return false;
    }

    rmt_copy_encoder_config_t enc_cfg = {};
    if (rmt_new_copy_encoder(&enc_cfg, &s_copy_encoder) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_copy_encoder failed");
        ws2812_close();
        return false;
    }

    if (rmt_enable(s_chan) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed");
        ws2812_close();
        return false;
    }

    s_chan_gpio = gpio;
    ESP_LOGI(TAG, "RMT channel ready on GPIO%d", gpio);
    return true;
}

static bool parse_color(const char *spec, uint8_t *r, uint8_t *g, uint8_t *b);

static bool ensure_symbols(int count)
{
    size_t need = (size_t)count * 24 + 1;
    if (s_symbols && s_symbols_cap >= need) {
        return true;
    }
    if (s_symbols) {
        heap_caps_free(s_symbols);
        s_symbols = NULL;
        s_symbols_cap = 0;
    }

    s_symbols = heap_caps_calloc(need, sizeof(rmt_symbol_word_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_symbols) {
        return false;
    }
    s_symbols_cap = need;
    return true;
}

/* Push one solid-color frame to the LED chain. Caller must NOT hold s_rmt_lock. */
static esp_err_t ws2812_transmit(int gpio, int count, uint8_t r, uint8_t g, uint8_t b,
                                 char *output, size_t output_size)
{
    if (!s_rmt_lock) {
        s_rmt_lock = xSemaphoreCreateMutex();
    }
    if (s_rmt_lock && xSemaphoreTake(s_rmt_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        snprintf(output, output_size, "Error: WS2812 bus busy");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;

    if (!ws2812_open(gpio)) {
        snprintf(output, output_size, "Error: failed to init RMT on GPIO%d", gpio);
        err = ESP_FAIL;
        goto done;
    }
    if (!ensure_symbols(count)) {
        snprintf(output, output_size, "Error: out of memory");
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    size_t idx = 0;
    for (int px = 0; px < count; px++) {
        /* WS2812 expects GRB order, MSB first */
        uint32_t grb = ((uint32_t)g << 16) | ((uint32_t)r << 8) | (uint32_t)b;
        for (int bit = 23; bit >= 0; bit--) {
            bool one = ((grb >> bit) & 0x1) != 0;
            s_symbols[idx].level0 = 1;
            s_symbols[idx].duration0 = one ? WS2812_T1H_TICKS : WS2812_T0H_TICKS;
            s_symbols[idx].level1 = 0;
            s_symbols[idx].duration1 = one ? WS2812_T1L_TICKS : WS2812_T0L_TICKS;
            idx++;
        }
    }
    s_symbols[idx].level0 = 0;
    s_symbols[idx].duration0 = WS2812_RESET_TICKS;
    s_symbols[idx].level1 = 0;
    s_symbols[idx].duration1 = 0;
    idx++;

    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
    };
    err = rmt_transmit(s_chan, s_copy_encoder, s_symbols,
                       idx * sizeof(rmt_symbol_word_t), &tx_cfg);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(s_chan, 100);
    }
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: RMT transmit failed (%d)", (int)err);
        err = ESP_FAIL;
    }

done:
    if (s_rmt_lock) {
        xSemaphoreGive(s_rmt_lock);
    }
    return err;
}

static void ws2812_blink_task(void *arg)
{
    (void)arg;
    int cycles = 0;

    while (s_blink_active && (s_blink_cycles == 0 || cycles < s_blink_cycles)) {
        char msg[64];

        ws2812_transmit(s_blink_gpio, s_blink_count, s_blink_r, s_blink_g, s_blink_b,
                        msg, sizeof(msg));

        /* Sleep in slices so ws2812_blink_stop() reacts quickly */
        int remain = s_blink_interval_ms;
        while (remain > 0 && s_blink_active) {
            int step = remain > 50 ? 50 : remain;
            vTaskDelay(pdMS_TO_TICKS(step));
            remain -= step;
        }
        if (!s_blink_active) {
            break;
        }

        ws2812_transmit(s_blink_gpio, s_blink_count, 0, 0, 0, msg, sizeof(msg));

        remain = s_blink_interval_ms;
        while (remain > 0 && s_blink_active) {
            int step = remain > 50 ? 50 : remain;
            vTaskDelay(pdMS_TO_TICKS(step));
            remain -= step;
        }
        cycles++;
    }

    /* Leave the LED off when the blink finishes */
    char msg[64];
    ws2812_transmit(s_blink_gpio, s_blink_count, 0, 0, 0, msg, sizeof(msg));
    ESP_LOGI(TAG, "Blink task finished after %d cycle(s)", cycles);

    s_blink_task = NULL;
    vTaskDelete(NULL);
}

void ws2812_blink_stop(void)
{
    if (!s_blink_active && !s_blink_task) {
        return;
    }
    s_blink_active = false;

    /* The task wakes up at least every 50 ms and turns the LED off on exit */
    for (int i = 0; i < 20 && s_blink_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_blink_active = false;
    ESP_LOGI(TAG, "Blink stopped");
}

esp_err_t ws2812_blink_start(int gpio, int count, const char *color, int brightness,
                             int interval_ms, int cycles,
                             char *output, size_t output_size)
{
    uint8_t r = 0, g = 0, b = 0;

    if (!gpio_policy_pin_is_allowed(gpio)) {
        if (gpio_policy_pin_forbidden_hint(gpio, output, output_size)) {
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(output, output_size, "Error: pin %d is not allowed for WS2812 output", gpio);
        return ESP_ERR_INVALID_ARG;
    }
    if (count < 1 || count > WS2812_MAX_PIXELS) {
        snprintf(output, output_size, "Error: count must be 1-%d", WS2812_MAX_PIXELS);
        return ESP_ERR_INVALID_ARG;
    }
    if (!parse_color(color, &r, &g, &b)) {
        snprintf(output, output_size, "Error: unknown color '%s'", color ? color : "");
        return ESP_ERR_INVALID_ARG;
    }
    if (interval_ms < 50) interval_ms = 50;
    if (brightness < 0) brightness = 0;
    if (brightness > 255) brightness = 255;

    s_blink_r = (uint8_t)((r * brightness) / 255);
    s_blink_g = (uint8_t)((g * brightness) / 255);
    s_blink_b = (uint8_t)((b * brightness) / 255);
    s_blink_gpio = gpio;
    s_blink_count = count;
    s_blink_interval_ms = interval_ms;
    s_blink_cycles = cycles;

    ws2812_blink_stop();

    s_blink_active = true;
    if (xTaskCreate(ws2812_blink_task, "ws2812_blink", 3072, NULL, 4, &s_blink_task) != pdPASS) {
        s_blink_active = false;
        snprintf(output, output_size, "Error: failed to start blink task (out of memory)");
        return ESP_ERR_NO_MEM;
    }

    snprintf(output, output_size,
             "Blinking %d WS2812 LED(s) on GPIO%d (#%02X%02X%02X), on/off every %d ms%s",
             count, gpio, s_blink_r, s_blink_g, s_blink_b, interval_ms,
             cycles == 0 ? " until stopped" : "");
    ESP_LOGI(TAG, "blink start: gpio=%d count=%d interval=%dms cycles=%d",
             gpio, count, interval_ms, cycles);
    return ESP_OK;
}

static bool parse_hex_color(const char *s, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (strlen(s) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (!isxdigit((unsigned char)s[i])) {
            return false;
        }
    }
    unsigned int value = (unsigned int)strtoul(s, NULL, 16);
    *r = (uint8_t)((value >> 16) & 0xFF);
    *g = (uint8_t)((value >> 8) & 0xFF);
    *b = (uint8_t)(value & 0xFF);
    return true;
}

static bool parse_color(const char *spec, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (!spec || spec[0] == '\0') {
        return false;
    }

    if (spec[0] == '#') {
        return parse_hex_color(spec + 1, r, g, b);
    }
    if (strncmp(spec, "0x", 2) == 0 || strncmp(spec, "0X", 2) == 0) {
        return parse_hex_color(spec + 2, r, g, b);
    }
    if (parse_hex_color(spec, r, g, b)) {
        return true;
    }

    if (strcasecmp(spec, "off") == 0 || strcasecmp(spec, "black") == 0) {
        *r = *g = *b = 0;
        return true;
    }
    if (strcasecmp(spec, "white") == 0) {
        *r = *g = *b = 255;
        return true;
    }
    if (strcasecmp(spec, "red") == 0) {
        *r = 255; *g = 0; *b = 0;
        return true;
    }
    if (strcasecmp(spec, "green") == 0) {
        *r = 0; *g = 255; *b = 0;
        return true;
    }
    if (strcasecmp(spec, "blue") == 0) {
        *r = 0; *g = 0; *b = 255;
        return true;
    }
    if (strcasecmp(spec, "yellow") == 0) {
        *r = 255; *g = 255; *b = 0;
        return true;
    }
    if (strcasecmp(spec, "cyan") == 0) {
        *r = 0; *g = 255; *b = 255;
        return true;
    }
    if (strcasecmp(spec, "magenta") == 0 || strcasecmp(spec, "purple") == 0) {
        *r = 255; *g = 0; *b = 255;
        return true;
    }
    if (strcasecmp(spec, "orange") == 0) {
        *r = 255; *g = 128; *b = 0;
        return true;
    }
    if (strcasecmp(spec, "warm") == 0) {
        *r = 255; *g = 180; *b = 107;
        return true;
    }
    if (strcasecmp(spec, "cool") == 0) {
        *r = 200; *g = 220; *b = 255;
        return true;
    }

    return false;
}

esp_err_t ws2812_set_pixels(int gpio, int count, const char *color, int brightness,
                            char *output, size_t output_size)
{
    uint8_t r = 0, g = 0, b = 0;

    if (!gpio_policy_pin_is_allowed(gpio)) {
        if (gpio_policy_pin_forbidden_hint(gpio, output, output_size)) {
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(output, output_size, "Error: pin %d is not allowed for WS2812 output", gpio);
        return ESP_ERR_INVALID_ARG;
    }
    if (count < 1 || count > WS2812_MAX_PIXELS) {
        snprintf(output, output_size, "Error: count must be 1-%d", WS2812_MAX_PIXELS);
        return ESP_ERR_INVALID_ARG;
    }
    if (!parse_color(color, &r, &g, &b)) {
        snprintf(output, output_size,
                 "Error: unknown color '%s' (use #RRGGBB or white/red/green/blue/off)",
                 color ? color : "");
        return ESP_ERR_INVALID_ARG;
    }
    if (brightness < 0) brightness = 0;
    if (brightness > 255) brightness = 255;

    r = (uint8_t)((r * brightness) / 255);
    g = (uint8_t)((g * brightness) / 255);
    b = (uint8_t)((b * brightness) / 255);

    /* A fixed color overrides a running blink */
    ws2812_blink_stop();

    esp_err_t err = ws2812_transmit(gpio, count, r, g, b, output, output_size);
    if (err != ESP_OK) {
        return err;
    }

    snprintf(output, output_size, "Set %d WS2812 LED(s) on GPIO%d to #%02X%02X%02X (brightness %d)",
             count, gpio, r, g, b, brightness);
    ESP_LOGI(TAG, "gpio=%d count=%d color=#%02X%02X%02X brightness=%d",
             gpio, count, r, g, b, brightness);
    return ESP_OK;
}

esp_err_t tool_ws2812_set_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    int gpio = s_cfg_gpio;
    int count = s_cfg_count;
    int brightness = 255;
    char color[32] = "white";

    cJSON *pin_obj = cJSON_GetObjectItem(root, "gpio");
    cJSON *count_obj = cJSON_GetObjectItem(root, "count");
    cJSON *color_obj = cJSON_GetObjectItem(root, "color");
    cJSON *bright_obj = cJSON_GetObjectItem(root, "brightness");

    if (cJSON_IsNumber(pin_obj)) gpio = (int)pin_obj->valuedouble;
    if (cJSON_IsNumber(count_obj)) count = (int)count_obj->valuedouble;
    if (cJSON_IsNumber(bright_obj)) brightness = (int)bright_obj->valuedouble;
    if (cJSON_IsString(color_obj) && color_obj->valuestring) {
        snprintf(color, sizeof(color), "%s", color_obj->valuestring);
    }

    esp_err_t err = ws2812_set_pixels(gpio, count, color, brightness, output, output_size);
    cJSON_Delete(root);
    return err;
}

esp_err_t tool_ws2812_blink_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    int gpio = s_cfg_gpio;
    int count = s_cfg_count;
    int brightness = 255;
    int interval_ms = s_blink_interval_ms;
    int cycles = 0;
    char color[32] = "white";

    cJSON *pin_obj = cJSON_GetObjectItem(root, "gpio");
    cJSON *count_obj = cJSON_GetObjectItem(root, "count");
    cJSON *color_obj = cJSON_GetObjectItem(root, "color");
    cJSON *bright_obj = cJSON_GetObjectItem(root, "brightness");
    cJSON *interval_obj = cJSON_GetObjectItem(root, "interval_ms");
    cJSON *cycles_obj = cJSON_GetObjectItem(root, "cycles");

    if (cJSON_IsNumber(pin_obj)) gpio = (int)pin_obj->valuedouble;
    if (cJSON_IsNumber(count_obj)) count = (int)count_obj->valuedouble;
    if (cJSON_IsNumber(bright_obj)) brightness = (int)bright_obj->valuedouble;
    if (cJSON_IsNumber(interval_obj)) interval_ms = (int)interval_obj->valuedouble;
    if (cJSON_IsNumber(cycles_obj)) cycles = (int)cycles_obj->valuedouble;
    if (cJSON_IsString(color_obj) && color_obj->valuestring) {
        snprintf(color, sizeof(color), "%s", color_obj->valuestring);
    }

    esp_err_t err = ws2812_blink_start(gpio, count, color, brightness,
                                       interval_ms, cycles, output, output_size);
    cJSON_Delete(root);
    return err;
}

esp_err_t tool_ws2812_stop_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;
    ws2812_blink_stop();

    char msg[64];
    if (ws2812_transmit(s_cfg_gpio, s_cfg_count, 0, 0, 0, msg, sizeof(msg)) != ESP_OK) {
        snprintf(output, output_size, "Blink stopped, but failed to turn the LED off: %s", msg);
        return ESP_FAIL;
    }
    snprintf(output, output_size, "Blink stopped, LED on GPIO%d is off", s_cfg_gpio);
    return ESP_OK;
}

esp_err_t tool_ws2812_init(void)
{
    ws2812_load_config();
    ESP_LOGI(TAG, "WS2812 tool initialized (GPIO%d, %d LED(s), max %d)",
             s_cfg_gpio, s_cfg_count, WS2812_MAX_PIXELS);
    return ESP_OK;
}
