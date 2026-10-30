// Draws level / pitch / haptic / beats / pulses over time onto a canvas.
// Used live by test.html and for the PNG plots of offline renders.

export function drawPlot(cv, { audio = [], haptic = [], pulse = [] }, { t0, t1, title = '', dpr = 1 } = {}) {
  const g = cv.getContext('2d'), W = cv.width, H = cv.height;
  if (t0 == null) t0 = audio.length ? audio[0].t : 0;
  if (t1 == null) t1 = audio.length ? audio[audio.length - 1].t : 1;
  const padL = 34 * dpr, padR = 8 * dpr, padT = 22 * dpr, padB = 20 * dpr;
  const x = (t) => padL + ((t - t0) / Math.max(1e-6, t1 - t0)) * (W - padL - padR);
  const y = (v) => padT + (1 - v) * (H - padT - padB);
  g.fillStyle = '#0f1115'; g.fillRect(0, 0, W, H);
  g.font = `${11 * dpr}px ui-monospace, Menlo, monospace`;
  g.strokeStyle = '#2a2f3a'; g.lineWidth = 1; g.fillStyle = '#7d8696';
  for (const v of [0, 0.5, 1]) { g.beginPath(); g.moveTo(padL, y(v)); g.lineTo(W - padR, y(v)); g.stroke(); g.fillText(v.toFixed(1), 4 * dpr, y(v) + 4 * dpr); }
  const span = t1 - t0, stepS = span > 8 ? 2 : span > 3 ? 1 : 0.5;
  for (let s = Math.ceil(t0 / stepS) * stepS; s <= t1; s += stepS) g.fillText(`${s.toFixed(1)}s`, x(s) - 10 * dpr, H - 5 * dpr);
  const vis = (arr) => arr.filter((p) => p.t >= t0 && p.t <= t1);
  // haptic: filled area
  const hv = vis(haptic);
  if (hv.length) {
    g.beginPath(); g.moveTo(x(hv[0].t), y(0));
    for (const p of hv) g.lineTo(x(p.t), y(p.level));
    g.lineTo(x(hv[hv.length - 1].t), y(0)); g.closePath();
    g.fillStyle = 'rgba(255,170,100,0.28)'; g.fill();
    g.beginPath(); hv.forEach((p, i) => (i ? g.lineTo : g.moveTo).call(g, x(p.t), y(p.level)));
    g.strokeStyle = '#ffaa64'; g.lineWidth = 1.5 * dpr; g.stroke();
  }
  const av = vis(audio);
  const line = (key, color, w) => {
    g.beginPath(); av.forEach((p, i) => (i ? g.lineTo : g.moveTo).call(g, x(p.t), y(p[key])));
    g.strokeStyle = color; g.lineWidth = w * dpr; g.stroke();
  };
  line('pitch', 'rgba(150,200,255,0.55)', 1);
  line('level', '#7fe3c4', 1.8);
  g.fillStyle = 'rgba(127,227,196,0.9)';
  for (const p of av) if (p.beat) g.fillRect(x(p.t) - 0.5 * dpr, y(0) - 6 * dpr, 1 * dpr, 6 * dpr);
  g.fillStyle = '#e8e1ff';
  for (const p of vis(pulse)) { g.beginPath(); g.arc(x(p.t), y(Math.min(1, p.strength)), 2.5 * dpr, 0, 7); g.fill(); }
  g.fillStyle = '#c9d1dc'; g.fillText(title, padL, 14 * dpr);
  const leg = [['level', '#7fe3c4'], ['pitch', 'rgba(150,200,255,0.8)'], ['haptic', '#ffaa64'], ['pulse', '#e8e1ff']];
  let lx = W - padR - 250 * dpr;
  for (const [n, c] of leg) { g.fillStyle = c; g.fillRect(lx, 7 * dpr, 10 * dpr, 3 * dpr); g.fillStyle = '#9aa3b2'; g.fillText(n, lx + 13 * dpr, 13 * dpr); lx += 62 * dpr; }
}
