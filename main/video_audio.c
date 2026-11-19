// Capa OSD de Nofrendo para ESP32-S3 (4848S040): LCD RGB 480x480 + controles tactiles + audio I2S opcional
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#if CONFIG_AUDIO_I2S
#include "driver/i2s_std.h"
#endif

// Nofrendo define sus propios bool/true/false
#undef false
#undef true
#undef bool
#include <noftypes.h>
#include <bitmap.h>
#include <nofconfig.h>
#include <event.h>
#include <gui.h>
#include <log.h>
#include <nes.h>
#include <nes_pal.h>
#include <nesinput.h>
#include <osd.h>
#include <vid_drv.h>

#include "lcd_rgb.h"
#include "ui.h"
#include "gamepad.h"
#include "zapper.h"

#define SAMPLERATE   22050
#define FRAGSIZE     128
#define NES_W        256
#define NES_H        NES_SCREEN_HEIGHT    /* 240: la PPU escribe las 240 lineas */

/* ---------------- Timer ---------------- */
static TimerHandle_t timer;

int osd_installtimer(int frequency, void *func, int funcsize, void *counter, int countersize)
{
    timer = xTimerCreate("nes", configTICK_RATE_HZ / frequency, pdTRUE, NULL,
                         (TimerCallbackFunction_t)func);
    xTimerStart(timer, 0);
    return 0;
}

/* ---------------- Audio ---------------- */
static void (*audio_callback)(void *buffer, int length) = NULL;
static int16_t audio_in[FRAGSIZE];
#if CONFIG_AUDIO_I2S
static i2s_chan_handle_t i2s_tx;
static int16_t audio_out[FRAGSIZE * 2];          /* estereo: L y R iguales */
#endif

static void do_audio_frame(void)
{
#if CONFIG_AUDIO_I2S
    if (!audio_callback) return;
    int left = SAMPLERATE / NES_REFRESH_RATE;
    while (left) {
        int n = left > FRAGSIZE ? FRAGSIZE : left;
        audio_callback(audio_in, n);
        for (int i = 0; i < n; i++) {
            int v = (audio_in[i] * CONFIG_AUDIO_VOLUME) / 100;
            if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
            audio_out[2 * i] = audio_out[2 * i + 1] = (int16_t)v;
        }
        size_t written;
        if (!i2s_tx || i2s_channel_write(i2s_tx, audio_out, n * 4, &written,
                                         pdMS_TO_TICKS(100)) != ESP_OK) return;
        left -= n;
    }
#endif
}

void osd_setsound(void (*playfunc)(void *buffer, int length)) { audio_callback = playfunc; }
static void osd_stopsound(void) { audio_callback = NULL; }

static int osd_init_sound(void)
{
#if CONFIG_AUDIO_I2S
    /* PCM5102A: BCK, LRCK (LCK) y DIN. SCK del modulo a GND (usa su PLL interno). */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 256;
    chan_cfg.auto_clear = true;                   /* silencio si no llegan datos */
    if (i2s_new_channel(&chan_cfg, &i2s_tx, NULL) != ESP_OK) {
        ESP_LOGW("audio", "No se pudo crear el canal I2S, sin sonido");
        i2s_tx = NULL;
    } else {
        i2s_std_config_t std_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLERATE),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                            I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = CONFIG_I2S_BCK,
                .ws   = CONFIG_I2S_LRCK,
                .dout = CONFIG_I2S_DIN,
                .din  = I2S_GPIO_UNUSED,
                .invert_flags = { 0 },
            },
        };
        if (i2s_channel_init_std_mode(i2s_tx, &std_cfg) != ESP_OK ||
            i2s_channel_enable(i2s_tx) != ESP_OK) {
            ESP_LOGW("audio", "No se pudo iniciar I2S, sin sonido");
            i2s_tx = NULL;
        } else {
            ESP_LOGI("audio", "I2S PCM5102A: BCK=%d LRCK=%d DIN=%d, %d Hz",
                     CONFIG_I2S_BCK, CONFIG_I2S_LRCK, CONFIG_I2S_DIN, SAMPLERATE);
        }
    }
