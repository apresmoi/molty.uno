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
  /**
   * Optional streaming STT: opened when a listen starts, fed every device mic
   * chunk as it arrives, finalized at the listen's end, so the transcript is
   * ready moments after speech ends instead of after a batch upload. Absent =
   * the relay buffers the listen and calls stt() once. May throw when the
   * backend is unreachable (the relay then uses the batch path or fails).
   */
  sttStream?(opts?: SttStreamOptions): SttStream;
}

export interface SttStreamOptions {
  /**
   * The backend's semantic end-of-turn (e.g. Kyutai's pause prediction): the human has
   * finished, so the listen can end now instead of at the device's silence timer or its
   * window. Called at most once, only while the stream is still open.
   */
  onTurnEnd?: () => void;
}

/** One streaming transcription (VoiceProvider.sttStream). */
export interface SttStream {
  /** A mic chunk, PCM16 mono @16 kHz, in order. Ignored after end() / abort() or a failure. */
  push(pcm: Int16Array): void;
  /** No more audio: resolves to the final transcript (rejects when the backend failed). */
  end(): Promise<string>;
  /** Drop it: nobody wants the transcript (no speech, the listen failed). Idempotent. */
  abort(): void;
}
