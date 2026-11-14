/*
 * SPDX-License-Identifier: MIT
 *
 * audio_es.c: ES8311 speaker + ES7210 mics on one duplex I2S port (AMOLED-1.75C).
 *
 * Wiring as the Waveshare BSP (bsp_audio_init, bsp_audio_codec_{speaker,microphone}_init):
 * I2S master, standard Philips frames, MCLK = 256 × fs, both codecs I2C slaves driven
 * through espressif/esp_codec_dev's codec interfaces (registers only: this file owns
 * the I2S channels). 16-bit stereo both ways: the ES8311 plays the left slot (the mono
 * sample goes to both), the ES7210 sends MIC1 left and MIC2 right (their mean is the
 * mic signal), as the Spec_Analyzer demo reads them. The ES8311 DAC sits at 0 dB;
 * volume 0..21 scales the PCM, as on the 1.46. The amp (PA pin) stays on.
 *
 * Ownership and the say's playback position follow the 1.46 (mole_audio.c).
 */
#include "audio_es.h"
#include "board.h"
#include "molty_board.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "audio_es";

#define AUDIO_PORT      I2S_NUM_0
#define BOOT_RATE       16000
#define ADC_GAIN_DB     24.0f       /* ES7210 PGA: the Spec_Analyzer demo's CODEC_DEFAULT_ADC_VOLUME */
#define ES8311_DAC_VOL  0x32        /* DAC volume register: 0xBF = 0 dB, 0.5 dB steps */
#define FRAMES          256         /* stereo frames per I2S write */

static i2s_chan_handle_t s_tx, s_rx;
static const audio_codec_if_t *s_dac, *s_adc;
static SemaphoreHandle_t s_clk;     /* the shared clock's rate changes */
static uint32_t s_rate;
static volatile uint8_t s_volume = 21;

/* One owner per direction (the link adds a second task on top of HTTP callers). */
static portMUX_TYPE s_owner_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_tx_owned, s_rx_owned;
static bool claim(bool *owner)
{
    portENTER_CRITICAL(&s_owner_lock);
    bool ok = !*owner;
    if (ok) *owner = true;
    portEXIT_CRITICAL(&s_owner_lock);
    return ok;
}
static void release(bool *owner)
{
    portENTER_CRITICAL(&s_owner_lock);
    *owner = false;
    portEXIT_CRITICAL(&s_owner_lock);
}

static esp_err_t codecs_fs(uint32_t rate)
{
    esp_codec_dev_sample_info_t fs = { .bits_per_sample = 16, .channel = 2, .sample_rate = rate };
    return s_dac->set_fs(s_dac, &fs) == 0 && s_adc->set_fs(s_adc, &fs) == 0 ? ESP_OK : ESP_FAIL;
}

/* Run the shared clock at `rate`. The caller owns its direction; `other` is the other's. */
static esp_err_t clock_to(uint32_t rate, const bool *other)
{
    xSemaphoreTake(s_clk, portMAX_DELAY);
    esp_err_t e = ESP_OK;
    if (rate != s_rate) {
        if (*other) {
            e = ESP_ERR_INVALID_STATE;
        } else {
            i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
            i2s_channel_disable(s_tx);
            i2s_channel_disable(s_rx);
            e = i2s_channel_reconfig_std_clock(s_tx, &clk);
            if (e == ESP_OK) e = i2s_channel_reconfig_std_clock(s_rx, &clk);
            i2s_channel_enable(s_tx);
            i2s_channel_enable(s_rx);
            if (e == ESP_OK) e = codecs_fs(rate);
            s_rate = e == ESP_OK ? rate : 0;     /* 0: retime on the next request */
        }
    }
    xSemaphoreGive(s_clk);
    return e;
}

