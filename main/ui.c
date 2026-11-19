// Controles tipo RetroArch dibujados directamente en el framebuffer
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include "ui.h"
#include "lcd_rgb.h"
#include "gamepad.h"
#include "font8x8_basic.h"

#define C_BG     RGB565(16, 16, 20)
#define C_BTN    RGB565(58, 58, 70)
#define C_BTN_ON RGB565(150, 150, 175)
#define C_EDGE   RGB565(110, 110, 130)
#define C_TEXT   RGB565(215, 215, 225)
#define C_A      RGB565(180, 40, 50)
#define C_A_ON   RGB565(255, 90, 100)
#define C_B      RGB565(180, 110, 30)
#define C_B_ON   RGB565(255, 190, 80)

static uint32_t shown = 0xFFFFFFFF;
static bool overlay_on = false;

void ui_set_overlay(bool on) { overlay_on = on; }

void ui_fill_rect(int x, int y, int w, int h, uint16_t c)
{
    uint16_t *fb = lcd_fb();
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_W) w = LCD_W - x;
    if (y + h > LCD_H) h = LCD_H - y;
    for (int j = 0; j < h; j++) {
        uint16_t *p = fb + (y + j) * LCD_W + x;
        for (int i = 0; i < w; i++) p[i] = c;
    }
}

static void fill_circle(int cx, int cy, int r, uint16_t c)
{
    for (int dy = -r; dy <= r; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r) dx++;
        ui_fill_rect(cx - dx, cy + dy, 2 * dx + 1, 1, c);
    }
}

void ui_text(int x, int y, const char *s, int scale, uint16_t fg)
{
    uint16_t *fb = lcd_fb();
    for (int c = 0; s[c]; c++) {
        unsigned char ch = (unsigned char)s[c];
        if (ch > 127) ch = '?';
        const unsigned char *g = font8x8_basic[ch];
        for (int gy = 0; gy < 8; gy++)
            for (int gx = 0; gx < 8; gx++)
                if (g[gy] & (1 << gx))                      /* bit 0 = pixel izquierdo */
                    ui_fill_rect(x + (c * 8 + gx) * scale, y + gy * scale, scale, scale, fg);
    }
    (void)fb;
}

static void draw_arrow(int cx, int cy, int dir, uint16_t c)   /* 0 arriba, 1 abajo, 2 izq, 3 der */
{
    for (int i = 0; i < 12; i++) {
        int w = i * 2 + 1;
        switch (dir) {
        case 0: ui_fill_rect(cx - i, cy - 8 + i, w, 1, c); break;
        case 1: ui_fill_rect(cx - i, cy + 8 - i, w, 1, c); break;
        case 2: ui_fill_rect(cx - 8 + i, cy - i, 1, w, c); break;
        default: ui_fill_rect(cx + 8 - i, cy - i, 1, w, c); break;
        }
    }
}

static void draw_dpad(uint32_t p)
{
    const int cx = DPAD_CX, cy = DPAD_CY, L = 28, A = 56;      /* ancho de brazo y largo desde el centro */
    ui_fill_rect(cx - DPAD_R - 4, cy - DPAD_R - 4, 2 * DPAD_R + 8, 2 * DPAD_R + 8, C_BG);
    ui_fill_rect(cx - L, cy - DPAD_R, 2 * L, 2 * DPAD_R, C_EDGE);               /* cruz: borde */
    ui_fill_rect(cx - DPAD_R, cy - L, 2 * DPAD_R, 2 * L, C_EDGE);
    ui_fill_rect(cx - L + 2, cy - DPAD_R + 2, 2 * L - 4, 2 * DPAD_R - 4, C_BTN);    /* cruz: relleno */
    ui_fill_rect(cx - DPAD_R + 2, cy - L + 2, 2 * DPAD_R - 4, 2 * L - 4, C_BTN);
    /* brazos presionados */
    if (p & (1u << BTN_UP))    ui_fill_rect(cx - L + 2, cy - DPAD_R + 2, 2 * L - 4, DPAD_R - L, C_BTN_ON);
    if (p & (1u << BTN_DOWN))  ui_fill_rect(cx - L + 2, cy + L, 2 * L - 4, DPAD_R - L - 2, C_BTN_ON);
    if (p & (1u << BTN_LEFT))  ui_fill_rect(cx - DPAD_R + 2, cy - L + 2, DPAD_R - L, 2 * L - 4, C_BTN_ON);
    if (p & (1u << BTN_RIGHT)) ui_fill_rect(cx + L, cy - L + 2, DPAD_R - L - 2, 2 * L - 4, C_BTN_ON);
    draw_arrow(cx, cy - A + 4, 0, C_TEXT);
    draw_arrow(cx, cy + A - 4, 1, C_TEXT);
    draw_arrow(cx - A + 4, cy, 2, C_TEXT);
    draw_arrow(cx + A - 4, cy, 3, C_TEXT);
}

static void draw_round(int cx, int cy, const char *label, bool on, uint16_t col, uint16_t col_on)
{
    fill_circle(cx, cy, BTN_R + 2, C_EDGE);
    fill_circle(cx, cy, BTN_R, on ? col_on : col);
    ui_text(cx - 12, cy - 12, label, 3, C_TEXT);
}

static void draw_pill(int x, int y, const char *label, bool on)
{
    ui_fill_rect(x, y, PILL_W, PILL_H, C_EDGE);
    ui_fill_rect(x + 2, y + 2, PILL_W - 4, PILL_H - 4, on ? C_BTN_ON : C_BTN);
    int len = (int)strlen(label);
    ui_text(x + (PILL_W - len * 8) / 2, y + (PILL_H - 8) / 2, label, 1, C_TEXT);
}

void ui_draw_static(void)
{
    ui_fill_rect(0, GAME_Y + GAME_H, LCD_W, LCD_H - (GAME_Y + GAME_H), C_BG);
    shown = 0xFFFFFFFF;
    ui_update(0);
}

void ui_update(uint32_t p)
{
    if (!overlay_on) return;
    if (p == shown) return;
    uint32_t chg = p ^ shown;
    shown = p;
    const uint32_t DP = (1u << BTN_UP) | (1u << BTN_DOWN) | (1u << BTN_LEFT) | (1u << BTN_RIGHT);
    if (chg & DP)               draw_dpad(p);
    if (chg & (1u << BTN_A))    draw_round(A_CX, A_CY, "A", p & (1u << BTN_A), C_A, C_A_ON);
    if (chg & (1u << BTN_B))    draw_round(B_CX, B_CY, "B", p & (1u << BTN_B), C_B, C_B_ON);
    if (chg & (1u << BTN_SELECT)) draw_pill(SEL_X, SEL_Y, "SELECT", p & (1u << BTN_SELECT));
    if (chg & (1u << BTN_START))  draw_pill(STA_X, STA_Y, "START", p & (1u << BTN_START));
    if (chg & (1u << BTN_MENU))   draw_pill(MENU_X, MENU_Y, "MENU", p & (1u << BTN_MENU));
    lcd_flush_rows(GAME_Y + GAME_H, LCD_H);
}
