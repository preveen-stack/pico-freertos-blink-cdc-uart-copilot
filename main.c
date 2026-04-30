#include <stdio.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"

#define BLINK_GPIO 25

void blink_task(void *pvParameters) {
    (void) pvParameters;
    gpio_init(BLINK_GPIO);
    gpio_set_dir(BLINK_GPIO, GPIO_OUT);
    for (;;) {
        gpio_put(BLINK_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_put(BLINK_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
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

int main() {
    stdio_init_all();

    // Create tasks
    xTaskCreate(blink_task, "blink", 256, NULL, 1, NULL);
    xTaskCreate(log_task, "log", 512, NULL, 2, NULL);

    // Start the scheduler
    vTaskStartScheduler();

    // Should never reach here
    while (1) {
        tight_loop_contents();
    }
    return 0;
}
