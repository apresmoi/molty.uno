// The astrology screens' params, computed by the molty-astro skill's own code
// (skills/molty-astro/scripts/sky.mjs, run in the browser: astro.config.mjs). Loaded when the
// simulator first asks for a scene.
export type { Show } from "@skills/molty-astro/scripts/sky.mjs";
export const loadSky = () => import("@skills/molty-astro/scripts/sky.mjs");

/** A date-time field's value (no zone) as the UTC instant the skill takes. */
export const utc = (local: string) => new Date(`${local}:00Z`);
