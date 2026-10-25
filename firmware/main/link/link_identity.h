#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"
esp_err_t link_identity_init(void);
esp_err_t link_identity_forget(void);
const char *link_identity_id(void);
const char *link_identity_code(void);
void link_identity_rotate(void);
bool link_identity_tick(void);
cJSON *link_identity_hello(void);
