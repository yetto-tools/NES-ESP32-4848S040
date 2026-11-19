#pragma once

/* Si hay tarjeta SD con archivos .nes, muestra el menu tactil y copia el juego
 * elegido a la particion "nesgame". Si no hay SD, no hace nada y se usa
 * el juego que ya esta en la flash. */
void rom_menu_run(void);

/* Mensaje a pantalla completa (tambien sirve para errores) */
void menu_message(const char *l1, const char *l2);
