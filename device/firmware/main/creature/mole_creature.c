/*
 * mole_creature.c — Creature registry and dispatch for Molty (IDF v5.5)
 *
 * Capacity: up to 8 registered creatures (enough for any foreseeable roadmap).
 * No heap allocation for the registry itself — the vtable pointers are caller-owned.
 */

#include "mole_creature.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_dispatch;

static const char *TAG = "mole_creature";

/* ── Registry ────────────────────────────────────────────────────────────── */

#define MAX_CREATURES 8

static const mole_creature_t *s_registry[MAX_CREATURES];
static size_t                 s_count   = 0;
static const mole_creature_t *s_current = NULL;
static const char            *s_fallback = NULL;

void mole_creature_set_fallback(const char *name) { s_fallback = name; }

/* ── Registry API ────────────────────────────────────────────────────────── */

esp_err_t mole_creature_register(const mole_creature_t *c)
{
    if (!c || !c->name) return ESP_ERR_INVALID_ARG;
    /* Registry is populated at boot, before HTTP/link tasks start. */
    if (!s_dispatch) s_dispatch = xSemaphoreCreateRecursiveMutex();
    if (!s_dispatch) return ESP_ERR_NO_MEM;

    /* Replace existing entry with same name */
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_registry[i]->name, c->name) == 0) {
            s_registry[i] = c;
            ESP_LOGI(TAG, "creature '%s' replaced in registry", c->name);
            return ESP_OK;
        }
    }

    if (s_count >= MAX_CREATURES) {
        ESP_LOGE(TAG, "creature registry full (%d slots)", MAX_CREATURES);
        return ESP_ERR_NO_MEM;
    }

    s_registry[s_count++] = c;
    ESP_LOGI(TAG, "creature '%s' registered (slot %u)", c->name, (unsigned)(s_count - 1));
    return ESP_OK;
}

static esp_err_t dispatch_select(const char *name)
{
    if (!name) return ESP_ERR_INVALID_ARG;

    /* Find target */
    const mole_creature_t *target = NULL;
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_registry[i]->name, name) == 0) {
            target = s_registry[i];
            break;
        }
    }
    if (!target) {
        ESP_LOGW(TAG, "creature '%s' not found", name);
        return ESP_ERR_NOT_FOUND;
    }

    /* If we're already on this creature, no-op */
    if (s_current && strcmp(s_current->name, name) == 0) {
        ESP_LOGD(TAG, "creature '%s' already active — no-op", name);
        return ESP_OK;
    }

    /* Deinit current */
    if (s_current) {
        ESP_LOGI(TAG, "deinit '%s'", s_current->name);
        if (s_current->deinit) s_current->deinit();
        s_current = NULL;
    }

    /* Init new */
    ESP_LOGI(TAG, "init '%s'", target->name);
    s_current = target;   /* set before init so render() can be called from init */
    esp_err_t ret = target->init ? target->init() : ESP_OK;
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "creature '%s' init failed: %s", name, esp_err_to_name(ret));
        s_current = NULL;
        /* never leave the panel ownerless: fall back (one level — the fallback
         * selecting itself is excluded, so this cannot recurse) */
        if (s_fallback && strcmp(name, s_fallback) != 0) {
            ESP_LOGW(TAG, "falling back to creature '%s'", s_fallback);
            mole_creature_select(s_fallback);
        }
        return ret;
    }

    /* Initial render */
    if (target->render) {
        ret = target->render();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "creature '%s' initial render failed: %s",
                     name, esp_err_to_name(ret));
            /* Non-fatal — creature is still active */
        }
    }

    ESP_LOGI(TAG, "switched to creature '%s'", name);
    return ESP_OK;
}

const char *mole_creature_current(void)
{
    return s_current ? s_current->name : NULL;
}

static esp_err_t dispatch_state_get(char **out)
{
    if (!s_current) return ESP_ERR_INVALID_STATE;
    if (!s_current->state_get_json) return ESP_ERR_NOT_SUPPORTED;
    return s_current->state_get_json(out);
}

static esp_err_t dispatch_state_set(const char *body)
{
    if (!s_current) return ESP_ERR_INVALID_STATE;
    if (!s_current->state_set_json) return ESP_ERR_NOT_SUPPORTED;
    return s_current->state_set_json(body);
}

static esp_err_t dispatch_render(void)
{
    if (!s_current) return ESP_ERR_INVALID_STATE;
    if (!s_current->render) return ESP_ERR_NOT_SUPPORTED;
    return s_current->render();
}

size_t mole_creature_count(void)
{
    return s_count;
}

const char *mole_creature_name_at(size_t i)
{
    if (i >= s_count) return NULL;
    return s_registry[i]->name;
}

/* HTTP and agent-link calls may arrive on different tasks. Hold the dispatch
 * lock through deinit/init and state access so a creature cannot be freed
 * underneath the other caller. Render tasks use their own creature locks. */
esp_err_t mole_creature_select(const char *name) {
    if (!s_dispatch) return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(s_dispatch, portMAX_DELAY);
    esp_err_t e = dispatch_select(name);
    xSemaphoreGiveRecursive(s_dispatch);
    return e;
}
esp_err_t mole_creature_state_get(char **out) {
    if (!s_dispatch) return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(s_dispatch, portMAX_DELAY);
    esp_err_t e = dispatch_state_get(out);
    xSemaphoreGiveRecursive(s_dispatch);
    return e;
}
esp_err_t mole_creature_state_set(const char *body) {
    if (!s_dispatch) return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(s_dispatch, portMAX_DELAY);
    esp_err_t e = dispatch_state_set(body);
    xSemaphoreGiveRecursive(s_dispatch);
    return e;
}
esp_err_t mole_creature_render(void) {
    if (!s_dispatch) return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(s_dispatch, portMAX_DELAY);
    esp_err_t e = dispatch_render();
    xSemaphoreGiveRecursive(s_dispatch);
    return e;
}
