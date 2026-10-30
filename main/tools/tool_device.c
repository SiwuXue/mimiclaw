#include "tools/tool_device.h"
#include "tools/tool_ws2812.h"
#include "tools/tool_servo.h"
#include "tools/tool_gpio.h"
#include "tools/tool_i2c.h"
#include "tools/tool_spi.h"
#include "tools/tool_uart.h"
#include "tools/tool_adc.h"
#include "tools/tool_rmt.h"
#include "mimi_config.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "device";

#define DEVICE_FILE        MIMI_SPIFFS_BASE "/devices.json"
#define DEVICE_MAX_COUNT   16

esp_err_t tool_device_init(void)
{
    FILE *f = fopen(DEVICE_FILE, "r");
    if (!f) {
        ESP_LOGI(TAG, "No device table yet (%s)", DEVICE_FILE);
        return ESP_OK;
    }
    fclose(f);
    ESP_LOGI(TAG, "Device table found at %s", DEVICE_FILE);
    return ESP_OK;
}

static cJSON *devices_load(void)
{
    FILE *f = fopen(DEVICE_FILE, "r");
    if (!f) {
        return NULL;
    }

    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    if (!cJSON_IsArray(cJSON_GetObjectItem(root, "devices"))) {
        cJSON_DeleteItemFromObject(root, "devices");
        cJSON_AddItemToObject(root, "devices", cJSON_CreateArray());
    }
    return root;
}

static esp_err_t devices_save(cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    if (!text) {
        return ESP_FAIL;
    }
    FILE *f = fopen(DEVICE_FILE, "w");
    if (!f) {
        free(text);
        return ESP_FAIL;
    }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
    free(text);
    return ESP_OK;
}

static cJSON *device_find(cJSON *root, const char *name)
{
    cJSON *arr = cJSON_GetObjectItem(root, "devices");
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        cJSON *n = cJSON_GetObjectItem(item, "name");
        if (cJSON_IsString(n) && n->valuestring && strcmp(n->valuestring, name) == 0) {
            return item;
        }
    }
    return NULL;
}

static void copy_fields(cJSON *dst, cJSON *src, const char *skip1, const char *skip2)
{
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, src) {
        if (!item->string) continue;
        if (skip1 && strcmp(item->string, skip1) == 0) continue;
        if (skip2 && strcmp(item->string, skip2) == 0) continue;
        cJSON *dup = cJSON_Duplicate(item, true);
        if (dup) {
            cJSON_DeleteItemFromObject(dst, item->string);
            cJSON_AddItemToObject(dst, item->string, dup);
        }
    }
}

esp_err_t tool_device_define_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *in = cJSON_Parse(input_json ? input_json : "{}");
    if (!in) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *name_obj = cJSON_GetObjectItem(in, "name");
    cJSON *type_obj = cJSON_GetObjectItem(in, "type");
    if (!cJSON_IsString(name_obj) || !name_obj->valuestring ||
        !cJSON_IsString(type_obj) || !type_obj->valuestring) {
        snprintf(output, output_size,
                 "Error: 'name' and 'type' required (type: ws2812|servo|gpio|adc|i2c|spi|uart|rmt)");
        cJSON_Delete(in);
        return ESP_ERR_INVALID_ARG;
    }
    const char *name = name_obj->valuestring;
    const char *type = type_obj->valuestring;

    cJSON *root = devices_load();
    if (!root) {
        root = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "devices", cJSON_CreateArray());
    }
    cJSON *arr = cJSON_GetObjectItem(root, "devices");
    cJSON *existing = device_find(root, name);

    if (existing) {
        copy_fields(existing, in, "name", NULL);
        if (!cJSON_GetObjectItem(existing, "type")) {
            cJSON_AddStringToObject(existing, "type", type);
        }
        cJSON_ReplaceItemInObject(existing, "type", cJSON_CreateString(type));
    } else {
        if (cJSON_GetArraySize(arr) >= DEVICE_MAX_COUNT) {
            snprintf(output, output_size, "Error: device table full (max %d)", DEVICE_MAX_COUNT);
            cJSON_Delete(root);
            cJSON_Delete(in);
            return ESP_ERR_NO_MEM;
        }
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "name", name);
        cJSON_AddStringToObject(entry, "type", type);
        copy_fields(entry, in, "name", "type");
        cJSON_AddItemToArray(arr, entry);
    }

    esp_err_t err = devices_save(root);
    int total = cJSON_GetArraySize(arr);
    cJSON_Delete(root);
    cJSON_Delete(in);

    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to save %s", DEVICE_FILE);
        return ESP_FAIL;
    }

    snprintf(output, output_size, "Device '%s' (%s) saved. %d device(s) total.", name, type, total);
    ESP_LOGI(TAG, "define: %s (%s)", name, type);
    return ESP_OK;
}

