import { describe, expect, it } from 'vitest';
import { SHAKE_CHOICES, shakeChoice, shakeSupported } from './shake';

describe('shake to talk setting', () => {
  it('offers on (the default) and off', () => {
    expect(SHAKE_CHOICES.map((c) => c.on)).toEqual([true, false]);
    expect(SHAKE_CHOICES[0].label).toMatch(/default/);
  });
  it('maps what the device reports to the chip that is on', () => {
    expect(shakeChoice(false)).toBe(false);
    expect(shakeChoice(true)).toBe(true);
    for (const v of [undefined, null, 'off', 0, 1]) expect(shakeChoice(v), String(v)).toBe(true);
  });
  it('is shown only when the firmware reports shake_talk as a boolean', () => {
    expect(shakeSupported({ shake_talk: false })).toBe(true);
    expect(shakeSupported({ shake_talk: 'on' })).toBe(false);
    expect(shakeSupported({})).toBe(false);
    expect(shakeSupported(null)).toBe(false);
  });
});
