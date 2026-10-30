import { describe, expect, it } from 'vitest';
import { RING_CHOICES, RING_COLORS, ringChoice, ringSupported } from './ring';

describe('listening ring colour setting', () => {
  it('offers the accent (default) first, then the relay palette in its order', () => {
    expect(RING_CHOICES.map((c) => c.id)).toEqual(['accent', 'green', 'red', 'amber', 'blue', 'violet', 'pink', 'cyan', 'white']);
    expect(RING_CHOICES[0].value).toBe('accent');
    // the relay's values (relay/src/core/commands.ts COLORS), so a chip and an agent's name agree
    expect(RING_COLORS).toEqual({ green: '#3ccf96', red: '#ff6b6b', amber: '#ffa94d', blue: '#4dabf7', violet: '#b197fc', pink: '#f783ac', cyan: '#3bc9db', white: '#f1f3f5' });
  });
  it('maps what the device reports to the chip that is on', () => {
    expect(ringChoice('accent')).toBe('accent');
    expect(ringChoice(undefined)).toBe('accent');
    expect(ringChoice('#b197fc')).toBe('violet');
    expect(ringChoice('#B197FC')).toBe('violet');
    expect(ringChoice('#123456')).toBe('custom');
    expect(ringChoice('nonsense')).toBe('accent');
  });
  it('is shown only when the firmware reports ring_color', () => {
    expect(ringSupported({ ring_color: 'accent' })).toBe(true);
    expect(ringSupported({})).toBe(false);
    expect(ringSupported(null)).toBe(false);
  });
});
