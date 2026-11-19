idf_component_register(SRCS main.c osd.c video_audio.c lcd_rgb.c ui.c gamepad.c menu.c zapper.c
                       INCLUDE_DIRS .
                       WHOLE_ARCHIVE
                       REQUIRES nofrendo esp_lcd esp_driver_gpio esp_driver_i2c esp_driver_i2s
                                esp_partition spi_flash nvs_flash esp_timer esp_rom esp_mm
                                esp_driver_sdmmc fatfs sdmmc vfs)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-incompatible-pointer-types)
