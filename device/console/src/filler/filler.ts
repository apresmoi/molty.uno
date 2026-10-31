// The auto filler (Molty settings): when you finish speaking and no agent has answered after this
// long, Molty says a short filler itself ("Hmm…", "Let me think.") so the silence never drags on
// while a slow agent thinks. The device keeps the delay in NVS (POST /api/creature/state
// {filler_ms}: 0 = off, or 1500..5000; GET reports it), like the acknowledgement, and reports it
// with every listen; the relay plays the filler. Values: services/relay/src/core/phrases.ts.

export const FILLER_DEFAULT_MS = 2500;
export const FILLER_OFF = 0;

/** The chips, in order: off, then the delays. */
export const FILLER_CHOICES: ReadonlyArray<{ id: string; ms: number; label: string }> = [
  { id: 'off', ms: FILLER_OFF, label: 'off' },
  { id: '1.5s', ms: 1500, label: 'after 1.5 s' },
  { id: '2.5s', ms: 2500, label: 'after 2.5 s (default)' },
  { id: '3.5s', ms: 3500, label: 'after 3.5 s' },
  { id: '5s', ms: 5000, label: 'after 5 s' },
];

/** A value the device takes: 0 or 1500..5000, an integer. */
export const fillerValid = (ms: unknown): ms is number =>
  typeof ms === 'number' && Number.isInteger(ms) && (ms === FILLER_OFF || (ms >= 1500 && ms <= 5000));

/** Which chip is on for the device's filler_ms: a chip id, or 'custom' (a valid value no chip
 *  has); an invalid / missing value reads as the default. */
export function fillerChoice(ms: unknown): string {
  if (!fillerValid(ms)) return '2.5s';
  return FILLER_CHOICES.find((c) => c.ms === ms)?.id ?? 'custom';
}

/** Whether this firmware has the setting: it reports filler_ms in its state. */
export const fillerSupported = (state: { filler_ms?: unknown } | null | undefined): boolean => typeof state?.filler_ms === 'number';
