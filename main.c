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

#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "i2s.pio.h"

#define BLINK_GPIO 25
#define TONE_PIN 18  // PWM-capable pin used for tone output (change as needed)

// Default I2S pins (change via i2s init if desired)
#define I2S_DATA_PIN 15
#define I2S_BCLK_PIN 14
#define I2S_LRCLK_PIN 13

static volatile uint32_t blink_interval_ms = 500;
static volatile bool blink_enabled = true;
static volatile bool tick_enabled = true;

// Tone settings
static volatile uint32_t tone_freq_hz = 400; // default 400Hz
static volatile bool tone_running = false;

// I2S configuration
typedef struct {
    uint32_t sample_rate;
    uint8_t word_width; // bits (8/16/24/32)
    uint8_t channels; // 1=mono,2=stereo
} i2s_config_t;
static i2s_config_t i2s_cfg = {44100, 16, 2};

// I2S PIO+DMA state
static PIO i2s_pio = pio0;
static uint i2s_sm = 0xffff;
static int i2s_dma_chan = -1;
static volatile bool i2s_running = false;

// Buffer: 32-bit words, each word holds left<<16 | (right & 0xFFFF)
#define I2S_BUFFER_FRAMES 512u // power-of-two
static uint32_t i2s_buffer[I2S_BUFFER_FRAMES];
static uint32_t i2s_tone_freq = 440; // default tone for I2S

// Forward declarations
static void i2s_fill_buffer(uint32_t tone_freq_hz);
static bool i2s_init_pio_dma(void);
static void i2s_start(void);
static void i2s_stop(void);

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

// DMA IRQ handler: restart transfer to create continuous circular streaming
static void __not_in_flash_func(i2s_dma_irq0_handler)(void) {
    // acknowledge
    dma_hw->ints0 = 1u << i2s_dma_chan;
    // restart transfer
    dma_channel_set_read_addr(i2s_dma_chan, i2s_buffer, false);
    dma_channel_set_trans_count(i2s_dma_chan, I2S_BUFFER_FRAMES, true);
}

