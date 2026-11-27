// Remote voice provider: the owner's own "voice box" (a GPU machine running
// STT/TTS) that dialed OUT to this relay over the voice socket (`GET /v1/voice`,
// docs/AGENT-LINK.md "Voice box"). The box is one more relay client: the room
// owns its socket; this class only speaks the box protocol over whatever
// socket `link()` returns at the moment a request starts.
//
//   relay → box  {"t":"tts","id","text","voice"?}      box → relay  pcm16 frames (s=id) … {"t":"tts.end","id","error"?}
//                                                    box → relay  {"t":"tts.word","id","i","start_s"} per word (its timing)
//   relay → box  {"t":"tts.cancel","id"}
//   relay → box  {"t":"stt.start","id","rate":16000,"no_speech_ms"?}, pcm16 frames (s=id) as the device sends them,
//                {"t":"stt.end","id"} | {"t":"stt.cancel","id"}
//   box → relay  {"t":"stt.partial","id","text"}?  {"t":"stt.turn_end","id","reason"?}? (the human finished — or,
//                with reason "no_speech", nobody spoke within no_speech_ms: end the listen now)
//                {"t":"stt.final","id","text","ms"?} | {"t":"stt.final","id","error"}
//   box → relay  {"t":"stt.hint","id","k":"pause"|"question"}? (while the human talks: the room forwards it to Molty)
//                {"t":"stt.trace","id","trace":{…}}? (before stt.turn_end and stt.final: the latest is kept, core/trace.ts)
//                {"t":"stt.reflex","id","n","pick","conf","p","ms","chars","tokens"?}? (only when stt.start said reflex:true:
//                the box's classifier on reflex chunk n, core/reflex.ts)
//
// The voice front (docs/AGENT-LINK.md "Voice front"; a box whose hello says "front": 1, core/front.ts):
//   relay → box  stt.start adds "front": true (answer this listen yourself at the turn's end; "hold": true,
//                held to talk: the release ends it, never your turn detection)
//   relay → box  {"t":"tts",…,"front":"own"|"exact"|"wait"} (the agent's answer: in the front's words / verbatim /
//                "still waiting"), {"t":"front.context","text"}, {"t":"front.end","reason"}
//   box → relay  {"t":"stt.hint","id","k":"speech"} (the person started speaking)
//                {"t":"front.reply","id","stt","ms"?} then frames s=id, tts.word, {"t":"tts.text","id","text","final"?},
//                {"t":"tts.end","id","wait"?,"ms"?} (as a tts; wait: the front handed the turn to the agent)
//                {"t":"front.session","state","reason"?,"s"?,"usage"?,"today_s"?} (the box's session, for the log)
//
// Every request is bound to the socket it went out on: when that socket closes
// or is replaced, drop() fails its requests at once (`voice_box_offline`), so
// nothing waits on a box that is gone.
import { b64urlEncode, randomBytes } from "../core/crypto.js";
import { type AudioHeader, type JsonObject, MAX_AUDIO_PAYLOAD_BYTES, SAMPLE_RATE, encodeAudioFrame, isRecord } from "../core/protocol.js";
import { bytesToPcm16, pcm16ToBytes } from "../core/audio.js";
import { PcmFeed, type SynthTimer } from "../core/speech.js";
import { cleanBoxTrace } from "../core/trace.js";
import type { SttStream, SttStreamOptions, TtsOptions, VoiceProvider } from "./types.js";

/** The socket the box is on (a DeviceSocket in the room). */
export interface BoxChannel {
  sendText(text: string): void;
  sendBinary(bytes: Uint8Array): void;
  same(other: BoxChannel): boolean;
}

/**
 * Speech by the voice front (frontSpeak, or the box's own front.reply): PCM into `feed`, word cues on
 * the spoken words, and what is being said (the model's output transcript, `said`) as it is known.
 */
export interface FrontSpeech {
  readonly id: string;
  readonly feed: PcmFeed;
  /** own / exact / wait (an agent's answer, the relay's "still waiting"), reply (the front's own answer to a turn). */
  readonly mode: "own" | "exact" | "wait" | "reply";
  /** What is being said so far; `final` once the box says it is complete. */
  said: string;
  final: boolean;
  /** The front handed the person's turn to the agent (its reply was a holding line). */
  wait: boolean;
  /** The box's timings ({created} on a front.reply, {first_audio, total} at the end). */
  boxMs: JsonObject;
  /** Resolves when the box ended the stream (all of it generated: said and wait are final). */
  readonly ended: Promise<void>;
  /** Resolves when `said` changes or the stream ends (the device's text waits on it). */
  changed(): Promise<void>;
  /** Nobody will play it: the box stops (tts.cancel). */
  cancel(): void;
}

