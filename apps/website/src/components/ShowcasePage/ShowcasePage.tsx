import type { ReactNode } from "react";
import { REPO_URL } from "../../config/site.mjs";
import { WordHeading } from "../WordHeading";
import styles from "./ShowcasePage.module.css";

/** /showcase/: one big stage (the `stage` island: the cat's screen and its actions) under a
 *  heading the device's renderer draws (the `word` island). Reads without it: the heading keeps
 *  its text, the screen shows the cat's still. */
export function ShowcasePage({ fluid, word, stage }: { fluid: ReactNode; word: ReactNode; stage: ReactNode }) {
  return (
    <div className={styles.root} data-water="">
      {fluid}

      <section className={styles.show} aria-labelledby="showcase">
        <WordHeading as="h1" id="showcase" text="showcase" word={word} className={styles.word} />
        <p className={styles.text}>
          The cat, the first Molty, and everything it can do. Tap an action: the device's own code plays it right here.
          Tap the glass too.
        </p>
        {stage}
      </section>

      <footer className={styles.footer}>
        <a href="/#moltys">[ ← moltys ]</a>
        <a href="/moltys/creature-packages/">[ creature packages → ]</a>
        <a href="/moltys/packages/">[ packages &amp; scenes → ]</a>
        <a href={REPO_URL} target="_blank" rel="noopener noreferrer">
          [ source ↗ ]
        </a>
      </footer>
    </div>
  );
}
