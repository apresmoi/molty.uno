// The voice seam. Providers return / accept PCM16 mono @16 kHz only; all
// format conversion happens inside the provider (see src/core/audio.ts).

export interface TtsOptions {
  voice?: string;
}

export interface VoiceProvider {
  readonly name: string;
  tts(text: string, opts: TtsOptions): Promise<Int16Array>;
  stt(pcm: Int16Array): Promise<string>;
}
