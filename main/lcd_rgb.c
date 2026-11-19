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
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
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

/* Reloj de pixel: con 12 MHz el panel refresca a ~46 Hz (< 60 cuadros del juego) y el corte se mueve;
 * con 16 MHz refresca a ~61 Hz. Si la imagen se ve inestable, volver a 12000000. */
#define LCD_PCLK_HZ  16000000

static const char *TAG = "lcd";

/* Triple buffer sin tearing:
 *  - hw[0..2]: framebuffers del panel (PSRAM). El panel muestra uno ("shown"), otro puede estar pedido
 *    pero aun sin mostrarse ("req"), y el tercero esta libre para dibujar. Asi dibujar nunca espera
 *    al panel: el juego se renderiza en el libre y se pide como siguiente (si habia uno pedido que no
 *    llego a mostrarse, se descarta y gana el mas reciente).
 *  - fb: lienzo de la interfaz (botones, menu), que usan ui.c y menu.c; sus filas "sucias" se copian
 *    a cada hw[] antes de usarlo. */
#define NFB 3
static uint16_t *hw[NFB];
static uint16_t *fb;
/* Zonas de fb pendientes de copiar a cada hw[]: por fila, el rango de columnas [dx0, dx1) cambiado, y el rango de filas.
 * Antes solo se guardaban filas completas (el rango que envuelve todos los cambios): un boton obligaba a copiar ~180 filas. */
static int16_t dx0[NFB][LCD_H], dx1[NFB][LCD_H];
static int dy0[NFB], dy1[NFB];
static portMUX_TYPE dmux = portMUX_INITIALIZER_UNLOCKED;   /* protege las marcas; no bloquea a la tarea del tactil */

static void dirty_reset(void)
{
    for (int b = 0; b < NFB; b++) {
        for (int y = 0; y < LCD_H; y++) { dx0[b][y] = LCD_W; dx1[b][y] = 0; }
        dy0[b] = LCD_H; dy1[b] = -1;
    }
}

/* Marca [x0, x1) x [y0, y1) como cambiado para todos los buffers */
static void mark(int x0, int y0, int x1, int y1)
{
    portENTER_CRITICAL(&dmux);
    for (int b = 0; b < NFB; b++) {
        for (int y = y0; y < y1; y++) {
            if (x0 < dx0[b][y]) dx0[b][y] = (int16_t)x0;
            if (x1 > dx1[b][y]) dx1[b][y] = (int16_t)x1;
        }
        if (y0 < dy0[b]) dy0[b] = y0;
        if (y1 - 1 > dy1[b]) dy1[b] = y1 - 1;
    }
    portEXIT_CRITICAL(&dmux);
}
static volatile int req, shown;
static bool game_mode;                      /* true tras el primer cuadro de juego */
static SemaphoreHandle_t lcd_mtx, frame_sem;
static esp_lcd_panel_handle_t g_panel;
static volatile int64_t st_conv, st_espera;   /* diagnostico: us convirtiendo/escribiendo y us esperando el cambio de buffer */
static volatile int st_n;

void lcd_stats(float *conv_ms, float *espera_ms)
{
    int n = st_n ? st_n : 1;
    *conv_ms = st_conv / 1000.0f / n;
    *espera_ms = st_espera / 1000.0f / n;
    st_conv = 0; st_espera = 0; st_n = 0;
}

