// Work played unseen (a word settling, the cat getting up before it walks in) is done a little in
// each of the page's frames instead of in one go, so the page never stops for it: the jobs take
// turns, a step each, until a frame's share of time is spent. The steps are the same and in the
// same order, so what a job ends on is what it ended on in one go; only later.

/** A frame's share (ms): at least one step a frame, then more while one more fits. */
const BUDGET_MS = 8;

const jobs: Array<{ work: Iterator<unknown>; done: () => void; failed: (e: unknown) => void }> = [];
let raf = 0;

function run() {
  const start = performance.now();
  let now = start;
  let step = 0;
  do {
    const job = jobs.shift()!;
    try {
      if (job.work.next().done) job.done();
      else jobs.push(job);
    } catch (e) {
      job.failed(e);
    }
    step = performance.now() - now;
    now += step;
  } while (jobs.length && now - start + step <= BUDGET_MS); // room for another step like the last
  raf = jobs.length ? requestAnimationFrame(run) : 0;
}

/** Play `work` (a generator, a step a yield) over the page's frames; settles when it is done. */
export function sliced(work: Iterable<unknown>): Promise<void> {
  return new Promise((done, failed) => {
    jobs.push({ work: work[Symbol.iterator](), done, failed });
    raf ||= requestAnimationFrame(run);
  });
}
