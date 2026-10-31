import { describe, expect, it } from 'vitest';
import { IDLE_CHOICES, idleChoice, idleSupported, idleValid } from './idle';

describe('idle life setting', () => {
  it('offers lively (the default), calm, off', () => {
    expect(IDLE_CHOICES.map((c) => c.id)).toEqual(['lively', 'calm', 'off']);
    expect(IDLE_CHOICES.every((c) => idleValid(c.id))).toBe(true);
  });
  it('takes only what the device takes', () => {
    for (const v of ['Lively', 'quiet', '', true, false, 2, null, undefined]) expect(idleValid(v), String(v)).toBe(false);
  });
  it('maps what the device reports to the chip that is on', () => {
    expect(idleChoice('calm')).toBe('calm');
    expect(idleChoice('off')).toBe('off');
    expect(idleChoice(undefined)).toBe('lively');
    expect(idleChoice('wild')).toBe('lively');
  });
  it('is shown only when the firmware reports idle_life', () => {
    expect(idleSupported({ idle_life: 'off' })).toBe(true);
    expect(idleSupported({})).toBe(false);
    expect(idleSupported(null)).toBe(false);
  });
});
