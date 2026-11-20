import { useEffect, useRef, useState, type ReactNode } from "react";
import {
  PANEL,
  mountMoltySim,
  type LabSwitch,
  type MoltySim,
  type MoltyState,
  type SimEvent,
  type SimSpeed,
  type SimStats,
} from "../../services/molty-screen";
import { CAT, SCENES_URL } from "../../features/molty/content";
import { madeUpVoice } from "../../features/molty/actions";
import {
  AGENT_MOODS,
  ASPECT_KINDS,
  CHART_DATE,
  LAB_LABELS,
  PROGRESS_STEP,
  QUESTIONS,
  SAY_TEXT,
  WPM,
  sceneTtl,
} from "../../features/sim/controls";
import { loadSky, utc, type Show } from "../../features/sim/astro";
import styles from "./SimPage.module.css";

export interface SimPageProps {
  /** The cat's moods and gestures (packages/cat/cat.json). */
  moods: string[];
  gestures: string[];
  /** The firmware the module was built from: its commit, and the module's own hash. */
  firmware: string;
}

const SIGNS = [
  "aries",
  "taurus",
  "gemini",
  "cancer",
  "leo",
  "virgo",
  "libra",
  "scorpio",
  "sagittarius",
  "capricorn",
  "aquarius",
  "pisces",
];
const EVENT_KINDS = ["transit", "occultation", "appulse", "solar-eclipse", "lunar-eclipse"];
const LOG_MAX = 200;

function Group({ title, children }: { title: string; children: ReactNode }) {
  return (
    <fieldset className={styles.group}>
      <legend>{title}</legend>
      {children}
    </fieldset>
  );
}
function Row({ label, children }: { label?: string; children: ReactNode }) {
  return (
    <div className={styles.row}>
      {label && <span className={styles.label}>{label}</span>}
      {children}
    </div>
  );
}
function Btn({ onClick, children, on }: { onClick: () => void; children: ReactNode; on?: boolean }) {
  return (
    <button type="button" className={on ? styles.on : undefined} onClick={onClick}>
      {children}
    </button>
  );
}
/** A button held like the device's: down while pressed. */
function Hold({ onDown, onUp, children }: { onDown: () => void; onUp: () => void; children: ReactNode }) {
  return (
    <button
      type="button"
      onPointerDown={(e) => {
        e.currentTarget.setPointerCapture(e.pointerId);
        onDown();
      }}
      onPointerUp={onUp}
      onPointerCancel={onUp}
    >
      {children}
    </button>
  );
}

const ms = (v: number) => v.toFixed(1);