uint32_t es_audio_init(i2c_master_bus_handle_t bus)
{
    s_clk = xSemaphoreCreateMutex();
    if (!s_clk) return 0;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_PORT, I2S_ROLE_MASTER);
    cc.auto_clear = true;
    i2s_std_config_t std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(BOOT_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_MCLK, .bclk = AUDIO_BCLK, .ws = AUDIO_LRCK,
            .dout = AUDIO_DOUT, .din = AUDIO_DIN,
        },
    };
    esp_err_t e = i2s_new_channel(&cc, &s_tx, &s_rx);
    if (e == ESP_OK) e = i2s_channel_init_std_mode(s_tx, &std);
    if (e == ESP_OK) e = i2s_channel_init_std_mode(s_rx, &std);
    if (e == ESP_OK) e = i2s_channel_enable(s_tx);
    if (e == ESP_OK) e = i2s_channel_enable(s_rx);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "I2S failed (%s): no speaker, no mic", esp_err_to_name(e));
        return 0;
    }
    s_rate = BOOT_RATE;

    /* MCLK runs: the codecs can be configured */
    es8311_codec_cfg_t dac = {
        .ctrl_if    = audio_codec_new_i2c_ctrl(&(audio_codec_i2c_cfg_t){
                          .port = BOARD_I2C_PORT, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus }),
        .gpio_if    = audio_codec_new_gpio(),
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin     = AUDIO_PA,
        .use_mclk   = true,
    };
    es7210_codec_cfg_t adc = {
        .ctrl_if      = audio_codec_new_i2c_ctrl(&(audio_codec_i2c_cfg_t){
                            .port = BOARD_I2C_PORT, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = bus }),
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    esp_codec_dev_sample_info_t fs = { .bits_per_sample = 16, .channel = 2, .sample_rate = BOOT_RATE };
    uint32_t up = 0;
    if (dac.ctrl_if && dac.gpio_if && (s_dac = es8311_codec_new(&dac)) &&
        s_dac->set_fs(s_dac, &fs) == 0 && s_dac->enable(s_dac, true) == 0 &&
        s_dac->set_reg(s_dac, ES8311_DAC_VOL, 0xBF) == 0) up |= MOLTY_CAP_SPEAKER;
    else ESP_LOGW(TAG, "ES8311 failed: no speaker");
    if (adc.ctrl_if && (s_adc = es7210_codec_new(&adc)) &&
        s_adc->set_fs(s_adc, &fs) == 0 && s_adc->enable(s_adc, true) == 0 &&
        s_adc->set_mic_gain(s_adc, ADC_GAIN_DB) == 0) up |= MOLTY_CAP_MIC;
    else ESP_LOGW(TAG, "ES7210 failed: no mic");
    /* a retime sets both codecs' rate: one without the other cannot follow it */
    if (up != (MOLTY_CAP_SPEAKER | MOLTY_CAP_MIC)) return 0;
    ESP_LOGI(TAG, "ES8311 + ES7210 up, %d Hz", BOOT_RATE);
    return up;
}

void es_audio_set_volume(uint8_t vol) { s_volume = vol > 21 ? 21 : vol; }

/* mono → both slots; blocks while the DMA ring is full */
static esp_err_t tx_out(const int16_t *mono, size_t n)
{
    int16_t st[FRAMES * 2];
    while (n) {
        const size_t k = n > FRAMES ? FRAMES : n;
        for (size_t i = 0; i < k; i++) st[2 * i] = st[2 * i + 1] = mono[i];
        size_t wrote = 0;
        esp_err_t e = i2s_channel_write(s_tx, st, k * 4, &wrote, pdMS_TO_TICKS(1000));
        if (e != ESP_OK) return e;
        if (wrote != k * 4) return ESP_ERR_TIMEOUT;
        mono += k; n -= k;
    }
    return ESP_OK;
}

