// The light theme (src/styles/tokens.css): every text colour token holds WCAG AA on the light
// surfaces.
// Run: pnpm test (node --test; Node 22 strips the types).
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

/** WCAG relative luminance. */
function luminance(hex: string): number {
  const lin = (c: number) => (c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4);
  const [r, g, b] = [1, 3, 5].map((i) => lin(parseInt(hex.slice(i, i + 2), 16) / 255)) as [number, number, number];
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

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
  for (const text of ["ink", "dim", "accent", "warn", "sky"])
    for (const surface of ["bg", "panel", "glass"]) {
      const fg = read(light, text),
        bg = read(light, surface);
      assert.ok(fg && bg, `--m-${text} / --m-${surface} have light values`);
      assert.ok(ratio(fg, bg) >= 4.5, `--m-${text} ${fg} on --m-${surface} ${bg}: ${ratio(fg, bg).toFixed(2)}:1`);
    }
});
