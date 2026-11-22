// Menu tactil de seleccion de ROMs desde la microSD (SPI) para el ESP32-S3 4848S040
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdkconfig.h"
#if CONFIG_SD_ENABLE
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#endif
#if CONFIG_AUDIO_I2S
#include "driver/i2s_std.h"
#endif
#include <math.h>

#include "lcd_rgb.h"
#include "ui.h"
#include "gamepad.h"
#include "menu.h"
#include "zapper.h"

static const char *TAG = "menu";

#define MAX_ROMS   256
#define MOUNT      "/sdcard"

/* Diseno (pantalla 480x480, texto 8x8 con escala 2 = 16 px de alto) */
#define TITLE_H    48
#define MAX_CHARS  26              /* ancho maximo del nombre a escala 2 */
#define CART_Y0    (TITLE_H + 6)   /* zona del carrusel */
#define NAME_Y     348             /* titulo grande del juego */
#define COUNT_Y    380
#define FOOT_Y     404
#define FOOT_H     56

#define C_BLACK  RGB565(0, 0, 0)
#define C_WHITE  RGB565(255, 255, 255)
#define C_GRAY   RGB565(130, 130, 140)
#define C_RED    RGB565(190, 30, 40)
#define C_YELLOW RGB565(255, 230, 60)
#define C_BLUE   RGB565(20, 70, 170)
#define C_GREEN  RGB565(30, 200, 80)
#define C_BTN    RGB565(58, 58, 70)

static void clear_screen(void)
{
    lcd_init();
    ui_fill_rect(0, 0, LCD_W, LCD_H, C_BLACK);
}

static void text_center(int y, const char *s, int scale, uint16_t fg)
{
    int w = (int)strlen(s) * 8 * scale;
    ui_text((LCD_W - w) / 2, y, s, scale, fg);
}

void menu_message(const char *l1, const char *l2)
{
    clear_screen();
    if (l1) text_center(200, l1, 2, C_YELLOW);
    if (l2) text_center(240, l2, 2, C_WHITE);
    lcd_flush_rows(0, LCD_H);
}

static void draw_progress(int pct)
{
    const int x = 40, y = 300, w = LCD_W - 80, h = 28;
    ui_fill_rect(x, y, w, h, C_GRAY);
    ui_fill_rect(x + 2, y + 2, w - 4, h - 4, C_BLACK);
    ui_fill_rect(x + 2, y + 2, (w - 4) * pct / 100, h - 4, C_GREEN);
    lcd_flush_rows(y, y + h);
}

#if CONFIG_SD_ENABLE

/* ---------- Lista de ROMs ---------- */
static char *roms[MAX_ROMS];        /* ruta completa */
static int nroms;

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static int cmp_rom(const void *a, const void *b)
{
    return strcasecmp(base_name(*(char *const *)a), base_name(*(char *const *)b));
}

static void scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && nroms < MAX_ROMS) {
        const char *n = e->d_name;
        size_t l = strlen(n);
        if (n[0] == '.' || l < 5 || strcasecmp(n + l - 4, ".nes")) continue;
        char *full = malloc(strlen(dir) + l + 2);
        if (!full) break;
        sprintf(full, "%s/%s", dir, n);
        roms[nroms++] = full;
    }
    closedir(d);
}

/* Nombre para mostrar: sin carpeta ni ".nes", recortado */
static void display_name(const char *path, char *out, int max)
{
    const char *b = base_name(path);
    int l = (int)strlen(b) - 4;
    if (l > max) l = max;
    memcpy(out, b, l);
    out[l] = 0;
}

