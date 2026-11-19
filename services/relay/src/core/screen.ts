// SPDX-License-Identifier: MIT
// The scene on Molty's screen, as the relay follows it: so an agent that hears the human knows what
// they are looking at and what they last touched. Host-neutral and pure: room.ts keeps one record
// per device in KV and passes it through these on each thing it learns.
//
// What the relay learns from, the later of:
//   an agent's scene.play / scene.queue / scene.clear, and a show or a state that takes the screen;
//   the device's `scene` events {why: open | back | end, pkg?, layout?, spot?} (events.ts), the one to trust;
//   its `tap` events (the spot last touched);
//   the device's own status (`scene {pkg, layout, ttl_left}`), whenever an agent asks for status;
//   and the clock: a scene nobody said ended runs out with its ttl.
// The firmware's side: device/firmware/main/scene/scene_player.c (play, queue, open one deep, back,
// tick) and fluid/mole_fluid.c (the events, the clock that waits through a turn).
//
// What the relay cannot know, and does not guess:
//   the params of a layout the device opened by itself (they derive from the package, which never
//   passes through the relay): agents get the layout under it, its params and the spot that opened it;
//   which queued scene is up when the device did not say (older firmware, a reconnect): then nothing
//   is reported until the device, its status or a play says.

import { MAX_SCENE_TTL_S, PKG_ID_RE } from "./commands.js";
import { isRecord, type JsonObject } from "./protocol.js";

export const SCREEN = {
  /** A scene counts as up this long after its ttl: the firmware's clock waits for an answer this long (SCENE_TURN_S). */
  graceMs: 20_000,
  /** A tapped spot's reading rides along this long after the tap (on screen it lasts about 10 s; then it was just read). */
  readingMs: 60_000,
  /** The params an agent is handed, as JSON bytes: over it, lists are cut down (fit). */
  maxParamsBytes: 2048,
  /** A reading, in characters (the firmware holds 160 of one). */
  maxReading: 400,
  /** The wait for an agent's answer held at most (the firmware's SCENE_THINK_S). */
  maxWaitMs: 120_000,
  /** A scene an agent just put up is not ended by what the device said about the one before it. */
  settleMs: 1500,
  /** The firmware's SCENE_QUEUE_MAX. */
  queue: 8,
};

/** A layout's name as a package spells it (packages/tools/format.mjs NAME_RE; scene.play's are a subset). */
export const LAYOUT_RE = /^[A-Za-z0-9_-]{1,32}$/;

/** A scene as an agent asked for it. */
export type Play = { pkg: string; layout: string; params: JsonObject; ttl_s: number };
type Spot = { id: string; at: number };

export type Screen = {
  pkg: string;
  layout: string;
  /** The params it plays with; null: the relay does not know them (the device put it up). */
  params: JsonObject | null;
  /** When it runs out by itself, as far as the relay can tell (unix ms). */
  until: number;
  /** When an agent's command put it up (absent: the device said so). */
  played?: number;
  /** The spot last tapped on it. */
  spot?: Spot;
  /** The device opened this layout over another one, which waits under it: that one, and the spot that opened this. */
  under?: { layout: string; params: JsonObject | null; until: number; spot?: Spot };
  /** What plays after it, in order. */
  queue: Play[];
};

type Opened = Screen & { under: NonNullable<Screen["under"]> };

/** A scene starting now with what an agent asked for. */
const started = (p: Play, now: number, queue: Play[] = []): Screen => ({ pkg: p.pkg, layout: p.layout, params: p.params, until: now + p.ttl_s * 1000, queue });

/** A scene.play that the device took. */
export const played = (p: Play, now: number): Screen => ({ ...started(p, now), played: now });

/** Is it up at `at`, as far as the relay can tell? */
export const up = (s: Screen | undefined, at: number): boolean => !!s && s.until + SCREEN.graceMs > at;

/** A scene.queue that the device took: it plays at once when nothing does, else after the others. */
export function queued(s: Screen | undefined, p: Play, now: number): Screen {
  if (!s || (!up(s, now) && s.queue.length === 0)) return played(p, now);
  return { ...s, queue: [...s.queue, p].slice(-SCREEN.queue) };
}

