/* SPDX-License-Identifier: MIT, see mole_fluid.h
 *
 * scene_wheel.c: a part of mole_fluid.c, included there whole (its statics are that file's; the build lists only
 * mole_fluid.c): a scene's wheels, its centre and a beat's dots under the finger, and the frames of the two players a
 * scene draws live, the beat player's clock (beat_frame) and the tone player's wave (wave_frame). Split out for size
 * alone: the plugin loaders read a file only below 256 KiB. */
/* ── wheels (scene_player.h): a scene's dial turned by the finger, its centre, a beat's dots, all on this task ───────
 * A touch that comes down on the wheel (from its r0 out), the centre or a beat's dot is theirs until it lifts: no
 * ripple under it, no stir as it moves, no spot or tap (sense.c asks mole_fluid_zone first). The face up follows the
 * beat player: face 0 (the tempo) while it plays, face 1 (the choice) while it is stopped at rest, never changed under
 * a finger. Each click moves the value at once (the beat player's tempo; at rest, the rhythm the clock shows) and asks
 * the motor for a tick, the dial's own (sense.c takes them, mole_fluid_take_ticks). The centre's tap plays and stops
 * (rhythm_local_play), a dot's mutes its drum. The agent hears where a wheel settled when the finger leaves it
 * (mole_fluid_ring_event): a number that moved, a choice that moved. None of it changes the picture whole. */
enum { ZONE_NONE, ZONE_RING, ZONE_CENTRE, ZONE_DOT, ZONE_LABEL };
static struct {
    unsigned gen;                                      /* the scene it is for                              */
    scene_wheel_t w;
    float v0; int i0;                                  /* the value, the choice when the finger came down    */
    ring_drag_t drag;
    int zone;                                          /* the touch down now is the wheel's, the centre's, a dot's */
    char labels[SCENE_WHEEL_LABELS][PKG_KEY_MAX + 1];
    /* a pick's levels (the library has regions): 0 a region's rhythms, 1 the regions; the region the rhythms are of,
     * each label's library index (level 0), each label's tint, the regions' names and sizes, the last rhythm of each */
    int region, nregions, map[SCENE_WHEEL_LABELS], count[RHY_REGIONS], last[RHY_REGIONS];
    uint32_t tint[SCENE_WHEEL_LABELS], region_rgb[RHY_REGIONS];
    char regions[RHY_REGIONS][RHYTHM_LABEL_ROOM], counted[16];
} s_wh;
static atomic_int s_ticks;                             /* clicks the motor has not ticked yet               */
/* a wave up: the dust under it holds still (its drift and its light would send a third of the panel every frame, which a
 * wave's motion needs: wave_frame) */
static bool s_dust_hold;
/* while a wave shows, over its last 300 frames (the `wave.frames` log line): frames, those held still for the drawer's
 * backlog (the same picture again), those late (over 37.5 ms since the one before), the most cells the drawer owed */
static int s_wave_n, s_wave_held, s_wave_late, s_wave_owed;
#define WAVE_LAND_TICK (40 << 8 | 100)   /* a wave's rim: an octave's jump (ms << 8 | percent) */
#define WAVE_NOTE_TICK (30 << 8 | 90)    /* .. landing on a note (or a whole hit a second): between the octave's and the
                                          * fine step's (the dial's own, the waves package: 15 ms at 50) */
#define WAVE_HEAVY_US  6000            /* a frame's drawing cache past this: the wave's hand and rest take turns */
static atomic_int s_tick_plan;                         /* their dial's tick: ms << 8 | percent (0: the motor's own) */
static mole_ring_evt_t s_ring_evt;                     /* s_mutex */
static bool s_ring_evt_on;
/* the zones for sense.c (any task), s_mutex: the dial up (its r0), the centre (its r), the beat's dots */
#define ZONE_DOTS 64
static struct { int face; bool label; float r0, rc, ly, dx[ZONE_DOTS], dy[ZONE_DOTS], dr[ZONE_DOTS]; int nd; } s_zones;

