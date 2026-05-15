cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico_w CACHE STRING "Target board")

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(pill_dispenser_project C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(pill_dispenser
    project/Pill_dispenser/main.c
)

target_link_libraries(pill_dispenser
    pico_stdlib
    hardware_i2c
    hardware_uart
)

pico_enable_stdio_usb(pill_dispenser 0)
pico_enable_stdio_uart(pill_dispenser 1)
pico_add_extra_outputs(pill_dispenser)
