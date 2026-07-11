/* Hardware test — ESP32-C3 + 0.42" OLED + traffic light module (ESP-IDF port)
 * ------------------------------------------------------------
 * No WiFi, no proxy. Cycles RED -> YELLOW -> GREEN once per second
 * and shows the active color + a counter on the OLED.
 *
 * Pass criteria:
 *   - all three LEDs light up one by one, in the right order
 *   - OLED shows "TEST", the color name and an incrementing counter
 *
 * Board: ESP32-C3, native USB Serial/JTAG console (sdkconfig.defaults).
 * OLED: SSD1306 72x40 "ER" panel, I2C SDA=GPIO5 SCL=GPIO6, addr 0x3C.
 * ------------------------------------------------------------
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "u8g2.h"

static const char *TAG = "hw_test";

// ---- pins (confirm against your board's pinout) ----
#define PIN_RED    3
#define PIN_YELLOW 4
#define PIN_GREEN  10
#define ACTIVE_HIGH true // common cathode module

// This board's OLED: SCL=6, SDA=5.
#define I2C_SDA_IO 5
#define I2C_SCL_IO 6
#define I2C_DISPLAY_ADDRESS 0x3C
#define I2C_FREQ_HZ 100000 // extra noise margin; bus corruption was traced to LED wiring/grounding, not clock speed
#define I2C_TIMEOUT_MS 1000

static i2c_master_bus_handle_t i2c_bus_handle = NULL;
static i2c_master_dev_handle_t display_dev_handle = NULL;

// u8x8 I2C byte transport callback — see u8g2 porting guide.
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

// u8x8 GPIO/delay callback — no reset line on this board, only timing.
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

static const char *names[3] = { "RED", "YELLOW", "GREEN" };
static const int   pins[3]  = { PIN_RED, PIN_YELLOW, PIN_GREEN };

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
    for (int i = 0; i < 3; i++) {
        gpio_set_level(pins[i], ACTIVE_HIGH ? 0 : 1); // all off
    }
}

static void init_display(u8g2_t *u8g2)
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

    u8g2_Setup_ssd1306_i2c_72x40_er_f(u8g2, U8G2_R2, u8x8_byte_i2c_cb, u8x8_gpio_delay_cb); // R2 = rotated 180deg
    u8g2_InitDisplay(u8g2);
    u8g2_SetPowerSave(u8g2, 0);
    u8g2_SetContrast(u8g2, 255);
}

void app_main(void)
{
    init_leds();

    static u8g2_t u8g2;
    init_display(&u8g2);

    int step = 0;
    unsigned long counter = 0;

    while (1) {
        // light only the current color
        for (int i = 0; i < 3; i++) {
            gpio_set_level(pins[i], ((i == step) == ACTIVE_HIGH) ? 1 : 0);
        }

        // OLED status
        u8g2_ClearBuffer(&u8g2);
        u8g2_SetFont(&u8g2, u8g2_font_6x10_tf);
        u8g2_DrawStr(&u8g2, 0, 10, "TEST");
        u8g2_SetFont(&u8g2, u8g2_font_10x20_tf);
        u8g2_DrawStr(&u8g2, 0, 28, names[step]);
        u8g2_SetFont(&u8g2, u8g2_font_6x10_tf);
        char buf[16];
        snprintf(buf, sizeof(buf), "n=%lu", counter);
        u8g2_DrawStr(&u8g2, 0, 39, buf);
        u8g2_SendBuffer(&u8g2);

        printf("step=%s counter=%lu\n", names[step], counter);

        step = (step + 1) % 3;
        counter++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
