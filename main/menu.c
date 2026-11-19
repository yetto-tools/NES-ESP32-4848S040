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
#define ROW_H      40
#define LIST_Y     (TITLE_H + 8)
#define LIST_ROWS  9
#define MAX_CHARS  26              /* ancho maximo del nombre a escala 2 */
#define FOOT_Y     (LIST_Y + LIST_ROWS * ROW_H + 8)   /* 416 */
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

/* ---------- Dibujo del menu ---------- */
static void draw_button(int x, int y, int w, int h, const char *label, uint16_t bg)
{
    ui_fill_rect(x, y, w, h, C_GRAY);
    ui_fill_rect(x + 2, y + 2, w - 4, h - 4, bg);
    int lw = (int)strlen(label) * 16;
    ui_text(x + (w - lw) / 2, y + (h - 16) / 2, label, 2, C_WHITE);
}

static void draw_list(int sel, int top)
{
    char line[MAX_CHARS + 4], name[MAX_CHARS + 1];
    ui_fill_rect(0, LIST_Y, LCD_W, LIST_ROWS * ROW_H, C_BLACK);
    for (int i = 0; i < LIST_ROWS; i++) {
        int idx = top + i;
        if (idx >= nroms) break;
        display_name(roms[idx], name, MAX_CHARS - 2);
        snprintf(line, sizeof(line), "%s", name);
        int y = LIST_Y + i * ROW_H;
        if (idx == sel) ui_fill_rect(6, y, LCD_W - 12, ROW_H - 4, C_BLUE);
        ui_text(20, y + (ROW_H - 4 - 16) / 2, line, 2, idx == sel ? C_YELLOW : C_WHITE);
        if (zapper_efectivo(base_name(roms[idx])))
            ui_text(LCD_W - 40, y + (ROW_H - 4 - 16) / 2, "Z", 2, C_GREEN);   /* usa Zapper */
    }
    /* contador y pie */
    ui_fill_rect(0, FOOT_Y, LCD_W, LCD_H - FOOT_Y, C_BLACK);
    static const char *const ZL[] = { "ZAP:AUTO", "ZAP:SI", "ZAP:NO" };
    draw_button(10, FOOT_Y, 70, FOOT_H, "<", C_BTN);
    draw_button(88, FOOT_Y, 70, FOOT_H, ">", C_BTN);
    draw_button(166, FOOT_Y, 150, FOOT_H, ZL[zapper_modo(base_name(roms[sel]))], C_BTN);
    draw_button(324, FOOT_Y, 146, FOOT_H, "JUGAR", C_GREEN);
    snprintf(line, sizeof(line), "%d/%d", sel + 1, nroms);
    ui_text(14, FOOT_Y + FOOT_H + 2, line, 1, C_GRAY);
    lcd_flush_rows(LIST_Y, LCD_H);
}

static void save_last(const char *path)
{
    nvs_handle_t h;
    if (nvs_open("nes", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "last", base_name(path));
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

/* Espera un toque nuevo (baja y sube el dedo) y devuelve su posicion */
static void wait_tap(int *x, int *y)
{
    int tx, ty;
    while (gamepad_touch(&tx, &ty)) vTaskDelay(pdMS_TO_TICKS(20));      /* soltar */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (gamepad_touch(&tx, &ty)) {
            *x = tx; *y = ty;
            while (gamepad_touch(&tx, &ty)) vTaskDelay(pdMS_TO_TICKS(20));
            return;
        }
    }
}

/* Barra de titulo con el boton de calibracion del tactil (CAL, a la derecha) */
#define CAL_X  374
#define CAL_W  96
static void draw_screen(int sel, int top)
{
    clear_screen();
    ui_fill_rect(0, 0, LCD_W, TITLE_H, C_RED);
    text_center((TITLE_H - 16) / 2, "ELIGE UN JUEGO", 2, C_WHITE);
    draw_button(CAL_X, 6, CAL_W, TITLE_H - 12, "CAL", C_BTN);
    draw_list(sel, top);
    lcd_flush_rows(0, TITLE_H);
}

static int run_menu(void)
{
    int sel = load_last(), top = 0;
    if (sel >= LIST_ROWS) top = sel - LIST_ROWS / 2;
    if (top > nroms - LIST_ROWS) top = nroms - LIST_ROWS;
    if (top < 0) top = 0;
    draw_screen(sel, top);

    for (;;) {
        int x, y;
        wait_tap(&x, &y);
        int old = sel;
        if (y < TITLE_H) {
            if (x >= CAL_X - 8) {                       /* recalibrar el tactil y volver a la lista */
                gamepad_calibrate(true);
                draw_screen(sel, top);
            }
        } else if (y >= FOOT_Y) {
            if (x < 84)       sel = sel - LIST_ROWS < 0 ? 0 : sel - LIST_ROWS;        /* pagina anterior */
            else if (x < 162) sel = sel + LIST_ROWS >= nroms ? nroms - 1 : sel + LIST_ROWS;
            else if (x < 320) {                                                     /* modo Zapper del juego */
                const char *n = base_name(roms[sel]);
                zapper_guardar_modo(n, (zapper_modo(n) + 1) % 3);
                draw_list(sel, top);
            } else return sel;                                                      /* JUGAR */
        } else if (y >= LIST_Y && y < LIST_Y + LIST_ROWS * ROW_H) {
            int idx = top + (y - LIST_Y) / ROW_H;
            if (idx < nroms) {
                if (idx == sel) return sel;       /* tocar de nuevo el seleccionado = jugar */
                sel = idx;
            }
        }
        if (sel != old) {
            if (sel < top) top = sel;
            if (sel >= top + LIST_ROWS) top = sel - LIST_ROWS + 1;
            draw_list(sel, top);
        }
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
