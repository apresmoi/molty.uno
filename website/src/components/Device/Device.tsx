import { useEffect, useMemo, useRef, useState } from "react";
import { PolyCamera, PolyMesh, PolyScene, useCameraContext, type CameraHandle } from "@layoutit/polycss-react";
import { PUCK, puckPolygons, type PuckColors } from "../../features/device/puck";
import {
  REST,
  REVEAL_SECONDS,
  glareOf,
  revealFrame,
  type RevealFrame,
  type RevealKind,
} from "../../features/device/reveal";
import { GlassDome } from "../GlassDome";
import styles from "./Device.module.css";

// key light from the upper left, a little in front (polycss world: surface → light, +X is the
// face), as the bezel's lit lip and the glass's highlight
const LIGHT = { direction: [0.55, -0.45, 0.7] as [number, number, number], intensity: 1.6 };
const AMBIENT = { intensity: 1.1 };
type Camera = { handle: CameraHandle; apply: () => void };

/** The wall's colours from the theme's tokens (polycss takes hex only). */
function readColors(): PuckColors {
  const css = getComputedStyle(document.documentElement);
  const read = (k: string) => css.getPropertyValue(k).trim() || "#222222";
  return { wall: read("--m-device-wall"), back: read("--m-device-back") };
}

/** Hands the polycss camera up once it is mounted, so a turn is one scene-root transform. */
function CameraLink({ onCamera }: { onCamera: (c: Camera) => void }) {
  const { cameraRef, applyTransformDirect } = useCameraContext();
  useEffect(() => onCamera({ handle: cameraRef.current, apply: applyTransformDirect }), []);
  return null;
}

/** `?reveal=<0…1>` freezes every reveal at that progress (screenshots of the mid-turn). */
const frozenAt = () => {
  const v = new URLSearchParams(location.search).get("reveal");
  return v === null ? null : Math.min(1, Math.max(0, Number(v) || 0));
};
const toggle = (el: HTMLElement, name: string, on: boolean) => {
  if (el.hasAttribute(name) !== on) el.toggleAttribute(name, on);
};
const GLARE_VARS = ["--glare-x", "--glare-y", "--glare-squeeze", "--glare-angle", "--glare-fade"];

/** A round screen's device: the bezel (a rounded, lit lip and a gasket), the screen and its
 *  domed glass, a contact shadow. At rest it is flat CSS (face on, nothing 3D, no layers of its
 *  own). The first time it is well in view it turns into place (`reveal`): only for those
 *  ~1.2 s is the board's wall a polycss scene (the face layer turns with the same orthographic
 *  rotation, the glass's reflections slide against the turn), then the scene is unmounted and
 *  the device is flat again. `replay` changing turns it in again (quickly). It never takes the
 *  pointer. In a page-fluid Molty slot the Molty waits for the turn (data-fluid-hold) and
 *  turns with it (data-fluid-yaw). */
