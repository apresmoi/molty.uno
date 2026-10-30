/*
 * mole_audio.c — I2S audio driver for the Molty portal firmware (IDF v5.5)
 *
 * Speaker (PCM5101, I2S TX standard mode):
 *   BCLK = GPIO48,  LRC/WS = GPIO38,  DOUT = GPIO47
 *
 * Microphone (MSM PDM mic, I2S RX PDM mode):
 *   CLK  = GPIO15,  WS    = GPIO2,    DIN  = GPIO39
 *   The ESP32-S3's hardware PDM2PCM filter is used, so i2s_channel_read()
 *   returns int16 PCM samples directly — no software demodulation needed.
 *
 * Pin assignments from Waveshare 1.46 demo bundle: .../Audio_PCM5101.h and MIC_MSM.h.
 */

#include "mole_audio.h"
#include "board.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include "driver/i2s_common.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "mole_audio";
static void rx_release(void);

// Pins: board.h

// I2S port assignments: TX on I2S_NUM_0, RX on I2S_NUM_1.
// IMPORTANT — the Waveshare 1.46 mic is **standard-mode I2S, NOT PDM**, despite
// being labeled "MSM PDM" in some places. Waveshare's own MIC_MSM.cpp uses
// `i2s.begin(I2S_MODE_STD, 16000, 16-bit, STEREO)` with all four pins
// (BCK + WS + DOUT placeholder + DIN). We follow that here. The PDM-RX
// I2S_NUM_0 constraint therefore doesn't apply to us; we can put TX or RX
// on either port. Keeping RX on I2S_NUM_1 to avoid any TX/RX contention on 0.
#define AUDIO_TX_I2S_PORT  I2S_NUM_0
#define AUDIO_RX_I2S_PORT  I2S_NUM_1

// ── Module state ───────────────────────────────────────────────────────────────
static i2s_chan_handle_t s_tx_chan = NULL;
static i2s_chan_handle_t s_rx_chan = NULL;
static uint32_t          s_tx_rate = 0;
static uint32_t          s_rx_rate = 0;

/* Software volume: 0..21, default 21 (full scale).
 * Applied as (sample * s_vol_scale) / 21 during playback. */
static volatile uint8_t s_volume = 21;

// ── TX init ────────────────────────────────────────────────────────────────────

esp_err_t mole_audio_tx_init(uint32_t sample_rate_hz)
{
    if (s_tx_chan && s_tx_rate == sample_rate_hz) return ESP_OK;

    if (s_tx_chan) {
        i2s_channel_disable(s_tx_chan);
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
    }

    /* Allocate TX channel on I2S0 */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_TX_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(
        i2s_new_channel(&chan_cfg, &s_tx_chan, NULL),
        TAG, "i2s_new_channel TX");

    /* Standard mode (Philips), mono 16-bit */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_TX_BCLK,
            .ws   = AUDIO_TX_LRC,
            .dout = AUDIO_TX_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(s_tx_chan, &std_cfg),
        TAG, "i2s_channel_init_std_mode TX");
    ESP_RETURN_ON_ERROR(
        i2s_channel_enable(s_tx_chan),
        TAG, "i2s_channel_enable TX");

    s_tx_rate = sample_rate_hz;
    ESP_LOGI(TAG, "TX I2S started: %lu Hz, BCLK=%d LRC=%d DOUT=%d",
             (unsigned long)sample_rate_hz, AUDIO_TX_BCLK, AUDIO_TX_LRC, AUDIO_TX_DOUT);
    return ESP_OK;
}

// ── RX init ────────────────────────────────────────────────────────────────────

