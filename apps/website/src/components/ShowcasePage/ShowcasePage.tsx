import type { ReactNode } from "react";
import { SHOWCASE_INK } from "../../features/showcase/content";
import { REPO_URL } from "../../config/site.mjs";
import styles from "./ShowcasePage.module.css";

/** /showcase/: one big stage in the page fluid (the `stage` island: slot, actions, names)
 *  under a heading the fluid draws in the stage Molty's colour. Reads without the engine:
 *  the heading keeps its text, the slot shows the Molty's still. */
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
          data-fluid-tint="showcase"
          data-fluid-delay="0.3"
        >
          showcase
        </h1>
        <p className={styles.text} data-fluid-quiet="">
          Every Molty and everything it can do. Pick one, tap an action: it plays right here, in the fluid.
        </p>
        {stage}
      </section>

      <footer className={styles.footer}>
        <a href="/#moltys">[ ← moltys ]</a>
        <a href="/moltys/making-your-own/">[ make your own → ]</a>
        <a href="/moltys/format/">[ the .molty format → ]</a>
        <a href={REPO_URL} target="_blank" rel="noopener noreferrer">
          [ source ↗ ]
        </a>
      </footer>
    </div>
  );
}
