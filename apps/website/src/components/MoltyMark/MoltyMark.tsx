import styles from "./MoltyMark.module.css";

/** The molty.UNO mark: two square dots on one diagonal, gliding to the other and back. */
export function MoltyMark({ className }: { className?: string }) {
  return (
    <svg className={className ? `${styles.mark} ${className}` : styles.mark} viewBox="0 0 10 10" aria-hidden="true">
      <rect className={styles.left} x="0" y="6" width="4" height="4" />
      <rect className={styles.right} x="6" y="0" width="4" height="4" />
    </svg>
  );
}