/* the 1.46's clip playback: amp × volume, 5 ms fades against the click at either end */
static esp_err_t play_raw(const int16_t *samples, size_t n, int16_t amp)
{
    const uint32_t vol = s_volume;
    size_t fade = s_rate * 5 / 1000;
    if (fade * 2 > n) fade = n / 2;
    int16_t buf[FRAMES];
    for (size_t done = 0; done < n;) {
        const size_t k = n - done > FRAMES ? FRAMES : n - done;
        for (size_t i = 0; i < k; i++) {
            const size_t idx = done + i;
            int32_t s = (int32_t)samples[idx] * amp / 32767 * (int32_t)vol / 21;
            if (idx < fade) s = s * (int32_t)idx / (int32_t)fade;
            else if (fade && idx >= n - fade) s = s * (int32_t)(n - 1 - idx) / (int32_t)fade;
            buf[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
        }
        esp_err_t e = tx_out(buf, k);
        if (e != ESP_OK) return e;
        done += k;
    }
    return ESP_OK;
}

esp_err_t es_audio_play_raw(const int16_t *samples, size_t n, uint32_t rate, int16_t amp)
{
    if (!claim(&s_tx_owned)) return ESP_ERR_INVALID_STATE;
    esp_err_t e = clock_to(rate, &s_rx_owned);
    if (e == ESP_OK) e = play_raw(samples, n, amp);
    release(&s_tx_owned);
    return e;
}

/* say playback position: samples handed to I2S since stream_begin, and the DMA ring
 * they wait in before the DAC plays them */
static volatile uint32_t s_tx_written, s_tx_dma;

esp_err_t es_audio_tx_stream_begin(uint32_t rate)
{
    if (!claim(&s_tx_owned)) return ESP_ERR_INVALID_STATE;
    esp_err_t e = clock_to(rate, &s_rx_owned);
    if (e != ESP_OK) { release(&s_tx_owned); return e; }
    i2s_chan_info_t info;
    s_tx_written = 0;
    s_tx_dma = i2s_channel_get_info(s_tx, &info) == ESP_OK ? info.total_dma_buf_size / 4 : 0;
    return ESP_OK;
}

uint32_t es_audio_tx_stream_played(void)
{
    const uint32_t w = s_tx_written, d = s_tx_dma;
    return w > d ? w - d : 0;
}

esp_err_t es_audio_tx_stream_write(const int16_t *samples, size_t n)
{
    int16_t buf[FRAMES];
    while (n) {
        const size_t k = n > FRAMES ? FRAMES : n;
        for (size_t i = 0; i < k; i++) buf[i] = (int16_t)((int32_t)samples[i] * s_volume / 21);
        esp_err_t e = tx_out(buf, k);
        if (e != ESP_OK) return e;
        s_tx_written += (uint32_t)k;
        samples += k; n -= k;
    }
    return ESP_OK;
}

/* A whole DMA ring of silence behind the PCM: once it is queued, the PCM has played. */
esp_err_t es_audio_tx_stream_drain(void)
{
    i2s_chan_info_t info;
    esp_err_t e = i2s_channel_get_info(s_tx, &info);
    if (e != ESP_OK) return e;
    static const int16_t silence[128];
    for (size_t left = info.total_dma_buf_size / 4 + 1; left;) {
        const size_t k = left > 128 ? 128 : left;
        if ((e = es_audio_tx_stream_write(silence, k)) != ESP_OK) return e;
        left -= k;
    }
    return ESP_OK;
}

void es_audio_tx_stream_end(void) { release(&s_tx_owned); }

/* ── mic ─────────────────────────────────────────────────────────────────────── */

/* stereo frames → mono (mean of the two mics) × gain, clipped */
static void rx_mix(const int16_t *st, int16_t *out, size_t n, int32_t gain)
{
    for (size_t i = 0; i < n; i++) {
        int32_t s = ((int32_t)st[2 * i] + st[2 * i + 1]) / 2 * gain;
        out[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
}

/* the 1.46's clip capture: 8× gain, read in 4 KB DMA-capable chunks, PCM in PSRAM */
static esp_err_t record_raw(uint32_t ms, int16_t **out, size_t *n_out)
{
    if (ms > 10000) ms = 10000;
    const size_t n = (size_t)((uint64_t)s_rate * ms / 1000), chunk = 1024;
    int16_t *st = heap_caps_malloc(chunk * 4, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!st) return ESP_ERR_NO_MEM;
    int16_t *mono = heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!mono) mono = malloc(n * sizeof(int16_t));
    if (!mono) { heap_caps_free(st); return ESP_ERR_NO_MEM; }
    size_t got = 0;
    while (got < n) {
        const size_t want = n - got > chunk ? chunk : n - got;
        size_t bytes = 0;
        esp_err_t e = i2s_channel_read(s_rx, st, want * 4, &bytes, pdMS_TO_TICKS(1000));
        if (e != ESP_OK) { heap_caps_free(st); free(mono); return e; }
        if (bytes < 4) break;
        rx_mix(st, mono + got, bytes / 4, 8);
        got += bytes / 4;
    }
    heap_caps_free(st);
    *out = mono;
    *n_out = got;
    return ESP_OK;
}

esp_err_t es_audio_record_raw(uint32_t rate, uint32_t ms, int16_t **out, size_t *n)
{
    if (!claim(&s_rx_owned)) return ESP_ERR_INVALID_STATE;
    esp_err_t e = clock_to(rate, &s_tx_owned);
    if (e == ESP_OK) e = record_raw(ms, out, n);
    release(&s_rx_owned);
    return e;
}

static int16_t *s_stream_buf;          /* one chunk of stereo frames, DMA-capable */
static size_t s_stream_frames;
static uint32_t s_stream_ms, s_stream_gain;
static volatile bool s_stream_active;

esp_err_t es_audio_rx_start_stream(uint32_t rate, uint32_t chunk_ms, uint8_t gain)
{
    if (!claim(&s_rx_owned)) return ESP_ERR_INVALID_STATE;
    rate = rate < 8000 ? 8000 : rate > 32000 ? 32000 : rate;
    chunk_ms = chunk_ms < 50 ? 50 : chunk_ms > 500 ? 500 : chunk_ms;
    gain = gain < 1 ? 1 : gain > 32 ? 32 : gain;
    esp_err_t e = clock_to(rate, &s_tx_owned);
    const size_t frames = (size_t)((uint64_t)rate * chunk_ms / 1000);
    if (e == ESP_OK && !(s_stream_buf = heap_caps_malloc(frames * 4, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)))
        e = ESP_ERR_NO_MEM;
    if (e != ESP_OK) { release(&s_rx_owned); return e; }
    s_stream_frames = frames;
    s_stream_ms = chunk_ms;
    s_stream_gain = gain;
    s_stream_active = true;
    ESP_LOGI(TAG, "mic stream: %lu Hz, %lu ms chunks, gain %u",
             (unsigned long)rate, (unsigned long)chunk_ms, (unsigned)gain);
    return ESP_OK;
}

esp_err_t es_audio_rx_read_chunk(int16_t *buf, size_t chunk_samples, size_t *got)
{
    *got = 0;
    if (!s_stream_active) return ESP_ERR_INVALID_STATE;
    size_t bytes = 0;
    esp_err_t e = i2s_channel_read(s_rx, s_stream_buf, s_stream_frames * 4, &bytes,
                                   pdMS_TO_TICKS(s_stream_ms * 3 + 500));
    if (e != ESP_OK) return e;
    size_t n = bytes / 4;
    if (n > chunk_samples) n = chunk_samples;
    rx_mix(s_stream_buf, buf, n, (int32_t)s_stream_gain);
    *got = n;
    return ESP_OK;
}

void es_audio_rx_stop_stream(void)
{
    if (!s_stream_active) return;
    s_stream_active = false;
    heap_caps_free(s_stream_buf);
    s_stream_buf = NULL;
    ESP_LOGI(TAG, "mic stream stopped");
    release(&s_rx_owned);
}

bool es_audio_rx_stream_active(void) { return s_stream_active; }
