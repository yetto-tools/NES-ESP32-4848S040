#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include <string.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nofrendo.h"
#include "menu.h"
#include "lcd_rgb.h"
#include "gamepad.h"

static const char *TAG = "nes";

/* Nofrendo pide la ROM aqui: se lee directo de la particion "nesgame" mapeada en memoria */
char *osd_getromdata(void)
{
    const void *romdata = NULL;
    esp_partition_mmap_handle_t h;
    const esp_partition_t *part = esp_partition_find_first(0x40, 0x01, NULL);
    if (!part) {
        ESP_LOGE(TAG, "No se encontro la particion de la ROM");
        return NULL;
    }
    if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &romdata, &h) != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo mapear la ROM");
        return NULL;
    }
    const unsigned char *r = romdata;
    if (r[0] != 'N' || r[1] != 'E' || r[2] != 'S') {
        ESP_LOGE(TAG, "La particion no contiene una ROM .nes valida (graba game.nes en 0x110000)");
        return (char *)romdata;
    }
    /* Copia la ROM a PSRAM: leerla de la flash (cache) hace esperar al emulador en cada fallo de cache,
     * sobre todo al cargar mucho escenario (scroll rapido); la PSRAM es varias veces mas rapida. */
    size_t tam = 16 + ((r[6] & 4) ? 512 : 0) + (size_t)r[4] * 16384 + (size_t)r[5] * 8192;
    if (tam > part->size) tam = part->size;
    void *copia = heap_caps_malloc(tam, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copia) {
        memcpy(copia, romdata, tam);
        esp_partition_munmap(h);
        ESP_LOGI(TAG, "ROM de %u bytes copiada a PSRAM (%p)", (unsigned)tam, copia);
        return (char *)copia;
    }
    ESP_LOGW(TAG, "Sin memoria para copiar la ROM; se lee de la flash");
    return (char *)romdata;
}

static void nes_task(void *arg)
{
    nofrendo_main(0, NULL);
    /* Si vuelve, la ROM no se pudo cargar (mapper no soportado, archivo danado...) */
    ESP_LOGE(TAG, "No se pudo ejecutar la ROM (mapper no soportado?)");
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
}

void app_main(void)
{
    nvs_flash_init();
    lcd_init();
    gamepad_init();
    vTaskDelay(pdMS_TO_TICKS(400));
    int tx, ty;
    bool force = gamepad_touch(&tx, &ty);      /* dedo puesto al encender = recalibrar */
    gamepad_calibrate(force);
    rom_menu_run();      /* menu tactil de la tarjeta SD (si hay) */
    ESP_LOGI(TAG, "Iniciando emulador NES");
    xTaskCreatePinnedToCore(nes_task, "nes", 12288, NULL, 5, NULL, 1);   /* nucleo 1: solo el emulador */
}
