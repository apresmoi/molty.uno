import type { ReactNode } from "react";
import { SHOWCASE_INK } from "../../features/showcase/content";
import { REPO_URL } from "../../config/site.mjs";
import styles from "./ShowcasePage.module.css";

/** /showcase/: one big stage (the `stage` island: the cat's screen and its actions) under a
 *  heading the fluid draws in the cat's colour. Reads without the engine: the heading keeps its
 *  text, the screen shows the cat's still. */
export function ShowcasePage({ fluid, stage }: { fluid: ReactNode; stage: ReactNode }) {
  return (
    <div className={styles.root}>
      {fluid}

      <section className={styles.show} aria-labelledby="showcase">
        <h1
          id="showcase"
          className={styles.word}
          data-fluid-word=""
          data-fluid-color={SHOWCASE_INK}
          data-fluid-delay="0.3"
        >
          showcase
        </h1>
        <p className={styles.text} data-fluid-quiet="">
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
