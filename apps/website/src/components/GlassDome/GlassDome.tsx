import styles from "./GlassDome.module.css";

/** The device's slightly domed glass over a round screen, lit like a watch crystal: a broad soft
 *  sheen and one crisp highlight (a window and a lamp up-left, where the bezel is lit from), a
 *  faint reflection along the opposite rim, darker toward the edge (fresnel). The reflections
 *  belong to the room, not the glass: a parent turning the device sets `--glare-x` / `--glare-y`
 *  (fractions of the diameter they slide by), `--glare-squeeze` (how much the dome's curve
 *  squeezes them toward the rim), `--glare-angle` and `--glare-fade`; unset, they rest. Static
 *  gradients only (no filters), and never the pointer: taps reach the screen under it. */
export function GlassDome() {
  return (
    <span className={styles.dome} aria-hidden="true">
      <span className={styles.sheen} />
      <span className={styles.spec} />
      <span className={styles.rim} />
    </span>
  );
}
