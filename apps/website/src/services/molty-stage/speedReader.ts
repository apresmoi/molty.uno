// The talk demo on molty's round screen, one fluid: @glyphcss/fluid/reader with
// `shape: "circle"` (molty's 52-column disc) and molty's reader rules (ORP focal letter,
// per-word delay by length and punctuation, shared with the firmware). Each word condenses
// out of the fluid with its ORP letter accented and lands with a soft ring; after the last
// word the letters melt back.
// When a question ends, the same fluid listens: "yes", "no" and "idk" condense as text
// targets in three 120° sectors, ringed by a halo inside the rim that wobbles each beat like a mic level
// (the device's `listening` mood); a tap ripples where it landed, melts the other word and
// keeps the chosen one a beat. The caller (SpeedReader) runs the conversation:
// say → listen → tap → say the answer.
// Runs only while on screen: offscreen the fluid is torn down, and the line being said
// starts over when it comes back.
import { GlyphFluidTargets, type GlyphFluidTarget } from "@glyphcss/fluid";
import {
  createGlyphFluidReader,
  glyphReaderWordDelay,
  tokenizeGlyphReaderText,
  type GlyphFluidReader,
} from "@glyphcss/fluid/reader";
import FluidWorker from "@glyphcss/fluid/worker?worker";

const FPS = 30;
const LEAD_IN = 550; // the reader's default lead-in, ms
const RING_MS = 760; // listening: one halo per beat
const HALO = "#3ccf96"; // the wordmark green: Molty is listening
/** The answers' fluid ink (the letters over it take the matching --m-yes/no/idk tokens). */
const CHOICE_INK: Record<"yes" | "no" | "idk", string> = { yes: "#3ccf96", no: "#ff6b6b", idk: "#ffa94d" };
const HALO_PX = 96; // the halo's canvas (the fluid area-averages it onto its cells)
const HALO_R = 0.43; // its radius, of the screen's width
const HOLD_MS = 650; // a tap keeps the chosen word this long before it melts

export type Choice = "yes" | "no" | "idk";
export const CHOICES: readonly Choice[] = ["yes", "no", "idk"];

export interface SpeedReaderHandle {
  /** Read `text` once; `done` fires after its last word. */
  say(text: string, done: () => void): void;
  setWpm(wpm: number): void;
  /** The choices condense in their sectors and it listens (halo), or they melt. */
  listen(on: boolean): void;
  /** A tap: a ripple at the point, the other words melt, then the chosen one. */
  tap(choice: Choice, clientX: number, clientY: number): void;
  destroy(): void;
}

export interface SpeedReaderHosts {
  reader: HTMLElement;
  /** The choices' words, laid out in their sectors (the fluid draws them). */
  choices: Record<Choice, HTMLElement>;
}

/** How long a reading takes (lead-in + every word's delay), for the still, engine-less demo. */
export function readingMs(text: string, wpm: number): number {
  return tokenizeGlyphReaderText(text).reduce((ms, t) => ms + glyphReaderWordDelay(t, wpm), LEAD_IN);
}

export async function mountSpeedReader(hosts: SpeedReaderHosts, wpm: number): Promise<SpeedReaderHandle> {
  return new Talk(hosts, wpm);
}

class Talk implements SpeedReaderHandle {
  private reader: GlyphFluidReader | null = null;
  private words: Record<Choice, GlyphFluidTarget> | null = null;
  private halo: GlyphFluidTarget | null = null;
  private observer: IntersectionObserver;
  private text = "";
  private done: (() => void) | null = null;
  private listening = false;
  private ring = 0;
  private hold = 0;
  private off: (() => void) | null = null;

  constructor(
    private hosts: SpeedReaderHosts,
    private wpm: number,
  ) {
    this.observer = new IntersectionObserver(([e]) => this.show(!!e?.isIntersecting), { threshold: 0.2 });
    // both screens sit in one figure: watch the reader's, they come and go together
    this.observer.observe(hosts.reader);
  }

