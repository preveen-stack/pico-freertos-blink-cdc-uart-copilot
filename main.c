#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "pico/time.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <math.h>

#define BLINK_GPIO 25
#define TONE_PIN 18  // PWM-capable pin used for tone output (change as needed)

static volatile uint32_t blink_interval_ms = 500;
static volatile bool blink_enabled = true;
static volatile bool tick_enabled = true;

// Tone settings
static volatile uint32_t tone_freq_hz = 400; // default 400Hz
static volatile bool tone_running = false;

// I2S configuration stub
typedef struct {
    uint32_t sample_rate;
    uint8_t word_width; // bits
    uint8_t channels; // 1=mono,2=stereo
} i2s_config_t;
static i2s_config_t i2s_cfg = {44100, 16, 2};


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

// Tone (timer-based square wave) helper functions using repeating_timer
static repeating_timer_t tone_timer;

static bool tone_timer_cb(repeating_timer_t *rt) {
    // Toggle pin (fast, safe in IRQ)
    gpio_xor_mask(1u << TONE_PIN);
    return true; // keep repeating
}

static void tone_start(void) {
    // configure pin as output
    gpio_init(TONE_PIN);
    gpio_set_dir(TONE_PIN, GPIO_OUT);
    uint32_t freq = tone_freq_hz ? tone_freq_hz : 400;
    // half period in microseconds
    int64_t half_us = (1000000LL) / (2 * (int64_t)freq);
    if (half_us < 1) half_us = 1;
    // start repeating timer; if already running, update by removing and adding
    // remove if exists
    // Best-effort: try cancelling previous timer
    cancel_repeating_timer(&tone_timer);
    add_repeating_timer_us((int64_t)half_us, tone_timer_cb, NULL, &tone_timer);
    tone_running = true;
}

static void tone_stop(void) {
    cancel_repeating_timer(&tone_timer);
    // ensure pin low
    gpio_put(TONE_PIN, 0);
    tone_running = false;
}

static bool tone_set_freq(uint32_t hz) {
    if (hz == 0 || hz > 15000) return false;
    tone_freq_hz = hz;
    if (tone_running) {
        tone_start();
    }
    return true;
}

void log_task(void *pvParameters) {
    (void) pvParameters;
    uint32_t cnt = 0;
    for (;;) {
        // Print log over USB CDC (stdio is routed to USB because CMake enables it)
        if (tick_enabled) {
            printf("LOG: tick %u\r\n", (unsigned)cnt++);
            fflush(stdout);
        }
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
    printf("Command task started. Commands: 'clocks', 'blink start', 'blink stop', 'blink interval <ms>', 'tick on', 'tick off', 'tone start', 'tone stop', 'tone freq <Hz>', 'i2s init <sample_rate> <word_width> <channels>', 'bootsel', 'reset'\r\n");
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
                if (strcmp(p, "tick on") == 0) {
                    tick_enabled = true;
                    printf("ACK: tick enabled\r\n");
                    fflush(stdout);
                } else if (strcmp(p, "tick off") == 0) {
                    tick_enabled = false;
                    printf("ACK: tick disabled\r\n");
                    fflush(stdout);
                } else if (strcmp(p, "tick on") == 0) {
                    tick_enabled = true;
                    printf("ACK: tick enabled\r\n");
                    fflush(stdout);
                } else if (strcmp(p, "tick off") == 0) {
                    tick_enabled = false;
                    printf("ACK: tick disabled\r\n");
                    fflush(stdout);
                } else if (strcmp(p, "clocks") == 0) {
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
                } else if (strcmp(p, "tone start") == 0) {
                    tone_start();
                    printf("ACK: tone started (%u Hz)\r\n", (unsigned)tone_freq_hz);
                    fflush(stdout);
                } else if (strcmp(p, "tone stop") == 0) {
                    tone_stop();
                    printf("ACK: tone stopped\r\n");
                    fflush(stdout);
                } else if (strncmp(p, "tone freq ", 10) == 0) {
                    char *num = p + 10;
                    long val = strtol(num, NULL, 10);
                    if (val < 1 || val > 15000) {
                        printf("ERR: invalid tone frequency; must be 1..15000 Hz\r\n");
                    } else {
                        tone_set_freq((uint32_t)val);
                        printf("ACK: tone frequency set to %u Hz\r\n", (unsigned)tone_freq_hz);
                    }
                    fflush(stdout);
                } else if (strncmp(p, "i2s init ", 9) == 0) {
                    // parse three args: sample_rate, word_width, channels
                    uint32_t sr = 0; uint32_t ww = 0; uint32_t ch = 0;
                    int r = sscanf(p+9, "%u %u %u", &sr, &ww, &ch);
                    if (r >= 1) i2s_cfg.sample_rate = sr;
                    if (r >= 2) i2s_cfg.word_width = (uint8_t)ww;
                    if (r >= 3) i2s_cfg.channels = (uint8_t)ch;
                    printf("ACK: i2s configured: sample_rate=%u, word_width=%u, channels=%u\r\n", (unsigned)i2s_cfg.sample_rate, (unsigned)i2s_cfg.word_width, (unsigned)i2s_cfg.channels);
                    printf("NOTE: PIO I2S TX not fully implemented; stored configuration for future use.\r\n");
                    fflush(stdout);
                } else if (strcmp(p, "bootsel") == 0) {
                    printf("ACK: rebooting to BOOTSEL (entering USB mass storage bootloader)\r\n");
                    fflush(stdout);
                    sleep_ms(100);
                    reset_usb_boot(0, 0); // does not return
                } else if (strcmp(p, "reset") == 0) {
                    printf("ACK: resetting board\r\n");
                    fflush(stdout);
                    sleep_ms(100);
                    watchdog_reboot(0, 0, 0); // should reboot
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
