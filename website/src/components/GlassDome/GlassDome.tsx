import styles from "./GlassDome.module.css";

/** The device's domed glass shell over a round screen: one layer of gradients, no filters, no
 *  motion. It fills its positioned parent and never takes the pointer, so taps and the
 *  fluid's ripples go through to the screen under it. */
export function GlassDome() {
  return <span className={styles.dome} aria-hidden="true" />;
}