esp_err_t tool_device_list_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;

    cJSON *root = devices_load();
    if (!root) {
        snprintf(output, output_size,
                 "No devices defined. Use device_define {\"name\",\"type\",\"pin\"} to add one.");
        return ESP_OK;
    }

    cJSON *arr = cJSON_GetObjectItem(root, "devices");
    int count = cJSON_GetArraySize(arr);
    if (count == 0) {
        snprintf(output, output_size, "No devices defined.");
        cJSON_Delete(root);
        return ESP_OK;
    }

    size_t off = 0;
    off += (size_t)snprintf(output, output_size, "Devices (%d):", count);
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (off >= output_size - 24) {
            break;
        }
        cJSON *n = cJSON_GetObjectItem(item, "name");
        cJSON *t = cJSON_GetObjectItem(item, "type");
        cJSON *p = cJSON_GetObjectItem(item, "pin");
        cJSON *c = cJSON_GetObjectItem(item, "count");
        off += (size_t)snprintf(output + off, output_size - off, " %s[%s",
                                cJSON_IsString(n) ? n->valuestring : "?",
                                cJSON_IsString(t) ? t->valuestring : "?");
        if (cJSON_IsNumber(p)) {
            off += (size_t)snprintf(output + off, output_size - off, " pin=%d", (int)p->valuedouble);
        }
        if (cJSON_IsNumber(c)) {
            off += (size_t)snprintf(output + off, output_size - off, " count=%d", (int)c->valuedouble);
        }
        off += (size_t)snprintf(output + off, output_size - off, "]");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static void set_string(cJSON *obj, const char *key, const char *value)
{
    cJSON_DeleteItemFromObject(obj, key);
    cJSON_AddStringToObject(obj, key, value);
}

static void set_number(cJSON *obj, const char *key, double value)
{
    cJSON_DeleteItemFromObject(obj, key);
    cJSON_AddNumberToObject(obj, key, value);
}

static esp_err_t run_json(esp_err_t (*fn)(const char *, char *, size_t),
                          cJSON *params, char *output, size_t output_size)
{
    char *text = cJSON_PrintUnformatted(params);
    if (!text) {
        snprintf(output, output_size, "Error: failed to build tool input");
        return ESP_FAIL;
    }
    esp_err_t err = fn(text, output, output_size);
    free(text);
    return err;
}

