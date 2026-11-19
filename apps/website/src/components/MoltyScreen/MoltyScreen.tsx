import { useEffect, useRef, useState } from "react";
import {
  PANEL,
  mountMoltyScreen,
  runProgram,
  warmMoltyScreen,
  type MoltyScreenHandle,
  type ProgramRun,
} from "../../services/molty-screen";
import { CAT, CAT_STILL, PROGRAMS, SCENES_URL, type ProgramName } from "../../features/molty/content";
import type { RevealKind } from "../../features/device/reveal";
import { Device } from "../Device";
import styles from "./MoltyScreen.module.css";

/** A Molty device on the page: the round screen runs the firmware's own renderer (the cat, as
 *  the device draws it), in its bezel and glass (Device). It fills the square its parent gives
 *  it. The pointer is the finger on the glass: a tap ripples the water and the cat comes to the
 *  front, a tap on a question's answer or a scene's spot does what it does on the device.
 *
 *  It mounts when it nears the viewport and powers on once its device has turned in; until it
 *  draws (and without JavaScript, or if the renderer cannot load) a still of the cat stands in.
 *  `boot`: the device's boot screen first. `play`: what it does by itself (features/molty).
 *  `scenes`: it may be asked for scenes. `onReady` hands the running screen over (null when it
 *  goes); `onAnswer` and `onFrame` are the screen's own. */
export function MoltyScreen({
  reveal,
  boot = false,
  scenes = false,
  play,
  onReady,
  onAnswer,
  onFrame,
}: {
  reveal: RevealKind;
  boot?: boolean;
  scenes?: boolean;
  play?: ProgramName;
  onReady?: (screen: MoltyScreenHandle | null) => void;
  onAnswer?: (index: number) => void;
  onFrame?: (seconds: number) => void;
}) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const lit = useRef(true); // the device's screen: off through its turn
  const power = useRef<() => void>(() => {});
  const callbacks = useRef({ onReady, onAnswer, onFrame });
  callbacks.current = { onReady, onAnswer, onFrame };
  const [live, setLive] = useState(false);

  useEffect(() => {
    const reducedMotion = matchMedia("(prefers-reduced-motion: reduce)").matches;
    const needs = { creature: CAT, scenes: scenes || play === "chart" ? SCENES_URL : undefined };
    let screen: MoltyScreenHandle | null = null;
    let program: ProgramRun | null = null;
    let alive = true;
    let near = false;
    let started = false;
    power.current = () => {
      if (started || !near || !lit.current) return;
      started = true;
      const mount = mountMoltyScreen(canvas.current!, {
        ...needs,
        boot,
        reducedMotion,
        onAnswer: (i) => {
          program?.answer(i);
          callbacks.current.onAnswer?.(i);
        },
        onFrame: (t) => {
          program?.frame(t);
          callbacks.current.onFrame?.(t);
        },
      });
      Promise.all([mount, play ? PROGRAMS[play]() : null]).then(
        ([s, p]) => {
          if (!alive) return s.destroy();
          screen = s;
          setLive(true);
          callbacks.current.onReady?.(s);
          if (p) program = runProgram(p, s);
          if (reducedMotion) program?.frame(Infinity); // no clock: its one thing, as a still
        },
        () => {}, // the still stays (the service marked the page)
      );
    };
    const watch = new IntersectionObserver(
      (entries) => {
        if (!entries[entries.length - 1]!.isIntersecting) return;
        near = true;
        warmMoltyScreen(needs);
        power.current();
      },
      { rootMargin: "60% 0px" },
    );
    watch.observe(canvas.current!);
    return () => {
      alive = false;
      watch.disconnect();
      power.current = () => {};
      if (screen) callbacks.current.onReady?.(null);
      screen?.destroy();
    };
  }, []);

  return (
    <span className={styles.root} data-molty-screen="" data-live={live || undefined}>
      <img className={styles.still} src={CAT_STILL} alt="" width={PANEL} height={PANEL} />
      <canvas ref={canvas} className={styles.canvas} width={PANEL} height={PANEL} />
      <Device
        reveal={reveal}
        onScreen={(on) => {
          lit.current = on;
          if (on) power.current();
        }}
      />
    </span>
  );
}
