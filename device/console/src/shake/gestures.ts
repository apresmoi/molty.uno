// Gestures (Molty settings): what tap, double tap and the two buttons do. The device keeps them in
// NVS (POST /api/creature/state {gestures: {tap: "talk"}}; GET reports them), like shake_talk.
// A recording and a say waiting for its first audio always take the tap; a question and a pairing
// take everything. Values: device/firmware/main/sense/gesture.h.

export const GESTURE_INPUTS: ReadonlyArray<{ id: string; label: string; fallback: string }> = [
  { id: 'tap', label: 'tap', fallback: 'event' },
  { id: 'double_tap', label: 'double tap', fallback: 'talk' },
  { id: 'button_accept', label: 'BOOT button', fallback: 'event' },
  { id: 'button_reject', label: 'PWR button', fallback: 'event' },
];

export const GESTURE_ACTIONS: ReadonlyArray<{ id: string; label: string }> = [
  { id: 'event', label: 'tell agent' },
  { id: 'talk', label: 'talk' },
  { id: 'stop', label: 'stop' },
  { id: 'none', label: 'nothing' },
];

/** Which chip is on for what the device reports; a missing / unknown value reads as the input's default. */
export const gestureChoice = (bound: Partial<Record<string, string>> | null | undefined, input: { id: string; fallback: string }): string => {
  const v = bound?.[input.id];
  return GESTURE_ACTIONS.some((a) => a.id === v) ? (v as string) : input.fallback;
};

/** Whether this firmware has the setting: it reports gestures in its state. */
export const gesturesSupported = (state: { gestures?: unknown } | null | undefined): boolean =>
  typeof state?.gestures === 'object' && state.gestures !== null;
