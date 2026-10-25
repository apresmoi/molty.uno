// OpenAI provider (secret OPENAI_API_KEY). TTS asks for `pcm`, which OpenAI
// documents as raw 24 kHz signed 16-bit little-endian mono; we resample to
// 16 kHz. STT uploads a 16 kHz WAV to /v1/audio/transcriptions.
import { encodeWav, toPcm16k } from "../core/audio.js";
import type { TtsOptions, VoiceProvider } from "./types.js";

export interface OpenAiOptions {
  apiKey: string;
  baseUrl?: string;
  ttsModel?: string;
  sttModel?: string;
  defaultVoice?: string;
  fetch?: typeof fetch;
}

export class OpenAiVoice implements VoiceProvider {
  readonly name = "openai";
  private readonly base: string;
  private readonly f: typeof fetch;

  constructor(private readonly o: OpenAiOptions) {
    if (!o.apiKey) throw new Error("OPENAI_API_KEY is not set");
    this.base = (o.baseUrl || "https://api.openai.com/v1").replace(/\/$/, "");
    this.f = o.fetch ?? ((...a) => fetch(...a));
  }

  async tts(text: string, opts: TtsOptions): Promise<Int16Array> {
    const res = await this.f(`${this.base}/audio/speech`, {
      method: "POST",
      headers: { authorization: `Bearer ${this.o.apiKey}`, "content-type": "application/json" },
      body: JSON.stringify({ model: this.o.ttsModel || "gpt-4o-mini-tts", input: text, voice: opts.voice || this.o.defaultVoice || "alloy", response_format: "pcm" }),
    });
    if (!res.ok) throw new Error(`openai tts HTTP ${res.status}`);
    return toPcm16k(new Uint8Array(await res.arrayBuffer()), 24000);
  }

  async stt(pcm: Int16Array): Promise<string> {
    const form = new FormData();
    form.append("model", this.o.sttModel || "gpt-4o-mini-transcribe");
    form.append("file", new Blob([encodeWav(pcm)], { type: "audio/wav" }), "listen.wav");
    const res = await this.f(`${this.base}/audio/transcriptions`, { method: "POST", headers: { authorization: `Bearer ${this.o.apiKey}` }, body: form });
    if (!res.ok) throw new Error(`openai stt HTTP ${res.status}`);
    const out = (await res.json()) as { text?: unknown };
    if (typeof out.text !== "string") throw new Error("openai stt returned no text");
    return out.text.trim();
  }
}