#endif
    audio_callback = NULL;
    return 0;
}

void osd_getsoundinfo(sndinfo_t *info)
{
    info->sample_rate = SAMPLERATE;
    info->bps = 16;
}

/* ---------------- Video ---------------- */
static int init(int width, int height) { return 0; }
static void shutdown(void) {}
static int set_mode(int width, int height) { return 0; }
static void set_palette(rgb_t *pal);
static void clear(uint8 color) {}
static bitmap_t *lock_write(void);
static void free_write(int num_dirties, rect_t *dirty_rects);
static void custom_blit(bitmap_t *bmp, int num_dirties, rect_t *dirty_rects);

static char fb[1];
static QueueHandle_t vidQueue;
static bitmap_t *myBitmap;
static uint16_t myPalette[256];
static uint8_t palLum[256];
#define ZAP_LUM_MIN   220    /* solo blanco brillante: el cielo azul (~157) no cuenta como luz */
#define ZAP_RADIO     10     /* un dedo cubre ~20 px de pantalla y tapa el blanco: area tolerante (en NES px) */
#define ZAP_GATILLO   6      /* cuadros con el gatillo pulsado por cada toque (disparo breve, como el real) */
#define ZAP_APUNTA    45     /* cuadros que el sensor sigue apuntando al punto tras el toque (~0.75 s) */
static volatile int zap_disparo;                 /* cuadros de gatillo que faltan */
static volatile int zap_apunta;                  /* cuadros que faltan con el sensor apuntando */
static volatile int zap_ax, zap_ay;              /* punto apuntado (NES), fijado en el instante del toque */          /* luminancia por color de paleta (sensor del Zapper) */

viddriver_t sdlDriver = {
    "RGB480", init, shutdown, set_mode, set_palette, clear,
    lock_write, free_write, custom_blit, false
};

void osd_getvideoinfo(vidinfo_t *info)
{
    info->default_width = NES_W;
    info->default_height = NES_H;
    info->driver = &sdlDriver;
}

void osd_togglefullscreen(int code) {}

static void set_palette(rgb_t *pal)
{
    for (int i = 0; i < 256; i++) {
        uint16_t c = ((pal[i].r >> 3) << 11) | ((pal[i].g >> 2) << 5) | (pal[i].b >> 3);
        palLum[i] = (pal[i].r * 30 + pal[i].g * 59 + pal[i].b * 11) / 100;
        myPalette[i] = c;                                /* RGB565 nativo (bus RGB) */
    }
}

static bitmap_t *lock_write(void)
{
    myBitmap = bmp_createhw((uint8 *)fb, NES_W, NES_H, NES_W * 2);
    return myBitmap;
}

static void free_write(int num_dirties, rect_t *dirty_rects) { bmp_destroy(&myBitmap); }

static void custom_blit(bitmap_t *bmp, int num_dirties, rect_t *dirty_rects)
{
    xQueueSend(vidQueue, &bmp, 0);      /* si la pantalla va ocupada, se salta el frame */
    do_audio_frame();
}

static void draw_mira(void);

/* Corre en el nucleo 1; la emulacion en el nucleo 0 */
static void videoTask(void *arg)
{
    bitmap_t *bmp = NULL;
    int frames = 0;
    TickType_t t0 = xTaskGetTickCount();
    while (1) {
        xQueueReceive(vidQueue, &bmp, portMAX_DELAY);
        lcd_draw_game((const uint8_t **)bmp->line, myPalette);
        draw_mira();
        frames++;
        TickType_t now = xTaskGetTickCount();
        if (now - t0 >= pdMS_TO_TICKS(5000)) {      /* FPS por el monitor serie */
            ESP_LOGI("video", "%.1f fps en pantalla, RAM libre %u",
                     frames * 1000.0f / pdTICKS_TO_MS(now - t0),
                     (unsigned)esp_get_free_heap_size());
            frames = 0;
            t0 = now;
        }
    }
}

