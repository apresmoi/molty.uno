// The listen window (Molty settings): how long a listen may run when the agent gives no
// window of its own. Every listen ends early anyway (end of speech / the voice box's end of
// turn): this is a ceiling. The device keeps it in NVS (POST /api/creature/state
// {listen_ms}: 0 = unlimited, or 5000..30000; GET reports it), like ring_color. Unlimited
// still has a 5-minute safety ceiling on the device. Values: relay/src/core/commands.ts.

export const LISTEN_DEFAULT_MS = 30000;
export const LISTEN_UNLIMITED = 0;

/** The chips, in order: 5..30 s, then unlimited. */
export const LISTEN_CHOICES: ReadonlyArray<{ id: string; ms: number; label: string }> = [
  { id: '5s', ms: 5000, label: '5 s' },
  { id: '10s', ms: 10000, label: '10 s' },
  { id: '15s', ms: 15000, label: '15 s' },
  { id: '20s', ms: 20000, label: '20 s' },
  { id: '30s', ms: 30000, label: '30 s (default)' },
  { id: 'unlimited', ms: LISTEN_UNLIMITED, label: 'unlimited' },
];

/** A value the device takes: 0 or 5000..30000, an integer. */
export const listenValid = (ms: unknown): ms is number =>
  typeof ms === 'number' && Number.isInteger(ms) && (ms === LISTEN_UNLIMITED || (ms >= 5000 && ms <= 30000));

/** Which chip is on for the device's listen_ms: a chip id, or 'custom' (a valid value an API
 *  call set that no chip has); an invalid / missing value reads as the default. */
export function listenChoice(ms: unknown): string {
  if (!listenValid(ms)) return '30s';
  return LISTEN_CHOICES.find((c) => c.ms === ms)?.id ?? 'custom';
}

/** Whether this firmware has the setting: it reports listen_ms in its state. */
export const listenSupported = (state: { listen_ms?: unknown } | null | undefined): boolean => typeof state?.listen_ms === 'number';