static void ring_evt(const scene_item_t *d, const char *id_of_pick) {
    mole_ring_evt_t e = { 0 };
    const scene_t *sc = s_sp.cur;
    if (s_wh.w.level) snprintf(e.spot, sizeof(e.spot), "region");   /* (a pick's regions: one level up) */
    else snprintf(e.spot, sizeof(e.spot), "%.*s", (int)d->nsid, sc->say + d->sid);
    e.index = s_wh.w.nlabels ? s_wh.w.index : -1;
    e.value = s_wh.w.value;
    if (s_wh.w.nlabels) snprintf(e.label, sizeof(e.label), "%s", s_wh.labels[s_wh.w.index]);
    snprintf(e.id, sizeof(e.id), "%s", id_of_pick ? id_of_pick : "");
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_ring_evt = e; s_ring_evt_on = true;
    xSemaphoreGive(s_mutex);
    mole_diag_event(false, "scene.ring %s %s", e.spot, e.label[0] ? e.label : "");
}
/* a pick's labels at its level: a region's rhythms (each its library index, the region's tint), or the regions */
static void pick_labels(void) {
    s_wh.nregions = rhythm_regions(s_wh.regions, s_wh.region_rgb, s_wh.count, RHY_REGIONS);
    if (s_wh.w.level && s_wh.nregions > 1) {
        s_wh.w.nlabels = s_wh.nregions;
        for (int r = 0; r < s_wh.nregions; r++) { strcpy(s_wh.labels[r], s_wh.regions[r]); s_wh.tint[r] = s_wh.region_rgb[r]; }
        s_wh.w.index = s_wh.region;
    } else {
        s_wh.w.level = 0;
        s_wh.w.nlabels = rhythm_labels(s_wh.nregions > 1 ? s_wh.region : -1, s_wh.labels, s_wh.map, SCENE_WHEEL_LABELS);
        for (int i = 0; i < s_wh.w.nlabels; i++) s_wh.tint[i] = s_wh.nregions > 1 ? s_wh.region_rgb[s_wh.region] : 0;
    }
    s_wh.w.tints = s_wh.tint;
    /* above the name, small and in its tint: the region (a tap there goes up a level); at the regions, under the
     * region's name: how many rhythms it holds */
    s_wh.w.above = s_wh.nregions > 1 && !s_wh.w.level ? s_wh.regions[s_wh.region] : NULL;
    s_wh.w.above_rgb = s_wh.nregions > 1 ? s_wh.region_rgb[s_wh.region] : 0;
}
static void counted(void) {   /* the regions' level: the one under the notch, its size */
    const int r = s_wh.w.index, n = r >= 0 && r < s_wh.nregions ? s_wh.count[r] : 0;
    snprintf(s_wh.counted, sizeof(s_wh.counted), "%d RHYTHM%s", n, n == 1 ? "" : "S");
    s_wh.w.below = s_wh.w.level ? s_wh.counted : NULL;
}
/* the face up now: its labels (a pick's are the beat player's library, at its level) and its value */
static void wheel_face(const scene_t *sc, int face) {
    const int n = scene_dial(sc, face);
    s_wh.w.face = face; s_wh.w.since = SCENE_WHEEL_SHOW_S; s_wh.w.index = 0; s_wh.w.level = 0;
    s_wh.w.above = s_wh.w.below = NULL; s_wh.w.tints = NULL;
    if (n >= 0 && sc->items[n].sets == 2) pick_labels();
    else s_wh.w.nlabels = n < 0 ? 0 : scene_dial_labels(sc, n, s_wh.labels, SCENE_WHEEL_LABELS);
    s_wh.w.labels = (const char (*)[PKG_KEY_MAX + 1])s_wh.labels;
    if (n >= 0) s_wh.w.value = sc->items[n].val;
}
/* a pick's level: up to the regions, or into region r (its last rhythm, or its first, on the clock at once) */
static void pick_level(int level, int r) {
    s_wh.w.level = level;
    s_wh.region = r;
    pick_labels();
    if (!level && s_wh.w.nlabels) {
        int i = 0;
        for (int q = 0; q < s_wh.w.nlabels; q++) if (s_wh.map[q] == s_wh.last[r]) i = q;
        s_wh.w.index = i;
        rhythm_local_pick(s_wh.map[i]);
    }
    counted();
}
/* a wave's wheel follows the tone player: playing, the pitch (face 0); stopped, its chord kinds (face 1); never a word
 * in the middle (the wave draws its own) */
