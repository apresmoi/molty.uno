import styles from "./MoltyMark.module.css";

/** The molty.UNO mark: two square dots on one diagonal, swapping to the other. */
export function MoltyMark({ className }: { className?: string }) {
  return (
    <svg className={className ? `${styles.mark} ${className}` : styles.mark} viewBox="0 0 10 10" aria-hidden="true">
      <g className={styles.rise}>
        <rect x="0" y="6" width="4" height="4" />
        <rect x="6" y="0" width="4" height="4" />
      </g>
      <g className={styles.fall}>
        <rect x="0" y="0" width="4" height="4" />
        <rect x="6" y="6" width="4" height="4" />
      </g>
    </svg>
  );
}
