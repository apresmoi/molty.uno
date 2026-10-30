import { describe, expect, it } from "vitest";
import { encodeWav, floatToPcm16, pcm16ToBytes, parseWav } from "../src/core/audio.js";
import { b64Decode } from "../src/core/crypto.js";
import { createVoice } from "../src/voice/index.js";
import { NoneVoice, STT_UNAVAILABLE } from "../src/voice/none.js";
import { OpenAiVoice } from "../src/voice/openai.js";
import { WorkersAiVoice, toBytes } from "../src/voice/workers-ai.js";

function wav24kStereo(n: number) {
  const pcm = new Int16Array(n * 2).fill(8000);
  const bytes = encodeWav(pcm, 24000); // header says mono; patch to stereo
  const v = new DataView(bytes.buffer);
  v.setUint16(22, 2, true);
  v.setUint32(28, 24000 * 4, true);
  v.setUint16(32, 4, true);
  return bytes;
}

describe("Workers AI provider", () => {
  it("asks Aura for linear16 WAV @16k and converts whatever comes back to PCM16 mono 16 kHz", async () => {
    const calls: any[] = [];
    const ai = {
      async run(model: string, inputs: any) {
        calls.push({ model, inputs });
        return new Response(wav24kStereo(2400)).body; // ReadableStream, like the binding
      },
    };
    const v = new WorkersAiVoice(ai, {});
    const pcm = await v.tts("hello", { voice: "asteria" });
    expect(calls[0]).toEqual({ model: "@cf/deepgram/aura-2-en", inputs: { text: "hello", speaker: "asteria", encoding: "linear16", container: "wav", sample_rate: 16000 } });
    expect(pcm).toBeInstanceOf(Int16Array);
    expect(pcm.length).toBe(1600);
    expect(Math.abs(pcm[800] - 8000)).toBeLessThan(2);
  });

  it("defaults the voice and errors clearly on MP3", async () => {
    const ai = { run: async () => Uint8Array.from([0x49, 0x44, 0x33, 3, 0, 0]) };
    await expect(new WorkersAiVoice(ai, { defaultVoice: "luna" }).tts("x", {})).rejects.toThrow(/MP3/);
  });

  it("STT sends a base64 16 kHz WAV to whisper-large-v3-turbo and returns trimmed text", async () => {
    let got: any;
    const ai = {
      async run(model: string, inputs: any) {
        got = { model, inputs };
        return { text: "  hi molty ", word_count: 2 };
      },
    };
    const text = await new WorkersAiVoice(ai).stt(new Int16Array(320).fill(5));
    expect(text).toBe("hi molty");
    expect(got.model).toBe("@cf/openai/whisper-large-v3-turbo");
    const wav = parseWav(b64Decode(got.inputs.audio)!);
    expect([wav.rate, wav.channels, wav.samples.length]).toEqual([16000, 1, 320]);
  });

  it("legacy @cf/openai/whisper gets a byte array", async () => {
    let audio: unknown;
    const ai = { run: async (_m: string, i: any) => ((audio = i.audio), { text: "ok" }) };
    await new WorkersAiVoice(ai, { sttModel: "@cf/openai/whisper" }).stt(new Int16Array(10));
    expect(Array.isArray(audio)).toBe(true);
  });

  it("toBytes normalizes binding outputs", async () => {
    const b = Uint8Array.from([1, 2, 3]);
    expect(Array.from(await toBytes(b.buffer))).toEqual([1, 2, 3]);
    expect(Array.from(await toBytes({ audio: btoa("\u0001\u0002") }))).toEqual([1, 2]);
    await expect(toBytes(42)).rejects.toThrow();
  });
});

describe("OpenAI provider", () => {
  it("TTS requests raw pcm (24 kHz) and resamples to 16 kHz; STT posts a WAV", async () => {
    const seen: any[] = [];
    const fetch = (async (url: string, init: any) => {
      seen.push({ url, init });
      if (url.endsWith("/audio/speech")) return new Response(pcm16ToBytes(floatToPcm16(new Float32Array(2400).fill(0.25))));
      return Response.json({ text: "transcribed" });
    }) as unknown as typeof globalThis.fetch;
    const v = new OpenAiVoice({ apiKey: "sk-test", fetch });
    const pcm = await v.tts("hey", {});
    expect(pcm.length).toBe(1600);
    expect(JSON.parse(seen[0].init.body)).toMatchObject({ input: "hey", response_format: "pcm", voice: "alloy" });
    expect(seen[0].init.headers.authorization).toBe("Bearer sk-test");
    expect(await v.stt(new Int16Array(16))).toBe("transcribed");
    const file = (seen[1].init.body as FormData).get("file") as Blob;
    expect(parseWav(new Uint8Array(await file.arrayBuffer())).rate).toBe(16000);
  });
  it("requires a key", () => {
    expect(() => new OpenAiVoice({ apiKey: "" })).toThrow(/OPENAI_API_KEY/);
  });
});

describe("none provider + selection", () => {
  it("none returns silence sized to the text and a fixed STT marker", async () => {
    const v = new NoneVoice();
    const pcm = await v.tts("one two three four", {});
    expect(pcm.length).toBe(16000);
    expect(pcm.every((x) => x === 0)).toBe(true);
    expect(await v.stt(new Int16Array(1))).toBe(STT_UNAVAILABLE);
  });
  it("createVoice picks by VOICE_PROVIDER, defaulting on the AI binding", () => {
    const ai = { run: async () => ({}) };
    expect(createVoice({}).name).toBe("none");
    expect(createVoice({ AI: ai }).name).toBe("workers-ai");
    expect(createVoice({ AI: ai, VOICE_PROVIDER: "none" }).name).toBe("none");
    expect(createVoice({ VOICE_PROVIDER: "openai", OPENAI_API_KEY: "k" }).name).toBe("openai");
    expect(() => createVoice({ VOICE_PROVIDER: "workers-ai" })).toThrow();
    expect(() => createVoice({ VOICE_PROVIDER: "x" })).toThrow();
  });
});
