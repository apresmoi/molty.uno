// The fluid's voice. Builds every sound layer on any BaseAudioContext (realtime or
// OfflineAudioContext) and reports what it actually plays back onto the bus.
//
// Clock rule: every sync-critical onset (think beats, babble syllables, chimes, chirps)
// is scheduled on ctx.currentTime with a small lookahead and pushed into an event queue
// stamped with its audio-clock time. tick() (called ~60 Hz by a driver: setInterval in
// the browser, suspend()-stepping offline) releases queue events once the audio clock
// has passed them (minus output latency, i.e. when they are *heard*) and emits:
//   'audio' { level, pitch, beat, t, layers }   every tick (t = audio-clock seconds)
//   'pulse' { strength, x, y, t, source }       on onsets that should ripple the fluid
// So the visuals follow the audio, never the other way round.
//
// Consumed: 'frame' (breath, energy, mood, state), 'state', 'settle', 'ripple', 'word', 'mood'.

import { PRESETS, VOWELS, PENTA, MOODS } from './presets.js';

export const LAYERS = ['breath', 'think', 'speak', 'transition', 'touch', 'mood', 'agitation'];
export const LAYER_LABELS = {
  breath: 'breathing', think: 'thinking', speak: 'speaking', transition: 'transitions',
  touch: 'touch drops', mood: 'moods', agitation: 'mic rumble',
};

const clamp = (v, a = 0, b = 1) => (v < a ? a : v > b ? b : v);
const semi = (n) => Math.pow(2, n / 12);
const PITCH_LO = 70, PITCH_HI = 1400;
const pitchNorm = (hz) => clamp(Math.log2(hz / PITCH_LO) / Math.log2(PITCH_HI / PITCH_LO));