/* Mira roja donde el sistema cree que se disparo (solo con Zapper activo y dedo en el juego) */
static void draw_mira(void)
{
    int tx, ty;
    if (!zapper_activo() || !gamepad_touch(&tx, &ty)) return;
    if (tx < GAME_X || tx >= GAME_X + GAME_W || ty < GAME_Y || ty >= GAME_Y + GAME_H) return;
    const uint16_t rojo = RGB565(255, 40, 40);
    int x0 = tx - 10 < GAME_X ? GAME_X : tx - 10, x1 = tx + 11 > GAME_X + GAME_W ? GAME_X + GAME_W : tx + 11;
    int y0 = ty - 10 < GAME_Y ? GAME_Y : ty - 10, y1 = ty + 11 > GAME_Y + GAME_H ? GAME_Y + GAME_H : ty + 11;
    ui_fill_rect(x0, ty - 1, x1 - x0, 3, rojo);
    ui_fill_rect(tx - 1, y0, 3, y1 - y0, rojo);
    lcd_flush_rows(y0, y1);
}

/* ---------------- Entrada ---------------- */
static void osd_initinput(void) { gamepad_init(); }

void osd_getinput(void)
{
    static const int ev[BTN_COUNT] = {
        event_joypad1_up, event_joypad1_down, event_joypad1_left, event_joypad1_right,
        event_joypad1_a, event_joypad1_b, event_joypad1_start, event_joypad1_select,
    };
    static uint32_t old = 0;
    uint32_t b = gamepad_read();
    uint32_t chg = b ^ old;
    old = b;
    for (int i = 0; i < BTN_COUNT; i++) {
        if (chg & (1u << i)) {
            event_t h = event_get(ev[i]);
            if (h) h((b & (1u << i)) ? INP_STATE_MAKE : INP_STATE_BREAK);
        }
    }
    /* Zapper: un toque nuevo en el area de juego fija el punto apuntado y pulsa el gatillo unos cuadros */
    {
        static int was = 0;
        int tx, ty, t = gamepad_touch(&tx, &ty);
        if (zap_disparo > 0 && !(t && !was)) zap_disparo--;
        if (zap_apunta > 0 && !(t && !was)) zap_apunta--;
        if (t && !was && zapper_activo() &&
            tx >= GAME_X && tx < GAME_X + GAME_W && ty >= GAME_Y && ty < GAME_Y + GAME_H) {
            zap_ax = (tx - GAME_X) * NES_W / GAME_W;     /* el punto se fija al tocar; el dedo puede deslizarse despues */
            zap_ay = (ty - GAME_Y) * NES_H / GAME_H;
            zap_disparo = ZAP_GATILLO;
            zap_apunta = ZAP_APUNTA;
        }
        was = t;
    }
    /* Start + Select + A + B = reinicio */
    static int combo = 0;
    const uint32_t RESET = (1u << BTN_START) | (1u << BTN_SELECT) | (1u << BTN_A) | (1u << BTN_B);
    if ((b & RESET) == RESET) {
        if (!combo) { event_t h = event_get(event_soft_reset); if (h) h(INP_STATE_MAKE); }
        combo = 1;
    } else combo = 0;

    /* Boton MENU (un toque corto) = reiniciar y volver al menu de ROMs */
    /* Se mide en tiempo real: contar cuadros fallaba si el emulador va lento o salta cuadros */
    static int64_t menu_btn_t0 = 0;
    if (b & (1u << BTN_MENU)) {
        int64_t now = esp_timer_get_time();
        if (!menu_btn_t0) menu_btn_t0 = now;
        if (now - menu_btn_t0 > 120000) { ESP_LOGW("menu", "boton MENU: reiniciando al menu de ROMs"); esp_restart(); }
    } else menu_btn_t0 = 0;

    /* Select + Start durante 1 segundo = reiniciar */
    static int menu_hold = 0;
    const uint32_t MENU = (1u << BTN_START) | (1u << BTN_SELECT);
    if ((b & RESET) == MENU) {
        if (++menu_hold > NES_REFRESH_RATE) esp_restart();
    } else menu_hold = 0;
}


