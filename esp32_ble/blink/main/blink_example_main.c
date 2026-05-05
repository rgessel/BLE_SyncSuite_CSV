#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

#include "driver/i2s_std.h"
#include "led_strip.h"
#include "esp_timer.h"

#define LED_GPIO   2
#define LED_COUNT  1

#define I2S_BCLK 26
#define I2S_WS   25
#define I2S_DIN  33

#define SAMPLE_RATE 16000

// Trigger / release thresholds (tune these)
#define SOUND_THRESHOLD    500000
#define RELEASE_THRESHOLD  200000   // must be LOWER than SOUND_THRESHOLD

static led_strip_handle_t strip;
static i2s_chan_handle_t rx_chan;

// INMP441: 24-bit sample in 32-bit slot -> shift right 8
static inline int32_t abs_s24_from_slot32(int32_t slot32)
{
    int32_t s = slot32 >> 8;
    if (s < 0) s = -s;
    return s;
}

void app_main(void)
{
    // ----- LED init -----
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = LED_COUNT,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &strip);
    led_strip_clear(strip);
    led_strip_refresh(strip);

    // ----- I2S RX init -----
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_new_channel(&chan_cfg, NULL, &rx_chan);

    i2s_std_config_t i2s_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT,
                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .bclk = I2S_BCLK,
            .ws   = I2S_WS,
            .din  = I2S_DIN,
            .dout = I2S_GPIO_UNUSED,
            .mclk = I2S_GPIO_UNUSED,
        },
    };
    i2s_channel_init_std_mode(rx_chan, &i2s_cfg);
    i2s_channel_enable(rx_chan);

    // We read stereo frames: [L(32), R(32), ...]
    int32_t samples[64];

    // "Armed" means we're ready to detect a new clap.
    bool armed = true;

    while (1) {
        size_t bytes_read = 0;
        i2s_channel_read(rx_chan, samples, sizeof(samples), &bytes_read, portMAX_DELAY);

        // Compute peak over this chunk (use LEFT channel; L/R pin = GND on INMP441)
        int32_t peak = 0;
        int frames = (int)(bytes_read / 8); // 8 bytes per stereo frame

        for (int i = 0; i < frames; i++) {
            int32_t L_abs = abs_s24_from_slot32(samples[i * 2]); // left slot
            if (L_abs > peak) peak = L_abs;
        }

        // Gate printing: only print on the first chunk that crosses the threshold
        if (armed && peak > SOUND_THRESHOLD) {
            int64_t t_us = esp_timer_get_time(); // microseconds since boot
            printf("[%" PRId64 " us] Sound detected! Peak = %ld\n", t_us, (long)peak);

            led_strip_set_pixel(strip, 0, 0, 255, 0); // green
            led_strip_refresh(strip);

            armed = false; // disarm until signal drops
        }

        // Re-arm once things quiet down
        if (!armed && peak < RELEASE_THRESHOLD) {
            led_strip_clear(strip);
            led_strip_refresh(strip);
            armed = true;
        }
    }
}