/* ---------- Copiar ROM a la flash ---------- */
static bool install_rom(const char *path)
{
    char name[MAX_CHARS + 1];
    display_name(path, name, MAX_CHARS);

    const esp_partition_t *part = esp_partition_find_first(0x40, 0x01, NULL);
    if (!part) { menu_message("Error:", "falta la particion nesgame"); return false; }

    FILE *f = fopen(path, "rb");
    if (!f) { menu_message("No se pudo abrir:", name); return false; }
    struct stat st;
    stat(path, &st);
    size_t size = st.st_size;

    uint8_t *buf = malloc(4096), *cur = malloc(4096);
    if (!buf || !cur) { fclose(f); free(buf); free(cur); return false; }

    bool ok = false;
    if (size < 16 || size > part->size) {
        menu_message("ROM demasiado grande", "(maximo 1 MB)");
        goto out;
    }
    if (fread(buf, 1, 16, f) != 16 || memcmp(buf, "NES\x1a", 4)) {
        menu_message("No es una ROM .nes valida:", name);
        goto out;
    }

    /* 1) Si ya esta grabada igual, no gastar la flash */
    clear_screen();
    text_center(200, "Revisando...", 2, C_YELLOW);
    text_center(240, name, 2, C_WHITE);
    lcd_flush_rows(0, LCD_H);
    bool same = true;
    fseek(f, 0, SEEK_SET);
    for (size_t off = 0; off < size && same; off += 4096) {
        size_t n = size - off > 4096 ? 4096 : size - off;
        if (fread(buf, 1, n, f) != n) { same = false; break; }
        esp_partition_read(part, off, cur, n);
        if (memcmp(buf, cur, n)) same = false;
    }

    if (!same) {
        ui_fill_rect(0, 190, LCD_W, 40, C_BLACK);
        text_center(200, "Copiando a la memoria...", 2, C_YELLOW);
        lcd_flush_rows(190, 230);
        size_t erase = (size + 4095) & ~4095;
        if (esp_partition_erase_range(part, 0, erase) != ESP_OK) {
            menu_message("Error al borrar la flash", NULL);
            goto out;
        }
        fseek(f, 0, SEEK_SET);
        int last = -1;
        for (size_t off = 0; off < size; off += 4096) {
            size_t n = size - off > 4096 ? 4096 : size - off;
            if (fread(buf, 1, n, f) != n ||
                esp_partition_write(part, off, buf, n) != ESP_OK) {
                menu_message("Error al copiar", name);
                goto out;
            }
            int pct = (off + n) * 100 / size;
            if (pct / 5 != last) { draw_progress(pct); last = pct / 5; }
        }
        ESP_LOGI(TAG, "ROM copiada: %s (%u bytes)", path, (unsigned)size);
    } else {
        ESP_LOGI(TAG, "ROM ya instalada: %s", path);
    }
    ok = true;
out:
    fclose(f);
    free(buf);
    free(cur);
    return ok;
}

/* ---------- Dibujo del menu: carrusel de cartuchos ---------- */
static void draw_button(int x, int y, int w, int h, const char *label, uint16_t bg)
{
    ui_fill_rect(x, y, w, h, C_GRAY);
    ui_fill_rect(x + 2, y + 2, w - 4, h - 4, bg);
    int lw = (int)strlen(label) * 16;
    ui_text(x + (w - lw) / 2, y + (h - 16) / 2, label, 2, C_WHITE);
}

#define C_FONDO  RGB565(22, 22, 30)
#define C_CART   RGB565(150, 150, 158)     /* plastico gris del cartucho */
#define C_CART_L RGB565(205, 205, 212)     /* borde claro */
#define C_CART_D RGB565(80, 80, 90)        /* borde oscuro */