/** /sim/: the whole device, its controls grouped as it is used, what an agent hears. */
export function SimPage({ moods, gestures, firmware }: SimPageProps) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const sim = useRef<MoltySim | null>(null);
  const [error, setError] = useState("");
  const [zoom, setZoom] = useState(false);
  const [speed, setSpeed] = useState<SimSpeed>("browser");
  const [stats, setStats] = useState<SimStats | null>(null);
  const [state, setState] = useState<MoltyState | null>(null);
  const [log, setLog] = useState<SimEvent[]>([]);
  const [lab, setLab] = useState<LabSwitch[]>([]);
  const [voice, setVoice] = useState(0.5);
  const [speaking, setSpeaking] = useState(false);
  const [text, setText] = useState(SAY_TEXT);
  const [wpm, setWpm] = useState(WPM.def);
  const [pct, setPct] = useState(0);
  const [date, setDate] = useState(CHART_DATE);
  const [queue, setQueue] = useState(false);
  const [sign, setSign] = useState("leo");
  const [dnd, setDnd] = useState(false);
  const [note, setNote] = useState("");
  const statsAt = useRef(0);

  useEffect(() => {
    let gone = false;
    mountMoltySim(canvas.current!, {
      creature: CAT,
      scenes: SCENES_URL,
      onEvent: (e) => setLog((l) => [e, ...l].slice(0, LOG_MAX)),
      onStats: (s) => {
        const now = performance.now();
        if (now - statsAt.current < 250) return;
        statsAt.current = now;
        setStats(s);
        setState(sim.current?.core.read() ?? null);
      },
    })
      .then((s) => {
        if (gone) return s.destroy();
        sim.current = s;
        setLab(s.dev.lab());
        (window as unknown as { __sim: MoltySim }).__sim = s;
      })
      .catch((e: Error) => setError(e.message));
    return () => {
      gone = true;
      sim.current?.destroy();
    };
  }, []);

  // the voice: the slider's level, or a made-up voice while "speak" runs, ~10 a second as the mic's VAD
  useEffect(() => {
    const t0 = performance.now();
    const id = setInterval(() => {
      const v = speaking ? madeUpVoice((performance.now() - t0) / 1000) * voice * 1.6 : voice;
      sim.current?.core.level(Math.min(1, v));
    }, 100);
    const stop = speaking ? setTimeout(() => setSpeaking(false), 4000) : 0;
    return () => {
      clearInterval(id);
      clearTimeout(stop);
    };
  }, [speaking, voice]);

  const go = (f: (s: MoltySim) => unknown) => () => {
    if (sim.current) f(sim.current);
  };
  const show = async (make: (sky: Awaited<ReturnType<typeof loadSky>>) => Show) => {
    const s = sim.current;
    if (!s) return;
    try {
      const sky = await loadSky();
      const scenes = make(sky).scenes;
      scenes.forEach(([layout, params, ttl], i) =>
        s.dev.sceneOp(layout, params, ttl ?? (layout === "chart" ? 300 : sceneTtl(layout)), queue || i > 0),
      );
      setNote("");
    } catch (e) {
      setNote((e as Error).message);
    }
  };
  const when = () => utc(date);
  const tonightDate = () => new Date();

  const copy = async () => {
    const lines = [
      `molty sim, firmware ${firmware}`,
      `speed: ${speed === "device" ? "device (modelled)" : "browser"}`,
      `switches: ${lab.map((l) => `${l.name}=${l.value}`).join(", ") || "none"}`,
      `on screen: ${state ? `${state.state} ${state.mood}${state.scene?.layout ? ` ${state.scene.layout}` : ""}` : "-"}`,
      "last events (newest first):",
      ...log.slice(0, 20).map((e) => `${e.at.toFixed(2)}s ${e.name ?? e.t} ${JSON.stringify(e.data ?? {})}`),
    ].join("\n");
    try {
      await navigator.clipboard.writeText(lines);
      setNote("copied");
    } catch {
      setNote(lines);
    }
  };

  const scene = state?.scene as (MoltyState["scene"] & { ttl_left?: number }) | undefined;
  return (
    <div className={styles.root}>
      <header className={styles.head}>
        <h1>molty sim</h1>
        <p>The device's own code, here. Click the glass as you tap the device.</p>
      </header>

      <div className={styles.main}>
        <section className={styles.side}>
          <div className={styles.screen} data-zoom={zoom ? "2" : "1"}>
            <canvas ref={canvas} width={PANEL} height={PANEL} className={styles.glass} data-sim-glass="" />
          </div>
          {error && <p className={styles.error}>The screen could not start: {error}</p>}
          <Row>
            <Btn on={zoom} onClick={() => setZoom(!zoom)}>
              2x
            </Btn>
            <span className={styles.label}>speed</span>
            <Btn
              on={speed === "browser"}
              onClick={() => {
                setSpeed("browser");
                sim.current?.setSpeed("browser");
              }}
            >
              browser
            </Btn>
            <Btn
              on={speed === "device"}
              onClick={() => {
                setSpeed("device");
                sim.current?.setSpeed("device");
              }}
            >
              device (modelled)
            </Btn>
          </Row>
          <p className={styles.small}>
            {speed === "browser"
              ? "Browser: as fast as this computer runs it, the whole screen drawn each frame. Shows the look."
              : "Device (modelled): only what the device sends to its panel, each frame shown as long as the device would take for it. An estimate from the panel's measured costs."}
          </p>
          <p className={styles.readout} data-sim-readout="">
            {stats
              ? `${stats.fps.toFixed(1)} fps · frame ${ms(stats.p50)} / ${ms(stats.p95)} / ${ms(stats.worst)} ms (p50 / p95 / worst, ${speed === "device" ? "modelled" : "measured"}) · ${stats.last.windows} windows, ${(stats.last.pixels / 1000).toFixed(1)} kpx sent${speed === "device" ? ` · device work ${ms(stats.last.drawMs)} ms` : ""}`
              : "starting"}
          </p>
          <p className={styles.readout} data-sim-state="">
            {state
              ? `${state.creature_info?.name ?? "creature"} · ${state.state} · ${state.mood || "-"}${state.act ? ` · ${state.act}` : ""} · ${scene?.layout ? `${scene.layout}, ${scene.ttl_left ?? 0} s left` : "no scene"}`
              : "-"}
          </p>
        </section>

        <section className={styles.controls}>
          <Group title="creature">
            <Row label="mood">
              {[...moods, ...AGENT_MOODS].map((m) => (
                <Btn key={m} on={state?.mood === m} onClick={go((s) => s.core.state({ mood: m }))}>
                  {m}
                </Btn>
              ))}
            </Row>
            <Row label="gesture">
              {gestures.map((g) => (
                <Btn key={g} on={state?.act === g} onClick={go((s) => s.core.state({ act: g }))}>
                  {g}
                </Btn>
              ))}
              <Btn onClick={go((s) => s.dev.shakeImu())}>shake</Btn>
            </Row>
          </Group>

          <Group title="talk">
            <Row label="listen">
              <Btn onClick={go((s) => s.dev.talk(true))}>start</Btn>
              <Btn onClick={go((s) => s.dev.talk(false))}>stop</Btn>
              <Btn on={speaking} onClick={() => setSpeaking(true)}>
                speak
              </Btn>
              <label className={styles.label}>
                voice{" "}
                <input
                  type="range"
                  min={0}
                  max={1}
                  step={0.05}
                  value={voice}
                  onChange={(e) => setVoice(Number(e.target.value))}
                />
              </label>
            </Row>
            <Row label="say">
              <textarea rows={2} value={text} onChange={(e) => setText(e.target.value)} />
            </Row>
            <Row>
              <label className={styles.label}>
                words a minute{" "}
                <input
                  type="number"
                  min={WPM.lo}
                  max={WPM.hi}
                  value={wpm}
                  onChange={(e) => setWpm(Number(e.target.value))}
                />
              </label>
              <Btn onClick={go((s) => s.core.state({ state: "text", text, wpm }))}>say</Btn>
            </Row>
          </Group>

          <Group title="questions">
            <Row label="ask">
              {Object.entries(QUESTIONS).map(([name, q]) => (
                <Btn key={name} onClick={go((s) => s.dev.question(q()))}>
                  {name}
                </Btn>
              ))}
              <Btn onClick={go((s) => s.dev.questionEnd("cancelled"))}>cancel</Btn>
            </Row>
            <p className={styles.small}>Tap an answer on the glass. The answer shows in the log.</p>
          </Group>

          <Group title="progress">
            <Row>
              <Btn
                onClick={go((s) => {
                  const p = Math.min(100, pct + PROGRESS_STEP);
                  setPct(p);
                  s.dev.progress(p);
                })}
              >
                grow
              </Btn>
              <Btn onClick={go((s) => s.dev.progress(-1))}>no percent</Btn>
              <Btn
                onClick={go((s) => {
                  setPct(0);
                  s.dev.progress(100, true);
                })}
              >
                done
              </Btn>
              <Btn
                onClick={go((s) => {
                  setPct(0);
                  s.dev.progressStop();
                })}
              >
                stop
              </Btn>
              <span className={styles.label}>{pct}%</span>
            </Row>
            <p className={styles.small}>The install flow: questions, install, then tap install on the glass.</p>
          </Group>

          <Group title="scenes (astrology)">
            <Row label="date (UTC)">
              <input type="datetime-local" value={date} onChange={(e) => setDate(e.target.value)} />
              <label className={styles.label}>
                <input type="checkbox" checked={queue} onChange={(e) => setQueue(e.target.checked)} /> queue
              </label>
            </Row>
            <Row>
              <Btn onClick={() => show((k) => k.chart(when()))}>chart</Btn>
              <Btn onClick={() => show((k) => k.tonight(tonightDate()))}>tonight</Btn>
              <Btn onClick={() => show(() => ({ scenes: [["signs", {}]] }))}>signs</Btn>
              <Btn onClick={go((s) => s.core.sceneClear())}>clear</Btn>
            </Row>
            <Row label="aspect">
              {ASPECT_KINDS.map((k) => (
                <Btn key={k} onClick={() => show((x) => x.aspect(k, when()))}>
                  {k}
                </Btn>
              ))}
            </Row>
            <Row label="event">
              {EVENT_KINDS.map((k) => (
                <Btn key={k} onClick={() => show((x) => x.EVENTS[k](when()))}>
                  {k}
                </Btn>
              ))}
            </Row>
            <Row label="sign">
              <select value={sign} onChange={(e) => setSign(e.target.value)}>
                {SIGNS.map((n) => (
                  <option key={n}>{n}</option>
                ))}
              </select>
              <Btn onClick={() => show((x) => x.closeup(when(), sign))}>close-up</Btn>
            </Row>
          </Group>

          <Group title="device">
            <Row label="screen">
              <Btn onClick={go((s) => s.dev.screen(false))}>sleep</Btn>
              <Btn onClick={go((s) => s.dev.screen(true))}>wake</Btn>
              <Btn
                on={dnd}
                onClick={go((s) => {
                  s.dev.dnd(!dnd);
                  setDnd(!dnd);
                })}
              >
                do not disturb
              </Btn>
            </Row>
            <Row label="buttons">
              <Hold onDown={go((s) => s.dev.button(0, true))} onUp={go((s) => s.dev.button(0, false))}>
                accept
              </Hold>
              <Hold onDown={go((s) => s.dev.button(1, true))} onUp={go((s) => s.dev.button(1, false))}>
                reject
              </Hold>
              <Btn
                onClick={go(async (s) => {
                  setLog([]);
                  setDnd(false);
                  await s.boot();
                })}
              >
                restart
              </Btn>
            </Row>
            <p className={styles.small}>
              Hold accept to talk. Reject: a press turns the screen off or on, two do not disturb, a long hold says
              goodbye. As the device's buttons out of the box.
            </p>
          </Group>

          {lab.length > 0 && (
            <Group title="lab">
              {lab.map((l) => {
                const shown = LAB_LABELS[l.name];
                const set = (v: number) => {
                  sim.current?.setLab(l.index, v);
                  setLab((all) => all.map((x) => (x.index === l.index ? { ...x, value: v } : x)));
                };
                return (
                  <Row key={l.name} label={shown?.label ?? l.name}>
                    {shown ? (
                      shown.values.map(([v, name]) => (
                        <Btn key={v} on={l.value === v} onClick={() => set(v)}>
                          {name}
                        </Btn>
                      ))
                    ) : (
                      <input
                        type="number"
                        min={l.lo}
                        max={l.hi}
                        value={l.value}
                        onChange={(e) => set(Number(e.target.value))}
                      />
                    )}
                  </Row>
                );
              })}
            </Group>
          )}
        </section>
      </div>

      <section className={styles.log}>
        <Row label="what an agent hears (newest first)">
          <Btn onClick={copy}>copy this state as text</Btn>
          <Btn onClick={() => setLog([])}>clear log</Btn>
        </Row>
        {note && <pre className={styles.note}>{note}</pre>}
        <ol className={styles.events} data-sim-log="">
          {log.map((e, i) => (
            <li key={`${log.length - i}`}>
              <span className={styles.at}>{e.at.toFixed(2)} s</span> <b>{e.name ?? e.t}</b>{" "}
              <span>{JSON.stringify(e.data ?? Object.fromEntries(Object.entries(e).filter(([k]) => k !== "at")))}</span>
            </li>
          ))}
        </ol>
      </section>
    </div>
  );
}
