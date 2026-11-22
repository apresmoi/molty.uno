// One clock for every water on the page: the screens and the page's own field take the same
// frames (the display's, at most 60 a second, none while the tab is hidden), in one order, so a
// wave is at the same moment on both sides of a rim. The screens go first: each meets the
// page's water as it is, then takes its frame; the page's water takes the frame last.

/** The page's frame rate. The device paces its own frames at 33 a second (mole_fluid.c). */
const FPS = 60;
export const FRAME_MS = 1000 / FPS;
/** The longest step the firmware takes for one frame (mole_fluid.c clamps the same). */
const MAX_STEP_MS = 100;

/** A frame: `ms` since the last one (a first one, or one after a pause: one frame's). */
type Frame = (ms: number) => void;

const frames: Array<{ frame: Frame; last: boolean }> = [];
let raf = 0;
let then = 0;

function tick(now: number) {
  raf = frames.length ? requestAnimationFrame(tick) : 0;
  const dt = then ? now - then : FRAME_MS;
  if (dt < FRAME_MS - 2) return; // a faster display: every other frame of it
  then = now;
  const ms = Math.min(dt, MAX_STEP_MS);
  for (const f of [...frames]) f.frame(ms);
  if (!frames.length) then = 0;
}

/** Take the page's frames from now on (`last`: after every screen, the page's own water).
 *  Returns how to stop. */
export function onFrame(frame: Frame, last = false): () => void {
  const entry = { frame, last };
  if (last) frames.push(entry);
  else frames.splice(frames.findLastIndex((f) => !f.last) + 1, 0, entry);
  if (!raf) {
    then = 0;
    raf = requestAnimationFrame(tick);
  }
  return () => {
    const i = frames.indexOf(entry);
    if (i >= 0) frames.splice(i, 1);
  };
}