static bool i2s_init_pio_dma(void) {
    if (i2s_running) return true;
    // claim a state machine
    if (i2s_sm == 0xffff) {
        i2s_sm = pio_claim_unused_sm(i2s_pio, true);
    }
    uint offset = pio_add_program(i2s_pio, &i2s_tx_program);
    pio_sm_config c = i2s_tx_program_get_default_config(offset);
    // data out pin
    sm_config_set_out_pins(&c, I2S_DATA_PIN, 1);
    // sideset pin is BCLK
    sm_config_set_sideset_pins(&c, I2S_BCLK_PIN);
    // shift MSB first, autopull every 32 bits
    sm_config_set_out_shift(&c, false, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    // init pins
    pio_gpio_init(i2s_pio, I2S_DATA_PIN);
    pio_gpio_init(i2s_pio, I2S_BCLK_PIN);
    pio_gpio_init(i2s_pio, I2S_LRCLK_PIN);
    // set pin directions
    pio_sm_set_consecutive_pindirs(i2s_pio, i2s_sm, I2S_DATA_PIN, 1, true);
    // sideset pin direction
    pio_sm_set_consecutive_pindirs(i2s_pio, i2s_sm, I2S_BCLK_PIN, 1, true);
    // configure and enable
    pio_sm_init(i2s_pio, i2s_sm, offset, &c);

    // claim DMA channel
    i2s_dma_chan = dma_claim_unused_channel(true);
    dma_channel_config dmac = dma_channel_get_default_config(i2s_dma_chan);
    channel_config_set_transfer_data_size(&dmac, DMA_SIZE_32);
    channel_config_set_read_increment(&dmac, true);
    channel_config_set_write_increment(&dmac, false);
    channel_config_set_dreq(&dmac, pio_get_dreq(i2s_pio, i2s_sm, true));
    // set ring on read to buffer size
    // buffer bytes = I2S_BUFFER_FRAMES * 4
    int log2_bytes = 0;
    uint32_t bytes = I2S_BUFFER_FRAMES * sizeof(uint32_t);
    while ((1u << log2_bytes) < bytes) log2_bytes++;
    // ensure exact power of two
    if ((1u << log2_bytes) != bytes) return false;
    channel_config_set_ring(&dmac, true, log2_bytes);
    dma_channel_configure(i2s_dma_chan, &dmac,
                          &i2s_pio->txf[i2s_sm], // dst
                          i2s_buffer,            // src
                          I2S_BUFFER_FRAMES,     // transfer count
                          false);
    // enable irq on completion
    dma_channel_set_irq0_enabled(i2s_dma_chan, true);
    irq_set_exclusive_handler(DMA_IRQ_0, i2s_dma_irq0_handler);
    irq_set_enabled(DMA_IRQ_0, true);
    return true;
}

static void i2s_fill_buffer(uint32_t tone_freq_hz) {
    // generate sine samples for the configured sample rate
    const uint32_t S = i2s_cfg.sample_rate;
    const uint32_t N = I2S_BUFFER_FRAMES;
    for (uint32_t n = 0; n < N; ++n) {
        double t = (double)n / (double)S;
        double v = sin(2.0 * M_PI * (double)tone_freq_hz * t);
        int16_t samp = (int16_t)(v * 0x7FFF);
        uint32_t word = ((uint16_t)samp << 16) | ((uint16_t)samp & 0xFFFF);
        i2s_buffer[n] = word;
    }
}

static void i2s_start(void) {
    if (i2s_running) return;
    if (!i2s_init_pio_dma()) {
        printf("ERR: i2s init failed\r\n");
        return;
    }
    // prepare LRCLK via PWM (50% duty at sample_rate)
    // configure LRCLK pin for PWM
    uint slice = pwm_gpio_to_slice_num(I2S_LRCLK_PIN);
    gpio_set_function(I2S_LRCLK_PIN, GPIO_FUNC_PWM);
    uint32_t sys_hz = clock_get_hz(clk_sys);
    // choose clock divider and wrap so wrap <= 65535
    float clkdiv = 1.0f;
    uint32_t wrap = sys_hz / i2s_cfg.sample_rate - 1;
    if (wrap > 65535) {
        // increase divisor
        clkdiv = (float)sys_hz / (float)(i2s_cfg.sample_rate * 65536u) + 1.0f;
        wrap = (uint32_t)((float)sys_hz / (clkdiv * (float)i2s_cfg.sample_rate) - 1.0f);
        if (wrap > 65535) wrap = 65535;
    }
    pwm_set_wrap(slice, wrap);
    pwm_set_chan_level(slice, pwm_gpio_to_channel(I2S_LRCLK_PIN), wrap/2);
    pwm_set_clkdiv(slice, clkdiv);
    pwm_set_enabled(slice, true);

    // fill buffer with tone
    i2s_fill_buffer(i2s_tone_freq);

    // start PIO SM
    pio_sm_set_enabled(i2s_pio, i2s_sm, true);

    // start DMA
    dma_channel_set_read_addr(i2s_dma_chan, i2s_buffer, false);
    dma_channel_set_write_addr(i2s_dma_chan, &i2s_pio->txf[i2s_sm], false);
    dma_channel_set_trans_count(i2s_dma_chan, I2S_BUFFER_FRAMES, true);
    i2s_running = true;
    printf("ACK: i2s started (sample_rate=%u, word_width=%u, channels=%u)\r\n", (unsigned)i2s_cfg.sample_rate, (unsigned)i2s_cfg.word_width, (unsigned)i2s_cfg.channels);
    fflush(stdout);
}

static void i2s_stop(void) {
    if (!i2s_running) return;
    // stop DMA
    dma_channel_abort(i2s_dma_chan);
    // disable IRQ
    dma_channel_set_irq0_enabled(i2s_dma_chan, false);
    irq_set_enabled(DMA_IRQ_0, false);
    // stop PIO
    pio_sm_set_enabled(i2s_pio, i2s_sm, false);
    // disable PWM LRCLK
    pwm_set_enabled(pwm_gpio_to_slice_num(I2S_LRCLK_PIN), false);
    i2s_running = false;
    printf("ACK: i2s stopped\r\n");
    fflush(stdout);
}

void command_task(void *pvParameters) {
    (void) pvParameters;
    char buf[128];
    size_t idx = 0;
    int c;
    printf("Command task started. Commands: 'clocks', 'blink start', 'blink stop', 'blink interval <ms>', 'tick on', 'tick off', 'tone start', 'tone stop', 'tone freq <Hz>', 'tone measure <ms>', 'i2s init <sample_rate> <word_width> <channels>', 'i2s start', 'i2s stop', 'i2s tone <Hz>', 'bootsel', 'reset'\r\n");
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
                } else if (strncmp(p, "tone measure", 12) == 0) {
                    // tone measure <ms> : measure pin transitions for given ms (default 200 ms)
                    char *num = p + 12;
                    long ms = 200;
                    if (*num) ms = strtol(num, NULL, 10);
                    if (ms < 10) ms = 10;
                    if (ms > 2000) ms = 2000; // limit
                    if (!tone_running) {
                        printf("ERR: tone not running, cannot measure\r\n");
                        fflush(stdout);
                    } else {
                        uint64_t start = time_us_64();
                        uint64_t end = start + (uint64_t)ms * 1000ULL;
                        int prev = gpio_get(TONE_PIN);
                        uint32_t edges = 0;
                        while (time_us_64() < end) {
                            int lvl = gpio_get(TONE_PIN);
                            if (lvl != prev) {
                                edges++;
                                prev = lvl;
                            }
                        }
                        double duration_s = (double)ms / 1000.0;
                        double freq = ((double)edges) / (2.0 * duration_s);
                        printf("MEAS: edges=%u, duration_ms=%u, freq=%.2f Hz\r\n", (unsigned)edges, (unsigned)ms, freq);
                        fflush(stdout);
                    }
                } else if (strncmp(p, "i2s init ", 9) == 0) {
                    // parse three args: sample_rate, word_width, channels
                    uint32_t sr = 0; uint32_t ww = 0; uint32_t ch = 0;
                    int r = sscanf(p+9, "%u %u %u", &sr, &ww, &ch);
                    if (r >= 1) i2s_cfg.sample_rate = sr;
                    if (r >= 2) i2s_cfg.word_width = (uint8_t)ww;
                    if (r >= 3) i2s_cfg.channels = (uint8_t)ch;
                    printf("ACK: i2s configured: sample_rate=%u, word_width=%u, channels=%u\r\n", (unsigned)i2s_cfg.sample_rate, (unsigned)i2s_cfg.word_width, (unsigned)i2s_cfg.channels);
                    fflush(stdout);
                } else if (strcmp(p, "i2s start") == 0) {
                    i2s_start();
                } else if (strcmp(p, "i2s stop") == 0) {
                    i2s_stop();
                } else if (strcmp(p, "i2s status") == 0) {
                    // print runtime I2S/PIO/DMA/PWM status
                    printf("I2S status:\r\n");
                    printf("  running: %s\r\n", i2s_running ? "yes" : "no");
                    if (i2s_running) {
                        printf("  pio: pio%d\r\n", (i2s_pio==pio0)?0:1);
                        printf("  sm: %u\r\n", (unsigned)i2s_sm);
                        printf("  dma_chan: %d\r\n", i2s_dma_chan);
                    } else {
                        printf("  pio: (not running)\r\n");
                    }
                    printf("  sample_rate: %u\r\n", (unsigned)i2s_cfg.sample_rate);
                    printf("  word_width: %u\r\n", (unsigned)i2s_cfg.word_width);
                    printf("  channels: %u\r\n", (unsigned)i2s_cfg.channels);
                    printf("  tone_freq: %u Hz\r\n", (unsigned)i2s_tone_freq);
                    printf("  buffer_frames: %u\r\n", (unsigned)I2S_BUFFER_FRAMES);
                    // LRCLK PWM info
                    int lr_slice = pwm_gpio_to_slice_num(I2S_LRCLK_PIN);
                    uint32_t wrap = pwm_get_wrap(lr_slice);
                    printf("  LRCLK pwm slice: %d, wrap: %u\r\n", lr_slice, (unsigned)wrap);
                    fflush(stdout);
                } else if (strncmp(p, "i2s tone ", 9) == 0) {
                    char *num = p + 9;
                    long val = strtol(num, NULL, 10);
                    if (val < 1 || val > 15000) {
                        printf("ERR: invalid i2s tone frequency; must be 1..15000 Hz\r\n");
                    } else {
                        i2s_tone_freq = (uint32_t)val;
                        if (i2s_running) {
                            i2s_fill_buffer(i2s_tone_freq);
                        }
                        printf("ACK: i2s tone set to %u Hz\r\n", (unsigned)i2s_tone_freq);
                    }
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
    xTaskCreate(command_task, "cmd", 1024, NULL, 1, NULL);

    // Start the scheduler
    vTaskStartScheduler();

    // Should never reach here
    while (1) {
        tight_loop_contents();
    }
    return 0;
}
