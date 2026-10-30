// Voice provider selection. VOICE_PROVIDER = workers-ai | openai | none.
// Default: workers-ai when an AI binding exists, else none.
import { NoneVoice } from "./none.js";
import { OpenAiVoice } from "./openai.js";
import type { VoiceProvider } from "./types.js";
import { type AiBinding, WorkersAiVoice } from "./workers-ai.js";

export type { TtsOptions, VoiceProvider } from "./types.js";

export interface VoiceEnv {
  VOICE_PROVIDER?: string;
  AI?: AiBinding;
  TTS_MODEL?: string;
  STT_MODEL?: string;
  TTS_VOICE?: string;
  OPENAI_API_KEY?: string;
  OPENAI_BASE_URL?: string;
}

export function createVoice(env: VoiceEnv): VoiceProvider {
  const kind = env.VOICE_PROVIDER || (env.AI ? "workers-ai" : "none");
  switch (kind) {
    case "workers-ai":
      if (!env.AI) throw new Error("VOICE_PROVIDER=workers-ai needs the AI binding");
      return new WorkersAiVoice(env.AI, { ttsModel: env.TTS_MODEL, sttModel: env.STT_MODEL, defaultVoice: env.TTS_VOICE });
    case "openai":
      return new OpenAiVoice({ apiKey: env.OPENAI_API_KEY ?? "", baseUrl: env.OPENAI_BASE_URL, ttsModel: env.TTS_MODEL, sttModel: env.STT_MODEL, defaultVoice: env.TTS_VOICE });
    case "none":
      return new NoneVoice();
    default:
      throw new Error(`unknown VOICE_PROVIDER ${kind}`);
  }
}
