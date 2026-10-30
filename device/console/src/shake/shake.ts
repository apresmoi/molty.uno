// Shake to talk (Molty settings): whether a shake makes Molty start listening by itself, so
// the human can just shake and speak — the agents get their words as one `utterance`. Off:
// a shake is only an event (a nudge). The device keeps it in NVS (POST /api/creature/state
// {shake_talk: true | false}; GET reports it), like ring_color. On out of the box.
// Values: device/firmware/main/fluid/mole_fluid.h; the protocol: docs/AGENT-LINK.md "Shake to talk".

export const SHAKE_TALK_DEFAULT = true;

/** The chips, in order. */
export const SHAKE_CHOICES: ReadonlyArray<{ on: boolean; label: string }> = [
  { on: true, label: 'on (default)' },
  { on: false, label: 'off' },
];

/** Which chip is on for what the device reports; a missing / invalid value reads as the default. */
export const shakeChoice = (v: unknown): boolean => (typeof v === 'boolean' ? v : SHAKE_TALK_DEFAULT);

/** Whether this firmware has the setting: it reports shake_talk in its state. */
export const shakeSupported = (state: { shake_talk?: unknown } | null | undefined): boolean => typeof state?.shake_talk === 'boolean';
