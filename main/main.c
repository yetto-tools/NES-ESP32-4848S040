#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_partition.h"
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
    const char *r = romdata;
    if (r[0] != 'N' || r[1] != 'E' || r[2] != 'S') {
        ESP_LOGE(TAG, "La particion no contiene una ROM .nes valida (graba game.nes en 0x110000)");
    }
    ESP_LOGI(TAG, "ROM en %p", romdata);
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
    xTaskCreatePinnedToCore(nes_task, "nes", 12288, NULL, 5, NULL, 0);
}
