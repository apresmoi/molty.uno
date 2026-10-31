// The turn acknowledgement (Molty settings): what Molty does by itself the moment you finish
// speaking, before any agent has answered — a soft chirp and its thinking face, its thinking
// face only, or nothing. The device keeps it in NVS (POST /api/creature/state {ack:
// "chirp" | "thinking" | "off"}; GET reports it), like ring_color. Agents can only turn it
// down for a listen (ack: false | "silent"). Values: device/firmware/main/link/link_protocol.h.

export const ACK_DEFAULT = 'chirp';

/** The chips, in order. */
export const ACK_CHOICES: ReadonlyArray<{ id: string; label: string }> = [
  { id: 'chirp', label: 'Chirp and thinking face' },
  { id: 'thinking', label: 'Thinking face' },
  { id: 'off', label: 'Nothing' },
];

/** A value the device takes. */
export const ackValid = (v: unknown): v is string => typeof v === 'string' && ACK_CHOICES.some((c) => c.id === v);

/** Which chip is on for the device's ack; an invalid / missing value reads as the default. */
export const ackChoice = (v: unknown): string => (ackValid(v) ? v : ACK_DEFAULT);

/** Whether this firmware has the setting: it reports ack in its state. */
export const ackSupported = (state: { ack?: unknown } | null | undefined): boolean => typeof state?.ack === 'string';