/* Color de la etiqueta segun el nombre (FNV-1a): cada juego conserva siempre el suyo */
static uint16_t color_etiqueta(const char *nombre)
{
    static const uint16_t paleta[] = {
        RGB565(200, 40, 50), RGB565(40, 90, 200), RGB565(40, 150, 70), RGB565(225, 170, 30),
        RGB565(225, 100, 30), RGB565(130, 60, 190), RGB565(30, 150, 160), RGB565(210, 70, 140),
    };
    uint32_t h = 2166136261u;
    for (const char *p = nombre; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
    return paleta[(h >> 8) % (sizeof(paleta) / sizeof(paleta[0]))];
}

/* Reparte el nombre en lineas de a lo sumo `ancho` caracteres (<=15), cortando por palabras; devuelve cuantas */
static int partir_nombre(const char *nombre, int ancho, char lineas[][16], int max_lineas)
{
    int n = 0, len = 0;
    char *cur = lineas[0];
    cur[0] = 0;
    const char *p = nombre;
    while (*p == ' ' || *p == '_' || *p == '-') p++;
    while (*p) {
        const char *fin = p;
        while (*fin && *fin != ' ' && *fin != '_' && *fin != '-') fin++;
        int pl = (int)(fin - p);
        if (pl > ancho) pl = ancho;
        if (len && len + 1 + pl > ancho) {           /* no cabe: nueva linea */
            if (n + 1 >= max_lineas) break;
            cur = lineas[++n]; len = 0; cur[0] = 0;
        }
        if (len) cur[len++] = ' ';
        memcpy(cur + len, p, pl); len += pl; cur[len] = 0;
        p = fin;
        while (*p == ' ' || *p == '_' || *p == '-') p++;
    }
    return n + 1;
}

/* Oscurece un color RGB565 a la mitad (cartuchos que no estan seleccionados) */
static uint16_t apagar(uint16_t c)
{
    return (uint16_t)((((c >> 11) & 31) / 2) << 11 | (((c >> 5) & 63) / 2) << 5 | ((c & 31) / 2));
}

/* Un cartucho NES de ancho w y alto h con centro horizontal en cx y borde superior en top */
static void draw_cart(int cx, int top, int w, int h, const char *nombre, bool sel, bool zap)
{
    int x = cx - w / 2;
    uint16_t et = color_etiqueta(nombre);
    if (!sel) et = apagar(et);
    ui_fill_rect(x + 4, top + 4, w, h, RGB565(8, 8, 12));                          /* sombra */
    ui_fill_rect(x, top, w, h, sel ? C_CART : RGB565(95, 95, 102));
    ui_fill_rect(x, top, w, 3, C_CART_L);  ui_fill_rect(x, top, 3, h, C_CART_L);
    ui_fill_rect(x, top + h - 3, w, 3, C_CART_D);  ui_fill_rect(x + w - 3, top, 3, h, C_CART_D);
    int ey = top + h / 12, eh = h / 28 > 1 ? h / 28 : 1;                           /* estrias superiores */
    for (int i = 0; i < 3; i++) ui_fill_rect(x + w / 8, ey + i * (eh + 3), w * 3 / 4, eh, C_CART_D);
    int lx = x + w / 10, ly = top + h * 22 / 100, lw = w * 8 / 10, lh = h * 52 / 100;   /* etiqueta */
    ui_fill_rect(lx, ly, lw, lh, et);
    ui_fill_rect(lx, ly, lw, lh / 7, RGB565(240, 240, 240));                       /* banda "NES" */
    ui_text(lx + 4, ly + (lh / 7 - 8) / 2, "NES", 1, RGB565(60, 60, 70));
    if (sel) {
        int sc = w >= 160 ? 2 : 1;
        char L[4][16];
        int ancho = lw / (8 * sc) - 1;
        int nl = partir_nombre(nombre, ancho > 15 ? 15 : ancho, L, 4);
        int ty = ly + lh / 7 + (lh - lh / 7 - nl * 8 * sc - (nl - 1) * 4) / 2;
        for (int i = 0; i < nl; i++)
            ui_text(lx + (lw - (int)strlen(L[i]) * 8 * sc) / 2, ty + i * (8 * sc + 4), L[i], sc, C_WHITE);
    } else {
        char ini[2] = { nombre[0] ? nombre[0] : '?', 0 };
        ui_text(lx + (lw - 24) / 2, ly + lh / 7 + (lh - lh / 7 - 24) / 2, ini, 3, RGB565(220, 220, 220));
    }
    int ny = top + h * 82 / 100;                                                    /* conector inferior */
    ui_fill_rect(x + w / 6, ny, w * 2 / 3, h - (ny - top) - 3, C_CART_D);
    ui_fill_rect(x + w / 4, ny + 3, w / 2, h - (ny - top) - 9, RGB565(30, 30, 36));
    if (zap && sel) ui_text(x + w - 22, top + 8, "Z", 2, C_GREEN);                  /* usa Zapper */
}

static void draw_list(int sel)
{
    char name[MAX_CHARS + 1], vecino[MAX_CHARS + 1], line[24];
    ui_fill_rect(0, CART_Y0, LCD_W, FOOT_Y - CART_Y0, C_FONDO);
    if (sel > 0) {
        display_name(roms[sel - 1], vecino, 12);
        draw_cart(70, CART_Y0 + 90, 110, 140, vecino, false, false);
    }
    if (sel + 1 < nroms) {
        display_name(roms[sel + 1], vecino, 12);
        draw_cart(LCD_W - 70, CART_Y0 + 90, 110, 140, vecino, false, false);
    }
    display_name(roms[sel], name, MAX_CHARS);
    draw_cart(LCD_W / 2, CART_Y0 + 8, 190, 270, name, true, zapper_efectivo(base_name(roms[sel])));
    ui_text((LCD_W - (int)strlen(name) * 16) / 2, NAME_Y, name, 2, C_WHITE);
    snprintf(line, sizeof(line), "%d / %d", sel + 1, nroms);
    ui_text((LCD_W - (int)strlen(line) * 8) / 2, COUNT_Y, line, 1, C_GRAY);
    /* pie: anterior, modo Zapper, jugar, siguiente */
    static const char *const ZL[] = { "ZAP:AUTO", "ZAP:SI", "ZAP:NO" };
    ui_fill_rect(0, FOOT_Y, LCD_W, LCD_H - FOOT_Y, C_BLACK);
    draw_button(10, FOOT_Y, 70, FOOT_H, "<", C_BTN);
    draw_button(88, FOOT_Y, 150, FOOT_H, ZL[zapper_modo(base_name(roms[sel]))], C_BTN);
    draw_button(246, FOOT_Y, 146, FOOT_H, "JUGAR", C_GREEN);
    draw_button(400, FOOT_Y, 70, FOOT_H, ">", C_BTN);
    lcd_flush_rows(CART_Y0, LCD_H);
}

static void save_last(const char *path)
{
    nvs_handle_t h;
    if (nvs_open("nes", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "last", base_name(path));
        zapper_guardar_firma();
        nvs_commit(h);
        nvs_close(h);
    }
}

static int load_last(void)
{
    nvs_handle_t h;
    char last[256];
    size_t len = sizeof(last);
    int idx = 0;
    if (nvs_open("nes", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "last", last, &len) == ESP_OK)
            for (int i = 0; i < nroms; i++)
                if (!strcmp(base_name(roms[i]), last)) { idx = i; break; }
        nvs_close(h);
    }
    return idx;
}

