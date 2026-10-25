import { useEffect, useState } from "react";
import styles from "./ThemeToggle.module.css";

type Theme = "dark" | "light";
/** Same storage key and <html data-theme> as Starlight's own picker, so docs and landing agree. */
const KEY = "starlight-theme";

export function ThemeToggle() {
  const [theme, setTheme] = useState<Theme | null>(null);
  useEffect(() => setTheme(document.documentElement.dataset.theme === "light" ? "light" : "dark"), []);
  const next: Theme = theme === "light" ? "dark" : "light";
  const flip = () => {
    document.documentElement.dataset.theme = next;
    try {
      localStorage.setItem(KEY, next);
    } catch {
      // private mode: the choice lasts for this page only
    }
    setTheme(next);
  };
  return (
    <button type="button" className={styles.toggle} onClick={flip} aria-label={`Switch to ${next} theme`}>
      [ {theme ? next : "theme"} ]
    </button>
  );
}
