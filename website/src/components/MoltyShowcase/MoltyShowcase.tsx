import { useEffect, useMemo, useRef, useState } from "react";
import type { Molty } from "../../features/moltys/catalog";
import { AUTOPLAY_GAP, AUTOPLAY_PER_MOLTY, readShowcaseUrl, writeShowcaseUrl } from "../../features/showcase/content";
import {
  ACT_EVENT,
  actionsOf,
  actValue,
  type ActDetail,
  type MoltyAction,
  type MoltyActions,
} from "../../services/molty-stage";
import { Device } from "../Device";
import styles from "./MoltyShowcase.module.css";

type Phase = "idle" | "asked" | "playing";

const all = (a: MoltyActions) => [...a.moods, ...a.own, ...a.glass];
/** A beat wins over a mood of the same name (the URL carries names only). */
const find = (a: MoltyActions, name: string) => [...a.own, ...a.glass, ...a.moods].find((x) => x.name === name);
const pick = <T,>(xs: T[], not?: (x: T) => boolean) => {
  const pool = not ? xs.filter((x) => !not(x)) : xs;
  const from = pool.length ? pool : xs;
  return from[Math.floor(Math.random() * from.length)]!;
};
const short = (m: Molty) => m.name.toLowerCase();

/** The showcase stage: the chosen Molty lives in the page fluid (one big slot) and plays any
 *  of its actions on demand: the page writes data-fluid-act on the slot, pageFluid plays it
 *  and answers with ACT_EVENT (start / end). URL: ?m=<id>&a=<action>. */
export function MoltyShowcase({ moltys, initial }: { moltys: Molty[]; initial: string }) {
  const [molty, setMolty] = useState(moltys.find((m) => m.id === initial) ?? moltys[0]!);
  const [act, setAct] = useState(""); // the slot's data-fluid-act
  const [asked, setAsked] = useState<MoltyAction | null>(null);
  const [phase, setPhase] = useState<Phase>("idle");
  const [auto, setAuto] = useState(false);
  const slot = useRef<HTMLDivElement>(null);
  const n = useRef(0);
  const actRef = useRef("");
  const steps = useRef(0);
  const actions = useMemo(() => actionsOf(molty.id), [molty.id]);

  const sync = (m: Molty, a: MoltyAction | null) =>
    history.replaceState(
      history.state,
      "",
      location.pathname + writeShowcaseUrl(location.search, { molty: m.id, action: a?.name ?? null }) + location.hash,
    );

  /** Bring the stage back on screen when a tap far below it changed it (phones). */
  const reveal = () => {
    const r = slot.current?.getBoundingClientRect();
    if (r && (r.bottom < r.height * 0.5 || r.top > innerHeight - r.height * 0.5))
      slot.current!.scrollIntoView({ block: "center", behavior: "smooth" });
  };

  const ask = (m: Molty, a: MoltyAction) => {
    const value = actValue(a, ++n.current);
    actRef.current = value;
    setMolty(m);
    setAct(value);
    setAsked(a);
    setPhase("asked");
    sync(m, a);
  };

  const choose = (m: Molty) => {
    actRef.current = "";
    setMolty(m);
    setAct("");
    setAsked(null);
    setPhase("idle");
    sync(m, null);
  };

  const step = () => {
    steps.current++;
    if (steps.current % AUTOPLAY_PER_MOLTY === 0) {
      const next = pick(moltys, (m) => m.id === molty.id);
      ask(next, pick(all(actionsOf(next.id))));
    } else
      ask(
        molty,
        pick(all(actions), (a) => a.name === asked?.name),
      );
  };

  // a link opens a Molty doing an action: ?m=<id>&a=<action> (malformed or unknown → ignored)
  useEffect(() => {
    const url = readShowcaseUrl(location.search);
    const m = moltys.find((x) => x.id === url.molty);
    if (!m) return;
    const a = url.action ? find(actionsOf(m.id), url.action) : undefined;
    if (a) ask(m, a);
    else choose(m);
  }, []);

  // the slot answers: start / end of what was asked last
  useEffect(() => {
    const el = slot.current;
    if (!el) return;
    const on = (e: Event) => {
      const d = (e as CustomEvent<ActDetail>).detail;
      if (d.act !== actRef.current) return;
      setPhase(d.phase === "start" ? "playing" : "idle");
    };
    el.addEventListener(ACT_EVENT, on);
    return () => el.removeEventListener(ACT_EVENT, on);
  }, []);

  // autoplay: the next action a moment after one ends; never stuck if the slot stays silent
  // (no engine, or the Molty still forming)
  useEffect(() => {
    if (!auto) return;
    const wait = phase === "idle" ? AUTOPLAY_GAP : phase === "asked" ? 6 : (asked?.len ?? 4) + 3;
    const id = setTimeout(() => (phase === "idle" ? step() : setPhase("idle")), wait * 1000);
    return () => clearTimeout(id);
  }, [auto, phase, act]);

  const playing = phase !== "idle" ? asked : null;
  const button = (a: MoltyAction) => (
    <li key={`${a.kind}:${a.name}`}>
      <button
        type="button"
        aria-pressed={playing?.kind === a.kind && playing.name === a.name}
        onClick={() => {
          ask(molty, a);
          reveal();
        }}
      >
        {a.name}
      </button>
    </li>
  );
  const group = (label: string, list: MoltyAction[]) =>
    list.length > 0 && (
      <div className={styles.group}>
        <h3>{label}</h3>
        <ul className={styles.acts}>{list.map(button)}</ul>
      </div>
    );

  return (
    <div className={styles.root}>
      <div
        ref={slot}
        className={styles.stage}
        data-fluid-molty={molty.id}
        data-fluid-slot="showcase"
        data-fluid-act={act}
        role="img"
        aria-label={`${molty.name}, emerging from the fluid`}
      >
        <img className={styles.still} src={molty.thumb} alt="" width={150} height={150} />
        <Device reveal="turn" replay={molty.id} />
      </div>

      <div className={styles.panel} data-fluid-quiet="">
        <p className={styles.now} aria-live="polite">
          <b>{short(molty)}</b>
          <span>{playing ? (phase === "asked" ? `· ${playing.name} …` : `· ${playing.name}`) : "· dancing"}</span>
        </p>
        <div className={styles.play}>
          <button
            type="button"
            onClick={() => {
              ask(
                molty,
                pick(all(actions), (a) => a.name === asked?.name),
              );
              reveal();
            }}
          >
            random
          </button>
          <button type="button" aria-pressed={auto} onClick={() => setAuto((x) => !x)}>
            autoplay: {auto ? "on" : "off"}
          </button>
        </div>
        {group("moods", actions.moods)}
        {group(`${short(molty)}'s own`, actions.own)}
        {group("at the glass", actions.glass)}
        <p className={styles.file}>
          <a href={molty.file} download={molty.fileName}>
            [ {molty.fileName} ↓ ]
          </a>
        </p>
      </div>

      <div className={styles.names} data-fluid-quiet="">
        <h3>the {moltys.length} moltys</h3>
        <ul aria-label={`The ${moltys.length} Moltys`}>
          {moltys.map((m) => (
            <li key={m.id}>
              <button
                type="button"
                aria-pressed={m.id === molty.id}
                onClick={() => {
                  choose(m);
                  reveal();
                }}
              >
                {short(m)}
              </button>
            </li>
          ))}
        </ul>
      </div>
    </div>
  );
}
