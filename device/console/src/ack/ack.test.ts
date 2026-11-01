import { describe, expect, it } from 'vitest';
import { ACK_CHOICES, ackChoice, ackSupported, ackValid } from './ack';

describe('turn acknowledgement setting', () => {
  it('offers chirp + thinking (the default), thinking only, off', () => {
    expect(ACK_CHOICES.map((c) => c.id)).toEqual(['chirp', 'thinking', 'off']);
    expect(ACK_CHOICES.every((c) => ackValid(c.id))).toBe(true);
  });
  it('takes only what the device takes', () => {
    for (const v of ['silent', 'Chirp', '', true, false, 2, null, undefined]) expect(ackValid(v), String(v)).toBe(false);
  });
  it('maps what the device reports to the chip that is on', () => {
    expect(ackChoice('thinking')).toBe('thinking');
    expect(ackChoice('off')).toBe('off');
    expect(ackChoice(undefined)).toBe('chirp');
    expect(ackChoice('loud')).toBe('chirp');
  });
  it('is shown only when the firmware reports ack', () => {
    expect(ackSupported({ ack: 'off' })).toBe(true);
    expect(ackSupported({})).toBe(false);
    expect(ackSupported(null)).toBe(false);
  });
});