esp_err_t mole_audio_rx_init(uint32_t sample_rate_hz)
{
    if (s_rx_chan && s_rx_rate == sample_rate_hz) return ESP_OK;

    if (s_rx_chan) {
        i2s_channel_disable(s_rx_chan);
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_RX_I2S_PORT, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(
        i2s_new_channel(&chan_cfg, NULL, &s_rx_chan),
        TAG, "i2s_new_channel RX");

    // Standard-mode I2S RX, 16-bit stereo. The "MSM" mic on the 1.46 is an
    // I2S device, not a PDM device (Waveshare's MIC_MSM.cpp uses I2S_MODE_STD
    // + STEREO at 16 kHz). Hardware emits L+R 16-bit frames; the mic's data
    // appears on one slot and the other slot is silent. We deinterleave the
    // active slot in mole_audio_record_raw.
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_RX_CLK,    // GPIO 15
            .ws   = AUDIO_RX_WS,     // GPIO 2
            .dout = I2S_GPIO_UNUSED,
            .din  = AUDIO_RX_DIN,    // GPIO 39
            .invert_flags = {0},
        },
    };

    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(s_rx_chan, &std_cfg),
        TAG, "i2s_channel_init_std_mode RX");
    ESP_RETURN_ON_ERROR(
        i2s_channel_enable(s_rx_chan),
        TAG, "i2s_channel_enable RX");

    s_rx_rate = sample_rate_hz;
    ESP_LOGI(TAG, "RX STD started: %lu Hz, BCK=%d WS=%d DIN=%d",
             (unsigned long)sample_rate_hz, AUDIO_RX_CLK, AUDIO_RX_WS, AUDIO_RX_DIN);
    return ESP_OK;
}

// ── Volume ─────────────────────────────────────────────────────────────────────

void mole_audio_set_volume(uint8_t vol)
{
    if (vol > 21) vol = 21;
    s_volume = vol;
}

// ── Playback (synchronous) ─────────────────────────────────────────────────────

/*
 * NOTE: mole_audio_play_raw() blocks the calling task until all samples have
 * been clocked out via I2S DMA.  The HTTP handler that calls this will be
 * blocked for the duration of playback.  esp_http_server workers are
 * independent tasks, so other HTTP requests are not affected.
 */
static esp_err_t play_raw(const int16_t *samples, size_t n_samples,
                               uint32_t sample_rate_hz, int16_t amp_scale)
{
    ESP_RETURN_ON_ERROR(mole_audio_tx_init(sample_rate_hz), TAG, "tx_init");

    /* Apply software volume (s_volume / 21) and caller amplitude combined.
     * Both are applied as integer multiply + shift to avoid floating point in
     * the inner loop.  amp_scale is already in int16 units (max 32767). */
    uint8_t vol = s_volume;

    /* 5 ms linear fade-in + fade-out kills the click on start (jump from
     * silence to first sample) and end (abrupt I2S TX channel disable).
     * Computed in samples; clamped if the clip is too short. */
    uint32_t fade_n = (sample_rate_hz * 5) / 1000;
    if (fade_n * 2 > n_samples) fade_n = (uint32_t)(n_samples / 2);

    /* Write in 512-sample chunks to keep stack small. */
    const size_t CHUNK = 512;
    int16_t *buf = malloc(CHUNK * sizeof(int16_t));
    if (!buf) return ESP_ERR_NO_MEM;

    esp_err_t ret = ESP_OK;
    size_t written_total = 0;
    while (written_total < n_samples) {
        size_t n = n_samples - written_total;
        if (n > CHUNK) n = CHUNK;

        for (size_t i = 0; i < n; i++) {
            size_t idx = written_total + i;
            int32_t s = (int32_t)samples[idx];
            /* Scale by amp_scale (caller) and volume (user setting) */
            s = s * amp_scale / 32767;
            s = s * vol / 21;
            /* Fade in over first fade_n samples; fade out over last fade_n */
            if (fade_n > 0) {
                if (idx < fade_n) {
                    s = s * (int32_t)idx / (int32_t)fade_n;
                } else if (idx >= n_samples - fade_n) {
                    size_t k = n_samples - 1 - idx;
                    s = s * (int32_t)k / (int32_t)fade_n;
                }
            }
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            buf[i] = (int16_t)s;
        }

        size_t bytes_written = 0;
        ret = i2s_channel_write(s_tx_chan, buf, n * sizeof(int16_t),
                                &bytes_written, pdMS_TO_TICKS(5000));
        if (ret != ESP_OK) break;
        written_total += n;
    }

    free(buf);
    return ret;
}

