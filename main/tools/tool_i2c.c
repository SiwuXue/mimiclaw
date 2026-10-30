#include "tools/tool_i2c.h"
#include "tools/gpio_policy.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "cJSON.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "i2c";

#define I2C_MAX_BUSES     2
#define I2C_TIMEOUT_MS    100
#define I2C_MAX_BYTES     32
#define I2C_DEFAULT_FREQ  100000
#define I2C_DEFAULT_SDA   8
#define I2C_DEFAULT_SCL   9

typedef struct {
    int port;
    int sda;
    int scl;
    i2c_master_bus_handle_t bus;
} bus_slot_t;

static bus_slot_t s_buses[I2C_MAX_BUSES];

esp_err_t tool_i2c_init(void)
{
    memset(s_buses, 0, sizeof(s_buses));
    return ESP_OK;
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

static i2c_master_bus_handle_t i2c_get_bus(int port, int sda, int scl,
                                           char *out, size_t out_size)
{
    if (port < 0 || port >= I2C_MAX_BUSES) {
        snprintf(out, out_size, "Error: i2c port must be 0 or 1");
        return NULL;
    }
    if (!pin_ok(sda, out, out_size) || !pin_ok(scl, out, out_size)) {
        return NULL;
    }
    if (sda == scl) {
        snprintf(out, out_size, "Error: SDA and SCL must be different pins");
        return NULL;
    }

    bus_slot_t *slot = &s_buses[port];
    if (slot->bus && slot->sda == sda && slot->scl == scl) {
        return slot->bus;
    }
    if (slot->bus) {
        i2c_del_master_bus(slot->bus);
        slot->bus = NULL;
    }

    i2c_master_bus_config_t cfg = {
        .i2c_port = (i2c_port_t)port,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&cfg, &slot->bus) != ESP_OK) {
        snprintf(out, out_size, "Error: failed to init I2C bus %d on SDA=%d SCL=%d",
                 port, sda, scl);
        slot->bus = NULL;
        return NULL;
    }
    slot->port = port;
    slot->sda = sda;
    slot->scl = scl;
    ESP_LOGI(TAG, "I2C bus %d ready (SDA=%d SCL=%d)", port, sda, scl);
    return slot->bus;
}

static i2c_master_dev_handle_t i2c_add_dev(i2c_master_bus_handle_t bus, uint8_t addr, int freq)
{
    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = freq,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(bus, &dcfg, &dev) != ESP_OK) {
        return NULL;
    }
    return dev;
}