  say(text: string, done: () => void) {
    this.text = text.trim() || this.text;
    this.done = done;
    if (!this.reader) return; // read when back on screen
    this.reader.clock.setText(this.text);
    this.reader.clock.play();
  }
  setWpm(wpm: number) {
    this.wpm = wpm;
    this.reader?.clock.setWpm(wpm); // takes effect from the next word
  }
  listen(on: boolean) {
    this.listening = on;
    clearTimeout(this.hold);
    this.applyListen();
  }
  tap(choice: Choice, x: number, y: number) {
    this.listening = false;
    clearInterval(this.ring);
    this.halo?.melt();
    this.reader?.fluid.impulse(x, y, { strength: 1.4, radius: 0.09 });
    for (const c of CHOICES) if (c !== choice) this.words?.[c].melt();
    clearTimeout(this.hold);
    this.hold = window.setTimeout(() => this.applyListen(), HOLD_MS);
  }
  destroy() {
    this.observer.disconnect();
    this.show(false);
  }

  private applyListen() {
    const on = this.listening;
    if (this.words) for (const w of Object.values(this.words)) on ? w.condense() : w.melt();
    if (this.halo) on ? this.halo.condense() : this.halo.melt();
    clearInterval(this.ring);
    if (!on) return;
    this.ring = window.setInterval(() => this.beat(), RING_MS);
  }

  /** A mic level: the halo around the words wobbles, a soft ripple at an uneven strength
   *  somewhere on it each beat. */
  private beat() {
    const r = this.hosts.reader.getBoundingClientRect();
    const a = Math.random() * Math.PI * 2;
    const d = r.width * HALO_R;
    this.reader?.fluid.impulse(r.left + r.width / 2 + Math.cos(a) * d, r.top + r.height / 2 + Math.sin(a) * d, {
      strength: 0.4 + Math.random() * 0.6,
      radius: 0.06,
    });
  }

  /** The listening halo: a thin ring just inside the screen's rim. */
  private haloImage(): HTMLCanvasElement {
    const cv = document.createElement("canvas");
    cv.width = cv.height = HALO_PX;
    const g = cv.getContext("2d");
    if (g) {
      g.strokeStyle = HALO;
      g.lineWidth = HALO_PX * 0.035;
      g.beginPath();
      g.arc(HALO_PX / 2, HALO_PX / 2, HALO_PX * HALO_R, 0, Math.PI * 2);
      g.stroke();
    }
    return cv;
  }

  private show(on: boolean) {
    if (on === !!this.reader) return;
    if (!on) {
      clearInterval(this.ring);
      clearTimeout(this.hold);
      this.off?.();
      this.off = null;
      this.reader?.destroy();
      this.reader = this.words = this.halo = null;
      return;
    }
    const screen = {
      shape: "circle",
      fps: FPS,
      fpsFallback: false,
      worker: () => new FluidWorker(),
    } as const;
    this.reader = createGlyphFluidReader(this.hosts.reader, {
      ...screen,
      // only a line still being said: an idle reader would show its first word, still
      text: this.done ? this.text : "",
      wpm: this.wpm,
    });
    this.off = this.reader.clock.subscribe((e) => {
      if (e.type !== "end") return;
      const done = this.done;
      this.done = null;
      done?.();
    });
    // the choices and the halo are more targets on the reader's own fluid
    const fluid = this.reader.fluid;
    const word = (el: HTMLElement, choice: Choice) => {
      const t = fluid.addTarget(GlyphFluidTargets.text(el, { color: CHOICE_INK[choice] }));
      t.melt();
      return t;
    };
    const c = this.hosts.choices;
    this.words = { yes: word(c.yes, "yes"), no: word(c.no, "no"), idk: word(c.idk, "idk") };
    this.halo = fluid.addTarget(
      GlyphFluidTargets.image(this.haloImage(), this.hosts.reader, {
        ink: "alpha",
      }),
    );
    this.halo.melt();
    this.applyListen();
  }
}
