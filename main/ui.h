#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Geometria de los controles en pantalla (coordenadas de pantalla 480x480) */
#define DPAD_CX 90
#define DPAD_CY 392
#define DPAD_R  84      /* radio de la cruceta */
#define DPAD_DEAD 20    /* zona muerta central */
#define A_CX 420
#define A_CY 372
#define B_CX 345
#define B_CY 412
#define BTN_R 40
#define SEL_X 175
#define SEL_Y 444
#define STA_X 249
#define STA_Y 444
#define MENU_X 212
#define MENU_Y 322
#define PILL_W 66
#define PILL_H 30

/* Dibuja todos los controles sin presionar */
void ui_draw_static(void);
/* Redibuja los controles segun la mascara de botones presionados */
void ui_update(uint32_t pressed);
/* Texto 8x8 escalado; (x,y) esquina superior izquierda */
void ui_text(int x, int y, const char *s, int scale, uint16_t fg);
void ui_fill_rect(int x, int y, int w, int h, uint16_t c);
/* Activa/desactiva el redibujado de los controles (el menu los desactiva) */
void ui_set_overlay(bool on);
