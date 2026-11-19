// Pantalla Guition ESP32-S3-4848S040: ST7701 (init por SPI de 3 hilos) + bus RGB paralelo 16 bits
#include <string.h>
#include "esp_log.h"
#include "esp_cache.h"
#include "esp_rom_sys.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "lcd_rgb.h"

#define PIN_BL      38
#define PIN_ST_CS   39
#define PIN_ST_SCK  48
#define PIN_ST_SDA  47
#define PIN_HSYNC   16
#define PIN_VSYNC   17
#define PIN_DE      18
#define PIN_PCLK    21
/* D0..D4 = B0..B4, D5..D10 = G0..G5, D11..D15 = R0..R4 */
#define DATA_PINS   { 4, 5, 6, 7, 15,  8, 20, 3, 46, 9, 10,  11, 12, 13, 14, 0 }

static const char *TAG = "lcd";
static uint16_t *fb;

/* ---- ST7701: SPI de 3 hilos por bit-bang (9 bits: bit D/C + 8 de dato) ---- */
static void st_write(bool data, uint8_t v)
{
    uint16_t w = (data ? 0x100 : 0) | v;
    gpio_set_level(PIN_ST_CS, 0);
    for (int i = 8; i >= 0; i--) {
        gpio_set_level(PIN_ST_SCK, 0);
        gpio_set_level(PIN_ST_SDA, (w >> i) & 1);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_ST_SCK, 1);
        esp_rom_delay_us(1);
    }
    gpio_set_level(PIN_ST_CS, 1);
    esp_rom_delay_us(2);
}
static void st_cmd(uint8_t cmd, const uint8_t *args, size_t n)
{
    st_write(false, cmd);
    for (size_t i = 0; i < n; i++) st_write(true, args[i]);
}
#define CMD(c, ...) do { const uint8_t a[] = { __VA_ARGS__ }; st_cmd(c, a, sizeof(a)); } while (0)
#define CMD0(c) st_cmd(c, NULL, 0)

/* Secuencia base ST7701S de ESPHome (mipi_rgb/models/st7701s.py) + ajuste GUITION-4848S040 (CD 00) */
static void st7701_init(void)
{
    gpio_config_t io = { .pin_bit_mask = (1ULL << PIN_ST_CS) | (1ULL << PIN_ST_SCK) | (1ULL << PIN_ST_SDA),
                         .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
    gpio_set_level(PIN_ST_CS, 1);
    gpio_set_level(PIN_ST_SCK, 1);

    CMD0(0x01);
    vTaskDelay(pdMS_TO_TICKS(150));
    CMD(0xFF, 0x77, 0x01, 0x00, 0x00, 0x10);
    CMD(0xC0, 0x3B, 0x00);
    CMD(0xC1, 0x0D, 0x02);
    CMD(0xC2, 0x31, 0x05);
    CMD(0xB0, 0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08, 0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18);
    CMD(0xB1, 0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08, 0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18);
    CMD(0xFF, 0x77, 0x01, 0x00, 0x00, 0x11);
    CMD(0xB0, 0x60);
    CMD(0xB1, 0x32);
    CMD(0xB2, 0x07);
    CMD(0xB3, 0x80);
    CMD(0xB5, 0x49);
    CMD(0xB7, 0x85);
    CMD(0xB8, 0x21);
    CMD(0xC1, 0x78);
    CMD(0xC2, 0x78);
    CMD(0xE0, 0x00, 0x1B, 0x02);
    CMD(0xE1, 0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x00, 0x44, 0x44);
    CMD(0xE2, 0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00, 0xEC, 0xA0, 0x00, 0x00);
    CMD(0xE3, 0x00, 0x00, 0x11, 0x11);
    CMD(0xE4, 0x44, 0x44);
    CMD(0xE5, 0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0, 0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0);
    CMD(0xE6, 0x00, 0x00, 0x11, 0x11);
    CMD(0xE7, 0x44, 0x44);
    CMD(0xE8, 0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0, 0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0);
    CMD(0xEB, 0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40);
    CMD(0xEC, 0x3C, 0x00);
    CMD(0xED, 0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA);
    CMD(0xFF, 0x77, 0x01, 0x00, 0x00, 0x13);
    CMD(0xE5, 0xE4);
    CMD(0xFF, 0x77, 0x01, 0x00, 0x00, 0x10);
    CMD(0xCD, 0x00);
    CMD(0xC7, CONFIG_LCD_MIRROR_X ? 0x04 : 0x00);
    CMD(0x36, CONFIG_LCD_MIRROR_Y ? 0x10 : 0x00);
    CMD(0x3A, 0x60);
    CMD0(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));
    CMD0(0x29);
    vTaskDelay(pdMS_TO_TICKS(30));
}

void lcd_init(void)
{
    if (fb) return;
    gpio_config_t bl = { .pin_bit_mask = 1ULL << PIN_BL, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&bl);
    gpio_set_level(PIN_BL, 0);

    st7701_init();

    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = 12 * 1000 * 1000,
            .h_res = LCD_W, .v_res = LCD_H,
            .hsync_pulse_width = 8, .hsync_back_porch = 20, .hsync_front_porch = 10,
            .vsync_pulse_width = 8, .vsync_back_porch = 10, .vsync_front_porch = 10,
            .flags = { .pclk_active_neg = 0 },
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .bounce_buffer_size_px = LCD_W * 20,
        .hsync_gpio_num = PIN_HSYNC, .vsync_gpio_num = PIN_VSYNC,
        .de_gpio_num = PIN_DE, .pclk_gpio_num = PIN_PCLK, .disp_gpio_num = -1,
        .data_gpio_nums = DATA_PINS,
        .flags = { .fb_in_psram = 1 },
    };
    esp_lcd_panel_handle_t panel;
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    void *p;
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel, 1, &p));
    fb = p;
    lcd_fill(0);
    gpio_set_level(PIN_BL, 1);
    ESP_LOGI(TAG, "LCD 480x480 listo");
}

uint16_t *lcd_fb(void) { return fb; }

void lcd_flush_rows(int y0, int y1)
{
    if (y0 < 0) y0 = 0;
    if (y1 > LCD_H) y1 = LCD_H;
    if (y1 <= y0) return;
    esp_cache_msync(fb + y0 * LCD_W, (size_t)(y1 - y0) * LCD_W * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

void lcd_fill(uint16_t color)
{
    for (int i = 0; i < LCD_W * LCD_H; i++) fb[i] = color;
    lcd_flush_rows(0, LCD_H);
}

void lcd_draw_game(const uint8_t **lines, const uint16_t *pal)
{
    static uint16_t xmap_ready, xmap[GAME_W];
    static uint16_t rowbuf[GAME_W];
    if (!xmap_ready) {
        for (int x = 0; x < GAME_W; x++) xmap[x] = x * 256 / GAME_W;
        xmap_ready = 1;
    }
    int prev_sy = -1;
    for (int y = 0; y < GAME_H; y++) {
        int sy = y * 240 / GAME_H;
        uint16_t *dst = fb + (GAME_Y + y) * LCD_W + GAME_X;
        if (sy == prev_sy) {
            memcpy(dst, dst - LCD_W, GAME_W * 2);        /* misma linea de origen: copiar la anterior */
        } else {
            const uint8_t *src = lines[sy];
            for (int x = 0; x < GAME_W; x++) rowbuf[x] = pal[src[xmap[x]]];
            memcpy(dst, rowbuf, GAME_W * 2);
            prev_sy = sy;
        }
    }
    lcd_flush_rows(GAME_Y, GAME_Y + GAME_H);
}