// ── Recording (synchronous) ────────────────────────────────────────────────────

/*
 * NOTE: mole_audio_record_raw() blocks the calling task for the duration of
 * recording (up to 10 s per spec).  Other HTTP requests are unaffected.
 */
static esp_err_t record_raw(uint32_t sample_rate_hz, uint32_t ms,
                                 int16_t **out_buf, size_t *out_samples)
{
    if (ms > 10000) ms = 10000;

    ESP_RETURN_ON_ERROR(mole_audio_rx_init(sample_rate_hz), TAG, "rx_init");

    // RX is STEREO 16-bit (see rx_init): hardware emits L+R frames at
    // sample_rate_hz, so for `ms` of audio we receive 2*n int16 samples.
    // We read the stereo stream into a temp buffer and copy every other
    // sample into the mono output. The Waveshare mic puts its data on the
    // LEFT slot (index 0, 2, 4, ...) — confirmed by Waveshare's ESP_SR
    // setup using SR_CHANNELS_STEREO and the SR library reading channel 0.
    size_t n_mono   = (size_t)((uint64_t)sample_rate_hz * ms / 1000);

    // The stereo buffer is the I2S read destination — must be DMA-capable
    // (internal DRAM), not PSRAM. Without MALLOC_CAP_DMA the default malloc
    // may return PSRAM under CONFIG_SPIRAM_USE_MALLOC and i2s_channel_read
    // fails with ESP_ERR_NO_MEM. Internal DRAM is scarce (~95 KB free with
    // the world creature up), so read in fixed chunks instead of the whole
    // clip: a 2 s capture used to need 128 KB here and failed. The mono
    // output lives in PSRAM (only touched by CPU + the WAV-emit path).
    const size_t chunk_frames = 1024;  // 4 KB stereo chunk
    int16_t *stereo = heap_caps_malloc(chunk_frames * 2 * sizeof(int16_t),
                                        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!stereo) return ESP_ERR_NO_MEM;
    int16_t *mono   = heap_caps_malloc(n_mono * sizeof(int16_t),
                                        MALLOC_CAP_SPIRAM);
    if (!mono) mono = malloc(n_mono * sizeof(int16_t));
    if (!mono) { heap_caps_free(stereo); return ESP_ERR_NO_MEM; }

    // Apply 8x default software gain — the MSM mic outputs low-level samples
    // (designed for close talk at modest amplitude); without gain, recordings
    // sound very quiet when played back. Clip to int16 range.
    const int32_t gain = 8;
    size_t frames_got = 0;
    while (frames_got < n_mono) {
        size_t want = n_mono - frames_got;
        if (want > chunk_frames) want = chunk_frames;
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(s_rx_chan, stereo,
                                         want * 2 * sizeof(int16_t),
                                         &bytes_read, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            heap_caps_free(stereo); heap_caps_free(mono);
            return ret;
        }
        size_t got = (bytes_read / sizeof(int16_t)) / 2;
        if (got == 0) break;
        for (size_t i = 0; i < got; i++) {
            // RIGHT slot (odd index). LEFT was silent in practice — the MSM mic
            // on the 1.46 outputs on the I2S right channel. Empirically verified.
            int32_t s = (int32_t)stereo[i * 2 + 1] * gain;
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            mono[frames_got + i] = (int16_t)s;
        }
        frames_got += got;
    }
    heap_caps_free(stereo);

    *out_samples = frames_got;
    *out_buf     = mono;
    return ESP_OK;
}

// ── Mic streaming state ────────────────────────────────────────────────────────
// Protected by the assumption that only one caller uses this API at a time.
// The WebSocket handler enforces mutual exclusion via s_mic_stream_active before
// spawning a task; record_raw checks the same flag.

static volatile bool     s_stream_active      = false;
static volatile uint32_t s_stream_rate        = 0;
static volatile uint32_t s_stream_chunk_ms    = 0;
static volatile uint32_t s_stream_gain        = 8;
static volatile size_t   s_stream_chunk_samples = 0;

// DMA stereo buffer for streaming (reused across chunks; allocated once per stream).
static int16_t *s_stream_stereo_buf = NULL;
static size_t   s_stream_stereo_samples = 0;  // number of int16 values in the buffer

