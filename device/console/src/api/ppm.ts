import type { Frame } from './types';

/** Parse a binary P6 PPM (maxval ≤ 255) as sent by GET /api/framebuffer. */
export function parsePPM(buf: Uint8Array): Frame {
  let pos = 0;
  const token = (): string => {
    for (;;) { // skip whitespace and # comments
      const c = buf[pos];
      if (c === 0x23) { while (pos < buf.length && buf[pos] !== 0x0a) pos++; }
      else if (c === 0x20 || c === 0x0a || c === 0x0d || c === 0x09) pos++;
      else break;
    }
    const start = pos;
    while (pos < buf.length && buf[pos] > 0x20) pos++;
    return String.fromCharCode(...buf.subarray(start, pos));
  };
  if (token() !== 'P6') throw new Error('not a P6 PPM');
  const width = +token(), height = +token(), max = +token();
  if (!(width > 0 && height > 0) || max !== 255) throw new Error('unsupported PPM header');
  pos++; // single whitespace after maxval
  const n = width * height * 3;
  if (buf.length - pos < n) throw new Error('truncated PPM');
  return { width, height, rgb: buf.subarray(pos, pos + n) };
}