/* Espera un gesto: devuelve donde empezo y donde termino el dedo (un toque tiene inicio ~ fin) */
static void wait_gesture(int *x0, int *y0, int *x1, int *y1)
{
    int tx, ty;
    while (gamepad_touch(&tx, &ty)) vTaskDelay(pdMS_TO_TICKS(20));      /* soltar */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (gamepad_touch(&tx, &ty)) {
            *x0 = *x1 = tx; *y0 = *y1 = ty;
            while (gamepad_touch(&tx, &ty)) { *x1 = tx; *y1 = ty; vTaskDelay(pdMS_TO_TICKS(20)); }
            return;
        }
    }
}

/* Tono de prueba (440 Hz, ~1.5 s) por el mismo I2S que usa el juego: permite comprobar bocina, cableado y las
 * resistencias R21-R23 sin cargar una ROM. Crea y destruye su propio canal para no interferir con el del juego. */
static void prueba_sonido(void)
{
#if CONFIG_AUDIO_I2S
    menu_message("Prueba de sonido", "440 Hz ...");
    i2s_chan_handle_t tx = NULL;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear = true;
    if (i2s_new_channel(&cc, &tx, NULL) != ESP_OK) {
        menu_message("Prueba de sonido", "No se pudo crear el I2S");
        vTaskDelay(pdMS_TO_TICKS(1500));
        return;
    }
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(22050),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, .bclk = CONFIG_I2S_BCK, .ws = CONFIG_I2S_LRCK,
            .dout = CONFIG_I2S_DIN, .din = I2S_GPIO_UNUSED, .invert_flags = { 0 },
        },
    };
    if (i2s_channel_init_std_mode(tx, &sc) != ESP_OK || i2s_channel_enable(tx) != ESP_OK) {
        i2s_del_channel(tx);
        menu_message("Prueba de sonido", "No se pudo iniciar el I2S");
        vTaskDelay(pdMS_TO_TICKS(1500));
        return;
    }
    static int16_t buf[256 * 2];
    uint32_t n = 0;
    for (int blk = 0; blk < 130; blk++) {
        for (int i = 0; i < 256; i++, n++) {
            int16_t v = (int16_t)(sinf(2.0f * 3.14159265f * 440.0f * (float)(n % 22050) / 22050.0f) * 12000.0f);
            buf[2 * i] = buf[2 * i + 1] = v;
        }
        size_t w;
        if (i2s_channel_write(tx, buf, sizeof(buf), &w, pdMS_TO_TICKS(300)) != ESP_OK) break;
    }
    i2s_channel_disable(tx);
    i2s_del_channel(tx);
    ESP_LOGI(TAG, "prueba de sonido: tono de 440 Hz enviado (BCK=%d LRCK=%d DIN=%d)", CONFIG_I2S_BCK, CONFIG_I2S_LRCK, CONFIG_I2S_DIN);
