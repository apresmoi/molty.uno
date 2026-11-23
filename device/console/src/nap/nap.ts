// Screen off (Molty settings): how long after Molty falls asleep its screen goes dark by itself, one time on battery
// and one on USB power, or never. A touch, a button, a shake, picking Molty up or the agent showing something lights
// it again. The device keeps both (POST /api/creature/state {screen_off_battery_s, screen_off_usb_s}: 0 = never or
// 10..3600 s; GET reports them), like idle life. Values: device/firmware/main/fluid/screen_nap.h.

export const NAP_BATTERY_DEFAULT = 60;
export const NAP_USB_DEFAULT = 600;

/** The two settings, in the page's order. */
export const NAP_SETTINGS: ReadonlyArray<{ key: 'screen_off_battery_s' | 'screen_off_usb_s'; label: string; def: number }> = [
  { key: 'screen_off_battery_s', label: 'screen off on battery', def: NAP_BATTERY_DEFAULT },
  { key: 'screen_off_usb_s', label: 'screen off on USB', def: NAP_USB_DEFAULT },
];

/** The chips, in order (seconds; 0 = never). */
export const NAP_TIMES: ReadonlyArray<number> = [30, 60, 120, 300, 600, 1800, 0];

/** A value the device takes. */
export const napValid = (v: unknown): v is number => typeof v === 'number' && Number.isInteger(v) && (v === 0 || (v >= 10 && v <= 3600));

/** A chip's words: "never", "30 s", "1 min", with "(default)" on the setting's default. */
export const napLabel = (s: number, def: number): string =>
  (s === 0 ? 'never' : s < 60 ? `${s} s` : `${s / 60} min`) + (s === def ? ' (default)' : '');

/** Which chip is on for a value the device reports; an invalid / missing value reads as the default. */
export const napChoice = (v: unknown, def: number): number => (napValid(v) ? v : def);

/** Whether this firmware has the settings: it reports both in its state. */
export const napSupported = (state: { screen_off_battery_s?: unknown; screen_off_usb_s?: unknown } | null | undefined): boolean =>
  typeof state?.screen_off_battery_s === 'number' && typeof state?.screen_off_usb_s === 'number';
