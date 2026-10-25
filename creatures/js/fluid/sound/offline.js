// Offline rendering: run a scenario through the exact same engine + motor on an
// OfflineAudioContext, stepping the audio clock with suspend() so tick() and the fake
// fluid's 'frame' run at ~60 Hz of *audio* time. Uses a private bus so a render never
// leaks events into the live page. Returns the rendered buffer plus every event emitted.

import { createEngine } from './engine.js';
import { createMotor } from './motor.js';
import { FakeFluid, SCENARIOS } from './scenarios.js';

export function createBus() {
  const subs = new Map();
  return {
    on(type, fn) { if (!subs.has(type)) subs.set(type, new Set()); subs.get(type).add(fn); return () => subs.get(type).delete(fn); },
    emit(type, data) { const s = subs.get(type); if (s) for (const fn of s) fn(data); },
  };
}

export async function renderScenario(name, { sampleRate = 48000, preset = 'glass', layers = {}, channels = 2 } = {}) {
  const sc = typeof name === 'string' ? SCENARIOS[name] : name;
  if (!sc) throw new Error(`unknown scenario ${name}`);
  const len = Math.ceil(sc.seconds * sampleRate);
  const ctx = new OfflineAudioContext(channels, len, sampleRate);
  const bus = createBus();
  const log = { audio: [], pulse: [], haptic: [], word: [], bus: [] };
  bus.on('audio', (a) => log.audio.push({ t: a.t, level: a.level, pitch: a.pitch, beat: a.beat, layers: a.layers }));
  bus.on('pulse', (p) => log.pulse.push(p));
  bus.on('haptic', (h) => log.haptic.push(h));
  const engine = createEngine({ ctx, bus, preset, layers, reportLatency: false });
  const motor = createMotor({ bus });
  const fluid = new FakeFluid(bus);
  sc.setup?.(fluid);
  const acts = [...(sc.actions || [])].sort((a, b) => a[0] - b[0]);
  const hooks = [...(sc.hooks || [])].sort((a, b) => a[0] - b[0]);
  const quantum = 128, stepFrames = quantum * Math.round(sampleRate / 60 / quantum); // ≈16 ms
  const step = stepFrames / sampleRate;
  let tickMs = 0, ticks = 0;

  const onStep = (t) => {
    const w0 = performance.now();
    while (hooks.length && hooks[0][0] <= t) hooks.shift()[1](fluid);
    fluid.frame(t, step);
    while (acts.length && acts[0][0] <= t) {
      const [at, type, data] = acts.shift();
      log.bus.push({ t: ctx.currentTime, at, type, data });
      bus.emit(type, data);
    }
    engine.tick();
    tickMs += performance.now() - w0; ticks++;
  };
  let k = 1;
  const schedule = () => {
    const t = k * step; k++;
    if (t >= sc.seconds - step) return;
    ctx.suspend(t).then(() => { onStep(ctx.currentTime); schedule(); ctx.resume(); });
  };
  schedule();
  const w0 = performance.now();
  const buffer = await ctx.startRendering();
  const wallMs = performance.now() - w0;
  engine.dispose(); motor.dispose(); fluid.dispose();
  return { buffer, log, wallMs, tickMs: tickMs / Math.max(1, ticks), seconds: sc.seconds, pattern: motor.takePattern() };
}

export function encodeWav(buffer) {
  const ch = buffer.numberOfChannels, sr = buffer.sampleRate, n = buffer.length;
  const out = new DataView(new ArrayBuffer(44 + n * ch * 2));
  const str = (o, s) => { for (let i = 0; i < s.length; i++) out.setUint8(o + i, s.charCodeAt(i)); };
  str(0, 'RIFF'); out.setUint32(4, 36 + n * ch * 2, true); str(8, 'WAVE'); str(12, 'fmt ');
  out.setUint32(16, 16, true); out.setUint16(20, 1, true); out.setUint16(22, ch, true);
  out.setUint32(24, sr, true); out.setUint32(28, sr * ch * 2, true); out.setUint16(32, ch * 2, true);
  out.setUint16(34, 16, true); str(36, 'data'); out.setUint32(40, n * ch * 2, true);
  const data = Array.from({ length: ch }, (_, c) => buffer.getChannelData(c));
  let o = 44;
  for (let i = 0; i < n; i++) for (let c = 0; c < ch; c++) {
    const v = Math.max(-1, Math.min(1, data[c][i])); out.setInt16(o, v < 0 ? v * 0x8000 : v * 0x7fff, true); o += 2;
  }
  return new Blob([out.buffer], { type: 'audio/wav' });
}