#else
    menu_message("Prueba de sonido", "Audio desactivado");
    vTaskDelay(pdMS_TO_TICKS(1500));
#endif
}

/* Barra de titulo con los botones SND (prueba de sonido) y CAL (calibracion del tactil), a la derecha */
#define CAL_X  374
#define CAL_W  96
#define SND_X  270
#define SND_W  96
static void draw_screen(int sel)
{
    clear_screen();
    ui_fill_rect(0, 0, LCD_W, TITLE_H, C_RED);
    ui_text(14, (TITLE_H - 16) / 2, "ELIGE UN JUEGO", 2, C_WHITE);
    draw_button(SND_X, 6, SND_W, TITLE_H - 12, "SND", C_BTN);
    draw_button(CAL_X, 6, CAL_W, TITLE_H - 12, "CAL", C_BTN);
    draw_list(sel);
    lcd_flush_rows(0, TITLE_H);
}

#define SWIPE_PX 50

static int run_menu(void)
{
    int sel = load_last();
    draw_screen(sel);

    for (;;) {
        int x0, y0, x1, y1;
        wait_gesture(&x0, &y0, &x1, &y1);
        int old = sel, dx = x1 - x0, dy = y1 - y0;
        if (abs(dx) >= SWIPE_PX && abs(dx) > abs(dy) && y0 >= CART_Y0 && y0 < FOOT_Y) {
            sel += dx < 0 ? 1 : -1;                                                 /* deslizar: siguiente / anterior */
        } else if (y0 < TITLE_H) {
            if (x0 >= CAL_X - 8) {                                                  /* recalibrar el tactil y volver */
                gamepad_calibrate(true);
                draw_screen(sel);
            } else if (x0 >= SND_X - 8) {                                           /* tono de prueba por la bocina */
                prueba_sonido();
                draw_screen(sel);
            }
        } else if (y0 >= FOOT_Y) {
            if (x0 < 84)       sel--;                                               /* anterior */
            else if (x0 < 242) {                                                    /* modo Zapper del juego */
                const char *n = base_name(roms[sel]);
                zapper_guardar_modo(n, (zapper_modo(n) + 1) % 3);
                draw_list(sel);
            } else if (x0 < 396) return sel;                                        /* JUGAR */
            else               sel++;                                               /* siguiente */
        } else if (y0 >= CART_Y0) {
            if (x0 < 135)               sel--;                                      /* cartucho de la izquierda */
            else if (x0 >= LCD_W - 135) sel++;                                      /* cartucho de la derecha */
            else return sel;                                                        /* cartucho central = jugar */
        }
        if (sel < 0) sel = 0;
        if (sel >= nroms) sel = nroms - 1;
        if (sel != old) draw_list(sel);
    }
}

