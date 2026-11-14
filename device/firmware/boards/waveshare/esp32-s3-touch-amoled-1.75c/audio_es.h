/*
 * SPDX-License-Identifier: MIT
 *
 * audio_es.h: speaker (ES8311 + NS4150B amp) and mics (ES7210) on one duplex I2S port
 * (Waveshare AMOLED-1.75C). The molty_speaker_ops_t / molty_mic_ops_t of the board,
 * with the 1.46's semantics (mole_audio.h): PCM16 mono in and out, volume 0..21 in
 * software, one exclusive owner per direction.
 *
 * Both directions share BCLK/LRCK/MCLK, so they share one sample rate: a request at
 * another rate retimes the port only while the other direction is idle, and is
 * refused (ESP_ERR_INVALID_STATE) while it is busy.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/* I2S at 16 kHz, both codecs configured and enabled (the amp stays on). Returns
 * MOLTY_CAP_SPEAKER | MOLTY_CAP_MIC, or 0: the shared clock needs both codecs. */
uint32_t  es_audio_init(i2c_master_bus_handle_t bus);

esp_err_t es_audio_play_raw(const int16_t *samples, size_t n, uint32_t rate_hz, int16_t amp_scale);
void      es_audio_set_volume(uint8_t vol);
esp_err_t es_audio_tx_stream_begin(uint32_t rate_hz);
esp_err_t es_audio_tx_stream_write(const int16_t *samples, size_t n);
esp_err_t es_audio_tx_stream_drain(void);
void      es_audio_tx_stream_end(void);
uint32_t  es_audio_tx_stream_played(void);

esp_err_t es_audio_record_raw(uint32_t rate_hz, uint32_t ms, int16_t **out, size_t *n);
esp_err_t es_audio_rx_start_stream(uint32_t rate_hz, uint32_t chunk_ms, uint8_t gain);
esp_err_t es_audio_rx_read_chunk(int16_t *buf, size_t chunk_samples, size_t *got);
void      es_audio_rx_stop_stream(void);
bool      es_audio_rx_stream_active(void);
