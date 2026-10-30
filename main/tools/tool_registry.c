#include "tool_registry.h"
#include "mimi_config.h"
#include "tools/tool_web_search.h"
#include "tools/tool_get_time.h"
#include "tools/tool_files.h"
#include "tools/tool_cron.h"
#include "tools/tool_gpio.h"
#include "tools/tool_ws2812.h"
#include "tools/tool_servo.h"
#include "tools/tool_i2c.h"
#include "tools/tool_spi.h"
#include "tools/tool_uart.h"
#include "tools/tool_adc.h"
#include "tools/tool_rmt.h"
#include "tools/tool_device.h"

#include <string.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tools";

#define MAX_TOOLS 32

static mimi_tool_t s_tools[MAX_TOOLS];
static int s_tool_count = 0;
static char *s_tools_json = NULL;  /* cached JSON array string */

static void register_tool(const mimi_tool_t *tool)
{
    if (s_tool_count >= MAX_TOOLS) {
        ESP_LOGE(TAG, "Tool registry full");
        return;
    }
    s_tools[s_tool_count++] = *tool;
    ESP_LOGI(TAG, "Registered tool: %s", tool->name);
}

static void build_tools_json(void)
{
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < s_tool_count; i++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", s_tools[i].name);
        cJSON_AddStringToObject(tool, "description", s_tools[i].description);

        cJSON *schema = cJSON_Parse(s_tools[i].input_schema_json);
        if (schema) {
            cJSON_AddItemToObject(tool, "input_schema", schema);
        }

        cJSON_AddItemToArray(arr, tool);
    }

    free(s_tools_json);
    s_tools_json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    ESP_LOGI(TAG, "Tools JSON built (%d tools)", s_tool_count);
}

