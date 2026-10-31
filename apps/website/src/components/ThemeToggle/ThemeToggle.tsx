import { useEffect, useState } from "react";
import styles from "./ThemeToggle.module.css";

type Theme = "dark" | "light";
/** Same storage key and <html data-theme> as Starlight's own picker, so docs and landing agree. */
const KEY = "starlight-theme";

/** The visible label is CSS-driven off <html data-theme> (set by an inline head script before
 *  paint, here and in Starlight), so the first paint already names the theme a click switches
 *  to; React state only keeps the aria-label in step. */
export function ThemeToggle() {
  const [theme, setTheme] = useState<Theme | null>(null);
  useEffect(() => setTheme(document.documentElement.dataset.theme === "light" ? "light" : "dark"), []);
  const flip = () => {
    const next: Theme = document.documentElement.dataset.theme === "light" ? "dark" : "light";
    document.documentElement.dataset.theme = next;
    try {
      localStorage.setItem(KEY, next);
    } catch {
      // private mode: the choice lasts for this page only
    }
    setTheme(next);
  };
  return (
    <button
      type="button"
      className={styles.toggle}
      onClick={flip}
      aria-label={theme ? `Switch to ${theme === "light" ? "dark" : "light"} theme` : "Switch theme"}
    >
      [ <span className={styles.toLight}>light</span>
      <span className={styles.toDark}>dark</span> ]
    </button>
  );
}
