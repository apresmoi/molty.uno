import { describe, expect, it } from 'vitest';
import { NAP_SETTINGS, NAP_TIMES, napChoice, napLabel, napSupported, napValid } from './nap';

describe('screen off settings', () => {
  it('offers the times the device takes, never last, with the defaults among them', () => {
    expect(NAP_TIMES.every(napValid)).toBe(true);
    expect(NAP_TIMES[NAP_TIMES.length - 1]).toBe(0);
    expect(NAP_SETTINGS.map((s) => [s.key, s.def])).toEqual([['screen_off_battery_s', 60], ['screen_off_usb_s', 600]]);
    for (const s of NAP_SETTINGS) expect(NAP_TIMES).toContain(s.def);
  });
  it('takes only what the device takes', () => {
    for (const v of [5, 9, 3601, -1, 1.5, '60', true, null, undefined]) expect(napValid(v), String(v)).toBe(false);
    for (const v of [0, 10, 60, 3600]) expect(napValid(v)).toBe(true);
  });
  it('labels the chips in plain words, the default marked', () => {
    expect(napLabel(0, 60)).toBe('never');
    expect(napLabel(30, 60)).toBe('30 s');
    expect(napLabel(60, 60)).toBe('1 min (default)');
    expect(napLabel(600, 600)).toBe('10 min (default)');
  });
  it('maps what the device reports to the chip that is on', () => {
    expect(napChoice(0, 60)).toBe(0);
    expect(napChoice(300, 60)).toBe(300);
    expect(napChoice(undefined, 600)).toBe(600);
    expect(napChoice(7, 60)).toBe(60);
  });
  it('is shown only when the firmware reports both', () => {
    expect(napSupported({ screen_off_battery_s: 60, screen_off_usb_s: 0 })).toBe(true);
    expect(napSupported({ screen_off_battery_s: 60 })).toBe(false);
    expect(napSupported(null)).toBe(false);
  });
});
