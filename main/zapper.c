// Seleccion del modo Zapper por juego: deteccion por nombre + ajuste manual guardado en NVS
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include "nvs.h"
#include "sdkconfig.h"
#include "zapper.h"

static volatile bool activo = CONFIG_ZAPPER_ENABLE;

/* Juegos de pistola conocidos (nombre normalizado: minusculas y solo letras/digitos) */
static const char *const CLAVES[] = {
    "duckhunt", "hogansalley", "wildgunman", "gumshoe", "operationwolf", "freedomforce",
    "gotcha", "laserinvasion", "madcity", "shootingrange", "totheearth", "barkerbill",
    "babyboomer", "bayoubilly", "chiller", "crimebusters", "mechanizedattack",
    "tropicalangel", "masterchu", "lightgun",
};

static void normalizar(const char *in, char *out, size_t max)
{
    size_t n = 0;
    for (; *in && n + 1 < max; in++)
        if (isalnum((unsigned char)*in)) out[n++] = (char)tolower((unsigned char)*in);
    out[n] = 0;
}

bool zapper_adivinar(const char *archivo)
{
    char n[96];
    normalizar(archivo, n, sizeof(n));
    for (size_t i = 0; i < sizeof(CLAVES) / sizeof(CLAVES[0]); i++)
        if (strstr(n, CLAVES[i])) return true;
    return false;
}

/* Clave NVS (maximo 15 caracteres): "z" + FNV-1a del nombre normalizado */
static void clave(const char *archivo, char *key)
{
    char n[96];
    normalizar(archivo, n, sizeof(n));
    uint32_t h = 2166136261u;
    for (const char *p = n; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
    static const char *hx = "0123456789abcdef";
    key[0] = 'z';
    for (int i = 0; i < 8; i++) key[1 + i] = hx[(h >> (28 - 4 * i)) & 15];
    key[9] = 0;
}

int zapper_modo(const char *archivo)
{
    nvs_handle_t h;
    uint8_t v = ZAP_AUTO;
    char k[10];
    clave(archivo, k);
    if (nvs_open("nes", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, k, &v) != ESP_OK || v > ZAP_NO) v = ZAP_AUTO;
        nvs_close(h);
    }
    return v;
}

void zapper_guardar_modo(const char *archivo, int modo)
{
    nvs_handle_t h;
    char k[10];
    clave(archivo, k);
    if (nvs_open("nes", NVS_READWRITE, &h) == ESP_OK) {
        if (modo == ZAP_AUTO) nvs_erase_key(h, k); else nvs_set_u8(h, k, (uint8_t)modo);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool zapper_efectivo(const char *archivo)
{
    int m = zapper_modo(archivo);
    return m == ZAP_SI || (m == ZAP_AUTO && zapper_adivinar(archivo));
}

void zapper_seleccionar(const char *archivo)
{
    char last[256];
    if (!archivo) {
        nvs_handle_t h;
        size_t len = sizeof(last);
        if (nvs_open("nes", NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_str(h, "last", last, &len) == ESP_OK) archivo = last;
            nvs_close(h);
        }
    }
    /* Sin nombre conocido (ROM grabada directo a la flash): valor por defecto de Kconfig */
    activo = archivo ? zapper_efectivo(archivo) : CONFIG_ZAPPER_ENABLE;
}

bool zapper_activo(void) { return activo; }