export const BOX_OFFLINE = "voice_box_offline";
export const BOX_ERROR = "voice_box_error";

/** Samples per frame to the box: the 16 KB payload cap. */
const FRAME_SAMPLES = MAX_AUDIO_PAYLOAD_BYTES / 2;
/** A TTS stream with no audio and no end for this long is torn down (the room's segment timeout is shorter). */
const TTS_IDLE_MS = 60000;

interface TtsOp {
  ch: BoxChannel;
  feed: PcmFeed;
  onWord?: (i: number, startS: number) => void;
}
interface FrontOp {
  ch: BoxChannel;
  sp: FrontSpeech;
  /** The box ended it (error = failed). */
  end(error?: Error, wait?: boolean, ms?: JsonObject): void;
  text(said: string, final: boolean): void;
}
interface SttOp {
  ch: BoxChannel;
  /** stt.hint speech from the box: the person started speaking. */
  speech(): void;
  settle(err: Error | undefined, text?: string): void;
  /** stt.turn_end from the box: once, and only while the audio is still flowing. */
  turnEnd(reason?: "no_speech"): void;
  /** stt.hint from the box, while the audio is still flowing. */
  hint(k: "pause" | "question"): void;
  /** stt.partial from the box: the text so far, only while the audio is still flowing. */
  partial(text: string): void;
  /** stt.reflex from the box, while the audio is still flowing. */
  reflex(frame: Record<string, unknown>): void;
  ms?: JsonObject;
  /** The box's latest stt.trace. */
  trace?: JsonObject;
}

export class RemoteVoiceProvider implements VoiceProvider {
  readonly name = "box";
  private readonly ttsOps = new Map<string, TtsOp>();
  private readonly sttOps = new Map<string, SttOp>();
  private readonly openerOps = new Map<string, { ch: BoxChannel; done(line: string | undefined): void }>();
  private readonly frontOps = new Map<string, FrontOp>();
  private seq = 0;
  /** The box's front answers a listen by itself (front.reply): the room plays it, or cancels it. */
  onFrontReply: ((sttId: string, sp: FrontSpeech) => void) | undefined;
  /** The box's front session opened / closed (front.session): the room logs it. */
  onFrontSession: ((frame: Record<string, unknown>) => void) | undefined;

  constructor(
    /** The box's socket now, or undefined when no box is connected (and greeted). */
    private readonly link: () => BoxChannel | undefined,
    private readonly timer: SynthTimer,
  ) {}

  private newId(kind: "t" | "s") {
    return `${kind}${++this.seq}${b64urlEncode(randomBytes(4))}`;
  }

  private channel(): BoxChannel {
    const ch = this.link();
    if (!ch) throw new Error(BOX_OFFLINE);
    return ch;
  }

  /** How many requests are in flight (tests, status). */
  get pending(): number {
    return this.ttsOps.size + this.sttOps.size + this.frontOps.size;
  }

  // ---------------- first words (core/opener.ts) ----------------

  /** The box's small model starts the reply: its line, or undefined (no line, no box, gave up by `ms`). */
  opener(text: string, history: ReadonlyArray<{ role: string; text: string }>, ms: number): Promise<string | undefined> {
    const ch = this.link();
    if (!ch) return Promise.resolve(undefined);
    const id = this.newId("t");
    return new Promise((resolve) => {
      const handle = this.timer.setTimeout(() => done(undefined), ms);
      const done = (line: string | undefined) => {
        this.timer.clearTimeout(handle);
        this.openerOps.delete(id);
        resolve(line);
      };
      this.openerOps.set(id, { ch, done });
      try {
        ch.sendText(JSON.stringify({ t: "opener", id, text, history: history.map((m) => ({ role: m.role, text: m.text })) }));
      } catch {
        done(undefined);
      }
    });
  }

  // ---------------- TTS ----------------

  async tts(text: string, opts: TtsOptions): Promise<Int16Array> {
    const parts: Int16Array[] = [];
    let n = 0;
    for await (const p of this.ttsStream(text, opts)) {
      parts.push(p);
      n += p.length;
    }
    if (n === 0) throw new Error("tts returned no audio");
    const out = new Int16Array(n);
    let o = 0;
    for (const p of parts) {
      out.set(p, o);
      o += p.length;
    }
    return out;
  }