function mulberry32(a) {
  return function () {
    a |= 0; a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
function hashStr(s) {
  let h = 2166136261;
  for (let i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 16777619); }
  return h >>> 0;
}

// Seamless looping noise (white/pink/brown), normalised to ~0.2 RMS.
function noiseBuffer(ctx, kind, seconds = 4) {
  const sr = ctx.sampleRate, fade = Math.floor(sr * 0.05);
  const n = Math.floor(sr * seconds) + fade;
  const raw = new Float32Array(n);
  const rnd = mulberry32(kind === 'pink' ? 7 : kind === 'brown' ? 11 : 3);
  let b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, last = 0;
  for (let i = 0; i < n; i++) {
    const w = rnd() * 2 - 1;
    if (kind === 'pink') { // Paul Kellet
      b0 = 0.99886 * b0 + w * 0.0555179; b1 = 0.99332 * b1 + w * 0.0750759;
      b2 = 0.969 * b2 + w * 0.153852; b3 = 0.8665 * b3 + w * 0.3104856;
      b4 = 0.55 * b4 + w * 0.5329522; b5 = -0.7616 * b5 - w * 0.016898;
      raw[i] = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362; b6 = w * 0.115926;
    } else if (kind === 'brown') {
      last = (last + 0.02 * w) * 0.998; raw[i] = last;
    } else raw[i] = w;
  }
  const len = n - fade, buf = ctx.createBuffer(1, len, sr), d = buf.getChannelData(0);
  let sum = 0;
  for (let i = 0; i < len; i++) {
    let v = raw[i];
    if (i < fade) { const k = i / fade; v = v * k + raw[len + i] * (1 - k); }
    d[i] = v; sum += v * v;
  }
  const g = 0.2 / Math.sqrt(sum / len || 1);
  for (let i = 0; i < len; i++) d[i] *= g;
  return buf;
}

function makeWave(ctx, harmonics) {
  const real = new Float32Array(harmonics.length + 1), imag = new Float32Array(harmonics.length + 1);
  harmonics.forEach((a, i) => { imag[i + 1] = a; });
  return ctx.createPeriodicWave(real, imag);
}

export function syllables(word) {
  const m = String(word).toLowerCase().match(/[aeiouyáéíóúü]+/g);
  const vowels = (m || ['a']).slice(0, 5).map((g) => {
    const c = g.normalize('NFD').replace(/[̀-ͯ]/g, '')[0];
    return VOWELS[c] ? c : 'a';
  });
  return vowels; // one entry per syllable, its dominant vowel
}

export function createEngine({
  ctx, bus, preset = 'glass', destination = ctx.destination, volume = 0.8,
  layers = {}, seed = 1, reportLatency = true,
}) {
  const rnd = mulberry32(seed);
  const unsubs = [];
  const queue = []; // {t, beat?, pulse?, pitch?, until?, source, layer}
  let P = PRESETS[preset] || PRESETS.glass;
  let wave = makeWave(ctx, P.harmonics);
  let mood = 'neutral', M = MOODS.neutral;
  let disposed = false;

  const pink = noiseBuffer(ctx, 'pink'), brown = noiseBuffer(ctx, 'brown'), white = noiseBuffer(ctx, 'white', 2);

  // ---- graph -----------------------------------------------------------------
  // layer → layerGain(on/off) → [analyser] → duck → report(analyser, pre-volume) + master
  // agitation bypasses duck/report (it is the mic; reporting it would feed back).
  const sink = ctx.createGain(); sink.gain.value = 0; sink.connect(destination);
  const duck = ctx.createGain();
  const masterVol = ctx.createGain(); masterVol.gain.value = volume;
  const comp = ctx.createDynamicsCompressor();
  comp.threshold.value = -14; comp.knee.value = 12; comp.ratio.value = 4;
  comp.attack.value = 0.004; comp.release.value = 0.18;
  duck.connect(masterVol); masterVol.connect(comp); comp.connect(destination);
  const report = ctx.createAnalyser(); report.fftSize = 1024; duck.connect(report); report.connect(sink);

  const L = {};
  for (const name of LAYERS) {
    const g = ctx.createGain(); g.gain.value = layers[name] === false ? 0 : 1;
    const an = ctx.createAnalyser(); an.fftSize = 1024;
    g.connect(an); an.connect(sink);
    g.connect(name === 'agitation' ? masterVol : duck);
    L[name] = { gain: g, an, on: layers[name] !== false, env: 0 };
  }
  const tdBuf = new Float32Array(1024);

  const now = () => ctx.currentTime;
  const outLat = () => (reportLatency ? (ctx.outputLatency || ctx.baseLatency || 0) : 0);
  function push(ev) {
    queue.push(ev);
    if (queue.length > 1 && queue[queue.length - 2].t > ev.t) queue.sort((a, b) => a.t - b.t);
  }
  function startStop(src, t0, t1) { src.start(t0); src.stop(t1); src.onended = () => { try { src.disconnect(); } catch {} }; }

  // ---- breathing: pink noise swell following frame.breath ---------------------
  const br = {
    src: ctx.createBufferSource(), bp: ctx.createBiquadFilter(), lp: ctx.createBiquadFilter(), amp: ctx.createGain(),
    phaseMode: true, smoothDown: 0, prevB: null, prevA: 0, prevDa: 0, flowMax: 0.4, lastT: null,
  };
  br.src.buffer = pink; br.src.loop = true;
  br.bp.type = 'bandpass'; br.bp.Q.value = 0.9; br.bp.frequency.value = 800;
  br.lp.type = 'lowpass'; br.lp.frequency.value = 3200;
  br.amp.gain.value = 0;
  br.src.connect(br.bp); br.bp.connect(br.lp); br.lp.connect(br.amp); br.amp.connect(L.breath.gain);
  br.src.start();

  function onBreath(b, t) {
    // Contract says breath is a 0..1 *phase*. If it ever falls smoothly for many frames
    // it is really a fullness amount; adapt instead of guessing wrong forever.
    if (br.prevB != null) {
      const d = br.prevB - b;
      br.smoothDown = d > 0 && d < 0.2 ? br.smoothDown + 1 : 0;
      if (br.smoothDown > 10) br.phaseMode = false;
    }
    br.prevB = b;
    const a = br.phaseMode ? 0.5 - 0.5 * Math.cos(2 * Math.PI * b) : clamp(b);
    const dt = br.lastT == null ? 1 / 60 : Math.max(1e-3, t - br.lastT); br.lastT = t;
    const da = (a - br.prevA) / dt;
    br.flowMax = Math.max(0.15, br.flowMax * 0.999, Math.abs(da));
    const flow = clamp(Math.abs(da) / br.flowMax);
    const sw = (0.35 * a + 0.65 * flow) * M.duck;
    br.amp.gain.setTargetAtTime(0.55 * sw, t, 0.06);
    const f = (da >= 0 ? 900 + 1500 * a : 480 + 650 * a) * M.bright;
    br.bp.frequency.setTargetAtTime(f, t, 0.08);
    // inhale onset: the turnaround at the bottom of the breath
    if (br.prevDa <= 0 && da > 0 && a < 0.35 && L.breath.on)
      push({ t, beat: true, pulse: { strength: 0.15, x: 0.5, y: 0.5 }, source: 'breath' });
    br.prevDa = da; br.prevA = a;
  }

  // ---- thinking: detuned hum + clock-scheduled tremolo -------------------------
  const th = { env: ctx.createGain(), am: ctx.createGain(), lp: ctx.createBiquadFilter(), oscs: [],
    active: false, sched: false, next: 0, n: 0, offAt: 0 };
  th.env.gain.value = 0; th.am.gain.value = 0.12;
  th.lp.type = 'lowpass'; th.lp.frequency.value = 1400; th.lp.Q.value = 0.5;
  th.lp.connect(th.am); th.am.connect(th.env); th.env.connect(L.think.gain);
  const thLfo = ctx.createOscillator(); thLfo.frequency.value = 0.11;
  const thLfoAmt = ctx.createGain(); thLfoAmt.gain.value = 6; thLfo.connect(thLfoAmt); thLfo.start();
  function buildThinkOscs() {
    for (const o of th.oscs) { try { o.stop(); o.disconnect(); } catch {} }
    th.oscs = [];
    const freqs = [...P.think.chord, P.think.chord[0] / 2];
    freqs.forEach((f, i) => {
      for (const det of i === 3 ? [0] : [-5, 5]) {
        const o = ctx.createOscillator(), g = ctx.createGain();
        o.frequency.value = f * semi(M.shift * 0.5); o.detune.value = det; o._f = f;
        thLfoAmt.connect(o.detune);
        g.gain.value = i === 3 ? 0.16 : 0.09 / (1 + i * 0.4);
        o.connect(g); g.connect(th.lp); o.start(); th.oscs.push(o);
      }
    });
  }
  buildThinkOscs();
  function retuneThink(t) { for (const o of th.oscs) o.frequency.setTargetAtTime(o._f * semi(M.shift * 0.5), t, 0.3); }

  function setThinking(on) {
    if (on === th.active) return;
    th.active = on;
    const t = now();
    th.env.gain.cancelScheduledValues(t);
    th.env.gain.setTargetAtTime(on ? 1 : 0, t, on ? 0.12 : 0.18);
    if (on && !th.sched) { th.sched = true; th.next = t + 0.05; th.n = 0; }
    if (!on) th.offAt = t + 1.0;
  }
  function scheduleThink(t) {
    if (!th.sched) return;
    if (!th.active && t > th.offAt) { th.sched = false; return; }
    const per = 1 / P.think.hz, ahead = t + 0.15, g = th.am.gain;
    while (th.next < ahead) {
      const t0 = th.next, accent = th.n % 4 === 0, peak = accent ? 1.0 : 0.6, trough = 0.05;
      g.linearRampToValueAtTime(trough, t0);
      g.linearRampToValueAtTime(peak, t0 + 0.25 * per);
      g.linearRampToValueAtTime(trough + (peak - trough) * 0.35, t0 + 0.55 * per);
      if (th.active && L.think.on)
        push({ t: t0 + 0.25 * per, beat: true, pulse: { strength: accent ? 0.5 : 0.2, x: 0.5, y: 0.5 },
          pitch: pitchNorm(P.think.chord[accent ? 0 : 1] * semi(M.shift * 0.5)), until: t0 + per, source: 'think' });
      th.next += per; th.n++;
    }
  }

  // ---- speaking: formant babble per 'word' ------------------------------------
  const sp = { active: [], wordN: 0 };
  function cancelSpeech(t) {
    for (const v of sp.active) {
      if (v.t1 <= t) continue;
      try { v.env.gain.cancelScheduledValues(t); v.env.gain.setTargetAtTime(0, t, 0.008); v.osc.stop(t + 0.05); } catch {}
    }
    sp.active = [];
    for (let i = queue.length - 1; i >= 0; i--) if (queue[i].source === 'speak' && queue[i].t > t) queue.splice(i, 1);
  }
  function speakWord({ word = '', ms = 300 } = {}) {
    if (!L.speak.on) return [];
    const t = now();
    cancelSpeech(t);
    const sy = syllables(word);
    const n = sy.length;
    const span = Math.max(0.06, (ms / 1000) * 0.82);
    const per = Math.min(span / n, 0.24);
    const len = per * 0.86;
    const h = hashStr(word.toLowerCase() + sp.wordN++);
    const end = /[?]$/.test(word) ? 5 : /[.!]$/.test(word) ? -3 : 0;
    const t0 = t + 0.025;
    const onsets = [];
    sy.forEach((v, i) => {
      const ts = t0 + i * per;
      const off = PENTA[(h >>> (i * 3)) % PENTA.length] + M.shift;
      let f1 = P.f0 * semi(off), f2 = f1 * semi(((h >>> (i * 2 + 9)) & 1) ? 1 : -1);
      if (i === n - 1 && end) f2 = f1 * semi(end);
      const [F1, F2] = VOWELS[v];
      const osc = ctx.createOscillator(); osc.setPeriodicWave(wave);
      const lp = ctx.createBiquadFilter(); lp.type = 'lowpass'; lp.frequency.value = P.bright * M.bright; lp.Q.value = 0.4;
      const b1 = ctx.createBiquadFilter(); b1.type = 'bandpass'; b1.frequency.value = F1; b1.Q.value = P.formantQ;
      const b2 = ctx.createBiquadFilter(); b2.type = 'bandpass'; b2.frequency.value = F2; b2.Q.value = P.formantQ * 1.3;
      const env = ctx.createGain(); env.gain.value = 0;
      osc.connect(lp); lp.connect(b1); lp.connect(b2); lp.connect(env); // small dry path keeps the pitch audible
      const mixF = ctx.createGain(); mixF.gain.value = 2.2; b1.connect(mixF); b2.connect(mixF); mixF.connect(env);
      env.connect(L.speak.gain);
      osc.frequency.setValueAtTime(f1, ts);
      if (P.quantize) osc.frequency.setValueAtTime(f2, ts + len * 0.5);
      else osc.frequency.exponentialRampToValueAtTime(f2, ts + len);
      const atk = Math.min(0.014, len * 0.2), pk = 0.2;
      env.gain.setValueAtTime(0, ts);
      env.gain.linearRampToValueAtTime(pk, ts + atk);
      env.gain.setTargetAtTime(pk * 0.55, ts + atk, len * 0.35);
      env.gain.setTargetAtTime(0, ts + len * 0.7, len * 0.12);
      startStop(osc, ts, ts + len + 0.08);
      osc.onended = () => { for (const n2 of [osc, lp, b1, b2, mixF, env]) { try { n2.disconnect(); } catch {} } };
      sp.active.push({ osc, env, t1: ts + len + 0.08 });
      push({ t: ts, beat: true, pulse: { strength: 0.35 + 0.25 * (i === 0), x: 0.5, y: 0.5 },
        pitch: pitchNorm(f1), until: ts + len, source: 'speak', word, syl: i, n, len });
      onsets.push(ts);
    });
    return onsets;
  }

  // ---- transitions: whoosh on 'state', chime on 'settle' -----------------------
  const tr = { whoosh: null };
  function whoosh({ from, to } = {}, dur = 0.9) {
    if (!L.transition.on) return;
    const t = now() + 0.01;
    if (tr.whoosh) endWhoosh(t, 0.08);
    const src = ctx.createBufferSource(); src.buffer = white; src.loop = true;
    const bp = ctx.createBiquadFilter(); bp.type = 'bandpass'; bp.Q.value = 1.4;
    const env = ctx.createGain(); env.gain.value = 0;
    const tone = ctx.createOscillator(), tg = ctx.createGain(); tone.type = 'sine'; tg.gain.value = 0;
    const fA = (P.think.chord[0] * semi(M.shift)) / 2, fB = fA * (String(to).includes('text') ? 2 : 1.5);
    bp.frequency.setValueAtTime(260, t);
    bp.frequency.exponentialRampToValueAtTime(2400 * M.bright, t + dur * 0.4);
    bp.frequency.exponentialRampToValueAtTime(520, t + dur);
    env.gain.setValueAtTime(0, t);
    env.gain.linearRampToValueAtTime(0.5, t + dur * 0.35);
    env.gain.setTargetAtTime(0, t + dur * 0.5, dur * 0.18);
    tone.frequency.setValueAtTime(fA, t); tone.frequency.exponentialRampToValueAtTime(fB, t + dur * 0.8);
    tg.gain.setValueAtTime(0, t); tg.gain.linearRampToValueAtTime(0.12, t + dur * 0.4);
    tg.gain.setTargetAtTime(0, t + dur * 0.6, dur * 0.15);
    src.connect(bp); bp.connect(env); env.connect(L.transition.gain);
    tone.connect(tg); tg.connect(L.transition.gain);
    src.start(t, rnd() * 1.5); src.stop(t + dur + 1.2); tone.start(t); tone.stop(t + dur + 1.2);
    tr.whoosh = { src, tone, env, tg, t1: t + dur + 1.2 };
    push({ t: t + dur * 0.35, beat: true, pulse: { strength: 0.25, x: 0.5, y: 0.5 }, pitch: pitchNorm(fB), until: t + dur, source: 'transition' });
  }
  function endWhoosh(t, tau = 0.06) {
    const w = tr.whoosh; if (!w) return; tr.whoosh = null;
    if (t >= w.t1) return;
    for (const g of [w.env.gain, w.tg.gain]) { g.cancelScheduledValues(t); g.setTargetAtTime(0, t, tau); }
  }
  function bell(dest, root, t, amp = 1, partials = P.chime.partials, shape = 1) {
    for (const [ratio, a, decay] of partials) {
      const o = ctx.createOscillator(), g = ctx.createGain();
      o.type = 'sine'; o.frequency.value = root * ratio;
      g.gain.setValueAtTime(0, t);
      g.gain.linearRampToValueAtTime(0.16 * a * amp, t + 0.006);
      g.gain.setTargetAtTime(0, t + 0.006, (decay * shape) / 3);
      o.connect(g); g.connect(dest); startStop(o, t, t + decay * shape + 0.3);
      o.onended = () => { try { o.disconnect(); g.disconnect(); } catch {} };
    }
  }
  function settleChime() {
    if (!L.transition.on) return;
    const t = now() + 0.015;
    endWhoosh(t, 0.12);
    const r = P.chime.root * semi(M.shift);
    const second = mood === 'happy' ? 4 : mood === 'sleepy' ? -5 : 7;
    bell(L.transition.gain, r, t, 0.9);
    bell(L.transition.gain, r * semi(second), t + 0.07, 0.55);
    push({ t, beat: true, pulse: { strength: 0.35, x: 0.5, y: 0.5 }, pitch: pitchNorm(r), until: t + 0.5, source: 'settle' });
  }

  // ---- touch: panned water drop -----------------------------------------------
  let emittingPulse = false, lastPulseT = -1;
  function bloop({ x = 0.5, strength = 0.5 } = {}) {
    if (!L.touch.on) return;
    const t = now() + 0.005, s = clamp(strength);
    const [lo, hi] = P.bloop;
    const f = (hi - (hi - lo) * s) * semi(M.shift);
    let out = L.touch.gain;
    if (ctx.createStereoPanner) { const p = ctx.createStereoPanner(); p.pan.value = clamp(x) * 1.6 - 0.8; p.connect(out); out = p; }
    const drop = (tt, ff, amp) => {
      const o = ctx.createOscillator(), g = ctx.createGain(); o.type = 'sine';
      o.frequency.setValueAtTime(ff, tt); o.frequency.exponentialRampToValueAtTime(ff * 2.3, tt + 0.07);
      g.gain.setValueAtTime(0, tt); g.gain.linearRampToValueAtTime(amp, tt + 0.004);
      g.gain.setTargetAtTime(0, tt + 0.01, 0.045);
      o.connect(g); g.connect(out); startStop(o, tt, tt + 0.35);
      o.onended = () => { try { o.disconnect(); g.disconnect(); } catch {} };
    };
    drop(t, f, 0.3 + 0.35 * s);
    drop(t + 0.075 + 0.03 * rnd(), f * 1.5, 0.08 + 0.1 * s);
    // beat only, no pulse: the fluid already rippled (and a pulse would echo forever)
    push({ t, beat: true, pitch: pitchNorm(f), until: t + 0.12, source: 'touch' });
  }

  // ---- moods --------------------------------------------------------------------
  function setMood(name) {
    name = MOODS[name] ? name : 'neutral';
    if (name === mood) return;
    mood = name; M = MOODS[name];
    const t = now();
    duck.gain.setTargetAtTime(M.duck, t, 0.25);
    retuneThink(t);
    setThinking(name === 'thinking' || stateThinking);
    if (!L.mood.on) return;
    const dest = L.mood.gain, t0 = t + 0.02;
    if (name === 'happy') {
      const r = P.chime.root / 2;
      [0, 4, 7, 12].forEach((s, i) => {
        bell(dest, r * semi(s), t0 + i * 0.085, 0.7, P.chime.partials.slice(0, 2), 0.45);
        push({ t: t0 + i * 0.085, beat: true, pulse: { strength: 0.3, x: 0.5, y: 0.5 }, pitch: pitchNorm(r * semi(s)), until: t0 + i * 0.085 + 0.08, source: 'mood' });
      });
    } else if (name === 'sleepy') {
      const o = ctx.createOscillator(), o2 = ctx.createOscillator(), g = ctx.createGain();
      o.type = 'sine'; o2.type = 'sine';
      o.frequency.setValueAtTime(196, t0); o.frequency.exponentialRampToValueAtTime(146.8, t0 + 1.6);
      o2.frequency.setValueAtTime(246.9, t0); o2.frequency.exponentialRampToValueAtTime(174.6, t0 + 1.6);
      g.gain.setValueAtTime(0, t0); g.gain.linearRampToValueAtTime(0.2, t0 + 0.6); g.gain.setTargetAtTime(0, t0 + 0.9, 0.5);
      o.connect(g); o2.connect(g); g.connect(dest); startStop(o, t0, t0 + 3.5); startStop(o2, t0, t0 + 3.5);
      push({ t: t0 + 0.6, beat: true, pulse: { strength: 0.15, x: 0.5, y: 0.5 }, pitch: pitchNorm(170), until: t0 + 1.6, source: 'mood' });
    } else if (name === 'surprised') {
      const o = ctx.createOscillator(), g = ctx.createGain(); o.setPeriodicWave(wave);
      const lp = ctx.createBiquadFilter(); lp.type = 'lowpass'; lp.frequency.value = 3000;
      o.frequency.setValueAtTime(330, t0); o.frequency.exponentialRampToValueAtTime(1250, t0 + 0.22);
      g.gain.setValueAtTime(0, t0); g.gain.linearRampToValueAtTime(0.3, t0 + 0.02); g.gain.setTargetAtTime(0, t0 + 0.18, 0.05);
      o.connect(lp); lp.connect(g); g.connect(dest); startStop(o, t0, t0 + 0.6);
      bell(dest, 1250, t0 + 0.23, 0.35, P.chime.partials.slice(0, 2), 0.4);
      push({ t: t0 + 0.02, beat: true, pulse: { strength: 0.7, x: 0.5, y: 0.5 }, pitch: pitchNorm(700), until: t0 + 0.3, source: 'mood' });
    } else if (name === 'listening') {
      [0, 7].forEach((s, i) => bell(dest, 523.25 * semi(s), t0 + i * 0.12, 0.25, P.chime.partials.slice(0, 1), 0.5));
      push({ t: t0, beat: true, pitch: pitchNorm(523), until: t0 + 0.3, source: 'mood' });
    }
  }

  // ---- mic-level agitation rumble -------------------------------------------------
  const ag = { src: ctx.createBufferSource(), lp: ctx.createBiquadFilter(), amp: ctx.createGain(), sub: ctx.createOscillator(), subG: ctx.createGain() };
  ag.src.buffer = brown; ag.src.loop = true;
  ag.lp.type = 'lowpass'; ag.lp.frequency.value = 170;
  ag.amp.gain.value = 0; ag.sub.frequency.value = 52; ag.subG.gain.value = 0.25;
  ag.src.connect(ag.lp); ag.lp.connect(ag.amp); ag.sub.connect(ag.subG); ag.subG.connect(ag.amp); ag.amp.connect(L.agitation.gain);
  ag.src.start(); ag.sub.start();
  function onEnergy(e, t) {
    const x = clamp((e - 0.08) / 0.92);
    ag.amp.gain.setTargetAtTime(0.9 * x * x, t, 0.1);
    ag.lp.frequency.setTargetAtTime(140 + 260 * x, t, 0.1);
  }

  // ---- bus wiring -------------------------------------------------------------------
  let stateThinking = false, lastFrameMood = null;
  unsubs.push(bus.on('frame', (f = {}) => {
    const t = now();
    if (typeof f.breath === 'number') onBreath(f.breath, t);
    if (typeof f.energy === 'number') onEnergy(f.energy, t);
    if (f.mood != null && f.mood !== lastFrameMood) { lastFrameMood = f.mood; setMood(f.mood); }
  }));
  unsubs.push(bus.on('state', (s = {}) => {
    stateThinking = /think/i.test(String(s.to || ''));
    setThinking(stateThinking || mood === 'thinking');
    whoosh(s);
  }));
  unsubs.push(bus.on('settle', () => settleChime()));
  unsubs.push(bus.on('ripple', (r = {}) => {
    if (emittingPulse || now() - lastPulseT < 0.03) return; // our own pulse echoed back as a ripple
    bloop(r);
  }));
  unsubs.push(bus.on('word', (w) => speakWord(w)));
  unsubs.push(bus.on('mood', (m = {}) => setMood(m.name)));

  // ---- the audio frame: analysis + release of clock-stamped events ----------------
  let pitch = 0.25, pitchUntil = 0, lastTick = 0;
  function rmsLevel(an) {
    an.getFloatTimeDomainData(tdBuf);
    let s = 0; for (let i = 0; i < tdBuf.length; i++) s += tdBuf[i] * tdBuf[i];
    const db = 10 * Math.log10(s / tdBuf.length + 1e-12);
    return clamp((db + 60) / 54); // -60 dBFS → 0, -6 dBFS → 1
  }
  const follow = (prev, x, dt) => x > prev ? prev + (x - prev) * (1 - Math.exp(-dt / 0.012)) : prev + (x - prev) * (1 - Math.exp(-dt / 0.05));
  let mainEnv = 0;
  function tick() {
    if (disposed) return null;
    const t = now(), heard = t - outLat(), dt = clamp(t - lastTick, 1e-3, 0.1); lastTick = t;
    scheduleThink(t);
    let beat = false; const pulses = [];
    while (queue.length && queue[0].t <= heard) {
      const ev = queue.shift();
      if (ev.beat) beat = true;
      if (ev.pitch != null) { pitch = ev.pitch; pitchUntil = ev.until || ev.t + 0.1; }
      if (ev.pulse) pulses.push({ ...ev.pulse, t: ev.t, source: ev.source });
    }
    if (heard > pitchUntil) pitch += (0.25 - pitch) * (1 - Math.exp(-dt / 0.3));
    const lv = {};
    for (const name of LAYERS) { const l = L[name]; l.env = follow(l.env, l.on ? rmsLevel(l.an) : 0, dt); lv[name] = l.env; }
    mainEnv = follow(mainEnv, rmsLevel(report), dt);
    const frame = { level: mainEnv, pitch, beat, t: heard, layers: lv };
    bus.emit('audio', frame);
    if (pulses.length) {
      emittingPulse = true;
      try { for (const p of pulses) { lastPulseT = t; bus.emit('pulse', p); } } finally { emittingPulse = false; }
    }
    return frame;
  }

  function setLayer(name, on) {
    const l = L[name]; if (!l) return;
    l.on = !!on; l.gain.gain.setTargetAtTime(on ? 1 : 0, now(), 0.05);
    if (name === 'speak' && !on) cancelSpeech(now());
  }
  function setPreset(name) {
    if (!PRESETS[name]) return;
    P = PRESETS[name]; wave = makeWave(ctx, P.harmonics); buildThinkOscs();
  }
  function setVolume(v) { masterVol.gain.setTargetAtTime(clamp(v), now(), 0.03); }
  function dispose() {
    disposed = true; unsubs.forEach((u) => u());
    for (const n of [br.src, thLfo, ag.src, ag.sub, ...th.oscs]) { try { n.stop(); } catch {} }
    try { comp.disconnect(); sink.disconnect(); } catch {}
  }

  return {
    ctx, tick, setLayer, setPreset, setVolume, setThinking, setMood, dispose,
    speakWord, whoosh, settleChime, bloop,
    get preset() { return P.label; }, get mood() { return mood; },
    layerOn: (n) => !!(L[n] && L[n].on),
  };
}
