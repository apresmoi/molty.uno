import type { ReactNode } from "react";
import {
  pillars,
  howItWorks,
  howNotes,
  pairingSteps,
  hardware,
  flashSteps,
  repoTree,
} from "../../features/home/content";
import { REPO_URL } from "../../config/site.mjs";
import styles from "./HomePage.module.css";

function SectionHead({ id, label, title }: { id?: string; label: string; title: string }) {
  return (
    <header className={styles.head} id={id}>
      <p className={styles.label}>{label}</p>
      <h2>{title}</h2>
    </header>
  );
}

export function HomePage({ hero, gallery, message }: { hero: ReactNode; gallery: ReactNode; message: ReactNode }) {
  return (
    <div className={styles.root}>
      <section className={styles.hero}>
        <div className={styles.heroCopy}>
          <p className={styles.label}>open source · MIT</p>
          <h1>
            A calm creature of characters <span>for your desk.</span>
          </h1>
          <p className={styles.lede}>
            A round screen of flowing braille. Little Moltys live in it. Your AI agent is its brain.
          </p>
          <div className={styles.actions}>
            <a className={styles.primary} href="/getting-started/introduction/">
              Get started
            </a>
            <a className={styles.ghost} href={REPO_URL} target="_blank" rel="noopener noreferrer">
              GitHub ↗
            </a>
          </div>
        </div>
        <div className={styles.heroScreen}>
          {hero}
          <p className={styles.note}>live · the real engine · tap the glass</p>
        </div>
      </section>

      <section className={styles.section} aria-labelledby="what">
        <SectionHead id="what" label="what it is" title="Not a toy. Not an assistant. A small presence." />
        <ul className={styles.pillars}>
          {pillars.map((p) => (
            <li key={p.title}>
              <span className={styles.glyph} aria-hidden="true">
                {p.glyph}
              </span>
              <h3>{p.title}</h3>
              <p>{p.text}</p>
            </li>
          ))}
        </ul>
      </section>

      <section className={styles.section} aria-labelledby="how">
        <SectionHead id="how" label="how it works" title="Molty is the body. Your agent is the brain." />
        <pre
          className={styles.diagram}
          aria-label="Your agent talks MCP or REST to the relay; Molty dials out to the relay over WSS."
        >
          {howItWorks}
        </pre>
        <ul className={styles.notes}>
          {howNotes.map(([lead, rest]) => (
            <li key={lead}>
              <b>{lead}</b> {rest}
            </li>
          ))}
        </ul>
      </section>

      <section className={styles.section} aria-labelledby="moltys">
        <SectionHead id="moltys" label="meet the moltys" title="Twenty-six of them. Pick one, or make your own." />
        {gallery}
        <p className={styles.more}>
          Each Molty is a ~1 KB <code>.molty</code> file the device loads at runtime — no reflash.{" "}
          <a href="/moltys/making-your-own/">Make your own →</a>
        </p>
      </section>

      <section className={styles.section} aria-labelledby="agents">
        <SectionHead id="agents" label="connect your agent" title="One message. One tap." />
        <div className={styles.pairing}>
          {message}
          <ol className={styles.steps}>
            {pairingSteps.map((s) => (
              <li key={s}>{s}</li>
            ))}
            <li className={styles.works}>
              Works with Claude, Grok, OpenClaw — anything that speaks MCP or HTTP.{" "}
              <a href="/agents/connect/">Details →</a>
            </li>
          </ol>
        </div>
      </section>

      <section className={styles.section} aria-labelledby="build">
        <SectionHead id="build" label="build your own" title="One board. One cable." />
        <div className={styles.build}>
          <dl className={styles.facts}>
            {hardware.map(([k, v]) => (
              <div key={k}>
                <dt>{k}</dt>
                <dd>{v}</dd>
              </div>
            ))}
          </dl>
          <pre className={styles.terminal}>{flashSteps}</pre>
        </div>
        <p className={styles.more}>
          <a href="/getting-started/hardware/">Hardware, build and flash →</a>
        </p>
      </section>

      <section className={styles.section} aria-labelledby="open">
        <SectionHead id="open" label="open source" title="All of it. MIT." />
        <pre className={styles.tree}>{repoTree}</pre>
        <p className={styles.more}>
          <a href={REPO_URL} target="_blank" rel="noopener noreferrer">
            Read the code on GitHub ↗
          </a>{" "}
          · <a href="/contributing/">Contributing →</a>
        </p>
      </section>

      <footer className={styles.footer}>
        <span>⠶ molty.uno</span>
        <span>MIT</span>
        <a href="/getting-started/introduction/">docs</a>
        <a href="/llms.txt">llms.txt</a>
        <a href={REPO_URL} target="_blank" rel="noopener noreferrer">
          github
        </a>
      </footer>
    </div>
  );
}
