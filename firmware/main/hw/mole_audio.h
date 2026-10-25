/*
 * mole_audio.h — I2S audio driver for the Molty portal firmware (IDF v5.5)
 *
 * TX (speaker, PCM5101 DAC):  BCLK=GPIO48, LRC=GPIO38, DOUT=GPIO47
 * RX (microphone, MSM PDM):   CLK=GPIO15,  WS=GPIO2,   DIN=GPIO39
 *
 * The PCM5101 is a passive I2S slave; it has no control interface exposed
 * on this board.  Volume is applied in software by scaling samples.
 *
 * I2S TX uses i2s_std (Philips mode), I2S RX uses i2s_pdm (PDM → PCM in
 * hardware via ESP32-S3's built-in PDM2PCM filter — you get int16 samples
 * directly from i2s_channel_read()).
 *
 * Both channels are allocated lazily on first use and kept open across
 * calls to avoid re-init overhead.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise I2S TX (speaker) channel.  Idempotent — safe to call multiple
 * times (returns ESP_OK if already initialised).
 */
esp_err_t mole_audio_tx_init(uint32_t sample_rate_hz);

/**
 * Initialise I2S RX (microphone) PDM channel.  Idempotent.
 */
esp_err_t mole_audio_rx_init(uint32_t sample_rate_hz);

/**
 * Play raw int16 PCM mono samples synchronously.
 * Blocks until all samples have been clocked out.
 * amp_scale in 0..32767 — samples are multiplied by amp_scale/32767.
 */
esp_err_t mole_audio_play_raw(const int16_t *samples, size_t n_samples,
                               uint32_t sample_rate_hz, int16_t amp_scale);

/**
 * Record raw int16 PCM mono samples synchronously.
 * Blocks until n_samples samples have been captured.
 * Caller must free() the returned buffer.
 */
esp_err_t mole_audio_record_raw(uint32_t sample_rate_hz, uint32_t ms,
                                 int16_t **out_buf, size_t *out_samples);

/**
 * Software volume: 0..21 (mirrors PCM5101's 22 volume steps).
 * Applied as a linear scale to all subsequently played samples.
 * Default: 21 (full volume).
 */
void mole_audio_set_volume(uint8_t vol);

/* ── Mic streaming (non-blocking, for WebSocket use) ─────────────────────────
 *
 * Only one mic consumer is allowed at a time. Attempting to start a stream
 * while another is active (or while mole_audio_record_raw is running) returns
 * ESP_ERR_INVALID_STATE.
 *
 * Typical usage:
 *   mole_audio_rx_start_stream(16000, 200, 8)   -- opens the I2S channel
 *   loop: mole_audio_rx_read_chunk(buf, chunk_samples, &got)
 *   mole_audio_rx_stop_stream()
 *
 * mole_audio_rx_read_chunk blocks until one chunk worth of stereo samples have
 * been captured, deinterleaves the RIGHT slot, applies gain, and writes
 * chunk_samples mono int16 samples into buf.  buf must point to at least
 * chunk_samples * sizeof(int16_t) bytes.  The function is meant to be called
 * from a dedicated FreeRTOS task pinned to core 0.
 */
esp_err_t mole_audio_rx_start_stream(uint32_t sample_rate_hz,
                                      uint32_t chunk_ms,
                                      uint8_t  gain);

/**
 * Read one chunk.  Blocks for approximately chunk_ms.
 * Returns the number of mono samples written in *out_samples.
 * buf must be at least chunk_samples * sizeof(int16_t) bytes, where
 * chunk_samples was reported by mole_audio_rx_start_stream.
 */
esp_err_t mole_audio_rx_read_chunk(int16_t *buf, size_t chunk_samples,
                                    size_t *out_samples);

/**
 * Stop the stream and release mic resources.  Safe to call even if no stream
 * is active.
 */
void mole_audio_rx_stop_stream(void);

/**
 * Returns true if a mic stream is currently active.
 * Used by /api/audio/record to detect mic_busy.
 */
bool mole_audio_rx_stream_active(void);

#ifdef __cplusplus
}
#endif

/* Streaming TX: exclusive ownership until end; write from one speaker task.
 * No per-chunk fades. All local play calls return busy during the stream. */
esp_err_t mole_audio_tx_stream_begin(uint32_t rate);
esp_err_t mole_audio_tx_stream_write(const int16_t *samples, size_t n);
/* Blocks until queued PCM is clocked out, retaining stream ownership. */
esp_err_t mole_audio_tx_stream_drain(void);
void mole_audio_tx_stream_end(void);
