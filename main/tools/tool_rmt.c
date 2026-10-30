#include "tools/tool_rmt.h"
#include "tools/gpio_policy.h"

#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "rmt";

#define RMT_RESOLUTION_HZ   1000000   /* 1 tick = 1 us */
#define RMT_MAX_SYMBOLS     128
#define RMT_MEM_BLOCK       64

typedef struct {
    int gpio;
    rmt_channel_handle_t chan;
    rmt_encoder_handle_t enc;
} tx_slot_t;

static tx_slot_t s_tx;
static rmt_channel_handle_t s_rx_chan = NULL;
static int s_rx_gpio = -1;
static SemaphoreHandle_t s_rx_sem = NULL;
static rmt_symbol_word_t s_rx_buf[RMT_MAX_SYMBOLS];

esp_err_t tool_rmt_init(void)
{
    s_tx.gpio = -1;
    s_tx.chan = NULL;
    s_tx.enc = NULL;
    if (!s_rx_sem) {
        s_rx_sem = xSemaphoreCreateBinary();
    }
    return ESP_OK;
}

static bool pin_ok(int gpio, char *out, size_t out_size)
{
    if (gpio_policy_pin_is_allowed(gpio)) {
        return true;
    }
    if (gpio_policy_pin_forbidden_hint(gpio, out, out_size)) {
        return false;
    }
    snprintf(out, out_size, "Error: pin %d is not allowed for RMT (see set_gpio_allow)", gpio);
    return false;
}

static void tx_close(void)
{
    if (s_tx.chan) {
        rmt_disable(s_tx.chan);
        rmt_del_channel(s_tx.chan);
        s_tx.chan = NULL;
    }
    if (s_tx.enc) {
        rmt_del_encoder(s_tx.enc);
        s_tx.enc = NULL;
    }
    s_tx.gpio = -1;
}

static bool tx_open(int gpio, char *out, size_t out_size)
{
    if (s_tx.chan && s_tx.gpio == gpio) {
        return true;
    }
    tx_close();

    rmt_tx_channel_config_t cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = gpio,
        .mem_block_symbols = RMT_MEM_BLOCK,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    if (rmt_new_tx_channel(&cfg, &s_tx.chan) != ESP_OK) {
        snprintf(out, out_size, "Error: rmt_new_tx_channel failed on GPIO%d", gpio);
        s_tx.chan = NULL;
        return false;
    }
    rmt_copy_encoder_config_t enc_cfg = {};
    if (rmt_new_copy_encoder(&enc_cfg, &s_tx.enc) != ESP_OK ||
        rmt_enable(s_tx.chan) != ESP_OK) {
        snprintf(out, out_size, "Error: failed to enable RMT TX on GPIO%d", gpio);
        tx_close();
        return false;
    }
    s_tx.gpio = gpio;
    ESP_LOGI(TAG, "RMT TX ready on GPIO%d", gpio);
    return true;
}

static bool rx_done_cb(rmt_channel_handle_t chan,
                       const rmt_rx_done_event_data_t *edata, void *user_data)
{
    (void)chan;
    (void)edata;
    (void)user_data;
    BaseType_t woken = pdFALSE;
    if (s_rx_sem) {
        xSemaphoreGiveFromISR(s_rx_sem, &woken);
    }
    return woken == pdTRUE;
}

static void rx_close(void)
{
    if (s_rx_chan) {
        rmt_disable(s_rx_chan);
        rmt_del_channel(s_rx_chan);
        s_rx_chan = NULL;
    }
    s_rx_gpio = -1;
}

static bool rx_open(int gpio, char *out, size_t out_size)
{
    if (s_rx_chan && s_rx_gpio == gpio) {
        return true;
    }
    rx_close();

    rmt_rx_channel_config_t cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = gpio,
        .mem_block_symbols = RMT_MEM_BLOCK,
        .resolution_hz = RMT_RESOLUTION_HZ,
    };
    if (rmt_new_rx_channel(&cfg, &s_rx_chan) != ESP_OK) {
        snprintf(out, out_size, "Error: rmt_new_rx_channel failed on GPIO%d", gpio);
        s_rx_chan = NULL;
        return false;
    }
    rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = rx_done_cb,
    };
    if (rmt_rx_register_event_callbacks(s_rx_chan, &cbs, NULL) != ESP_OK ||
        rmt_enable(s_rx_chan) != ESP_OK) {
        snprintf(out, out_size, "Error: failed to enable RMT RX on GPIO%d", gpio);
        rx_close();
        return false;
    }
    s_rx_gpio = gpio;
    ESP_LOGI(TAG, "RMT RX ready on GPIO%d", gpio);
    return true;
}

