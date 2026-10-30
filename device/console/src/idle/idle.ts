// Idle life (Molty settings): how much Molty moves by itself when nobody drives it — small
// idle actions such as a glance, a blink, a stretch, a yawn before it dozes off. lively: a small
// gesture every ~10–40 s; calm: fewer and smaller, ~20–60 s; off: none — it still dozes off
// after a long quiet. Idle life never makes noise. The device keeps it in NVS (POST
// /api/creature/state {idle_life: "lively" | "calm" | "off"}; GET reports it), like ack.
// Values: device/firmware/main/fluid/fluid_director.h.

export const IDLE_DEFAULT = 'lively';

/** The chips, in order. */
export const IDLE_CHOICES: ReadonlyArray<{ id: string; label: string }> = [
  { id: 'lively', label: 'lively (default)' },
  { id: 'calm', label: 'calm' },
  { id: 'off', label: 'off' },
];

/** A value the device takes. */
export const idleValid = (v: unknown): v is string => typeof v === 'string' && IDLE_CHOICES.some((c) => c.id === v);

/** Which chip is on for the device's idle life; an invalid / missing value reads as the default. */
export const idleChoice = (v: unknown): string => (idleValid(v) ? v : IDLE_DEFAULT);

/** Whether this firmware has the setting: it reports idle_life in its state. */
export const idleSupported = (state: { idle_life?: unknown } | null | undefined): boolean => typeof state?.idle_life === 'string';