static void wave_wheel(const scene_t *sc) {
    int level, index;
    float midi, land;
    if (!wave_where(&level, &index, &midi, &land)) return;
    const int face = level == WAVE_AT_PLAYING ? 0 : 1;
    if (!s_wh.drag.down && (face != s_wh.w.face || !s_wh.w.labels)) {
        s_wh.w.face = face; s_wh.w.since = SCENE_WHEEL_SHOW_S; s_wh.w.above = s_wh.w.below = NULL;
        s_wh.w.nlabels = face && scene_dial(sc, 1) >= 0 ? wave_labels(s_wh.labels, SCENE_WHEEL_LABELS) : 0;
        s_wh.w.labels = (const char (*)[PKG_KEY_MAX + 1])s_wh.labels;
        s_wh.w.tints = NULL; s_wh.w.level = 0;
    }
    s_wh.w.quiet = true; s_wh.w.land = land;
    if (!s_wh.drag.down) { s_wh.w.index = index; s_wh.w.value = midi; }
}
/* each frame: a new scene starts its wheel afresh; the face and a beat player's dial follow the player while no
 * finger is on it */
static void wheel_frame(float rdt) {
    scene_t *sc = s_cur == ST_SCENE ? s_sp.cur : NULL;
    if (!sc || (scene_dial(sc, 0) < 0 && scene_dial(sc, 1) < 0 && scene_centre(sc) < 0)) { s_wh.gen = 0; return; }
    if (s_wh.gen != s_sp.gen) {
        memset(&s_wh, 0, sizeof(s_wh));
        for (int r = 0; r < RHY_REGIONS; r++) s_wh.last[r] = -1;
        s_wh.gen = s_sp.gen;
        wheel_face(sc, 0);
    }
    if (sc->wave >= 0) wave_wheel(sc);
    rhy_view_t v;
    const bool beat = sc->wave < 0 && rhythm_now(&v, NULL) && v.on;
    if (!s_wh.drag.down && sc->wave < 0) {
        const int face = beat && v.rest && scene_dial(sc, 1) >= 0 ? 1 : 0;
        const int n = scene_dial(sc, face);
        const bool pick = n >= 0 && beat && sc->items[n].sets == 2;
        if (pick) s_wh.last[rhythm_region_of(v.cur)] = v.cur;
        /* a new face; or at a region's rhythms, the player on a rhythm of another (the agent's pick): its region */
        if (face != s_wh.w.face || (pick && !s_wh.w.level && s_wh.nregions > 1 && rhythm_region_of(v.cur) != s_wh.region)) {
            if (pick) s_wh.region = rhythm_region_of(v.cur);
            wheel_face(sc, face);
        }
        if (n >= 0 && beat && sc->items[n].sets == 1) s_wh.w.value = v.bpm;
        if (pick && !s_wh.w.level)
            for (int q = 0; q < s_wh.w.nlabels; q++) if (s_wh.map[q] == v.cur) s_wh.w.index = q;
    }
    s_wh.w.since += rdt;
    s_wh.w.flash = fmaxf(0.f, s_wh.w.flash - rdt / 0.15f);
    s_wh.w.turning = s_wh.drag.down;
}
/* the finger turned the wheel `clicks` (clockwise positive) */
static void wheel_turn(int clicks, bool fast) {
    scene_t *sc = s_sp.cur;
    const int n = scene_dial(sc, s_wh.w.face);
    if (n < 0) return;
    const scene_item_t *d = &sc->items[n];
    if (!clicks) return;
    if (sc->wave >= 0) {                                  /* the tone player's: a chord kind, or the pitch */
        const int moved = wave_local_turn(clicks, fast);
        if (!moved) return;                               /* at its end, or a note holding: no click to feel */
        int level;
        float land;
        wave_where(&level, &s_wh.w.index, &s_wh.w.value, &land);
        s_wh.w.flash = 1.f;
        s_hold.on = false;
        /* (clicks faster than the motor's gap share one tick: the strongest of them, so a note between fine steps is felt) */
        const int plan = moved == WAVE_TURN_OCTAVE ? WAVE_LAND_TICK : moved == WAVE_TURN_NOTE ? WAVE_NOTE_TICK : d->tick_ms << 8 | d->tick;
        if (!atomic_load(&s_ticks) || (plan >> 8) >= (atomic_load(&s_tick_plan) >> 8)) atomic_store(&s_tick_plan, plan);
        atomic_fetch_add(&s_ticks, abs(clicks));
        return;
    }
    if (s_wh.w.nlabels) {
        s_wh.w.index = ((s_wh.w.index + clicks) % s_wh.w.nlabels + s_wh.w.nlabels) % s_wh.w.nlabels;
        if (d->sets == 2 && s_wh.w.level) counted();         /* the regions: the one under the notch, its size */
        else if (d->sets == 2) rhythm_local_pick(s_wh.map[s_wh.w.index]);   /* the clock shows it at once (at rest: silent) */
    } else {
        const float v = fminf(d->vmax, fmaxf(d->vmin, s_wh.w.value + clicks * (fast ? d->fast : d->by)));
        if (v == s_wh.w.value) return;                    /* at its end: no click to feel */
        s_wh.w.value = v;
        if (d->sets == 1) rhythm_local_bpm(v);            /* the rhythm keeps playing, at the new tempo at once */
    }
    s_wh.w.flash = 1.f;
    s_hold.on = false;                                    /* (a turn is never a hold) */
    atomic_store(&s_tick_plan, d->tick_ms << 8 | d->tick);
    atomic_fetch_add(&s_ticks, abs(clicks));
}
/* the words a tap takes up a level, while they show: the region's label above the name (up to the regions; a wave's
 * note there is its readout alone); where they are (glass y) */