/* ---------------- Zapper tactil ---------------- */
/* Un toque en el area del juego = disparo en ese punto (coordenadas NES 256x240).
 * D4 = gatillo (1 mientras hay dedo). D3 = sensor de luz: 0 si el haz ya paso por el
 * punto y el pixel esta iluminado, 1 si no. Duck Hunt muestra un cuadro negro con
 * cajas blancas en los blancos para detectar el impacto. */
uint8 osd_getzapper(void)
{
    if (!zapper_activo()) return 0;     /* este juego no usa Zapper */
    if (zap_apunta <= 0) return INP_ZAPPER_MISS;        /* sin punto apuntado */
    int nx = zap_ax, ny = zap_ay;
    /* gatillo: breve; sensor de luz: independiente, sigue apuntando aunque el gatillo ya se haya soltado */
    uint8 v = INP_ZAPPER_MISS | (zap_disparo > 0 ? INP_ZAPPER_TRIG : 0);
    nes_t *n = nes_getcontextptr();
    bitmap_t *buf = vid_getbuffer();
    if (n && buf) {
        /* El juego sondea $4017 ~2000 veces por cuadro: se examina cada fila UNA vez por cuadro y el
         * resultado se conserva (la luz persiste mientras haya blanco en filas ya dibujadas). Solo
         * cuentan filas ya dibujadas en ESTE cuadro (<= scanline); las de mas abajo conservan el
         * cuadro anterior y daban luz falsa en el cuadro negro de prueba. */
        static int sig_fila, con_luz_cuadro, scan_prev = 1000, ax_prev = -1, ay_prev = -1;
        int sl = n->scanline;
        if (sl < scan_prev || nx != ax_prev || ny != ay_prev) {      /* cuadro nuevo o punto nuevo */
            sig_fila = ny - ZAP_RADIO < 0 ? 0 : ny - ZAP_RADIO;
            con_luz_cuadro = 0;
            ax_prev = nx; ay_prev = ny;
        }
        scan_prev = sl;
        int ymax = sl < NES_H - 1 ? sl : NES_H - 1;
        if (ymax > ny + ZAP_RADIO) ymax = ny + ZAP_RADIO;
        for (; sig_fila <= ymax; sig_fila++) {
            const uint8_t *fila = buf->line[sig_fila];
            for (int x = nx - ZAP_RADIO; x <= nx + ZAP_RADIO; x++)
                if (x >= 0 && x < NES_W && palLum[fila[x]] >= ZAP_LUM_MIN) { con_luz_cuadro = 1; break; }
        }
        if (con_luz_cuadro) return v & ~INP_ZAPPER_MISS;
    }
    return v;
}

static void osd_freeinput(void) {}
void osd_getmouse(int *x, int *y, int *button) {}

/* ---------------- Init / shutdown ---------------- */
void osd_shutdown(void)
{
    osd_stopsound();
    osd_freeinput();
}

static int logprint(const char *string) { return printf("%s", string); }

int osd_init(void)
{
    log_chain_logfunc(logprint);
    if (osd_init_sound()) return -1;

    lcd_init();
    ui_set_overlay(true);
    ui_draw_static();
    vidQueue = xQueueCreate(1, sizeof(bitmap_t *));
    xTaskCreatePinnedToCore(&videoTask, "videoTask", 6144, NULL, 5, NULL, 1);
    osd_initinput();
    return 0;
}
