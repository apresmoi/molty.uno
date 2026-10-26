/*
 * mole_creature.h — Creature registry and dispatch layer for Molty (IDF v5.5)
 *
 * A "creature" owns the LCD framebuffer for a span of time.  At any moment
 * exactly one creature is active.  Switching creatures stops the previous
 * creature's FreeRTOS tasks / frees its state, then starts the new one.
 *
 * Boot creature and fallback: "fluid" (set by app_main via
 * mole_creature_set_fallback). It is the only registered creature today; the
 * .molty character creatures are states of the fluid (fluid/mole_fluid.c).
 *
 * Persistence is out of scope — the active creature lives in RAM only and
 * resets to the boot creature on power-cycle.
 *
 * Hardware lifecycle: molty_board_init() runs ONCE from app_main before any
 * creature is selected; creatures draw through molty_board.h and must NOT
 * initialise hardware themselves.
 */

#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Creature vtable ──────────────────────────────────────────────────────── */

typedef struct mole_creature {
    const char *name;                          /**< e.g. "fluid"              */
    esp_err_t (*init)(void);                   /**< allocate state, mount tasks */
    void      (*deinit)(void);                 /**< stop tasks, free state     */
    esp_err_t (*render)(void);                 /**< composite to fb + flush    */
    esp_err_t (*state_get_json)(char **out);   /**< heap-alloc JSON; caller free() */
    esp_err_t (*state_set_json)(const char *body); /**< parse + apply + render  */
} mole_creature_t;

/* ── Registry API ─────────────────────────────────────────────────────────── */

/**
 * Register a creature.  Registering the same name twice replaces the old entry.
 * Must be called before mole_creature_select().
 */
esp_err_t   mole_creature_register(const mole_creature_t *c);

/**
 * Switch to creature by name.
 *   1. If a creature is active, call its deinit().
 *   2. Set new current.
 *   3. Call new creature's init().
 *   4. Call new creature's render().
 * Returns ESP_ERR_NOT_FOUND if name not registered.
 */
esp_err_t   mole_creature_select(const char *name);

/**
 * Creature to fall back to when a select fails: the requested creature's init()
 * failed (or it is not registered after a prior one was torn down), so instead of
 * leaving the panel with no owner, the registry selects this one. The failed
 * select still returns its error. NULL disables the fallback.
 */
void        mole_creature_set_fallback(const char *name);

/** Name of the currently active creature, or NULL if none. */
const char *mole_creature_current(void);

/**
 * Proxy to current creature's state_get_json().
 * Returns ESP_ERR_INVALID_STATE if no creature active.
 */
esp_err_t   mole_creature_state_get(char **out);

/**
 * Proxy to current creature's state_set_json().
 * Returns ESP_ERR_INVALID_STATE if no creature active.
 */
esp_err_t   mole_creature_state_set(const char *body);

/**
 * Proxy to current creature's render().
 * Returns ESP_ERR_INVALID_STATE if no creature active.
 */
esp_err_t   mole_creature_render(void);

/** Number of registered creatures. */
size_t      mole_creature_count(void);

/** Name of registered creature at index i (0-based).  NULL if out of range. */
const char *mole_creature_name_at(size_t i);

#ifdef __cplusplus
}
#endif