static bool label_shown(const scene_t *sc, float *y) {
    *y = SCENE_WHEEL_ABOVE_Y;
    return !(sc && sc->wave >= 0) && s_wh.w.above != NULL;
}
static bool label_at(const scene_t *sc, float x, float y) {
    float ly;
    return label_shown(sc, &ly) && fabsf(x) <= SCENE_WHEEL_ABOVE_W && fabsf(y - ly) <= SCENE_WHEEL_ABOVE_H;
}
/* where (x, y) (glass units, y up) is: the dial up (from its r0 out), the region's label, the centre, a beat's dot (a
 * dot only while it plays: at rest the clock shows a choice) */
static int zone_at(const scene_t *sc, float x, float y, int *layer) {
    *layer = -1;
    if (!sc || s_cur != ST_SCENE || s_ask.scr.phase != FLUID_ASK_NONE || s_listen) return ZONE_NONE;
    const bool ours = s_wh.gen == s_sp.gen;
    const int d = scene_dial(sc, ours ? s_wh.w.face : 0), c = scene_centre(sc);
    if (d >= 0 && ring_grabs(sc->items[d].r0, x, y)) return ZONE_RING;
    if (ours && label_at(sc, x, y)) return ZONE_LABEL;
    if (c >= 0 && x * x + y * y <= sc->items[c].r * sc->items[c].r) return ZONE_CENTRE;
    if ((!ours || s_wh.w.face == 0) && (*layer = scene_beat_hit(sc, x, y)) >= 0) return ZONE_DOT;
    return ZONE_NONE;
}
/* the touch at (x, y) (glass units, y up) at t s: down (first), moved, or lifted (n 0). True: the wheel's (the water
 * leaves it alone). */
