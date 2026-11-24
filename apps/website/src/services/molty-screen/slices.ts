// Work played unseen (a word settling, the cat getting up before it walks in) is done a little in
// each of the page's frames instead of in one go, so the page never stops for it. Each frame's
// share goes to what the visitor is looking at: the job whose canvas is nearest the middle of the
// view, in view before out of view, and before either the work a device in view already waits for
// (it holds its turn in until its picture is ready); work for a canvas farther than a viewport
// out waits until it comes back. A job's steps are the same and in the same order, so what it ends on is what it
// ended on in one go; only later.

/** A frame's share (ms): at least one step a frame, then more while one more fits. */
const BUDGET_MS = 8;
/** Work for a canvas this many viewports out of view waits. */
const FAR = 1;

/** Which goes first (the lower): in view, its middle's distance from the view's middle (under 1
 *  viewport); out of view, 1 and its distance from the view; far, Infinity (it waits). */
export function rank(box: { top: number; bottom: number }, view: number): number {
  const out = Math.max(box.top - view, -box.bottom, 0) / view;
  if (out > FAR) return Infinity;
  return out > 0 ? 1 + out : Math.min(1, Math.abs((box.top + box.bottom - view) / 2) / view);
}

type Job = {
  work: Iterator<unknown>;
  place: Element;
  waited?: () => boolean;
  done: () => void;
  failed: (e: unknown) => void;
};
const jobs: Job[] = [];
let raf = 0;
// a canvas coming back near wakes the queue (it sleeps while all its work waits)
let near: IntersectionObserver | null = null;
const wake = () => (raf ||= requestAnimationFrame(run));

function run() {
  raf = 0;
  const view = innerHeight;
  const order = new Map(
    jobs.map((job) => {
      const r = rank(job.place.getBoundingClientRect(), view);
      return [job, r < Infinity && job.waited?.() ? r - 2 : r];
    }),
  );
  jobs.sort((a, b) => order.get(a)! - order.get(b)! || 0); // stable: ties keep their turn
  const start = performance.now();
  let now = start;
  let step = 0;
  // room for another step like the last
  while (jobs.length && order.get(jobs[0]!)! < Infinity && now - start + step <= BUDGET_MS) {
    const job = jobs[0]!;
    try {
      if (job.work.next().done) end(job).done();
    } catch (e) {
      end(job).failed(e);
    }
    step = performance.now() - now;
    now += step;
  }
  if (jobs.some((job) => order.get(job)! < Infinity)) wake();
}

function end(job: Job) {
  jobs.splice(jobs.indexOf(job), 1);
  near?.unobserve(job.place);
  return job;
}

/** Play `work` (a generator, a step a yield) over the page's frames, in turn with the others by
 *  where `place` (its canvas) is, first while `waited` says its device waits for it; settles
 *  when it is done. */
export function sliced(work: Iterable<unknown>, place: Element, waited?: () => boolean): Promise<void> {
  return new Promise((done, failed) => {
    jobs.push({ work: work[Symbol.iterator](), place, waited, done, failed });
    near ??= new IntersectionObserver(wake, { rootMargin: `${FAR * 100}% 0px` });
    near.observe(place);
    wake();
  });
}
