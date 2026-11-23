// The ink screen (a concept: no ink board exists in the firmware): a square 1-bit e-paper panel
// showing the round screen's live picture, its middle square cut out, shrunk to the panel's dots
// and each dot inked or left as paper. It changes a few times a second, as e-paper does.

export const INK_SCREEN = {
  /** The panel's dots a side (a 1.54 inch e-paper square). */
  px: 200,
  /** The square of the round screen's 466 px picture it shows: left, top, side. */
  crop: [58, 50, 350] as const,
  /** A dot brighter than this on the screen is inked (0 to 255, its light). */
  threshold: 70,
  /** The ink's grey, and the seconds between two pictures. */
  ink: 34,
  every: 0.3,
} as const;

/** RGBA pixels as the ink screen shows them, in place: a dot is inked where the picture is lit
 *  (its light over `threshold`) and paper (white, the page's paper colour multiplies it) where
 *  it is dark. Opaque either way. */
export function toInk(rgba: Uint8ClampedArray, threshold: number = INK_SCREEN.threshold): Uint8ClampedArray {
  for (let i = 0; i < rgba.length; i += 4) {
    const light = (0.3 * rgba[i]! + 0.59 * rgba[i + 1]! + 0.11 * rgba[i + 2]!) * (rgba[i + 3]! / 255);
    rgba[i] = rgba[i + 1] = rgba[i + 2] = light > threshold ? INK_SCREEN.ink : 255;
    rgba[i + 3] = 255;
  }
  return rgba;
}
