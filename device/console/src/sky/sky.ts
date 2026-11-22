// The sky (Molty settings): today's moon, small, behind the creature, lit to its real phase, and now and then a
// falling star. Off: neither. The device keeps it (POST /api/creature/state {sky: true | false}; GET reports it), like
// shake to talk. On out of the box. The moon needs the date, which the device takes from network time once on Wi-Fi.
// Values: device/firmware/main/creature/sky.h.

export const SKY_DEFAULT = true;

/** The chips, in order. */
export const SKY_CHOICES: ReadonlyArray<{ on: boolean; label: string }> = [
  { on: true, label: 'on (default)' },
  { on: false, label: 'off' },
];

/** Which chip is on for what the device reports; a missing / invalid value reads as the default. */
export const skyChoice = (v: unknown): boolean => (typeof v === 'boolean' ? v : SKY_DEFAULT);

/** Whether this firmware has the setting: it reports sky in its state. */
export const skySupported = (state: { sky?: unknown } | null | undefined): boolean => typeof state?.sky === 'boolean';
