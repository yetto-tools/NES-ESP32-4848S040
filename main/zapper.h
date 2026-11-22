#pragma once
#include <stdbool.h>

/* Modo del Zapper tactil por juego */
enum { ZAP_AUTO, ZAP_SI, ZAP_NO };

/* Deteccion automatica por nombre de archivo (Duck Hunt, Hogan's Alley, ...) */
bool zapper_adivinar(const char *archivo);
/* Modo guardado para ese juego (ZAP_AUTO si no hay) y su persistencia en NVS */
int  zapper_modo(const char *archivo);
void zapper_guardar_modo(const char *archivo, int modo);
/* true si el Zapper quedaria activo para ese juego (modo manual o deteccion) */
bool zapper_efectivo(const char *archivo);
/* Elige el juego que se va a emular; archivo=NULL usa el ultimo jugado (ROM de la flash) */
void zapper_seleccionar(const char *archivo);
/* Guarda la firma de la ROM de la flash (llamar tras instalar una ROM desde la SD) */
void zapper_guardar_firma(void);
/* Lo consulta el emulador: true si el toque en pantalla debe actuar como Zapper */
bool zapper_activo(void);
