#include "tools/tool_uart.h"
#include "tools/gpio_policy.h"

#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "uart";

#define UART_RX_BUF   2048
#define UART_MAX_READ 64

static int s_baud[3] = {0, 0, 0};
static int s_tx[3] = {-1, -1, -1};
static int s_rx[3] = {-1, -1, -1};

esp_err_t tool_uart_init(void)
{
    for (int i = 0; i < 3; i++) {
        s_baud[i] = 0;
        s_tx[i] = -1;
        s_rx[i] = -1;
    }
    return ESP_OK;
}

static uart_port_t port_from(int n)
{
    if (n == 1) return UART_NUM_1;
    if (n == 2) return UART_NUM_2;
    return UART_NUM_MAX;
}

static bool pin_ok(int pin, char *out, size_t out_size)
{
    if (gpio_policy_pin_is_allowed(pin)) {
        return true;
    }
    if (gpio_policy_pin_forbidden_hint(pin, out, out_size)) {
        return false;
    }
    snprintf(out, out_size, "Error: pin %d is not allowed (see set_gpio_allow)", pin);
    return false;
}

static esp_err_t uart_setup_port(int n, int tx, int rx, int baud,
                                 char *out, size_t out_size)
{
    uart_port_t port = port_from(n);
    if (port == UART_NUM_MAX) {
        snprintf(out, out_size, "Error: port must be 1 or 2 (0 is the serial console)");
        return ESP_ERR_INVALID_ARG;
    }
    if (!pin_ok(tx, out, out_size) || !pin_ok(rx, out, out_size)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_baud[n] == baud && s_tx[n] == tx && s_rx[n] == rx &&
        uart_is_driver_installed(port)) {
        return ESP_OK;
    }

    if (uart_is_driver_installed(port)) {
        uart_driver_delete(port);
    }

    uart_config_t cfg = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(port, &cfg);
    if (err == ESP_OK) {
        err = uart_set_pin(port, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
        err = uart_driver_install(port, UART_RX_BUF, 0, 0, NULL, 0);
    }
    if (err != ESP_OK) {
        snprintf(out, out_size, "Error: failed to set up UART%d (%d)", n, (int)err);
        return ESP_FAIL;
    }

    s_baud[n] = baud;
    s_tx[n] = tx;
    s_rx[n] = rx;
    ESP_LOGI(TAG, "UART%d ready (TX=%d RX=%d @%d)", n, tx, rx, baud);
    return ESP_OK;
}

static void append_preview(char *out, size_t out_size, const uint8_t *buf, int len)
{
    size_t off = strlen(out);
    if (off + 4 >= out_size) return;
    snprintf(out + off, out_size - off, " ascii=\"");
    off = strlen(out);
    for (int i = 0; i < len && off + 2 < out_size; i++) {
        unsigned char c = buf[i];
        out[off++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        out[off] = '\0';
    }
    off = strlen(out);
    if (off + 2 < out_size) {
        snprintf(out + off, out_size - off, "\"");
    }
}

esp_err_t tool_uart_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    int n = 1;
    int baud = 115200;
    int tx = -1, rx = -1;
    cJSON *obj;
    if (cJSON_IsNumber(obj = cJSON_GetObjectItem(root, "port"))) n = (int)obj->valuedouble;
    if (cJSON_IsNumber(obj = cJSON_GetObjectItem(root, "baud"))) baud = (int)obj->valuedouble;
    if (cJSON_IsNumber(obj = cJSON_GetObjectItem(root, "tx"))) tx = (int)obj->valuedouble;
    if (cJSON_IsNumber(obj = cJSON_GetObjectItem(root, "rx"))) rx = (int)obj->valuedouble;

    cJSON *op_obj = cJSON_GetObjectItem(root, "op");
    if (!cJSON_IsString(op_obj) || !op_obj->valuestring) {
        snprintf(output, output_size, "Error: 'op' required: setup | write | read");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    const char *op = op_obj->valuestring;
    uart_port_t port = port_from(n);
    if (port == UART_NUM_MAX) {
        snprintf(output, output_size, "Error: port must be 1 or 2 (0 is the serial console)");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;

    if (strcmp(op, "setup") == 0) {
        if (tx < 0 || rx < 0) {
            snprintf(output, output_size, "Error: 'tx' and 'rx' pins required for setup");
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        err = uart_setup_port(n, tx, rx, baud, output, output_size);
        if (err == ESP_OK) {
            snprintf(output, output_size, "UART%d ready: TX=%d RX=%d @%d baud", n, tx, rx, baud);
        }
    } else if (strcmp(op, "write") == 0) {
        if (!uart_is_driver_installed(port)) {
            snprintf(output, output_size, "Error: UART%d not set up yet (call op=setup first)", n);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_STATE;
        }
        cJSON *data_obj = cJSON_GetObjectItem(root, "data");
        if (!cJSON_IsString(data_obj) || !data_obj->valuestring) {
            snprintf(output, output_size, "Error: 'data' required");
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        size_t len = strlen(data_obj->valuestring);
        int sent = uart_write_bytes(port, data_obj->valuestring, len);
        if (sent < 0) {
            snprintf(output, output_size, "Error: UART%d write failed", n);
            err = ESP_FAIL;
        } else {
            snprintf(output, output_size, "UART%d wrote %d byte(s)", n, sent);
        }
    } else if (strcmp(op, "read") == 0) {
        if (!uart_is_driver_installed(port)) {
            snprintf(output, output_size, "Error: UART%d not set up yet (call op=setup first)", n);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_STATE;
        }
        int timeout_ms = 500;
        int want = UART_MAX_READ;
        if (cJSON_IsNumber(obj = cJSON_GetObjectItem(root, "timeout_ms"))) timeout_ms = (int)obj->valuedouble;
        if (cJSON_IsNumber(obj = cJSON_GetObjectItem(root, "len"))) want = (int)obj->valuedouble;
        if (want < 1) want = 1;
        if (want > UART_MAX_READ) want = UART_MAX_READ;

        uint8_t *buf = heap_caps_calloc((size_t)want, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) {
            snprintf(output, output_size, "Error: out of memory");
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        int got = uart_read_bytes(port, buf, (size_t)want, pdMS_TO_TICKS(timeout_ms));
        if (got <= 0) {
            snprintf(output, output_size, "UART%d: no data within %d ms", n, timeout_ms);
            err = ESP_ERR_TIMEOUT;
        } else {
            snprintf(output, output_size, "UART%d read %d byte(s):", n, got);
            for (int i = 0; i < got; i++) {
                size_t len = strlen(output);
                if (len + 4 >= output_size) break;
                snprintf(output + len, output_size - len, " %02X", buf[i]);
            }
            append_preview(output, output_size, buf, got);
        }
        heap_caps_free(buf);
    } else {
        snprintf(output, output_size, "Error: unknown op '%s' (use setup | write | read)", op);
        err = ESP_ERR_INVALID_ARG;
    }

    cJSON_Delete(root);
    return err;
}
