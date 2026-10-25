import styles from "./SiteTitle.module.css";
export function SiteTitle() {
  return (
    <a href="/" className={styles.title} aria-label="Molty home">
      <span className={styles.mark} aria-hidden="true">
        ⠶
      </span>
      <span className={styles.name}>molty</span>
    </a>
  );
}
