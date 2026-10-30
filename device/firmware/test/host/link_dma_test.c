/* Compile the exact streaming functions from mole_audio.c, with I2S modeled
 * as a bounded FIFO. This checks the drain implementation, not just its call. */
#include "link_sdk_stub.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef void *i2s_chan_handle_t;
typedef struct { unsigned total_dma_buf_size; } i2s_chan_info_t;
static i2s_chan_handle_t s_tx_chan=(void *)1;
static unsigned s_volume=21;
static bool s_tx_owned;
static volatile uint32_t s_tx_written, s_tx_dma;
static int16_t fifo[2048];
static size_t capacity, queued, played_audio;
static esp_err_t write_error;
static bool claim(bool *owned) { if(*owned) return false; *owned=true; return true; }
static void release(bool *owned) { *owned=false; }
esp_err_t mole_audio_tx_init(uint32_t rate) { assert(rate==16000); return ESP_OK; }
esp_err_t i2s_channel_get_info(i2s_chan_handle_t chan,i2s_chan_info_t *info) {
    assert(chan==s_tx_chan); info->total_dma_buf_size=(unsigned)(capacity*2); return ESP_OK;
}
esp_err_t i2s_channel_write(i2s_chan_handle_t chan,const void *data,size_t size,size_t *written,unsigned timeout) {
    assert(chan==s_tx_chan && timeout==1000 && s_tx_owned && !(size&1));
    if(write_error) return write_error;
    const int16_t *p=data;
    for(size_t i=0;i<size/2;i++) {
        if(queued==capacity) {
            if(fifo[0]) played_audio++;
            memmove(fifo,fifo+1,(--queued)*sizeof(*fifo));
        }
        fifo[queued++]=p[i];
    }
    *written=size; return ESP_OK;
}
#include "audio_stream_under_test.inc"
int main(void) {
    const size_t geometries[]={17,240*6,2048};
    const size_t lengths[]={1,239,240,241,1025,4096};
    int16_t pcm[4096]; for(size_t i=0;i<4096;i++) pcm[i]=1234;
    for(size_t g=0;g<3;g++) for(size_t n=0;n<6;n++) {
        capacity=geometries[g]; queued=played_audio=0;
        assert(mole_audio_tx_stream_begin(16000)==ESP_OK && s_tx_owned);
        assert(mole_audio_tx_stream_begin(16000)==ESP_ERR_INVALID_STATE);
        assert(mole_audio_tx_stream_played()==0);
        assert(mole_audio_tx_stream_write(pcm,lengths[n])==ESP_OK);
        assert(played_audio<lengths[n]); /* final PCM is still queued */
        /* the playback position: what really left the FIFO, never ahead of it */
        assert(mole_audio_tx_stream_played()==played_audio);
        assert(mole_audio_tx_stream_drain()==ESP_OK && s_tx_owned);
        assert(played_audio==lengths[n]); /* final PCM really left the FIFO */
        assert(mole_audio_tx_stream_played()>=lengths[n]);  /* ...and the position says so */
        mole_audio_tx_stream_end(); assert(!s_tx_owned);
    }
    assert(mole_audio_tx_stream_begin(16000)==ESP_OK);
    write_error=ESP_ERR_TIMEOUT;
    assert(mole_audio_tx_stream_drain()==ESP_ERR_TIMEOUT);
    mole_audio_tx_stream_end();
    /* a long stream: the position tracks the DAC exactly while the ring is full */
    write_error=ESP_OK; capacity=240*6; queued=played_audio=0;
    assert(mole_audio_tx_stream_begin(16000)==ESP_OK);
    for (int i=0;i<10;i++) { assert(mole_audio_tx_stream_write(pcm,4096)==ESP_OK); assert(mole_audio_tx_stream_played()==played_audio && played_audio==(i+1)*4096u-capacity); }
    mole_audio_tx_stream_end();
    puts("LINK DMA PASS: actual driver drain across 18 PCM/geometry combinations, ownership and I2S failure, playback position");
}
