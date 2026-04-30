# rpi-pico-freertos-blink

Simple Raspberry Pi Pico project that blinks the onboard LED using FreeRTOS and provides USB-CDC command interface.

## Requirements
- Raspberry Pi Pico SDK installed and PICO_SDK_PATH set
- CMake >= 3.13, a GCC toolchain for RP2040

## Build

mkdir build && cd build
cmake .. -DPICO_SDK_PATH=../../pico-sdk
make -j4

Copy the generated .uf2 to the Pico in BOOTSEL mode.

## Notes
- This project expects a FreeRTOS-Kernel directory at the repository root. If FreeRTOS is elsewhere, adjust CMakeLists.txt accordingly.

## Verification
- See [VERIFICATION.md](./VERIFICATION.md) for the verification summary, logs, WAV dumps and waveform screenshots.

## Hardware
- Board: Raspberry Pi Pico (RP2040)
- Onboard LED: GPIO 25
- USB: USB-C (USB-CDC / TinyUSB used for stdio over USB)

## Software versions and tools (observed on host)
- FreeRTOS Kernel: FreeRTOS-Kernel V11.1.0 (part of FreeRTOS 202406.00 LTS)
- Pico SDK: 2.2.0 (from pico_sdk_version.cmake)
- Toolchain: arm-none-eabi-gcc (Arm GNU Toolchain 14.2.1) — path: /usr/local/bin/arm-none-eabi-gcc

## Notes & recommendations
- Large raw logs were added under verification/logs for traceability; consider removing or moving to CI artifacts if repo size becomes an issue.
- To reproduce verification, run the provided scripts/verify_serial.sh script.
