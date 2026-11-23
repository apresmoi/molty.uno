// The demo's sky and weather (/demo/): data and small sums only. What the controls call is the module's own entry
// (core.ts: time, weather, state {sky, weather_on}, demoStar). The kinds and lifetimes are the firmware's
// (device/firmware/main/creature/weather.h), the moon's nights are its sky's (sky.h, sky_test.c).

/** The weather an agent tells (POST /api/creature/state {weather: {kind, s}}), in the module's order (wm_weather). */
export const WEATHER_KINDS = ["clear", "drizzle", "rain", "downpour"] as const;
export type WeatherKind = (typeof WEATHER_KINDS)[number];

/** How long a told weather holds before it eases back to clear, s (weather.h WX_LIFE_MIN_S..MAX_S; an hour when not
 *  given). */
export const WEATHER_LIFETIMES: ReadonlyArray<[number, string]> = [
  [60, "1 min"],
  [600, "10 min"],
  [3600, "1 h"],
  [3 * 3600, "3 h"],
];

/** What the module reports (GET /api/creature/state weather), read as a line: "rain · 54 min left · floor wet 100%". */
export function weatherLine(w: { kind?: string; s_left?: number; level?: number; wet?: number } | undefined): string {
  if (!w) return "-";
  const left = w.s_left ?? 0;
  const time = left >= 120 ? `${Math.round(left / 60)} min left` : `${left} s left`;
  const parts = [w.kind && w.kind !== "clear" ? `${w.kind} · ${time}` : "clear"];
  if ((w.level ?? 0) > 0 && Math.abs((w.level ?? 0) - WEATHER_KINDS.indexOf((w.kind ?? "clear") as WeatherKind)) > 0.01)
    parts.push(`easing (${(w.level ?? 0).toFixed(2)})`);
  if ((w.wet ?? 0) > 0) parts.push(`floor wet ${Math.round((w.wet ?? 0) * 100)}%`);
  return parts.join(" · ");
}

/** A mean lunation from a known new moon (2000-01-06 18:14 UTC): good to about half a day, enough to jump to a night
 *  (the module draws the moon of each day's noon). */
const SYNODIC_DAYS = 29.530588853;
const NEW_MOON_MS = Date.UTC(2000, 0, 6, 18, 14);
const DAY_MS = 86_400_000;

/** The moon's age at `t`, days since the new moon. */
export const moonAge = (t: number) => ((((t - NEW_MOON_MS) / DAY_MS) % SYNODIC_DAYS) + SYNODIC_DAYS) % SYNODIC_DAYS;
/** Its lit fraction at `t`, 0..1 (as sky.c from the elongation, the elongation here from the mean age). */
export const moonLit = (t: number) => (1 - Math.cos((2 * Math.PI * moonAge(t)) / SYNODIC_DAYS)) / 2;
/** The next new (0) or full (0.5) moon after `t` (ms). */
export function nextPhase(t: number, phase: 0 | 0.5): number {
  const days = (phase * SYNODIC_DAYS - moonAge(t) + SYNODIC_DAYS) % SYNODIC_DAYS;
  return t + (days < 0.5 ? days + SYNODIC_DAYS : days) * DAY_MS;
}

/** Nights worth a jump (sky_test.c checks the module's moon on the first; the others from sky_moon_at). */
export const MOON_NIGHTS: ReadonlyArray<[string, number]> = [
  ["supermoon 2016", Date.UTC(2016, 10, 14, 13, 52)], // full, 356,5xx km: the largest drawn moon
  ["micromoon 2026", Date.UTC(2026, 4, 31, 16, 0)], // full, 406,2xx km: the smallest
];

/** The day slider's reach either side of the date picked, days. */
export const DAY_SPAN = 15;

/** The date field's value (datetime-local, UTC) for `t`, and back. */
export const toField = (t: number) => new Date(t).toISOString().slice(0, 16);
export const fromField = (v: string) => Date.parse(`${v}:00Z`);
