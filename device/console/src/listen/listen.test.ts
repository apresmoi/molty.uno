import { describe, expect, it } from 'vitest';
import { LISTEN_CHOICES, listenChoice, listenSupported, listenValid } from './listen';

describe('listen window setting', () => {
  it('offers 5..30 s, then unlimited (0); 30 s is the default', () => {
    expect(LISTEN_CHOICES.map((c) => c.ms)).toEqual([5000, 10000, 15000, 20000, 30000, 0]);
    expect(LISTEN_CHOICES.find((c) => c.ms === 30000)!.label).toMatch(/default/);
    expect(LISTEN_CHOICES[LISTEN_CHOICES.length - 1].label).toBe('unlimited');
    expect(LISTEN_CHOICES.every((c) => listenValid(c.ms))).toBe(true);
  });
  it('takes only what the device takes: 0 or 5000..30000', () => {
    for (const v of [0, 5000, 12000, 30000]) expect(listenValid(v), String(v)).toBe(true);
    for (const v of [4999, 30001, -1, 1.5, '30000', null, undefined]) expect(listenValid(v), String(v)).toBe(false);
  });
  it('maps what the device reports to the chip that is on', () => {
    expect(listenChoice(30000)).toBe('30s');
    expect(listenChoice(0)).toBe('unlimited');
    expect(listenChoice(5000)).toBe('5s');
    expect(listenChoice(12000)).toBe('custom');
    expect(listenChoice(undefined)).toBe('30s');
    expect(listenChoice(99)).toBe('30s');
  });
  it('is shown only when the firmware reports listen_ms', () => {
    expect(listenSupported({ listen_ms: 0 })).toBe(true);
    expect(listenSupported({ listen_ms: 30000 })).toBe(true);
    expect(listenSupported({})).toBe(false);
    expect(listenSupported(null)).toBe(false);
  });
});