/** A scene nobody told the relay about: what is on, without its params. */
const unknown = (pkg: string, layout: string, until: number, spot?: Spot): Screen => ({ pkg, layout, params: null, until, ...(spot ? { spot } : {}), queue: [] });

/** The layout under an opened one is back. */
const backUnder = (s: Opened): Screen => ({
  pkg: s.pkg,
  layout: s.under.layout,
  params: s.under.params,
  until: s.under.until,
  ...(s.under.spot ? { spot: s.under.spot } : {}),
  queue: s.queue,
});

/** The device's `scene` event (cleaned, events.ts): what is up now. */
export function sceneChanged(s: Screen | undefined, ev: JsonObject, now: number): Screen | undefined {
  const { why, pkg, layout, spot } = ev as { why: string; pkg?: string; layout?: string; spot?: string };
  // an end the device sent as the agent's play arrived is about the scene before it
  if (why === "end" && s?.played !== undefined && now - s.played < SCREEN.settleMs) return s;
  if (!pkg || !layout) return undefined; // nothing is up: the creature is back
  const same = s?.pkg === pkg ? s : undefined;
  if (why === "open" && same && same.layout !== layout) {
    // a tap opened a layout over this one (one deep: opened from an opened one, it takes its place)
    const by = spot ? { id: spot, at: same.spot?.id === spot ? same.spot.at : now } : undefined;
    const under = same.under ?? { layout: same.layout, params: same.params, until: same.until, ...(by ? { spot: by } : {}) };
    return { pkg, layout, params: null, until: under.until, ...(by ? { spot: by } : {}), under, queue: same.queue };
  }
  if (why !== "end" && same?.layout === layout) return same; // it is what the relay holds
  const next = s ? s.queue.findIndex((q) => q.pkg === pkg && q.layout === layout) : -1;
  // back under an opened layout: a tap beside it, or it ran out (unless the one under ran out too and the queue's next is its like)
  if (same?.under && same.under.layout === layout && !(why === "end" && next >= 0 && now >= same.under.until)) return backUnder(same as Opened);
  // the queue's next (an earlier one the relay did not hear end is dropped)
  if (why === "end" && next >= 0) return started(s!.queue[next], now, s!.queue.slice(next + 1));
  return unknown(pkg, layout, now + MAX_SCENE_TTL_S * 1000, spot ? { id: spot, at: now } : undefined);
}

/** A tap on a spot of the scene that is up. */
export const tapped = (s: Screen | undefined, spot: string, now: number): Screen | undefined => (s && up(s, now) ? { ...s, spot: { id: spot, at: now } } : s);

/** The device's scene clock waits while Molty listens or speaks (mole_fluid.c): the relay's waits with it. */
export function held(s: Screen | undefined, from: number, to: number): Screen | undefined {
  if (!s || !up(s, from) || to <= from) return s;
  return { ...s, until: s.until + to - from, ...(s.under ? { under: { ...s.under, until: s.under.until + to - from } } : {}) };
}

/** The device came back: what it said meanwhile is lost, so the order of what was queued is no longer known. */
export const reconnected = (s: Screen | undefined): Screen | undefined => (s && s.queue.length ? { ...s, queue: [] } : s);

/**
 * The device's own status (`scene {pkg, layout, ttl_left}`, absent when none is up): the truth at that
 * moment. The relay's record gives it its params when it is plain which scene that is: the one the relay
 * holds, the one under it, or the only queued one of that layout. Otherwise the params stay unknown.
 */
export function deviceSaid(s: Screen | undefined, scene: unknown, now: number): Screen | undefined {
  if (s?.played !== undefined && now - s.played < SCREEN.settleMs) return s; // the device's status is a frame behind a play
  if (!isRecord(scene) || typeof scene.pkg !== "string" || !PKG_ID_RE.test(scene.pkg) || typeof scene.layout !== "string" || !LAYOUT_RE.test(scene.layout)) return undefined;
  const left = typeof scene.ttl_left === "number" && scene.ttl_left >= 0 && scene.ttl_left <= MAX_SCENE_TTL_S ? scene.ttl_left : MAX_SCENE_TTL_S;
  const until = now + left * 1000;
  const is = (q: { pkg: string; layout: string }) => q.pkg === scene.pkg && q.layout === scene.layout;
  if (!s) return unknown(scene.pkg, scene.layout, until);
  const after = s.queue.filter(is);
  if (is(s) && (up(s, now) || after.length === 0)) return { ...s, until, ...(s.under ? { under: { ...s.under, until: Math.max(s.under.until, until) } } : {}) };
  if (s.under && s.pkg === scene.pkg && s.under.layout === scene.layout) return { ...backUnder(s as Opened), until };
  if (!is(s) && after.length === 1) return { ...started(after[0], now, s.queue.slice(s.queue.indexOf(after[0]) + 1)), until };
  return unknown(scene.pkg, scene.layout, until);
}

