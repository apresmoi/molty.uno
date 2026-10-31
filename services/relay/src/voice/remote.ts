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
//   relay → box  {"t":"live.start","id"}, pcm16 frames (s=id) as the device sends them, {"t":"live.listen","id"} (Molty
//                has played everything: the mic is on again), {"t":"live.stop","id"}
//   box → relay  {"t":"live.started","id"}, {"t":"live.reply","id","gap_ms"?} then pcm16 frames (s=id), {"t":"live.reply_end","id"},
//                {"t":"live.heard","id","text"}, {"t":"live.said","id","text"}, {"t":"live.end","id","reason","billed_s"?,"error"?}
//                A live conversation (molty_voice/live.py): the box talks to the model, the room plays turns.
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

/** A live conversation's handlers (`liveStart`): each is called on the box's frames, in order. */
export interface LiveHandlers {
  /** A reply began: its audio goes into `feed`, which ends with the reply. */
  onReply(feed: PcmFeed, gapMs: number | undefined): void;
  /** The box closed the session (quiet, cap, a model failure) or went away. Once. */
  onEnd(reason: string, billedS: number | undefined, error: string | undefined): void;
  /** A piece of the transcript, either side. */
  onWords(who: "heard" | "said", text: string): void;
}

/** A live conversation on the box, started (`liveStart` resolved). */
export interface LiveSession {
  readonly id: string;
  /** The mic's frames, for the length of one device listen. */
  stream(): SttStream;
  /** Molty has played everything: the box may send the mic again. */
  listen(): void;
  /** Close the session. Idempotent; the box also closes by itself on quiet, on its cap and when this socket goes. */
  stop(): void;
}

/** The socket the box is on (a DeviceSocket in the room). */
export interface BoxChannel {
  sendText(text: string): void;
  sendBinary(bytes: Uint8Array): void;
  same(other: BoxChannel): boolean;
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
interface LiveOp {
  ch: BoxChannel;
  feed?: PcmFeed;
  started?: (ok: true | string) => void;
  h: LiveHandlers;
}
interface SttOp {
  ch: BoxChannel;
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
  private readonly liveOps = new Map<string, LiveOp>();
  private seq = 0;

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
    return this.ttsOps.size + this.sttOps.size;
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

  // ---------------- a live conversation (molty_voice/live.py) ----------------

  /** Opens a session on the box: the session, or why not (no box, it failed, not started within `ms`). */
  liveStart(h: LiveHandlers, ms: number): Promise<LiveSession | string> {
    const ch = this.link();
    if (!ch) return Promise.resolve(BOX_OFFLINE);
    const id = this.newId("s");
    const op: LiveOp = { ch, h };
    this.liveOps.set(id, op);
    const stop = () => {
      if (!this.liveOps.has(id)) return;
      this.sendQuiet(ch, { t: "live.stop", id });
    };
    return new Promise((resolve) => {
      const handle = this.timer.setTimeout(() => op.started?.("live_start_timeout"), ms);
      op.started = (r) => {
        op.started = undefined;
        this.timer.clearTimeout(handle);
        if (r !== true) {
          stop(); // a session the box may still be opening must not stay open, billed
          this.liveOps.delete(id);
          resolve(r);
          return;
        }
        resolve({
          id,
          stream: () => this.liveStream(ch, id),
          listen: () => {
            if (this.liveOps.has(id)) this.sendQuiet(ch, { t: "live.listen", id });
          },
          stop,
        });
      };
      try {
        ch.sendText(JSON.stringify({ t: "live.start", id }));
      } catch {
        op.started(BOX_OFFLINE);
      }
    });
  }

  private liveStream(ch: BoxChannel, id: string): SttStream {
    let seq = 0;
    return {
      push: (pcm) => {
        if (!this.liveOps.has(id) || pcm.length === 0) return;
        try {
          for (let i = 0; i < pcm.length; i += FRAME_SAMPLES) {
            ch.sendBinary(encodeAudioFrame({ s: id, seq: seq++, kind: "pcm16", rate: SAMPLE_RATE, end: false }, pcm16ToBytes(pcm.subarray(i, i + FRAME_SAMPLES))));
          }
        } catch {
          /* the box is gone: drop() ends the session */
        }
      },
      end: () => Promise.resolve(""),
      abort: () => {},
    };
  }

