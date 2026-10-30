#include "tools/tool_spi.h"
#include "tools/gpio_policy.h"

#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "spi";

#define SPI_MAX_BYTES   32
#define SPI_HOST_ID     SPI2_HOST
#define SPI_DEFAULT_FREQ 1000000

static bool s_bus_inited = false;
static int s_mosi = -1;
static int s_miso = -1;
static int s_sclk = -1;

esp_err_t tool_spi_init(void)
{
    s_bus_inited = false;
    s_mosi = s_miso = s_sclk = -1;
    return ESP_OK;
}

static bool pin_ok(int pin, bool allow_unused, char *out, size_t out_size)
{
    if (allow_unused && pin < 0) {
        return true;
    }
    if (gpio_policy_pin_is_allowed(pin)) {
        return true;
    }
    if (gpio_policy_pin_forbidden_hint(pin, out, out_size)) {
        return false;
    }
    snprintf(out, out_size, "Error: pin %d is not allowed (see set_gpio_allow)", pin);
    return false;
}

static bool ensure_bus(int mosi, int miso, int sclk, char *out, size_t out_size)
{
    if (s_bus_inited) {
        if (s_mosi == mosi && s_miso == miso && s_sclk == sclk) {
            return true;
        }
        spi_bus_free(SPI_HOST_ID);
        s_bus_inited = false;
    }

    spi_bus_config_t cfg = {
        .mosi_io_num = mosi,
        .miso_io_num = miso,
        .sclk_io_num = sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_MAX_BYTES,
    };
    if (spi_bus_initialize(SPI_HOST_ID, &cfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        snprintf(out, out_size, "Error: failed to init SPI bus (MOSI=%d MISO=%d SCLK=%d)",
                 mosi, miso, sclk);
        return false;
    }
    s_bus_inited = true;
    s_mosi = mosi;
    s_miso = miso;
    s_sclk = sclk;
    ESP_LOGI(TAG, "SPI2 ready (MOSI=%d MISO=%d SCLK=%d)", mosi, miso, sclk);
    return true;
}

static int parse_hex_bytes(const char *s, uint8_t *buf, int max)
{
    int n = 0;
    while (s && *s && n < max) {
        while (*s && (isspace((unsigned char)*s) || *s == ',' || *s == ':')) {
            s++;
        }
        if (!*s || !isxdigit((unsigned char)*s)) {
            break;
        }
        unsigned int value = 0;
        int digits = 0;
        while (digits < 2 && isxdigit((unsigned char)*s)) {
            char c = (char)tolower((unsigned char)*s);
            value = value * 16 + (unsigned int)(isdigit((unsigned char)c) ? c - '0' : c - 'a' + 10);
            s++;
            digits++;
        }
        buf[n++] = (uint8_t)value;
    }
    return n;
}

esp_err_t tool_spi_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    int mosi = 11, miso = 13, sclk = 12, cs = 10;
    int mode = 0, freq = SPI_DEFAULT_FREQ, bits = 0;
    cJSON *obj;

    if (cJSON_IsNumber(obj = cJSON_GetObjectItem(root, "mosi"))) mosi = (int)obj->valuedouble;
    if (cJSON_IsNumber(obj = cJSON_GetObjectItem(root, "miso"))) miso = (int)obj->valuedouble;
    if (cJSON_IsNumber(obj = cJSON_GetObjectItem(root, "sclk"))) sclk = (int)obj->valuedouble;
    if (cJSON_IsNumber(obj = cJSON_GetObjectItem(root, "cs"))) cs = (int)obj->valuedouble;
    if (cJSON_IsNumber(obj = cJSON_GetObjectItem(root, "mode"))) mode = (int)obj->valuedouble;
    if (cJSON_IsNumber(obj = cJSON_GetObjectItem(root, "freq"))) freq = (int)obj->valuedouble;
    if (cJSON_IsNumber(obj = cJSON_GetObjectItem(root, "bits"))) bits = (int)obj->valuedouble;

    if (!pin_ok(mosi, false, output, output_size) ||
        !pin_ok(sclk, false, output, output_size) ||
        !pin_ok(miso, true, output, output_size) ||
        !pin_ok(cs, true, output, output_size)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (mode < 0 || mode > 3) {
        snprintf(output, output_size, "Error: 'mode' must be 0-3");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t tx[SPI_MAX_BYTES];
    int tx_len = 0;
    if (cJSON_IsString(obj = cJSON_GetObjectItem(root, "tx")) && obj->valuestring) {
        tx_len = parse_hex_bytes(obj->valuestring, tx, SPI_MAX_BYTES);
    }
    if (tx_len == 0) {
        snprintf(output, output_size, "Error: 'tx' required (hex bytes, e.g. \"01 02 03\")");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (bits <= 0) {
        bits = tx_len * 8;
    }
    if (bits > SPI_MAX_BYTES * 8) {
        bits = SPI_MAX_BYTES * 8;
    }

    if (!ensure_bus(mosi, miso, sclk, output, output_size)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    spi_device_interface_config_t dcfg = {
        .clock_speed_hz = freq,
        .mode = (uint8_t)mode,
        .spics_io_num = cs,
        .queue_size = 1,
    };
    spi_device_handle_t dev = NULL;
    esp_err_t err = spi_bus_add_device(SPI_HOST_ID, &dcfg, &dev);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: spi_bus_add_device failed (%d)", (int)err);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    uint8_t *rx = heap_caps_calloc(SPI_MAX_BYTES, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rx) {
        spi_bus_remove_device(dev);
        snprintf(output, output_size, "Error: out of memory");
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    spi_transaction_t t = {
        .length = (size_t)bits,
        .tx_buffer = tx,
        .rx_buffer = miso >= 0 ? rx : NULL,
    };
    err = spi_device_transmit(dev, &t);

    if (err == ESP_OK) {
        if (miso >= 0) {
            snprintf(output, output_size, "SPI %d-bit xfer @%dHz mode %d, RX: ",
                     bits, freq, mode);
            for (int i = 0; i < (bits + 7) / 8; i++) {
                size_t len = strlen(output);
                if (len + 4 >= output_size) break;
                snprintf(output + len, output_size - len, len == 0 ? "%02X" : " %02X", rx[i]);
            }
        } else {
            snprintf(output, output_size, "SPI sent %d byte(s) @%dHz mode %d (no MISO)",
                     (bits + 7) / 8, freq, mode);
        }
        ESP_LOGI(TAG, "spi transfer ok (%d bits)", bits);
    } else {
        snprintf(output, output_size, "Error: SPI transfer failed (%d)", (int)err);
    }

    heap_caps_free(rx);
    spi_bus_remove_device(dev);
    cJSON_Delete(root);
    return err;
}
