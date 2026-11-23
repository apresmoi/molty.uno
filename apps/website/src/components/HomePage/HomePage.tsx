import type { ReactNode } from "react";
import { bridgeFlow, boards, inkConcept, PORTING_HREF } from "../../features/home/content";
import { REPO_URL } from "../../config/site.mjs";
import { MoltyMark } from "../MoltyMark";
import { WordHeading } from "../WordHeading";
import styles from "./HomePage.module.css";

/** A square for a Molty device (the island the page passes in): decoration, to tap. `fan`: bake's
 *  row of three instead (wider than tall). */
function Slot({ device, fan = false }: { device: ReactNode; fan?: boolean }) {
  return (
    <div className={`${styles.slot} ${fan ? styles.fan : ""}`} aria-hidden="true">
      {device}
    </div>
  );
}

function Links({ children }: { children: ReactNode }) {
  return <p className={styles.links}>{children}</p>;
}

export function HomePage({
  fluid,
  title,
  wordTalk,
  wordMoltys,
  wordBridge,
  wordBoards,
  reader,
  picker,
  message,
  heroDevice,
  bridgeDevice,
  bakeDevice,
}: {
  /** The page's water (PageWater) and its headings' words (MoltyWord islands). */
  fluid: ReactNode;
  title: ReactNode;
  wordTalk: ReactNode;
  wordMoltys: ReactNode;
  wordBridge: ReactNode;
  wordBoards: ReactNode;
  reader: ReactNode;
  picker: ReactNode;
  message: ReactNode;
  /** The Molty devices (MoltyScreen islands: they turn in on first sight, then power on): the
   *  hero's, bridge's and bake's three (BakeFan). */
  heroDevice: ReactNode;
  bridgeDevice: ReactNode;
  bakeDevice: ReactNode;
}) {
  return (
    <div className={styles.root} data-water="">
      {fluid}

      <section className={styles.hero} aria-label="molty.UNO">
        <WordHeading as="h1" text="molty.UNO" word={title} className={styles.mark} />
        <div className={styles.heroBody}>
          <div className={styles.heroCopy}>
            <p className={styles.lede}>
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
        <WordHeading id="talk" text="talk" word={wordTalk} className={styles.word} />
        <p className={styles.text}>
          <span className={styles.wide}>
            Your agent's words, one at a time, on the glass. It asks, Molty listens, and yes, no or idk is one tap on
            the glass.
          </span>
          <span className={styles.phone}>
            Your agent's words, one at a time. Molty asks; yes, no or idk is one tap on the glass.
          </span>
        </p>
        {reader}
        <Links>
          <a href="/getting-started/on-screen/">[ what molty shows → ]</a>
          <a href="/architecture/">[ how it renders → ]</a>
        </Links>
      </section>

      <section className={`${styles.section} ${styles.gallery}`} aria-labelledby="moltys">
        <WordHeading id="moltys" text="moltys" word={wordMoltys} className={styles.word} />
        <p className={styles.text}>
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
          <WordHeading id="bridge" text="bridge" word={wordBridge} className={styles.word} />
          <p className={styles.text}>
            Moltys have no brain of their own. Any agent becomes one: Claude, Grok, OpenClaw, anything that speaks MCP
            or HTTP.
          </p>
          <pre
            className={`${styles.flow} ${styles.wide}`}
            aria-label="Your agent talks MCP or REST to the relay; Molty dials out to the relay over WSS."
          >
            {bridgeFlow}
          </pre>
          <p className={`${styles.text} ${styles.wide}`}>One message. One tap on the glass.</p>
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
          <WordHeading id="boards" text="bake" word={wordBoards} className={styles.word} />
          <p className={styles.text}>
            <span className={styles.wide}>
              One creature, any screen. Flash it onto a round screen, or port it to yours: the same cat, drawn for each.
            </span>
            <span className={styles.phone}>
              One creature, any screen. Flash it onto a round one, or port it to yours.
            </span>
          </p>
          <ul className={styles.boards}>
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
              <b>{inkConcept.tag}</b>
              <span>
                {inkConcept.screen}
                <small>{inkConcept.board}</small>
              </span>
              <i className={styles.concept}>{inkConcept.status}</i>
            </li>
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
        <Slot device={bakeDevice} fan />
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
