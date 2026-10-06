/*
 * Tab5 audio out: ES8388 codec (I2C 0x10 on the system bus) fed by I2S0,
 * set up the way Espressif's Tab5 BSP does it, with esp_codec_dev. The
 * speaker amplifier (SPK_EN on the IO expander) is only on while playing
 * through the speaker; with headphones in it stays off.
 */
#include "aout.h"

#include "deck_hal.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"

static const char *TAG = "aout";

/* I2S pins, from m5tab5_pinmap.h (C++ there, so repeated here). */
#define I2S_MCLK GPIO_NUM_30
#define I2S_SCLK GPIO_NUM_27
#define I2S_LCLK GPIO_NUM_29
#define I2S_DOUT GPIO_NUM_26

static i2s_chan_handle_t s_tx;
static esp_codec_dev_handle_t s_dev;
static bool s_open;
static bool s_headphones;
static int s_volume = 60;

static bool setup(void)
{
    if (s_dev) return true;

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.auto_clear        = true; /* silence, not stale samples, on underrun */
    chan.dma_desc_num      = 8;    /* ~90 ms of buffer at 44.1 kHz rides out SD hiccups */
    chan.dma_frame_num     = 512;
    if (i2s_new_channel(&chan, &s_tx, NULL) != ESP_OK) return false;
    i2s_std_config_t std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = I2S_MCLK,
                .bclk = I2S_SCLK,
                .ws   = I2S_LCLK,
                .dout = I2S_DOUT,
                .din  = I2S_GPIO_UNUSED,
            },
    };
    if (i2s_channel_init_std_mode(s_tx, &std) != ESP_OK || i2s_channel_enable(s_tx) != ESP_OK) return false;

    audio_codec_i2s_cfg_t i2s_cfg           = {.port = I2S_NUM_0, .tx_handle = s_tx};
    const audio_codec_data_if_t *data_if    = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg           = {.addr = ES8388_CODEC_DEFAULT_ADDR, .bus_handle = (i2c_master_bus_handle_t)hal_tab5_sys_i2c()};
    const audio_codec_ctrl_if_t *ctrl_if    = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if    = audio_codec_new_gpio();
    if (data_if == NULL || ctrl_if == NULL || gpio_if == NULL) return false;

    es8388_codec_cfg_t codec_cfg = {
        .ctrl_if     = ctrl_if,
        .gpio_if     = gpio_if,
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin      = -1, /* the amplifier is on the IO expander, see aout_route */
        .master_mode = false,
        .hw_gain     = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    const audio_codec_if_t *codec = es8388_codec_new(&codec_cfg);
    if (codec == NULL) return false;
    esp_codec_dev_cfg_t dev_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data_if};
    s_dev                       = esp_codec_dev_new(&dev_cfg);
    return s_dev != NULL;
}

bool aout_open(uint32_t rate, int channels)
{
    if (!setup()) {
        ESP_LOGE(TAG, "codec setup failed");
        return false;
    }
    if (s_open) esp_codec_dev_close(s_dev);
    esp_codec_dev_sample_info_t fs = {.bits_per_sample = 16, .channel = (uint8_t)channels, .sample_rate = rate};
    s_open = esp_codec_dev_open(s_dev, &fs) == ESP_CODEC_DEV_OK;
    if (!s_open) {
        ESP_LOGE(TAG, "codec open %lu Hz x%d failed", (unsigned long)rate, channels);
        return false;
    }
    esp_codec_dev_set_out_vol(s_dev, s_volume);
    s_headphones = hal_headphones();
    hal_speaker_amp(!s_headphones);
    ESP_LOGI(TAG, "%lu Hz x%d to %s", (unsigned long)rate, channels, s_headphones ? "headphones" : "speaker");
    return true;
}

void aout_write(const int16_t *pcm, size_t count)
{
    if (!s_open) return;
    esp_codec_dev_write(s_dev, (void *)pcm, (int)(count * sizeof(int16_t))); /* bytes */
}

void aout_close(void)
{
    hal_speaker_amp(false);
    if (s_open) esp_codec_dev_close(s_dev);
    s_open = false;
}

void aout_volume(int percent)
{
    s_volume = percent;
    if (s_open) esp_codec_dev_set_out_vol(s_dev, percent);
}

void aout_route(bool headphones)
{
    if (headphones == s_headphones || !s_open) return;
    s_headphones = headphones;
    hal_speaker_amp(!headphones);
    ESP_LOGI(TAG, "output: %s", headphones ? "headphones" : "speaker");
}
