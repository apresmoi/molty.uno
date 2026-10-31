// The three OG card options: what each card holds and how it is laid out (card-only CSS; the
// parts keep the site's own classes and tokens). `settle`: ms after the fluid is live (and the
// Moltys, or `waitFor`) before the capture, picked from a burst (OG_BURST=1); `pick`: the best
// frame in a window instead (pickFrame in generate-options.mjs: the Molty's face, the
// wordmark mid-condense).
const base = `
  html, body { margin: 0; width: 1200px; height: 630px; overflow: hidden; background: var(--m-bg); }
  .og { position: relative; box-sizing: border-box; width: 1200px; height: 630px; }
  /* over the page fluid's fixed layer, as the landing's own blocks are */
  .og > :not(astro-island), .og [data-speed-reader] > * { position: relative; z-index: 1; }
  .og-line { width: auto; margin: 0; color: var(--m-ink); font: 26px / 1.3 var(--m-font-mono); }
`;

/** B3's layout (B2 polished, the same composition): the device at ~95 % of the card (screen
 *  550 px, bezel ~600). The wordmark 88 px at 33 % down the screen, under the glass's
 *  highlight: its glyphs (5.24 em wide, from ~5 px below the box top) keep ≥ 30 px inside the
 *  circle (half-width 231 px ≤ √(245² − 79²) = 232). The main Molty's 280 px slot from 44.5 %
 *  down: its body ends ~10 % above the slot's foot, inside the rim. Companions: 270 px slots
 *  in the plain field, a veil only over the card's outer 150 px. */
const FAMILY = `${base}
  .og > .og-screen { position: absolute; left: 325px; top: 40px; width: 550px; aspect-ratio: 1; }
  .og-screen > .og-word { position: absolute; left: 50%; top: 33%; translate: -50% 0; font-size: 88px; font-weight: 800; line-height: 1; white-space: nowrap; }
  .og-screen > .og-under { position: absolute; left: 50%; top: 44.5%; translate: -50% 0; width: 280px; aspect-ratio: 1; margin: 0; }
  .og > .og-side-zone { position: absolute; top: 0; width: 300px; height: 630px; }
  .og > .og-side-zone.og-left { left: 0; }
  .og > .og-side-zone.og-right { right: 0; }
  .og > .og-companion { position: absolute; top: 180px; width: 270px; aspect-ratio: 1; margin: 0; }
  .og > .og-companion.og-left { left: 28px; }
  .og > .og-companion.og-right { right: 28px; }
  .og > .og-veil { position: absolute; top: 0; width: 150px; height: 630px; pointer-events: none; z-index: 1; }
  .og > .og-veil.og-left { left: 0; background: linear-gradient(to right, rgba(0, 0, 0, 0.6), rgba(0, 0, 0, 0)); }
  .og > .og-veil.og-right { right: 0; background: linear-gradient(to left, rgba(0, 0, 0, 0.6), rgba(0, 0, 0, 0)); }
`;

/** The settled window: every Molty is asked to hold the `listening` mood (front-on, attentive;
 *  the showcase's data-fluid-act) once formed, and the frames come from inside that 4 s hold,
 *  after its 0.7 s fade-in: no hops, turns or glass beats. The card pass: both companions
 *  formed together, eyes open. The screen pass: the wordmark whole and still, the main
 *  Molty's eyes open (not mid-blink). */
const WINDOW = { act: 'mood:listening', from: 2300, to: 5300 };
const SIDES = { ...WINDOW, face: '.og-companion', white: 225, share: true, settled: '.og-companion' };
const SCREEN_PICK = { ...WINDOW, face: '.og-under', band: [0.96, 1.1] };

/** The screen pass: B3's screen at 2× (1100 px) in a 1300 px viewport, everything inside it
 *  scaled with it (the wordmark 176 px, the Molty's slot 560 px). */