  private liveEnd(id: string, op: LiveOp, reason: string, billedS: number | undefined, error: string | undefined) {
    this.liveOps.delete(id);
    op.feed?.end();
    op.feed = undefined;
    if (op.started) op.started(error ?? reason);
    else op.h.onEnd(reason, billedS, error);
  }

  private onLiveText(op: LiveOp, id: string, frame: Record<string, unknown>) {
    if (frame.t === "live.started") op.started?.(true);
    else if (frame.t === "live.reply") {
      op.feed?.end();
      op.feed = new PcmFeed();
      op.h.onReply(op.feed, typeof frame.gap_ms === "number" && Number.isFinite(frame.gap_ms) ? frame.gap_ms : undefined);
    } else if (frame.t === "live.reply_end") {
      op.feed?.end();
      op.feed = undefined;
    } else if ((frame.t === "live.heard" || frame.t === "live.said") && typeof frame.text === "string") {
      op.h.onWords(frame.t === "live.heard" ? "heard" : "said", frame.text.slice(0, 4000));
    } else if (frame.t === "live.end") {
      const billed = typeof frame.billed_s === "number" && Number.isFinite(frame.billed_s) ? frame.billed_s : undefined;
      this.liveEnd(id, op, typeof frame.reason === "string" ? frame.reason.slice(0, 40) : "ended", billed, typeof frame.error === "string" ? frame.error.slice(0, 200) : undefined);
    }
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
    };
    this.sttOps.set(id, op);
    try {
      ch.sendText(JSON.stringify({ t: "stt.start", id, rate: SAMPLE_RATE, ...(opts.noSpeechMs !== undefined ? { no_speech_ms: opts.noSpeechMs } : {}), ...(opts.reflex ? { reflex: true } : {}) }));
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
    const id = typeof frame.id === "string" ? frame.id : undefined;
    if (!id) return;
    const live = typeof frame.t === "string" && frame.t.startsWith("live.") ? this.liveOps.get(id) : undefined;
    if (live) {
      if (live.ch.same(from)) this.onLiveText(live, id, frame);
      return;
    }
    if (frame.t === "opener.result") {
      const op = this.openerOps.get(id);
      if (op && op.ch.same(from)) op.done(typeof frame.text === "string" ? frame.text.slice(0, 400) : undefined);
    } else if (frame.t === "tts.end") {
      const op = this.ttsOps.get(id);
      if (!op || !op.ch.same(from)) return;
      op.feed.end(typeof frame.error === "string" && frame.error ? new Error(BOX_ERROR) : undefined);
    } else if (frame.t === "tts.word") {
      const op = this.ttsOps.get(id);
      const i = frame.i, s = frame.start_s;
      if (!op || !op.ch.same(from) || op.feed.done) return;
      if (typeof i !== "number" || !Number.isInteger(i) || i < 0 || i > 100000 || typeof s !== "number" || !Number.isFinite(s) || s < 0 || s > 3600) return;
      op.onWord?.(i, s);
    } else if (frame.t === "stt.turn_end") {
      const op = this.sttOps.get(id);
      if (op && op.ch.same(from)) op.turnEnd(frame.reason === "no_speech" ? "no_speech" : undefined);
    } else if (frame.t === "stt.hint") {
      const op = this.sttOps.get(id);
      if (op && op.ch.same(from) && (frame.k === "pause" || frame.k === "question")) op.hint(frame.k);
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
    const live = this.liveOps.get(header.s);
    if (live) {
      if (live.ch.same(from)) live.feed?.push(bytesToPcm16(payload));
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
    for (const [id, op] of [...this.liveOps]) {
      if (only && !op.ch.same(only)) continue;
      this.liveEnd(id, op, code, undefined, code);
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
