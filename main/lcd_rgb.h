#pragma once
#include <stdint.h>

#define LCD_W 480
#define LCD_H 480

/* Area del juego (NES 256x240 ampliado a 384x288, relacion 4:3) */
#define GAME_W 384
#define GAME_H 288
#define GAME_X ((LCD_W - GAME_W) / 2)
#define GAME_Y 4
/* Zona de controles tactiles (debajo del juego) */
#define CTRL_Y 300

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

void lcd_init(void);
uint16_t *lcd_fb(void);
/* Sincroniza filas [y0, y1) del framebuffer (PSRAM) con la memoria que lee el LCD */
void lcd_flush_rows(int y0, int y1);
void lcd_fill(uint16_t color);
/* Frame indexado 256x240 (lineas de 8 bits) -> area del juego, con la paleta RGB565 */
void lcd_draw_game(const uint8_t **lines, const uint16_t *palette);
