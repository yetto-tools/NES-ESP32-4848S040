// Entrada tactil: GT911 por I2C, multitoque (hasta 5 dedos), zonas de los controles de ui.h
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "nvs.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "gamepad.h"
#include "lcd_rgb.h"
#include "ui.h"

static const char *TAG = "touch";
static i2c_master_dev_handle_t dev;
static volatile uint32_t state;
static volatile int tdown, tx, ty;      /* primer dedo (para el menu), ya calibrado */
static volatile int rawx, rawy;         /* primer dedo, coordenadas crudas del GT911 */

/* Calibracion: pantalla = a * crudo + b, con intercambio de ejes opcional */
typedef struct { int32_t swap; float ax, bx, ay, by; } tcal_t;
static tcal_t cal = { 0, 1.0f, 0.0f, 1.0f, 0.0f };

static void cal_map(int rx, int ry, int *x, int *y)
{
    float u = cal.swap ? ry : rx, v = cal.swap ? rx : ry;
    int px = (int)(cal.ax * u + cal.bx), py = (int)(cal.ay * v + cal.by);
    if (px < 0) px = 0; if (px > LCD_W - 1) px = LCD_W - 1;
    if (py < 0) py = 0; if (py > LCD_H - 1) py = LCD_H - 1;
    *x = px; *y = py;
}

static esp_err_t gt_read(uint16_t reg, uint8_t *buf, size_t n)
{
    uint8_t r[2] = { reg >> 8, reg & 0xFF };
    return i2c_master_transmit_receive(dev, r, 2, buf, n, 50);
}
static esp_err_t gt_write8(uint16_t reg, uint8_t v)
{
    uint8_t b[3] = { reg >> 8, reg & 0xFF, v };
    return i2c_master_transmit(dev, b, 3, 50);
}

static inline int sq(int v) { return v * v; }

/* Convierte un punto de pantalla en botones presionados */
static uint32_t hit(int x, int y)
{
    uint32_t m = 0;
    int dx = x - DPAD_CX, dy = y - DPAD_CY;
    if (sq(dx) + sq(dy) <= sq(DPAD_R + 12)) {
        if (dx < -DPAD_DEAD) m |= 1u << BTN_LEFT;
        if (dx >  DPAD_DEAD) m |= 1u << BTN_RIGHT;
        if (dy < -DPAD_DEAD) m |= 1u << BTN_UP;
        if (dy >  DPAD_DEAD) m |= 1u << BTN_DOWN;
    }
    int da = sq(x - A_CX) + sq(y - A_CY), db = sq(x - B_CX) + sq(y - B_CY);
    const int rr = sq(BTN_R + 10);
    if (da <= rr && da <= db) m |= 1u << BTN_A;
    if (db <= rr && db <= da) m |= 1u << BTN_B;
    const int s = 6;   /* margen extra para los botones pequeños */
    if (x >= SEL_X - s && x < SEL_X + PILL_W + s && y >= SEL_Y - s && y < SEL_Y + PILL_H + s) m |= 1u << BTN_SELECT;
    if (x >= STA_X - s && x < STA_X + PILL_W + s && y >= STA_Y - s && y < STA_Y + PILL_H + s) m |= 1u << BTN_START;
    if (x >= MENU_X - s && x < MENU_X + PILL_W + s && y >= MENU_Y - s && y < MENU_Y + PILL_H + s) m |= 1u << BTN_MENU;
    return m;
}

/* El GT911 a veces entrega tramas vacias con el dedo puesto: se suelta el boton solo si pasan
 * RELEASE_US sin ningun punto, para que no parpadee (y los toques largos cuenten como largos) */
#define RELEASE_US 120000

