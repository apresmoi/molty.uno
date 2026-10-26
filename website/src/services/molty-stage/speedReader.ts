// The device's speed reader on its own round screen: @glyphcss/fluid/reader with
// `shape: "circle"` (molty's 52-column disc) and molty's reader rules (ORP focal letter,
// per-word delay by length and punctuation, shared with the firmware). Each word condenses
// out of the fluid with its ORP letter accented and lands with a soft ring; after the last
// word the letters melt back and, after a beat, it reads again.
// Runs only while on screen: offscreen the reader is torn down (no fluid, no word clock)
// and it starts the sentence over when it comes back.
import { createGlyphFluidReader, type GlyphFluidReader } from "@glyphcss/fluid/reader";
import FluidWorker from "@glyphcss/fluid/worker?worker";

const REST = 2600; // ms between readings
const FPS = 30;

export interface SpeedReaderHandle {
  setText(text: string): void;
  setWpm(wpm: number): void;
  destroy(): void;
}

export async function mountSpeedReader(host: HTMLElement, text: string, wpm: number): Promise<SpeedReaderHandle> {
  return new SpeedReader(host, text, wpm);
}

class SpeedReader implements SpeedReaderHandle {
  private reader: GlyphFluidReader | null = null;
  private observer: IntersectionObserver;

  constructor(
    private host: HTMLElement,
    private text: string,
    private wpm: number,
  ) {
    this.observer = new IntersectionObserver(([e]) => this.show(!!e?.isIntersecting), { threshold: 0.25 });
    this.observer.observe(host);
  }

  setText(text: string) {
    this.text = text.trim() || this.text;
    this.reader?.clock.setText(this.text);
  }
  setWpm(wpm: number) {
    this.wpm = wpm;
    this.reader?.clock.setWpm(wpm); // takes effect from the next word
  }
  destroy() {
    this.observer.disconnect();
    this.show(false);
  }

  private show(on: boolean) {
    if (on === !!this.reader) return;
    if (!on) {
      this.reader?.destroy();
      this.reader = null;
      return;
    }
    this.reader = createGlyphFluidReader(this.host, {
      text: this.text,
      wpm: this.wpm,
      shape: "circle",
      loop: REST,
      fps: FPS,
      fpsFallback: false,
      worker: () => new FluidWorker(),
    });
  }
}
