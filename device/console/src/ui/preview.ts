// Live screen preview: polls GET /api/framebuffer (~2 fps, never overlapping) into a
// round canvas. Each frame costs the device about half its frame rate while it sends, so
// pages start it on request (auto: only where the preview is the point, like setup). Optional onTap(x, y) gets device-normalized coords (-1..1, y down —
// the same convention as the fluid's touch input).
import type { DeviceApi } from '../api/device';
import { h } from './dom';

export interface Preview { el: HTMLElement; stop(): void }

export function preview(api: DeviceApi, opts: { size?: number; onTap?: (x: number, y: number) => void; periodMs?: number; auto?: boolean } = {}): Preview {
  const period = opts.periodMs ?? 500;
  const canvas = h('canvas', { width: 412, height: 412, class: 'screen', 'aria-label': 'live device screen' });
  const ring = h('div', { class: 'tapring' });
  let running = !!opts.auto;
  const status = h('div', { class: 'pv-status' }, running ? 'connecting…' : 'Live view is off: it slows Molty while it runs.');
  const toggle = h('button', { type: 'button', class: 'link', onclick: () => {
    running = !running;
    toggle.textContent = running ? 'Stop live view' : 'Show live view';
    clearTimeout(timer);
    if (running) { status.textContent = 'connecting…'; void loop(); }
    else status.textContent = 'Live view is off: it slows Molty while it runs.';
  } }, running ? 'Stop live view' : 'Show live view');
  const el = h('div', { class: 'preview' + (opts.onTap ? ' tappable' : ''), style: opts.size ? `--pv:${opts.size}px` : undefined },
    h('div', { class: 'screen-wrap' }, canvas, ring), status, opts.auto ? null : toggle);
  const ctx = canvas.getContext('2d')!;
  let img: ImageData | null = null;
  let alive = true, timer = 0, frames = 0, since = performance.now(), fails = 0;

  const draw = (w: number, hgt: number, rgb: Uint8Array) => {
    if (canvas.width !== w || canvas.height !== hgt) { canvas.width = w; canvas.height = hgt; img = null; }
    img ??= ctx.createImageData(w, hgt);
    const d = img.data;
    for (let i = 0, j = 0; i < rgb.length; i += 3, j += 4) { d[j] = rgb[i]; d[j + 1] = rgb[i + 1]; d[j + 2] = rgb[i + 2]; d[j + 3] = 255; }
    ctx.putImageData(img, 0, 0);
  };

  const loop = async () => {
    if (!alive || !running) return;
    if (document.hidden) { timer = window.setTimeout(loop, 1000); return; }
    const t0 = performance.now();
    try {
      const f = await api.framebuffer();
      if (!alive || !running) return;
      draw(f.width, f.height, f.rgb);
      frames++; fails = 0;
      const dt = (performance.now() - since) / 1000;
      if (dt > 2) { status.textContent = `live ⠶ ${(frames / dt).toFixed(1)} fps preview`; frames = 0; since = performance.now(); }
      else if (status.textContent?.startsWith('connecting')) status.textContent = 'live';
      el.classList.remove('stale');
    } catch (e) {
      if ((e as Error).name !== 'AbortError' && ++fails >= 2) { el.classList.add('stale'); status.textContent = 'preview unavailable'; }
    }
    timer = window.setTimeout(loop, Math.max(60, period - (performance.now() - t0)));
  };
  if (running) void loop();

  if (opts.onTap) {
    canvas.addEventListener('click', (ev) => {
      const r = canvas.getBoundingClientRect();
      const x = ((ev.clientX - r.left) / r.width) * 2 - 1, y = ((ev.clientY - r.top) / r.height) * 2 - 1;
      if (x * x + y * y > 1) return; // outside the round glass
      ring.style.left = `${ev.clientX - r.left}px`; ring.style.top = `${ev.clientY - r.top}px`;
      ring.classList.remove('go'); void ring.offsetWidth; ring.classList.add('go');
      opts.onTap!(+x.toFixed(3), +y.toFixed(3));
    });
  }
  return { el, stop() { alive = false; clearTimeout(timer); } };
}
