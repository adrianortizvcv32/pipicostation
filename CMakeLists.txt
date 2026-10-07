cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico CACHE STRING "Board type")
endif()
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(pico_sdk_import.cmake)
project(pico_launcher C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

# Libreria de SD + FatFs: se reutiliza la del proyecto del juego.
# (si tu carpeta esta en otro lado: -DSD_DIR=/ruta/a/no-OS-FatFS-SD-SPI-RPi-Pico)
set(SD_DIR ${CMAKE_CURRENT_LIST_DIR}/../minecraft/lib/no-OS-FatFS-SD-SPI-RPi-Pico CACHE PATH "Libreria SD")
if(NOT EXISTS ${SD_DIR}/FatFs_SPI/CMakeLists.txt)
    message(FATAL_ERROR "No encuentro la libreria de SD en ${SD_DIR}")
endif()
add_subdirectory(${SD_DIR}/FatFs_SPI build_sd)

add_executable(pico_launcher
    src/main.c
    src/st7789.c
    src/hw_config.c
    src/ui.c
    src/fondo.c
    src/bootxp.c
    src/bootxp_data.c
    src/bootxp_foot.c
    src/video.c
    src/audio.c
)
target_include_directories(pico_launcher PRIVATE src)
target_compile_options(pico_launcher PRIVATE -O2 -Wall -Wno-unused-function)
target_compile_definitions(pico_launcher PRIVATE SD_API_ANTIGUA=1)

target_link_libraries(pico_launcher
    pico_stdlib
    hardware_spi
    hardware_gpio
    hardware_dma
    hardware_pwm
    hardware_clocks
    hardware_flash
    hardware_watchdog
    hardware_sync
    FatFs_SPI
)

pico_enable_stdio_usb(pico_launcher 0)
pico_enable_stdio_uart(pico_launcher 0)
pico_add_extra_outputs(pico_launcher)

# El launcher + boot2 deben caber antes del sector de config (0x3F000).
add_custom_command(TARGET pico_launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DBIN=$<TARGET_FILE_DIR:pico_launcher>/pico_launcher.bin
            -DMAX=258048 -P ${CMAKE_CURRENT_LIST_DIR}/check_size.cmake
)