/* Accepts "3C5A", "3c 5a", "3c,5a" -> bytes */
static int parse_hex_bytes(const char *s, uint8_t *buf, int max)
{
    int n = 0;

    while (s && *s && n < max) {
        while (*s && (isspace((unsigned char)*s) || *s == ',' || *s == ':' || *s == '-')) {
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

static void append_hex(char *out, size_t out_size, int value)
{
    size_t len = strlen(out);
    if (len + 4 >= out_size) {
        return;
    }
    snprintf(out + len, out_size - len, len == 0 ? "%02X" : " %02X", value & 0xFF);
}

esp_err_t tool_i2c_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *op_obj = cJSON_GetObjectItem(root, "op");
    if (!cJSON_IsString(op_obj) || !op_obj->valuestring) {
        snprintf(output, output_size, "Error: 'op' required: scan | read | write");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    int port = 0, sda = I2C_DEFAULT_SDA, scl = I2C_DEFAULT_SCL, freq = I2C_DEFAULT_FREQ;
    int addr = -1;
    cJSON *obj;

    if (cJSON_IsNumber(obj = cJSON_GetObjectItem(root, "port"))) port = (int)obj->valuedouble;
    if (cJSON_IsNumber(obj = cJSON_GetObjectItem(root, "sda"))) sda = (int)obj->valuedouble;
    if (cJSON_IsNumber(obj = cJSON_GetObjectItem(root, "scl"))) scl = (int)obj->valuedouble;
    if (cJSON_IsNumber(obj = cJSON_GetObjectItem(root, "freq"))) freq = (int)obj->valuedouble;
    if (cJSON_IsNumber(obj = cJSON_GetObjectItem(root, "addr"))) addr = (int)obj->valuedouble;

    const char *op = op_obj->valuestring;
    i2c_master_bus_handle_t bus = i2c_get_bus(port, sda, scl, output, output_size);
    if (!bus) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;

    if (strcmp(op, "scan") == 0) {
        output[0] = '\0';
        int found = 0;
        for (int a = 1; a < 127; a++) {
            if (i2c_master_probe(bus, (uint8_t)a, I2C_TIMEOUT_MS) == ESP_OK) {
                append_hex(output, output_size, a);
                found++;
            }
        }
        if (found == 0) {
            snprintf(output, output_size, "No I2C device found on SDA=%d SCL=%d", sda, scl);
        } else {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), " (%d device(s), hex addresses)", found);
            size_t len = strlen(output);
            if (len + strlen(tmp) < output_size) {
                strcat(output, tmp);
            }
        }
    } else if (strcmp(op, "read") == 0 || strcmp(op, "write") == 0) {
        if (addr < 0 || addr > 127) {
            snprintf(output, output_size, "Error: 'addr' required (0-127)");
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        i2c_master_dev_handle_t dev = i2c_add_dev(bus, (uint8_t)addr, freq);
        if (!dev) {
            snprintf(output, output_size, "Error: failed to attach address 0x%02X", addr);
            cJSON_Delete(root);
            return ESP_FAIL;
        }

        if (strcmp(op, "write") == 0) {
            uint8_t data[I2C_MAX_BYTES];
            int len = 0;
            if (cJSON_IsString(obj = cJSON_GetObjectItem(root, "data")) && obj->valuestring) {
                len = parse_hex_bytes(obj->valuestring, data, I2C_MAX_BYTES);
            }
            const char *reg = NULL;
            if (cJSON_IsString(obj = cJSON_GetObjectItem(root, "reg")) && obj->valuestring) {
                reg = obj->valuestring;
            }
            if (len == 0 && !reg) {
                snprintf(output, output_size, "Error: provide 'data' or 'reg' (hex)");
                err = ESP_ERR_INVALID_ARG;
            } else {
                uint8_t frame[I2C_MAX_BYTES + 1];
                int flen = 0;
                if (reg) {
                    uint8_t rbuf[1];
                    int rlen = parse_hex_bytes(reg, rbuf, 1);
                    if (rlen == 1) {
                        frame[flen++] = rbuf[0];
                    }
                }
                memcpy(frame + flen, data, (size_t)len);
                flen += len;

                err = i2c_master_transmit(dev, frame, (size_t)flen, I2C_TIMEOUT_MS);
                if (err == ESP_OK) {
                    snprintf(output, output_size, "Wrote %d byte(s) to 0x%02X: ", flen, addr);
                    for (int i = 0; i < flen; i++) {
                        append_hex(output, output_size, frame[i]);
                    }
                } else {
                    snprintf(output, output_size, "Error: I2C write to 0x%02X failed (%d)",
                             addr, (int)err);
                }
            }
        } else {
            int len = 4;
            if (cJSON_IsNumber(obj = cJSON_GetObjectItem(root, "len"))) {
                len = (int)obj->valuedouble;
            }
            if (len < 1) len = 1;
            if (len > I2C_MAX_BYTES) len = I2C_MAX_BYTES;

            uint8_t buf[I2C_MAX_BYTES];
            const char *reg = NULL;
            if (cJSON_IsString(obj = cJSON_GetObjectItem(root, "reg")) && obj->valuestring) {
                reg = obj->valuestring;
            }

            if (reg) {
                uint8_t rbyte[1];
                int rlen = parse_hex_bytes(reg, rbyte, 1);
                if (rlen != 1) {
                    snprintf(output, output_size, "Error: 'reg' must be one hex byte");
                    cJSON_Delete(root);
                    i2c_master_bus_rm_device(dev);
                    return ESP_ERR_INVALID_ARG;
                }
                err = i2c_master_transmit_receive(dev, rbyte, 1, buf, (size_t)len,
                                                  I2C_TIMEOUT_MS);
            } else {
                err = i2c_master_receive(dev, buf, (size_t)len, I2C_TIMEOUT_MS);
            }

            if (err == ESP_OK) {
                snprintf(output, output_size, "Read %d byte(s) from 0x%02X: ", len, addr);
                for (int i = 0; i < len; i++) {
                    append_hex(output, output_size, buf[i]);
                }
            } else {
                snprintf(output, output_size, "Error: I2C read from 0x%02X failed (%d)",
                         addr, (int)err);
            }
        }

        i2c_master_bus_rm_device(dev);
    } else {
        snprintf(output, output_size, "Error: unknown op '%s' (use scan | read | write)", op);
        err = ESP_ERR_INVALID_ARG;
    }

    cJSON_Delete(root);
    return err;
}