  async *ttsStream(text: string, opts: TtsOptions): AsyncGenerator<Int16Array> {
    const ch = this.channel();
    const id = this.newId("t");
    const feed = new PcmFeed();
    this.ttsOps.set(id, { ch, feed, onWord: opts.onWord });
    const cancel = () => {
      if (feed.done) return;
      feed.end(opts.signal?.reason ?? new Error("tts cancelled"));
      this.sendQuiet(ch, { t: "tts.cancel", id });
    };
    opts.signal?.addEventListener("abort", cancel, { once: true });
    try {
      if (opts.signal?.aborted) return;
      const msg: JsonObject = { t: "tts", id, text };
      if (opts.voice) msg.voice = opts.voice;
      try {
        ch.sendText(JSON.stringify(msg));
      } catch {
        throw new Error(BOX_OFFLINE);
      }
      for (;;) {
        for (const p of feed.take()) yield p;
        if (feed.done) {
          if (feed.error !== undefined) throw feed.error;
          return;
        }
        await feed.waitFor((f, ms) => this.timer.setTimeout(f, ms), (h) => this.timer.clearTimeout(h), TTS_IDLE_MS);
        if (!feed.done && !feed.queued) throw new Error("tts_timeout");
      }
    } finally {
      opts.signal?.removeEventListener("abort", cancel);
      this.ttsOps.delete(id);
      // The consumer stopped early (or the stream stalled): tell the box to stop synthesising.
      if (!feed.done) {
        feed.end();
        this.sendQuiet(ch, { t: "tts.cancel", id });
      }
    }
  }

  // ---------------- the voice front ----------------

  /** The agent's answer (or "still waiting") spoken by the box's front: tts {front: mode}. Throws when no box. */
  frontSpeak(text: string, mode: "own" | "exact" | "wait"): FrontSpeech {
    const ch = this.channel();
    const id = this.newId("t");
    const sp = this.frontOp(ch, id, mode);
    try {
      ch.sendText(JSON.stringify({ t: "tts", id, text, front: mode }));
    } catch {
      this.frontOps.get(id)?.end(new Error(BOX_OFFLINE));
    }
    return sp;
  }

  /** Silent context for the front (the agent's `front {context}`); false: no box. */
  frontContext(text: string): boolean {
    const ch = this.link();
    if (!ch) return false;
    this.sendQuiet(ch, { t: "front.context", text });
    return true;
  }

  /** The conversation is over: the box closes its session (billing stops). */
  frontEnd(reason: string): void {
    const ch = this.link();
    if (ch) this.sendQuiet(ch, { t: "front.end", reason });
  }

  private frontOp(ch: BoxChannel, id: string, mode: FrontSpeech["mode"]): FrontSpeech {
    const feed = new PcmFeed();
    let endedResolve!: () => void;
    const ended = new Promise<void>((r) => (endedResolve = r));
    let wake: Array<() => void> = [];
    const poke = () => {
      const w = wake;
      wake = [];
      for (const f of w) f();
    };
    const sp: FrontSpeech = {
      id,
      feed,
      mode,
      said: "",
      final: false,
      wait: false,
      boxMs: {},
      ended,
      changed: () => (feed.done ? Promise.resolve() : new Promise<void>((r) => wake.push(r))),
      cancel: () => {
        if (!this.frontOps.has(id)) return;
        this.frontOps.delete(id);
        feed.end(new Error("tts cancelled"));
        this.sendQuiet(ch, { t: "tts.cancel", id });
        endedResolve();
        poke();
      },
    };
    this.frontOps.set(id, {
      ch,
      sp,
      text: (said, final) => {
        sp.said = said;
        sp.final ||= final;
        poke();
      },
      end: (error, wait, ms) => {
        this.frontOps.delete(id);
        sp.wait = !!wait;
        sp.final = true;
        if (ms) Object.assign(sp.boxMs, ms);
        feed.end(error);
        endedResolve();
        poke();
      },
    });
    // the consumer gave up (the say failed, the device went): the box stops generating
    feed.signal.addEventListener("abort", () => sp.cancel(), { once: true });
    return sp;
  }

  // ---------------- STT ----------------

  async stt(pcm: Int16Array): Promise<string> {
    const s = this.sttStream();
    for (let i = 0; i < pcm.length; i += FRAME_SAMPLES) s.push(pcm.subarray(i, i + FRAME_SAMPLES));
    return s.end();
  }

