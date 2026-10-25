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

/** A square where one Molty emerges from the page fluid (still portrait until it can). */
function Slot({ molty, className }: { molty: string; className?: string }) {
  const src = still(molty.split(",")[0]!);
  return (
    <div className={`${styles.slot} ${className ?? ""}`} data-fluid-molty={molty} aria-hidden="true">
      {src && <img className={styles.still} src={src} alt="" width={150} height={150} />}
    </div>
  );
}

function Links({ children }: { children: ReactNode }) {
  return <p className={styles.links}>{children}</p>;
}

export function HomePage({ fluid, picker, message }: { fluid: ReactNode; picker: ReactNode; message: ReactNode }) {
  return (
    <div className={styles.root}>
      {fluid}

      <section className={styles.hero} aria-label="molty.UNO">
        <h1 className={styles.mark} data-fluid-word="" data-fluid-color={WORDMARK} data-fluid-delay="0.5">
          molty.UNO
        </h1>
        <div className={styles.heroBody}>
          <div className={styles.heroCopy} data-fluid-quiet="">
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
          <div
            className={styles.heroSlot}
            data-fluid-molty={HERO_MOLTYS.join(",")}
            data-fluid-every="12"
            data-fluid-delay="2.4"
            aria-hidden="true"
          >
            <img className={styles.still} src={still(HERO_MOLTYS[0]!)} alt="" width={150} height={150} />
          </div>
        </div>
        <p className={styles.three} data-fluid-quiet="">
          <a href="#fluid">01 fluid</a>
          <a href="#moltys">02 moltys</a>
          <a href="#bridge">03 bridge</a>
          <a href="#boards">04 bake</a>
        </p>
      </section>

      <section className={styles.section} aria-labelledby="fluid">
        <p className={styles.num}>01 · the rendering</p>
        <Word id="fluid" color={FLUID_INK}>
          fluid
        </Word>
        <p className={styles.text} data-fluid-quiet="">
          One fluid of braille fills the screen. Words, creatures and ripples are shapes it flows toward, never painted
          on top. This page is that fluid.
        </p>
        <div className={styles.pool} data-fluid-pool="">
          <span>touch it</span>
        </div>
        <Links>
          <a href="/architecture/">[ how it renders → ]</a>
        </Links>
      </section>

      <section className={styles.section} aria-labelledby="moltys">
        <p className={styles.num}>02 · the creatures</p>
        <Word id="moltys" color={WORDMARK} tint="gallery">
          moltys
        </Word>
        <p className={styles.text} data-fluid-quiet="">
          A Molty is a ~1 KB <code>.molty</code> file the fluid grows a creature from. Same file, same creature, any
          screen.
        </p>
        {picker}
        <Links>
          <a href="/moltys/making-your-own/">[ make your own → ]</a>
          <a href="/moltys/format/">[ the .molty format → ]</a>
        </Links>
      </section>

      <section className={`${styles.section} ${styles.split} ${styles.flip}`} aria-labelledby="bridge">
        <div className={styles.copy}>
          <p className={styles.num}>03 · the bridge</p>
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
        <Slot molty={BRIDGE_MOLTY.id} />
      </section>

      <section className={`${styles.section} ${styles.split}`} aria-labelledby="boards">
        <div className={styles.copy}>
          <p className={styles.num}>04 · bake it</p>
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
        <Slot molty={BAKE_MOLTY.id} />
      </section>

      <footer className={styles.footer}>
        <span>⠶ molty.UNO</span>
        <span>MIT</span>
        <a href="/getting-started/introduction/">docs</a>
        <a href="/llms.txt">llms.txt</a>
        <a href={REPO_URL} target="_blank" rel="noopener noreferrer">
          source ↗
        </a>
      </footer>
    </div>
  );
}
