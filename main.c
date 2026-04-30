#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#define BLINK_GPIO 25

static volatile uint32_t blink_interval_ms = 500;
static volatile bool blink_enabled = true;

void blink_task(void *pvParameters) {
    (void) pvParameters;
    gpio_init(BLINK_GPIO);
    gpio_set_dir(BLINK_GPIO, GPIO_OUT);
    for (;;) {
        if (blink_enabled) {
            gpio_put(BLINK_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(blink_interval_ms/2));
            gpio_put(BLINK_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS((blink_interval_ms+1)/2));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

void log_task(void *pvParameters) {
    (void) pvParameters;
    uint32_t cnt = 0;
    for (;;) {
        // Print log over USB CDC (stdio is routed to USB because CMake enables it)
        printf("LOG: tick %u\r\n", (unsigned)cnt++);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void print_clocks(void) {
    printf("System clocks:\r\n");
    printf("  clk_sys:  %u Hz\r\n", (unsigned)clock_get_hz(clk_sys));
    printf("  clk_usb:  %u Hz\r\n", (unsigned)clock_get_hz(clk_usb));
    printf("  clk_peri: %u Hz\r\n", (unsigned)clock_get_hz(clk_peri));
    fflush(stdout);
}

void command_task(void *pvParameters) {
    (void) pvParameters;
    char buf[128];
    size_t idx = 0;
    int c;
    printf("Command task started. Commands: 'clocks', 'blink start', 'blink stop', 'blink interval <ms>'\r\n");
    fflush(stdout);

    for (;;) {
        c = getchar_timeout_us(100000); // 100ms timeout
        if (c == PICO_ERROR_TIMEOUT) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (c == '\r') continue;
        if (c == '\n') {
            buf[idx] = '\0';
            if (idx > 0) {
                // trim leading spaces
                char *p = buf;
                while (*p && (*p == ' ' || *p == '\t')) p++;
                if (strcmp(p, "clocks") == 0) {
                    print_clocks();
                } else if (strcmp(p, "blink start") == 0) {
                    blink_enabled = true;
                    printf("ACK: blink started\r\n");
                    fflush(stdout);
                } else if (strcmp(p, "blink stop") == 0) {
                    blink_enabled = false;
                    printf("ACK: blink stopped\r\n");
                    fflush(stdout);
                } else if (strncmp(p, "blink interval ", 15) == 0) {
                    char *num = p + 15;
                    long val = strtol(num, NULL, 10);
                    if (val < 50) val = 50; // minimum 50ms
                    blink_interval_ms = (uint32_t)val;
                    printf("ACK: blink interval set to %u ms\r\n", (unsigned)blink_interval_ms);
                    fflush(stdout);
                } else {
                    printf("ERR: unknown command: '%s'\r\n", p);
                    fflush(stdout);
                }
            }
            idx = 0;
        } else {
            if (idx < sizeof(buf) - 1) {
                buf[idx++] = (char)c;
            }
        }
    }
}

int main() {
    stdio_init_all();

    // Create tasks
    xTaskCreate(blink_task, "blink", 256, NULL, 1, NULL);
    xTaskCreate(log_task, "log", 512, NULL, 2, NULL);
    xTaskCreate(command_task, "cmd", 512, NULL, 1, NULL);

    // Start the scheduler
    vTaskStartScheduler();

    // Should never reach here
    while (1) {
        tight_loop_contents();
    }
    return 0;
}