  sttStream(opts: SttStreamOptions = {}): SttStream & { readonly id: string; readonly boxMs: () => JsonObject | undefined; trace(): JsonObject | undefined } {
    const ch = this.channel();
    const id = this.newId("s");
    let seq = 0;
    let state: "open" | "ending" | "settled" = "open";
    let turned = false;
    let resolve!: (t: string) => void;
    let reject!: (e: Error) => void;
    const result = new Promise<string>((res, rej) => ((resolve = res), (reject = rej)));
    // Nobody may be awaiting it yet (the box can drop mid-listen): never an unhandled rejection.
    result.catch(() => {});
    const op: SttOp = {
      ch,
      settle: (err, text) => {
        if (state === "settled") return;
        const open = state === "open";
        state = "settled";
        this.sttOps.delete(id);
        if (err) reject(err);
        else resolve(text ?? "");
        if (err && open) opts.onFail?.(); // gone mid-listen: nobody will call the turn's end
      },
      turnEnd: (reason) => {
        if (state !== "open" || turned) return;
        turned = true;
        opts.onTurnEnd?.(reason);
      },
      hint: (k) => {
        if (state === "open" && !turned) opts.onHint?.(k);
      },
      partial: (text) => {
        if (state !== "open" || turned) return;
        opts.onPartial?.(text);
      },
      reflex: (frame) => {
        if (state === "open" && !turned) opts.onReflex?.(frame);
      },
      speech: () => {
        if (state === "open" && !turned) opts.onSpeech?.();
      },
    };
    this.sttOps.set(id, op);
    try {
      ch.sendText(JSON.stringify({ t: "stt.start", id, rate: SAMPLE_RATE, ...(opts.noSpeechMs !== undefined ? { no_speech_ms: opts.noSpeechMs } : {}), ...(opts.reflex ? { reflex: true } : {}), ...(opts.front ? { front: true } : {}), ...(opts.front && opts.hold ? { hold: true } : {}) }));
    } catch {
      op.settle(new Error(BOX_OFFLINE));
    }
    return {
      id,
      boxMs: () => op.ms,
      trace: () => op.trace,
      push: (pcm) => {
        if (state !== "open" || pcm.length === 0) return;
        try {
          for (let i = 0; i < pcm.length; i += FRAME_SAMPLES) {
            const header: AudioHeader = { s: id, seq: seq++, kind: "pcm16", rate: SAMPLE_RATE, end: false };
            ch.sendBinary(encodeAudioFrame(header, pcm16ToBytes(pcm.subarray(i, i + FRAME_SAMPLES))));
          }
        } catch {
          op.settle(new Error(BOX_OFFLINE));
        }
      },
      end: () => {
        if (state === "open") {
          state = "ending";
          try {
            ch.sendText(JSON.stringify({ t: "stt.end", id }));
          } catch {
            op.settle(new Error(BOX_OFFLINE));
          }
        }
        return result;
      },
      abort: () => {
        if (state === "settled") return;
        this.sendQuiet(ch, { t: "stt.cancel", id });
        op.settle(new Error("stt cancelled"));
      },
    };
  }

  // ---------------- frames from the box ----------------

