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

export const OPTIONS = [
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