static esp_err_t rx_start_stream(uint32_t sample_rate_hz,
                                      uint32_t chunk_ms,
                                      uint8_t  gain)
{
    if (s_stream_active) return ESP_ERR_INVALID_STATE;

    // Clamp parameters per spec
    if (sample_rate_hz < 8000)  sample_rate_hz = 8000;
    if (sample_rate_hz > 32000) sample_rate_hz = 32000;
    if (chunk_ms < 50)   chunk_ms = 50;
    if (chunk_ms > 500)  chunk_ms = 500;
    if (gain < 1)  gain = 1;
    if (gain > 32) gain = 32;

    ESP_RETURN_ON_ERROR(mole_audio_rx_init(sample_rate_hz), TAG, "rx_init");

    size_t chunk_samples   = (size_t)((uint64_t)sample_rate_hz * chunk_ms / 1000);
    size_t stereo_samples  = chunk_samples * 2;

    int16_t *sbuf = heap_caps_malloc(stereo_samples * sizeof(int16_t),
                                     MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!sbuf) return ESP_ERR_NO_MEM;

    s_stream_stereo_buf     = sbuf;
    s_stream_stereo_samples = stereo_samples;
    s_stream_rate           = sample_rate_hz;
    s_stream_chunk_ms       = chunk_ms;
    s_stream_gain           = gain;
    s_stream_chunk_samples  = chunk_samples;
    s_stream_active         = true;

    ESP_LOGI(TAG, "mic stream started: rate=%lu, chunk_ms=%lu, gain=%u, chunk_samples=%zu",
             (unsigned long)sample_rate_hz, (unsigned long)chunk_ms,
             (unsigned)gain, chunk_samples);
    return ESP_OK;
}

esp_err_t mole_audio_rx_read_chunk(int16_t *buf, size_t chunk_samples,
                                    size_t *out_samples)
{
    if (!s_stream_active || !s_stream_stereo_buf) return ESP_ERR_INVALID_STATE;

    size_t bytes_to_read = s_stream_stereo_samples * sizeof(int16_t);
    size_t timeout_ms    = s_stream_chunk_ms * 3 + 500;  // generous timeout

    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(s_rx_chan,
                                     s_stream_stereo_buf,
                                     bytes_to_read,
                                     &bytes_read,
                                     pdMS_TO_TICKS(timeout_ms));
    if (ret != ESP_OK) {
        *out_samples = 0;
        return ret;
    }

    size_t frames_got = (bytes_read / sizeof(int16_t)) / 2;
    if (frames_got > chunk_samples) frames_got = chunk_samples;

    const int32_t gain = (int32_t)s_stream_gain;
    for (size_t i = 0; i < frames_got; i++) {
        // RIGHT slot (odd index) — same as record_raw, empirically verified.
        int32_t s = (int32_t)s_stream_stereo_buf[i * 2 + 1] * gain;
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        buf[i] = (int16_t)s;
    }

    *out_samples = frames_got;
    return ESP_OK;
}

void mole_audio_rx_stop_stream(void)
{
    if (!s_stream_active) return;

    s_stream_active = false;

    if (s_stream_stereo_buf) {
        heap_caps_free(s_stream_stereo_buf);
        s_stream_stereo_buf     = NULL;
        s_stream_stereo_samples = 0;
    }

    // Leave the I2S channel open — mole_audio_rx_init is idempotent and
    // reinitialising every time adds latency for no benefit. The channel is
    // disabled/re-created only when the sample rate changes.
    ESP_LOGI(TAG, "mic stream stopped");
    rx_release();
}

bool mole_audio_rx_stream_active(void)
{
    return s_stream_active;
}

/* The link adds a second task using I2S. Claim ownership at the driver seam,
 * including local HTTP callers; never let one caller reconfigure a live channel. */
static portMUX_TYPE s_owner_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_tx_owned, s_rx_owned;
/* say playback position: samples handed to I2S since stream_begin, and the DMA ring
 * they sit in before the DAC plays them (mole_audio_tx_stream_played) */