void rom_menu_run(void)
{
    lcd_init();
    gamepad_init();
    ui_set_overlay(false);

    /* D1, D2 y D3 deben estar en alto: si D3 queda en bajo al iniciar, la tarjeta entra en modo SPI */
    static const int pullups[] = { 1, 2, 39, 40, 41, 42 };
    for (int i = 0; i < (int)(sizeof(pullups) / sizeof(pullups[0])); i++) {
        gpio_reset_pin(pullups[i]);
        gpio_set_direction(pullups[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(pullups[i], GPIO_PULLUP_ONLY);
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    esp_vfs_fat_mount_config_t mc = {
        .format_if_mount_failed = false,
        .max_files = 3,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    menu_message("Leyendo tarjeta SD...", NULL);

    /* D0 no esta confirmado: se prueba el configurado y luego otros candidatos */
    const int d0_try[] = { CONFIG_SD_D0, 41, 42, 40, 2, 1, 39 };
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < (int)(sizeof(d0_try) / sizeof(d0_try[0])) && err != ESP_OK; i++) {
        if (i > 0 && d0_try[i] == CONFIG_SD_D0) continue;
#if CONFIG_AUDIO_I2S
        if (d0_try[i] == CONFIG_I2S_BCK || d0_try[i] == CONFIG_I2S_LRCK || d0_try[i] == CONFIG_I2S_DIN)
            continue;                   /* pines reservados para el audio */
#endif
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.flags = SDMMC_HOST_FLAG_1BIT;
        host.max_freq_khz = CONFIG_SD_MHZ * 1000;
        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.width = 1;
        slot.clk = CONFIG_SD_CLK;
        slot.cmd = CONFIG_SD_CMD;
        slot.d0 = d0_try[i];
        slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
        card = NULL;
        err = esp_vfs_fat_sdmmc_mount(MOUNT, &host, &slot, &mc, &card);
        ESP_LOGI(TAG, "SD con D0=%d: %s", d0_try[i], esp_err_to_name(err));
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Sin tarjeta SD (%s); se usa el juego guardado", esp_err_to_name(err));
        zapper_seleccionar(NULL);
        menu_message("Sin tarjeta SD", "Iniciando juego guardado...");
        vTaskDelay(pdMS_TO_TICKS(1200));
        return;
    }
    sdmmc_card_print_info(stdout, card);

    nroms = 0;
    scan_dir(MOUNT);
    scan_dir(MOUNT "/nes");
    scan_dir(MOUNT "/roms");
    qsort(roms, nroms, sizeof(char *), cmp_rom);
    ESP_LOGI(TAG, "%d ROMs en la SD", nroms);

    if (nroms == 0) {
        zapper_seleccionar(NULL);
        menu_message("No hay archivos .nes en la SD", "Iniciando juego guardado...");
        vTaskDelay(pdMS_TO_TICKS(2000));
    } else {
        while (1) {
            int sel = run_menu();
            if (install_rom(roms[sel])) { save_last(roms[sel]); zapper_seleccionar(base_name(roms[sel])); break; }
            vTaskDelay(pdMS_TO_TICKS(2500));        /* mostrar el error y volver */
        }
    }

    for (int i = 0; i < nroms; i++) free(roms[i]);
    esp_vfs_fat_sdcard_unmount(MOUNT, card);
    ui_fill_rect(0, 0, LCD_W, LCD_H, C_BLACK);
}

#else
void rom_menu_run(void) { lcd_init(); gamepad_init(); zapper_seleccionar(NULL); }
#endif