  /** A text frame from the box socket `from` (already parsed). */
  onText(from: BoxChannel, frame: Record<string, unknown>): void {
    if (frame.t === "front.session") return this.onFrontSession?.(frame);
    const id = typeof frame.id === "string" ? frame.id : undefined;
    if (!id) return;
    if (frame.t === "opener.result") {
      const op = this.openerOps.get(id);
      if (op && op.ch.same(from)) op.done(typeof frame.text === "string" ? frame.text.slice(0, 400) : undefined);
    } else if (frame.t === "front.reply") {
      const stt = frame.stt;
      if (typeof stt !== "string" || this.frontOps.has(id) || id.length > 128) return;
      const sp = this.frontOp(from, id, "reply");
      if (isRecord(frame.ms)) Object.assign(sp.boxMs, cleanMs(frame.ms));
      if (this.onFrontReply) this.onFrontReply(stt, sp);
      else sp.cancel();
    } else if (frame.t === "tts.text") {
      const op = this.frontOps.get(id);
      if (op && op.ch.same(from) && typeof frame.text === "string") op.text(frame.text.trim().slice(0, 4000), frame.final === true);
    } else if (frame.t === "tts.end") {
      const err = typeof frame.error === "string" && frame.error ? new Error(BOX_ERROR) : undefined;
      const fop = this.frontOps.get(id);
      if (fop && fop.ch.same(from)) return fop.end(err, frame.wait === true, isRecord(frame.ms) ? cleanMs(frame.ms) : undefined);
      const op = this.ttsOps.get(id);
      if (!op || !op.ch.same(from)) return;
      op.feed.end(err);
    } else if (frame.t === "tts.word") {
      const i = frame.i, s = frame.start_s;
      if (typeof i !== "number" || !Number.isInteger(i) || i < 0 || i > 100000 || typeof s !== "number" || !Number.isFinite(s) || s < 0 || s > 3600) return;
      const fop = this.frontOps.get(id);
      if (fop && fop.ch.same(from) && !fop.sp.feed.done) return fop.sp.feed.cue(i, Math.round(s * SAMPLE_RATE));
      const op = this.ttsOps.get(id);
      if (!op || !op.ch.same(from) || op.feed.done) return;
      op.onWord?.(i, s);
    } else if (frame.t === "stt.turn_end") {
      const op = this.sttOps.get(id);
      if (op && op.ch.same(from)) op.turnEnd(frame.reason === "no_speech" ? "no_speech" : undefined);
    } else if (frame.t === "stt.hint") {
      const op = this.sttOps.get(id);
      if (op && op.ch.same(from) && (frame.k === "pause" || frame.k === "question")) op.hint(frame.k);
      else if (op && op.ch.same(from) && frame.k === "speech") op.speech();
    } else if (frame.t === "stt.trace") {
      // kept on the op itself (still reachable once the op settled: the room reads it at the listen's end)
      const op = this.sttOps.get(id);
      const t = op && op.ch.same(from) ? cleanBoxTrace(frame.trace) : undefined;
      if (op && t) op.trace = t;
    } else if (frame.t === "stt.reflex") {
      const op = this.sttOps.get(id);
      if (op && op.ch.same(from)) op.reflex(frame);
    } else if (frame.t === "stt.partial") {
      const op = this.sttOps.get(id);
      if (op && op.ch.same(from) && typeof frame.text === "string") op.partial(frame.text.trim().slice(0, 4000));
    } else if (frame.t === "stt.final") {
      const op = this.sttOps.get(id);
      if (!op || !op.ch.same(from)) return;
      if (typeof frame.error === "string" && frame.error) return op.settle(new Error(BOX_ERROR));
      if (isRecord(frame.ms)) op.ms = frame.ms as JsonObject;
      op.settle(undefined, typeof frame.text === "string" ? frame.text.trim().slice(0, 4000) : "");
    }
    // stt.partial is informational (onPartial: the relay's partial transcripts): the final text is what counts.
  }

  /** A binary frame from the box: TTS audio for one of our requests. */
  onAudio(from: BoxChannel, header: AudioHeader, payload: Uint8Array): void {
    const fop = this.frontOps.get(header.s);
    if (fop && fop.ch.same(from)) {
      fop.sp.feed.push(bytesToPcm16(payload));
      return;
    }
    const op = this.ttsOps.get(header.s);
    if (!op || !op.ch.same(from)) return;
    op.feed.push(bytesToPcm16(payload));
    if (header.end) op.feed.end();
  }

  /** The box socket went away (closed, replaced, unpaired): fail its requests now (all of them when `only` is absent). */
  drop(only?: BoxChannel, code = BOX_OFFLINE): void {
    for (const [id, op] of this.ttsOps) {
      if (only && !op.ch.same(only)) continue;
      this.ttsOps.delete(id);
      op.feed.end(new Error(code));
    }
    for (const op of [...this.sttOps.values()]) {
      if (only && !op.ch.same(only)) continue;
      op.settle(new Error(code));
    }
    for (const op of [...this.frontOps.values()]) {
      if (only && !op.ch.same(only)) continue;
      op.end(new Error(code));
    }
  }

  private sendQuiet(ch: BoxChannel, msg: JsonObject) {
    try {
      ch.sendText(JSON.stringify(msg));
    } catch {
      /* the box is gone */
    }
  }
}

/** The box's ms object, numbers 0..3600000 only. */
function cleanMs(v: Record<string, unknown>): JsonObject {
  const out: JsonObject = {};
  for (const [k, x] of Object.entries(v).slice(0, 8)) if (/^[a-z_]{1,32}$/.test(k) && typeof x === "number" && Number.isFinite(x) && x >= 0 && x <= 3600000) out[k] = Math.round(x);
  return out;
}
