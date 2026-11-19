cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nes_esp32_s3)

# "idf.py flash" tambien graba la ROM en la particion "nesgame"
esptool_py_flash_to_partition(flash "nesgame" "${CMAKE_SOURCE_DIR}/rom/game.nes")