static bool IRAM_ATTR on_frame_free(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *e, void *u)
{
    BaseType_t hp = pdFALSE;
    shown = req;                         /* al inicio de cada cuadro el panel adopta el ultimo buffer pedido */
    xSemaphoreGiveFromISR(frame_sem, &hp);
    return hp == pdTRUE;
}

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
            .pclk_hz = LCD_PCLK_HZ,
            .h_res = LCD_W, .v_res = LCD_H,
            .hsync_pulse_width = 8, .hsync_back_porch = 20, .hsync_front_porch = 10,
            .vsync_pulse_width = 8, .vsync_back_porch = 10, .vsync_front_porch = 10,
            .flags = { .pclk_active_neg = 0 },
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = NFB,
        .bounce_buffer_size_px = LCD_W * 20,
        .hsync_gpio_num = PIN_HSYNC, .vsync_gpio_num = PIN_VSYNC,
        .de_gpio_num = PIN_DE, .pclk_gpio_num = PIN_PCLK, .disp_gpio_num = -1,
        .data_gpio_nums = DATA_PINS,
        .flags = { .fb_in_psram = 1 },
    };
    esp_lcd_panel_handle_t panel;
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &panel));
    g_panel = panel;
    lcd_mtx = xSemaphoreCreateMutex();
    frame_sem = xSemaphoreCreateBinary();
    esp_lcd_rgb_panel_event_callbacks_t cbs = { .on_frame_buf_complete = on_frame_free };
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel, &cbs, NULL));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    void *p0, *p1, *p2;
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel, NFB, &p0, &p1, &p2));
    hw[0] = p0; hw[1] = p1; hw[2] = p2;
    uint16_t *canvas = heap_caps_calloc((size_t)LCD_W * LCD_H, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(canvas ? ESP_OK : ESP_ERR_NO_MEM);
    for (int b = 0; b < NFB; b++) {
        memset(hw[b], 0, (size_t)LCD_W * LCD_H * 2);
    }
    dirty_reset();
    req = shown = 0;                /* el panel arranca mostrando hw[0] */
    fb = canvas;                    /* al final: lcd_init() sale temprano si fb ya existe */
    lcd_fill(0);
    gpio_set_level(PIN_BL, 1);
    ESP_LOGI(TAG, "LCD 480x480 listo");
}

uint16_t *lcd_fb(void) { return fb; }

/* En modo "bounce buffer" el panel lee los hw[] a traves de la misma cache que usa la CPU, asi que no
 * hace falta esp_cache_msync (ESP-IDF tampoco lo hace en este modo). */

/* Buffer libre: ni el que muestra el panel ni el ultimo pedido */
static int libre(void)
{
    int r = req, sh = shown;
    for (int i = 0; i < NFB; i++) if (i != r && i != sh) return i;
    return 0;
}

/* Copia a hw[b] las filas de fb que cambiaron desde la ultima vez que se sincronizo ese buffer */
static void sync_rows(int b)
{
    portENTER_CRITICAL(&dmux);
    int ya = dy0[b], yb = dy1[b];
    dy0[b] = LCD_H; dy1[b] = -1;                  /* lo que se marque desde ahora cuenta para la proxima vez */
    portEXIT_CRITICAL(&dmux);
    for (int y = ya; y <= yb; y++) {
        portENTER_CRITICAL(&dmux);
        int a = dx0[b][y], e = dx1[b][y];
        dx0[b][y] = LCD_W; dx1[b][y] = 0;
        portEXIT_CRITICAL(&dmux);
        if (e > a) memcpy(hw[b] + (size_t)y * LCD_W + a, fb + (size_t)y * LCD_W + a, (size_t)(e - a) * 2);
    }
}

/* Pide mostrar hw[w] en el siguiente cuadro del panel. No espera. */
static void pedir(int w)
{
    esp_lcd_panel_draw_bitmap(g_panel, 0, 0, LCD_W, LCD_H, hw[w]);
    req = w;
}

void lcd_flush_rows(int y0, int y1)
{
    if (y0 < 0) y0 = 0;
    if (y1 > LCD_H) y1 = LCD_H;
    if (y1 <= y0 || !lcd_mtx) return;
    xSemaphoreTake(lcd_mtx, portMAX_DELAY);
    mark(0, y0, LCD_W, y1);
    if (!game_mode) {            /* menus y calibracion: mostrar ya y esperar a que se vea. En juego, con el siguiente cuadro */
        int w = libre();
        sync_rows(w);
        xSemaphoreTake(frame_sem, 0);
        pedir(w);
        xSemaphoreTake(frame_sem, pdMS_TO_TICKS(60));
    }
    xSemaphoreGive(lcd_mtx);
}

/* Marca un rectangulo del lienzo como cambiado (en juego se copia a los buffers con el siguiente cuadro). No bloquea. */
void lcd_dirty_rect(int x, int y, int w, int h)
{
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > LCD_W) x1 = LCD_W;
    if (y1 > LCD_H) y1 = LCD_H;
    if (x1 <= x || y1 <= y) return;
    mark(x, y, x1, y1);
}

void lcd_fill(uint16_t color)
{
    for (int i = 0; i < LCD_W * LCD_H; i++) fb[i] = color;
    lcd_flush_rows(0, LCD_H);
}