esp_err_t tool_device_do_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *in = cJSON_Parse(input_json ? input_json : "{}");
    if (!in) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *name_obj = cJSON_GetObjectItem(in, "name");
    cJSON *action_obj = cJSON_GetObjectItem(in, "action");
    if (!cJSON_IsString(name_obj) || !name_obj->valuestring ||
        !cJSON_IsString(action_obj) || !action_obj->valuestring) {
        snprintf(output, output_size, "Error: 'name' and 'action' required");
        cJSON_Delete(in);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = devices_load();
    if (!root) {
        snprintf(output, output_size,
                 "Error: no device '%s' (device table is empty; use device_define first)",
                 name_obj->valuestring);
        cJSON_Delete(in);
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *dev = device_find(root, name_obj->valuestring);
    if (!dev) {
        snprintf(output, output_size,
                 "Error: unknown device '%s'. Use device_list to see defined names.",
                 name_obj->valuestring);
        cJSON_Delete(root);
        cJSON_Delete(in);
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *type_obj = cJSON_GetObjectItem(dev, "type");
    const char *type = cJSON_IsString(type_obj) ? type_obj->valuestring : "";
    const char *action = action_obj->valuestring;

    /* Merge: device fields first, then caller overrides (minus name/action) */
    cJSON *params = cJSON_CreateObject();
    copy_fields(params, dev, "name", "type");
    copy_fields(params, in, "name", "action");

    esp_err_t err;

    if (strcmp(type, "ws2812") == 0) {
        if (strcmp(action, "blink") == 0 || strcmp(action, "flash") == 0) {
            err = run_json(tool_ws2812_blink_execute, params, output, output_size);
        } else if (strcmp(action, "stop") == 0) {
            err = run_json(tool_ws2812_stop_execute, params, output, output_size);
        } else {
            cJSON *v = cJSON_GetObjectItem(params, "value");
            if (cJSON_IsString(v)) {
                set_string(params, "color", v->valuestring);
            } else if (strcmp(action, "off") == 0) {
                set_string(params, "color", "off");
            } else if (strcmp(action, "on") == 0) {
                if (!cJSON_GetObjectItem(params, "color")) {
                    set_string(params, "color", "white");
                }
            } else if (!cJSON_GetObjectItem(params, "color")) {
                set_string(params, "color", action);
            }
            cJSON_DeleteItemFromObject(params, "value");
            err = run_json(tool_ws2812_set_execute, params, output, output_size);
        }
    } else if (strcmp(type, "servo") == 0) {
        cJSON *v = cJSON_GetObjectItem(params, "value");
        if (cJSON_IsNumber(v)) {
            set_number(params, "angle", v->valuedouble);
        }
        err = run_json(tool_servo_set_execute, params, output, output_size);
    } else if (strcmp(type, "gpio") == 0) {
        if (strcmp(action, "read") == 0) {
            err = run_json(tool_gpio_read_execute, params, output, output_size);
        } else {
            cJSON *v = cJSON_GetObjectItem(params, "value");
            cJSON *pin_obj = cJSON_GetObjectItem(params, "pin");
            int state;

            if (strcmp(action, "toggle") == 0) {
                int pin = cJSON_IsNumber(pin_obj) ? (int)pin_obj->valuedouble : -1;
                if (pin >= 0) {
                    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT);
                    state = gpio_get_level(pin) ? 0 : 1;
                } else {
                    state = 1;
                }
            } else if (cJSON_IsNumber(v)) {
                state = (int)v->valuedouble ? 1 : 0;
            } else {
                state = (strcmp(action, "off") == 0) ? 0 : 1;
            }
            cJSON_DeleteItemFromObject(params, "value");
            set_number(params, "state", state);
            err = run_json(tool_gpio_write_execute, params, output, output_size);
        }
    } else if (strcmp(type, "adc") == 0) {
        err = run_json(tool_adc_execute, params, output, output_size);
    } else if (strcmp(type, "i2c") == 0) {
        set_string(params, "op", action);
        err = run_json(tool_i2c_execute, params, output, output_size);
    } else if (strcmp(type, "spi") == 0) {
        cJSON *v = cJSON_GetObjectItem(params, "value");
        if (cJSON_IsString(v)) {
            set_string(params, "tx", v->valuestring);
        }
        err = run_json(tool_spi_execute, params, output, output_size);
    } else if (strcmp(type, "uart") == 0) {
        set_string(params, "op", action);
        cJSON *v = cJSON_GetObjectItem(params, "value");
        if (cJSON_IsString(v)) {
            set_string(params, "data", v->valuestring);
        }
        err = run_json(tool_uart_execute, params, output, output_size);
    } else if (strcmp(type, "rmt") == 0) {
        set_string(params, "op", action);
        err = run_json(tool_rmt_execute, params, output, output_size);
    } else {
        snprintf(output, output_size,
                 "Error: unsupported device type '%s' (use ws2812|servo|gpio|adc|i2c|spi|uart|rmt)",
                 type);
        err = ESP_ERR_NOT_SUPPORTED;
    }

    cJSON_Delete(params);
    cJSON_Delete(root);
    cJSON_Delete(in);
    return err;
}