static void touch_task(void *arg)
{
    int64_t last_touch = 0;
    uint32_t prev = 0xFFFFFFFF;
    int was_down = 0;
    for (;;) {
        uint8_t st = 0;
        uint32_t m = state;
        if (gt_read(0x814E, &st, 1) == ESP_OK && (st & 0x80)) {
            int n = st & 0x0F;
            uint32_t nm = 0;
            int down = 0, ntx = tx, nty = ty;
            if (n > 5) n = 5;
            if (n > 0) {
                uint8_t buf[5 * 8];
                if (gt_read(0x814F, buf, n * 8)   /* punto 1 empieza en 0x814F: id, xL, xH, yL, yH, tam */ == ESP_OK) {
                    for (int i = 0; i < n; i++) {
                        int x = buf[i * 8 + 1] | (buf[i * 8 + 2] << 8);
                        int y = buf[i * 8 + 3] | (buf[i * 8 + 4] << 8);
                        int rx = x, ry = y;
                        if (i == 0) { rawx = rx; rawy = ry; }
                        cal_map(rx, ry, &x, &y);
                        if (i == 0) { ntx = x; nty = y; down = 1; }
                        nm |= hit(x, y);
                        if (i == 0 && !was_down) ESP_LOGI(TAG, "dedo abajo: crudo=(%d,%d) pantalla=(%d,%d) -> botones 0x%02x", rx, ry, x, y, (unsigned)hit(x, y));
                    }
                }
            }
            gt_write8(0x814E, 0);
            if (down) { tx = ntx; ty = nty; tdown = 1; m = nm; last_touch = esp_timer_get_time(); }
        }
        if (esp_timer_get_time() - last_touch > RELEASE_US) { m = 0; tdown = 0; }   /* dedo levantado */
        state = m;
        was_down = tdown;
        if (m != prev) { ui_update(m); prev = m; }
        vTaskDelay(pdMS_TO_TICKS(4));
    }
}

void gamepad_init(void)
{
    static int done = 0;
    if (done) return;
    done = 1;
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0, .sda_io_num = CONFIG_TOUCH_SDA, .scl_io_num = CONFIG_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) { ESP_LOGE(TAG, "No se pudo iniciar I2C"); return; }
    uint8_t addr = 0;
    if (i2c_master_probe(bus, 0x5D, 50) == ESP_OK) addr = 0x5D;
    else if (i2c_master_probe(bus, 0x14, 50) == ESP_OK) addr = 0x14;
    if (!addr) { ESP_LOGE(TAG, "GT911 no responde en 0x5D ni 0x14 (revisa SDA/SCL)"); return; }
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 400000 };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dc, &dev));
    uint8_t id[4] = { 0 };
    gt_read(0x8140, id, 4);
    ESP_LOGI(TAG, "GT911 en 0x%02X (id %.4s)", addr, (char *)id);
    uint8_t cfg[6] = { 0 };
    if (gt_read(0x8047, cfg, 6) == ESP_OK)
        ESP_LOGI(TAG, "GT911 config v%u: resolucion X=%u Y=%u, puntos max=%u", cfg[0], cfg[1] | (cfg[2] << 8),
                 cfg[3] | (cfg[4] << 8), cfg[5] & 0x0F);
    xTaskCreatePinnedToCore(touch_task, "touch", 4096, NULL, 7, NULL, 0);   /* prioridad mayor que video (5) y audio (6): su trabajo es minimo y no debe quedar sin CPU */
}

uint32_t gamepad_read(void) { return state; }

int gamepad_touch(int *x, int *y)
{
    if (!tdown) return 0;
    *x = tx; *y = ty;
    return 1;
}

/* ---------- Calibracion en pantalla ---------- */
static void draw_cross(int x, int y)
{
    ui_fill_rect(x - 24, y - 2, 49, 5, RGB565(255, 230, 60));
    ui_fill_rect(x - 2, y - 24, 5, 49, RGB565(255, 230, 60));
    ui_fill_rect(x - 6, y - 6, 13, 13, RGB565(220, 40, 50));
}

/* Espera un toque y devuelve la media de las coordenadas crudas mientras el dedo esta puesto */
static void capture(float *ox, float *oy)
{
    for (;;) {
        while (tdown) vTaskDelay(pdMS_TO_TICKS(20));          /* soltar */
        while (!tdown) vTaskDelay(pdMS_TO_TICKS(10));         /* esperar dedo */
        vTaskDelay(pdMS_TO_TICKS(80));                         /* dejar que se asiente */
        float sx = 0, sy = 0; int n = 0;
        while (tdown) { sx += rawx; sy += rawy; n++; vTaskDelay(pdMS_TO_TICKS(10)); }
        if (n >= 5) { *ox = sx / n; *oy = sy / n; return; }
    }
}

