import { describe, expect, it } from 'vitest';
import { SKY_CHOICES, skyChoice, skySupported } from './sky';

describe('sky setting', () => {
  it('offers on (the default) and off', () => {
    expect(SKY_CHOICES.map((c) => c.on)).toEqual([true, false]);
    expect(SKY_CHOICES[0].label).toMatch(/default/);
  });
  it('maps what the device reports to the chip that is on', () => {
    expect(skyChoice(false)).toBe(false);
    expect(skyChoice(true)).toBe(true);
    for (const v of [undefined, null, 'off', 0, 1]) expect(skyChoice(v), String(v)).toBe(true);
  });
  it('is shown only when the firmware reports sky as a boolean', () => {
    expect(skySupported({ sky: false })).toBe(true);
    expect(skySupported({ sky: 'on' })).toBe(false);
    expect(skySupported({})).toBe(false);
    expect(skySupported(null)).toBe(false);
  });
});
