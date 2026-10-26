// The device's speed reader on its own round screen: the REAL FluidStage (creatures/js/setup,
// the 412 px disc the console embeds) and the REAL reader rules (creatures/js/fluid/reader.js:
// ORP focal letter, per-word delay by length and punctuation). Driven exactly like the fluid
// viewer's speed-reader block (creatures/js/fluid/main.js): the sentence fixes the type size,
// each word condenses out of the fluid with its ORP letter accented, and lands with a soft
// ring. After the last word the letters melt back and, after a beat, it reads again.
// Runs only while on screen: offscreen the canvas is detached (the stage idles) and the
// word clock stops.
import type { FluidStage, ReaderToken } from "@creatures/setup/fluid-stage.js";

type Engine = {
  FluidStage: typeof import("@creatures/setup/fluid-stage.js").FluidStage;
  tokenize: (text: string) => ReaderToken[];
  wordDelay: (token: ReaderToken, wpm: number) => number;
};

const LEAD_IN = 550; // ms: let the ink gather before the first word (as main.js)
const REST = 2600; // ms between readings

export interface SpeedReaderHandle {
  setText(text: string): void;
  setWpm(wpm: number): void;
  destroy(): void;
}

export async function mountSpeedReader(host: HTMLElement, text: string, wpm: number): Promise<SpeedReaderHandle> {
  const [stage, reader] = await Promise.all([
    import("@creatures/setup/fluid-stage.js"),
    import("@creatures/fluid/reader.js"),
  ]);
  return new SpeedReader(host, { FluidStage: stage.FluidStage, ...reader }, text, wpm);
}

class SpeedReader implements SpeedReaderHandle {
  private stage: FluidStage;
  private tokens: ReaderToken[] = [];
  private index = -1;
  private timer = 0;
  private visible = false;
  private observer: IntersectionObserver;

  constructor(
    private host: HTMLElement,
    private engine: Engine,
    private text: string,
    private wpm: number,
  ) {
    this.stage = new engine.FluidStage(412);
    this.observer = new IntersectionObserver(([e]) => this.show(!!e?.isIntersecting), { threshold: 0.25 });
    this.observer.observe(host);
  }

  setText(text: string) {
    this.text = text.trim() || this.text;
    this.restart();
  }
  setWpm(wpm: number) {
    this.wpm = wpm; // takes effect from the next word
  }
  destroy() {
    clearTimeout(this.timer);
    this.observer.disconnect();
    this.stage.el.remove(); // a detached FluidStage idles
  }

  private show(on: boolean) {
    if (on === this.visible) return;
    this.visible = on;
    clearTimeout(this.timer);
    if (on) {
      this.host.replaceChildren(this.stage.el);
      this.restart();
    } else this.stage.el.remove();
  }

  private restart() {
    clearTimeout(this.timer);
    if (!this.visible) return;
    this.tokens = this.engine.tokenize(this.text);
    if (!this.tokens.length) return;
    this.stage.textT.setSentence(this.tokens); // one type size for the whole sentence
    this.index = -1;
    this.stage.blob();
    this.timer = window.setTimeout(() => this.next(), LEAD_IN);
  }

  private next() {
    if (!this.visible) return;
    this.index++;
    const tok = this.tokens[this.index];
    if (!tok) {
      this.stage.blob(); // the letters melt back into the fluid
      this.timer = window.setTimeout(() => this.restart(), REST);
      return;
    }
    this.stage.textT.setWord(tok);
    if (this.index === 0) this.stage.set("text");
    else this.stage.ripple(0, 0, 0.63, 0.12); // a soft ring leaves the word on every change
    this.timer = window.setTimeout(() => this.next(), this.engine.wordDelay(tok, this.wpm));
  }
}
