// A device below the fold brings its picture with it: it turns in only once its screen has
// settled (MoltyScreen prepares it before it is seen), so the picture rides the turning face
// from the first frame. When the picture cannot be ready in time it holds, unseen in its start
// pose, at most HOLD_S; then it turns in all the same, with its screen dark until the picture
// comes (the way every device came in before).

/** The longest a device waits for its picture once it is well in view (seconds). */
export const HOLD_S = 2.5;

/** `go` runs once: when the device is wanted (well in view) and its picture is ready, or HOLD_S
 *  after it was wanted if the picture is still not ready. */
export function holdTurn(go: () => void) {
  let wanted = false;
  let ready = false;
  let done = false;
  let cap: ReturnType<typeof setTimeout> | undefined;
  const check = () => {
    if (done || !wanted || !ready) return;
    done = true;
    clearTimeout(cap);
    go();
  };
  return {
    want() {
      if (wanted) return;
      wanted = true;
      cap = setTimeout(() => {
        ready = true;
        check();
      }, HOLD_S * 1000);
      check();
    },
    ready() {
      ready = true;
      check();
    },
    cancel() {
      clearTimeout(cap);
      done = true;
    },
  };
}
