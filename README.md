rpi-pico-freertos-blink

Simple Raspberry Pi Pico project that blinks the onboard LED using FreeRTOS.

Requirements:
- Raspberry Pi Pico SDK installed and PICO_SDK_PATH set
- CMake >= 3.13, a GCC toolchain for RP2040

Build:

mkdir build && cd build
cmake .. -DPICO_SDK_PATH=../../pico-sdk
make -j4

Copy the generated .uf2 to the Pico in BOOTSEL mode.

Notes:
- This project expects a FreeRTOS-Kernel directory at the repository root. If FreeRTOS is elsewhere, adjust CMakeLists.txt accordingly.