const bytes = (v: unknown) => new TextEncoder().encode(JSON.stringify(v)).byteLength;

/**
 * Params small enough to hand to an agent with every utterance. Over SCREEN.maxParamsBytes, lists
 * are cut down, in this order: every element that is not of a `keep` spot (its `id`, or its `of`)
 * shrinks to `{id}` (one without an id goes); then those go too; then the params are left out (null).
 */
function fit(params: JsonObject, keep: string[]): { params: JsonObject | null; cut: boolean } {
  if (bytes(params) <= SCREEN.maxParamsBytes) return { params, cut: false };
  const kept = (e: unknown) => isRecord(e) && ((typeof e.id === "string" && keep.includes(e.id)) || (typeof e.of === "string" && keep.includes(e.of)));
  const lists = (rest: (e: unknown) => unknown[]) => Object.fromEntries(Object.entries(params).map(([k, v]) => [k, Array.isArray(v) ? v.flatMap((e) => (kept(e) ? [e] : rest(e))) : v])) as JsonObject;
  for (const rest of [(e: unknown) => (isRecord(e) && typeof e.id === "string" ? [{ id: e.id }] : []), () => []]) {
    const out = lists(rest);
    if (bytes(out) <= SCREEN.maxParamsBytes) return { params: out, cut: true };
  }
  return { params: null, cut: true };
}

/** A spot's reading: the `say` of the list element with its id (how a package's spots carry them, docs/PACKAGES.md). */
function readingOf(params: JsonObject | null, id: string): string | undefined {
  for (const v of Object.values(params ?? {})) {
    if (!Array.isArray(v)) continue;
    for (const e of v) if (isRecord(e) && e.id === id && typeof e.say === "string" && e.say) return e.say.slice(0, SCREEN.maxReading);
  }
  return undefined;
}

/**
 * What an agent is told is on Molty's screen at `at` (an utterance's `device.scene`, status's
 * `relay.scene`), or undefined when no scene is up:
 *   {pkg, layout, params, spot?, spot_age_s?, reading?, under?, trimmed?}
 * params: of the scene.play that put it up; null when the relay does not know them (the device opened
 *   this layout itself: see `under`; or it only heard the device say what is on).
 * spot: the last spot the human tapped on it, spot_age_s seconds before `at`; reading: that spot's
 *   text, while the tap is at most SCREEN.readingMs old.
 * under: {layout, params, spot?}: the layout this one was opened from, and the spot that opened it.
 * trimmed: the params were cut down to fit (see fit).
 */
export function sceneContext(s: Screen | undefined, at: number): JsonObject | undefined {
  if (!s || !up(s, at)) return undefined;
  const keep = [s.spot?.id, s.under?.spot?.id].filter((id): id is string => !!id);
  let trimmed = false;
  const small = (p: JsonObject | null) => {
    if (!p) return null;
    const f = fit(p, keep);
    trimmed ||= f.cut;
    return f.params;
  };
  const out: JsonObject = { pkg: s.pkg, layout: s.layout, params: small(s.params) };
  if (s.spot) {
    out.spot = s.spot.id;
    out.spot_age_s = Math.max(0, Math.round((at - s.spot.at) / 1000));
    const reading = at - s.spot.at <= SCREEN.readingMs ? readingOf(s.params ?? s.under?.params ?? null, s.spot.id) : undefined;
    if (reading) out.reading = reading;
  }
  if (s.under) out.under = { layout: s.under.layout, params: small(s.under.params), ...(s.under.spot ? { spot: s.under.spot.id } : {}) };
  if (trimmed) out.trimmed = true;
  return out;
}