static volatile uint32_t s_tx_written, s_tx_dma;
static bool claim(bool *owner) {
    portENTER_CRITICAL(&s_owner_lock);
    bool ok = !*owner;
    if (ok) *owner = true;
    portEXIT_CRITICAL(&s_owner_lock);
    return ok;
}
static void release(bool *owner) {
    portENTER_CRITICAL(&s_owner_lock);
    *owner = false;
    portEXIT_CRITICAL(&s_owner_lock);
}

esp_err_t mole_audio_play_raw(const int16_t *samples, size_t n, uint32_t rate, int16_t amp) {
    if (!claim(&s_tx_owned)) return ESP_ERR_INVALID_STATE;
    esp_err_t e = play_raw(samples, n, rate, amp);
    release(&s_tx_owned);
    return e;
}
esp_err_t mole_audio_record_raw(uint32_t rate, uint32_t ms, int16_t **out, size_t *n) {
    if (!claim(&s_rx_owned)) return ESP_ERR_INVALID_STATE;
    esp_err_t e = record_raw(rate, ms, out, n);
    release(&s_rx_owned);
    return e;
}
esp_err_t mole_audio_rx_start_stream(uint32_t rate, uint32_t ms, uint8_t gain) {
    if (!claim(&s_rx_owned)) return ESP_ERR_INVALID_STATE;
    esp_err_t e = rx_start_stream(rate, ms, gain);
    if (e != ESP_OK) release(&s_rx_owned);
    return e;
}

esp_err_t mole_audio_tx_stream_begin(uint32_t rate) {
    if (!claim(&s_tx_owned)) return ESP_ERR_INVALID_STATE;
    esp_err_t e = mole_audio_tx_init(rate);
    if (e != ESP_OK) { release(&s_tx_owned); return e; }
    i2s_chan_info_t info;
    s_tx_written = 0;
    s_tx_dma = i2s_channel_get_info(s_tx_chan, &info) == ESP_OK ? info.total_dma_buf_size / sizeof(int16_t) : 0;
    return ESP_OK;
}
/* Samples the DAC has played since stream_begin: written minus the DMA ring's worth
 * still queued (the writer blocks on a full ring, so it is full while PCM flows). After
 * an underrun it lags by at most the ring (6 × 240 = 90 ms at 16 kHz), never leads. */
uint32_t mole_audio_tx_stream_played(void) {
    const uint32_t w = s_tx_written, d = s_tx_dma;
    return w > d ? w - d : 0;
}
esp_err_t mole_audio_tx_stream_write(const int16_t *samples, size_t n) {
    int16_t buf[512];
    while (n) {
        size_t count = n > 512 ? 512 : n;
        for (size_t i = 0; i < count; i++) buf[i] = (int32_t)samples[i] * s_volume / 21;
        size_t written = 0;
        esp_err_t e = i2s_channel_write(s_tx_chan, buf, count * 2, &written, 1000);
        if (e != ESP_OK) return e;
        if (written != count * 2) return ESP_ERR_TIMEOUT;
        s_tx_written += (uint32_t)count;
        samples += count; n -= count;
    }
    return ESP_OK;
}
esp_err_t mole_audio_tx_stream_drain(void) {
    i2s_chan_info_t info;
    esp_err_t e = i2s_channel_get_info(s_tx_chan, &info);
    if (e != ESP_OK) return e;
    /* Refill one complete DMA ring with silence. Each descriptor is writable
     * only after its previous samples have been sent; crossing the entire ring
     * therefore clocks out even a partially filled final audio descriptor.
     * Query the actual DMA size rather than assuming IDF's default geometry. */
    const int16_t silence[128] = {0};
    size_t remaining = info.total_dma_buf_size / sizeof(int16_t) + 1;
    while (remaining) {
        size_t n = remaining > 128 ? 128 : remaining;
        e = mole_audio_tx_stream_write(silence, n);
        if (e != ESP_OK) return e;
        remaining -= n;
    }
    return ESP_OK;
}
void mole_audio_tx_stream_end(void) { release(&s_tx_owned); }

static void rx_release(void) { release(&s_rx_owned); }
