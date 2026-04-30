cmake_minimum_required(VERSION 3.13)
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(rpi-pico-freertos-blink C CXX ASM)

# Initialize the SDK
pico_sdk_init()

# Generate PIO header for I2S PIO program
# moved to after target definition


# Provide freertos_config target so FreeRTOS knows where FreeRTOSConfig.h lives
add_library(freertos_config INTERFACE)
# Project source dir contains FreeRTOSConfig.h
target_include_directories(freertos_config SYSTEM INTERFACE ${CMAKE_SOURCE_DIR})
# Also set deprecated variable to ensure upstream CMakeLists find the config directory
set(FREERTOS_CONFIG_FILE_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Build a static FreeRTOS kernel library from the kernel sources (RP2040 port)
add_library(freertos_kernel STATIC
    "${CMAKE_SOURCE_DIR}/../FreeRTOS-Kernel/croutine.c"
    "${CMAKE_SOURCE_DIR}/../FreeRTOS-Kernel/event_groups.c"
    "${CMAKE_SOURCE_DIR}/../FreeRTOS-Kernel/list.c"
    "${CMAKE_SOURCE_DIR}/../FreeRTOS-Kernel/queue.c"
    "${CMAKE_SOURCE_DIR}/../FreeRTOS-Kernel/stream_buffer.c"
    "${CMAKE_SOURCE_DIR}/../FreeRTOS-Kernel/tasks.c"
    "${CMAKE_SOURCE_DIR}/../FreeRTOS-Kernel/timers.c"
    "${CMAKE_SOURCE_DIR}/../FreeRTOS-Kernel/portable/ThirdParty/GCC/RP2040/port.c"
    "${CMAKE_SOURCE_DIR}/../FreeRTOS-Kernel/portable/MemMang/heap_3.c"
)

target_include_directories(freertos_kernel PUBLIC
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/../FreeRTOS-Kernel/include"
    "${CMAKE_SOURCE_DIR}/../FreeRTOS-Kernel/portable/ThirdParty/GCC/RP2040/include"
)

target_link_libraries(freertos_kernel PUBLIC pico_base_headers hardware_clocks hardware_exception pico_multicore pico_stdlib)

add_executable(rpi-pico-freertos-blink main.c)

target_link_libraries(rpi-pico-freertos-blink PRIVATE freertos_kernel pico_stdlib hardware_gpio hardware_pio hardware_dma hardware_irq hardware_pwm)

# Generate PIO header for I2S PIO program (now that target exists)
pico_generate_pio_header(rpi-pico-freertos-blink ${CMAKE_SOURCE_DIR}/i2s.pio)

# Create map/bin/hex files
pico_add_extra_outputs(rpi-pico-freertos-blink)

# Enable USB output for printf
pico_enable_stdio_usb(rpi-pico-freertos-blink 1)
pico_enable_stdio_uart(rpi-pico-freertos-blink 0)
