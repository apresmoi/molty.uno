// The weather on Molty's glass: drizzle, rain or a downpour behind the creature, drops on the glass, a wet floor after.
// An agent tells it with a lifetime (POST /api/creature/state {weather: {kind, s}}) and it eases back to clear when that
// runs out; the device reports {kind, s_left, level, wet}. Here: a way to try each kind for a few minutes (not a stored
// preference) and the setting `weather_on`, kept on the device like the sky. Values: device/firmware/main/creature/weather.h.

export const WEATHER_KINDS = ['clear', 'drizzle', 'rain', 'downpour'] as const;
export type WeatherKind = (typeof WEATHER_KINDS)[number];
/** How long a try holds, s: then it eases back to clear by itself. */
export const WEATHER_TRY_S = 300;

export interface WeatherNow { kind?: unknown; s_left?: unknown; level?: unknown; wet?: unknown }

/** The setting's chips, in order. */
export const WEATHER_ON_CHOICES: ReadonlyArray<{ on: boolean; label: string }> = [
  { on: true, label: 'on (default)' },
  { on: false, label: 'off' },
];
/** Which setting chip is on for what the device reports; a missing / invalid value reads as on. */
export const weatherOnChoice = (v: unknown): boolean => (typeof v === 'boolean' ? v : true);

/** Whether this firmware has the weather: it reports weather_on and a weather object. */
export const weatherSupported = (state: { weather_on?: unknown; weather?: unknown } | null | undefined): boolean =>
  typeof state?.weather_on === 'boolean' && typeof state?.weather === 'object' && state.weather !== null;

/** The kind told now ('clear' for anything unknown). */
export const weatherKind = (w: WeatherNow | null | undefined): WeatherKind =>
  (WEATHER_KINDS as readonly string[]).includes(w?.kind as string) ? (w!.kind as WeatherKind) : 'clear';

/** The order for a try of `kind`: clear ends what holds. */
export const weatherTry = (kind: WeatherKind) => ({ weather: kind === 'clear' ? 'clear' : { kind, s: WEATHER_TRY_S } });

/** A line for what the device says: "rain, 4 min left", "clear, the floor drying", "clear". */
export function weatherLine(w: WeatherNow | null | undefined): string {
  const kind = weatherKind(w);
  const left = typeof w?.s_left === 'number' ? w.s_left : 0;
  const wet = typeof w?.wet === 'number' ? w.wet : 0;
  if (kind !== 'clear' && left > 0) return `${kind}, ${left >= 120 ? `${Math.round(left / 60)} min` : `${Math.round(left)} s`} left`;
  return wet > 0 ? 'clear, the floor drying' : 'clear';
}