void gamepad_calibrate(bool force)
{
    nvs_handle_t h;
    if (!force && nvs_open("nes", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(cal);
        tcal_t tmp;
        bool ok = nvs_get_blob(h, "tcal", &tmp, &len) == ESP_OK && len == sizeof(tmp);
        nvs_close(h);
        if (ok) { cal = tmp; ESP_LOGI(TAG, "Calibracion cargada (swap=%d)", (int)cal.swap); return; }
    }
    static const int T[4][2] = { { 40, 40 }, { 440, 40 }, { 40, 440 }, { 440, 440 } };
    static const char *names[4] = { "ARRIBA IZQUIERDA", "ARRIBA DERECHA", "ABAJO IZQUIERDA", "ABAJO DERECHA" };
    float rx[4], ry[4];
    for (int k = 0; k < 4; k++) {
        ui_fill_rect(0, 0, LCD_W, LCD_H, RGB565(0, 0, 0));
        ui_text(80, 190, "CALIBRACION DEL TACTIL", 2, RGB565(255, 255, 255));
        ui_text(176, 240, "Toca la cruz:", 1, RGB565(200, 200, 210));
        ui_text((LCD_W - (int)strlen(names[k]) * 16) / 2, 262, names[k], 2, RGB565(255, 230, 60));
        draw_cross(T[k][0], T[k][1]);
        lcd_flush_rows(0, LCD_H);
        capture(&rx[k], &ry[k]);
        ESP_LOGI(TAG, "cal punto %d: pantalla=(%d,%d) crudo=(%.0f,%.0f)", k, T[k][0], T[k][1], rx[k], ry[k]);
    }
    tcal_t c = { 0, 1, 0, 1, 0 };
    c.swap = fabsf(ry[1] - ry[0]) > fabsf(rx[1] - rx[0]);     /* mover en X de pantalla cambia el eje Y crudo */
    float u[4], v[4];
    for (int k = 0; k < 4; k++) { u[k] = c.swap ? ry[k] : rx[k]; v[k] = c.swap ? rx[k] : ry[k]; }
    float u0 = (u[0] + u[2]) / 2, u1 = (u[1] + u[3]) / 2, v0 = (v[0] + v[1]) / 2, v1 = (v[2] + v[3]) / 2;
    if (fabsf(u1 - u0) < 20 || fabsf(v1 - v0) < 20) {
        ESP_LOGE(TAG, "Calibracion invalida (toques muy juntos); se usa la anterior");
        ui_fill_rect(0, 0, LCD_W, LCD_H, RGB565(0, 0, 0));
        ui_text(60, 230, "Calibracion invalida, reintenta", 2, RGB565(255, 80, 80));
        lcd_flush_rows(0, LCD_H);
        vTaskDelay(pdMS_TO_TICKS(1500));
        gamepad_calibrate(true);
        return;
    }
    c.ax = (T[1][0] - T[0][0]) / (u1 - u0);  c.bx = T[0][0] - c.ax * u0;
    c.ay = (T[2][1] - T[0][1]) / (v1 - v0);  c.by = T[0][1] - c.ay * v0;
    cal = c;
    ESP_LOGI(TAG, "Calibracion: swap=%d x=%.3f*u%+.1f y=%.3f*v%+.1f", (int)c.swap, c.ax, c.bx, c.ay, c.by);
    if (nvs_open("nes", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "tcal", &c, sizeof(c));
        nvs_commit(h);
        nvs_close(h);
    }
    ui_fill_rect(0, 0, LCD_W, LCD_H, RGB565(0, 0, 0));
    ui_text(176, 230, "LISTO", 3, RGB565(60, 220, 100));
    lcd_flush_rows(0, LCD_H);
    vTaskDelay(pdMS_TO_TICKS(700));
}
