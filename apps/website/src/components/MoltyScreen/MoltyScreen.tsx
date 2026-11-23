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
import { afterTitle, heroCue, together } from "../../features/home/intro";
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
 *  What it plays unseen first (the cat getting up) it plays while the device turns in, so the
 *  picture is ready when the device stands.
 *  Without JavaScript, with reduced motion, or if the renderer cannot load, a still of the cat
 *  stands in. `data-screen` says which: "off" (the device is away), "dark", "live", "still".
 *  `enter`: the cat is not there at first: it walks in from beyond the window's right edge,
 *  across the page, through the rim, and sits (once; outside the glass a clear canvas carries
 *  the same renderer's picture of the cat alone).
 *  `boot`: the device's boot screen first. `play`: what it does by itself (features/molty).
 *  `scenes`: it may be asked for scenes. `onReady` hands the running screen over (null when it
 *  goes); `onAnswer` and `onFrame` are the screen's own. Take b of an intro preview (`?intro=b`,
 *  features/home/intro.ts TOGETHER): the hero's renderer mounts as soon as it is near, and its
 *  clock starts as the title starts forming, so the cat walks in (outside the glass) while the
 *  device turns; the screen still comes on once it stands. */
export function MoltyScreen({
  reveal,
  boot = false,
  enter = false,
  scenes = false,
  play,
  onReady,
  onAnswer,
  onFrame,
}: {
  reveal: RevealKind;
  boot?: boolean;
  enter?: boolean;
  scenes?: boolean;
  play?: ProgramName;
  onReady?: (screen: MoltyScreenHandle | null) => void;
  onAnswer?: (index: number) => void;
  onFrame?: (seconds: number) => void;
}) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const outside = useRef<HTMLCanvasElement>(null);
  const [entered, setEntered] = useState(false);
  const power = useRef<() => void>(() => {});
  const callbacks = useRef({ onReady, onAnswer, onFrame });
  callbacks.current = { onReady, onAnswer, onFrame };
  const state = useRef({ turning: false, stands: false, live: false, failed: false });
  const [screen, setScreen] = useState<"off" | "dark" | "live" | "still">();
  const [one, setOne] = useState(false); // take b's: the hero and the title as one
  const show = () => {
    const s = state.current;
    // waiting for the renderer the screen is dark (the still would be shown, then hidden by the
    // boot); with reduced motion nothing boots, so the still holds until the picture replaces it
    const waiting = matchMedia("(prefers-reduced-motion: reduce)").matches ? "still" : "dark";
    const now = !s.stands ? "off" : s.live ? "live" : s.failed ? "still" : waiting;
    setScreen(now);
    if (reveal === "arrive" && s.stands) heroCue(now);
  };

  useEffect(() => {
    const reducedMotion = matchMedia("(prefers-reduced-motion: reduce)").matches;
    const needs = { creature: CAT, scenes: scenes || play === "chart" ? SCENES_URL : undefined };
    const early = reveal === "arrive" ? together() : undefined;
    setOne(!!early);
    let handle: MoltyScreenHandle | null = null;
    let program: ProgramRun | null = null;
    let alive = true;
    let near = false;
    let started = false;
    let stood = () => {};
    const shown = early ? afterTitle(0) : new Promise<void>((done) => (stood = done));
    power.current = () => {
      if (state.current.stands) stood();
      if (started || !near || !(early || state.current.turning || state.current.stands)) return;
      started = true;
      const mount = mountMoltyScreen(canvas.current!, {
        ...needs,
        boot,
        shown,
        entrance: enter ? { outside: outside.current, pace: early?.walk } : undefined,
        reducedMotion,
        onEntered: () => alive && setEntered(true),
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
    <span
      className={styles.root}
      data-molty-screen=""
      data-dust-focus="device"
      data-screen={screen}
      data-together={one || undefined}
    >
      {enter && (
        <canvas
          ref={outside}
          className={styles.outside}
          data-outside=""
          data-gone={entered || undefined}
          width={PANEL}
          height={PANEL}
        />
      )}
      <span className={styles.panel} />
      <img className={styles.still} src={CAT_STILL} alt="" width={PANEL} height={PANEL} />
      <canvas ref={canvas} className={styles.canvas} width={PANEL} height={PANEL} />
      <Device
        reveal={reveal}
        onTurn={() => {
          if (reveal === "arrive") heroCue("turning");
          state.current.turning = true;
          power.current();
        }}
        onScreen={(on) => {
          state.current.stands = on;
          show();
          if (on) power.current();
        }}
      />
    </span>
  );
}
