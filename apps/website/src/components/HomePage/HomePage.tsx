import type { ReactNode } from "react";
import {
  WORDMARK,
  FLUID_INK,
  CAT_INK,
  BRIDGE_INK,
  BAKE_INK,
  bridgeFlow,
  boards,
  PORTING_HREF,
} from "../../features/home/content";
import { REPO_URL } from "../../config/site.mjs";
import { MoltyMark } from "../MoltyMark";
import styles from "./HomePage.module.css";

/** A heading the fluid draws: its text condenses out of the fluid while on screen. The DOM
 *  text stays (selectable, read aloud) and turns transparent once the fluid is live. */
function Word({ id, children, color }: { id?: string; children: string; color: string }) {
  return (
    <h2 id={id} className={styles.word} data-fluid-word="" data-fluid-color={color}>
      {children}
    </h2>
  );
}

/** A square for a Molty device (the island the page passes in): decoration, to tap. */
function Slot({ device }: { device: ReactNode }) {
  return (
    <div className={styles.slot} aria-hidden="true">
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
  /** The three Molty devices (MoltyScreen islands: they turn in on first sight, then power on). */
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
          <div className={styles.heroSlot} aria-hidden="true">
            {heroDevice}
          </div>
        </div>
      </section>

      <section className={`${styles.section} ${styles.reading}`} aria-labelledby="talk">
        <Word id="talk" color={FLUID_INK}>
          talk
        </Word>
        <p className={styles.text} data-fluid-quiet="">
          <span className={styles.wide}>
            Your agent's words, one at a time, condensing out of the fluid. It asks, Molty listens, and yes, no or idk
            is one tap on the glass.
          </span>
          <span className={styles.phone}>
            Your agent's words condense out of the fluid. Molty asks; yes, no or idk is one tap on the glass.
          </span>
        </p>
        {reader}
        <Links>
          <a href="/getting-started/on-screen/">[ what molty shows → ]</a>
          <a href="/architecture/">[ how it renders → ]</a>
        </Links>
      </section>

      <section className={`${styles.section} ${styles.gallery}`} aria-labelledby="moltys">
        <Word id="moltys" color={CAT_INK}>
          moltys
        </Word>
        <p className={styles.text} data-fluid-quiet="">
          <span className={styles.wide}>
            The first Molty is a cat. This is the device's own drawing code, running here. Tap the glass and it comes to
            you. More creatures are coming.
          </span>
          <span className={styles.phone}>
            The first Molty is a cat, drawn by the device's own code. Tap the glass. More creatures are coming.
          </span>
        </p>
        {picker}
        <Links>
          <a href="/showcase/">[ everything it does → ]</a>
          <a href="/moltys/creature-packages/">[ creature packages → ]</a>
          <a className={styles.wide} href="/moltys/packages/">
            [ packages &amp; scenes → ]
          </a>
        </Links>
      </section>

      <section className={`${styles.section} ${styles.split} ${styles.flip}`} aria-labelledby="bridge">
        <div className={styles.copy}>
          <Word id="bridge" color={BRIDGE_INK}>
            bridge
          </Word>
          <p className={styles.text} data-fluid-quiet="">
            Moltys have no brain of their own. Any agent becomes one: Claude, Grok, OpenClaw, anything that speaks MCP
            or HTTP.
          </p>
          <pre
            className={`${styles.flow} ${styles.wide}`}
            data-fluid-quiet=""
            aria-label="Your agent talks MCP or REST to the relay; Molty dials out to the relay over WSS."
          >
            {bridgeFlow}
          </pre>
          <p className={`${styles.text} ${styles.wide}`} data-fluid-quiet="">
            One message. One tap on the glass.
          </p>
          {message}
          <Links>
            <a href="/agents/connect/">[ connect an agent → ]</a>
            <a className={styles.wide} href="/agents/protocol/">
              [ protocol → ]
            </a>
            <a className={styles.wide} href="/agents/cli/">
              [ local MCP → ]
            </a>
          </Links>
        </div>
        <Slot device={bridgeDevice} />
      </section>

      <section className={`${styles.section} ${styles.split}`} aria-labelledby="boards">
        <div className={styles.copy}>
          <Word id="boards" color={BAKE_INK}>
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
            <a href="/build/#waitlist">[ get one built → ]</a>
            <a href="/build/">[ build your own → ]</a>
            <a className={styles.wide} href="/getting-started/hardware/">
              [ build from source → ]
            </a>
          </Links>
        </div>
        <Slot device={bakeDevice} />
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
