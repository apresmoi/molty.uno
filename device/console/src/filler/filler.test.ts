import { describe, expect, it } from 'vitest';
import { FILLER_CHOICES, fillerChoice, fillerSupported, fillerValid } from './filler';

describe('auto filler setting', () => {
  it('offers off, then 1.5..5 s; 2.5 s is the default', () => {
    expect(FILLER_CHOICES.map((c) => c.ms)).toEqual([0, 1500, 2500, 3500, 5000]);
    expect(FILLER_CHOICES.find((c) => c.ms === 2500)!.label).toMatch(/default/);
    expect(FILLER_CHOICES[0].label).toBe('off');
    expect(FILLER_CHOICES.every((c) => fillerValid(c.ms))).toBe(true);
  });
  it('takes only what the device takes: 0 or 1500..5000', () => {
    for (const v of [0, 1500, 2000, 5000]) expect(fillerValid(v), String(v)).toBe(true);
    for (const v of [1499, 5001, -1, 2500.5, '2500', null, undefined]) expect(fillerValid(v), String(v)).toBe(false);
  });
  it('maps what the device reports to the chip that is on', () => {
    expect(fillerChoice(2500)).toBe('2.5s');
    expect(fillerChoice(0)).toBe('off');
    expect(fillerChoice(5000)).toBe('5s');
    expect(fillerChoice(2000)).toBe('custom');
    expect(fillerChoice(undefined)).toBe('2.5s');
    expect(fillerChoice(99)).toBe('2.5s');
  });
  it('is shown only when the firmware reports filler_ms', () => {
    expect(fillerSupported({ filler_ms: 0 })).toBe(true);
    expect(fillerSupported({ filler_ms: 2500 })).toBe(true);
    expect(fillerSupported({})).toBe(false);
    expect(fillerSupported(null)).toBe(false);
  });
});