esp_err_t tool_registry_init(void)
{
    s_tool_count = 0;

    /* Register web_search */
    tool_web_search_init();

    mimi_tool_t ws = {
        .name = "web_search",
        .description = "Search the web for current information via Tavily (preferred) or Brave when configured.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"The search query\"}},"
            "\"required\":[\"query\"]}",
        .execute = tool_web_search_execute,
    };
    register_tool(&ws);

    /* Register get_current_time */
    mimi_tool_t gt = {
        .name = "get_current_time",
        .description = "Get the current date and time. Also sets the system clock. Call this when you need to know what time or date it is.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_get_time_execute,
    };
    register_tool(&gt);

    /* Register read_file */
    mimi_tool_t rf = {
        .name = "read_file",
        .description = "Read a file from SPIFFS storage. Path must start with " MIMI_SPIFFS_BASE "/.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " MIMI_SPIFFS_BASE "/\"}},"
            "\"required\":[\"path\"]}",
        .execute = tool_read_file_execute,
    };
    register_tool(&rf);

    /* Register write_file */
    mimi_tool_t wf = {
        .name = "write_file",
        .description = "Write or overwrite a file on SPIFFS storage. Path must start with " MIMI_SPIFFS_BASE "/.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " MIMI_SPIFFS_BASE "/\"},"
            "\"content\":{\"type\":\"string\",\"description\":\"File content to write\"}},"
            "\"required\":[\"path\",\"content\"]}",
        .execute = tool_write_file_execute,
    };
    register_tool(&wf);

    /* Register edit_file */
    mimi_tool_t ef = {
        .name = "edit_file",
        .description = "Find and replace text in a file on SPIFFS. Replaces first occurrence of old_string with new_string.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " MIMI_SPIFFS_BASE "/\"},"
            "\"old_string\":{\"type\":\"string\",\"description\":\"Text to find\"},"
            "\"new_string\":{\"type\":\"string\",\"description\":\"Replacement text\"}},"
            "\"required\":[\"path\",\"old_string\",\"new_string\"]}",
        .execute = tool_edit_file_execute,
    };
    register_tool(&ef);

    /* Register list_dir */
    mimi_tool_t ld = {
        .name = "list_dir",
        .description = "List files on SPIFFS storage, optionally filtered by path prefix.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"prefix\":{\"type\":\"string\",\"description\":\"Optional path prefix filter, e.g. " MIMI_SPIFFS_BASE "/memory/\"}},"
            "\"required\":[]}",
        .execute = tool_list_dir_execute,
    };
    register_tool(&ld);

    /* Register cron_add */
    mimi_tool_t ca = {
        .name = "cron_add",
        .description = "Schedule a recurring or one-shot task. The message will trigger an agent turn when the job fires.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Short name for the job\"},"
            "\"schedule_type\":{\"type\":\"string\",\"description\":\"'every' for recurring interval or 'at' for one-shot at a unix timestamp\"},"
            "\"interval_s\":{\"type\":\"integer\",\"description\":\"Interval in seconds (required for 'every')\"},"
            "\"at_epoch\":{\"type\":\"integer\",\"description\":\"Unix timestamp to fire at (required for 'at')\"},"
            "\"message\":{\"type\":\"string\",\"description\":\"Message to inject when the job fires, triggering an agent turn\"},"
            "\"channel\":{\"type\":\"string\",\"description\":\"Optional reply channel (e.g. 'telegram'). If omitted, current turn channel is used when available\"},"
            "\"chat_id\":{\"type\":\"string\",\"description\":\"Optional reply chat_id. Required when channel='telegram'. If omitted during a Telegram turn, current chat_id is used\"}"
            "},"
            "\"required\":[\"name\",\"schedule_type\",\"message\"]}",
        .execute = tool_cron_add_execute,
    };
    register_tool(&ca);

    /* Register cron_list */
    mimi_tool_t cl = {
        .name = "cron_list",
        .description = "List all scheduled cron jobs with their status, schedule, and IDs.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_cron_list_execute,
    };
    register_tool(&cl);

    /* Register cron_remove */
    mimi_tool_t cr = {
        .name = "cron_remove",
        .description = "Remove a scheduled cron job by its ID.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"job_id\":{\"type\":\"string\",\"description\":\"The 8-character job ID to remove\"}},"
            "\"required\":[\"job_id\"]}",
        .execute = tool_cron_remove_execute,
    };
    register_tool(&cr);

    /* Register GPIO tools */
    tool_gpio_init();

    mimi_tool_t gw = {
        .name = "gpio_write",
        .description = "Set a GPIO pin HIGH or LOW. Controls LEDs, relays, and other digital outputs.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"pin\":{\"type\":\"integer\",\"description\":\"GPIO pin number\"},"
            "\"state\":{\"type\":\"integer\",\"description\":\"1 for HIGH, 0 for LOW\"}},"
            "\"required\":[\"pin\",\"state\"]}",
        .execute = tool_gpio_write_execute,
    };
    register_tool(&gw);

    mimi_tool_t gr = {
        .name = "gpio_read",
        .description = "Read a GPIO pin state. Returns HIGH or LOW. Use for checking switches, sensors, and digital inputs.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"pin\":{\"type\":\"integer\",\"description\":\"GPIO pin number\"}},"
            "\"required\":[\"pin\"]}",
        .execute = tool_gpio_read_execute,
    };
    register_tool(&gr);

    mimi_tool_t ga = {
        .name = "gpio_read_all",
        .description = "Read all allowed GPIO pin states in a single call. Returns each pin's HIGH/LOW state.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_gpio_read_all_execute,
    };
    register_tool(&ga);

    /* Register WS2812 (addressable RGB LED) tool */
    tool_ws2812_init();

    mimi_tool_t ws2812 = {
        .name = "ws2812_set",
        .description = "Drive WS2812/SK6812 addressable RGB LEDs (including onboard RGB LEDs). "
                       "Sets all LEDs in the chain to one color. Use this instead of gpio_write for addressable LEDs, "
                       "because they need a precise data protocol and a plain HIGH/LOW does nothing.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"gpio\":{\"type\":\"integer\",\"description\":\"Data pin. Omit to use the device's configured default pin.\"},"
            "\"count\":{\"type\":\"integer\",\"description\":\"Number of LEDs in the chain (1-64). Omit to use the configured default.\"},"
            "\"color\":{\"type\":\"string\",\"description\":\"#RRGGBB hex or a name: white, red, green, blue, yellow, cyan, magenta, orange, warm, cool, off\"},"
            "\"brightness\":{\"type\":\"integer\",\"description\":\"0-255, default 255\"}"
            "},"
            "\"required\":[\"color\"]}",
        .execute = tool_ws2812_set_execute,
    };
    register_tool(&ws2812);

    mimi_tool_t ws2812_blink = {
        .name = "ws2812_blink",
        .description = "Start a PERSISTENT blink on WS2812/SK6812 LEDs. The device keeps blinking by itself "
                       "in the background, so a single call is enough — do NOT call it repeatedly. "
                       "Use this whenever the user asks for 闪烁/blink/flash/呼吸灯/闪灯. "
                       "Use ws2812_stop to end it. A plain ws2812_set also ends a running blink.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"gpio\":{\"type\":\"integer\",\"description\":\"Data pin. Omit to use the device's configured default pin.\"},"
            "\"count\":{\"type\":\"integer\",\"description\":\"Number of LEDs in the chain (1-64). Omit for default.\"},"
            "\"color\":{\"type\":\"string\",\"description\":\"#RRGGBB hex or a name: white, red, green, blue, yellow, cyan, magenta, orange, warm, cool\"},"
            "\"brightness\":{\"type\":\"integer\",\"description\":\"0-255, default 255\"},"
            "\"interval_ms\":{\"type\":\"integer\",\"description\":\"On time and off time in milliseconds, each. 500 = one blink per second. Default 500.\"},"
            "\"cycles\":{\"type\":\"integer\",\"description\":\"Number of blink cycles, 0 = keep blinking until stopped. Default 0.\"}"
            "},"
            "\"required\":[\"color\"]}",
        .execute = tool_ws2812_blink_execute,
    };
    register_tool(&ws2812_blink);

    mimi_tool_t ws2812_stop = {
        .name = "ws2812_stop",
        .description = "Stop the persistent blink started by ws2812_blink and turn the LEDs off.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_ws2812_stop_execute,
    };
    register_tool(&ws2812_stop);

    /* Register servo (LEDC PWM) tool */
    tool_servo_init();

    mimi_tool_t servo = {
        .name = "servo_set",
        .description = "Rotate a hobby servo (SG90/MG996R etc.) to an absolute angle using PWM. "
                       "Angle 0-180 degrees. Use this instead of gpio_write: a servo needs a 50Hz PWM pulse, not a steady level.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"angle\":{\"type\":\"integer\",\"description\":\"Target angle in degrees, 0-180 (90 = center)\"},"
            "\"gpio\":{\"type\":\"integer\",\"description\":\"Servo signal pin. Omit to use the device's configured default pin.\"}"
            "},"
            "\"required\":[\"angle\"]}",
        .execute = tool_servo_set_execute,
    };
    register_tool(&servo);

    /* Register generic bus primitives (I2C / SPI / UART / ADC / RMT) */
    tool_i2c_init();
    tool_spi_init();
    tool_uart_init();
    tool_adc_init();
    tool_rmt_init();

    mimi_tool_t i2c = {
        .name = "i2c",
        .description = "I2C master. op=scan lists devices on the bus; op=read/op=write move hex bytes. "
                       "Covers most sensors, RTCs and OLEDs. Default bus is SDA=8/SCL=9 at 100 kHz.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"op\":{\"type\":\"string\",\"description\":\"scan | read | write\"},"
            "\"sda\":{\"type\":\"integer\",\"description\":\"SDA pin, default 8\"},"
            "\"scl\":{\"type\":\"integer\",\"description\":\"SCL pin, default 9\"},"
            "\"port\":{\"type\":\"integer\",\"description\":\"0 or 1, default 0\"},"
            "\"freq\":{\"type\":\"integer\",\"description\":\"Clock Hz, default 100000\"},"
            "\"addr\":{\"type\":\"integer\",\"description\":\"7-bit address, required for read/write\"},"
            "\"reg\":{\"type\":\"string\",\"description\":\"Optional register byte in hex, e.g. \\\"0F\\\"\"},"
            "\"data\":{\"type\":\"string\",\"description\":\"Bytes to write in hex, e.g. \\\"01 02\\\"\"},"
            "\"len\":{\"type\":\"integer\",\"description\":\"Bytes to read, default 4 (max 32)\"}"
            "},"
            "\"required\":[\"op\"]}",
        .execute = tool_i2c_execute,
    };
    register_tool(&i2c);

    mimi_tool_t spi = {
        .name = "spi",
        .description = "SPI master transfer. Sends hex bytes over SPI2 and returns the received bytes.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"tx\":{\"type\":\"string\",\"description\":\"Bytes to send in hex, e.g. \\\"01 02 03\\\"\"},"
            "\"mosi\":{\"type\":\"integer\",\"description\":\"MOSI pin, default 11\"},"
            "\"miso\":{\"type\":\"integer\",\"description\":\"MISO pin, default 13 (omit -1 for write-only)\"},"
            "\"sclk\":{\"type\":\"integer\",\"description\":\"SCLK pin, default 12\"},"
            "\"cs\":{\"type\":\"integer\",\"description\":\"CS pin, default 10\"},"
            "\"mode\":{\"type\":\"integer\",\"description\":\"SPI mode 0-3, default 0\"},"
            "\"freq\":{\"type\":\"integer\",\"description\":\"Clock Hz, default 1000000\"},"
            "\"bits\":{\"type\":\"integer\",\"description\":\"Optional total bit count, default 8 * byte count\"}"
            "},"
            "\"required\":[\"tx\"]}",
        .execute = tool_spi_execute,
    };
    register_tool(&spi);

    mimi_tool_t uart = {
        .name = "uart",
        .description = "UART1/UART2 (UART0 is the device console). op=setup binds pins and baud once, "
                       "then op=write sends text and op=read returns received bytes as hex.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"op\":{\"type\":\"string\",\"description\":\"setup | write | read\"},"
            "\"port\":{\"type\":\"integer\",\"description\":\"1 or 2, default 1\"},"
            "\"tx\":{\"type\":\"integer\",\"description\":\"TX pin (setup)\"},"
            "\"rx\":{\"type\":\"integer\",\"description\":\"RX pin (setup)\"},"
            "\"baud\":{\"type\":\"integer\",\"description\":\"Baud rate, default 115200\"},"
            "\"data\":{\"type\":\"string\",\"description\":\"Text to send (write)\"},"
            "\"timeout_ms\":{\"type\":\"integer\",\"description\":\"Read timeout, default 500\"},"
            "\"len\":{\"type\":\"integer\",\"description\":\"Max bytes to read, default 64\"}"
            "},"
            "\"required\":[\"op\"]}",
        .execute = tool_uart_execute,
    };
    register_tool(&uart);

    mimi_tool_t adc = {
        .name = "adc",
        .description = "Read an analog pin (12-bit, 0-3.3V). ESP32-S3 ADC pins are GPIO1-10 and GPIO11-14.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"pin\":{\"type\":\"integer\",\"description\":\"ADC-capable GPIO, default 4\"},"
            "\"samples\":{\"type\":\"integer\",\"description\":\"Averaging samples, default 1 (max 32)\"}"
            "},"
            "\"required\":[\"pin\"]}",
        .execute = tool_adc_execute,
    };
    register_tool(&adc);

    mimi_tool_t rmt = {
        .name = "rmt",
        .description = "Raw RMT timing engine. op=pulse sends an exact level/duration train (infrared, ultrasonic trigger, "
                       "custom LEDs); op=capture records the edges of an incoming signal (DHT, IR receiver, one-wire) "
                       "and returns level:microseconds pairs.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"op\":{\"type\":\"string\",\"description\":\"pulse | capture\"},"
            "\"gpio\":{\"type\":\"integer\",\"description\":\"Signal pin\"},"
            "\"symbols\":{\"type\":\"array\",\"description\":\"pulse only: [{level:0|1, us:number}, ...]\","
            "\"items\":{\"type\":\"object\",\"properties\":{\"level\":{\"type\":\"integer\"},\"us\":{\"type\":\"integer\"}}}},"
            "\"idle_us\":{\"type\":\"integer\",\"description\":\"capture: signal ends after this much silence, default 5000\"},"
            "\"max_symbols\":{\"type\":\"integer\",\"description\":\"capture: max edges, default 64\"},"
            "\"timeout_ms\":{\"type\":\"integer\",\"description\":\"capture: wait time, default 200\"}"
            "},"
            "\"required\":[\"op\",\"gpio\"]}",
        .execute = tool_rmt_execute,
    };
    register_tool(&rmt);

    /* Register the device table */
    tool_device_init();

    mimi_tool_t dev_define = {
        .name = "device_define",
        .description = "Register or update a named device so later calls can use its name instead of pins. "
                       "type: ws2812 | servo | gpio | adc | i2c | spi | uart | rmt. "
                       "Store every extra key the driver needs (pin, count, sda, scl, addr, port, tx, rx, baud...).",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Short ASCII device name, e.g. led or servo1 (avoid non-ASCII characters)\"},"
            "\"type\":{\"type\":\"string\",\"description\":\"ws2812 | servo | gpio | adc | i2c | spi | uart | rmt\"}"
            "},"
            "\"required\":[\"name\",\"type\"]}",
        .execute = tool_device_define_execute,
    };
    register_tool(&dev_define);

    mimi_tool_t dev_list = {
        .name = "device_list",
        .description = "List all defined devices with their type and key pins.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_device_list_execute,
    };
    register_tool(&dev_list);

    mimi_tool_t dev_do = {
        .name = "device_do",
        .description = "PREFERRED way to drive hardware: run an action on a named device and skip pin bookkeeping. "
                       "Actions by type — ws2812: on/off/set(color)/blink/stop; servo: set(angle in value); "
                       "gpio: on/off/toggle/read; adc: read; i2c: scan/read/write; spi: transfer; "
                       "uart: setup/write/read; rmt: pulse/capture. Extra keys are passed through.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Device name from device_list (ASCII)\"},"
            "\"action\":{\"type\":\"string\",\"description\":\"Action for that device type\"},"
            "\"value\":{\"description\":\"Generic value: color for ws2812, angle for servo, 0/1 for gpio, text for uart\"},"
            "\"interval_ms\":{\"type\":\"integer\",\"description\":\"Blink on/off time (ws2812 blink)\"},"
            "\"cycles\":{\"type\":\"integer\",\"description\":\"Blink cycles, 0 = forever\"}"
            "},"
            "\"required\":[\"name\",\"action\"]}",
        .execute = tool_device_do_execute,
    };
    register_tool(&dev_do);

    build_tools_json();

    ESP_LOGI(TAG, "Tool registry initialized");
    return ESP_OK;
}

const char *tool_registry_get_tools_json(void)
{
    return s_tools_json;
}

esp_err_t tool_registry_execute(const char *name, const char *input_json,
                                char *output, size_t output_size)
{
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            ESP_LOGI(TAG, "Executing tool: %s", name);
            return s_tools[i].execute(input_json, output, output_size);
        }
    }

    ESP_LOGW(TAG, "Unknown tool: %s", name);
    snprintf(output, output_size, "Error: unknown tool '%s'", name);
    return ESP_ERR_NOT_FOUND;
}
