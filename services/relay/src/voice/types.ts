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
  /**
   * Word timing, when the backend knows it (the voice box: Kyutai's TTS): word `i` of
   * this text (0-based, whitespace-split) is spoken from `startS` seconds into its audio.
   * Called in order, before or while that audio arrives. Backends without timings never
   * call it (the relay then estimates).
   */
  onWord?: (i: number, startS: number) => void;
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
   * window. Called at most once, only while the stream is still open. `reason`
   * "no_speech": nobody spoke within `noSpeechMs` (only when it was given) — the
   * other way a turn ends.
   */
  onTurnEnd?: (reason?: "no_speech") => void;
  /**
   * Nobody-spoke timeout for the backend (ms): given when the device's own VAD is off
   * and the backend decides the end of the turn. Absent: the backend never calls it.
   */
  noSpeechMs?: number;
  /** The stream failed while it was still open (the backend went away mid-listen). */
  onFail?: () => void;
  /**
   * A listener hint from the backend while the human talks (voice box `stt.hint`): `pause` (a
   * mid-turn pause began) or `question` (a word ending in "?"). Only while the stream is open.
   */
  onHint?: (k: "pause" | "question") => void;
}

/** One streaming transcription (VoiceProvider.sttStream). */
export interface SttStream {
  /** A mic chunk, PCM16 mono @16 kHz, in order. Ignored after end() / abort() or a failure. */
  push(pcm: Int16Array): void;
  /** No more audio: resolves to the final transcript (rejects when the backend failed). */
  end(): Promise<string>;
  /** Drop it: nobody wants the transcript (no speech, the listen failed). Idempotent. */
  abort(): void;
  /** The backend's own trace of this transcription (voice box `stt.trace`, the latest), if any. */
  trace?(): Record<string, unknown> | undefined;
}
