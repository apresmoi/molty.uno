import type { ReactNode } from "react";
import {
  WORDMARK,
  FLUID_INK,
  HERO_MOLTYS,
  BRIDGE_MOLTY,
  BAKE_MOLTY,
  bridgeFlow,
  boards,
  PORTING_HREF,
} from "../../features/home/content";
import { MOLTYS } from "../../features/moltys/catalog";
import { REPO_URL } from "../../config/site.mjs";
import { MoltyMark } from "../MoltyMark";
import styles from "./HomePage.module.css";

const still = (id: string) => MOLTYS.find((m) => m.id === id)?.thumb;

/** A heading the fluid draws: its text condenses out of the fluid while on screen. The DOM
 *  text stays (selectable, read aloud) and turns transparent once the fluid is live. */
function Word({ id, children, color, tint }: { id?: string; children: string; color: string; tint?: string }) {
  return (
    <h2 id={id} className={styles.word} data-fluid-word="" data-fluid-color={color} data-fluid-tint={tint}>
      {children}
    </h2>
  );
}

/** A square where one Molty emerges from the page fluid (still portrait until it can), inside
 *  its device (the island the page passes in). */
function Slot({ molty, device, className }: { molty: string; device: ReactNode; className?: string }) {
  const src = still(molty.split(",")[0]!);
  return (
    <div className={`${styles.slot} ${className ?? ""}`} data-fluid-molty={molty} aria-hidden="true">
      {src && <img className={styles.still} src={src} alt="" width={150} height={150} />}
      {device}
    </div>
  );
}

function Links({ children }: { children: ReactNode }) {
  return <p className={styles.links}>{children}</p>;
}

export function HomePage({
  fluid,
  reader,
  picker,
  message,
  heroDevice,
  bridgeDevice,
  bakeDevice,
}: {
  fluid: ReactNode;
  reader: ReactNode;
  picker: ReactNode;
  message: ReactNode;
  /** The devices around the three page slots (Device islands: they turn in on first sight). */
  heroDevice: ReactNode;
  bridgeDevice: ReactNode;
  bakeDevice: ReactNode;
}) {
  return (
    <div className={styles.root}>
      {fluid}

      <section className={styles.hero} aria-label="molty.UNO">
        <h1 className={styles.mark} data-fluid-word="" data-fluid-color={WORDMARK} data-fluid-delay="0.5">
          molty.UNO
        </h1>
        <div className={styles.heroBody}>
          <div className={styles.heroCopy}>
            <p className={styles.lede} data-fluid-quiet="">
              Liquid ASCII creatures.
              <br />
              Any agent is the brain.
              <br />
              <span>Bake them onto your own screen.</span>
            </p>
            <Links>
              <a href="/setup/">[ setup ]</a>
              <a href="/getting-started/introduction/">[ docs ]</a>
              <a href={REPO_URL} target="_blank" rel="noopener noreferrer">
                [ source ↗ ]
              </a>
            </Links>
          </div>
          <div
            className={styles.heroSlot}
            data-fluid-molty={HERO_MOLTYS.join(",")}
            data-fluid-every="12"
            data-fluid-delay="0.5"
            data-fluid-run-in={HERO_MOLTYS[0]}
            aria-hidden="true"
          >
            <img className={styles.still} src={still(HERO_MOLTYS[0]!)} alt="" width={150} height={150} />
            {heroDevice}
          </div>
        </div>
      </section>

      <section className={`${styles.section} ${styles.reading}`} aria-labelledby="talk">
        <Word id="talk" color={FLUID_INK}>
          talk
        </Word>
        <p className={styles.text} data-fluid-quiet="">
          Your agent's words, one at a time, condensing out of the fluid. It asks, Molty listens, and yes, no or idk is
          one tap on the glass.
        </p>
        {reader}
        <Links>
          <a href="/architecture/">[ how it renders → ]</a>
        </Links>
      </section>

      <section className={`${styles.section} ${styles.gallery}`} aria-labelledby="moltys">
        <Word id="moltys" color={WORDMARK} tint="gallery">
          moltys
        </Word>
        <p className={styles.text} data-fluid-quiet="">
          A Molty is a ~1 KB <code>.molty</code> file the fluid grows a creature from. Same file, same creature, any
          screen.
        </p>
        {picker}
        <Links>
          <a href="/showcase/">[ all moltys &amp; actions → ]</a>
          <a href="/moltys/making-your-own/">[ make your own → ]</a>
          <a href="/moltys/format/">[ the .molty format → ]</a>
        </Links>
      </section>

      <section className={`${styles.section} ${styles.split} ${styles.flip}`} aria-labelledby="bridge">
        <div className={styles.copy}>
          <Word id="bridge" color={BRIDGE_MOLTY.color}>
            bridge
          </Word>
          <p className={styles.text} data-fluid-quiet="">
            Moltys have no brain of their own. Any agent becomes one: Claude, Grok, OpenClaw, anything that speaks MCP
            or HTTP.
          </p>
          <pre
            className={styles.flow}
            data-fluid-quiet=""
            aria-label="Your agent talks MCP or REST to the relay; Molty dials out to the relay over WSS."
          >
            {bridgeFlow}
          </pre>
          <p className={styles.text} data-fluid-quiet="">
            One message. One tap on the glass.
          </p>
          {message}
          <Links>
            <a href="/agents/connect/">[ connect an agent → ]</a>
            <a href="/agents/protocol/">[ protocol → ]</a>
            <a href="/agents/cli/">[ local MCP → ]</a>
          </Links>
        </div>
        <Slot molty={BRIDGE_MOLTY.id} device={bridgeDevice} />
      </section>

      <section className={`${styles.section} ${styles.split}`} aria-labelledby="boards">
        <div className={styles.copy}>
          <Word id="boards" color={BAKE_MOLTY.color}>
            bake
          </Word>
          <p className={styles.text} data-fluid-quiet="">
            Flash it onto a round screen. Or port it to yours.
          </p>
          <ul className={styles.boards} data-fluid-quiet="">
            {boards.map((b) => (
              <li key={b.tag}>
                <b>{b.tag}</b>
                <span>
                  {b.screen}
                  <small>{b.board}</small>
                </span>
                <i>{b.status}</i>
              </li>
            ))}
            <li>
              <b>yours</b>
              <span>
                any screen
                <small>a display, a driver, the fluid</small>
              </span>
              <a href={PORTING_HREF}>port it →</a>
            </li>
          </ul>
          <Links>
            <a href="/setup/">[ flash from the browser → ]</a>
            <a href="/getting-started/hardware/">[ build from source → ]</a>
          </Links>
        </div>
        <Slot molty={BAKE_MOLTY.id} device={bakeDevice} />
      </section>

      <footer className={styles.footer}>
        <span className={styles.brand}>
          <MoltyMark /> molty.UNO
        </span>
        <span>MIT</span>
        <a href="/getting-started/introduction/">docs</a>
        <a href="/llms.txt">llms.txt</a>
        <a href={REPO_URL} target="_blank" rel="noopener noreferrer">
          source ↗
        </a>
        <a className={styles.credit} href="https://ledeluge.me" target="_blank" rel="noopener noreferrer">
          created by ledeluge.me
        </a>
      </footer>
    </div>
  );
}
