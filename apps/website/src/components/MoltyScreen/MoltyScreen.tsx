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
 *  The device is one thing: nothing of its screen shows until its frame stands in place. Then
 *  the screen comes on inside it: dark first, the renderer's picture as soon as it draws (it
 *  mounts when the device nears the viewport, and its clock starts when the device stands).
 *  Without JavaScript, with reduced motion, or if the renderer cannot load, a still of the cat
 *  stands in. `data-screen` says which: "off" (the device is away), "dark", "live", "still".
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
  const power = useRef<() => void>(() => {});
  const callbacks = useRef({ onReady, onAnswer, onFrame });
  callbacks.current = { onReady, onAnswer, onFrame };
  const state = useRef({ stands: false, live: false, failed: false });
  const [screen, setScreen] = useState<"off" | "dark" | "live" | "still">();
  const show = () => {
    const s = state.current;
    // waiting for the renderer the screen is dark (the still would be shown, then hidden by the
    // boot); with reduced motion nothing boots, so the still holds until the picture replaces it
    const waiting = matchMedia("(prefers-reduced-motion: reduce)").matches ? "still" : "dark";
    setScreen(!s.stands ? "off" : s.live ? "live" : s.failed ? "still" : waiting);
  };

  useEffect(() => {
    const reducedMotion = matchMedia("(prefers-reduced-motion: reduce)").matches;
    const needs = { creature: CAT, scenes: scenes || play === "chart" ? SCENES_URL : undefined };
    let handle: MoltyScreenHandle | null = null;
    let program: ProgramRun | null = null;
    let alive = true;
    let near = false;
    let started = false;
    power.current = () => {
      if (started || !near || !state.current.stands) return;
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
          handle = s;
          state.current.live = true;
          show();
          callbacks.current.onReady?.(s);
          if (p) program = runProgram(p, s);
          if (reducedMotion) program?.frame(Infinity); // no clock: its one thing, as a still
        },
        () => {
          // the still stands in (the service marked the page)
          state.current.failed = true;
          show();
        },
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
      if (handle) callbacks.current.onReady?.(null);
      handle?.destroy();
    };
  }, []);

  return (
    <span className={styles.root} data-molty-screen="" data-screen={screen}>
      <span className={styles.panel} />
      <img className={styles.still} src={CAT_STILL} alt="" width={PANEL} height={PANEL} />
      <canvas ref={canvas} className={styles.canvas} width={PANEL} height={PANEL} />
      <Device
        reveal={reveal}
        onScreen={(on) => {
          state.current.stands = on;
          show();
          if (on) power.current();
        }}
      />
    </span>
  );
}
