// `none`: no speech backend. TTS yields silence sized to the text (so the
// streaming path still runs end to end); STT yields a fixed marker.
import type { VoiceProvider } from "./types.js";

export const STT_UNAVAILABLE = "[stt unavailable]";

export class NoneVoice implements VoiceProvider {
  readonly name = "none";
  async tts(text: string): Promise<Int16Array> {
    const words = text.trim().split(/\s+/).filter(Boolean).length;
    const ms = Math.min(10000, Math.max(300, words * 250));
    return new Int16Array((ms * 16000) / 1000);
  }
  async stt(): Promise<string> {
    return STT_UNAVAILABLE;
  }
}