static esp_err_t rmt_pulse(int gpio, cJSON *symbols, char *out, size_t out_size)
{
    if (!cJSON_IsArray(symbols)) {
        snprintf(out, out_size, "Error: 'symbols' must be an array of {level, us}");
        return ESP_ERR_INVALID_ARG;
    }
    int count = cJSON_GetArraySize(symbols);
    if (count < 1 || count > RMT_MAX_SYMBOLS) {
        snprintf(out, out_size, "Error: symbols must be 1-%d entries", RMT_MAX_SYMBOLS);
        return ESP_ERR_INVALID_ARG;
    }
    if (!tx_open(gpio, out, out_size)) {
        return ESP_FAIL;
    }

    rmt_symbol_word_t *buf = heap_caps_calloc((size_t)count, sizeof(rmt_symbol_word_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        snprintf(out, out_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }

    int idx = 0;
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(symbols, i);
        cJSON *lvl = cJSON_GetObjectItem(item, "level");
        cJSON *us = cJSON_GetObjectItem(item, "us");
        if (!cJSON_IsNumber(lvl) || !cJSON_IsNumber(us)) {
            heap_caps_free(buf);
            snprintf(out, out_size, "Error: each symbol needs numeric 'level' and 'us'");
            return ESP_ERR_INVALID_ARG;
        }
        int level = (int)lvl->valuedouble ? 1 : 0;
        int duration = (int)us->valuedouble;
        if (duration < 1) duration = 1;
        if (duration > 32767) duration = 32767;
        buf[idx].level0 = level;
        buf[idx].duration0 = (uint16_t)duration;
        buf[idx].level1 = 0;
        buf[idx].duration1 = 0;
        idx++;
    }

    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    esp_err_t err = rmt_transmit(s_tx.chan, s_tx.enc, buf,
                                 (size_t)idx * sizeof(rmt_symbol_word_t), &tx_cfg);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(s_tx.chan, 100);
    }
    heap_caps_free(buf);

    if (err != ESP_OK) {
        snprintf(out, out_size, "Error: RMT transmit failed (%d)", (int)err);
        return ESP_FAIL;
    }

    snprintf(out, out_size, "Sent %d pulse(s) on GPIO%d (1 us resolution)", idx, gpio);
    return ESP_OK;
}

static esp_err_t rmt_capture(int gpio, int idle_us, int max_symbols, int timeout_ms,
                             char *out, size_t out_size)
{
    if (!rx_open(gpio, out, out_size)) {
        return ESP_FAIL;
    }
    if (max_symbols < 1) max_symbols = 1;
    if (max_symbols > RMT_MAX_SYMBOLS) max_symbols = RMT_MAX_SYMBOLS;
    if (idle_us < 1000) idle_us = 1000;

    if (s_rx_sem) {
        xSemaphoreTake(s_rx_sem, 0);   /* drain stale signal */
    }

    rmt_receive_config_t rcfg = {
        .signal_range_min_ns = 200,
        .signal_range_max_ns = (uint32_t)idle_us * 1000,
    };
    esp_err_t err = rmt_receive(s_rx_chan, s_rx_buf,
                                (size_t)max_symbols * sizeof(rmt_symbol_word_t), &rcfg);
    if (err != ESP_OK) {
        snprintf(out, out_size, "Error: rmt_receive failed (%d)", (int)err);
        return ESP_FAIL;
    }

    bool got = false;
    if (s_rx_sem) {
        got = xSemaphoreTake(s_rx_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    }
    if (!got) {
        snprintf(out, out_size,
                 "No signal captured on GPIO%d within %d ms (check wiring and idle level)",
                 gpio, timeout_ms);
        return ESP_ERR_TIMEOUT;
    }

    out[0] = '\0';
    int emitted = 0;
    for (int i = 0; i < max_symbols; i++) {
        rmt_symbol_word_t s = s_rx_buf[i];
        if (s.duration0 == 0) {
            break;
        }
        size_t len = strlen(out);
        if (len >= out_size - 12) {
            break;
        }
        snprintf(out + len, out_size - len, "%s%d:%d",
                 emitted == 0 ? "" : " ", s.level0 ? 1 : 0, (int)s.duration0);
        emitted++;
        if (s.duration1 == 0) {
            break;
        }
        len = strlen(out);
        if (len >= out_size - 12) {
            break;
        }
        snprintf(out + len, out_size - len, " %d:%d",
                 s.level1 ? 1 : 0, (int)s.duration1);
        emitted++;
    }

    char tail[48];
    snprintf(tail, sizeof(tail), " (%d edges, level:us)", emitted);
    size_t len = strlen(out);
    if (len + strlen(tail) < out_size) {
        strcat(out, tail);
    }
    if (emitted == 0) {
        snprintf(out, out_size, "Captured no edges on GPIO%d", gpio);
    }
    return ESP_OK;
}

esp_err_t tool_rmt_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *op_obj = cJSON_GetObjectItem(root, "op");
    cJSON *pin_obj = cJSON_GetObjectItem(root, "gpio");
    if (!cJSON_IsString(op_obj) || !op_obj->valuestring) {
        snprintf(output, output_size, "Error: 'op' required: pulse | capture");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (!cJSON_IsNumber(pin_obj)) {
        snprintf(output, output_size, "Error: 'gpio' required");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    int gpio = (int)pin_obj->valuedouble;
    if (!pin_ok(gpio, output, output_size)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    const char *op = op_obj->valuestring;
    esp_err_t err;

    if (strcmp(op, "pulse") == 0) {
        err = rmt_pulse(gpio, cJSON_GetObjectItem(root, "symbols"), output, output_size);
    } else if (strcmp(op, "capture") == 0) {
        int idle_us = 5000, max_symbols = 64, timeout_ms = 200;
        cJSON *obj;
        if (cJSON_IsNumber(obj = cJSON_GetObjectItem(root, "idle_us"))) idle_us = (int)obj->valuedouble;
        if (cJSON_IsNumber(obj = cJSON_GetObjectItem(root, "max_symbols"))) max_symbols = (int)obj->valuedouble;
        if (cJSON_IsNumber(obj = cJSON_GetObjectItem(root, "timeout_ms"))) timeout_ms = (int)obj->valuedouble;
        err = rmt_capture(gpio, idle_us, max_symbols, timeout_ms, output, output_size);
    } else {
        snprintf(output, output_size, "Error: unknown op '%s' (use pulse | capture)", op);
        err = ESP_ERR_INVALID_ARG;
    }

    cJSON_Delete(root);
    return err;
}
