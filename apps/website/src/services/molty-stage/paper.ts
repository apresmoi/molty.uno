// The light theme's paper. The page fluid is drawn as in the dark (light glyphs on black) and the
// page inverts it (`--m-fluid-filter: invert(1)`), so a cell lit at strength k in colour c reads
// as 1 - k·c on white: ink on paper. Plain invert(1) is its own exact inverse (no clipping), so a
// device screen undoes it with the same filter (`--m-screen-filter`) and its Molty shows exactly
// as in the dark theme. Invert also turns hues to their complement, so the colours the page
// itself hands the fluid (the dim field, the headings) are handed over pre-inverted: what lands
// on paper is the intended hue, darkened only as far as a 3:1 contrast on white needs.

type RGB = [number, number, number];

const hexOf = (rgb: RGB) =>
  "#" +
  rgb
    .map((v) =>
      Math.round(Math.min(1, Math.max(0, v)) * 255)
        .toString(16)
        .padStart(2, "0"),
    )
    .join("");
const rgbOf = (hex: string): RGB => [1, 3, 5].map((i) => parseInt(hex.slice(i, i + 2), 16) / 255) as RGB;
const lin = (c: number) => (c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4);
const gam = (c: number) => (c <= 0.0031308 ? 12.92 * c : 1.055 * c ** (1 / 2.4) - 0.055);

/** WCAG relative luminance and the contrast of a colour on white. */
export function luminance(hex: string): number {
  const [r, g, b] = rgbOf(hex).map(lin) as RGB;
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}
export const contrastOnWhite = (hex: string) => 1.05 / (luminance(hex) + 0.05);

// OKLab (Björn Ottosson) ↔ linear sRGB
function toLab([r, g, b]: RGB): RGB {
  const l = Math.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
  const m = Math.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
  const s = Math.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
  return [
    0.2104542553 * l + 0.793617785 * m - 0.0040720468 * s,
    1.9779984951 * l - 2.428592205 * m + 0.4505937099 * s,
    0.0259040371 * l + 0.7827717662 * m - 0.808675766 * s,
  ];
}
function fromLab([L, a, b]: RGB): RGB {
  const l = (L + 0.3963377774 * a + 0.2158037573 * b) ** 3;
  const m = (L - 0.1055613458 * a - 0.0638541728 * b) ** 3;
  const s = (L - 0.0894841775 * a - 1.291485548 * b) ** 3;
  return [
    4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
    -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
    -0.0041960863 * l - 0.7034186147 * m + 1.707614701 * s,
  ];
}
const inGamut = (rgb: RGB) => rgb.every((v) => v >= -1e-4 && v <= 1 + 1e-4);

/** OKLCH lightness L at hue h: the most chroma (up to `c`) that sRGB can show, as hex. */
function lch(L: number, c: number, h: number): string {
  let lo = 0,
    hi = c;
  if (!inGamut(fromLab([L, c * Math.cos(h), c * Math.sin(h)]))) {
    for (let i = 0; i < 24; i++) {
      const mid = (lo + hi) / 2;
      if (inGamut(fromLab([L, mid * Math.cos(h), mid * Math.sin(h)]))) lo = mid;
      else hi = mid;
    }
    c = lo;
  }
  return hexOf(fromLab([L, c * Math.cos(h), c * Math.sin(h)]).map(gam) as RGB);
}

/** A colour as ink on white paper: its own hue and chroma, its OKLCH lightness lowered just
 *  enough for `min` contrast on white (unchanged if it already has it). */
export function paperInk(hex: string, min = 3): string {
  if (contrastOnWhite(hex) >= min) return hex;
  const [L, a, b] = toLab(rgbOf(hex).map(lin) as RGB);
  const c = Math.hypot(a, b),
    h = Math.atan2(b, a);
  let lo = 0,
    hi = L;
  for (let i = 0; i < 30; i++) {
    const mid = (lo + hi) / 2;
    if (contrastOnWhite(lch(mid, c, h)) >= min) lo = mid;
    else hi = mid;
  }
  return lch(lo, c, h);
}

/** The colour that reads as `hex` after invert(1). */
export const inverted = (hex: string) => hexOf(rgbOf(hex).map((v) => 1 - v) as RGB);

/** What the page hands the fluid for a heading in `hex` on paper (the light theme). */
export const paperSource = (hex: string) => inverted(paperInk(hex));
