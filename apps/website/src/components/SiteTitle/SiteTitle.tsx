import { MoltyMark } from "../MoltyMark";
import styles from "./SiteTitle.module.css";
export function SiteTitle() {
  return (
    <a href="/" className={styles.title} aria-label="molty.UNO home">
      <MoltyMark />
      <span className={styles.name}>
        molty.<b>UNO</b>
      </span>
    </a>
  );
}
