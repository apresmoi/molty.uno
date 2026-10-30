// The voice seam. Providers return / accept PCM16 mono @16 kHz only; all
// format conversion happens inside the provider (see src/core/audio.ts).

export interface TtsOptions {
  voice?: string;
  /**
   * Aborted when nobody will read the audio any more (segment timeout, the
   * consumer gave up, the turn failed). Providers cancel their request / stream
   * on it so a stalled backend does not keep running.
   */
  signal?: AbortSignal;
}

export interface VoiceProvider {
  readonly name: string;
  tts(text: string, opts: TtsOptions): Promise<Int16Array>;
  /**
   * Optional: the same audio as tts(), yielded in pieces as the backend
   * produces it, so the relay can start streaming to the device before the
   * whole segment is synthesised. Absent = the relay calls tts() per segment.
   */
  ttsStream?(text: string, opts: TtsOptions): AsyncIterable<Int16Array>;
  stt(pcm: Int16Array): Promise<string>;
}
