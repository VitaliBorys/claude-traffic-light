/* Claude Traffic Light — ESP32-C3 + 0.42" OLED (ESP-IDF port, final version)
 * ------------------------------------------------------------
 * Polls the local proxy (GET /util), drives the traffic-light module
 * (common cathode, 3 signals + GND) and shows the 5h-window percentage
 * and countdown-to-reset on the built-in OLED.
 *
 * Board: "ESP32-C3 0.42 OLED" (ABRobot / 01Space style).
 *   OLED over I2C: SDA=GPIO5, SCL=GPIO6, SSD1306 72x40, addr 0x3C.
 *   GPIO8 (onboard LED) and GPIO9 (BOOT) are also in use — left alone.
 *
 * Configure WiFi SSID/password and the proxy URL via `idf.py menuconfig`
 * under "Example Configuration".
 * ------------------------------------------------------------
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_http_client.h"
#include "u8g2.h"

static const char *TAG = "traffic_light";

// ---------------- SETTINGS ----------------
#define WIFI_SSID     CONFIG_EXAMPLE_WIFI_SSID
#define WIFI_PASS     CONFIG_EXAMPLE_WIFI_PASSWORD
#define PROXY_URL     CONFIG_EXAMPLE_PROXY_URL

// Free pins (not used by OLED 5/6, onboard LED 8, BOOT 9)
#define PIN_RED    3
#define PIN_YELLOW 4
#define PIN_GREEN  10

#define ACTIVE_HIGH true // common cathode module

#define POLL_MS        20000 // proxy poll period
#define ERROR_BLINK_MS 1000   // OFFLINE (errors/connection) blink period: 1s on / 1s off
#define DISPLAY_MS     30000  // OLED page swap period (5h <-> 7d)
// --------------------------------------------

// This board's OLED: SCL=6, SDA=5, no offset.
#define I2C_SDA_IO 5
#define I2C_SCL_IO 6
#define I2C_DISPLAY_ADDRESS 0x3C
#define I2C_FREQ_HZ 100000 // extra noise margin; bus corruption was traced to LED wiring/grounding, not clock speed
#define I2C_TIMEOUT_MS 1000

static i2c_master_bus_handle_t i2c_bus_handle = NULL;
static i2c_master_dev_handle_t display_dev_handle = NULL;

typedef enum { MODE_GREEN, MODE_YELLOW, MODE_RED, MODE_OFFLINE } light_mode_t;
static light_mode_t mode = MODE_OFFLINE;

static float util5h = 0;   // 0..1
static float util7d = 0;   // 0..1
static int   reset5h = -1; // seconds until reset, -1 = unknown
static int   reset7d = -1; // seconds until reset, -1 = unknown
static bool  online = false;
static bool  blocked = false; // true = proxy reports rate_limited/exceeded (shows "LIMIT")

static bool error_blink_on = false; // OFFLINE cadence
static bool show_weekly = false;    // OLED page: false=5h, true=7d

// ---------------- WiFi ----------------
static volatile bool wifi_connected = false;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        wifi_connected = true;
    }
}

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Disable modem sleep: periodic radio wake for beacons otherwise seems to
    // glitch the I2C bus to the OLED (SDA/SCL run right next to the antenna).
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

// Waits up to timeout_ms for a WiFi connection (mirrors original 15s wait).
static void connect_wifi(int timeout_ms)
{
    int64_t t0 = esp_timer_get_time();
    while (!wifi_connected && (esp_timer_get_time() - t0) / 1000 < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

// ---------------- OLED (U8g2 over new i2c_master driver) ----------------
static uint8_t u8x8_byte_i2c_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    static uint8_t buffer[132];
    static uint8_t buf_idx;

    switch (msg) {
    case U8X8_MSG_BYTE_INIT: {
        i2c_device_config_t dev_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = I2C_DISPLAY_ADDRESS,
            .scl_speed_hz = I2C_FREQ_HZ,
            .scl_wait_us = 0,
            .flags.disable_ack_check = false,
        };
        if (i2c_master_bus_add_device(i2c_bus_handle, &dev_config, &display_dev_handle) != ESP_OK) {
            ESP_LOGE(TAG, "failed to add display to I2C bus");
            return 0;
        }
        break;
    }
    case U8X8_MSG_BYTE_START_TRANSFER:
        buf_idx = 0;
        break;
    case U8X8_MSG_BYTE_SET_DC:
        break;
    case U8X8_MSG_BYTE_SEND:
        for (size_t i = 0; i < arg_int; ++i) {
            buffer[buf_idx++] = *((uint8_t *)arg_ptr + i);
        }
        break;
    case U8X8_MSG_BYTE_END_TRANSFER:
        if (buf_idx > 0 && display_dev_handle != NULL) {
            if (i2c_master_transmit(display_dev_handle, buffer, buf_idx, I2C_TIMEOUT_MS) != ESP_OK) {
                ESP_LOGE(TAG, "I2C transmit failed");
                return 0;
            }
        }
        break;
    default:
        return 0;
    }
    return 1;
}

static uint8_t u8x8_gpio_delay_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    switch (msg) {
    case U8X8_MSG_GPIO_AND_DELAY_INIT:
        break;
    case U8X8_MSG_DELAY_MILLI:
        vTaskDelay(pdMS_TO_TICKS(arg_int));
        break;
    case U8X8_MSG_DELAY_10MICRO:
        esp_rom_delay_us(arg_int * 10);
        break;
    case U8X8_MSG_DELAY_100NANO:
        __asm__ __volatile__("nop");
        break;
    case U8X8_MSG_DELAY_I2C:
        esp_rom_delay_us(5 / arg_int);
        break;
    case U8X8_MSG_GPIO_RESET:
        break;
    default:
        return 0;
    }
    return 1;
}

static u8g2_t u8g2;

static void init_display(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_IO,
        .scl_io_num = I2C_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &i2c_bus_handle));

    u8g2_Setup_ssd1306_i2c_72x40_er_f(&u8g2, U8G2_R2, u8x8_byte_i2c_cb, u8x8_gpio_delay_cb); // R2 = rotated 180deg
    u8g2_InitDisplay(&u8g2);
    u8g2_SetPowerSave(&u8g2, 0);
    u8g2_SetContrast(&u8g2, 255);
}

// ---------------- LEDs ----------------
static void write_led(int pin, bool on) { gpio_set_level(pin, (on == ACTIVE_HIGH) ? 1 : 0); }
static void set_lights(bool r, bool y, bool g)
{
    write_led(PIN_RED, r);
    write_led(PIN_YELLOW, y);
    write_led(PIN_GREEN, g);
}

static void init_leds(void)
{
    uint64_t mask = (1ULL << PIN_RED) | (1ULL << PIN_YELLOW) | (1ULL << PIN_GREEN);
    gpio_config_t io_conf = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    set_lights(false, false, false);
}

// ---------------- Tiny hand-rolled JSON field extraction ----------------
// Mirrors the original Arduino jsonNumber()/jsonHas() helpers: no library,
// just enough to pull a handful of known fields out of a flat JSON object.
static float json_number(const char *s, const char *key, bool *ok)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(s, pat);
    if (!p) { *ok = false; return 0; }
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p == '\0' || *p == 'n') { *ok = false; return 0; } // null
    *ok = true;
    return strtof(p, NULL);
}
static bool json_has(const char *s, const char *key, const char *value)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":\"%s\"", key, value);
    return strstr(s, pat) != NULL;
}

// ---------------- HTTP poll ----------------
#define HTTP_BUF_SIZE 512
typedef struct { char buf[HTTP_BUF_SIZE]; int len; } http_resp_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_t *r = (http_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int room = HTTP_BUF_SIZE - 1 - r->len;
        int n = evt->data_len < room ? evt->data_len : room;
        if (n > 0) {
            memcpy(r->buf + r->len, evt->data, n);
            r->len += n;
            r->buf[r->len] = '\0';
        }
    }
    return ESP_OK;
}

static void draw_screen(void)
{
    u8g2_ClearBuffer(&u8g2);
    if (!online) {
        u8g2_SetFont(&u8g2, u8g2_font_6x10_tf);
        u8g2_DrawStr(&u8g2, 0, 12, "Claude");
        u8g2_DrawStr(&u8g2, 0, 26, "no data");
    } else {
        char buf[20];
        float u = show_weekly ? util7d : util5h;
        u8g2_SetFont(&u8g2, u8g2_font_6x10_tf);
        u8g2_DrawStr(&u8g2, 0, 8, show_weekly ? "Claude 7d" : "Claude 5h");
        u8g2_SetFont(&u8g2, u8g2_font_10x20_tf);
        snprintf(buf, sizeof(buf), "%d%%", (int)(u * 100 + 0.5f));
        u8g2_DrawStr(&u8g2, 0, 28, buf);
        u8g2_SetFont(&u8g2, u8g2_font_6x10_tf);
        if (blocked) {
            u8g2_DrawStr(&u8g2, 0, 39, "LIMIT");
        } else {
            int reset = show_weekly ? reset7d : reset5h;
            if (reset >= 0) {
                int d = reset / 86400;
                int h = (reset % 86400) / 3600;
                if (show_weekly) snprintf(buf, sizeof(buf), "reset %dd%02dh", d, h);
                else             snprintf(buf, sizeof(buf), "reset %dh%02dm", h, (reset % 3600) / 60);
                u8g2_DrawStr(&u8g2, 0, 39, buf);
            } else {
                u8g2_DrawStr(&u8g2, 0, 39, "reset --");
            }
        }
    }
    u8g2_SendBuffer(&u8g2);
}

static void poll_proxy(void)
{
    if (!wifi_connected) connect_wifi(15000);

    http_resp_t resp = { .len = 0 };
    resp.buf[0] = '\0';

    esp_http_client_config_t config = {
        .url = PROXY_URL,
        .timeout_ms = 4000,
        .event_handler = http_event_handler,
        .user_data = &resp,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK && esp_http_client_get_status_code(client) == 200) {
        bool ok, okr, ok7, okr7;
        float u = json_number(resp.buf, "util_5h", &ok);
        float r = json_number(resp.buf, "reset_in_5h", &okr);
        float u7 = json_number(resp.buf, "util_7d", &ok7);
        float r7 = json_number(resp.buf, "reset_in_7d", &okr7);
        bool is_blocked = json_has(resp.buf, "status_5h", "rate_limited") ||
                          json_has(resp.buf, "status_5h", "exceeded") ||
                          json_has(resp.buf, "status_7d", "rate_limited") ||
                          json_has(resp.buf, "status_7d", "exceeded");
        if (!ok) {
            online = false; mode = MODE_OFFLINE; // no data yet
        } else {
            online = true;
            util5h = u;
            util7d = ok7 ? u7 : 0;
            reset5h = okr ? (int)r : -1;
            reset7d = okr7 ? (int)r7 : -1;
            blocked = is_blocked;
            if (blocked || u >= 1.0f || util7d >= 1.0f)   mode = MODE_RED; // solid red for both
            else if (u >= 0.75f || util7d >= 0.75f)        mode = MODE_YELLOW;
            else                                           mode = MODE_GREEN;
        }
        ESP_LOGI(TAG, "util_5h=%.2f util_7d=%.2f reset5h=%ds reset7d=%ds -> mode=%d",
                 online ? util5h : -1, online ? util7d : -1, reset5h, reset7d, mode);
    } else {
        online = false; mode = MODE_OFFLINE;
        ESP_LOGW(TAG, "HTTP err=%d status=%d (proxy unreachable)", err,
                 err == ESP_OK ? esp_http_client_get_status_code(client) : -1);
    }
    esp_http_client_cleanup(client);
    draw_screen();
}

static void render(void)
{
    switch (mode) {
    case MODE_GREEN:     set_lights(false, false, true); break;
    case MODE_YELLOW:    set_lights(false, true, false); break;
    case MODE_RED:       set_lights(true, false, false); break;
    case MODE_OFFLINE:   set_lights(false, error_blink_on, false); break; // 1s on/1s off yellow = no data/error
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    init_leds();
    init_display();
    draw_screen();

    wifi_init_sta();
    connect_wifi(15000);
    poll_proxy();

    int64_t last_poll = esp_timer_get_time() / 1000;
    int64_t last_error_blink = last_poll;
    int64_t last_display_switch = last_poll;

    while (1) {
        int64_t now = esp_timer_get_time() / 1000;
        if (now - last_poll >= POLL_MS) { poll_proxy(); last_poll = now; }
        if (now - last_error_blink >= ERROR_BLINK_MS) { error_blink_on = !error_blink_on; last_error_blink = now; }
        if (now - last_display_switch >= DISPLAY_MS) {
            show_weekly = !show_weekly;
            last_display_switch = now;
            if (online) draw_screen();
        }
        render();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