/* Detalle del tiempo de dibujo (ciclos de CPU, 240 por microsegundo): calcular pixeles, volcar a PSRAM, sincronizar interfaz */
static volatile uint32_t pc_calc, pc_vol, pc_sinc, pc_n;
static inline uint32_t ccount(void)
{
    uint32_t c;
    __asm__ __volatile__("rsr %0, ccount" : "=a"(c));
    return c;
}

void lcd_stats_detalle(float *calc_ms, float *vol_ms, float *sinc_ms)
{
    float k = 1.0f / 240000.0f / (pc_n ? pc_n : 1);
    *calc_ms = pc_calc * k; *vol_ms = pc_vol * k; *sinc_ms = pc_sinc * k;
    pc_calc = pc_vol = pc_sinc = pc_n = 0;
}

void lcd_draw_game(const uint8_t **lines, const uint16_t *pal, int mira_x, int mira_y)
{
    static uint16_t rowbuf[IMG_W];
    xSemaphoreTake(lcd_mtx, portMAX_DELAY);
    int64_t t0 = esp_timer_get_time();
    if (!game_mode) {
        /* Primer cuadro de juego: las bandas laterales y el borde superior NO se redibujan en cada cuadro, asi
         * que conservaban restos del menu y, al alternar entre los 3 buffers, parpadeaban. Se borran en el lienzo
         * y se marcan todas las filas para que cada buffer se sincronice por completo la primera vez que se use. */
        for (int y = 0; y < GAME_Y + GAME_H; y++) {
            uint16_t *fila = fb + (size_t)y * LCD_W;
            if (y < GAME_Y) {
                for (int x = 0; x < LCD_W; x++) fila[x] = 0;
            } else {
                for (int x = 0; x < IMG_X; x++) fila[x] = 0;
                for (int x = IMG_X + IMG_W; x < LCD_W; x++) fila[x] = 0;
            }
        }
        mark(0, 0, LCD_W, LCD_H);
    }
    game_mode = true;
    int w = libre();
    { uint32_t cs = ccount(); sync_rows(w); pc_sinc += ccount() - cs; }                                              /* botones u otras filas de la interfaz pendientes */
    uint16_t *base = hw[w];
    int prev_sy = -1;
    for (int y = 0; y < GAME_H; y++) {
        int sy = y * 240 / GAME_H;
        if (sy != prev_sy) {                                   /* linea de origen nueva: convertirla (si no, se repite la anterior) */
            const uint8_t *src = lines[sy] + IMG_CROP;         /* se descartan 8 columnas por lado (overscan) */
            /* 240 columnas a 360 px = 1.5x: cada 2 pixeles origen (a, b) dan 3 de destino (a, a, b) */
            uint32_t cc = ccount();
            uint16_t *o = rowbuf;
            for (int m = 0; m < IMG_W / 3; m++, o += 3) {
                uint16_t a = pal[src[2 * m]], b = pal[src[2 * m + 1]];
                o[0] = a; o[1] = a; o[2] = b;
            }
            pc_calc += ccount() - cc;
            prev_sy = sy;
        }
        uint32_t cv = ccount();
        memcpy(base + (GAME_Y + y) * LCD_W + IMG_X, rowbuf, IMG_W * 2);
        pc_vol += ccount() - cv;
    }
    if (mira_x >= 0) {                                         /* mira roja del Zapper, dentro del area de juego */
        const uint16_t rojo = RGB565(255, 40, 40);
        int x0 = mira_x - 10 < IMG_X ? IMG_X : mira_x - 10;
        int x1 = mira_x + 11 > IMG_X + IMG_W ? IMG_X + IMG_W : mira_x + 11;
        int y0 = mira_y - 10 < GAME_Y ? GAME_Y : mira_y - 10;
        int y1 = mira_y + 11 > GAME_Y + GAME_H ? GAME_Y + GAME_H : mira_y + 11;
        for (int y = mira_y - 1; y <= mira_y + 1; y++)
            if (y >= GAME_Y && y < GAME_Y + GAME_H)
                for (int x = x0; x < x1; x++) base[y * LCD_W + x] = rojo;
        for (int y = y0; y < y1; y++)
            for (int x = mira_x - 1; x <= mira_x + 1; x++)
                if (x >= IMG_X && x < IMG_X + IMG_W) base[y * LCD_W + x] = rojo;
    }
    int64_t t1 = esp_timer_get_time();
    pedir(w);
    st_conv += t1 - t0;
    st_espera += esp_timer_get_time() - t1;
    st_n++;
    pc_n++;
    xSemaphoreGive(lcd_mtx);
}
