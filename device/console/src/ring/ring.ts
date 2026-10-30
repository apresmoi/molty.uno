// The listening ring's colour (Molty settings): the device keeps one default in NVS
// (POST /api/creature/state {ring_color: "#rrggbb" | "accent"}; GET reports it the same
// way). An agent's listen / talk may still pick its own colour for one listen.
// The names and values are the relay's palette (services/relay/src/core/commands.ts COLORS).

export const RING_COLORS = {
  green: '#3ccf96',
  red: '#ff6b6b',
  amber: '#ffa94d',
  blue: '#4dabf7',
  violet: '#b197fc',
  pink: '#f783ac',
  cyan: '#3bc9db',
  white: '#f1f3f5',
} as const;
export type RingName = keyof typeof RING_COLORS;

/** The chips, in order: the palette's own accent first (the default), then the colours. */
export const RING_CHOICES: ReadonlyArray<{ id: 'accent' | RingName; value: string }> = [
  { id: 'accent', value: 'accent' },
  ...(Object.entries(RING_COLORS) as [RingName, string][]).map(([id, value]) => ({ id, value })),
];

/** Which chip is on for the device's ring_color: 'accent' (unset or older firmware), a
 *  palette name, or 'custom' (a hex an agent or the API set that is not in the palette). */
export function ringChoice(ring: unknown): 'accent' | RingName | 'custom' {
  if (typeof ring !== 'string' || ring === '' || ring === 'accent') return 'accent';
  const hex = ring.toLowerCase();
  const hit = (Object.entries(RING_COLORS) as [RingName, string][]).find(([, v]) => v === hex);
  return hit ? hit[0] : /^#[0-9a-f]{6}$/.test(hex) ? 'custom' : 'accent';
}

/** Whether this firmware has the setting: it reports ring_color in its state. */
export const ringSupported = (state: { ring_color?: unknown } | null | undefined): boolean => typeof state?.ring_color === 'string';
