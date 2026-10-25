// Workers AI provider (binding `AI`). Model ids verified against
// developers.cloudflare.com/workers-ai/models on 2026-10-25:
//   TTS  @cf/deepgram/aura-2-en  {text, speaker, encoding, container, sample_rate}
//   STT  @cf/openai/whisper-large-v3-turbo  {audio: base64} -> {text}
// We ask Aura for linear16 in a WAV container at 16 kHz and still parse +
// resample whatever comes back, so a different rate or channel count is fine.
// MP3 (Aura's default, MeloTTS's only output) is rejected with a clear error:
// no MP3 decoder is bundled.
import { b64Encode } from "../core/crypto.js";
import { encodeWav, toPcm16k } from "../core/audio.js";
import type { TtsOptions, VoiceProvider } from "./types.js";

/** Structural type of the `AI` binding; no Cloudflare import needed. */
export interface AiBinding {
  run(model: string, inputs: Record<string, unknown>, options?: Record<string, unknown>): Promise<unknown>;
}

export interface WorkersAiOptions {
  ttsModel?: string;
  sttModel?: string;
  defaultVoice?: string;
}

export const DEFAULT_TTS_MODEL = "@cf/deepgram/aura-2-en";
export const DEFAULT_STT_MODEL = "@cf/openai/whisper-large-v3-turbo";

export class WorkersAiVoice implements VoiceProvider {
  readonly name = "workers-ai";
  private readonly ttsModel: string;
  private readonly sttModel: string;
  private readonly defaultVoice: string;

  constructor(
    private readonly ai: AiBinding,
    opts: WorkersAiOptions = {},
  ) {
    this.ttsModel = opts.ttsModel || DEFAULT_TTS_MODEL;
    this.sttModel = opts.sttModel || DEFAULT_STT_MODEL;
    this.defaultVoice = opts.defaultVoice || "luna";
  }

  async tts(text: string, opts: TtsOptions): Promise<Int16Array> {
    const inputs = this.ttsModel.includes("melotts")
      ? { prompt: text, lang: opts.voice || "en" }
      : { text, speaker: opts.voice || this.defaultVoice, encoding: "linear16", container: "wav", sample_rate: 16000 };
    const bytes = await toBytes(await this.ai.run(this.ttsModel, inputs));
    if (bytes.byteLength === 0) throw new Error("tts returned no audio");
    return toPcm16k(bytes, 16000);
  }

  async stt(pcm: Int16Array): Promise<string> {
    const wav = encodeWav(pcm);
    // The original @cf/openai/whisper (and -tiny-en) take a byte array; turbo takes base64.
    const audio = /whisper(-tiny-en)?$/.test(this.sttModel) ? Array.from(wav) : b64Encode(wav);
    const out = await this.ai.run(this.sttModel, { audio });
    const text = (out as { text?: unknown } | null)?.text;
    if (typeof text !== "string") throw new Error("stt returned no text");
    return text.trim();
  }
}

/** Normalizes every shape AI.run returns for audio models into bytes. */
export async function toBytes(out: unknown): Promise<Uint8Array> {
  if (out instanceof Uint8Array) return out;
  if (out instanceof ArrayBuffer) return new Uint8Array(out);
  if (typeof Response !== "undefined" && out instanceof Response) return new Uint8Array(await out.arrayBuffer());
  if (out && typeof (out as ReadableStream).getReader === "function") {
    return new Uint8Array(await new Response(out as ReadableStream).arrayBuffer());
  }
  const audio = (out as { audio?: unknown } | null)?.audio;
  if (typeof audio === "string") return Uint8Array.from(atob(audio), (c) => c.charCodeAt(0));
  if (typeof out === "string") return Uint8Array.from(atob(out), (c) => c.charCodeAt(0));
  throw new Error("unrecognized TTS output");
}