static bool wheel_touch(int n, float x, float y, float t) {
    scene_t *sc = s_sp.cur;
    if (n <= 0) {
        if (s_wh.zone == ZONE_RING && sc) {
            const int d = scene_dial(sc, s_wh.w.face);
            ring_up(&s_wh.drag);
            s_wh.w.since = 0.f; s_wh.w.turning = false;
            if (d >= 0 && (s_wh.w.nlabels ? s_wh.w.index != s_wh.i0 : s_wh.w.value != s_wh.v0)) {   /* settled where it moved to */
                char id[RHY_ID_MAX + 1] = "";
                if (sc->items[d].sets == 2 && !s_wh.w.level) { if (sc->wave >= 0) wave_kind_id(s_wh.w.index, id); else rhythm_id(s_wh.map[s_wh.w.index], id); }
                ring_evt(&sc->items[d], id);
            }
        }
        const bool was = s_wh.zone != ZONE_NONE;
        s_wh.zone = ZONE_NONE;
        return was;
    }
    if (s_wh.zone == ZONE_RING) {
        const int d = scene_dial(sc, s_wh.w.face);
        bool fast;
        if (d >= 0) wheel_turn(ring_move(&s_wh.drag, sc->items[d].step, x, y, t, &fast), fast);
        return true;
    }
    if (s_wh.zone != ZONE_NONE) return true;
    if (s_touch_down) return false;                       /* (a touch that began elsewhere) */
    int layer;
    s_wh.zone = zone_at(sc, x, y, &layer);
    switch (s_wh.zone) {
    case ZONE_RING: ring_down(&s_wh.drag, x, y, t); s_wh.v0 = s_wh.w.value; s_wh.i0 = s_wh.w.index; s_wh.w.turning = true; break;
    case ZONE_CENTRE:                                     /* the regions: into the one chosen; else plays and stops */
        if (sc->wave >= 0) wave_local_centre();           /* (a wave: plays, and stops to its chooser) */
        else if (s_wh.w.face == 1 && s_wh.w.level) pick_level(0, s_wh.w.index);
        else rhythm_local_play();
        break;
    case ZONE_LABEL: pick_level(1, s_wh.region); break;   /* up to the regions, on its own */
    case ZONE_DOT:                                        /* (the tap's place, 0..1 of the panel) */
        if (sc->wave >= 0) wave_local_mute(layer, (x + 1.f) * 0.5f, (1.f - y) * 0.5f);
        else rhythm_local_mute(layer, (x + 1.f) * 0.5f, (1.f - y) * 0.5f);
        break;
    default: break;
    }
    return s_wh.zone != ZONE_NONE;
}

/* The picture's own delay, for the beat (rhythm_view_ahead): from the sim reading the player to the light, measured
 * each frame (the sim's frame to its first window on the panel: the wait for the handoff, the drawer's cells and the
 * sender's start, ~45 ms on the 1.75C at 30 fps), plus half a refresh (the panel's scan reaches the middle of the
 * glass) and half a frame (a frame shows for a whole frame: its moment is its middle). The fluid task's. */
static int32_t s_view_lat_us = 45000, s_view_ahead_us = 45000 + 8400 + 16700;

/* A scene's beat (scene_player.h scene_beat): the beat player's pattern and where the speaker is in it, each frame. The
 * sound lives as long as its clock: no clock on screen for RHYTHM_ORPHAN_S stops it, and once the rhythm a clock showed
 * stops (rhythm.stop, the reject button) the clock goes too. */
