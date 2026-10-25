// A fake fluid + scripted scenarios, used by test.html (live) and offline.js (rendering).
// The fake fluid emits 'frame' like the real visuals would, and reacts to 'pulse'/'audio'
// by raising its surface energy (so the mic-rumble loop is exercised too).

export class FakeFluid {
  constructor(bus) {
    this.bus = bus; this.breathing = true; this.period = 4.2; this.mood = null; this.state = 'blob';
    this.energy = 0.05; this.micEnergy = 0; this.level = 0; this.t = 0;
    this.offs = [
      bus.on('pulse', (p) => { this.energy = Math.min(1, this.energy + 0.25 * (p.strength ?? 0.5)); }),
      bus.on('audio', (a) => { this.level = a.level; }),
    ];
  }
  frame(t, dt) {
    this.t = t;
    this.energy += (0.05 - this.energy) * (1 - Math.exp(-dt / 0.4));
    const breath = this.breathing ? (t / this.period) % 1 : 0;
    const energy = Math.min(1, Math.max(this.energy, this.micEnergy));
    this.bus.emit('frame', { t, dt, energy, breath, state: this.state, mood: this.mood });
  }
  dispose() { this.offs.forEach((o) => o()); }
}

export const SENTENCE = 'hello there. i am molty, a little fluid of letters. do you want to play?';
export const wordMs = (w) => 200 + 26 * w.replace(/[^\p{L}\p{N}]/gu, '').length + (/[.,!?]$/.test(w) ? 220 : 0);

function speakActions(at, text = SENTENCE) {
  const out = []; let t = at;
  for (const word of text.split(/\s+/)) {
    const ms = wordMs(word);
    out.push([t, 'word', { word, orp: Math.floor(word.length * 0.35), ms }]);
    t += ms / 1000;
  }
  return { actions: out, end: t };
}

// Each scenario: seconds, optional fluid setup, timed bus actions [t, type, data] and
// optional fluid hooks [t, fn(fluid)].
export const SCENARIOS = {
  breathe: { seconds: 9, setup: (f) => { f.breathing = true; }, actions: [] },
  think: {
    seconds: 6, setup: (f) => { f.breathing = true; },
    actions: [[0.2, 'mood', { name: 'thinking' }], [5.2, 'mood', { name: 'neutral' }]],
  },
  speak: (() => { const s = speakActions(0.3); return { seconds: s.end + 0.8, setup: (f) => { f.breathing = false; }, actions: s.actions }; })(),
  transition: {
    seconds: 6.5, setup: (f) => { f.breathing = false; },
    actions: [
      [0.3, 'state', { from: 'blob', to: 'creature' }], [1.25, 'settle', { state: 'creature' }],
      [2.4, 'state', { from: 'creature', to: 'text' }], [3.2, 'settle', { state: 'text' }],
      [4.3, 'state', { from: 'text', to: 'effect:ripple' }], [5.3, 'settle', { state: 'effect:ripple' }],
    ],
  },
  touch: {
    seconds: 5, setup: (f) => { f.breathing = false; },
    actions: [0.2, 0.75, 1.2, 1.9, 2.4, 2.75, 3.3, 4.0].map((t, i) => [t, 'ripple', { x: (i * 0.37) % 1, y: 0.5, strength: [0.3, 0.8, 0.5, 1, 0.2, 0.6, 0.9, 0.4][i] }]),
  },
  moods: {
    seconds: 11, setup: (f) => { f.breathing = true; },
    actions: [[0.2, 'mood', { name: 'happy' }], [2.2, 'mood', { name: 'sleepy' }], [5.2, 'mood', { name: 'surprised' }],
      [6.8, 'mood', { name: 'listening' }], [8.4, 'mood', { name: 'thinking' }], [10.4, 'mood', { name: 'neutral' }]],
  },
  agitation: {
    seconds: 6, setup: (f) => { f.breathing = false; },
    hooks: Array.from({ length: 60 }, (_, i) => [i * 0.1, (f) => { const x = i / 40; f.micEnergy = x < 1 ? x : Math.max(0, 1 - (x - 1) * 2); }]),
    actions: [],
  },
};
SCENARIOS.all = (() => {
  const s = speakActions(5.4, 'oh hello. i was thinking about you.');
  return {
    seconds: 14, setup: (f) => { f.breathing = true; },
    actions: [
      [0.4, 'ripple', { x: 0.3, y: 0.4, strength: 0.6 }], [1.2, 'ripple', { x: 0.75, y: 0.6, strength: 0.9 }],
      [2.0, 'mood', { name: 'thinking' }], [4.6, 'mood', { name: 'surprised' }],
      [4.9, 'state', { from: 'blob', to: 'text' }], [5.3, 'settle', { state: 'text' }],
      ...s.actions,
      [s.end + 0.2, 'state', { from: 'text', to: 'creature' }], [s.end + 1.1, 'settle', { state: 'creature' }],
      [s.end + 1.4, 'mood', { name: 'happy' }],
    ],
  };
})();