const SCREEN_CSS = `
  html, body { margin: 0; width: 1300px; height: 1300px; overflow: hidden; background: var(--m-bg); }
  .og { position: relative; box-sizing: border-box; width: 1300px; height: 1300px; }
  .og > :not(astro-island) { position: relative; z-index: 1; }
  .og > .og-screen { position: absolute; left: 100px; top: 100px; width: 1100px; aspect-ratio: 1; }
  .og-screen > .og-word { position: absolute; left: 50%; top: 33%; translate: -50% 0; font-size: 176px; font-weight: 800; line-height: 1; white-space: nowrap; }
  .og-screen > .og-under { position: absolute; left: 50%; top: 44.5%; translate: -50% 0; width: 560px; aspect-ratio: 1; margin: 0; }
`;
const SCREEN = { scale: 2, viewport: { width: 1300, height: 1300 }, css: SCREEN_CSS, pick: SCREEN_PICK, bezel: 1.1, lift: 1.35 };

export const OPTIONS = [
  // B3 ("family"): the brand on a max-size screen, the main Molty under it, two companions
  // at the sides; the centre square (device + wordmark + main Molty) stands alone. Captured
  // SETTLED (no pointer), in two real renders: the card (companions formed together) and a
  // 2× screen (wordmark whole, the main Molty's best face) set into it. Once per main Molty.
  {
    name: 'family-duck',
    file: 'B3-duck',
    moltyDir: 'b3',
    molty: 'cute-duck',
    companions: ['cute-bunny', 'cute-octopus'],
    themes: ['dark'],
    square: true,
    wordDelay: 0.3,
    pick: SIDES,
    screen: SCREEN,
    css: FAMILY,
  },
  {
    name: 'family-bunny',
    file: 'B3-bunny',
    moltyDir: 'b3',
    molty: 'cute-bunny',
    companions: ['cute-duck', 'cute-ghost'],
    themes: ['dark'],
    square: true,
    wordDelay: 0.3,
    pick: SIDES,
    screen: { ...SCREEN, pick: { ...SCREEN_PICK, white: 235 } },
    css: FAMILY,
  },
  // The device IS the image (hardware brands' cards: Playdate, Tidbyt, Analogue): the round
  // device centred at ~88 % of the card's height, everything that matters in the centre square
  // (messengers crop to it; X may trim top and bottom), few big elements (previews are tiny).
  {
    name: 'big',
    file: 'A-big',
    moltyDir: 'big-device',
    prefix: 'A-big-',
    molty: 'cute-duck',
    themes: ['dark'],
    square: true,
    pick: { from: 500, to: 5500 },
    css: `${base}
      .og > .og-screen { position: absolute; left: 345px; top: 50px; width: 510px; aspect-ratio: 1; margin: 0; }
      .og > .og-mark { position: absolute; left: 40px; bottom: 34px; }
      .og-mark > span { display: inline-flex; align-items: center; gap: 12px; color: var(--m-accent); font: 700 34px / 1 var(--m-font-mono); }
      .og-mark svg { width: 24px; height: 24px; }
    `,
  },
  // …and the brand ON the screen (Playdate's trick): the fluid condenses "molty.UNO" across the
  // upper screen, the Molty forms under it
  {
    name: 'brand',
    file: 'B-brand',
    moltyDir: 'big-device',
    prefix: 'B-brand-',
    molty: 'cute-duck',
    themes: ['dark'],
    square: true,
    wordDelay: 0.3,
    pick: { from: 1500, to: 6500 },
    css: `${base}
      .og > .og-screen { position: absolute; left: 345px; top: 50px; width: 510px; aspect-ratio: 1; }
      .og-screen > .og-word { position: absolute; left: 50%; top: 31%; translate: -50% 0; font-size: 84px; line-height: 1; white-space: nowrap; }
      .og-screen > .og-under { position: absolute; left: 50%; bottom: -3%; translate: -50% 0; width: 330px; aspect-ratio: 1; margin: 0; }
    `,
  },
  {
    name: 'device',
    file: '1-device',
    moltyDir: '.', // OG_MOLTYS: <outDir>/<name>.png
    molty: 'cute-duck',
    line: 'Liquid ASCII creatures. Any agent is the brain.',
    // its first dances, before the glass beat (a close-up) comes round
    pick: { from: 500, to: 5500 },
    css: `${base}
      .og-device { display: flex; flex-direction: column; align-items: center; padding-top: 26px; }
      .og-screen { position: relative; flex: none; width: 372px; aspect-ratio: 1; margin: 0; }
      .og-device .og-word { margin-top: 18px; font-size: 104px; line-height: 1; }
      .og-device .og-line { margin-top: 14px; }
    `,
  },
  {
    // the device card with a bolder wordmark (the braille reads weakly at 600 px): a smaller
    // screen, a larger heading
    name: 'device-word',
    file: '1b-device',
    moltyDir: 'bold-word',
    molty: 'cute-duck',
    themes: ['dark'],
    line: 'Liquid ASCII creatures. Any agent is the brain.',
    pick: { from: 500, to: 5500 },
    css: `${base}
      .og-device-word { display: flex; flex-direction: column; align-items: center; padding-top: 22px; }
      .og-screen { position: relative; flex: none; width: 330px; aspect-ratio: 1; margin: 0; }
      .og-device-word .og-word { margin-top: 14px; font-size: 140px; line-height: 1; }
      .og-device-word .og-line { margin-top: 8px; }
    `,
  },
  {
    name: 'conversation',
    file: '2-conversation',
    question: 'Build passed. Open the PR?',
    line: 'Your agent asks. One tap on the glass.',
    waitFor: '[data-reader-glass][data-listening]',
    settle: 1800,
    css: `${base}
      .og-conversation { display: grid; grid-template-columns: 500px 1fr; align-items: center; column-gap: 64px; padding: 0 60px 0 66px; }
      .og-conversation [data-reader-glass] { grid-column: 1; grid-row: 1; width: 500px; }
      .og-conversation [data-speed-reader] > [data-fluid-quiet] { grid-column: 2; grid-row: 1; align-self: center; max-width: none; padding-bottom: 90px; }
      .og-conversation [data-speed-reader] > [data-fluid-quiet] > p:first-child { max-width: 15ch; color: var(--m-ink); font: 600 46px / 1.18 var(--m-font-mono); letter-spacing: -0.02em; }
      .og-conversation [data-speed-reader] > [data-fluid-quiet] > p:nth-child(2) { font-size: 22px; }
      .og-conversation [data-speed-reader] > [data-fluid-quiet] > :nth-child(n + 3) { display: none; }
      .og > .og-side { position: absolute; left: 640px; bottom: 54px; }
      .og-foot > span { display: inline-flex; align-items: center; gap: 12px; color: var(--m-accent); font: 700 34px / 1 var(--m-font-mono); }
      .og-foot svg { width: 24px; height: 24px; }
      .og-foot .og-line { margin-top: 12px; color: var(--m-dim); font-size: 22px; }
    `,
  },
  {
    name: 'wordmark',
    file: '3-wordmark',
    molty: 'cute-octopus',
    // light: a page Molty is drawn inverted on paper (a Molty lives on its screen) and the
    // wordmark mid-condense washes out; this card is dark only
    themes: ['dark'],
    line: 'Liquid ASCII creatures for any screen.',
    wordDelay: 6,
    moltyDelay: 0,
    // the wordmark starts condensing 6 s after the fluid mounts: frames from 3 s into the
    // Moltys being live until 7 s cover it
    pick: { from: 3000, to: 7000, band: [0.45, 0.75] },
    css: `${base}
      .og-wordmark { padding: 118px 0 0 44px; }
      .og-wordmark .og-word { font-size: 180px; line-height: 1; }
      .og-wordmark .og-line { margin-top: 64px; font-size: 30px; }
      .og > .og-molty { position: absolute; right: 0; top: 176px; width: 460px; aspect-ratio: 1; margin: 0; }
    `,
  },
];
