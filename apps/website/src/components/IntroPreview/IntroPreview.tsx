import { PACES, TITLE_INTROS, type IntroPreview as Preview } from "../../features/home/intro";
import styles from "./IntroPreview.module.css";

/** The intro preview's switches (only with `?intro=` or `pace=`): each take of the title, each
 *  pace, and the page's own ("now"), and a replay (the page loaded again, so all of it replays). */
export function IntroPreview({ preview }: { preview: Preview }) {
  const href = (key: "intro" | "pace", value: string | null) => {
    const q = new URLSearchParams(location.search);
    if (value) q.set(key, value);
    else q.delete(key);
    const s = q.toString();
    return `${location.pathname}${s ? `?${s}` : ""}`;
  };
  const pick = (key: "intro" | "pace", values: string[], on: string | undefined) => (
    <span>
      {key}
      {[...values, null].map((v) => (
        <a key={v ?? "now"} href={href(key, v)} aria-current={(v ?? undefined) === on || undefined}>
          {v ?? "now"}
        </a>
      ))}
    </span>
  );
  return (
    <nav className={styles.bar} aria-label="Intro preview">
      {pick("intro", Object.keys(TITLE_INTROS), preview.title)}
      {pick("pace", Object.keys(PACES), preview.pace)}
      <button type="button" onClick={() => location.reload()}>
        [ replay ]
      </button>
    </nav>
  );
}