export function Device({ reveal, replay }: { reveal: RevealKind; replay?: string }) {
  const root = useRef<HTMLSpanElement>(null);
  const face = useRef<HTMLSpanElement>(null);
  const lid = useRef<HTMLSpanElement>(null);
  const cover = useRef<HTMLSpanElement>(null);
  const camera = useRef<Camera | null>(null);
  const unit = useRef(0); // CSS px per world unit: the screen's radius
  const pose = useRef<RevealFrame>(REST);
  const raf = useRef(0);
  const seen = useRef(false);
  const queued = useRef<RevealKind | null>(null); // waits for the polycss scene to mount
  const [turning, setTurning] = useState(false);
  // a device that arrives (starts unseen) is rendered unseen already (the HTML, before any JS),
  // so nothing settled flashes before it turns in: CSS hides it (motion allowed) and shows it
  // anyway if no script takes over in a few seconds
  const [armed, setArmed] = useState(() => revealFrame(reveal, 0).opacity < 1);

  /** Put the whole device in pose `f`: a few style writes, one of them the polycss scene root. */
  const apply = (f: RevealFrame) => {
    pose.current = f;
    const el = root.current;
    if (!el) return;
    const rest = f.yaw === 0 && f.pitch === 0 && f.lid === 0;
    const c = camera.current;
    if (c) {
      c.handle.update({ rotX: 90 + f.pitch, rotY: f.yaw });
      c.apply();
    }
    el.style.opacity = f.opacity === 1 ? "" : String(f.opacity);
    el.style.transform = f.lift ? `translateY(${(f.lift * 2 * unit.current).toFixed(2)}px)` : "";
    const fc = face.current!;
    // the bezel, the screen and the glass ride the board's front plane: the camera's rotation
    fc.style.transform = rest
      ? ""
      : `rotateX(${90 + f.pitch}deg) rotateZ(${f.yaw}deg) rotateX(-90deg) translateZ(${(PUCK.front * unit.current).toFixed(2)}px)`;
    if (rest) for (const v of GLARE_VARS) fc.style.removeProperty(v);
    else {
      const g = glareOf(f.yaw, f.pitch);
      fc.style.setProperty("--glare-x", g.dx.toFixed(4));
      fc.style.setProperty("--glare-y", g.dy.toFixed(4));
      fc.style.setProperty("--glare-squeeze", (g.squeeze / g.restSqueeze).toFixed(3));
      fc.style.setProperty("--glare-angle", `${((g.angle * 180) / Math.PI).toFixed(1)}deg`);
      fc.style.setProperty("--glare-fade", g.fade.toFixed(3));
    }
    lid.current!.style.transform = f.lid ? `perspective(${(unit.current * 8).toFixed(0)}px) rotateX(${f.lid}deg)` : "";
    cover.current!.style.opacity = f.cover ? String(f.cover) : "";
    // attributes only on a change: each write is a mutation (pageFluid observes data-fluid-hold)
    toggle(el, "data-screen-off", f.cover > 0.5);
    const slot = el.closest<HTMLElement>("[data-fluid-molty]");
    if (slot) {
      toggle(slot, "data-fluid-hold", f.hold);
      // the rig's yaw turns the face toward screen-right; the board's toward screen-left
      if (f.moltyYaw) slot.dataset.fluidYaw = ((-f.moltyYaw * Math.PI) / 180).toFixed(3);
      else delete slot.dataset.fluidYaw;
    }
  };

  /** Back to rest at once and flat again: no scene, no frame clock, no composited layers. */
  const finish = () => {
    cancelAnimationFrame(raf.current);
    queued.current = null;
    apply(REST);
    if (root.current) toggle(root.current, "data-turning", false);
    camera.current = null;
    setTurning(false);
  };

  /** The frame clock of one reveal (the scene is mounted): stops at rest. */
  const run = (kind: RevealKind) => {
    const frozen = frozenAt();
    if (frozen !== null) return apply(revealFrame(kind, frozen));
    const ms = REVEAL_SECONDS[kind] * 1000;
    let t0 = -1;
    const step = (now: number) => {
      if (t0 < 0) t0 = now;
      const u = (now - t0) / ms;
      if (u >= 1) return finish();
      apply(revealFrame(kind, u));
      raf.current = requestAnimationFrame(step);
    };
    raf.current = requestAnimationFrame(step);
  };

  /** Turn in: mount the wall's scene (the clock starts once it is there), start pose now. */
  const play = (kind: RevealKind) => {
    cancelAnimationFrame(raf.current);
    unit.current = root.current!.clientWidth / 2;
    apply(revealFrame(kind, frozenAt() ?? 0));
    if (frozenAt() === 1) return finish();
    toggle(root.current!, "data-turning", true);
    if (camera.current) return run(kind);
    queued.current = kind;
    setTurning(true);
  };

  // the size: polycss zoom is CSS px per world unit (the screen's radius)
  useEffect(() => {
    const el = root.current!;
    const ro = new ResizeObserver(() => {
      unit.current = el.clientWidth / 2;
      camera.current?.handle.update({ zoom: unit.current });
      apply(pose.current);
    });
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  // the first sight: armed in the start pose (no scene yet), it turns in when it is well in
  // view; one that leaves the view mid-turn is put to rest at once (no reveal runs unseen)
  useEffect(() => {
    const el = root.current!;
    setArmed(false); // from here the pose is ours (inline), or at rest
    if (matchMedia("(prefers-reduced-motion: reduce)").matches) {
      seen.current = true;
      return;
    }
    apply(revealFrame(reveal, 0));
    const io = new IntersectionObserver(
      (entries) => {
        const e = entries[entries.length - 1]!;
        if (!seen.current && e.intersectionRatio >= 0.45) {
          seen.current = true;
          play(reveal);
        } else if (seen.current && !e.isIntersecting && el.hasAttribute("data-turning")) finish();
      },
      { threshold: [0, 0.45] },
    );
    io.observe(el);
    const slot = el.closest<HTMLElement>("[data-fluid-molty]");
    return () => {
      io.disconnect();
      cancelAnimationFrame(raf.current);
      // never leave the slot's Molty held
      delete slot?.dataset.fluidHold;
      delete slot?.dataset.fluidYaw;
    };
  }, []);

  // a new pick (the showcase): turn in again, quickly
  const first = useRef(true);
  useEffect(() => {
    if (first.current) return void (first.current = false);
    if (!seen.current || matchMedia("(prefers-reduced-motion: reduce)").matches) return;
    play("quick");
  }, [replay]);

  // the wall's polygons, in the theme's colours of the moment it turns (the back plate only
  // for a board that spins past edge-on: a coin)
  const polygons = useMemo(() => (turning ? puckPolygons(readColors(), { back: reveal === "coin" }) : null), [turning]);

  return (
    <span ref={root} className={styles.device} data-device={reveal} data-armed={armed || undefined} aria-hidden="true">
      <span className={styles.shadow} />
      {polygons && (
        <span className={styles.body}>
          <PolyCamera rotX={90 + pose.current.pitch} rotY={pose.current.yaw} zoom={unit.current || 1}>
            <CameraLink
              onCamera={(c) => {
                camera.current = c;
                apply(pose.current);
                const k = queued.current;
                queued.current = null;
                if (k) run(k);
              }}
            />
            <PolyScene directionalLight={LIGHT} ambientLight={AMBIENT}>
              <PolyMesh polygons={polygons} merge={false} />
            </PolyScene>
          </PolyCamera>
        </span>
      )}
      <span ref={face} className={styles.face}>
        <span className={styles.bezel}>
          <span className={styles.lip} />
        </span>
        <span ref={cover} className={styles.cover} />
        <span ref={lid} className={styles.lid}>
          <GlassDome />
        </span>
      </span>
    </span>
  );
}
