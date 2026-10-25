// Braille field: a slow ripple of U+2800 dots, the console's echo of the device fluid.
// Each braille cell is 2×4 dots; we sample a scalar field per dot and threshold it.
const DOT = [[0x01, 0x08], [0x02, 0x10], [0x04, 0x20], [0x40, 0x80]];

export function brailleFrame(cols: number, rows: number, t: number, density = 0.5): string {
  let out = '';
  for (let r = 0; r < rows; r++) {
    for (let c = 0; c < cols; c++) {
      let bits = 0;
      for (let dy = 0; dy < 4; dy++) for (let dx = 0; dx < 2; dx++) {
        const x = c * 2 + dx, y = r * 4 + dy;
        const v = Math.sin(x * 0.21 + t * 0.9) + Math.sin(y * 0.33 - t * 0.6) + Math.sin((x + y) * 0.13 + t * 0.4);
        const n = ((x * 73856093) ^ (y * 19349663)) & 1023;
        if (v / 3 + (n / 1023 - 0.5) * 0.35 > 1 - density * 2) bits |= DOT[dy][dx];
      }
      out += String.fromCharCode(0x2800 + bits);
    }
    if (r < rows - 1) out += '\n';
  }
  return out;
}

/** Animate a <pre> at ~12 fps; stops when the element leaves the DOM or motion is reduced. */
export function animateBraille(el: HTMLElement, cols: number, rows: number, density = 0.5) {
  const still = matchMedia('(prefers-reduced-motion: reduce)').matches;
  let last = 0, raf = 0;
  const tick = (ms: number) => {
    if (!el.isConnected) return;
    if (ms - last > 83) { last = ms; el.textContent = brailleFrame(cols, rows, ms / 1000, density); }
    if (!still) raf = requestAnimationFrame(tick);
  };
  el.textContent = brailleFrame(cols, rows, 0, density);
  if (!still) raf = requestAnimationFrame(tick);
  return () => cancelAnimationFrame(raf);
}
