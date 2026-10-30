#include "link_sdk_stub.h"
#include "link_identity.h"
#include "link_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    assert(link_identity_init() == ESP_OK);
    char first[LINK_ID_LEN+1]; strcpy(first,link_identity_id());
    uint8_t key[32]; memcpy(key,sdk_key,32);
    assert(link_id_valid(first) && !strcmp(first,sdk_id));
    assert(sdk_commits==1 && sdk_key_len==32);
    assert(link_identity_init()==ESP_OK && !strcmp(first,link_identity_id()));
    assert(sdk_commits==1 && !memcmp(key,sdk_key,32));
    cJSON *hello=link_identity_hello(); assert(hello);
    assert(!strcmp(cJSON_GetObjectItem(hello,"id")->valuestring,first));
    assert(cJSON_GetObjectItem(hello,"key") && cJSON_GetObjectItem(hello,"link_code"));
    assert(!strcmp(cJSON_GetObjectItem(hello,"board")->valuestring,"test-board"));
    char *caps=cJSON_PrintUnformatted(cJSON_GetObjectItem(hello,"caps"));
    assert(!strcmp(caps,"[\"fluid\",\"say\",\"say_cues\",\"listen\",\"listen_stop\",\"listen_window\",\"ack\",\"shake_talk\",\"show\",\"mood\",\"creature\",\"ask\",\"update\",\"log_upload\"]")); free(caps);
    cJSON_Delete(hello);
    /* no speaker / mic (the 1.28): no say / listen, so the relay refuses those jobs */
    sdk_caps &= ~(MOLTY_CAP_SPEAKER | MOLTY_CAP_MIC);
    hello=link_identity_hello(); caps=cJSON_PrintUnformatted(cJSON_GetObjectItem(hello,"caps"));
    assert(!strcmp(caps,"[\"fluid\",\"show\",\"mood\",\"creature\",\"ask\",\"update\",\"log_upload\"]")); free(caps); cJSON_Delete(hello);
    /* no touch: nothing could answer a question, so no ask and no update (nobody could
     * confirm it on screen; the relay gates on both caps) */
    sdk_caps &= ~MOLTY_CAP_TOUCH;
    hello=link_identity_hello(); caps=cJSON_PrintUnformatted(cJSON_GetObjectItem(hello,"caps"));
    assert(!strcmp(caps,"[\"fluid\",\"show\",\"mood\",\"creature\",\"log_upload\"]")); free(caps); cJSON_Delete(hello);
    sdk_caps = 0xFFFFFFFFu;
    char code[9]; strcpy(code,link_identity_code());
    sdk_now=899999999; assert(!link_identity_tick());
    sdk_now=900000000; assert(link_identity_tick());
    assert(strcmp(code,link_identity_code()));
    assert(!strcmp(first,link_identity_id()) && !memcmp(key,sdk_key,32));
    strcpy(sdk_url,"ws://lan:8787");
    assert(link_identity_forget()==ESP_OK && sdk_erases==1 && !sdk_url[0]);
    assert(link_id_valid(link_identity_id()) && strcmp(first,link_identity_id()));
    assert(memcmp(key,sdk_key,32));
    /* Migrate old installations that stored only the MAC identity's key. */
    memcpy(key,sdk_key,32); sdk_id[0]=0;
    assert(link_identity_init()==ESP_OK && link_id_valid(link_identity_id()));
    assert(memcmp(key,sdk_key,32));
    strcpy(sdk_id,"molty-a1b2c3");
    assert(link_identity_init()==ESP_ERR_INVALID_SIZE);
    puts("LINK IDENTITY PASS: persisted ID/key, reboot, rotation, forget and legacy migration");
}
