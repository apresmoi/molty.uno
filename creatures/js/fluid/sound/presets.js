// Voice/character presets for the fluid sound layer. Pure data — no Web Audio here,
// so the same tables can be ported to the device (C structs) unchanged.
//
// harmonics: amplitude of partials 1..N for the babble/chime oscillator (PeriodicWave).
// f0:        base speaking pitch (Hz). Syllable pitches are pentatonic offsets from it.
// formantQ:  resonance of the two vowel formant bandpasses (higher = more "vowel").
// bright:    lowpass cutoff on the voice source (Hz).
// quantize:  chiptune-style stepped pitch glides instead of smooth portamento.
// think:     chord (Hz) of the thinking hum, and its tremolo rate (Hz).
// chime:     settle chime root (Hz) + inharmonic partial ratios/amps/decays.
// bloop:     water-drop base pitch range (Hz) for touch ripples.

export const PRESETS = {
  glass: {
    label: 'glass',
    harmonics: [1, 0.32, 0.12, 0.05, 0.02],
    f0: 330,
    formantQ: 7,
    bright: 3200,
    quantize: false,
    think: { chord: [261.63, 392.0, 523.25], hz: 7.0 },
    chime: { root: 880, partials: [[1, 1, 1.4], [2.76, 0.35, 0.7], [5.4, 0.12, 0.35]] },
    bloop: [520, 1300],
  },
  warm: {
    label: 'warm',
    harmonics: [1, 0.55, 0.36, 0.25, 0.17, 0.11, 0.07, 0.04],
    f0: 190,
    formantQ: 5,
    bright: 1800,
    quantize: false,
    think: { chord: [130.81, 196.0, 261.63], hz: 6.0 },
    chime: { root: 523.25, partials: [[1, 1, 1.1], [2, 0.3, 0.6], [3, 0.12, 0.4]] },
    bloop: [360, 900],
  },
  'chiptune-soft': {
    label: 'chiptune-soft',
    // odd partials, rolled off: a rounded square
    harmonics: [1, 0, 0.3, 0, 0.14, 0, 0.07, 0, 0.03],
    f0: 262,
    formantQ: 4,
    bright: 2600,
    quantize: true,
    think: { chord: [220.0, 329.63, 440.0], hz: 8.0 },
    chime: { root: 659.25, partials: [[1, 1, 0.6], [2, 0.4, 0.35], [3, 0.15, 0.2]] },
    bloop: [440, 1100],
  },
};

export const PRESET_NAMES = Object.keys(PRESETS);

// Vowel formants (F1, F2 in Hz) — rough adult averages, squeezed a little upward so
// the babble reads small/cute rather than human.
export const VOWELS = {
  a: [850, 1350], e: [520, 2000], i: [330, 2450], o: [560, 980], u: [360, 850], y: [330, 2300],
};

// Pentatonic semitone offsets — any random pick still sounds musical.
export const PENTA = [0, 2, 4, 7, 9];

// Mood → voice modifiers. shift = semitones on all tonal voices, bright = cutoff scale,
// duck = output scale of the non-mic layers ("listening = quiet").
export const MOODS = {
  neutral:   { shift: 0,  bright: 1.0, duck: 1.0 },
  happy:     { shift: 3,  bright: 1.3, duck: 1.0 },
  sleepy:    { shift: -5, bright: 0.6, duck: 0.8 },
  surprised: { shift: 5,  bright: 1.4, duck: 1.0 },
  listening: { shift: 0,  bright: 0.9, duck: 0.45 },
  thinking:  { shift: -1, bright: 0.9, duck: 1.0 },
};