static void beat_frame(float rdt) {
    scene_t *sc = s_cur == ST_SCENE ? s_sp.cur : NULL;
    const bool shown = sc && sc->beat >= 0, active = rhythm_active();
    static unsigned seen_gen;                          /* the scene whose clock showed a rhythm playing */
    rhythm_orphan(shown, rdt);
    if (shown && active) seen_gen = s_sp.gen;
    if (shown && !active && seen_gen == s_sp.gen) { seen_gen = 0; scene_event("end", NULL); scene_clear(&s_sp); return; }
    if (!shown) {
        if (sc) scene_beat(sc, &(scene_beat_t){ 0 });   /* (no beat: back to the layout's own items, for the wheel's) */
        return;
    }
    static rhy_pattern_t pt;                           /* (a static: the task's stack stays small) */
    rhythm_view_ahead(s_view_ahead_us);                /* (where the sound is when this frame is seen) */
    static int logged;                                 /* (now and then, while it shows: /api/log) */
    if (++logged >= 600) { logged = 0; mole_diag_event(false, "beat.view lat=%dms ahead=%dms sync=%dms", (int)(s_view_lat_us / 1000), (int)(s_view_ahead_us / 1000), rhythm_sync()); }
    rhy_view_t v;
    scene_beat_t b = { 0 };
    if (rhythm_now(&v, &pt) && v.on) {
        const double L = pt.start[pt.steps];
        b.on = true; b.steps = pt.steps; b.beats = pt.beats; b.nlayers = pt.nlayers; b.muted = v.mute; b.rest = v.rest;
        for (int s = 0; s < pt.steps && s < SCENE_BEAT_STEPS; s++) b.angle[s] = (float)(90.0 - 360.0 * rhy_onset(&pt, s) / L);
        for (int l = 0; l < pt.nlayers && l < SCENE_BEAT_LAYERS; l++) { b.rgb[l] = pt.layer[l].rgb; b.hits[l] = v.hits[l]; }
        b.step = v.rest ? 0 : v.paused && v.count_in ? -1 : v.step;     /* (at rest: the hand at 12 o'clock) */
        b.cin_step = v.paused ? -1 : v.cin_step;
        b.since = v.rest ? SCENE_BEAT_SWELL_S : v.since; b.slot = v.slot; b.norder = v.norder;
        b.enter = v.rest && s_wh.gen == s_sp.gen && s_wh.w.face == 1 && s_wh.w.level;   /* (the regions: the centre enters) */
        snprintf(b.label, sizeof(b.label), "%s", pt.label);
    }
    b.hide_name = s_wh.gen == s_sp.gen && scene_wheel_words(sc, &s_wh.w);
    scene_beat(sc, &b);
}

/* A scene's wave (scene_player.h scene_wave): the tone player's chord and where the speaker is in it, each frame, as the
 * beat's (the same delay and trim); the trace from the player's own tables. The sound lives as long as its circle, and
 * once the chord the circle showed stops (wave.stop, the reject button) the circle goes too. Its room: the shapes a
 * frame holds less the wheel's, the hold ring's and, while they may show, a listen's ring and a subtitle's letters. */
