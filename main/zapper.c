// Seleccion del modo Zapper por juego: deteccion por nombre + ajuste manual guardado en NVS
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include "nvs.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
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

/* Firma (CRC32) de la ROM grabada en la particion nesgame: permite saber si el nombre "last" sigue
 * correspondiendo a lo que hay en la flash (una ROM grabada con idf.py flash no actualiza "last"). */
static uint32_t firma_particion(void)
{
    const esp_partition_t *p = esp_partition_find_first(0x40, 0x01, NULL);
    uint8_t buf[4096], cab[16];
    if (!p || esp_partition_read(p, 0, cab, 16) != ESP_OK || memcmp(cab, "NES", 4)) return 0;
    size_t total = 16 + (size_t)cab[4] * 16384 + (size_t)cab[5] * 8192 + ((cab[6] & 4) ? 512 : 0);
    if (total > p->size) total = p->size;
    uint32_t crc = 0;
    for (size_t off = 0; off < total; off += sizeof(buf)) {
        size_t n = total - off > sizeof(buf) ? sizeof(buf) : total - off;
        if (esp_partition_read(p, off, buf, n) != ESP_OK) return 0;
        crc = esp_rom_crc32_le(crc, buf, n);
    }
    return crc;
}

void zapper_guardar_firma(void)
{
    nvs_handle_t h;
    if (nvs_open("nes", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "lastsig", firma_particion());
        nvs_commit(h);
        nvs_close(h);
    }
}

void zapper_seleccionar(const char *archivo)
{
    char last[256];
    if (!archivo) {
        nvs_handle_t h;
        size_t len = sizeof(last);
        if (nvs_open("nes", NVS_READONLY, &h) == ESP_OK) {
            uint32_t sig = 0;
            if (nvs_get_str(h, "last", last, &len) == ESP_OK &&
                nvs_get_u32(h, "lastsig", &sig) == ESP_OK && sig == firma_particion())
                archivo = last;
            else
                ESP_LOGW("zapper", "la ROM de la flash no es la ultima instalada desde la SD: valor por defecto");
            nvs_close(h);
        }
    }
    /* Sin nombre conocido (ROM grabada directo a la flash): valor por defecto de Kconfig */
    activo = archivo ? zapper_efectivo(archivo) : CONFIG_ZAPPER_ENABLE;
    ESP_LOGI("zapper", "juego \"%s\": Zapper %s", archivo ? archivo : "(flash)", activo ? "activo" : "inactivo");
}

bool zapper_activo(void) { return activo; }
