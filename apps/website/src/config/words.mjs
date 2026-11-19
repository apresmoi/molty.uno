// The page's words as a scenes package (docs/PACKAGES.md): each a layout of thin lines, drawn on
// the page by the firmware's own scene player, in high definition, as it draws a chart's lines
// (dust gathering along each stroke). Data only: the letters below are strokes in a unit box
// (x right, y up; the baseline 0, the x-height 1, capitals and ascenders 1.5, descenders -0.5).
// scripts/words-package.mjs writes the package (src/assets/molty/words.json); plain .mjs in config/ because
// that script and the tests read it under Node.

/** A chart's line weight (packages/astrology: its rings and lines), glass units. */
export const LINE = 0.012;

/** The words: layout name, text, colour. */
export const WORDS = [
  ['molty-uno', 'molty.UNO', '#3ccf96'],
  ['talk', 'talk', '#9cb8ac'],
  ['moltys', 'moltys', '#a99be8'],
  ['bridge', 'bridge', '#c6a8f2'],
  ['bake', 'bake', '#ffdf5a'],
  ['showcase', 'showcase', '#a99be8'],
  ['build', 'build', '#3ccf96'],
];

const L = (...pts) => ({ k: 'line', pts });
/** Part of a circle: centre, radius, from a0 to a1 degrees (0 = right, counter-clockwise). */
const A = (x, y, r, a0, a1) => ({ k: 'arc', x, y, r, a0, a1 });
const O = (x, y, r) => ({ k: 'ring', x, y, r });

/** Each letter: its advance and its strokes (straight lines and parts of circles only: each is one
 *  shape to the renderer, which draws at most 256 in high definition). */
export const GLYPHS = {
  m: { w: 1.4, s: [L([0, 0], [0, 1]), A(0.35, 0.65, 0.35, 0, 180), L([0.7, 0], [0.7, 0.65]), A(1.05, 0.65, 0.35, 0, 180), L([1.4, 0], [1.4, 0.65])] },
  o: { w: 1, s: [O(0.5, 0.5, 0.5)] },
  l: { w: 0, s: [L([0, 0], [0, 1.5])] },
  t: { w: 0.7, s: [L([0.3, 1.45], [0.3, 0.25]), A(0.55, 0.25, 0.25, 180, 270), L([0.55, 0], [0.7, 0]), L([0, 1], [0.66, 1])] },
  y: { w: 0.9, s: [L([0, 1], [0.45, 0]), L([0.9, 1], [0.22, -0.5])] },
  '.': { w: 0.1, s: [O(0.05, 0.05, 0.05)] },
  u: { w: 0.7, s: [L([0, 1], [0, 0.35]), A(0.35, 0.35, 0.35, 180, 360), L([0.7, 1], [0.7, 0])] },
  U: { w: 1, s: [L([0, 1.5], [0, 0.5]), A(0.5, 0.5, 0.5, 180, 360), L([1, 0.5], [1, 1.5])] },
  N: { w: 1, s: [L([0, 0], [0, 1.5], [1, 0], [1, 1.5])] },
  O: { w: 1, s: [A(0.5, 1, 0.5, 0, 180), L([0, 1], [0, 0.5]), A(0.5, 0.5, 0.5, 180, 360), L([1, 0.5], [1, 1])] },
  a: { w: 1, s: [O(0.5, 0.5, 0.5), L([1, 1], [1, 0])] },
  k: { w: 0.8, s: [L([0, 0], [0, 1.5]), L([0.75, 1], [0, 0.4]), L([0.3, 0.64], [0.8, 0])] },
  s: { w: 0.6, s: [A(0.3, 0.75, 0.25, 30, 270), A(0.3, 0.25, 0.25, -150, 90)] },
  b: { w: 1, s: [L([0, 1.5], [0, 0]), O(0.5, 0.5, 0.5)] },
  r: { w: 0.65, s: [L([0, 1], [0, 0]), A(0.35, 0.65, 0.35, 30, 180)] },
  i: { w: 0, s: [L([0, 1], [0, 0]), O(0, 1.35, 0.05)] },
  d: { w: 1, s: [O(0.5, 0.5, 0.5), L([1, 1.5], [1, 0])] },
  g: { w: 1, s: [O(0.5, 0.5, 0.5), L([1, 1], [1, -0.05]), A(0.5, -0.05, 0.5, 200, 360)] },
  e: { w: 1, s: [A(0.5, 0.5, 0.5, 0, 315), L([0, 0.5], [1, 0.5])] },
  h: { w: 0.7, s: [L([0, 1.5], [0, 0]), A(0.35, 0.65, 0.35, 0, 180), L([0.7, 0.65], [0.7, 0])] },
  w: { w: 1.3, s: [L([0, 1], [0.32, 0], [0.65, 0.85], [0.98, 0], [1.3, 1])] },
  c: { w: 0.9, s: [A(0.5, 0.5, 0.5, 45, 315)] },
};

const GAP = 0.34; // between letters
const SPAN = 1.8; // the glass's width a word may take (glass units: the round window is 2 wide)
const SCALE_MAX = 0.24; // a short word's letters are no bigger than this (its x-height, glass units)

/** How a word sits on its round window: its scale and its half width, glass units. */
export function measure(text) {
  const advance = [...text].reduce((w, ch) => w + GLYPHS[ch].w, 0) + GAP * (text.length - 1);
  const scale = Math.min(SPAN / advance, SCALE_MAX);
  return { scale, half: (advance * scale) / 2 };
}

/** A word's layout: its strokes, centred on the glass. */
export function layout(text, color) {
  const { scale, half } = measure(text);
  const round = (v) => Math.round(v * 1000) / 1000;
  const items = [];
  let x = -half;
  for (const ch of text) {
    const g = GLYPHS[ch];
    for (const st of g.s) {
      const at = (px, py) => [round(x + px * scale), round((py - 0.5) * scale)];
      if (st.k === 'line') items.push({ k: 'line', w: LINE, color, pts: st.pts.map(([px, py]) => at(px, py)) });
      else {
        const [cx, cy] = at(st.x, st.y);
        const r = round(st.r * scale);
        items.push(st.k === 'ring' ? { k: 'ring', x: cx, y: cy, r, w: LINE, color } : { k: 'arc', x: cx, y: cy, r, w: LINE, a0: st.a0, a1: st.a1, color });
      }
    }
    x += (g.w + GAP) * scale;
  }
  return { dur: 0, items };
}

/** The package. */
export const wordsPackage = () => ({
  v: 'molty-pkg/1',
  id: 'words',
  name: 'Words',
  version: '1.0.0',
  kind: 'scenes',
  assets: {},
  layouts: Object.fromEntries(WORDS.map(([name, text, color]) => [name, layout(text, color)])),
});
