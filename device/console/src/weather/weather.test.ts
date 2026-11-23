import { describe, expect, it } from 'vitest';
import { WEATHER_KINDS, WEATHER_ON_CHOICES, WEATHER_TRY_S, weatherKind, weatherLine, weatherOnChoice, weatherSupported, weatherTry } from './weather';

describe('weather', () => {
  it('offers the four kinds, clear first', () => {
    expect([...WEATHER_KINDS]).toEqual(['clear', 'drizzle', 'rain', 'downpour']);
  });
  it('tries a kind for a few minutes, and clear ends it', () => {
    expect(weatherTry('rain')).toEqual({ weather: { kind: 'rain', s: WEATHER_TRY_S } });
    expect(weatherTry('clear')).toEqual({ weather: 'clear' });
    expect(WEATHER_TRY_S).toBeGreaterThanOrEqual(10);
    expect(WEATHER_TRY_S).toBeLessThanOrEqual(21600);
  });
  it('reads what the device says', () => {
    expect(weatherKind({ kind: 'downpour' })).toBe('downpour');
    expect(weatherKind({ kind: 'snow' })).toBe('clear');
    expect(weatherKind(null)).toBe('clear');
    expect(weatherLine({ kind: 'rain', s_left: 3400, wet: 1 })).toBe('rain, 57 min left');
    expect(weatherLine({ kind: 'drizzle', s_left: 42 })).toBe('drizzle, 42 s left');
    expect(weatherLine({ kind: 'clear', s_left: 0, wet: 0.4 })).toBe('clear, the floor drying');
    expect(weatherLine({ kind: 'clear', s_left: 0, wet: 0 })).toBe('clear');
  });
  it('has the setting on by default, shown only when the firmware reports it', () => {
    expect(WEATHER_ON_CHOICES.map((c) => c.on)).toEqual([true, false]);
    expect(weatherOnChoice(false)).toBe(false);
    for (const v of [undefined, null, 'off', 0]) expect(weatherOnChoice(v), String(v)).toBe(true);
    expect(weatherSupported({ weather_on: true, weather: { kind: 'clear' } })).toBe(true);
    expect(weatherSupported({ weather_on: true })).toBe(false);
    expect(weatherSupported({})).toBe(false);
    expect(weatherSupported(null)).toBe(false);
  });
});