static void wave_frame(float rdt) {
    scene_t *sc = s_cur == ST_SCENE ? s_sp.cur : NULL;
    const bool shown = sc && sc->wave >= 0, active = wave_active();
    static unsigned seen_gen;                          /* the scene whose circle showed a chord */
    s_dust_hold = shown;
    wave_orphan(shown, rdt);
    if (shown && active) seen_gen = s_sp.gen;
    if (shown && !active && seen_gen == s_sp.gen) { seen_gen = 0; scene_event("end", NULL); scene_clear(&s_sp); return; }
    if (!shown) return;
    static struct { wave_screen_t s; scene_wave_t w; float trace[SCENE_WAVE_PTS + 1]; } *f;   /* PSRAM, once: the task's stack is small */
    if (!f && !(f = heap_caps_calloc(1, sizeof(*f), MALLOC_CAP_SPIRAM))) return;
    rhythm_view_ahead(s_view_ahead_us);
    scene_wave_t *w = &f->w;
    s_wave_n++;
    if (rdt > 0.0375f) s_wave_late++;
    if (s_draw && s_draw->nowed > s_wave_owed) s_wave_owed = s_draw->nowed;
    if (s_wave_n >= 300) {                             /* (now and then while it shows: /api/log) */
        mole_diag_event(false, "wave.frames n=%d held=%d late=%d owed_max=%d", s_wave_n, s_wave_held, s_wave_late, s_wave_owed);
        s_wave_n = s_wave_held = s_wave_late = s_wave_owed = 0;
    }
    /* The hand moves every frame, its cells first in the drawer (FLUID_TAG_LEAD: never owed, never in two pieces), the
     * rest with it while the drawer has room. A frame its cache found heavy (WAVE_HEAVY_US: a chord's many dots and
     * strokes under the hand), or one that left cells owed, and the hand and the rest take turns, a frame each: the rest
     * (the same picture again, still) while the drawer catches up, the hand on the next; never more piled on what it
     * owes, old and new pieces mixed for as long as it moved */
    static bool hand_turn;
    static int owed_was;                               /* (what it owed the frame before: owing no less, the hand waits too) */
    const int nowed = s_draw ? s_draw->nowed : 0;
    const bool owed = nowed > 0, heavy = owed || (s_draw && s_draw->cache_us > WAVE_HEAVY_US);
    hand_turn = heavy && !hand_turn && !(owed && nowed >= owed_was);
    owed_was = nowed;
    if (w->on && (owed || hand_turn)) {
        if (hand_turn && w->level == SCENE_WAVE_PLAYING && scene_wave_fuse(w->f[0]) < 0.99f && wave_now(&f->s)) w->hand = f->s.v.H;
        w->still = true; s_wave_held += !hand_turn;           /* (the same picture again) */
        scene_wave(sc, w);
        return;
    }
    const float hand = w->hand;
    *w = (scene_wave_t){ 0 };
    w->hand = hand;
    if (wave_now(&f->s)) {
        const wave_screen_t *s = &f->s;
        if (scene_wave_fuse(s->v.f[0]) < 0.99f) w->hand = s->v.H;
        w->on = true; w->level = s->level; w->nv = s->nv; w->muted = s->v.mute; w->H = s->v.H; w->fc = s->v.fc;
        for (int j = 0; j < WAV_VOICES; j++) { w->count[j] = s->count[j]; w->rgb[j] = s->rgb[j]; w->ph[j] = s->v.ph[j]; w->f[j] = s->v.f[j]; w->env[j] = s->v.env[j]; w->pluck[j] = s->v.pluck[j]; }
        if (s->t) {
            /* (from 3 turns a second the afterglow holds, whole by 7: a faster hand would turn the whole outline every frame,
             * more than the screen redraws in one) */
            const float spin = fminf(1.f, fmaxf(0.f, (s->v.fc - 3.f) / 4.f)), held = fmaxf(scene_wave_fuse(s->v.f[0]), spin * spin * (3.f - 2.f * spin));
            wav_trace(s->t, &s->v, s->v.hold ? held : 1.f, 0.5f, SCENE_WAVE_REACH, SCENE_WAVE_AFTERGLOW, f->trace, SCENE_WAVE_PTS);
            w->trace = f->trace;
        }
        w->name = s->name; w->top = s->top; w->big = s->big; w->unit = s->unit;
        w->name_rgb = s->name_rgb; w->top_rgb = s->top_rgb; w->unit_rgb = s->unit_rgb; w->tone = s->tone; w->land = s->land;
        w->mark = !(s_wh.gen == s_sp.gen && (s_wh.w.turning || s_wh.w.since < SCENE_WHEEL_SHOW_S));
    }
    w->budget = FLUID_MAX_SHAPES - 16 - SCENE_HOLD_DOTS - 4 - (s_listen || s_rg.on > 0.f || s_pg.on > 0.f ? SCENE_RING_SEGS : 0) - (s_sub ? 20 : 0);
    scene_wave(sc, w);
}
