#pragma once
#include <stdint.h>
#include <stdbool.h>

enum { BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B, BTN_START, BTN_SELECT, BTN_COUNT };

/* Boton virtual de la interfaz (no es del control NES): volver al menu de ROMs */
#define BTN_MENU 8

/* Inicia el tactil GT911 y la tarea que lo lee */
void gamepad_init(void);
/* Bit i = 1 si el boton i esta presionado (cualquier dedo sobre su zona) */
uint32_t gamepad_read(void);
/* 1 si hay un dedo en pantalla (coordenadas de pantalla); para el menu */
int gamepad_touch(int *x, int *y);
/* Calibracion del tactil en pantalla (4 cruces). Si force=false y ya hay una guardada, solo la carga. */
void gamepad_calibrate(bool force);
