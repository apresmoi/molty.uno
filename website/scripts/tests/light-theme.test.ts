// The light theme (src/styles/tokens.css, services/molty-stage/paper.ts): every text colour token
// holds WCAG AA on the light surfaces; the fluid filter is plain invert(1), which the device
// screens undo with the same filter (exact only for a pure invert: anything else clips the
// Moltys' colours); the page's own fluid colours land on paper in their own hue at ≥ 3:1.
// Run: pnpm test (node --test; Node 22 strips the types).
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { contrastOnWhite, inverted, luminance, paperInk, paperSource } from "../../src/services/molty-stage/paper.ts";

const tokens = readFileSync(new URL("../../src/styles/tokens.css", import.meta.url), "utf8");
const block = (selector: string) => {
  const at = tokens.indexOf(`${selector} {`);
  assert.ok(at >= 0, `${selector} block`);
  return tokens.slice(at, tokens.indexOf("}", at));
};
const read = (css: string, name: string) => new RegExp(`--m-${name}:\\s*([^;]+);`).exec(css)?.[1]?.trim();
const light = block(':root[data-theme="light"]');
const hex6 = (c: string) => (c.length === 4 ? "#" + [...c.slice(1)].map((x) => x + x).join("") : c);
const ratio = (a: string, b: string) => {
  const [x, y] = [luminance(hex6(a)), luminance(hex6(b))];
  return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05);
};

test("light text tokens hold AA (4.5:1) on bg, panel and glass", () => {
  for (const text of ["ink", "dim", "accent", "warn"])
    for (const surface of ["bg", "panel", "glass"]) {
      const fg = read(light, text),
        bg = read(light, surface);
      assert.ok(fg && bg, `--m-${text} / --m-${surface} have light values`);
      assert.ok(ratio(fg, bg) >= 4.5, `--m-${text} ${fg} on --m-${surface} ${bg}: ${ratio(fg, bg).toFixed(2)}:1`);
    }
});

test("the light fluid filter is a plain invert, undone by the screens with the same filter", () => {
  assert.equal(read(light, "fluid-filter"), "invert(1)");
  assert.equal(read(light, "screen-filter"), read(light, "fluid-filter"));
  assert.equal(read(light, "still-filter"), read(light, "fluid-filter"));
  for (const c of ["#ffdf5a", "#3ccf96", "#c6a8f2", "#000000", "#ffffff"]) assert.equal(inverted(inverted(c)), c);
});

test("a heading lands on paper in its own hue at 3:1, darkened no more than that", () => {
  const hue = (hex: string) => {
    const [r, g, b] = [1, 3, 5].map((i) => parseInt(hex.slice(i, i + 2), 16));
    const max = Math.max(r, g, b),
      min = Math.min(r, g, b),
      d = max - min;
    const h = max === r ? ((g - b) / d) % 6 : max === g ? (b - r) / d + 2 : (r - g) / d + 4;
    return (h * 60 + 360) % 360;
  };
  for (const c of ["#3ccf96", "#9cb8ac", "#c6a8f2", "#ffdf5a", "#ff6b6b"]) {
    const ink = paperInk(c);
    const k = contrastOnWhite(ink);
    assert.ok(k >= 3 && k < 3.15, `${c} → ${ink}: ${k.toFixed(2)}:1`);
    const dh = Math.abs(hue(ink) - hue(c));
    assert.ok(Math.min(dh, 360 - dh) < 12, `${c} → ${ink} keeps its hue`);
    // what the fluid is handed reads as the ink after the page's invert(1)
    assert.equal(inverted(paperSource(c)), ink);
  }
  assert.equal(paperInk("#18201c"), "#18201c", "already dark enough: unchanged");
});
