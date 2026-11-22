#pragma once
#include <stdint.h>

#define LCD_W 480
#define LCD_H 480

/* Area del juego (NES 256x240 ampliado a 384x288, relacion 4:3) */
#define GAME_W 384
#define GAME_H 288
#define GAME_X ((LCD_W - GAME_W) / 2)
#define GAME_Y 4
/* Imagen NES recortada 8 columnas por lado (overscan, como en una tele): 240 col x 1.5 = 360 px, centrada */
#define IMG_CROP 8
#define IMG_W 360
#define IMG_X (GAME_X + (GAME_W - IMG_W) / 2)
/* Zona de controles tactiles (debajo del juego) */
#define CTRL_Y 300

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

void lcd_init(void);
/* Lienzo de la interfaz (RGB565 480x480): ui.c y menu.c dibujan aqui; el panel lo lee en cada cuadro */
uint16_t *lcd_fb(void);
/* Compatibilidad: ya no hace falta marcar ni presentar, los cambios del lienzo se ven con el siguiente cuadro */
void lcd_flush_rows(int y0, int y1);
void lcd_dirty_rect(int x, int y, int w, int h);
void lcd_fill(uint16_t color);

/* Paleta de 256 colores RGB565 con la que se convierte el cuadro del juego (el array lo mantiene el llamador) */
void lcd_set_palette(const uint16_t *pal);
/* Copia libre de 256x240 bytes donde escribir el siguiente cuadro; NULL si no hay (se salta ese cuadro) */
uint8_t *lcd_frame_begin(void);
/* Publica esa copia: el panel la adopta al inicio de su siguiente cuadro */
void lcd_frame_publish(uint8_t *copia);
/* Mira del Zapper en coordenadas de pantalla, o (-1, -1) para ocultarla */
void lcd_set_mira(int x, int y);
/* Diagnostico desde la ultima llamada: cuadros del panel, ms medios de la interrupcion por cuadro, la tira mas lenta (us)
 * y cuantas tiras de 20 filas superaron 500 us (cada una dura ~650 us; si tarda mas, la imagen se desplaza) */
void lcd_stats_isr(uint32_t *cuadros, float *ms_por_cuadro, uint32_t *max_us, uint32_t *lentas);
