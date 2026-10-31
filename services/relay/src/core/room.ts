// DeviceRoom: one per device_id. Owns TOFU device auth, pairing, agent tokens,
// command routing and say/listen jobs. Host-neutral: the host supplies
// sockets, storage, a clock and a voice provider.
//
// Hibernation rule: nothing that decides
// admission lives in instance fields. Which socket is the authenticated
// device is read from each socket's own metadata (the DO's serialized
// attachment); device key, link code, tokens, pairings and jobs live in KV.
// Instance fields below only hold work that is in flight -- and in-flight
// work (an open agent request, a pending timer, an incoming frame) keeps
// the host awake. If the host is evicted anyway, jobs left "running" with
// no in-memory task are reported as failed:"interrupted" instead of hanging.
//
// Agent events (shake/tap/button, and one `answer` per ended ask) are storage-backed: an `ev:` ring, a
// per-token read cursor `cur:<token hash>` and a webhook delivery queue
// `whq:` driven by the host's alarm. All survive hibernation.
//
// The agent skill is not the relay's: it ships as the molty plugin (skills/molty/).
// Agents read the contract they talk to from `api_version` (core/version.ts).
//
// Voice box (voice/remote.ts, docs/AGENT-LINK.md "Voice box"): an owner's own STT/TTS
// machine pairs like an agent (role "voice", same tap) and dials the voice socket. Its
// socket is marked stage "voice" in its own metadata (never "live": no device code path
// sees it); settings (`voicecfg`), the box's hello (`vbox`) and its last latencies
// (`vstat`) live in KV. Each say/listen picks a provider by `voice_mode` (pickVoice).
//
// Shake to talk (core/turns.ts, docs/AGENT-LINK.md "Shake to talk"): a device shake event
// naming a listen the device began by itself is adopted as a listen job (normal voice path);
// heard speech becomes ONE `utterance` event and the latest turn (`turn` in KV), which the
// first agent to say / talk claims. Section "Shake to talk" below; hooks in onEvt, command,
// talk, runSay, runTalk, runListen.
//
// Firmware updates (core/updates.ts, core/releases.ts): the last hello's fw /
// board / caps are kept (`devinfo`) so molty_updates answers while offline. A
// token gets one `fw_notice` per newer release (`fw_seen`). molty_update is a
// storage-driven job (`upd`): device `update` events move its phase, and the
// device's next hello settles it (new fw = done), so it survives eviction and
// the device's own reboot.

import {
  type AgentApi,
  type EventsRequest,
  type FirmwareNotice,
  type JobView,
  type Outcome,
  type OwnerApi,
  type PairRequest,
  type PairResult,
  TERMINAL,
  VOICE_MODES,
  type VoiceMode,
  type VoiceOwnerRequest,
  type WebhookRequest,
  type ActivityOwnerRequest,
} from "./api.js";
import { durationMs, bytesToPcm16, levels, pcm16ToBytes } from "./audio.js";
import { WORKING_ACTIONS, WORKING_CAP, WORKING_MOOD, ACK_CAP, ACK_MODES, GESTURES_CAP, type AskArgs, type SayMarks, COMMANDS, BOX_NO_SPEECH_MS, CommandError, DEVICE_NO_SPEECH_MS, LEGACY_MAX_LISTEN_MS, LISTEN_CEILING_MS, LISTEN_WINDOW_CAP, TALK_BUDGET_MS, THINK_HOLD_S, type TalkArgs, validateAsk, validateCommand, validateTalk, MAX_DRAFTS_PER_TURN, REACT_CAP, REACT_MIN_GAP_MS, REACT_SHOW_MS, validateDraft, validateReact } from "./commands.js";
import { PcmFeed, estimateSpeechMs, splitSpeech, synthesize } from "./speech.js";
import { DEFAULT_PARTIAL_POLICY, DRAFT_DROPPED_EVENT, DRAFT_USED_EVENT, type DropReason, PARTIAL_EVENT, PartialChunker, PauseTracker, judgeDraft, words } from "./partials.js";
import { CALIBRATION, type DraftOutcome, type SpeechStats, addOutcome, addTurn, emptyStats, markMisfire, policyFor, speechSummary } from "./calibration.js";
import { NOTES_PREFIX, type NotesRequest, notesKey, notesOp } from "./notes.js";
import { b64Decode, b64urlEncode, randomBytes, randomDigits, safeEqual, sha256Hex } from "./crypto.js";
import {
  CLOSE_FORGOTTEN,
  CLOSE_PROTOCOL,
  CLOSE_REPLACED,
  CLOSE_UNAUTHORIZED,
  LINK_CODE_RE,
  MAX_LISTEN_BYTES,
  PONG_TEXT,
  SAMPLE_RATE,
  type DecodedAudio,
  decodeAudioFrame,
  encodeAudioFrame,
  isRecord,
  parseHello,
  parseTextFrame,
  type JsonObject,
} from "./protocol.js";
import { type Clock, type KV, sleep } from "./store.js";
import { type AgentEvent, DEFAULT_WEBHOOK_EVENTS, INBOX_LIMITS, SAMPLE_EVENTS, UTTERANCE_EVENT, agentEventData, eventId, webhookWants } from "./events.js";
import { LISTEN_ADOPT_FEATURE, LOCAL_LISTEN_RE, type TurnClaim, type TurnRecord, claimTurn, turnTimings, utteranceData } from "./turns.js";
import { API_VERSION, FEATURES, compareVersions } from "./version.js";
import type { ReleaseLookup, ReleaseSource } from "./releases.js";
import { type DeviceFacts, type FirmwareBlock, CONSOLE_HINT, firmwareBlock, howToUpdate, installable, tooOld, updateActions } from "./updates.js";
import {
  type DeliveryError,
  type GuardResult,
  type RenderedRequest,
  type Resolver,
  WEBHOOK_LIMITS,
  WebhookFailure,
  categorize,
  type WebhookRecord,
  guardWebhookUrl,
  newWebhookSecret,
  parseWebhookSpec,
  redactRendered,
  redactUrl,
  redactWebhook,
  renderRequest,
  signatureHeader,
} from "./webhook.js";
import type { SttStream, VoiceProvider } from "../voice/types.js";
import { ACTIVITY_LIMITS, ACTIVITY_PREFIX, ActivityLog, type Who, jobDraft, parseActivityQuery } from "./activity.js";
import { BOX_OFFLINE, RemoteVoiceProvider } from "../voice/remote.js";
import {
  type AgentPhrases,
  BUILTIN_PHRASES,
  FILL_SKIP_MS,
  PHRASE_LIMITS,
  PHRASE_PREFIXES,
  type Phrase,
  type PhraseAudio,
  PhraseCache,
  builtin,
  fillerMs,
  mergePhrases,
  pickFiller,
  validatePhrasesRequest,
  CLASSIFY_BUDGET_MS,
  type UtteranceClassifier,
} from "./phrases.js";
import { ListenTrace, SeqOrder, TRACE_LIMITS, capTrace } from "./trace.js";
import { AUDIO_KEEP, AUDIO_PREFIX, AudioKeep, wav } from "./audio-keep.js";
import { DEFAULT_LIVELINESS, EngineBench, LIVELINESS, type Liveliness, REFLEX, REFLEX_MODES, REFLEX_SHOW, REFLEX_SOFT, type ReflexEngine, isLiveliness, type ReflexMode, type ReflexPick, ReflexSession, UTTERANCE_CATEGORIES, type UtteranceCategory, categoryOf, keywordCategory, parseBoxReflex, resolveEngine } from "./reflex.js";

export interface SocketMeta {
  /** hello: device not yet authenticated; live: the device; voice: a voice box (authenticated at upgrade). */
  stage: "hello" | "live" | "voice";
  opened: number;
  fw?: string;
  caps?: string[];
  board?: string;
  since?: number;
  /** voice: the box token's hash (revocation closes the socket). */
  tok?: string;
  /** voice: what the box offers, set by its voice.hello (absent = not greeted yet: not used). */
  box?: { stt: boolean; tts: boolean; reflex?: boolean };
  /**
   * voice: replaced / revoked / unpaired. Set BEFORE the socket is closed, in its own
   * serialized metadata (survives hibernation): the host may keep listing a closing socket
   * (Cloudflare's getWebSockets() includes CLOSING ones), and it must never be picked again.
   */
  retired?: boolean;
}

export interface DeviceSocket {
  sendText(text: string): void;
  sendBinary(bytes: Uint8Array): void;
  close(code: number, reason: string): void;
  getMeta(): SocketMeta | undefined;
  setMeta(meta: SocketMeta): void;
  same(other: DeviceSocket): boolean;
}

export interface SocketHost {
  sockets(): DeviceSocket[];
}

export interface RoomTimings {
  cmdTimeoutMs: number;
  helloTimeoutMs: number;
  pairWindowMs: number;
  ttsTimeoutMs: number;
  sttTimeoutMs: number;
  listenGraceMs: number;
  jobTtlMs: number;
  /** say streaming: samples per frame (<= 8192 for the 16 KB payload cap). */
  sayChunkSamples: number;
  /** say streaming: frames sent ahead of real time to fill the device ring buffer. */
  sayLeadFrames: number;
  /** talk: after the stream's playback end, how long to wait for the device's say.done before opening the mic. */
  sayDoneGraceMs: number;
  /** Automatic `thinking` hold after end of speech. */
  thinkHoldS: number;
  /** Longest a blocking talk call holds the request before answering {status:"pending"}. */
  talkBudgetMs: number;
  /** A talk heard the human and nobody read the result after this: it goes out as an `utterance`. */
  collectGraceMs: number;
  /** ask: after the device's own timeout_ms, how long the relay waits for its `answer` before resolving `timeout` itself. */
  askGraceMs: number;
  /** update: how long the on-screen "update?" waits for a tap (sent as timeout_ms); the relay adds askGraceMs. */
  updateConfirmMs: number;
  /** update: from the human's accept until the device is back on the new firmware. */
  updateMs: number;
  /** voice box: after the listen ended, how long its streaming STT may take for the final text. */
  boxSttFinalMs: number;
  /** react: how long a react's mood shows before Molty returns to listening. */
  reactShowMs: number;
  /** partials: none before an open turn ran this long (PartialPolicy.lead_ms; a shorter turn gets none). */
  partialLeadMs: number;
}

export const DEFAULT_TIMINGS: RoomTimings = {
  cmdTimeoutMs: 6000, // under the Go CLI's 8 s per-call budget and the common 10 s agent tool-call cap
  helloTimeoutMs: 10000,
  pairWindowMs: 120000,
  ttsTimeoutMs: 20000,
  sttTimeoutMs: 30000,
  listenGraceMs: 5000,
  jobTtlMs: 15 * 60 * 1000,
  sayChunkSamples: 5120, // 320 ms, 10240 B
  sayLeadFrames: 3,
  sayDoneGraceMs: 2500,
  thinkHoldS: THINK_HOLD_S,
  talkBudgetMs: TALK_BUDGET_MS,
  collectGraceMs: 5000,
  askGraceMs: 5000,
  updateConfirmMs: 60000,
  updateMs: 5 * 60 * 1000,
  boxSttFinalMs: 5000,
  reactShowMs: REACT_SHOW_MS,
  partialLeadMs: DEFAULT_PARTIAL_POLICY.lead_ms,
};

/** Firmware cap: say {cues:true} and frames with "w" word cues (the text follows the voice). */
const SAY_CUES_CAP = "say_cues";
/** Firmware cap: `cmd say_cancel {job}` drops a say the relay gave up on. */
const SAY_CANCEL_CAP = "say_cancel";
/** Firmware cap (with say_cues): say {marks:[[word, name]...]} fires each [mood]/[action] cue of the text at its word's cue. */
const SAY_MARKS_CAP = "say_marks";
/** Word cues one frame header carries (each ≤ 17 B: the header stays under 512 B). */
const SAY_CUES_PER_FRAME = 20;
/** Words of one say the device can cue (firmware LINK_CUE_WORDS): later words go uncued. */
const SAY_CUE_WORDS = 512;
/** say: a first frame shorter than a full chunk waits at most this long for more audio... */
const FIRST_FRAME_HOLD_MS = 100;
/**
 * ...or until it holds this much (160 ms). The device plays from its first frame; the voice
 * box (Kyutai) delivers 80 ms of audio every ~25 ms (3.2x real time, measured on the 4090,
 * also with the lookahead segment running), so 160 ms in hand leaves ~130 ms of slack for
 * the network where a full 320 ms chunk cost ~75 ms of waiting.
 */
const FIRST_FRAME_MIN_SAMPLES = 2560;

/** Room-side cap on one job wait; surfaces apply their own (molty_wait / GET /v1/jobs: 8 s). */
const JOB_WAIT_CAP_MS = 25000;

export interface RoomDeps {
  deviceId: string;
  kv: KV;
  clock: Clock;
  voice: VoiceProvider;
  host: SocketHost;
  timings?: Partial<RoomTimings>;
  log?: (msg: string, data?: Record<string, unknown>) => void;
  /** Host alarm: run `room.alarm()` at (or soon after) `at`. Replaces any earlier schedule. Without it webhooks are not delivered. */
  scheduler?: { setAlarm(at: number): Promise<void> };
  /** Outbound fetch for webhooks (default: global fetch). */
  fetch?: (req: Request) => Promise<Response>;
  /** Resolves a webhook host to its addresses for the SSRF guard; absent = literal checks only. */
  resolveHost?: Resolver;
  /** Latest firmware release (core/releases.ts); absent = always "unavailable". */
  releases?: ReleaseSource;
  /** The reflex's model engines (core/reflex.ts): Worker secrets TYPESAFE_API_KEY / OPENAI_API_KEY. Never logged. */
  reflexKeys?: { jev?: string; openai?: string };
  /**
   * Classifies the human's utterance for the auto filler (question / request / hard / other: core/phrases.ts
   * UtteranceClassifier). Default: Molty's reflex (fillerCategory: the owner's engine, keywords when off). Tests override it.
   */
  classifier?: UtteranceClassifier;
}

interface DeviceRecord {
  key_hash: string;
  claimed_at: number;
}
interface TokenRecord {
  agent_name: string;
  created: number;
  /** "voice": a voice box (opens only the voice socket). Absent = an agent (every record before roles). */
  role?: "voice";
  webhook?: WebhookRecord;
  /**
   * Webhook configuration revision: +1 on every successful set and delete, so
   * "never configured" and "deleted" differ. Registrations compare it inside
   * one KV.update (atomic read-check-write). Revocation deletes the record.
   */
  wh_rev?: number;
  /** Legacy (skill versions, removed): may still sit on tokens minted before; ignored. */
  skill_seen?: number;
  /** Newest firmware release this agent was told about (fw_notice / molty_updates). */
  fw_seen?: string;
}

/** The last hello's identity, kept for while the device is offline. */
interface DevInfo {
  fw: string | null;
  board: string | null;
  caps: string[] | null;
  at: number;
}

/** The one molty_update in flight (storage-driven: survives eviction and the device's reboot). */
interface UpdateRecord {
  job: string;
  from: string;
  to: string;
  /** confirm (on-screen question) | accepted | downloading | flashing | rebooting */
  phase: string;
  deadline: number;
  /** false: the `cmd update` ack never came (delivery unknown); tracked through the confirmation deadline anyway. */
  acked?: boolean;
}

type WebhookWrite = "ok" | "gone" | "conflict";
interface Delivery {
  tok: string;
  seq: number;
  attempt: number;
  due: number;
}
interface RateRecord {
  start: number;
  n: number;
  dropped: number;
}
interface PairRecord {
  id: string;
  req: string;
  code: string;
  agent_name: string;
  /** A voice box pairing (absent = agent). */
  role?: "voice";
  created: number;
  expires: number;
  status: "pending" | "approved" | "rejected" | "timeout";
  token?: string;
}
interface JobRecord extends JobView {
  created: number;
  updated: number;
  /** The agent that started it (activity log attribution; never in a JobView). */
  by?: Who;
}

interface PendingCmd {
  resolve(res: Outcome<JsonObject>): void;
  timer: unknown;
}

interface ListenBuffer {
  job: string;
  chunks: Uint8Array[];
  bytes: number;
  ended: boolean;
  wake?: () => void;
  firstAt?: number;
  endAt?: number;
  /** Device end-of-speech verdict from the end frame (absent on older firmware). */
  reason?: string;
  speech_ms?: number;
  /** The automatic `thinking` mood was sent at end of speech. */
  thinking?: boolean;
  /** The device acknowledges this listen's end itself (cap `ack`; from the listen's ack): with
   *  it the relay sends no automatic `thinking`, and `idle` on an empty transcript only when the
   *  device put thinking up (not "off"). Absent: older firmware, the relay's moods as before. */
  deviceAck?: (typeof ACK_MODES)[number];
  /** `screenRev` when this listen began: automatic moods apply only while it is unchanged. */
  screenRev: number;
  /** Streaming STT (voice box): every chunk is pushed here as it arrives. */
  stream?: SttStream;
  /** When the STT backend called the end of the human's turn (the listen ended there). */
  turnEndAt?: number;
  /** Semantic end of turn is THE end: the device's VAD was turned off for this listen (voice box + listen_stop). */
  semantic?: boolean;
  /** The streaming STT failed while the listen was open. */
  boxFailed?: boolean;
  /** The device's socket closed (or was replaced) before its end frame. */
  lost?: boolean;
  /** The device ended the listen with reason "error": its capture failed. */
  captureFailed?: boolean;
  /** The auto filler delay for this turn (core/phrases.ts fillerMs of the listen's ack). */
  fillerMs?: number;
  /** The turn trace being gathered (core/trace.ts). */
  trace: ListenTrace;
  /** Frames in seq order (the device resends what failed; core/trace.ts SeqOrder). */
  order: SeqOrder<DecodedAudio>;
  /** Gives a seq gap up after TRACE_LIMITS.gapMs. */
  gapTimer?: unknown;
  /** The device's own end frame arrived (its `tr` summary is in the trace). */
  devEnd?: boolean;
  /** The relay ended the listen (the box's turn end / no_speech / failure): the device's end frame comes after. */
  relayEnded?: boolean;
  /** DeviceRoom.roomGen when it began: a forget since then voids its delayed writes. */
  roomGen: number;
  /** The box's word arrivals (its stt.partial): the human's pauses, for the calibration. */
  pauses: PauseTracker;
  /** Molty's own reactions to what the human says (core/reflex.ts), when the owner turned it on. */
  reflex?: ReflexSession;
}

/** A finished listen's trace, until its job's activity entry is written (finishJob). */
interface JobTrace {
  trace: ListenTrace;
  audio: boolean;
  /** Resolves once the device's end frame came (or TRACE_TAIL_MS passed): absent = it already had. */
  tail?: Promise<void>;
}

/**
 * After a listen ended without the device's end frame (the box's turn end: the device stops
 * within ~100 ms), its frames still count for the trace — header diagnostics only, never audio
 * — for at most this long; the job's activity entry waits for it (the agent's result does not).
 */
const TRACE_TAIL_MS = 3000;

/** Owner voice settings (KV `voicecfg`). */
interface VoiceCfg {
  mode: VoiceMode;
  /** Default voice for the box when the agent gives none. */
  voice?: string;
  /** Molty's reflex (core/reflex.ts): which engine classifies what the human says (absent = off). */
  reflex?: ReflexMode;
  /** Reflex compare mode: every available engine runs on each chunk (trace), only `reflex` fires. */
  reflex_compare?: boolean;
  /** How lively the reflex is (core/reflex.ts REFLEX_LIVELINESS; absent = DEFAULT_LIVELINESS). */
  liveliness?: Liveliness;
}
/** The connected box's voice.hello (KV `vbox`). */
interface BoxInfo {
  name: string;
  version: string;
  voices: string[];
  stt: boolean;
  tts: boolean;
  /** It runs the reflex classifier (stt.reflex). */
  reflex?: boolean;
  since: number;
}
/** The box's last measured latencies (KV `vstat`). */
interface BoxStats {
  stt_ms?: number;
  stt_at?: number;
  tts_first_audio_ms?: number;
  tts_at?: number;
}
/** Which provider serves one say / listen. */
type VoicePick = { ok: true; provider: VoiceProvider; name: string; mode: VoiceMode; box: boolean; cfg: VoiceCfg } | { ok: false; error: string; message: string };

type JobKind = "say" | "listen" | "talk" | "ask" | "update" | "quick";

/** How an ask ended: a tap (index + label) or no answer (reason). */
type AskAnswer = { index: number; label: string; reason?: undefined } | { index: null; label: null; reason: "timeout" | "cancelled" | "offline" };
interface PendingAsk {
  options: string[];
  resolve(a: AskAnswer): void;
}
type SpeakResult = { ok: true; start: number; ms: number; timings: JsonObject } | { ok: false; error: string };
type ListenResult = (
  | { ok: true; start: number; text: string; reason: string; speech_ms?: number; duration_ms: number; peak: number; rms: number; timings: JsonObject; ack?: string; pauses: PauseTracker; endAt: number; fillerMs: number }
  | { ok: false; error: string }
) & { trace?: ListenTrace; audio?: boolean };

/** Speech whose synthesis already started (a draft's opener, a queued follow-up, a quick phrase): speak() plays it. */
interface Prepared {
  pick: Extract<VoicePick, { ok: true }>;
  feed: PcmFeed;
  at: number;
  /** The whole audio came from the phrase cache (core/phrases.ts): no TTS at all. */
  cached?: boolean;
  /** ...synthesised just now on a cache miss (then cached). */
  fresh?: boolean;
}

/** The relay itself, as the owner of a lead-in (Molty's own filler): anyone's answer may follow it. */
const RELAY_LEAD = "relay";

/**
 * The short lead-in playing now — a draft's opener, a quick phrase, or Molty's own filler — and who
 * owns it: that agent's next say / talk / quick (anyone's, behind the relay's filler) queues behind it
 * (startJob `after`) and starts the moment it ends. One mechanism for all three.
 */
interface LeadPlay {
  job: string;
  /** An agent's token hash, or RELAY_LEAD. */
  hash: string;
  /** The job queued behind it. */
  follow?: string;
}

/** The auto filler armed for one human turn (in flight: a timer). */
interface FillWatch {
  turn: string;
  timer: unknown;
  /** agentSeq when armed: anything an agent does after that stands the filler down. */
  seq: number;
  ack?: string;
  /** When the human stopped (relay clock). */
  endAt: number;
  /** The utterance's category for a context-picked filler (classified when armed, in parallel with the delay). */
  category: Promise<{ category: UtteranceCategory; by: string }>;
  /** The classifier's verdict once in (fireFiller never waits for it: not in yet → the keywords). */
  settled?: { category: UtteranceCategory; by: string };
  /** The utterance (the keywords fallback when the classifier is not in by the filler's moment). */
  text: string;
  /** The filler pool, looked up when armed (in parallel with the delay). */
  pool: Promise<{ pool: readonly Phrase[]; source: "agent" | "builtin"; owner?: string } | undefined>;
}

/** One agent's held draft for the open turn (core/partials.ts). */
interface HeldDraft {
  hash: string;
  by: Who;
  based_on: number;
  basedText: string;
  opener: string;
  max_new_words: number;
  voice?: string;
  prepared: Prepared;
  at: number;
  /** Drafts this agent made this turn (replacements included). */
  n: number;
}

/**
 * The open human turn: a shake's adopted listen, from its adoption until its utterance (or its
 * end without one). While it is open agents may only draft and react (human_speaking otherwise).
 */
interface OpenTurn {
  id: string;
  startAt: number;
  chunker: PartialChunker;
  timer?: unknown;
  drafts: Map<string, HeldDraft>;
  /** An agent reacted during this turn: the reflex yields to it for the rest of the turn. */
  agentReacted?: boolean;
}

/** The opener being spoken for a turn, and who owns it (their follow-up queues behind it). */
interface OpenerPlay {
  job: string;
  turn: string;
  hash: string;
  /** The draft outcome's time in the stats (a misfire turns that `used` around). */
  usedAt: number;
  /** When the opener's job ended. */
  endedAt?: number;
  /** Its owner's follow-up queued behind it (startJob `after`). */
  follow?: string;
  /** A misfire was counted for it already. */
  counted?: boolean;
}

function concat(a: Int16Array, b: Int16Array): Int16Array {
  if (a.length === 0) return b;
  const out = new Int16Array(a.length + b.length);
  out.set(a, 0);
  out.set(b, a.length);
  return out;
}

const K = {
  device: "device",
  link: "link",
  pairCurrent: "paircur",
  token: (hash: string) => `tok:${hash}`,
  pair: (id: string) => `pair:${id}`,
  job: (id: string) => `job:${id}`,
  evSeq: "evseq",
  evRate: "evrate",
  ev: (seq: number) => `ev:${String(seq).padStart(12, "0")}`,
  cur: (hash: string) => `cur:${hash}`,
  whq: (hash: string, seq: number) => `whq:${hash}:${String(seq).padStart(12, "0")}`,
  /** Legacy keys (skill versions, removed): only cleared on reset. */
  skillEv: "skillev",
  origin: "origin",
  devinfo: "devinfo",
  upd: "upd",
  voiceCfg: "voicecfg",
  vbox: "vbox",
  vstat: "vstat",
  /** The latest human turn (shake to talk, core/turns.ts). */
  turn: "turn",
  /** An agent's quick phrases (core/phrases.ts). */
  phrases: (hash: string) => `phr:${hash}`,
  /** The token hash of the agent that last answered a human turn (whose fillers Molty uses). */
  lastAnswerer: "fillby",
  /** How this Molty's human talks: the calibration record (core/calibration.ts). */
  speech: "speech",
};

/** Device-facing name of a voice box pairing (the console shows it next to the code). */
const voiceLabel = (name: string) => (/^voice box\b/i.test(name) ? name : `voice box · ${name}`);



const fail = (error: string, message?: string, details?: JsonObject): { ok: false; error: string; message?: string; details?: JsonObject } => ({
  ok: false,
  error,
  ...(message === undefined ? {} : { message }),
  ...(details ? { details } : {}),
});

/**
 * Commands refused while the human is talking (an open shake turn, `human_speaking`): they make a
 * sound or take the screen. (talk / ask likewise; `state` only when it patches.) Agents draft and react instead.
 */
const HUSHED: readonly string[] = ["say", "quick", "listen", "mood", "show", "act"];

/** An event as agents see it: the internal recipient (`to`) never leaves the relay. */
const publicEvent = ({ to: _to, ...ev }: AgentEvent): AgentEvent => ev;

/** Update phases a device reports between accept and its reboot. */
const UPDATE_PHASES = ["accepted", "downloading", "flashing", "rebooting"];

const NO_RELEASES: ReleaseSource = { latest: async () => ({ release: null, state: "unavailable", checked_at: null }) };

export class DeviceRoom implements AgentApi, OwnerApi {
  readonly deviceId: string;
  private readonly kv: KV;
  private readonly clock: Clock;
  private readonly voice: VoiceProvider;
  private readonly host: SocketHost;
  private readonly t: RoomTimings;
  private readonly log: NonNullable<RoomDeps["log"]>;
  private readonly scheduler: RoomDeps["scheduler"];
  private readonly fetcher: NonNullable<RoomDeps["fetch"]>;
  private readonly resolveHost: RoomDeps["resolveHost"];
  private readonly releases: ReleaseSource;
  /** An override of the filler's classifier (RoomDeps.classifier); absent = Molty's reflex (fillerCategory). */
  private readonly classifier: UtteranceClassifier | undefined;
  /** The voice box, spoken to over whichever voice socket is live (in-flight requests only). */
  private readonly box: RemoteVoiceProvider;
  /** The activity log (core/activity.ts): every hook below is one `this.activity.*` call. */
  readonly activity: ActivityLog;

  // ---- in-flight work only (see header comment) ----
  private readonly pendingCmds = new Map<string, PendingCmd>();
  private readonly pairWaiters = new Map<string, Set<() => void>>();
  private readonly jobWaiters = new Map<string, Set<() => void>>();
  private readonly tasks = new Set<string>();
  private readonly sayDone = new Map<string, () => void>();
  /** Asks shown (or being shown) on the device, by job: the device's `answer` resolves them. */
  private readonly pendingAsks = new Map<string, PendingAsk>();
  private listenBuf: ListenBuffer | undefined;
  private audioJob: string | undefined;
  /**
   * Bumped by every command that took the screen after a listen began: an explicit `mood`
   * (any agent) the device accepted, and an `ask` the device showed (or may have: no ack in
   * time). Automatic moods are a listen's own housekeeping: they never overwrite a newer
   * explicit mood, and never cancel a newer question (any mood cancels a pending ask).
   */
  private screenRev = 0;
  /** Explicit `mood` / `ask` commands sent but not yet answered: no automatic mood may overtake one on the wire. */
  private screenInFlight = 0;
  private readonly lastEvents = new Map<string, { data: unknown; at: number }>();
  private readonly inboxWaiters = new Map<string, Set<() => void>>();
  /** Outstanding long-polls per token hash (admission control). */
  private readonly polls = new Map<string, number>();
  private evChain: Promise<void> = Promise.resolve();
  private cmdSeq = 0;
  /** The owner's kept listen audio (core/audio-keep.ts). */
  private readonly audioKeep: AudioKeep;
  /** Shakes whose listen awaits adoption, by the device's local listen id (turn trace). */
  private readonly shakes = new Map<string, { at: number; data: JsonObject }>();
  /** Finished listens' traces by job, until finishJob writes them (core/trace.ts). */
  private readonly jobTraces = new Map<string, JobTrace>();
  /** The last listen that ended before the device's end frame: its late frames' diagnostics (TRACE_TAIL_MS). */
  private traceTail: { buf: ListenBuffer; done: () => void; over: Promise<void> } | undefined;
  /**
   * Bumped FIRST by forget: work that finishes later (a job's delayed activity entry, a trace
   * tail, kept audio) checks it and writes nothing for a forgotten device.
   */
  private roomGen = 0;
  /** Talk jobs that heard the human, until an agent reads the result (or it goes out as an utterance). */
  private readonly uncollected = new Set<string>();
  /** The fallback has claimed these words; its `delivered_as` marker lands when the promise does. */
  private readonly delivering = new Map<string, Promise<void>>();
  private updTimer: unknown;
  /** The human is talking (an adopted shake listen): agents may only draft and react. */
  private openTurn: OpenTurn | undefined;
  /** A queued job waiting for the audio lock of `after` (the owner's follow-up behind its opener). */
  private handoff: { after: string; job: string; go: () => void; lead?: string } | undefined;
  private opener: OpenerPlay | undefined;
  private lastReactAt = 0;
  private reactTimer: unknown;
  /** The reflex's last reaction (core/reflex.ts), and its model engines refused for good. */
  private lastReflexAt = 0;
  /** The reflex's soft curious glance alternates its side. */
  private reflexGlance = 0;
  private readonly reflexBench = new EngineBench();
  private readonly reflexKeys: { jev?: string; openai?: string };
  /** The latest listen's reflex (its categories outlive the listen: the filler asks right after the turn). */
  private lastReflex: ReflexSession | undefined;
  /** Writes to the calibration record are serialized (read-modify-write from several paths). */
  private speechChain: Promise<unknown> = Promise.resolve();
  /** The lead-in playing now (draft opener, quick phrase, filler): its follow-up queues behind it. */
  private lead: LeadPlay | undefined;
  /** Bumped by everything an agent starts or shows (jobs incl. a draft's opener, mood / show / act / state): the auto filler stands down. */
  private agentSeq = 0;
  private fillWatch: FillWatch | undefined;
  /** The last auto filler: an agent's quick right after it (same turn, nothing else since) is skipped ("filled"). */
  private lastFill: { turn: string; job: string; phrase: string; text: string; at: number; seq: number } | undefined;
  private lastFiller: string | undefined;
  /** Phrase syntheses in flight, by voice + text (deduped). */
  private readonly synthing = new Map<string, Promise<PhraseAudio | undefined>>();
  private readonly phraseCache: PhraseCache;

  constructor(deps: RoomDeps) {
    this.deviceId = deps.deviceId;
    this.kv = deps.kv;
    this.clock = deps.clock;
    this.voice = deps.voice;
    this.host = deps.host;
    this.t = { ...DEFAULT_TIMINGS, ...deps.timings };
    this.log = deps.log ?? (() => {});
    this.scheduler = deps.scheduler;
    this.fetcher = deps.fetch ?? ((req) => globalThis.fetch(req));
    this.resolveHost = deps.resolveHost;
    this.releases = deps.releases ?? NO_RELEASES;
    this.reflexKeys = { ...(deps.reflexKeys?.jev ? { jev: deps.reflexKeys.jev } : {}), ...(deps.reflexKeys?.openai ? { openai: deps.reflexKeys.openai } : {}) };
    this.classifier = deps.classifier;
    this.box = new RemoteVoiceProvider(() => this.boxSocket(), this.clock);
    this.activity = new ActivityLog({ deviceId: this.deviceId, kv: this.kv, clock: this.clock, log: this.log });
    this.audioKeep = new AudioKeep(this.kv, this.clock);
    this.phraseCache = new PhraseCache(this.kv, () => this.clock.now());
  }

  // =====================================================================
  // Device socket side
  // =====================================================================

  socketOpen(sock: DeviceSocket): void {
    const now = this.clock.now();
    sock.setMeta({ stage: "hello", opened: now });
    // Bound unauthenticated sockets: drop stale ones and keep at most 4 waiting.
    const waiting = this.host
      .sockets()
      .filter((s) => !s.same(sock) && s.getMeta()?.stage !== "live" && s.getMeta()?.stage !== "voice")
      .sort((a, b) => (a.getMeta()?.opened ?? 0) - (b.getMeta()?.opened ?? 0));
    for (const [i, s] of waiting.entries()) {
      const stale = now - (s.getMeta()?.opened ?? 0) > this.t.helloTimeoutMs;
      if (stale || waiting.length - i > 3) s.close(CLOSE_PROTOCOL, "hello timeout");
    }
  }

  async socketMessage(sock: DeviceSocket, data: string | Uint8Array): Promise<void> {
    const meta = sock.getMeta();
    if (!meta) return sock.close(CLOSE_PROTOCOL, "unknown socket");
    if (meta.stage === "voice") return this.voiceMessage(sock, meta, data);
    if (meta.stage === "hello") {
      if (this.clock.now() - meta.opened > this.t.helloTimeoutMs) return sock.close(CLOSE_PROTOCOL, "hello timeout");
      const frame = typeof data === "string" ? parseTextFrame(data) : undefined;
      if (!frame || frame.t !== "hello") return sock.close(CLOSE_PROTOCOL, "first frame must be hello");
      return this.onHello(sock, frame);
    }
    if (typeof data !== "string") return this.onAudio(data);
    const frame = parseTextFrame(data);
    if (!frame) return;
    switch (frame.t) {
      case "ping":
        sock.sendText(PONG_TEXT);
        return;
      case "pong":
        return;
      case "res":
        return this.onRes(frame);
      case "evt":
        return this.onEvt(frame);
      case "pair.result":
        return this.onPairResult(frame);
      case "link_code":
        return this.onLinkCode(frame.link_code);
      case "forget":
        return this.onForget(sock);
      case "log":
        return this.activity.deviceLog(frame);
      default:
        return;
    }
  }

  /** `code`: the WebSocket close code, when the host knows it. */
  socketClose(sock: DeviceSocket, code?: number): void {
    const stage = sock.getMeta()?.stage;
    const closed: JsonObject = code === undefined ? {} : { code };
    if (stage === "live") void this.activity.device("disconnect", closed);
    else if (stage === "voice" && sock.getMeta()?.box) void this.activity.record({ ts: this.clock.now(), kind: "voice_box", ev: "disconnect", data: closed });
    // A voice box that goes away fails its own requests at once (auto mode falls back, box mode fails).
    if (sock.getMeta()?.stage === "voice") {
      this.box.drop(sock);
      if (sock.getMeta()?.box) void this.rewarmPhrases(); // auto mode: the cloud's voice speaks now
      return;
    }
    if (sock.getMeta()?.stage !== "live") return;
    // The runtime may still list the closing socket; anything else live is a replacement.
    if (this.liveSocket(sock)) return;
    this.dropInFlight("device_offline");
  }

  private liveSocket(except?: DeviceSocket): DeviceSocket | undefined {
    return this.host.sockets().find((s) => (!except || !s.same(except)) && s.getMeta()?.stage === "live");
  }

  private dropInFlight(error: string) {
    // the device is gone: no end frame will come — a trace tail ends now (its entry without tr)
    this.traceTail?.done();
    for (const [id, p] of this.pendingCmds) {
      this.clock.clearTimeout(p.timer);
      this.pendingCmds.delete(id);
      p.resolve(fail(error));
    }
    if (this.listenBuf && !this.listenBuf.ended) {
      this.listenBuf.ended = true;
      this.listenBuf.lost = true; // the device went away mid-listen: an error, never "nobody spoke"
      this.listenBuf.wake?.();
    }
    // The device that showed the question is gone: nobody can answer it.
    for (const [job, p] of this.pendingAsks) {
      this.pendingAsks.delete(job);
      p.resolve({ index: null, label: null, reason: "offline" });
    }
  }

  private async onHello(sock: DeviceSocket, frame: Record<string, unknown>) {
    const hello = parseHello(frame);
    if (!hello) return sock.close(CLOSE_PROTOCOL, "malformed hello");
    if (hello.id !== this.deviceId) return sock.close(CLOSE_UNAUTHORIZED, "device id mismatch");
    const key = b64Decode(hello.key);
    if (!key || key.byteLength < 16 || key.byteLength > 64) return sock.close(CLOSE_UNAUTHORIZED, "bad device key");
    const hash = await sha256Hex(key);
    const record = await this.kv.get<DeviceRecord>(K.device);
    if (!record) {
      await this.kv.put<DeviceRecord>(K.device, { key_hash: hash, claimed_at: this.clock.now() });
      this.log("device claimed (tofu)", { device: this.deviceId });
    } else if (!safeEqual(record.key_hash, hash)) {
      void this.activity.device("refused", { code: CLOSE_UNAUTHORIZED, reason: "device key mismatch" });
      return sock.close(CLOSE_UNAUTHORIZED, "device key mismatch");
    }
    const previous = this.liveSocket(sock);
    sock.setMeta({ stage: "live", opened: sock.getMeta()?.opened ?? this.clock.now(), fw: hello.fw, caps: hello.caps, board: hello.board, since: this.clock.now() });
    if (previous) {
      previous.close(CLOSE_REPLACED, "replaced by a newer connection");
      this.dropInFlight("device_offline");
    }
    if (hello.link_code) await this.kv.put(K.link, { code: hello.link_code, at: this.clock.now() });
    await this.kv.put<DevInfo>(K.devinfo, { fw: hello.fw ?? null, board: hello.board ?? null, caps: hello.caps ?? null, at: this.clock.now() });
    // features: what this relay does for the device (listen_adopt: shake to talk, core/turns.ts);
    // now: the relay's clock (the device's time sync for log uploads); log: uploads welcome (core/activity.ts)
    sock.sendText(JSON.stringify({ t: "welcome", features: [LISTEN_ADOPT_FEATURE], now: this.clock.now(), log: { max_lines: ACTIVITY_LIMITS.deviceLinesAdvertised } }));
    await this.activity.device("connect", { fw: hello.fw ?? null, board: hello.board ?? null, ...(typeof frame.rssi === "number" ? { rssi: frame.rssi } : {}) });
    // Back from an update's reboot (or a reconnect during one): settle it by the version it runs.
    await this.settleUpdate(hello.fw ?? null);
  }

  private onRes(frame: Record<string, unknown>) {
    if (typeof frame.id !== "string") return;
    const pending = this.pendingCmds.get(frame.id);
    if (!pending) return;
    this.pendingCmds.delete(frame.id);
    this.clock.clearTimeout(pending.timer);
    if (frame.ok === true) {
      pending.resolve({ ok: true, result: isRecord(frame.result) ? (frame.result as JsonObject) : {} });
    } else {
      pending.resolve(fail(typeof frame.error === "string" && frame.error ? frame.error : "device_error"));
    }
  }

  private async onEvt(frame: Record<string, unknown>) {
    if (typeof frame.name !== "string" || frame.name.length > 64) return;
    const data = isRecord(frame.data) ? frame.data : {};
    this.lastEvents.set(frame.name, { data, at: this.clock.now() });
    if (this.lastEvents.size > 32) this.lastEvents.delete(this.lastEvents.keys().next().value!);
    if (frame.name === "say.done" && typeof data.job === "string") this.sayDone.get(data.job)?.();
    if (frame.name === "link_code") await this.onLinkCode(data.link_code);
    if (frame.name === "answer") return this.onAnswer(data); // its inbox event is appended when the ask job ends
    if (frame.name === "update") return this.onUpdateEvent(data); // relay-internal: drives the molty_update job
    if (frame.name === "shake") return this.onShake(data); // may start the shake's listen job
    const clean = agentEventData(frame.name, data);
    if (clean) await this.appendEvent(frame.name, clean);
    if (clean) await this.activity.device(frame.name, clean);
  }

  /**
   * Device `answer`: resolves the pending ask it names, once. Dropped unless the
   * shape is right (events.ts), the job is an ask still waiting here, the index
   * is within its options and the label (when sent) is exactly options[index].
   */
  private onAnswer(data: Record<string, unknown>) {
    const clean = agentEventData("answer", data);
    if (!clean) return;
    const job = clean.job as string;
    const p = this.pendingAsks.get(job);
    if (!p) return;
    let answer: AskAnswer;
    if (clean.index === null) {
      answer = { index: null, label: null, reason: clean.reason as "timeout" | "cancelled" };
    } else {
      const index = clean.index as number;
      if (index >= p.options.length) return;
      if (clean.label !== null && clean.label !== p.options[index]) return;
      answer = { index, label: p.options[index] };
    }
    this.pendingAsks.delete(job);
    p.resolve(answer);
  }

  private async onLinkCode(code: unknown) {
    if (typeof code === "string" && LINK_CODE_RE.test(code)) await this.kv.put(K.link, { code, at: this.clock.now() });
  }

  private async onForget(sock: DeviceSocket) {
    // before any await: nothing in flight may write the forgotten device's activity or audio back
    this.roomGen++;
    this.audioKeep.generation++;
    this.jobTraces.clear(); // (a trace tail ends with dropInFlight below: its entry is then void)
    await this.activity.settle(); // activity writes queued before the forget land BEFORE the wipe, never after
    for (const prefix of ["tok:", "pair:", "job:", "ev:", "cur:", "whq:", NOTES_PREFIX, ACTIVITY_PREFIX, AUDIO_PREFIX, ...PHRASE_PREFIXES]) {
      for (const key of (await this.kv.list(prefix)).keys()) await this.kv.delete(key);
    }
    for (const key of [K.device, K.link, K.pairCurrent, K.evSeq, K.evRate, K.skillEv, K.origin, K.devinfo, K.upd, K.voiceCfg, K.vbox, K.vstat, K.turn, K.speech, K.lastAnswerer]) await this.kv.delete(key);
    // Its token is gone with the rest: so is the voice box.
    this.closeBoxes(() => true, CLOSE_UNAUTHORIZED, "device forgotten");
    this.log("device forgotten", { device: this.deviceId });
    sock.sendText(JSON.stringify({ t: "forgotten" }));
    sock.close(CLOSE_FORGOTTEN, "forgotten");
    this.dropInFlight("device_offline");
  }

  // =====================================================================
  // Device commands
  // =====================================================================

  private sendCmd(name: string, args: JsonObject, timeoutMs = this.t.cmdTimeoutMs): Promise<Outcome<JsonObject>> {
    const sock = this.liveSocket();
    if (!sock) return Promise.resolve(fail("device_offline"));
    const id = `c${++this.cmdSeq}${b64urlEncode(randomBytes(3))}`;
    return new Promise((resolve) => {
      const timer = this.clock.setTimeout(() => {
        this.pendingCmds.delete(id);
        resolve(fail("device_timeout"));
      }, timeoutMs);
      this.pendingCmds.set(id, { resolve, timer });
      try {
        sock.sendText(JSON.stringify({ t: "cmd", id, name, args }));
      } catch {
        this.clock.clearTimeout(timer);
        this.pendingCmds.delete(id);
        resolve(fail("device_offline"));
      }
    });
  }

  // =====================================================================
  // Agent API
  // =====================================================================

  private async auth(token: string): Promise<{ rec: TokenRecord; hash: string } | undefined> {
    if (typeof token !== "string" || !token.startsWith(`${this.deviceId}.`)) return undefined;
    const hash = await sha256Hex(token);
    const rec = await this.kv.get<TokenRecord>(K.token(hash));
    return rec ? { rec, hash } : undefined;
  }

  /** An AGENT token: every agent surface (commands, jobs, inbox, webhooks, updates) goes through here. */
  private async agentAuth(token: string): Promise<{ rec: TokenRecord; hash: string } | ReturnType<typeof fail>> {
    const a = await this.auth(token);
    if (!a) return fail("unauthorized");
    if (a.rec.role === "voice") return fail("forbidden", "this is a voice box token: it only opens the voice socket (GET /v1/voice), never agent calls");
    return a;
  }

  /** Activity attribution of an authenticated token. */
  private who(a: { rec: TokenRecord; hash: string }): Who {
    return { who: a.hash.slice(0, 16), agent: a.rec.agent_name };
  }

  /** Runs one agent call and records it in the activity log (agent tokens only; resolved first: a call may revoke it). */
  private async logged<T extends Outcome<any>>(token: string, op: string, args: unknown, run: () => Promise<T>): Promise<T> {
    const t0 = this.clock.now();
    const a = await this.auth(token);
    const out = await run();
    return a && a.rec.role !== "voice" ? this.activity.call(this.who(a), op, args, t0, out) : out;
  }

  /** Revokes the calling token, agent or voice box (a box's socket closes with it). */
  async revoke(token: string): Promise<Outcome> {
    const a = await this.auth(token);
    if (!a) return fail("unauthorized");
    await this.activity.record({ ts: this.clock.now(), kind: "pair", ev: "revoked", ...this.who(a), ...(a.rec.role === "voice" ? { data: { role: "voice" } } : {}) });
    await this.kv.delete(K.token(a.hash));
    await this.kv.delete(K.cur(a.hash));
    await this.kv.delete(K.phrases(a.hash));
    await this.kv.delete(notesKey(a.hash));
    await this.dropDeliveries(a.hash);
    // its held draft goes with it (nobody would continue that opener)
    const held = this.openTurn?.drafts.get(a.hash);
    if (held) {
      this.openTurn!.drafts.delete(a.hash);
      held.prepared.feed.end();
    }
    if (a.rec.role === "voice") await this.forgetBox((m) => m.tok === a.hash, "voice box token revoked");
    return { ok: true, result: { revoked: true } };
  }

  async command(token: string, name: string, args: unknown): Promise<Outcome> {
    // the op is a known command name or "unknown_command": a caller-chosen string is never logged as an op
    const op = typeof name === "string" && (COMMANDS as readonly string[]).includes(name) ? name : "unknown_command";
    return this.logged(token, op, args, () => this.commandNow(token, name, args));
  }

  private async commandNow(token: string, name: string, args: unknown): Promise<Outcome> {
    const a = await this.agentAuth(token);
    if ("ok" in a) return a;
    return this.ride(a, await this.runCommand(a.rec, name, args, a.hash, this.who(a)));
  }

  private async runCommand(agent: TokenRecord, name: string, args: unknown, hash = "", by?: Who): Promise<Outcome> {
    let cmd;
    try {
      cmd = validateCommand(name, args);
    } catch (e) {
      if (e instanceof CommandError) return fail(e.code, e.message);
      throw e;
    }
    // the human is talking: nothing that makes a sound or takes the screen (draft / react instead)
    if (HUSHED.includes(cmd.name) || (cmd.name === "state" && Object.keys(cmd.args).length > 0)) {
      const hush = this.humanSpeaking();
      if (hush) return hush;
    }
    const live = this.liveSocket();
    if (!live) return fail("device_offline");
    if (cmd.name === "act") {
      // A gesture (cap `act`), gated like ask: older firmware is firmware_too_old, a device that
      // lists caps without it is unsupported. It never takes the screen: no screenRev / screenInFlight.
      const caps = live.getMeta()?.caps;
      if (!caps?.includes(FEATURES.act.cap)) {
        const old = tooOld("act", await this.facts(), await this.releases.latest("wait"));
        if (old) return fail("firmware_too_old", `molty_act needs Molty firmware ${old.needed} or newer; this Molty runs ${old.current}`, old);
        if (caps) return fail("unsupported", "this Molty has no gestures (no `act` in its caps)");
      }
    }
    if ((cmd.name === "mood" && cmd.args.mood === WORKING_MOOD) || (cmd.name === "act" && WORKING_ACTIONS.includes(cmd.args.action as string))) {
      const caps = live.getMeta()?.caps;
      if (!caps?.includes(WORKING_CAP)) {
        const old = tooOld("working", await this.facts(), await this.releases.latest("wait"));
        if (old) return fail("firmware_too_old", `this needs Molty firmware ${old.needed} or newer; this Molty runs ${old.current}`, old);
        return fail("unsupported", "this Molty has no working mood (no `working` in its caps)");
      }
    }
    if (cmd.name === "gestures") {
      const caps = live.getMeta()?.caps;
      if (caps && !caps.includes(GESTURES_CAP)) return fail("unsupported", "this Molty's firmware has no gesture settings: update it");
    }
    if (cmd.name === "quick") {
      if (live.getMeta()?.caps && !live.getMeta()!.caps!.includes("say")) return fail("unsupported", "this device has no speaker");
      return this.quick(hash, by, cmd.args, cmd.in_reply_to);
    }
    if (cmd.job) {
      // A device that lists its caps and leaves say/listen out has no speaker/mic
      // (device/firmware/boards/): refuse before spending a TTS call. No caps = older firmware.
      const caps = live.getMeta()?.caps;
      if (caps && !caps.includes(cmd.name)) {
        return fail("unsupported", `this device has no ${cmd.name === "say" ? "speaker" : "microphone"}`);
      }
      if (cmd.name === "listen") {
        const w = this.listenWindow(cmd.args.ms as number | undefined);
        if (!w.ok) return w;
      }
      if (cmd.name === "say") {
        // a human turn is open: the first answer claims it, other agents get turn_claimed (not audio_busy);
        // the claim is taken only once the say holds the audio lock (a refused say claims nothing)
        const pre = await this.claimCheck(hash, cmd.in_reply_to);
        if (!pre.ok) return pre;
        const called = this.clock.now();
        const sayArgs = cmd.client_timing ? { ...cmd.args, client_timing: cmd.client_timing } : cmd.args;
        // its own opener is playing: this say queues behind it and starts the moment it ends
        const after = this.followUpOf(hash);
        const started = await this.startJob("say", sayArgs, true, by, () => this.claimFor(hash, cmd.in_reply_to, called), after ? { after } : {});
        if (started.ok && after) this.noteFollow(after, started.result.job as string);
        return started;
      }
      return this.startJob(cmd.name as "say" | "listen", cmd.args, true, by);
    }
    let res: Outcome<JsonObject>;
    if (cmd.name === "mood") {
      // Counted only once the device took it: a refused mood (pairing prompt up, bad
      // state) must not silence the automatic thinking/idle that follows.
      this.screenInFlight++;
      try {
        res = await this.sendCmd(cmd.name, cmd.args);
      } finally {
        this.screenInFlight--;
      }
      if (res.ok) this.screenRev++;
    } else res = await this.sendCmd(cmd.name, cmd.args);
    // the agent is on it (a feeling, text, a gesture): Molty's own filler stands down
    if (res.ok && (cmd.name === "mood" || cmd.name === "show" || cmd.name === "act" || (cmd.name === "state" && Object.keys(cmd.args).length > 0))) this.agentSeq++;
    if (!res.ok || cmd.name !== "status") return res;
    const sock = this.liveSocket();
    const meta = sock?.getMeta();
    const events: JsonObject = {};
    for (const [k, v] of this.lastEvents) events[k] = { data: v.data as JsonObject, at: v.at };
    const firmware = firmwareBlock(await this.facts(), await this.releases.latest("wait"));
    return {
      ok: true,
      result: {
        ...res.result,
        relay: {
          api_version: API_VERSION,
          device_id: this.deviceId,
          online: !!sock,
          fw: meta?.fw ?? null,
          board: meta?.board ?? null,
          caps: meta?.caps ?? [],
          connected_since: meta?.since ?? null,
          // Who would speak now: the cloud provider, "box" (the owner's voice box), or voice_box_offline (box mode, no box).
          voice: await this.voiceName(),
          events,
          webhook: agent.webhook ? (redactWebhook(agent.webhook) as JsonObject) : null,
          firmware: firmware as unknown as JsonObject,
        },
      },
    };
  }

  // ---------------- pairing ----------------

  async pair(req: PairRequest): Promise<Outcome<PairResult>> {
    const waitMs = Math.max(0, Math.min(req.wait_ms ?? this.t.pairWindowMs, this.t.pairWindowMs));
    if (req.pair_id !== undefined) {
      if (typeof req.pair_id !== "string" || !/^p_[A-Za-z0-9_-]{22}$/.test(req.pair_id)) return fail("invalid_args", "bad pair_id");
      const rec = await this.kv.get<PairRecord>(K.pair(req.pair_id));
      if (!rec) return fail("not_found", "unknown or already-claimed pair_id");
      return this.awaitPair(rec.id, waitMs);
    }
    const name = req.agent_name;
    if (typeof name !== "string" || name.trim().length === 0 || name.length > 64 || /[\u0000-\u001f\u007f]/.test(name)) {
      return fail("invalid_args", "agent_name must be 1..64 printable characters");
    }
    if (req.role !== undefined && req.role !== "agent" && req.role !== "voice") return fail("invalid_args", 'role must be "agent" or "voice"');
    const voice = req.role === "voice";
    if (typeof req.link_code !== "string" || !LINK_CODE_RE.test(req.link_code)) return fail("bad_link_code");
    const sock = this.liveSocket();
    if (!sock) return fail("device_offline");
    const link = await this.kv.get<{ code: string }>(K.link);
    if (!link || !safeEqual(link.code, req.link_code)) return fail("bad_link_code");
    const now = this.clock.now();
    const currentId = await this.kv.get<string>(K.pairCurrent);
    if (currentId) {
      const current = await this.kv.get<PairRecord>(K.pair(currentId));
      if (current && current.status === "pending" && current.expires > now) return fail("pair_busy");
    }
    const rec: PairRecord = {
      id: `p_${b64urlEncode(randomBytes(16))}`,
      req: b64urlEncode(randomBytes(6)),
      code: randomDigits(4),
      agent_name: name.trim(),
      ...(voice ? { role: "voice" as const } : {}),
      created: now,
      expires: now + this.t.pairWindowMs,
      status: "pending",
    };
    await this.kv.put(K.pair(rec.id), rec);
    await this.kv.put(K.pairCurrent, rec.id);
    // The device (and its console) show this name next to the code: a voice box says so.
    const shown = voice ? voiceLabel(rec.agent_name) : rec.agent_name;
    sock.sendText(JSON.stringify({ t: "pair.request", req: rec.req, agent_name: shown, code: rec.code }));
    return this.awaitPair(rec.id, waitMs);
  }

  private async awaitPair(id: string, waitMs: number): Promise<Outcome<PairResult>> {
    const deadline = this.clock.now() + waitMs;
    for (;;) {
      const rec = await this.kv.get<PairRecord>(K.pair(id));
      if (!rec) return fail("not_found", "unknown or already-claimed pair_id");
      const now = this.clock.now();
      if (rec.status === "pending" && now >= rec.expires) rec.status = "timeout";
      if (rec.status === "approved" && rec.token) {
        await this.kv.delete(K.pair(id)); // the token is handed out exactly once
        const result: PairResult = { status: "approved", agent_token: rec.token, device_id: this.deviceId, agent_name: rec.agent_name, code: rec.code, api_version: API_VERSION };
        if (rec.role === "voice") result.role = "voice";
        return { ok: true, result };
      }
      if (rec.status !== "pending") {
        await this.kv.delete(K.pair(id));
        if ((await this.kv.get<string>(K.pairCurrent)) === id) await this.kv.delete(K.pairCurrent);
        return fail(rec.status === "rejected" ? "rejected" : "timeout");
      }
      const remaining = Math.min(deadline, rec.expires) - now;
      if (remaining <= 0) {
        return { ok: true, result: { status: "pending", pair_id: rec.id, code: rec.code, expires_in_s: Math.ceil((rec.expires - now) / 1000) } };
      }
      await this.waitOn(this.pairWaiters, id, remaining);
    }
  }

  private async onPairResult(frame: Record<string, unknown>) {
    const id = await this.kv.get<string>(K.pairCurrent);
    if (!id) return;
    const rec = await this.kv.get<PairRecord>(K.pair(id));
    if (!rec || rec.status !== "pending" || typeof frame.req !== "string" || !safeEqual(rec.req, frame.req)) return;
    if (this.clock.now() >= rec.expires) {
      rec.status = "timeout";
    } else if (frame.ok === true) {
      const token = `${this.deviceId}.${b64urlEncode(randomBytes(32))}`;
      const hash = await sha256Hex(token);
      if (rec.role === "voice") {
        // One voice box per device: the new one replaces any earlier box (token and socket).
        await this.unpairBoxes("replaced by a newly paired voice box");
        await this.kv.put<TokenRecord>(K.token(hash), { agent_name: rec.agent_name, created: this.clock.now(), role: "voice" });
      } else {
        await this.kv.put<TokenRecord>(K.token(hash), { agent_name: rec.agent_name, created: this.clock.now() });
        // A new agent starts at the head: it hears what happens from now on.
        await this.kv.put(K.cur(hash), (await this.kv.get<number>(K.evSeq)) ?? 0);
      }
      rec.status = "approved";
      rec.token = token;
    } else {
      rec.status = frame.reason === "timeout" ? "timeout" : "rejected";
    }
    await this.kv.put(K.pair(id), rec);
    await this.kv.delete(K.pairCurrent);
    await this.activity.record({
      ts: this.clock.now(),
      kind: "pair",
      ev: rec.status,
      agent: rec.agent_name,
      ...(rec.token ? { who: (await sha256Hex(rec.token)).slice(0, 16) } : {}),
      ...(rec.role === "voice" ? { data: { role: "voice" } } : {}),
    });
    // The device rotates its link code after a pairing; the old one is dead
    // until the device reports the new one (here, in hello, or link_code).
    if (rec.status === "approved") await this.kv.delete(K.link);
    if (typeof frame.link_code === "string") await this.onLinkCode(frame.link_code);
    this.notify(this.pairWaiters, id);
  }

  // ---------------- jobs ----------------

  async job(token: string, jobId: string, waitMs: number): Promise<Outcome<JobView>> {
    return this.logged(token, "wait", { job: jobId, wait_ms: waitMs }, () => this.jobNow(token, jobId, waitMs));
  }

  private async jobNow(token: string, jobId: string, waitMs: number): Promise<Outcome<JobView>> {
    const a = await this.agentAuth(token);
    if ("ok" in a) return a;
    // Only the agent that started a job may read it (its result carries that agent's words).
    // Ownerless jobs (a shake's adopted listen) hold what every agent got in `utterance`.
    // Someone else's job answers not_found, the same as a job that never existed.
    if (typeof jobId === "string") {
      const rec = await this.kv.get<JobRecord>(K.job(jobId));
      if (rec?.by && rec.by.who !== this.who(a).who) return fail("not_found");
    }
    let out = await this.runJob(jobId, waitMs);
    // A failed job is an error to every surface: nothing rides along on it.
    if (out.ok && out.result.status === "failed") return out;
    if (out.ok && out.result.status === "done" && (await this.collect(jobId))) out = await this.runJob(jobId, 0);
    return this.ride(a, out);
  }

  async talk(token: string, req: unknown): Promise<Outcome> {
    return this.logged(token, "talk", req, () => this.talkNow(token, req));
  }

  private async talkNow(token: string, req: unknown): Promise<Outcome> {
    const t0 = this.clock.now();
    const a = await this.agentAuth(token);
    if ("ok" in a) return a;
    let args: TalkArgs;
    try {
      args = validateTalk(req);
    } catch (e) {
      if (e instanceof CommandError) return fail(e.code, e.message);
      throw e;
    }
    const hush = this.humanSpeaking();
    if (hush) return hush;
    const live = this.liveSocket();
    if (!live) return fail("device_offline");
    const caps = live.getMeta()?.caps;
    if (caps && args.say !== undefined && !caps.includes("say")) return fail("unsupported", "this device has no speaker");
    if (caps && args.listen && !caps.includes("listen")) return fail("unsupported", "this device has no microphone");
    if (args.listen) {
      const w = this.listenWindow(args.listen_ms);
      if (!w.ok) return w;
    }
    // a human turn is open: the first answer claims it, other agents get turn_claimed (not audio_busy);
    // the claim is taken only once the talk holds the audio lock (a refused talk claims nothing)
    const pre = await this.claimCheck(a.hash, args.in_reply_to);
    if (!pre.ok) return pre;
    await this.kv.put(K.lastAnswerer, a.hash); // a talk converses with the human: Molty's fillers are its own
    const after = this.followUpOf(a.hash);
    const started = await this.startJob("talk", args as unknown as JsonObject, true, this.who(a), () => this.claimFor(a.hash, args.in_reply_to, t0), after ? { after } : {});
    if (!started.ok) return started;
    const job = started.result.job as string;
    if (after) this.noteFollow(after, job);
    let out = await this.runJob(job, Math.max(0, this.t.talkBudgetMs - (this.clock.now() - t0)));
    if (!out.ok) return out;
    if (out.result.status === "done" && (await this.collect(job))) {
      out = await this.runJob(job, 0);
      if (!out.ok) return out;
    }
    const v = out.result;
    if (v.status === "failed") return fail(v.error ?? "failed", `talk job ${job} failed`, { job });
    if (v.status === "done") {
      return this.ride(a, { ok: true, result: { ...(v.result ?? {}), job, status: "done" } });
    }
    return this.ride(a, {
      ok: true,
      result: { job, status: "pending", state: v.status, hint: "still talking: call molty_wait {job} (REST: GET /v1/jobs/<job>?wait_s=8) for the result" },
    });
  }

  async ask(token: string, req: unknown): Promise<Outcome> {
    return this.logged(token, "ask", req, () => this.askNow(token, req));
  }

  private async askNow(token: string, req: unknown): Promise<Outcome> {
    const t0 = this.clock.now();
    const a = await this.agentAuth(token);
    if ("ok" in a) return a;
    let args: AskArgs;
    try {
      args = validateAsk(req);
    } catch (e) {
      if (e instanceof CommandError) return fail(e.code, e.message);
      throw e;
    }
    const hush = this.humanSpeaking();
    if (hush) return hush;
    const live = this.liveSocket();
    if (!live) return fail("device_offline");
    // Firmware lists `ask` in caps only on boards with a touch screen. Caps say whether it can;
    // the version says why not: older firmware is firmware_too_old (an update unlocks it),
    // firmware that has ask but leaves it out is a board without touch (unsupported).
    const caps = live.getMeta()?.caps;
    if (!caps?.includes(FEATURES.ask.cap)) {
      const old = tooOld("ask", await this.facts(), await this.releases.latest("wait"));
      if (old) return fail("firmware_too_old", `molty_ask needs Molty firmware ${old.needed} or newer; this Molty runs ${old.current}`, old);
      if (caps) return fail("unsupported", "this device has no touch screen for answers: use molty_talk instead");
    }
    if (caps && args.say !== undefined && !caps.includes("say")) return fail("unsupported", "this device has no speaker: ask without say, or show the question first");
    // in_reply_to naming the open turn: traced (a `reply` entry), never claimed — an ask never takes the floor
    const turn = args.in_reply_to !== undefined ? await this.kv.get<TurnRecord>(K.turn) : undefined;
    const answers: JsonObject = turn && turn.id === args.in_reply_to ? { turn: { id: turn.id, at: turn.at }, called: t0 } : {};
    // Only the spoken part needs the speaker; waiting for the tap holds no audio lock.
    const started = await this.startJob("ask", { ...(args as unknown as JsonObject), ...answers }, args.say !== undefined, this.who(a));
    if (!started.ok) return started;
    const job = started.result.job as string;
    const out = await this.runJob(job, Math.max(0, this.t.talkBudgetMs - (this.clock.now() - t0)));
    if (!out.ok) return out;
    const v = out.result;
    if (v.status === "failed") return fail(v.error ?? "failed", `ask job ${job} failed`, { job });
    if (v.status === "done") return this.ride(a, { ok: true, result: { ...(v.result ?? {}), job, status: "done" } });
    return this.ride(a, {
      ok: true,
      result: { job, status: "pending", state: v.status, hint: "still waiting for the tap: call molty_wait {job} (REST: GET /v1/jobs/<job>?wait_s=8) until it is done" },
    });
  }

  // ---------------- updates ----------------

  async updates(token: string): Promise<Outcome> {
    return this.logged(token, "updates", undefined, () => this.updatesNow(token));
  }

  private async updatesNow(token: string): Promise<Outcome> {
    const a = await this.agentAuth(token);
    if ("ok" in a) return a;
    const fw = firmwareBlock(await this.facts(), await this.releases.latest("wait"));
    // Reporting the firmware release counts as telling it (atomically, never resurrecting a revoked token).
    if (fw.update_available && fw.latest) {
      const latest = fw.latest;
      await this.kv.update<TokenRecord, void>(K.token(a.hash), (cur) =>
        cur && !(cur.fw_seen && (compareVersions(cur.fw_seen, latest) ?? -1) >= 0) ? { next: { ...cur, fw_seen: latest }, result: undefined } : { result: undefined },
      );
    }
    const result = { api_version: API_VERSION, firmware: fw, actions: updateActions(fw) };
    // Re-read the token so the ride-along does not repeat what this result already says.
    const rec = (await this.kv.get<TokenRecord>(K.token(a.hash))) ?? a.rec;
    return this.ride({ rec, hash: a.hash }, { ok: true, result: result as unknown as JsonObject });
  }

  async update(token: string, req: unknown): Promise<Outcome> {
    return this.logged(token, "update", req, () => this.updateNow(token, req));
  }

  private async updateNow(token: string, req: unknown): Promise<Outcome> {
    const t0 = this.clock.now();
    const a = await this.agentAuth(token);
    if ("ok" in a) return a;
    // No arguments: what gets installed is decided here, from the configured repo's latest release.
    if (req !== undefined && req !== null && !(isRecord(req) && Object.keys(req).length === 0)) {
      return fail("invalid_args", "molty_update takes no arguments: it installs the latest official release");
    }
    const live = this.liveSocket();
    if (!live) return fail("device_offline");
    const d = await this.facts();
    const lookup = await this.releases.latest("wait");
    const fw = firmwareBlock(d, lookup);
    if (!fw.can_self_update) {
      return fail("unsupported", `this firmware (${d.fw ?? "unknown"}) can't update itself (no \`update\` in its caps)`, {
        current: d.fw,
        latest: fw.latest,
        how: `tell your human to ${CONSOLE_HINT}, which shows the latest firmware and installs it`,
      });
    }
    const busy = await this.kv.get<UpdateRecord>(K.upd);
    if (busy) {
      const v = await this.runJob(busy.job, 0);
      if (v.ok && !TERMINAL.includes(v.result.status)) return this.ride(a, { ok: true, result: this.pendingUpdate(v.result) });
    }
    if (lookup.state === "unavailable") return fail("release_unavailable", "couldn't reach the release list just now; try again in a minute");
    if (fw.status === "unknown") return fail("unknown_version", `Molty runs ${d.fw ?? "an unknown firmware"}, which can't be compared with releases: update it by hand (${CONSOLE_HINT})`);
    if (fw.status === "current" || fw.status === "no_release") {
      return this.ride(a, { ok: true, result: { status: "up_to_date", current: fw.current, latest: fw.latest } });
    }
    if (fw.status === "no_build_for_board") return fail("no_build", `release ${fw.latest} has no firmware image for this board (${fw.board})`);
    const asset = installable(lookup.release, d.board);
    if (!asset) return fail("no_checksum", `release ${fw.latest} publishes no sha256 for this board's image, so Molty can't verify it: update by hand (${CONSOLE_HINT})`);
    const args: JsonObject = { version: fw.latest!, url: asset.url, sha256: asset.sha256!, size: asset.size, from: d.fw! };
    const started = await this.startJob("update", args, false, this.who(a));
    if (!started.ok) return started;
    const job = started.result.job as string;
    const out = await this.runJob(job, Math.max(0, this.t.talkBudgetMs - (this.clock.now() - t0)));
    if (!out.ok) return out;
    const v = out.result;
    if (v.status === "failed") return fail(v.error ?? "failed", typeof v.detail === "string" ? v.detail : `update job ${job} failed`, { job });
    if (v.status === "done") return this.ride(a, { ok: true, result: { ...(v.result ?? {}), job, status: "done" } });
    return this.ride(a, { ok: true, result: this.pendingUpdate(v) });
  }

  private pendingUpdate(v: JobView): JsonObject {
    return {
      job: v.job,
      status: "pending",
      state: v.status,
      phase: (v.phase as string | undefined) ?? "confirm",
      hint:
        v.phase === "confirm" || v.phase === undefined
          ? "Molty is asking your human on its screen: call molty_wait {job} (REST: GET /v1/jobs/<job>?wait_s=8) until it is done"
          : "Molty is updating and will restart (about a minute): keep calling molty_wait {job} until it is done",
    };
  }

  /** Sends `cmd update` and arms the deadline; the rest is driven by device events and the next hello. */
  private async runUpdate(id: string, args: JsonObject) {
    const now = this.clock.now();
    await this.setJob(id, { status: "running", phase: "confirm" });
    await this.kv.put<UpdateRecord>(K.upd, { job: id, from: args.from as string, to: args.version as string, phase: "confirm", deadline: now + this.t.updateConfirmMs + this.t.askGraceMs });
    this.armUpdateTimer(this.t.updateConfirmMs + this.t.askGraceMs);
    const { from: _from, ...wire } = args;
    // The confirmation takes the screen like an ask: no automatic mood (an older listen's) may cancel it.
    this.screenInFlight++;
    let res: Outcome<JsonObject>;
    try {
      res = await this.sendCmd("update", { job: id, ...wire, timeout_ms: this.t.updateConfirmMs });
    } finally {
      this.screenInFlight--;
    }
    // Device events already in for this job (accepted / downloading, or a settled decline / failure)
    // prove the cmd landed: a lost ack must not discard them (the device keeps installing).
    const u = await this.kv.get<UpdateRecord>(K.upd);
    const evidenced = !u || u.job !== id || u.phase !== "confirm";
    if (res.ok || res.error === "device_timeout" || evidenced) this.screenRev++;
    if (res.ok || evidenced) return;
    // A missing ack is "delivery unknown", not a refusal: the question may well be on screen and
    // the human can still tap it. Keep tracking through the confirmation deadline (already armed);
    // a later `accepted` / progress event, a decline, or the next hello settles it, else update_timeout.
    if (res.error === "device_timeout") {
      await this.kv.update<UpdateRecord, void>(K.upd, (cur) => (cur && cur.job === id && cur.phase === "confirm" ? { next: { ...cur, acked: false }, result: undefined } : { result: undefined }));
      return;
    }
    await this.endUpdate(id, { status: "failed", error: res.error });
  }

  private armUpdateTimer(ms: number) {
    if (this.updTimer !== undefined) this.clock.clearTimeout(this.updTimer);
    this.updTimer = this.clock.setTimeout(() => {
      this.updTimer = undefined;
      void this.checkUpdateDeadline();
    }, Math.max(0, ms));
  }

  /** Fails the update job once its deadline passed (also checked lazily, after an eviction). */
  private async checkUpdateDeadline(): Promise<void> {
    const u = await this.kv.get<UpdateRecord>(K.upd);
    if (!u) return;
    // A timer may fire a little before the clock reaches the deadline: re-arm for the rest
    // instead of returning, or the update would never time out (the caller waits its whole budget).
    if (this.clock.now() < u.deadline) return this.armUpdateTimer(u.deadline - this.clock.now());
    const detail =
      u.phase === "confirm"
        ? u.acked === false
          ? "Molty never confirmed it showed the update question, and nobody answered it"
          : "nobody answered Molty's update question"
        : `Molty did not come back on firmware ${u.to} in time (last step: ${u.phase})`;
    await this.endUpdate(u.job, { status: "failed", error: "update_timeout", detail });
  }

  private async endUpdate(job: string, patch: Partial<JobRecord> & { status: "done" | "failed" }) {
    await this.kv.update<UpdateRecord, void>(K.upd, (cur) => (cur && cur.job === job ? { next: null, result: undefined } : { result: undefined }));
    if (this.updTimer !== undefined) {
      this.clock.clearTimeout(this.updTimer);
      this.updTimer = undefined;
    }
    await this.finishJob(job, patch);
  }

  /** Device `update {job, state, reason?, error?}` for the update in flight. */
  private async onUpdateEvent(data: Record<string, unknown>) {
    const u = await this.kv.get<UpdateRecord>(K.upd);
    if (!u || data.job !== u.job || typeof data.state !== "string") return;
    const state = data.state;
    const short = (v: unknown) => (typeof v === "string" ? v.replace(/[^\x20-\x7e]/g, "").slice(0, 80) : undefined);
    if (state === "declined") {
      const reason = data.reason === "timeout" || data.reason === "cancelled" ? data.reason : undefined;
      return this.endUpdate(u.job, { status: "done", result: { outcome: "declined", ...(reason ? { reason } : {}), current: u.from, latest: u.to } });
    }
    if (state === "failed") {
      const err = short(data.error);
      return this.endUpdate(u.job, { status: "failed", error: "update_failed", detail: `Molty could not install ${u.to}${err ? `: ${err}` : ""}; it still runs ${u.from}` });
    }
    if (!UPDATE_PHASES.includes(state)) return;
    // From the accept on, the device has updateMs to come back on the new firmware.
    const deadline = u.phase === "confirm" ? this.clock.now() + this.t.updateMs : u.deadline;
    if (u.phase === "confirm") this.armUpdateTimer(this.t.updateMs);
    await this.kv.put<UpdateRecord>(K.upd, { ...u, phase: state, deadline });
    await this.setJob(u.job, { phase: state });
    this.notify(this.jobWaiters, u.job);
  }

  /**
   * On every hello: the new firmware = done. The old one after `rebooting` = the new
   * image did not stick (rolled back). Any other phase: a reconnect mid-download, keep
   * waiting for the deadline.
   */
  private async settleUpdate(fw: string | null) {
    const u = await this.kv.get<UpdateRecord>(K.upd);
    if (!u) return;
    if (fw !== null && compareVersions(fw, u.to) === 0) {
      return this.endUpdate(u.job, { status: "done", result: { outcome: "updated", from: u.from, to: u.to } });
    }
    if (u.phase === "rebooting") {
      return this.endUpdate(u.job, { status: "failed", error: "rolled_back", detail: `Molty restarted but came back on ${fw ?? "an unknown firmware"}, not ${u.to}: the update did not stick` });
    }
    if (this.clock.now() >= u.deadline) return this.checkUpdateDeadline();
    this.armUpdateTimer(u.deadline - this.clock.now());
  }

  private async runJob(jobId: string, waitMs: number): Promise<Outcome<JobView>> {
    if (typeof jobId !== "string" || !jobId.startsWith(`${this.deviceId}.j_`)) return fail("not_found");
    const deadline = this.clock.now() + Math.max(0, Math.min(waitMs, JOB_WAIT_CAP_MS));
    for (;;) {
      const rec = await this.kv.get<JobRecord>(K.job(jobId));
      if (!rec) return fail("not_found");
      if (rec.kind === "update" && !TERMINAL.includes(rec.status)) {
        // Storage-driven (no in-memory task to lose): only its deadline ends it here.
        const u = await this.kv.get<UpdateRecord>(K.upd);
        if ((!u || u.job !== jobId) && !this.tasks.has(jobId)) {
          await this.finishJob(jobId, { status: "failed", error: "interrupted" });
          continue;
        }
        if (u && u.job === jobId && this.clock.now() >= u.deadline) {
          await this.checkUpdateDeadline();
          continue;
        }
      } else if (!TERMINAL.includes(rec.status) && !this.tasks.has(jobId)) {
        await this.finishJob(jobId, { status: "failed", error: "interrupted" });
        continue;
      }
      const remaining = deadline - this.clock.now();
      if (TERMINAL.includes(rec.status) || remaining <= 0) return { ok: true, result: viewOf(rec) };
      await this.waitOn(this.jobWaiters, jobId, remaining);
    }
  }

  /** `audio`: the job takes the one-at-a-time audio lock (an ask only while it speaks). */
  /**
   * `admit`: runs once the audio lock is held (taken synchronously with its check), before the
   * job exists; a failure releases the lock and is the call's answer, success adds args. A turn
   * claim goes here, so only an admitted say / talk ever claims the human's turn.
   *
   * `takeover`: the job holding the lock hands it straight to this one (a shake's listen to its
   * turn's opener: nothing can slip in between). `after`: when that job holds the lock, this one
   * queues behind it and gets the lock the moment it ends (an opener's follow-up; one at a time).
   * `prepared`: a say's speech whose synthesis already started (a draft's opener).
   * `lead`: this job is a lead-in (a draft's opener, a quick phrase, Molty's filler) owned by that
   * token hash (or RELAY_LEAD): once it holds the lock, its owner's follow-up may queue behind it.
   */
  private async startJob(
    kind: JobKind,
    args: JsonObject,
    audio = true,
    by?: Who,
    admit?: (id: string) => Promise<Outcome<JsonObject>>,
    opts: { takeover?: string; after?: string; prepared?: Prepared; lead?: string } = {},
  ): Promise<Outcome> {
    const id = `${this.deviceId}.j_${b64urlEncode(randomBytes(12))}`;
    let ready: Promise<void> | undefined;
    if (audio && this.audioJob) {
      if (opts.takeover !== undefined && this.audioJob === opts.takeover) this.takeLock(id, opts.lead);
      else if (opts.after !== undefined && this.audioJob === opts.after && !this.handoff)
        ready = new Promise<void>((go) => (this.handoff = { after: opts.after!, job: id, go, ...(opts.lead !== undefined ? { lead: opts.lead } : {}) }));
      // the running job's id is not disclosed: it may be another agent's
      else return fail("audio_busy", "another say / listen / talk is still running on Molty");
    } else if (audio) this.takeLock(id, opts.lead);
    if (admit) {
      let ok: Outcome<JsonObject>;
      try {
        ok = await admit(id);
      } catch (e) {
        ok = fail(errorCode(e));
      }
      if (!ok.ok) {
        this.dropHandoff(id);
        this.releaseAudio(id);
        return ok;
      }
      args = { ...args, ...ok.result };
    }
    // anything an agent starts (a draft's opener too) means it is answering: Molty's own filler stands down
    if (by) this.agentSeq++;
    const now = this.clock.now();
    this.tasks.add(id);
    await this.kv.put<JobRecord>(K.job(id), { job: id, kind, status: "queued", created: now, updated: now, ...(by ? { by } : {}) });
    if (opts.takeover === undefined) void this.purgeJobs(now); // an opener's path stays short: the next job purges
    const run =
      kind === "say"
        ? this.runSay(id, args, now, ready, opts.prepared)
        : kind === "listen"
          ? this.runListen(id, args, now)
          : kind === "ask"
            ? this.runAsk(id, args as unknown as AskArgs, now)
            : kind === "update"
              ? this.runUpdate(id, args)
              : kind === "quick"
                ? this.runQuick(id, args, now, ready)
                : this.runTalk(id, args as unknown as TalkArgs, now, ready);
    run
      .catch((e: unknown) => this.finishJob(id, { status: "failed", error: errorCode(e) }))
      .finally(() => {
        this.tasks.delete(id);
        this.dropHandoff(id);
        this.releaseAudio(id);
      });
    return { ok: true, result: { job: id } };
  }

  /** `id` holds the audio lock now; a lead-in (`lead`: its owner) becomes the one its follow-up may queue behind. */
  private takeLock(id: string, lead?: string) {
    this.audioJob = id;
    this.lead = lead !== undefined ? { job: id, hash: lead } : undefined;
  }

  /** The audio lock of `id` ends: to the job queued behind it (startJob `after`), else free. */
  private releaseAudio(id: string) {
    if (this.audioJob !== id) return;
    const h = this.handoff;
    if (h && h.after === id) {
      this.handoff = undefined;
      this.takeLock(h.job, h.lead);
      h.go();
    } else {
      this.audioJob = undefined;
      this.lead = undefined;
    }
  }

  /** A queued job that will not run any more leaves the queue. */
  private dropHandoff(id: string) {
    if (this.handoff?.job === id) this.handoff = undefined;
  }

  /** The record as written, or undefined when there was none to change (missing or already ended). */
  private async setJob(id: string, patch: Partial<JobRecord>): Promise<JobRecord | undefined> {
    const rec = await this.kv.get<JobRecord>(K.job(id));
    if (!rec || TERMINAL.includes(rec.status)) return undefined;
    const next = { ...rec, ...patch, updated: this.clock.now() };
    await this.kv.put(K.job(id), next);
    return next;
  }

  private async finishJob(id: string, patch: Partial<JobRecord> & { status: "done" | "failed" }) {
    // a listen's turn trace goes with its activity entry (built now: everything is in, failures too)
    const jt = this.jobTraces.get(id);
    this.jobTraces.delete(id);
    const gen = this.roomGen;
    const ended = await this.setJob(id, patch);
    const entry = async () => {
      if (!ended || gen !== this.roomGen) return; // forgotten meanwhile: nothing is written back
      const traced = jt ? { trace: capTrace(jt.trace.build()), ...(jt.audio ? { audio: true } : {}) } : {};
      await this.activity.record(jobDraft({ ...ended, ...traced }, ended.by));
    };
    // the device's end frame still on its way (after the box's turn end): the agent gets the
    // result now; the entry follows once that frame's diagnostics are in (<= TRACE_TAIL_MS)
    if (!jt?.tail) await entry();
    if (this.opener?.job === id) this.opener.endedAt = this.clock.now();
    this.tasks.delete(id);
    this.releaseAudio(id);
    // Failed speech: the device may have taken the say (its answer lost, the audio never sent) and
    // would wait for frames with the creature hidden. Fire and forget; another job is a no-op there.
    if (patch.status === "failed" && ended && (ended.kind === "say" || ended.kind === "talk" || ended.kind === "quick") && this.liveSocket()?.getMeta()?.caps?.includes(SAY_CANCEL_CAP)) {
      void this.sendCmd("say_cancel", { job: id }).then((c) => {
        if (!c.ok) this.log("say_cancel failed", { device: this.deviceId, job: id, error: c.error });
      });
    }
    this.notify(this.jobWaiters, id);
    if (jt?.tail) {
      await jt.tail;
      await entry();
    }
  }

  private async purgeJobs(now: number) {
    for (const [key, rec] of await this.kv.list<JobRecord>("job:")) {
      if (now - rec.created > this.t.jobTtlMs && !this.tasks.has(rec.job)) await this.kv.delete(key);
    }
  }

  /** Stage timings are returned in job results and logged, so real-device latency can be read after deploy. */
  private logTimings(id: string, kind: JobKind, timings: JsonObject) {
    this.log("job timings", { device: this.deviceId, job: id, kind, ...timings });
  }

  /**
   * `ready`: the job is queued behind its owner's opener (startJob `after`): its speech is
   * synthesised NOW and it plays the moment the opener ends. `prepared`: already synthesising.
   */
  private async runSay(id: string, args: JsonObject, created: number, ready?: Promise<void>, prepared?: Prepared) {
    const text = args.text as string;
    const voice = typeof args.voice === "string" ? args.voice : undefined;
    const pre = prepared ?? (ready ? await this.prepareSpeech(text, voice) : undefined);
    if (ready) await ready;
    // prepared speech (an opener) goes to the device at once; the "running" write lands meanwhile
    const running = this.setJob(id, { status: "running" });
    if (!prepared) await running;
    // an opener played to its turn's end: the follow-up starts once its last sample left (say.done)
    const r = pre && !("pick" in pre) ? pre : await this.speak(id, text, voice, args.opener === true, args.marks as SayMarks | undefined, pre);
    await running;
    if (!r.ok) return this.finishJob(id, { status: "failed", error: r.error });
    const timings: JsonObject = { queue: r.start - created, ...r.timings, total: this.clock.now() - created };
    if (isRecord(args.turn)) timings.turn = this.answerTimings(args, r.start + (r.timings.first_audio as number));
    if (isRecord(args.client_timing)) timings.client = args.client_timing;
    this.logTimings(id, "say", timings);
    await this.finishJob(id, { status: "done", result: { ms: r.ms, text, timings } });
    await this.replyTrace(id, "say", args, timings);
  }

  private async runListen(id: string, args: JsonObject, created: number) {
    const { ms, trigger, ...listenArgs } = args;
    let r: ListenResult | undefined;
    // before the turn ends (an opener, the utterance): whatever an agent does in answer stands the filler down
    let seq = this.agentSeq;
    try {
      await this.setJob(id, { status: "running" });
      r = await this.record(id, ms as number | undefined, listenArgs);
      seq = this.agentSeq;
    } finally {
      // a shake's turn ends with its listen, however that ends: drafts settle, the utterance goes
      // out, and agents may speak again (never left human_speaking by a throw)
      if (trigger === "shake") await this.endTurn(id, r?.ok ? r : undefined);
      else if (r?.ok) await this.noteHeard(r);
    }
    if (!r.ok) return this.finishJob(id, { status: "failed", error: r.error });
    const timings: JsonObject = { queue: r.start - created, ...r.timings, total: this.clock.now() - created };
    this.logTimings(id, "listen", timings);
    const extra: JsonObject = r.speech_ms !== undefined ? { speech_ms: r.speech_ms } : {};
    await this.finishJob(id, { status: "done", result: { text: r.text, duration_ms: r.duration_ms, peak: r.peak, rms: r.rms, reason: r.reason, ...extra, timings } });
    // shake to talk: every agent got the utterance; nobody answers in time → Molty fills itself
    // (only when some agent is paired to answer at all; a draft's opener already answered)
    if (trigger === "shake" && this.opener?.turn !== id && [...(await this.kv.list<TokenRecord>("tok:")).values()].some((t) => t.role !== "voice")) this.armFiller(id, r, seq);
  }

  /**
   * An agent read a done job: its words are the agent's now, unless the fallback claimed them
   * first. Then wait for the marker; true means read the job again, so the agent sees `delivered_as`.
   */
  private async collect(id: string): Promise<boolean> {
    if (this.uncollected.delete(id)) return false;
    const marked = this.delivering.get(id);
    if (!marked) return false;
    await marked;
    return true;
  }

  /**
   * A talk's listen heard the human and nobody read the result (the agent's run ended on `pending`):
   * their words go out as a normal `utterance`, a new turn. The job says so, for an agent that reads it after all.
   */
  private async deliverUncollected(id: string, r: Extract<ListenResult, { ok: true }>, gen: number) {
    if (!this.uncollected.delete(id) || gen !== this.roomGen) return;
    // the job has ended (setJob refuses): only this marker is added to its result
    const marked = (async () => {
      const rec = await this.kv.get<JobRecord>(K.job(id));
      if (rec) await this.kv.put(K.job(id), { ...rec, result: { ...(rec.result ?? {}), delivered_as: "utterance", turn_id: id } });
    })();
    this.delivering.set(id, marked);
    await marked;
    // long enough for a read that began before the marker to come back and look again
    this.clock.setTimeout(() => this.delivering.delete(id), this.t.collectGraceMs);
    const text = r.text.trim();
    await this.kv.put<TurnRecord>(K.turn, { id, at: this.clock.now() });
    const data = utteranceData({
      turn_id: id,
      text,
      reason: r.reason,
      speech_ms: r.speech_ms ?? r.duration_ms,
      stt_ms: (r.timings.stt as number) ?? 0,
      trigger: "talk",
      caps: this.liveSocket()?.getMeta()?.caps ?? [],
      ack: r.ack,
      voice: await this.voiceName(),
    });
    await this.appendEvent(UTTERANCE_EVENT, data, true);
    await this.activity.record({ ts: this.clock.now(), kind: "device", ev: "utterance", job: id, data: { text: text.slice(0, 500), reason: r.reason, trigger: "talk", words: words(text).length } });
  }

  /** say (optional) then listen, one job holding the audio lock across both, so nothing slips in between. `ready`: as runSay. */
  private async runTalk(id: string, args: TalkArgs, created: number, ready?: Promise<void>) {
    const pre = ready && args.say !== undefined ? await this.prepareSpeech(args.say, args.voice) : undefined;
    if (ready) await ready;
    await this.setJob(id, { status: "running" });
    const reply = args as unknown as JsonObject; // talk() adds {turn, called} when it answers a human turn
    let heard: Extract<ListenResult, { ok: true }> | undefined;
    const ms: JsonObject = {};
    const timings: JsonObject = { queue: this.clock.now() - created };
    const result: JsonObject = {};
    if (args.say !== undefined) {
      // The device's say.done is required before the mic opens (it must not record its own
      // voice); none within playback + sayDoneGraceMs fails the turn with playback_timeout.
      const s = pre && !("pick" in pre) ? pre : await this.speak(id, args.say, args.voice, true, args.marks, pre);
      if (!s.ok) return this.finishJob(id, { status: "failed", error: s.error });
      result.said = args.say;
      ms.tts_first_audio = (s.timings.first_audio as number) ?? 0;
      ms.speech = (s.timings.play as number) ?? 0;
      timings.say = s.timings;
      if (reply.turn) timings.turn = this.answerTimings(reply, s.start + (ms.tts_first_audio as number));
    } else if (reply.turn) timings.turn = this.answerTimings(reply);
    if (args.client_timing) timings.client = args.client_timing;
    // the human turn this talk answers, in that turn's trace (an `ev:"reply"` entry), once the say is over
    if (reply.turn) await this.replyTrace(id, "talk", reply, timings);
    if (args.listen) {
      const l = await this.record(id, args.listen_ms, { ...args.vad, ...(args.ring_color !== undefined ? { ring_color: args.ring_color } : {}), ...(args.ack !== undefined ? { ack: args.ack } : {}) });
      if (!l.ok) return this.finishJob(id, { status: "failed", error: l.error });
      heard = l;
      Object.assign(result, { heard: l.text, reason: l.reason, duration_ms: l.duration_ms, peak: l.peak, rms: l.rms });
      if (l.speech_ms !== undefined) result.speech_ms = l.speech_ms;
      const fill = this.fillerIn(l);
      if (fill !== undefined) result.filler_in_ms = fill;
      await this.noteHeard(l);
      // the conversation goes on: the next answer is timed from these words, whatever turn id it names
      if (reply.turn && l.text.trim()) {
        const turnId = (reply.turn as { id: string }).id;
        const at = this.clock.now();
        await this.kv.update<TurnRecord, void>(K.turn, (cur) => (cur?.id === turnId ? { next: { ...cur, heard_at: at }, result: undefined } : { result: undefined }));
      }
      ms.listen = (l.timings.record as number) ?? 0;
      ms.stt = (l.timings.stt as number) ?? 0;
      timings.listen = l.timings;
    } else {
      result.reason = "spoken";
    }
    ms.total = this.clock.now() - created;
    timings.total = ms.total;
    this.logTimings(id, "talk", timings);
    const seq = this.agentSeq; // before the agent has the result: its next call stands the filler down
    // before the job ends: a waiting talk / wait collects it while finishJob wakes them
    if (heard?.text.trim()) this.uncollected.add(id);
    await this.finishJob(id, { status: "done", result: { ...result, ms, timings } });
    // the human spoke: the agent now thinks; nothing from it in time → Molty fills itself
    if (heard) this.armFiller(id, heard, seq);
    if (this.uncollected.has(id)) {
      const gen = this.roomGen;
      const words = heard!;
      this.clock.setTimeout(() => void this.deliverUncollected(id, words, gen), this.t.collectGraceMs);
    }
  }

  /**
   * say (optional, until the device's say.done, as talk) then `cmd ask {job, options,
   * colors, timeout_ms}`, then wait for the device's `answer`. The audio lock is released
   * once the speech is over. Every way it ends that reaches the device is a done
   * job: a tap, or answer:null with reason timeout / cancelled (any later command
   * replaced it on the device) / offline. Each end is also one `answer` inbox event.
   */
  private async runAsk(id: string, args: AskArgs, created: number) {
    await this.setJob(id, { status: "running" });
    const ms: JsonObject = {};
    const timings: JsonObject = { queue: this.clock.now() - created };
    const result: JsonObject = {};
    let firstAudioAt: number | undefined;
    if (args.say !== undefined) {
      const s = await this.speak(id, args.say, args.voice, true, args.marks);
      this.releaseAudio(id);
      if (s.ok) firstAudioAt = s.start + ((s.timings.first_audio as number) ?? 0);
      if (!s.ok) return this.finishJob(id, { status: "failed", error: s.error });
      result.said = args.say;
      ms.tts_first_audio = (s.timings.first_audio as number) ?? 0;
      ms.speech = (s.timings.play as number) ?? 0;
      timings.say = s.timings;
    }
    // a question answering the human's turn (in_reply_to): in that turn's trace, once its say is over
    const reply = args as unknown as JsonObject;
    if (isRecord(reply.turn)) {
      timings.turn = this.answerTimings(reply, firstAudioAt);
      if (args.client_timing) timings.client = args.client_timing;
      await this.replyTrace(id, "ask", reply, timings);
    }
    const tAsk = this.clock.now();
    let settle!: (a: AskAnswer) => void;
    /** The first end accepted for this ask (a device `answer` may come before the cmd's `res`). */
    let ended: AskAnswer | undefined;
    const answered = new Promise<AskAnswer>((resolve) => (settle = (a) => resolve((ended ??= a))));
    // Registered before the cmd goes out: an answer may overtake the cmd's `res`.
    this.pendingAsks.set(id, { options: args.options, resolve: settle });
    let answer: AskAnswer;
    let tAck = tAsk;
    try {
      // The question takes the screen: no automatic mood (an older listen's) may overtake it.
      this.screenInFlight++;
      let res: Outcome<JsonObject>;
      try {
        // colors rides along always: firmware without button colours ignores the field.
        res = await this.sendCmd("ask", { job: id, options: args.options, colors: args.colors, timeout_ms: args.timeout_s * 1000 });
      } finally {
        this.screenInFlight--;
      }
      if (res.ok || res.error === "device_timeout" || ended) this.screenRev++;
      // A failed ack loses nothing the device already settled: an accepted answer stands. A missing
      // ack (device_timeout) is "delivery unknown": the question may be on screen, so keep waiting for
      // its answer until the ask's own deadline below (no answer by then: timeout).
      if (!res.ok && !ended && res.error !== "device_timeout") {
        if (res.error !== "device_offline") return this.finishJob(id, { status: "failed", error: res.error });
        settle({ index: null, label: null, reason: "offline" });
      }
      tAck = this.clock.now();
      // The device times out at timeout_ms and says so; this only covers a device that never does.
      const timer = this.clock.setTimeout(() => settle({ index: null, label: null, reason: "timeout" }), args.timeout_s * 1000 + this.t.askGraceMs);
      answer = await answered;
      this.clock.clearTimeout(timer);
    } finally {
      this.pendingAsks.delete(id);
    }
    const tEnd = this.clock.now();
    Object.assign(result, { answer: answer.label, index: answer.index });
    if (answer.reason) result.reason = answer.reason;
    ms.answer = tEnd - tAsk;
    ms.total = tEnd - created;
    timings.ask = { cmd: tAck - tAsk, wait: tEnd - tAck };
    timings.total = ms.total;
    if (args.client_timing) timings.client ??= args.client_timing;
    this.logTimings(id, "ask", timings);
    // Inbox first, then the result: the asking call's own ride-along reliably carries it (marked read).
    // Answers are agent-driven (one per ask), so they bypass the device event rate cap.
    const ev: JsonObject = { job: id, index: answer.index, label: answer.label };
    if (answer.reason) ev.reason = answer.reason;
    await this.appendEvent("answer", ev, true);
    await this.finishJob(id, { status: "done", result: { ...result, ms, timings } });
  }

  /**
   * TTS -> device, streamed: segments are synthesised in order with lookahead
   * (core/speech.ts) and their PCM is framed and paced to the device as soon as
   * it exists, so playback starts after the FIRST segment's synthesis, not the
   * whole text's. Wire format and pacing are unchanged: `cmd say` first, then
   * `s=job` pcm16 frames of `sayChunkSamples` (a shorter frame only when the
   * device would otherwise run dry, or at the end), `sayLeadFrames` ahead of
   * real time, `end:true` on the last (a header-only end frame when synthesis
   * finished after the last audio frame left). With the voice box `cmd say` goes
   * out with the request (its round trip overlaps the synthesis) and a TTS failure
   * before any audio closes that stream at once; with any other provider the
   * device is only told once the first audio exists, so such a failure never
   * touches it.
   */
  /**
   * speakNow, and the cues the device must not get: `[working]` waits for the say's real end
   * (say.done), then becomes the mood, so the say's own "back to idle" cannot undo it; the
   * working gestures go only to firmware that has them.
   */
  private async speak(id: string, text: string, voice: string | undefined, untilDone: boolean, marks?: SayMarks, prepared?: Prepared) {
    const working = !!this.liveSocket()?.getMeta()?.caps?.includes(WORKING_CAP);
    const wantsWork = !!marks?.some((m) => m[1] === WORKING_MOOD);
    const kept = marks?.filter((m) => m[1] !== WORKING_MOOD && (working || !WORKING_ACTIONS.includes(m[1])));
    const rev = this.screenRev;
    const r = await this.speakNow(id, text, voice, untilDone || wantsWork, kept, prepared);
    // an explicit mood during the say (the work was called off) has the last word
    if (r.ok && wantsWork && working && rev === this.screenRev && this.screenInFlight === 0) {
      const m = await this.sendCmd("mood", { mood: WORKING_MOOD });
      if (!m.ok) this.log("auto mood failed", { device: this.deviceId, mood: WORKING_MOOD, error: m.error });
    }
    return r;
  }

  private async speakNow(id: string, text: string, voice: string | undefined, untilDone: boolean, marks?: SayMarks, prepared?: Prepared): Promise<SpeakResult> {
    const t0 = this.clock.now();
    const ready = prepared ?? (await this.prepareSpeech(text, voice));
    if (!("pick" in ready)) return ready;
    const { pick, feed } = ready;
    const wait = (ms: number) => feed.waitFor((f, d) => this.clock.setTimeout(f, d), (h) => this.clock.clearTimeout(h), ms);
    let carry: Int16Array = new Int16Array(0);
    const absorb = () => {
      for (const p of feed.take()) carry = concat(carry, p);
    };
    const absorbCues = (into: Array<[number, number]>) => {
      for (const c of feed.takeCues()) if (c[0] < SAY_CUE_WORDS) into.push(c);
    };
    // With the box (first audio ~0.25 s), `cmd say` goes out at once, in parallel with the
    // synthesis: its round trip to the device no longer sits between the first audio and the
    // first frame. Elsewhere (the cloud can take a second) it waits for the first audio, so
    // the device never shows a say whose voice is still far off.
    const early = pick.box && !ready.cached; // cached audio is all there: cmd say with its exact length
    let sock: DeviceSocket | undefined;
    let cuesOn = false;
    const sayCmd = (ms: number) => {
      const deviceArgs: JsonObject = { job: id, text, ms, rate: SAMPLE_RATE };
      if (voice) deviceArgs.voice = voice;
      // Firmware with say_cues shows each word when the speaker reaches it: the frames carry
      // word cues [word, sample] on the stream's timeline (the box's own timing, else estimated).
      cuesOn = !!sock!.getMeta()?.caps?.includes(SAY_CUES_CAP);
      if (cuesOn) deviceArgs.cues = true;
      // Inline [cues] (validated + stripped from `text` in commands.ts): [word, name] marks the
      // device fires as its playback reaches that word's cue. They ride only inside this say:
      // no separate mood/act cmd, no screenRev bump. Firmware without the cap gets none.
      if (cuesOn && marks?.length && sock!.getMeta()?.caps?.includes(SAY_MARKS_CAP)) deviceArgs.marks = marks;
      return this.sendCmd("say", deviceArgs);
    };
    let cmd: Promise<Outcome<JsonObject>> | undefined;
    if (early) {
      sock = this.liveSocket();
      if (!sock) {
        feed.end();
        return { ok: false, error: "device_offline" };
      }
      // `ms` is informational on the device: the estimate (the audio does not exist yet)
      cmd = sayCmd(estimateSpeechMs(text));
    }
    try {
      // First audio (or the end) -- synthesize() bounds this with its per-segment timeout.
      while (!feed.queued && !feed.done) await wait(this.t.ttsTimeoutMs);
      absorb();
      if (carry.length === 0) {
        // Nothing to play: an early `cmd say` gets its stream closed (header-only end frame), so
        // the device frees the speaker instead of waiting out its 30 s frame timeout.
        if (cmd && (await cmd).ok) this.endStream(id, sock!, 0);
        throw feed.error ?? new Error("tts returned no audio");
      }
      const tFirst = this.clock.now();
      if (!early) {
        sock = this.liveSocket();
        if (!sock) return { ok: false, error: "device_offline" };
        // `ms` is informational on the device; exact when synthesis already finished.
        const announced = feed.done && feed.error === undefined ? durationMs(carry.length) : Math.max(durationMs(carry.length), estimateSpeechMs(text));
        cmd = sayCmd(announced);
      }
      let cues: Array<[number, number]> = [];
      const res = await cmd!;
      if (!res.ok) return { ok: false, error: res.error };

      let done = false;
      const doneSignal = new Promise<void>((resolve) => this.sayDone.set(id, () => ((done = true), resolve())));
      const chunk = this.t.sayChunkSamples;
      const chunkMs = (chunk * 1000) / SAMPLE_RATE;
      const lead = this.t.sayLeadFrames * chunkMs;
      let seq = 0;
      let playEnd = 0; // when the device will have played everything sent so far (relay clock)
      let firstSent = 0;
      let lastSent = 0;
      let samples = 0;
      let ended = false;
      let tSynth = feed.done ? tFirst : 0;
      const send = async (pcm: Int16Array, end: boolean) => {
        if (seq > 0) {
          const w = playEnd - lead - this.clock.now();
          if (w > 0) await sleep(this.clock, w);
        }
        const live = this.liveSocket();
        if (!live || !live.same(sock!)) throw new CodedError("device_offline");
        if (cuesOn) absorbCues(cues);
        // cues ride ahead of (or with) their audio; more than a header holds go in header-only frames first
        while (cuesOn && end && cues.length > SAY_CUES_PER_FRAME) {
          live.sendBinary(encodeAudioFrame({ s: id, seq: seq++, kind: "pcm16", rate: SAMPLE_RATE, end: false, w: cues.splice(0, SAY_CUES_PER_FRAME) }, new Uint8Array(0)));
        }
        const w = cuesOn && cues.length ? cues.splice(0, SAY_CUES_PER_FRAME) : undefined;
        live.sendBinary(encodeAudioFrame({ s: id, seq: seq++, kind: "pcm16", rate: SAMPLE_RATE, end, ...(w ? { w } : {}) }, pcm16ToBytes(pcm)));
        const now = this.clock.now();
        if (!firstSent) firstSent = now;
        lastSent = now;
        playEnd = Math.max(playEnd, now) + (pcm.length * 1000) / SAMPLE_RATE;
        samples += pcm.length;
        if (end) ended = true;
      };
      try {
        while (!done) {
          absorb();
          if (!tSynth && feed.done) tSynth = this.clock.now();
          if (feed.done && feed.error !== undefined) throw feed.error;
          // the first frame goes as soon as it holds FIRST_FRAME_MIN_SAMPLES (never more than a chunk)
          if (seq === 0 && carry.length >= Math.min(FIRST_FRAME_MIN_SAMPLES, chunk) && carry.length < chunk && !feed.done) {
            await send(carry, false);
            carry = new Int16Array(0);
            continue;
          }
          if (carry.length >= chunk) {
            const piece = carry.subarray(0, chunk);
            carry = carry.subarray(chunk);
            await send(piece, feed.done && !feed.queued && carry.length === 0);
            if (ended) break;
            continue;
          }
          if (feed.done) {
            await send(carry, true); // the tail, or a header-only end frame
            break;
          }
          // A partial frame and more audio coming: wait for it, but never let the device run dry
          // (the first frame waits at most FIRST_FRAME_HOLD_MS for more before it goes as is).
          const hungryIn = seq === 0 ? tFirst + FIRST_FRAME_HOLD_MS - this.clock.now() : playEnd - chunkMs - this.clock.now();
          if (carry.length && hungryIn <= 0) {
            await send(carry, false);
            carry = new Int16Array(0);
            continue;
          }
          await wait(carry.length ? hungryIn : this.t.ttsTimeoutMs);
        }
        // Playback ends ~ when the stream would have, unless the device says so first.
        const left = playEnd - this.clock.now() + (untilDone ? this.t.sayDoneGraceMs : 0);
        if (left > 0 && !done) await Promise.race([doneSignal, sleep(this.clock, left)]);
        // untilDone (talk): only the device's say.done proves the speaker is free. An
        // elapsed estimate is not playback: never report success (or open the mic) on it.
        if (untilDone && !done) return { ok: false, error: this.liveSocket()?.same(sock!) ? "playback_timeout" : "device_offline" };
      } catch (e) {
        // Mid-stream failure: close the device's stream so it plays what it has and frees the speaker.
        if (!ended) {
          try {
            this.liveSocket()?.sendBinary(encodeAudioFrame({ s: id, seq, kind: "pcm16", rate: SAMPLE_RATE, end: true }, new Uint8Array(0)));
          } catch {
            /* offline */
          }
        }
        return { ok: false, error: errorCode(e) };
      } finally {
        this.sayDone.delete(id);
      }
      const tEnd = this.clock.now();
      if (pick.box && !ready.cached) await this.noteBoxStats({ tts_first_audio_ms: tFirst - t0, tts_at: tEnd });
      return {
        ok: true,
        start: t0,
        ms: durationMs(samples),
        timings: {
          tts_first_byte: tFirst - t0,
          tts_total: (tSynth || tEnd) - t0,
          first_audio: firstSent - t0,
          stream: lastSent - firstSent,
          play: tEnd - firstSent,
          voice: pick.name,
          ...(ready.cached ? { cached: true } : {}),
        },
      };
    } catch (e) {
      return { ok: false, error: errorCode(e) };
    } finally {
      feed.end(); // stops synthesis nobody will play
    }
  }

  /**
   * Starts synthesising `text` into a feed nobody drains yet (speak() streams it later): a draft's
   * opener while the human still talks, a follow-up while its opener plays. The feed ends (and the
   * synthesis stops) with speak(), or with feed.end() when it will never be played.
   */
  private async prepareSpeech(text: string, voice: string | undefined, phrase?: "cache" | "if_cached"): Promise<Prepared | { ok: false; error: string }> {
    // A quick phrase (or an opener that happens to be one): its cached audio in the current voice —
    // "cache" synthesises and caches it on a miss, "if_cached" streams a miss as any speech.
    if (phrase && voice === undefined) {
      const v = await this.voiceNow();
      if (!v.ok) return { ok: false, error: v.error };
      let audio = await this.phraseCache.get(v.vkey, text);
      const fresh = !audio;
      if (!audio && phrase === "cache") audio = await this.synthPhrase(v, text);
      if (audio) {
        const feed = new PcmFeed();
        for (let o = 0; o < audio.pcm.length; o += this.t.sayChunkSamples) feed.push(audio.pcm.subarray(o, o + this.t.sayChunkSamples));
        for (const [w, smp] of audio.cues) feed.cue(w, smp);
        feed.end();
        return { pick: v.pick, feed, at: this.clock.now(), cached: true, ...(fresh ? { fresh: true } : {}) };
      }
      if (phrase === "cache") return { ok: false, error: "tts_failed" };
    }
    const pick = await this.pickVoice("tts");
    if (!pick.ok) return { ok: false, error: pick.error };
    const feed = new PcmFeed();
    // The agent's voice passes through; with none, the box uses the owner's default box voice.
    const ttsVoice = voice ?? (pick.box ? pick.cfg.voice : undefined);
    // The box streams: its first audio does not wait on the text's length, so sentences are not cut
    // apart for it (every cut is a seam of trailing + leading silence and a prosody restart).
    void synthesize(pick.provider, splitSpeech(text, { streaming: pick.box }), { voice: ttsVoice }, feed, this.clock, this.t.ttsTimeoutMs);
    return { pick, feed, at: this.clock.now() };
  }

  /** Closes a say stream on the device that never got (or will get no more) audio: a header-only end frame. */
  private endStream(id: string, sock: DeviceSocket, seq: number) {
    try {
      if (this.liveSocket()?.same(sock)) sock.sendBinary(encodeAudioFrame({ s: id, seq, kind: "pcm16", rate: SAMPLE_RATE, end: true }, new Uint8Array(0)));
    } catch {
      /* offline */
    }
  }

  /**
   * Device mic -> STT. STT starts the moment the device's end frame arrives.
   * The end frame may carry `reason` (device end-of-speech): `no_speech` skips
   * STT. The creature shows `thinking` from end of speech until the agent's
   * next command (or THINK_HOLD_S).
   */
  /**
   * The `ms` sent with a device listen. Firmware with the listen_window cap takes it only
   * when the agent gave one (undefined: the device applies its own setting, console 5..30 s
   * or unlimited, like ring_color) and reports the window in its ack. Older firmware takes
   * 1..10000 and always gets one: no unlimited, nothing longer.
   */
  private listenWindow(ms: number | undefined): Outcome<{ ms?: number }> {
    const caps = this.liveSocket()?.getMeta()?.caps;
    if (caps?.includes(LISTEN_WINDOW_CAP)) return { ok: true, result: ms !== undefined ? { ms } : {} };
    if (ms === undefined) return { ok: true, result: { ms: LEGACY_MAX_LISTEN_MS } };
    if (ms === 0 || ms > LEGACY_MAX_LISTEN_MS) {
      return fail("unsupported", `this firmware listens at most ${LEGACY_MAX_LISTEN_MS / 1000} s and has no unlimited window: update Molty (molty_update) for windows up to 30 s or unlimited`);
    }
    return { ok: true, result: { ms } };
  }

  /** `extra`: the device listen args besides job/ms/rate (VAD knobs, ring_color), passed through. `ms` undefined: the device's setting. */
  private async record(id: string, ms: number | undefined, extra: JsonObject = {}): Promise<ListenResult> {
    this.cancelFiller(); // a new listen: whatever turn was waiting for a filler is over
    this.lastFill = undefined; // ...and a filler of an earlier turn no longer skips anyone's quick
    const t0 = this.clock.now();
    let pick = await this.pickVoice("stt");
    if (!pick.ok) return { ok: false, error: pick.error };
    // the turn trace (core/trace.ts): an adopted shake listen carries its shake
    const local = typeof extra.adopt === "string" ? extra.adopt : undefined;
    const trace = new ListenTrace(t0, local ? "shake" : "agent");
    const shake = local ? this.shakes.get(local) : undefined;
    if (local) this.shakes.delete(local);
    if (shake) trace.shake = { at: shake.at - t0, ...shake.data };
    const buf: ListenBuffer = { job: id, chunks: [], bytes: 0, ended: false, screenRev: this.screenRev, trace, order: new SeqOrder<DecodedAudio>(), roomGen: this.roomGen, pauses: new PauseTracker() };
    let fallback: string | undefined;
    // Semantic end of turn by default: when the voice box serves this listen and the firmware
    // can be stopped by it (listen_stop), the box's end of turn is THE end — the device's
    // silence VAD is turned off (it heard Molty's own tail and cut humans off), and the box
    // also calls no_speech. Not when the agent tuned the device VAD itself (vad /
    // end_silence_ms), nor on the cloud path or older firmware: those keep the device VAD.
    const semantic = pick.box && extra.vad === undefined && extra.end_silence_ms === undefined && !!this.liveSocket()?.getMeta()?.caps?.includes("listen_stop");
    // Molty's reflex (core/reflex.ts): only on the box's streaming STT (its words while the human talks)
    if (pick.provider.sttStream && pick.box) buf.reflex = this.reflexFor(buf, pick.cfg);
    // Streaming STT (voice box): opened before the mic, so every chunk goes out the moment it arrives.
    if (pick.provider.sttStream) {
      try {
        buf.stream = pick.provider.sttStream({
          onTurnEnd: (reason) => this.onTurnEnd(buf, reason),
          onFail: () => this.onBoxFail(buf),
          onHint: (k) => this.onHint(buf, k),
          onPartial: (text) => this.onPartial(buf, text),
          ...(buf.reflex?.wantsLocal ? { reflex: true, onReflex: (f: Record<string, unknown>) => this.onBoxReflex(buf, f) } : {}),
          ...(semantic ? { noSpeechMs: typeof extra.no_speech_ms === "number" ? extra.no_speech_ms : BOX_NO_SPEECH_MS } : {}),
        });
        if (semantic && !buf.boxFailed) {
          buf.semantic = true;
          extra = { ...extra, vad: false };
        }
      } catch (e) {
        buf.reflex?.close();
        buf.reflex = undefined;
        if (pick.mode !== "auto") return { ok: false, error: errorCode(e) };
        // auto: the box vanished between the pick and the open: the cloud, batch.
        fallback = errorCode(e);
        pick = { ...pick, provider: this.voice, name: this.voice.name, box: false };
      }
    }
    let r: ListenResult;
    try {
      r = await this.recordInto(buf, pick, id, ms, extra, t0, fallback);
    } catch (e) {
      // STT failed (stt_timeout, a provider error): the job fails with this error — its trace stays
      await this.traced(buf, { ok: false, error: errorCode(e) });
      throw e;
    } finally {
      buf.stream?.abort(); // no-op once its final text is in
      buf.reflex?.close();
      this.clock.clearTimeout(buf.gapTimer);
    }
    return this.traced(buf, r);
  }

  /** A listen's result with its trace (the box's own part too) and, when the owner keeps audio, its PCM stored. */
  private async traced(buf: ListenBuffer, r: ListenResult): Promise<ListenResult> {
    const t = buf.trace;
    t.lost = buf.order.lost;
    t.dup = buf.order.dup;
    const box = buf.stream?.trace?.();
    if (box) t.box = box as JsonObject;
    let audio = false;
    if (buf.roomGen !== this.roomGen) return { ...r, trace: t }; // forgotten meanwhile: nothing kept, nothing logged
    if (buf.bytes > 0) {
      try {
        // the generation BEFORE the setting is read: a wipe after this read voids the store
        const gen = this.audioKeep.generation;
        const cfg = await this.activity.settings();
        if (cfg.keep_audio && cfg.transcripts) audio = await this.audioKeep.store(buf.job, buf.chunks, gen);
        if (audio) await this.scheduleDeliveries(); // the alarm also expires kept audio
      } catch (e) {
        this.log("keep audio failed", { device: this.deviceId, error: errorCode(e) });
      }
    }
    if (audio) t.relay.audio_kept = true;
    // kept for finishJob (bounded: a job always finishes; never more than a few listens in flight)
    if (buf.roomGen !== this.roomGen) return { ...r, trace: t }; // forgotten during the audio store: register nothing
    const jt: JobTrace = { trace: t, audio };
    if (this.traceTail?.buf === buf) jt.tail = this.traceTail.over;
    this.jobTraces.set(buf.job, jt);
    while (this.jobTraces.size > 8) this.jobTraces.delete(this.jobTraces.keys().next().value!);
    return { ...r, trace: t, ...(audio ? { audio } : {}) };
  }

  /** Late frames of `buf` (diagnostics only) until the device's end frame or TRACE_TAIL_MS. */
  /**
   * The listen's audio acceptance just stopped (record's end): when the relay ended it and the
   * device's end frame has not come, that job's later frames go to the trace — header
   * diagnostics only — from this very moment until that end frame or TRACE_TAIL_MS.
   */
  private beginTail(buf: ListenBuffer) {
    if (!buf.relayEnded || buf.devEnd || buf.lost || !this.liveSocket()) return;
    this.traceTail?.done(); // one tail at a time: an older one is over
    let resolve!: () => void;
    const over = new Promise<void>((r) => (resolve = r));
    const timer = this.clock.setTimeout(() => tail.done(), TRACE_TAIL_MS);
    const tail = {
      buf,
      over,
      done: () => {
        this.clock.clearTimeout(timer);
        if (this.traceTail === tail) this.traceTail = undefined;
        resolve();
      },
    };
    this.traceTail = tail;
  }

  /** A listener hint from the voice box (stt.hint) → Molty (cap `hints`), while that listen is open. */
  private onHint(buf: ListenBuffer, k: "pause" | "question") {
    if (buf.ended || this.listenBuf !== buf) return;
    const sock = this.liveSocket();
    if (!sock?.getMeta()?.caps?.includes("hints")) return;
    if (buf.trace.hints.length >= TRACE_LIMITS.hints) return;
    buf.trace.hint(k, this.clock.now());
    try {
      sock.sendText(JSON.stringify({ t: "hint", job: buf.job, k }));
    } catch {
      /* the device is gone: the listen ends anyway */
    }
  }

  private async recordInto(buf: ListenBuffer, pick: Extract<VoicePick, { ok: true }>, id: string, given: number | undefined, extra: JsonObject, t0: number, fallback?: string): Promise<ListenResult> {
    this.listenBuf = buf;
    let tAck = t0;
    // the window this listen records at most: what the device's ack reports (listen_window
    // firmware; 0 = unlimited → its LISTEN_CEILING_MS backstop), else what was sent
    let ms = LISTEN_CEILING_MS;
    try {
      const w = this.listenWindow(given);
      if (!w.ok) return { ok: false, error: w.error };
      // ack: only firmware that acknowledges turns itself knows the arg
      const { ack, ...rest } = extra;
      const sendAck = ack !== undefined && !!this.liveSocket()?.getMeta()?.caps?.includes(ACK_CAP);
      const res = await this.sendCmd("listen", { job: id, ...w.result, rate: SAMPLE_RATE, ...rest, ...(sendAck ? { ack } : {}) });
      if (!res.ok) return { ok: false, error: res.error };
      tAck = this.clock.now();
      const did = res.result.ack;
      if (typeof did === "string" && (ACK_MODES as readonly string[]).includes(did)) buf.deviceAck = did as (typeof ACK_MODES)[number];
      // the owner's auto filler delay (device setting; absent on older firmware: the default)
      buf.fillerMs = fillerMs(res.result.filler_ms);
      if (buf.fillerMs) void this.warmFillers(); // the filler's audio is ready long before it is needed
      const acked = res.result.ms;
      ms = typeof acked === "number" && Number.isFinite(acked) && acked >= 0 ? acked || LISTEN_CEILING_MS : (w.result.ms ?? LISTEN_CEILING_MS) || LISTEN_CEILING_MS;
      if (!buf.ended) {
        await new Promise<void>((resolve) => {
          const timer = this.clock.setTimeout(resolve, ms + this.t.listenGraceMs);
          buf.wake = () => {
            this.clock.clearTimeout(timer);
            resolve();
          };
        });
      }
    } finally {
      if (this.listenBuf === buf) {
        this.listenBuf = undefined;
        this.beginTail(buf); // same step: no frame of this job falls between the two
      }
    }
    const tEnd = buf.endAt ?? this.clock.now();
    const rt = buf.trace.relay; // the turn trace's relay side (ms from the listen's start)
    Object.assign(rt, { voice: pick.name, cmd: tAck - t0, end: tEnd - t0 }, buf.firstAt ? { first_frame: buf.firstAt - t0 } : {}, buf.turnEndAt !== undefined ? { turn_end: buf.turnEndAt - t0 } : {});
    // A listen that broke is an error, never "nobody spoke" (no_speech) nor an empty success:
    // the device went away mid-listen, or its capture failed (end frame reason "error").
    if (buf.lost) return { ok: false, error: "device_offline" };
    if (buf.captureFailed) return { ok: false, error: "listen_failed" };
    // Device VAD heard nothing: no STT call at all. Only the explicit verdict counts:
    // speech_ms is absent when VAD was off, and older firmware sent speech_ms:0 with
    // vad:false -- neither means silence (older firmware without reason: STT as before).
    const noSpeech = buf.reason === "no_speech";
    if (!this.liveSocket() && buf.bytes === 0) return { ok: false, error: "device_offline" };
    if (buf.bytes === 0 && !noSpeech) return { ok: false, error: "no_audio" };
    const all = new Uint8Array(buf.bytes);
    let o = 0;
    for (const c of buf.chunks) {
      all.set(c, o);
      o += c.byteLength;
    }
    const pcm = bytesToPcm16(all);
    const tStt = this.clock.now();
    let served = pick.name;
    let text = "";
    if (noSpeech) {
      // nothing said: no transcription at all (abort() in record() drops a stream)
    } else if (buf.stream) {
      // The box has had every chunk already: only the tail of the transcript is left.
      try {
        text = await withTimeout(this.clock, buf.stream.end(), this.t.boxSttFinalMs, "stt_timeout");
        await this.noteBoxStats({ stt_ms: this.clock.now() - tStt, stt_at: this.clock.now() });
      } catch (e) {
        buf.stream.abort();
        // box mode fails; auto falls back to the cloud, on the audio already buffered here.
        if (pick.mode !== "auto") throw e;
        fallback = errorCode(e);
        served = this.voice.name;
        text = await withTimeout(this.clock, this.voice.stt(pcm), this.t.sttTimeoutMs, "stt_timeout");
      }
    } else {
      // The cloud (or a provider without streaming): one batch call on the whole clip.
      text = await withTimeout(this.clock, pick.provider.stt(pcm), this.t.sttTimeoutMs, "stt_timeout");
    }
    const sttMs = noSpeech ? 0 : this.clock.now() - tStt;
    // Re-assert thinking after STT (covers the device's own idle racing the first one); nothing heard -> idle.
    if (buf.thinking) this.autoMood(buf, text ? "thinking" : "idle");
    const duration = durationMs(pcm.length);
    const reason = buf.reason ?? (!text ? "no_speech" : duration >= ms - 100 ? "max" : "ended");
    Object.assign(rt, { voice: served, stt: sttMs }, fallback ? { fallback } : {});
    return {
      ok: true,
      start: t0,
      text,
      reason,
      ...(buf.speech_ms !== undefined ? { speech_ms: buf.speech_ms } : {}),
      duration_ms: duration,
      ...levels(pcm),
      timings: {
        cmd: tAck - t0,
        record: tEnd - tAck,
        stt: sttMs,
        ...(buf.firstAt ? { first_frame: buf.firstAt - tAck } : {}),
        ...(buf.turnEndAt !== undefined ? { turn_end: buf.turnEndAt - tAck } : {}),
        voice: served,
        ...(fallback ? { voice_fallback: fallback } : {}),
      },
      ...(buf.deviceAck ? { ack: buf.deviceAck } : {}),
      pauses: buf.pauses,
      endAt: tEnd,
      fillerMs: buf.fillerMs ?? fillerMs(undefined),
    };
  }

  /**
   * Relay-driven mood around talking; fire-and-forget. Skipped once the device has
   * accepted an explicit mood or shown an ask since this listen began (the explicit
   * mood stands; the question stays up), and while one is still awaiting its answer.
   */
  private autoMood(buf: ListenBuffer, mood: "thinking" | "idle") {
    if (buf.screenRev !== this.screenRev || this.screenInFlight > 0) return;
    // the device acknowledged the turn itself (chirp + thinking): no thinking from here; idle
    // only takes down a thinking the device put up
    if (buf.deviceAck !== undefined && (mood === "thinking" || buf.deviceAck === "off")) return;
    const args: JsonObject = mood === "thinking" ? { mood, hold_s: this.t.thinkHoldS } : { mood };
    void this.sendCmd("mood", args).then((r) => {
      if (!r.ok) this.log("auto mood failed", { device: this.deviceId, mood, error: r.error });
    });
  }

  /**
   * The STT backend's semantic end of turn (voice box `stt.turn_end`): the listen ends HERE,
   * as if the device's end frame had arrived (reason `turn_end`; record() then finalizes the
   * stream at once). Firmware with the `listen_stop` cap is told to stop recording; older
   * firmware keeps its mic open until its own end, and those late frames are dropped
   * (onAudio ignores an ended listen).
   */
  private onTurnEnd(buf: ListenBuffer, why?: "no_speech") {
    if (buf.ended || this.listenBuf !== buf) return;
    const now = this.clock.now();
    buf.ended = true;
    buf.relayEnded = true;
    buf.endAt = now;
    buf.turnEndAt = now;
    // no_speech (the box heard nobody, device VAD off): as the device's own no_speech — no STT, transcript ""
    buf.reason = why === "no_speech" ? "no_speech" : "turn_end";
    if (this.liveSocket()?.getMeta()?.caps?.includes("listen_stop")) {
      // the reason rides along so the device ends with it and applies its acknowledgement
      // rules to it: no_speech → no chirp, no thinking (older firmware ignores it: turn_end)
      void this.sendCmd("listen_stop", { job: buf.job, reason: buf.reason }).then((r) => {
        if (!r.ok) this.log("listen_stop failed", { device: this.deviceId, job: buf.job, error: r.error });
      });
    }
    if (buf.bytes > 0 && buf.reason !== "no_speech") {
      buf.thinking = true;
      this.autoMood(buf, "thinking");
    }
    buf.wake?.();
  }

  /**
   * The voice box's stream failed mid-listen. With the device VAD on, nothing changes (the
   * device still ends the listen; record() then falls back or fails as before). With it off
   * (semantic), nobody would end the turn: end the listen now, reason `error` — what was
   * said so far is transcribed (auto mode: by the cloud) — instead of making the human wait
   * out the window.
   */
  private onBoxFail(buf: ListenBuffer) {
    buf.boxFailed = true;
    if (!buf.semantic || buf.ended || this.listenBuf !== buf) return;
    this.onTurnEnd(buf);
    buf.turnEndAt = undefined;
    buf.reason = "error";
  }

  private onAudio(bytes: Uint8Array) {
    const frame = decodeAudioFrame(bytes);
    if (!frame) return;
    const tail = this.traceTail;
    if (tail && frame.header.s === tail.buf.job) {
      // a listen already over: its trace takes the frame's diagnostics, never its audio
      const tb = tail.buf;
      if (tb.order.offer(frame.header.seq, frame, this.clock.now()) === "dup") return;
      tb.trace.frame(frame.header, frame.payload.byteLength, this.clock.now());
      tb.trace.late++;
      if (frame.header.end) tail.done();
      return;
    }
    const buf = this.listenBuf;
    if (!buf || frame.header.s !== buf.job) return;
    // In seq order: the device resends a frame that failed to go out, so one may come late or twice.
    const now = this.clock.now();
    const ready = buf.order.offer(frame.header.seq, frame, now);
    if (ready === "dup") return;
    // noted in the trace either way: the device's end frame (its `tr` summary) follows a box turn end
    buf.trace.frame(frame.header, frame.payload.byteLength, now);
    if (frame.header.end) buf.devEnd = true;
    if (buf.ended) {
      buf.trace.late += Math.max(1, ready.length); // after the end (or the box's turn end): dropped, as ever — counted
      return;
    }
    this.takeFrames(buf, ready);
  }

  /** Frames now in order: taken one by one; a gap still open is given up after TRACE_LIMITS.gapMs. */
  private takeFrames(buf: ListenBuffer, frames: DecodedAudio[]) {
    for (const f of frames) {
      if (buf.ended) buf.trace.late++;
      else this.takeFrame(buf, f);
    }
    this.clock.clearTimeout(buf.gapTimer);
    buf.gapTimer = undefined;
    const since = buf.ended ? undefined : buf.order.waitingSince();
    if (since === undefined) return;
    buf.gapTimer = this.clock.setTimeout(() => {
      buf.gapTimer = undefined;
      if (this.listenBuf === buf && !buf.ended) this.takeFrames(buf, buf.order.skip());
    }, Math.max(0, since + TRACE_LIMITS.gapMs - this.clock.now()));
  }

  private takeFrame(buf: ListenBuffer, frame: DecodedAudio) {
    if (buf.bytes + frame.payload.byteLength <= MAX_LISTEN_BYTES && frame.payload.byteLength > 0) {
      buf.chunks.push(frame.payload.slice());
      buf.bytes += frame.payload.byteLength;
      buf.firstAt ??= this.clock.now();
      // Streaming STT: forwarded now, while the human is still talking.
      buf.stream?.push(bytesToPcm16(frame.payload));
    }
    if (frame.header.end) {
      buf.ended = true;
      buf.endAt = this.clock.now();
      if (frame.header.reason) buf.reason = frame.header.reason;
      if (frame.header.reason === "error") buf.captureFailed = true;
      if (frame.header.speech_ms !== undefined) buf.speech_ms = frame.header.speech_ms;
      // End of speech: show thinking right away, while STT runs.
      if (buf.bytes > 0 && buf.reason !== "no_speech" && buf.reason !== "error") {
        buf.thinking = true;
        this.autoMood(buf, "thinking");
      }
      buf.wake?.();
    }
  }


  // =====================================================================
  // Shake to talk (core/turns.ts, docs/AGENT-LINK.md "Shake to talk")
  // =====================================================================

  /**
   * Device `shake`. With `listen` (the local id of the listen the device began on it), the relay
   * adopts that listen as a job — or declines it (listen_stop) when audio is busy or the board
   * has no mic. The agents' shake event then says `listening: true` and carries the `turn_id`
   * its utterance will have. The plain shake is appended either way (rate-capped as ever).
   */
  private async onShake(data: Record<string, unknown>) {
    const clean = agentEventData("shake", data);
    if (!clean) return;
    await this.noteMisfire();
    if (typeof data.listen === "string" && LOCAL_LISTEN_RE.test(data.listen)) {
      // the adopted listen's turn trace starts with its shake (record() takes it by the local id)
      this.shakes.set(data.listen, { at: this.clock.now(), data: { peak_g: clean.peak_g ?? null, rev: clean.reversals ?? null, ms: clean.ms ?? null } });
      while (this.shakes.size > 4) this.shakes.delete(this.shakes.keys().next().value!);
      const job = await this.adoptShake(data.listen);
      if (job) Object.assign(clean, { listening: true, turn_id: job });
    }
    await this.appendEvent("shake", clean);
    await this.activity.device("shake", clean);
  }

  /**
   * The shake's listen as a relay listen job (`cmd listen {adopt}`), or declined. Returns the job id.
   * Adopted: the human's turn is OPEN from here until its utterance (human_speaking for agents'
   * say / talk / ask / mood / show; drafts and reacts accepted), with this device's partial policy.
   */
  private async adoptShake(local: string): Promise<string | undefined> {
    const caps = this.liveSocket()?.getMeta()?.caps;
    const policy = { ...policyFor(await this.kv.get<SpeechStats>(K.speech)), lead_ms: this.t.partialLeadMs };
    const open = async (id: string): Promise<Outcome<JsonObject>> => {
      const now = this.clock.now();
      this.openTurn = { id, startAt: now, chunker: new PartialChunker(policy, now), drafts: new Map() };
      return { ok: true, result: {} };
    };
    const started = !caps || caps.includes("listen") ? await this.startJob("listen", { adopt: local, trigger: "shake" }, true, undefined, open) : fail("unsupported");
    if (started.ok) return started.result.job as string;
    // audio busy (a say / listen crossed the shake): the device ends its listen quietly
    void this.sendCmd("listen_stop", { job: local, reason: "no_speech" }).then((r) => {
      if (!r.ok) this.log("shake listen decline failed", { device: this.deviceId, error: r.error });
    });
    this.log("shake listen declined", { device: this.deviceId, error: started.error });
    return undefined;
  }

  /** The open turn is still being listened to (its listen has not ended). */
  private turnLive(turn: OpenTurn): boolean {
    return this.listenBuf?.job === turn.id && !this.listenBuf.ended;
  }

  /** The human is still talking (an open shake turn): agents may not speak or take the screen. */
  private humanSpeaking(): ReturnType<typeof fail> | undefined {
    const t = this.openTurn;
    if (!t) return undefined;
    if (!this.tasks.has(t.id) && this.audioJob !== t.id) {
      // its listen is gone (it can only end through endTurn; this is a backstop)
      this.closeTurn(t);
      return undefined;
    }
    return fail("human_speaking", `the human is still talking to Molty (turn ${t.id}): stay quiet until their utterance; meanwhile only molty_draft and molty_react are accepted`, { turn_id: t.id });
  }

  private closeTurn(t: OpenTurn) {
    if (t.timer !== undefined) this.clock.clearTimeout(t.timer);
    t.timer = undefined;
    if (this.openTurn === t) this.openTurn = undefined;
  }

  /** The box's text so far (any listen it streams): pauses for the calibration, partials for an open turn. */
  private onPartial(buf: ListenBuffer, text: string) {
    if (buf.ended || this.listenBuf !== buf) return;
    buf.pauses.update(text, this.clock.now());
    buf.reflex?.text(text);
    const turn = this.openTurn;
    if (!turn || turn.id !== buf.job) return;
    this.emitPartial(turn, turn.chunker.update(text, this.clock.now()));
  }

  /** One partial out (if due), then a timer for words that wait (the human paused mid-chunk). */
  private emitPartial(turn: OpenTurn, p: ReturnType<PartialChunker["check"]>) {
    if (p) {
      void this.appendEvent(PARTIAL_EVENT, { turn_id: turn.id, ...p }, true);
      this.turnTrace(turn)?.partial(p.seq, words(p.text_so_far).length, this.clock.now());
    }
    if (turn.timer !== undefined) this.clock.clearTimeout(turn.timer);
    turn.timer = undefined;
    const due = turn.chunker.dueAt();
    if (due === undefined) return;
    turn.timer = this.clock.setTimeout(() => {
      turn.timer = undefined;
      if (this.openTurn !== turn || !this.turnLive(turn)) return;
      this.emitPartial(turn, turn.chunker.check(this.clock.now()));
    }, Math.max(0, due - this.clock.now()) + 5);
  }

  /**
   * The shake's listen ended (`r` undefined: it failed). Settles the turn, in this order:
   * 1. drafts are judged against the final words (core/partials.ts judgeDraft): the first that holds
   *    wins, the rest are dropped;
   * 2. heard words → the latest turn (KV, before any event so the fastest answer finds it), claimed by
   *    the winner when there is one;
   * 3. the winner's opener takes the listen's audio lock directly and plays at once (after the device's
   *    ack chirp); the turn closes (agents may speak again);
   * 4. events: draft.used / draft.dropped (each to its agent only), then ONE `utterance` to every agent;
   * 5. the activity log and the calibration record.
   * Nobody spoke (no_speech, or STT heard nothing): no utterance at all, drafts dropped.
   */
  private async endTurn(id: string, r: Extract<ListenResult, { ok: true }> | undefined) {
    const turn = this.openTurn?.id === id ? this.openTurn : undefined;
    const now = this.clock.now();
    const text = r?.text.trim() ?? "";
    const drafts = turn ? [...turn.drafts.values()].sort((a, b) => a.at - b.at) : [];
    const drops: Array<{ d: HeldDraft; reason: DropReason; new_words?: number }> = [];
    let winner: { d: HeldDraft; new_words: number; job?: string } | undefined;
    try {
      for (const d of drafts) {
        if (!r) drops.push({ d, reason: "turn_failed" });
        else if (!text) drops.push({ d, reason: "no_speech" });
        else if (d.prepared.feed.done && d.prepared.feed.error !== undefined) drops.push({ d, reason: "synth_failed" });
        else {
          const v = judgeDraft(d.basedText, text, d.max_new_words);
          if (!v.ok) drops.push({ d, reason: v.reason, new_words: v.new_words });
          else if (winner) drops.push({ d, reason: "turn_claimed", new_words: v.new_words });
          else winner = { d, new_words: v.new_words };
        }
      }
      // the opener goes out FIRST (its cmd say before any storage write); the turn stays open — every
      // other say refused human_speaking — until the record below is written and the turn closes
      if (winner) {
        const w = winner.d;
        const started = await this.startJob(
          "say",
          { text: w.opener, opener: true, ...(w.voice ? { voice: w.voice } : {}), turn: { id, at: now } as unknown as JsonObject, called: now },
          true,
          w.by,
          undefined,
          { takeover: id, prepared: w.prepared, lead: w.hash },
        );
        if (started.ok) {
          winner.job = started.result.job as string;
          this.opener = { job: winner.job, turn: id, hash: w.hash, usedAt: now };
        } else {
          // cannot happen while the listen holds the lock; never leave the turn claimed for nothing
          drops.push({ d: w, reason: "turn_failed", new_words: winner.new_words });
          winner = undefined;
        }
      }
      if (text) await this.kv.put<TurnRecord>(K.turn, { id, at: now, ...(winner ? { by: winner.d.hash } : {}) });
      if (text && winner) await this.kv.put(K.lastAnswerer, winner.d.hash);
    } finally {
      if (turn) this.closeTurn(turn);
    }
    for (const { d, reason, new_words } of drops) {
      d.prepared.feed.end();
      await this.appendEvent(DRAFT_DROPPED_EVENT, { turn_id: id, based_on: d.based_on, reason, ...(new_words !== undefined ? { new_words } : {}) }, true, d.hash);
      await this.activity.record({ ts: now, kind: "device", ev: DRAFT_DROPPED_EVENT, job: id, agent: d.by.agent, who: d.by.who, ok: true, data: { reason, based_on: d.based_on, ...(new_words !== undefined ? { new_words } : {}) } });
    }
    if (winner) {
      const w = winner.d;
      await this.appendEvent(DRAFT_USED_EVENT, { turn_id: id, based_on: w.based_on, opener: w.opener, new_words: winner.new_words, job: winner.job! }, true, w.hash);
      await this.activity.record({ ts: now, kind: "device", ev: DRAFT_USED_EVENT, job: id, agent: w.by.agent, who: w.by.who, ok: true, data: { opener: w.opener, based_on: w.based_on, new_words: winner.new_words, say_job: winner.job! } });
    }
    const partials = turn?.chunker.seq ?? 0;
    // the turn trace: how its drafts ended (no openers' words, only the outcome)
    if (r?.trace && drafts.length) {
      r.trace.relay.draft = {
        ...(winner ? { used: winner.d.based_on, new_words: winner.new_words, opener_at: now - r.trace.t0 } : {}),
        ...(drops.length ? { dropped: drops.map((x) => x.reason) } : {}),
      };
    }
    if (r && text) {
      const data = utteranceData({
        turn_id: id,
        text,
        reason: r.reason,
        speech_ms: r.speech_ms ?? r.duration_ms,
        stt_ms: (r.timings.stt as number) ?? 0,
        trigger: "shake",
        caps: this.liveSocket()?.getMeta()?.caps ?? [],
        ack: r.ack,
        voice: await this.voiceName(),
      });
      // an agent's opener is already playing: the turn is taken (others stay quiet)
      if (winner) data.claimed = true;
      // otherwise: when Molty says its own filler unless an agent acts first (core/phrases.ts)
      else {
        const fill = this.fillerIn(r);
        if (fill !== undefined) data.filler_in_ms = fill;
      }
      // agent-facing, one per turn: not subject to the device event rate cap
      await this.appendEvent(UTTERANCE_EVENT, data, true);
      if (r.trace) r.trace.relay.utterance = this.clock.now() - r.trace.t0;
      await this.activity.record({
        ts: this.clock.now(),
        kind: "device",
        ev: "utterance",
        job: id,
        data: { text: text.slice(0, 500), reason: r.reason, trigger: "shake", words: words(text).length, partials, drafts: drafts.length, ...(winner ? { draft: "used" } : {}) },
      });
    }
    // the calibration: this turn's habits and every draft's outcome
    if (r && text) await this.noteHeard(r);
    if (partials || drafts.length) {
      await this.updateSpeech((s) => {
        let next: SpeechStats = { ...s, totals: { ...s.totals, partials: s.totals.partials + partials, drafts: s.totals.drafts + drafts.length } };
        for (const { reason } of drops) next = addOutcome(next, reason as DraftOutcome, now);
        if (winner) next = addOutcome(next, "used", now);
        return next;
      });
    }
  }

  /** A heard listen's habits (words, length, pauses) into the calibration record. */
  private async noteHeard(r: Extract<ListenResult, { ok: true }>) {
    const n = words(r.text.trim()).length;
    if (!n) return;
    await this.updateSpeech((s) => addTurn(s, { at: this.clock.now(), words: n, ms: r.speech_ms ?? r.duration_ms, pauses: r.pauses.pauses, max_pause_ms: r.pauses.max_pause_ms }));
  }

  /**
   * A shake while the last opener (or its follow-up) plays, or just after it: the human cut in —
   * that opener was probably wrong (calibration: `used` → `misfire`). Counted once per opener.
   */
  private async noteMisfire() {
    const o = this.opener;
    if (!o || o.counted) return;
    const playing = this.audioJob !== undefined && (this.audioJob === o.job || this.audioJob === o.follow);
    const now = this.clock.now();
    if (!playing && !(o.endedAt !== undefined && now - o.endedAt <= CALIBRATION.misfireMs)) return;
    o.counted = true;
    await this.updateSpeech((s) => markMisfire(s, o.usedAt, now));
    await this.activity.record({ ts: now, kind: "device", ev: "draft.misfire", job: o.turn, data: { say_job: o.job } });
  }

  /** Serialized read-modify-write of the calibration record; a changed threshold or policy goes to the activity log. */
  private updateSpeech(fn: (s: SpeechStats) => SpeechStats): Promise<void> {
    const run = this.speechChain.then(async () => {
      const before = (await this.kv.get<SpeechStats>(K.speech)) ?? emptyStats(this.clock.now());
      const after = fn(before);
      await this.kv.put(K.speech, after);
      const p0 = policyFor(before);
      const p1 = policyFor(after);
      const changed: JsonObject = {};
      if (before.threshold !== after.threshold) changed.threshold = { from: before.threshold, to: after.threshold };
      if (p0.source !== p1.source || p0.every_words !== p1.every_words || p0.every_ms !== p1.every_ms) {
        changed.partials = { source: p1.source, every_words: p1.every_words, every_ms: p1.every_ms, lead_ms: p1.lead_ms };
      }
      if (Object.keys(changed).length) await this.activity.record({ ts: this.clock.now(), kind: "settings", ev: "calibration", data: { ...changed, turns: after.turns.length } });
    });
    this.speechChain = run.catch((e: unknown) => this.log("speech stats failed", { device: this.deviceId, error: errorCode(e) }));
    return this.speechChain as Promise<void>;
  }

  /**
   * The job a say / talk / quick by `hash` queues behind: the lead-in playing now (startJob `lead`) —
   * its own draft opener or quick phrase, or Molty's own filler (anyone's answer follows that).
   */
  private followUpOf(hash: string): string | undefined {
    const l = this.lead;
    return l && (l.hash === hash || l.hash === RELAY_LEAD) && this.audioJob === l.job && !l.follow && !this.handoff ? l.job : undefined;
  }

  /** `job` was queued behind the lead-in `after`: nothing else may queue there (and an opener's misfire counts its follow-up). */
  private noteFollow(after: string, job: string) {
    if (this.lead?.job === after) this.lead.follow = job;
    if (this.opener?.job === after) this.opener.follow = job;
  }

  /** Would a say / talk by `hash` be refused the latest turn? Read only: nothing is claimed. */
  private async claimCheck(hash: string, inReplyTo: string | undefined): Promise<Outcome<JsonObject>> {
    const out = claimTurn(await this.kv.get<TurnRecord>(K.turn), hash, inReplyTo, this.clock.now());
    if (!out.ok) return fail("turn_claimed", `another agent is already answering the human's turn ${out.turn}: stay quiet (or answer the next one)`, { turn_id: out.turn });
    return { ok: true, result: {} };
  }

  /** The claim of an admitted say / talk, as the args its job needs ({turn, called} when it answers a turn). */
  private async claimFor(hash: string, inReplyTo: string | undefined, called: number): Promise<Outcome<JsonObject>> {
    const c = await this.claim(hash, inReplyTo);
    if (!c.ok) return c;
    return { ok: true, result: c.result.turn ? { turn: c.result.turn, called } : {} };
  }

  /** A say / talk against the latest turn (claimTurn), the claim written atomically. */
  private async claim(hash: string, inReplyTo: string | undefined): Promise<Outcome<{ turn?: JsonObject }>> {
    const now = this.clock.now();
    const c = await this.kv.update<TurnRecord, TurnClaim>(K.turn, (cur) => {
      const out = claimTurn(cur, hash, inReplyTo, now);
      return out.ok && out.next ? { next: out.next, result: out } : { result: out };
    });
    if (!c.ok) return fail("turn_claimed", `another agent is already answering the human's turn ${c.turn}: stay quiet (or answer the next one)`, { turn_id: c.turn });
    if (c.turn) await this.kv.put(K.lastAnswerer, hash); // the agent answering the human now: Molty's fillers are its own
    return { ok: true, result: c.turn ? { turn: c.turn as unknown as JsonObject } : {} };
  }

  /**
   * A say / talk that answers a human turn: one `trace` entry `ev:"reply"` on that turn (its
   * listen job), attributed to the answering agent — when it called, its first audio, and its
   * own client_timing. Never throws (the trace must not break the answer).
   */
  private async replyTrace(id: string, op: "say" | "talk" | "ask" | "quick", args: JsonObject, timings: JsonObject) {
    const turn = isRecord(timings.turn) ? timings.turn : undefined;
    if (!turn || typeof turn.id !== "string") return;
    const by = (await this.kv.get<JobRecord>(K.job(id)))?.by;
    const data: JsonObject = { op, job: id };
    if (typeof turn.reply === "number") data.reply_ms = turn.reply;
    if (typeof turn.first_audio === "number") data.first_audio_ms = turn.first_audio;
    if (isRecord(args.client_timing)) data.client_timing = args.client_timing;
    await this.activity.record({ ts: this.clock.now(), kind: "trace", ev: "reply", job: turn.id, ...(by ? { agent: by.agent, who: by.who } : {}), data });
  }

  /** timings.turn of an answer: the call and its first audio, in ms after the utterance. */
  private answerTimings(args: JsonObject, firstAudioAt?: number): JsonObject {
    const turn = args.turn as unknown as { id: string; at: number };
    return turnTimings(turn, args.called as number, firstAudioAt);
  }

  // =====================================================================
  // While the human speaks (core/partials.ts, docs/AGENT-LINK.md "While the human speaks")
  // =====================================================================

  async draft(token: string, req: unknown): Promise<Outcome> {
    return this.logged(token, "draft", req, () => this.draftNow(token, req));
  }

  /**
   * Holds a short opener for the open turn, based on one of its partials; its audio is synthesised
   * now, while the human still talks. One draft per agent per turn (a new one replaces it), at most
   * MAX_DRAFTS_PER_TURN made. Settled in endTurn.
   */
  private async draftNow(token: string, req: unknown): Promise<Outcome> {
    const a = await this.agentAuth(token);
    if ("ok" in a) return a;
    let args;
    try {
      args = validateDraft(req);
    } catch (e) {
      if (e instanceof CommandError) return fail(e.code, e.message);
      throw e;
    }
    const turn = this.openTurn;
    if (!turn || turn.id !== args.turn_id || !this.turnLive(turn)) return this.turnClosed(args.turn_id);
    const caps = this.liveSocket()?.getMeta()?.caps;
    if (caps && !caps.includes("say")) return fail("unsupported", "this device has no speaker");
    const basedText = turn.chunker.textOf(args.based_on);
    if (basedText === undefined) return fail("invalid_args", `based_on must be the seq of a partial of this turn (1..${turn.chunker.seq} so far)`);
    if ((turn.drafts.get(a.hash)?.n ?? 0) >= MAX_DRAFTS_PER_TURN) return fail("rate_limited", `at most ${MAX_DRAFTS_PER_TURN} drafts per turn: keep the one you have`);
    const threshold = (await this.kv.get<SpeechStats>(K.speech))?.threshold ?? CALIBRATION.threshold.initial;
    const max = Math.min(args.max_new_words ?? threshold, threshold);
    // re-based on a newer partial with the same words: its audio is already (being) synthesised — keep it
    const same = turn.drafts.get(a.hash);
    const reuse = same && same.opener === args.opener && same.voice === args.voice && !(same.prepared.feed.done && same.prepared.feed.error !== undefined);
    // an opener that is one of the agent's quick phrases (or a built-in) plays from the phrase cache
    const prepared = reuse ? same.prepared : await this.prepareSpeech(args.opener, args.voice, "if_cached");
    if (!("pick" in prepared)) return fail(prepared.error, "no voice to pre-synthesise the opener with");
    // the human may have finished while the voice was picked
    if (this.openTurn !== turn || !this.turnLive(turn)) {
      if (!reuse) prepared.feed.end();
      return this.turnClosed(args.turn_id);
    }
    const prev = turn.drafts.get(a.hash);
    if (prev) {
      if (prev.prepared !== prepared) prev.prepared.feed.end();
      void this.appendEvent(DRAFT_DROPPED_EVENT, { turn_id: turn.id, based_on: prev.based_on, reason: "replaced" }, true, a.hash);
    }
    this.turnTrace(turn)?.draft(args.based_on, this.clock.now());
    turn.drafts.set(a.hash, {
      hash: a.hash,
      by: this.who(a),
      based_on: args.based_on,
      basedText,
      opener: args.opener,
      max_new_words: max,
      ...(args.voice ? { voice: args.voice } : {}),
      prepared,
      at: this.clock.now(),
      n: (prev?.n ?? 0) + 1,
    });
    return this.ride(a, { ok: true, result: { turn_id: turn.id, based_on: args.based_on, status: "held", max_new_words: max, threshold } });
  }

  async react(token: string, req: unknown): Promise<Outcome> {
    return this.logged(token, "react", req, () => this.reactNow(token, req));
  }

  /**
   * A mood over the listening animation while the human talks: screen only (the firmware's
   * `mood {react:true}` never counts as an explicit mood, so the turn's acknowledgement stays),
   * back to listening after reactShowMs, at most one per REACT_MIN_GAP_MS per Molty.
   */
  private async reactNow(token: string, req: unknown): Promise<Outcome> {
    const a = await this.agentAuth(token);
    if ("ok" in a) return a;
    let args;
    try {
      args = validateReact(req);
    } catch (e) {
      if (e instanceof CommandError) return fail(e.code, e.message);
      throw e;
    }
    const turn = this.openTurn;
    if (!turn || turn.id !== args.turn_id || !this.turnLive(turn)) return this.turnClosed(args.turn_id);
    // a mood needs firmware that shows it without counting it (cap react); a gesture is the device's
    // act (cap act), which never takes the screen from the listen in the first place
    const caps = this.liveSocket()?.getMeta()?.caps;
    const what = args.mood ?? args.action;
    if (args.mood !== undefined && !caps?.includes(REACT_CAP)) {
      return fail("unsupported", "this Molty's firmware can't show a react mood over its listening (no `react` in its caps): update Molty (molty_updates)");
    }
    if (args.action !== undefined && !caps?.includes(FEATURES.act.cap)) return fail("unsupported", "this Molty has no gestures (no `act` in its caps)");
    const now = this.clock.now();
    const wait = this.lastReactAt + REACT_MIN_GAP_MS - now;
    if (wait > 0) {
      this.turnTrace(turn)?.react(what, now, "rate_limited");
      return fail("rate_limited", `one react per ${REACT_MIN_GAP_MS / 1000} s on a Molty: try again in ${wait} ms`, { retry_after_ms: wait });
    }
    this.lastReactAt = now;
    const res = args.mood !== undefined ? await this.sendCmd("mood", { mood: args.mood, hold_s: 0, react: true }) : await this.sendCmd("act", { action: args.action });
    this.turnTrace(turn)?.react(what, now, res.ok ? "ok" : res.error);
    if (!res.ok) return res;
    turn.agentReacted = true; // the reflex yields to the agent for the rest of this turn
    await this.updateSpeech((s) => ({ ...s, totals: { ...s.totals, reacts: s.totals.reacts + 1 } }));
    // a gesture plays over the listening and ends by itself
    if (args.action !== undefined) return this.ride(a, { ok: true, result: { turn_id: turn.id, shown: args.action, next_after_ms: REACT_MIN_GAP_MS } });
    this.clock.clearTimeout(this.reactTimer);
    this.reactTimer = this.clock.setTimeout(() => {
      // back to listening — unless the human finished meanwhile (the device's own ack owns the screen then)
      if (this.openTurn !== turn || !this.turnLive(turn)) return;
      void this.sendCmd("mood", { mood: "listening", hold_s: 0, react: true });
    }, this.t.reactShowMs);
    return this.ride(a, { ok: true, result: { turn_id: turn.id, shown: args.mood, for_ms: this.t.reactShowMs, next_after_ms: REACT_MIN_GAP_MS } });
  }

  // ---------------- Molty's reflex (core/reflex.ts, docs/AGENT-LINK.md "Reflex") ----------------

  /** The owner's reflex setting for one box-streamed listen (undefined: off). */
  private reflexFor(buf: ListenBuffer, cfg: VoiceCfg): ReflexSession | undefined {
    const mode = cfg.reflex ?? "off";
    if (mode === "off") return undefined;
    const avail = { local: !!this.boxSocket()?.getMeta()?.box?.reflex, jev: !!this.reflexKeys.jev, openai: !!this.reflexKeys.openai };
    const session: ReflexSession = new ReflexSession({
      mode,
      compare: !!cfg.reflex_compare,
      avail,
      keys: this.reflexKeys,
      fetch: this.fetcher,
      clock: this.clock,
      bench: this.reflexBench,
      liveliness: cfg.liveliness ?? DEFAULT_LIVELINESS,
      // the session's profile (an unknown stored value falls back to the default there)
      fire: (pick, soft): Promise<string> => this.reflexFire(buf, pick, soft, session.profile.gapMs),
      note: (n) => buf.trace.reflex({ ...n, at: n.at - buf.trace.t0 }),
    });
    this.lastReflex = session;
    buf.trace.relay.reflex_cfg = { mode, engine: session.engine, lv: session.liveliness, ...(session.fallback ? { fb: session.fallback } : {}), ...(cfg.reflex_compare ? { compare: session.engines } : {}) };
    return session;
  }

  /** The box's classifier verdict for one reflex chunk (stt.reflex), while that listen runs. */
  private onBoxReflex(buf: ListenBuffer, f: Record<string, unknown>) {
    if (buf.ended || this.listenBuf !== buf) return;
    const v = parseBoxReflex(f);
    if (v) buf.reflex?.onLocal(v);
  }

  /**
   * The utterance category of `text` (the latest partial or final) for a context-picked filler: the
   * reflex's own answer for those words when it has one, else one call of the owner's reflex engine
   * (keywords when the reflex is off, keyless or failing). The filler-category seam of feat/quick-phrases.
   */
  async fillerCategory(text: string): Promise<{ category: UtteranceCategory; engine: ReflexEngine; ms: number }> {
    const latest = this.lastReflex?.latest;
    if (latest && latest.n === words(text).length) return { category: latest.cat, engine: latest.engine, ms: 0 };
    const mode = (await this.voiceCfg()).reflex ?? "off";
    const engine = mode === "off" ? "keywords" : resolveEngine(mode, { local: false, jev: !!this.reflexKeys.jev, openai: !!this.reflexKeys.openai }).engine;
    return categoryOf(text, { engine, keys: this.reflexKeys, fetch: this.fetcher, clock: this.clock });
  }

  /** Something owns Molty's screen besides the listen: a question, a pairing code, an update. */
  private async ownerScreen(): Promise<boolean> {
    if (this.pendingAsks.size) return true;
    const now = this.clock.now();
    const pid = await this.kv.get<string>(K.pairCurrent);
    if (pid) {
      const p = await this.kv.get<PairRecord>(K.pair(pid));
      if (p && p.status === "pending" && p.expires > now) return true;
    }
    return !!(await this.kv.get<UpdateRecord>(K.upd));
  }

  /**
   * Shows a reflex pick through the react path — screen only, never a sound: in full (REFLEX_SHOW)
   * or, a mild pick, as its softer gesture (REFLEX_SOFT). Not when the listen is over, an owner
   * screen is up, an agent reacted this turn (agents win) or within `gapMs` (the liveliness's) of
   * any react / reflex. Resolves "ok" or why not.
   */
  private async reflexFire(buf: ListenBuffer, pick: Exclude<ReflexPick, "none">, soft: boolean, gapMs: number): Promise<string> {
    const live = () => !buf.ended && this.listenBuf === buf;
    if (!live()) return "closed";
    const turn = this.openTurn?.id === buf.job ? this.openTurn : undefined;
    const glance = pick === "curious" && soft ? (this.reflexGlance % 2 ? "glance_right" : "glance_left") : undefined;
    const show: { mood: "happy" | "surprised" | "thinking" } | { action: string } = soft ? { action: glance ?? REFLEX_SOFT[pick] } : REFLEX_SHOW[pick];
    const caps = this.liveSocket()?.getMeta()?.caps;
    if ("mood" in show ? !caps?.includes(REACT_CAP) : !caps?.includes(FEATURES.act.cap)) return "unsupported";
    if (await this.ownerScreen()) return "screen";
    // after the reads: an agent's react or the listen's end may have come meanwhile
    if (!live()) return "closed";
    if (turn?.agentReacted || this.clock.now() - this.lastReactAt < gapMs) return "agent";
    if (this.clock.now() - this.lastReflexAt < gapMs) return "rate_limited";
    this.lastReflexAt = this.clock.now();
    if (glance) this.reflexGlance++;
    const res = "mood" in show ? await this.sendCmd("mood", { mood: show.mood, hold_s: 0, react: true }) : await this.sendCmd("act", { action: show.action });
    if (!res.ok) return res.error;
    if ("mood" in show) {
      this.clock.clearTimeout(this.reactTimer);
      this.reactTimer = this.clock.setTimeout(() => {
        // back to listening — unless the human finished meanwhile (the device's own ack owns the screen then)
        if (!live()) return;
        void this.sendCmd("mood", { mood: "listening", hold_s: 0, react: true });
      }, this.t.reactShowMs);
    }
    return "ok";
  }

  /** The open turn's listen trace (core/trace.ts), while its listen runs. */
  private turnTrace(turn: OpenTurn): ListenTrace | undefined {
    return this.listenBuf?.job === turn.id ? this.listenBuf.trace : undefined;
  }

  private turnClosed(turnId: string) {
    return fail("turn_closed", "that human turn is not open (they finished, or it is not the current turn): answer its utterance as usual", { turn_id: turnId });
  }

  async notes(token: string, req: NotesRequest): Promise<Outcome> {
    // the log keeps the op and the key, never the value (notes are private to their agent)
    const summary: JsonObject = { action: String(req.action) };
    if ("key" in req && typeof req.key === "string") summary.key = req.key;
    return this.logged(token, "notes", summary, async () => {
      const a = await this.agentAuth(token);
      if ("ok" in a) return a;
      if (!isRecord(req) || !["list", "get", "put", "delete"].includes(req.action as string)) return fail("invalid_args", "action must be list, get, put or delete");
      const out = await notesOp(this.kv, a.hash, req, this.clock.now());
      return out.ok ? this.ride(a, out) : fail(out.error, out.message);
    });
  }

  /** Owner: how this Molty's human talks, the partial policy and the draft record (link-code auth). */
  async speechStats(linkCode: string, req: { action: "get" | "reset" }): Promise<Outcome> {
    const denied = await this.ownerAuth(linkCode);
    if (denied) return denied;
    if (req.action === "reset") {
      await this.speechChain;
      await this.kv.delete(K.speech);
      await this.activity.record({ ts: this.clock.now(), kind: "settings", ev: "calibration", data: { reset: true } });
    }
    await this.speechChain;
    return { ok: true, result: speechSummary(await this.kv.get<SpeechStats>(K.speech)) as unknown as JsonObject };
  }

  // =====================================================================
  // Voice box (docs/AGENT-LINK.md "Voice box")
  // =====================================================================

  /** The voice socket's upgrade: a VOICE token of this device, nothing else. */
  async voiceAuth(token: string): Promise<Outcome<{ hash: string }>> {
    const a = await this.auth(token);
    if (!a) return fail("unauthorized", "unknown or revoked voice box token: pair the box again (molty-voice pair)");
    if (a.rec.role !== "voice") return fail("forbidden", 'this is an agent token: the voice socket takes a voice box token (pair with role "voice")');
    return { ok: true, result: { hash: a.hash } };
  }

  /** An authenticated voice socket: it replaces any earlier box connection. Used once its voice.hello arrives. */
  voiceOpen(sock: DeviceSocket, tokenHash: string): void {
    sock.setMeta({ stage: "voice", opened: this.clock.now(), tok: tokenHash });
    this.closeBoxes((_m, s) => !s.same(sock), CLOSE_REPLACED, "replaced by a newer voice box connection");
  }

  private async voiceMessage(sock: DeviceSocket, meta: SocketMeta, data: string | Uint8Array): Promise<void> {
    if (meta.retired) return; // closing: nothing it says counts any more
    if (typeof data !== "string") {
      if (!meta.box) return;
      const frame = decodeAudioFrame(data);
      if (frame) this.box.onAudio(sock, frame.header, frame.payload);
      return;
    }
    const frame = parseTextFrame(data);
    if (!frame) return;
    switch (frame.t) {
      case "ping":
        sock.sendText(PONG_TEXT);
        return;
      case "voice.hello":
        return this.onBoxHello(sock, meta, frame);
      case "tts.end":
      case "tts.word":
      case "stt.final":
      case "stt.partial":
      case "stt.turn_end":
      case "stt.hint":
      case "stt.trace":
      case "stt.reflex":
        if (meta.box) this.box.onText(sock, frame);
        return;
      default:
        return;
    }
  }

  private async onBoxHello(sock: DeviceSocket, meta: SocketMeta, f: Record<string, unknown>) {
    const str = (v: unknown, max: number) => typeof v === "string" && v.length > 0 && v.length <= max && !/[\u0000-\u001f\u007f]/.test(v);
    const voices = Array.isArray(f.voices) ? f.voices.filter((v): v is string => str(v, 200)).slice(0, 64) : [];
    if (!str(f.name, 64) || (f.version !== undefined && !str(f.version, 32)) || typeof f.stt !== "boolean" || typeof f.tts !== "boolean" || (f.reflex !== undefined && typeof f.reflex !== "boolean")) {
      return sock.close(CLOSE_PROTOCOL, "malformed voice.hello");
    }
    // Unpaired since the upgrade (revoke / unpair / forget close it too, but never trust a stale socket).
    if (!meta.tok || !(await this.kv.get<TokenRecord>(K.token(meta.tok)))) return sock.close(CLOSE_UNAUTHORIZED, "voice box token revoked");
    const now = this.clock.now();
    // reflex: the box runs Molty's reflex classifier (stt.reflex; only boxes that say so are asked)
    const reflex = f.reflex === true && f.stt === true;
    sock.setMeta({ ...meta, box: { stt: f.stt, tts: f.tts, ...(reflex ? { reflex } : {}) } });
    await this.kv.put<BoxInfo>(K.vbox, { name: f.name as string, version: (f.version as string | undefined) ?? "", voices, stt: f.stt, tts: f.tts, ...(reflex ? { reflex } : {}), since: now });
    const cfg = await this.voiceCfg();
    sock.sendText(JSON.stringify({ t: "voice.welcome", device_id: this.deviceId, mode: cfg.mode, voice: cfg.voice ?? null }));
    this.log("voice box connected", { device: this.deviceId, stt: f.stt, tts: f.tts });
    await this.activity.record({ ts: now, kind: "voice_box", ev: "connect", data: { name: f.name as string, stt: f.stt, tts: f.tts } });
    void this.rewarmPhrases(); // auto mode: the box's voice speaks now; quick phrases follow it
  }

  /** The greeted voice box socket, if any. */
  private boxSocket(): DeviceSocket | undefined {
    return this.host.sockets().find((s) => {
      const m = s.getMeta();
      return m?.stage === "voice" && !!m.box && !m.retired;
    });
  }

  /** Closes voice sockets matching `match`, failing their in-flight requests first. */
  private closeBoxes(match: (m: SocketMeta, s: DeviceSocket) => boolean, code: number, reason: string) {
    for (const s of this.host.sockets()) {
      const m = s.getMeta();
      if (m?.stage !== "voice" || m.retired || !match(m, s)) continue;
      // Ineligible first (in its own attachment), then fail its requests, then close.
      s.setMeta({ ...m, retired: true });
      this.box.drop(s);
      s.close(code, reason);
    }
  }

  /** Closes the matching boxes for good (revoked / unpaired) and forgets the hello of a box no longer there. */
  private async forgetBox(match: (m: SocketMeta) => boolean, reason: string) {
    this.closeBoxes((m) => match(m), CLOSE_UNAUTHORIZED, reason);
    if (!this.host.sockets().some((s) => s.getMeta()?.stage === "voice" && !s.getMeta()?.retired)) await this.kv.delete(K.vbox);
  }

  /** Deletes every voice box token (one box per device) and closes their sockets; returns how many. */
  private async unpairBoxes(reason: string): Promise<number> {
    let n = 0;
    for (const [key, tok] of await this.kv.list<TokenRecord>("tok:")) {
      if (tok.role !== "voice") continue;
      await this.kv.delete(key);
      n++;
    }
    await this.forgetBox(() => true, reason);
    await this.kv.delete(K.vstat);
    return n;
  }

  private async voiceCfg(): Promise<VoiceCfg> {
    return (await this.kv.get<VoiceCfg>(K.voiceCfg)) ?? { mode: "auto" };
  }

  /**
   * Which provider serves one say (tts) or listen (stt), by `voice_mode`: auto = the box when
   * it is connected and offers it, else the cloud (env) provider; box = the box or
   * voice_box_offline; cloud = the env provider always.
   */
  private async pickVoice(kind: "tts" | "stt"): Promise<VoicePick> {
    const cfg = await this.voiceCfg();
    const cloud: VoicePick = { ok: true, provider: this.voice, name: this.voice.name, mode: cfg.mode, box: false, cfg };
    if (cfg.mode === "cloud") return cloud;
    const s = this.boxSocket();
    if (s?.getMeta()?.box?.[kind]) return { ok: true, provider: this.box, name: this.box.name, mode: cfg.mode, box: true, cfg };
    if (cfg.mode === "box") {
      return { ok: false, error: BOX_OFFLINE, message: s ? `the voice box offers no ${kind.toUpperCase()}` : "no voice box is connected (voice_mode is box)" };
    }
    return cloud;
  }

  private async voiceName(): Promise<string> {
    const p = await this.pickVoice("tts");
    return p.ok ? p.name : p.error;
  }

  private async noteBoxStats(patch: BoxStats) {
    await this.kv.update<BoxStats, void>(K.vstat, (cur) => ({ next: { ...cur, ...patch }, result: undefined }));
  }

  /** Owner calls: the device's CURRENT link code (the console reads it from Molty over the LAN). */
  private async ownerAuth(linkCode: string): Promise<Outcome | undefined> {
    const link = await this.kv.get<{ code: string }>(K.link);
    if (typeof linkCode !== "string" || !LINK_CODE_RE.test(linkCode) || !link || !safeEqual(link.code, linkCode)) {
      return fail("unauthorized", "wrong or expired link code (it rotates every 15 min: read it from Molty's console)");
    }
    return undefined;
  }

  /** Owner (console) voice settings, authenticated by the device's current link code. */
  async voiceSettings(linkCode: string, req: VoiceOwnerRequest): Promise<Outcome> {
    const denied = await this.ownerAuth(linkCode);
    if (denied) return denied;
    if (req.action === "unpair") {
      const n = await this.unpairBoxes("voice box unpaired by the owner");
      if (n) await this.activity.record({ ts: this.clock.now(), kind: "voice_box", ev: "unpaired", data: { by: "owner" } });
      return { ok: true, result: { unpaired: n, ...(await this.voiceView()) } };
    }
    if (req.action === "set") {
      const cfg = await this.voiceCfg();
      if (req.mode !== undefined) {
        if (!VOICE_MODES.includes(req.mode as VoiceMode)) return fail("invalid_args", `mode must be one of ${VOICE_MODES.join(", ")}`);
        cfg.mode = req.mode as VoiceMode;
      }
      if (req.voice !== undefined) {
        if (req.voice === null || req.voice === "") delete cfg.voice;
        else if (typeof req.voice !== "string" || req.voice.length > 200 || /[\u0000-\u001f\u007f]/.test(req.voice)) {
          return fail("invalid_args", "voice must be a voice name of 1..200 printable characters, or null for the box's default");
        } else cfg.voice = req.voice;
      }
      if (req.reflex !== undefined) {
        if (!REFLEX_MODES.includes(req.reflex as ReflexMode)) return fail("invalid_args", `reflex must be one of ${REFLEX_MODES.join(", ")}`);
        if (req.reflex === "off") delete cfg.reflex;
        else cfg.reflex = req.reflex as ReflexMode;
      }
      if (req.reflex_compare !== undefined) {
        if (typeof req.reflex_compare !== "boolean") return fail("invalid_args", "reflex_compare must be true or false");
        if (req.reflex_compare) cfg.reflex_compare = true;
        else delete cfg.reflex_compare;
      }
      if (req.liveliness !== undefined) {
        if (!isLiveliness(req.liveliness)) return fail("invalid_args", `liveliness must be one of ${LIVELINESS.join(", ")}`);
        if (req.liveliness === DEFAULT_LIVELINESS) delete cfg.liveliness;
        else cfg.liveliness = req.liveliness;
      }
      await this.kv.put<VoiceCfg>(K.voiceCfg, cfg);
      await this.activity.record({ ts: this.clock.now(), kind: "settings", ev: "voice", data: { mode: cfg.mode, voice: cfg.voice ?? null, ...(req.reflex !== undefined || req.reflex_compare !== undefined ? { reflex: cfg.reflex ?? "off", reflex_compare: !!cfg.reflex_compare } : {}), ...(req.liveliness !== undefined ? { liveliness: cfg.liveliness ?? DEFAULT_LIVELINESS } : {}) } });
      void this.rewarmPhrases(); // quick phrases follow Molty's voice (core/phrases.ts)
    }
    return { ok: true, result: await this.voiceView() };
  }

  // ---------------- activity log (core/activity.ts) ----------------

  /** Owner: the whole merged timeline, or set "log transcripts" (link-code auth, like voiceSettings). */
  async ownerActivity(linkCode: string, req: ActivityOwnerRequest): Promise<Outcome> {
    const denied = await this.ownerAuth(linkCode);
    if (denied) return denied;
    if (req.action === "set") {
      if (req.transcripts === undefined && req.keep_audio === undefined) return fail("invalid_args", "give transcripts and/or keep_audio (true or false)");
      if (req.transcripts !== undefined && typeof req.transcripts !== "boolean") return fail("invalid_args", "transcripts must be true or false");
      if (req.keep_audio !== undefined && typeof req.keep_audio !== "boolean") return fail("invalid_args", "keep_audio must be true or false");
      const before = await this.activity.settings();
      if (typeof req.transcripts === "boolean") await this.activity.setTranscripts(req.transcripts);
      if (typeof req.keep_audio === "boolean") await this.activity.setKeepAudio(req.keep_audio);
      const after = await this.activity.settings();
      // no transcripts means no audio either: what is kept goes now (and when keep_audio goes off)
      if (!after.transcripts || !after.keep_audio) await this.audioKeep.wipe();
      const changed: JsonObject = {};
      if (before.transcripts !== after.transcripts) changed.transcripts = after.transcripts;
      if (!!before.keep_audio !== !!after.keep_audio) changed.keep_audio = !!after.keep_audio;
      if (Object.keys(changed).length) await this.activity.record({ ts: this.clock.now(), kind: "settings", ev: "activity", data: changed });
    }
    const q = parseActivityQuery(req.action === "get" ? req.query : {});
    if (typeof q === "string") return fail("invalid_args", q);
    const page = await this.activity.query(q, { owner: true });
    const cfg = await this.activity.settings();
    return {
      ok: true,
      result: {
        ...page,
        transcripts: cfg.transcripts,
        keep_audio: !!cfg.keep_audio,
        audio: { turns: AUDIO_KEEP.turns, max_s: AUDIO_KEEP.maxMs / 1000, max_age_h: AUDIO_KEEP.maxAgeMs / 3600000 },
        limits: { max_age_h: ACTIVITY_LIMITS.maxAgeMs / 3600000, max_entries: ACTIVITY_LIMITS.chunkEntries * ACTIVITY_LIMITS.maxChunks },
      } as unknown as JsonObject,
    };
  }

  /** Owner: one kept listen's audio as WAV (link-code auth; agents never reach this). */
  async ownerAudio(linkCode: string, job: string): Promise<Outcome<{ wav: Uint8Array }>> {
    const denied = await this.ownerAuth(linkCode);
    if (denied) return denied as Outcome<{ wav: Uint8Array }>;
    if (typeof job !== "string" || !job.startsWith(`${this.deviceId}.j_`)) return fail("not_found");
    const pcm = await this.audioKeep.get(job);
    if (!pcm) return fail("not_found", "no audio kept for this turn (keep audio is off, or it expired after 24 h)");
    return { ok: true, result: { wav: wav(pcm) } };
  }

  /** Agent: its own entries only (+ its jobs' device lines and the device's connects around them). */
  async activityLog(token: string, req: unknown): Promise<Outcome> {
    const a = await this.agentAuth(token);
    if ("ok" in a) return a;
    const { agent: _agent, ...params } = isRecord(req) ? req : {};
    const q = parseActivityQuery(params);
    if (typeof q === "string") return fail("invalid_args", q);
    return { ok: true, result: (await this.activity.query(q, { who: this.who(a).who })) as unknown as JsonObject };
  }

  private async voiceView(): Promise<JsonObject> {
    const cfg = await this.voiceCfg();
    const tok = [...(await this.kv.list<TokenRecord>("tok:")).values()].find((t) => t.role === "voice");
    const connected = this.boxSocket();
    const info = await this.kv.get<BoxInfo>(K.vbox);
    const stats = (await this.kv.get<BoxStats>(K.vstat)) ?? {};
    const serving = async (k: "tts" | "stt") => {
      const p = await this.pickVoice(k);
      return p.ok ? p.name : p.error;
    };
    // the reflex: what each engine needs (local: a connected box with the classifier; jev / openai: the relay's key)
    const local = !!connected?.getMeta()?.box?.reflex;
    const avail = { local, jev: !!this.reflexKeys.jev, openai: !!this.reflexKeys.openai };
    const mode = cfg.reflex ?? "off";
    const sttServing = await serving("stt");
    const now = mode === "off" ? null : resolveEngine(mode, { ...avail, local: local && sttServing === "box" });
    return {
      mode: cfg.mode,
      voice: cfg.voice ?? null,
      cloud: this.voice.name,
      serving: { tts: await serving("tts"), stt: sttServing },
      reflex: {
        mode,
        compare: !!cfg.reflex_compare,
        engines: { ...avail, keywords: true },
        // the engine the next listen would fire with (null: off), and why not the chosen one
        engine: now?.engine ?? null,
        ...(now?.fallback ? { fallback: now.fallback } : {}),
        timeout_ms: REFLEX.timeoutMs,
        liveliness: isLiveliness(cfg.liveliness) ? cfg.liveliness : DEFAULT_LIVELINESS,
      },
      box: {
        paired: !!tok,
        name: tok?.agent_name ?? null,
        paired_at: tok?.created ?? null,
        connected: !!connected,
        connected_since: connected && info ? info.since : null,
        reported_name: connected && info ? info.name : null,
        version: connected && info ? info.version : null,
        voices: connected && info ? info.voices : [],
        stt: connected && info ? info.stt : false,
        tts: connected && info ? info.tts : false,
        last: stats as JsonObject,
      },
    };
  }

  // ---------------- agent events: inbox ----------------

  /**
   * Serialized so concurrent frames cannot race the sequence counter. `relay`: relay-generated, not
   * rate-limited. `to`: one agent's token hash — only that agent reads it and only its webhook gets it.
   */
  private appendEvent(type: string, data: JsonObject, relay = false, to?: string): Promise<void> {
    const run = this.evChain.then(() => this.doAppend(type, data, relay, to));
    this.evChain = run.catch(() => {});
    return run;
  }

  private async doAppend(type: string, data: JsonObject, relay: boolean, to?: string) {
    const now = this.clock.now();
    if (!relay) {
      const rate = (await this.kv.get<RateRecord>(K.evRate)) ?? { start: now, n: 0, dropped: 0 };
      if (now - rate.start >= 60000) Object.assign(rate, { start: now, n: 0 });
      if (rate.n >= INBOX_LIMITS.perMinute) {
        rate.dropped++;
        await this.kv.put(K.evRate, rate);
        return;
      }
      rate.n++;
      await this.kv.put(K.evRate, rate);
    }
    const seq = ((await this.kv.get<number>(K.evSeq)) ?? 0) + 1;
    await this.kv.put(K.evSeq, seq);
    const ev: AgentEvent = { id: eventId(seq), type, ts: now, data, ...(to ? { to } : {}) };
    await this.kv.put(K.ev(seq), ev);
    for (const [key, old] of await this.kv.list<AgentEvent>("ev:")) {
      const oldSeq = Number(key.slice(3));
      if (oldSeq <= seq - INBOX_LIMITS.keep || now - old.ts > INBOX_LIMITS.maxAgeMs) await this.kv.delete(key);
    }
    this.notify(this.inboxWaiters, "inbox");
    let queued = false;
    for (const [key, tok] of await this.kv.list<TokenRecord>("tok:")) {
      const hash = key.slice(4);
      if (!tok.webhook || tok.webhook.disabled || !webhookWants(tok.webhook.events, type) || (to && to !== hash)) continue;
      await this.kv.put<Delivery>(K.whq(hash, seq), { tok: hash, seq, attempt: 0, due: now });
      queued = true;
    }
    if (queued) await this.scheduleDeliveries();
  }

  /** Unread events after `since` (default: the agent's cursor), oldest first, at most `limit`. */
  private async readInbox(hash: string, since: number | undefined, limit: number) {
    const head = (await this.kv.get<number>(K.evSeq)) ?? 0;
    let cursor = await this.kv.get<number>(K.cur(hash));
    if (cursor === undefined) {
      cursor = head; // tokens minted before the inbox existed start at the head
      await this.kv.put(K.cur(hash), cursor);
    }
    const from = Math.min(since ?? cursor, head);
    if (from >= head) return { events: [] as AgentEvent[], next: from, more: false, cursor };
    const now = this.clock.now();
    const unread = [...(await this.kv.list<AgentEvent>("ev:")).entries()]
      .map(([key, ev]) => ({ seq: Number(key.slice(3)), ev }))
      .filter((e) => e.seq > from && now - e.ev.ts <= INBOX_LIMITS.maxAgeMs && (!e.ev.to || e.ev.to === hash));
    const page = unread.slice(0, limit);
    return { events: page.map((e) => publicEvent(e.ev)), next: page.length ? page[page.length - 1].seq : from, more: unread.length > page.length, cursor };
  }

  private async advance(hash: string, cursor: number, next: number) {
    if (next > cursor) await this.kv.put(K.cur(hash), next);
  }

  /**
   * Ride-along: attach this agent's unread events to a SUCCESSFUL outcome and
   * mark them read. Errors carry nothing and move nothing: clients often drop
   * error bodies, and a read cursor must only move when the events were
   * actually handed over. Surfaces must keep `inbox` through any reshaping.
   */
  private async withInbox<T>(hash: string, out: Outcome<T>): Promise<Outcome<T>> {
    if (!out.ok) return out;
    const r = await this.readInbox(hash, undefined, INBOX_LIMITS.piggyback);
    if (r.events.length === 0) return out;
    await this.advance(hash, r.cursor, r.next);
    return { ...out, inbox: { events: r.events, more: r.more, now: this.clock.now() } };
  }

  /** Everything that rides along on a successful authenticated outcome: unread events, then the firmware notice. */
  private async ride<T>(a: { rec: TokenRecord; hash: string }, out: Outcome<T>): Promise<Outcome<T>> {
    if (!out.ok) return out;
    return this.withFwNotice(a, await this.withInbox(a.hash, out));
  }

  /** What the relay knows about the device: the live socket's hello, else the last one stored. */
  private async facts(): Promise<DeviceFacts> {
    const meta = this.liveSocket()?.getMeta();
    if (meta) return { fw: meta.fw ?? null, board: meta.board ?? null, caps: meta.caps ?? null, online: true };
    const d = await this.kv.get<DevInfo>(K.devinfo);
    return { fw: d?.fw ?? null, board: d?.board ?? null, caps: d?.caps ?? null, online: false };
  }

  /**
   * One-time "newer firmware released" notice, same rules as the events ride-along (success
   * only, marked seen atomically on the current token record), once per token per
   * release. Never waits on GitHub: it uses whatever release answer is cached.
   */
  private async withFwNotice<T>(a: { rec: TokenRecord; hash: string }, out: Outcome<T>): Promise<Outcome<T>> {
    if (!out.ok) return out;
    const fw = firmwareBlock(await this.facts(), await this.releases.latest("cached"));
    if (!fw.update_available || !fw.latest || !fw.current || !fw.board) return out;
    const latest = fw.latest;
    if (a.rec.fw_seen && (compareVersions(a.rec.fw_seen, latest) ?? -1) >= 0) return out;
    const won = await this.kv.update<TokenRecord, boolean>(K.token(a.hash), (cur) => {
      if (!cur || (cur.fw_seen && (compareVersions(cur.fw_seen, latest) ?? -1) >= 0)) return { result: false };
      return { next: { ...cur, fw_seen: latest }, result: true };
    });
    if (!won) return out;
    const fw_notice: FirmwareNotice = { current: fw.current, latest, board: fw.board, notes_url: fw.notes_url ?? "", can_self_update: fw.can_self_update, how: howToUpdate(fw, latest) };
    return { ...out, fw_notice };
  }

  async events(token: string, req: EventsRequest): Promise<Outcome> {
    return this.logged(token, "events", { since: req.since ?? null, wait_ms: req.wait_ms }, () => this.eventsNow(token, req));
  }

  private async eventsNow(token: string, req: EventsRequest): Promise<Outcome> {
    const a = await this.agentAuth(token);
    if ("ok" in a) return a;
    if (req.since !== undefined && (!Number.isInteger(req.since) || req.since < 0)) return fail("invalid_args", "since must be an event id like e42");
    const waitMs = Math.max(0, Math.min(req.wait_ms, INBOX_LIMITS.restWaitS * 1000));
    // Admission: bounded concurrent long-polls per token and per device.
    let admitted = false;
    if (waitMs > 0) {
      const mine = this.polls.get(a.hash) ?? 0;
      let total = 0;
      for (const n of this.polls.values()) total += n;
      if (mine >= INBOX_LIMITS.pollsPerToken || total >= INBOX_LIMITS.pollsPerDevice) {
        return fail("too_many_polls", `at most ${INBOX_LIMITS.pollsPerToken} concurrent waits per agent and ${INBOX_LIMITS.pollsPerDevice} per device; wait for one to return`);
      }
      this.polls.set(a.hash, mine + 1);
      admitted = true;
    }
    try {
      const deadline = this.clock.now() + waitMs;
      for (;;) {
        const r = await this.readInbox(a.hash, req.since, INBOX_LIMITS.page);
        const remaining = deadline - this.clock.now();
        if (r.events.length > 0 || remaining <= 0 || req.signal?.aborted) {
          if (req.signal?.aborted) return fail("cancelled"); // nobody is listening: hand over nothing
          await this.advance(a.hash, r.cursor, r.next);
          return this.withFwNotice(a, { ok: true, result: { events: r.events as unknown as JsonObject[], next: eventId(r.next), more: r.more } });
        }
        await this.waitOn(this.inboxWaiters, "inbox", remaining, req.signal);
      }
    } finally {
      if (admitted) {
        const n = (this.polls.get(a.hash) ?? 1) - 1;
        if (n > 0) this.polls.set(a.hash, n);
        else this.polls.delete(a.hash);
      }
    }
  }

  // ---------------- agent events: webhooks ----------------

  async webhook(token: string, req: WebhookRequest): Promise<Outcome> {
    return this.logged(token, "webhook", req, () => this.webhookNow(token, req));
  }

  private async webhookNow(token: string, req: WebhookRequest): Promise<Outcome> {
    const a = await this.agentAuth(token);
    if ("ok" in a) return a;
    const key = K.token(a.hash);
    if (req.action === "get") {
      return this.ride(a, { ok: true, result: { webhook: a.rec.webhook ? (redactWebhook(a.rec.webhook) as JsonObject) : null } });
    }
    if (req.action === "delete") {
      const had = await this.kv.update<TokenRecord, boolean | undefined>(key, (cur) => {
        if (!cur) return { result: undefined };
        const { webhook, ...rest } = cur;
        return { next: { ...rest, wh_rev: (cur.wh_rev ?? 0) + 1 }, result: !!webhook };
      });
      if (had === undefined) return fail("unauthorized");
      await this.dropDeliveries(a.hash);
      return this.ride(a, { ok: true, result: { removed: had } });
    }
    const parsed = parseWebhookSpec(req.spec);
    if (!parsed.ok) return fail("invalid_args", parsed.message);
    // Revision this registration was based on; compared atomically when writing.
    const revision = a.rec.wh_rev ?? 0;
    // Dry run against every event type the webhook will receive, to URLs that pass the guard.
    const types = parsed.value.events ?? DEFAULT_WEBHOOK_EVENTS;
    const rendered: RenderedRequest[] = [];
    for (const type of types) {
      try {
        rendered.push(renderRequest(parsed.value, this.deviceId, SAMPLE_EVENTS[type]));
      } catch (e) {
        return fail("invalid_args", `for a ${type} event: ${e instanceof Error ? e.message : String(e)}`);
      }
    }
    // One deadline over the whole validation; each host resolved once.
    const resolve = this.resolveHost;
    const cache = new Map<string, Promise<string[]>>();
    const cached: Resolver | undefined = resolve && ((host, signal) => {
      if (!cache.has(host)) cache.set(host, resolve(host, signal));
      return cache.get(host)!;
    });
    const refused = await this.withDeadline(async (signal): Promise<GuardResult | undefined> => {
      for (const url of new Set(rendered.map((r) => r.url))) {
        const g = await guardWebhookUrl(url, cached, signal);
        if (!g.ok) return g;
      }
      return undefined;
    }).catch((): GuardResult => ({ ok: false, message: "url host did not resolve in time", category: "timeout" }));
    if (refused && !refused.ok) return fail("invalid_args", refused.message);
    // Atomic compare-and-write on the revision: a revoke (record gone), another
    // registration or a delete since `revision` was read makes this one lose.
    const secret = newWebhookSecret();
    const webhook: WebhookRecord = { ...parsed.value, secret, created: this.clock.now(), fails: 0 };
    const outcome = await this.kv.update<TokenRecord, WebhookWrite>(key, (cur) => {
      if (!cur) return { result: "gone" };
      if ((cur.wh_rev ?? 0) !== revision) return { result: "conflict" };
      return { next: { ...cur, webhook, wh_rev: revision + 1 }, result: "ok" };
    });
    if (outcome === "gone") return fail("unauthorized");
    if (outcome === "conflict") return fail("conflict", "the webhook changed while this registration was being validated; read it and retry");
    await this.dropDeliveries(a.hash);
    return this.ride(a, {
      ok: true,
      result: {
        webhook: redactWebhook(webhook) as JsonObject,
        secret,
        secret_note: "Shown once. Verify Molty-Signature: t=<unix ms>,v2=hex(hmac_sha256(secret, t + '\\n' + METHOD + '\\n' + path?query + '\\n' + body)).",
        dry_run: redactRendered(rendered[0]) as unknown as JsonObject,
      },
    });
  }

  private async dropDeliveries(hash: string) {
    for (const key of (await this.kv.list(`whq:${hash}:`)).keys()) await this.kv.delete(key);
  }

  private async scheduleDeliveries() {
    if (!this.scheduler) return;
    let next = Infinity;
    for (const d of (await this.kv.list<Delivery>("whq:")).values()) next = Math.min(next, d.due);
    // kept debug audio expires on the same alarm (core/audio-keep.ts)
    next = Math.min(next, (await this.audioKeep.nextExpiry()) ?? Infinity);
    if (next !== Infinity) await this.scheduler.setAlarm(Math.max(next, this.clock.now()));
  }

  /** Host alarm handler: delivers due webhook requests, then reschedules. */
  async alarm(): Promise<void> {
    await this.audioKeep.purge();
    const now = this.clock.now();
    const byToken = new Map<string, Array<[string, Delivery]>>();
    for (const [key, d] of await this.kv.list<Delivery>("whq:")) {
      if (d.due > now) continue;
      const list = byToken.get(d.tok) ?? [];
      if (list.length < 10) list.push([key, d]);
      byToken.set(d.tok, list);
    }
    // One agent's deliveries in order (they share a failure counter); agents in parallel.
    await Promise.all([...byToken.values()].map(async (list) => {
      for (const [key, d] of list) await this.deliver(key, d);
    }));
    await this.scheduleDeliveries();
  }

  /**
   * One delivery attempt. Webhook acks are separate from the read cursor: a
   * delivered event stays readable by pull/ride-along (the inbox is the source
   * of truth; agents dedupe by id), so an event that exhausts its retries is
   * never lost to the agent.
   */
  private async deliver(key: string, d: Delivery) {
    const tokKey = K.token(d.tok);
    const before = await this.kv.get<TokenRecord>(tokKey);
    const stored = await this.kv.get<AgentEvent>(K.ev(d.seq));
    if (!before?.webhook || before.webhook.disabled || !stored || !webhookWants(before.webhook.events, stored.type) || (stored.to && stored.to !== d.tok)) return this.kv.delete(key);
    const ev = publicEvent(stored);
    const hook = before.webhook;
    let error: DeliveryError | undefined;
    let status: number | null = null;
    const tSend = this.clock.now();
    try {
      // One deadline over DNS, signing and the request itself.
      await this.withDeadline(async (signal) => {
        let r;
        try {
          r = renderRequest(hook, this.deviceId, ev);
        } catch {
          throw new WebhookFailure("render_failed");
        }
        const guard = await guardWebhookUrl(r.url, this.resolveHost, signal);
        if (!guard.ok) throw new WebhookFailure(guard.category);
        const headers = { ...r.headers, "molty-signature": await signatureHeader(hook.secret, r.method, r.url, r.body, this.clock.now()) };
        const res = await this.fetcher(new Request(r.url, { method: r.method, headers, body: r.method === "GET" ? undefined : r.body, redirect: "manual", signal }));
        try {
          await res.body?.cancel();
        } catch {
          /* ignore */
        }
        status = res.status;
        if (res.status < 200 || res.status > 299) throw new WebhookFailure(`http_${res.status}`);
      });
    } catch (e) {
      error = categorize(e);
    }
    // Delivery stats are written atomically onto the CURRENT record, and only if
    // it still holds this webhook: never resurrect or overwrite a newer config.
    const now = this.clock.now();
    // an utterance's delivery is part of its turn's trace (core/trace.ts): status, latency, attempt
    if (ev.type === UTTERANCE_EVENT && typeof ev.data.turn_id === "string") {
      await this.activity.record({
        ts: now,
        kind: "trace",
        ev: "webhook",
        job: ev.data.turn_id,
        agent: before.agent_name,
        who: d.tok.slice(0, 16),
        ok: !error,
        data: { status, ms: now - tSend, attempt: d.attempt + 1, after_ms: now - ev.ts, ...(error ? { error } : {}) },
      });
    }
    const w = await this.kv.update<TokenRecord, WebhookRecord | undefined>(tokKey, (cur) => {
      if (!cur?.webhook || cur.webhook.secret !== hook.secret) return { result: undefined };
      const next = { ...cur.webhook };
      if (!error) {
        next.fails = 0;
        next.last_ok = now;
        delete next.last_error;
      } else {
        next.fails++;
        next.last_error = error;
        if (next.fails >= WEBHOOK_LIMITS.maxFails) next.disabled = `disabled after ${next.fails} consecutive failed deliveries; register it again to re-enable`;
      }
      return { next: { ...cur, webhook: next }, result: next };
    });
    if (!w || !error) return this.kv.delete(key); // removed/replaced meanwhile, or delivered
    this.log("webhook delivery failed", { device: this.deviceId, url: redactUrl(w.url), error, fails: w.fails });
    if (w.disabled) await this.dropDeliveries(d.tok);
    else if (d.attempt < WEBHOOK_LIMITS.retryMs.length) await this.kv.put<Delivery>(key, { ...d, attempt: d.attempt + 1, due: now + WEBHOOK_LIMITS.retryMs[d.attempt] });
    else await this.kv.delete(key);
  }

  /** Runs `fn` under one deadline: on expiry its signal aborts and the call fails as `timeout`. */
  private withDeadline<T>(fn: (signal: AbortSignal) => Promise<T>): Promise<T> {
    const ctl = new AbortController();
    return new Promise<T>((resolve, reject) => {
      const timer = this.clock.setTimeout(() => {
        ctl.abort();
        reject(new WebhookFailure("timeout"));
      }, WEBHOOK_LIMITS.timeoutMs);
      fn(ctl.signal).then(
        (v) => {
          this.clock.clearTimeout(timer);
          resolve(v);
        },
        (e: unknown) => {
          this.clock.clearTimeout(timer);
          reject(e);
        },
      );
    });
  }

  // =====================================================================
  // Quick phrases and the auto filler (core/phrases.ts, docs/AGENT-LINK.md "Quick phrases")
  // =====================================================================

  async phrases(token: string, req: unknown): Promise<Outcome> {
    return this.logged(token, "phrases", req, () => this.phrasesNow(token, req));
  }

  private async phrasesNow(token: string, req: unknown): Promise<Outcome> {
    const a = await this.agentAuth(token);
    if ("ok" in a) return a;
    let r;
    try {
      r = validatePhrasesRequest(req);
    } catch (e) {
      if (e instanceof CommandError) return fail(e.code, e.message);
      throw e;
    }
    const key = K.phrases(a.hash);
    if (r.action === "set") {
      const add = r.phrases;
      const now = this.clock.now();
      const builtins = r.builtins;
      const merged = await this.kv.update<AgentPhrases, Phrase[] | undefined>(key, (cur) => {
        const m = mergePhrases(cur?.phrases ?? [], add);
        const b = builtins ?? cur?.builtins;
        return m ? { next: { phrases: m, at: now, ...(b === false ? { builtins: false } : {}) }, result: m } : { result: undefined };
      });
      if (!merged) return fail("invalid_args", `at most ${PHRASE_LIMITS.perAgent} phrases per agent: delete some first (molty_phrases {action:"delete", id})`);
      // their audio, in Molty's current voice, now (bounded: a slow voice keeps warming after the answer)
      await Promise.race([this.warmTexts(add.map((p) => p.text)), sleep(this.clock, PHRASE_LIMITS.warmBudgetMs)]);
    } else if (r.action === "delete") {
      const ids = r.ids;
      await this.kv.update<AgentPhrases, void>(key, (cur) => {
        if (!cur) return { result: undefined };
        const left = ids[0] === "*" ? [] : cur.phrases.filter((p) => !ids.includes(p.id));
        return { next: left.length || cur.builtins === false ? { ...cur, phrases: left } : null, result: undefined };
      });
    }
    return this.ride(a, { ok: true, result: await this.phraseView(a.hash) });
  }

  /** The list an agent sees: its own phrases, the built-ins, whether each is ready (cached) in Molty's current voice. */
  private async phraseView(hash: string): Promise<JsonObject> {
    const rec = await this.kv.get<AgentPhrases>(K.phrases(hash));
    const mine = rec?.phrases ?? [];
    const useBuiltins = rec?.builtins !== false;
    const v = await this.voiceNow();
    const ready = async (text: string) => (v.ok ? this.phraseCache.has(v.vkey, text) : false);
    const own: JsonObject[] = [];
    for (const p of mine) own.push({ id: p.id, text: p.text, ...(p.filler ? { filler: true } : {}), for: p.for ?? "any", ready: await ready(p.text) });
    const builtins: JsonObject[] = [];
    // builtins off: this agent neither uses nor sees them
    if (useBuiltins) for (const p of BUILTIN_PHRASES) builtins.push({ id: p.id, text: p.text, for: p.for ?? "any", ready: await ready(p.text), ...(mine.some((m) => m.id === p.id) ? { shadowed: true } : {}) });
    return { phrases: own, builtins, builtins_enabled: useBuiltins, limits: { per_agent: PHRASE_LIMITS.perAgent, text_max: PHRASE_LIMITS.textMax }, voice: v.ok ? v.pick.name : v.error };
  }

  /** An agent's phrase by id: its own first (it may shadow a built-in), else the built-in (unless it turned them off). */
  private async findPhrase(hash: string, id: string): Promise<Phrase | undefined> {
    const rec = await this.kv.get<AgentPhrases>(K.phrases(hash));
    return rec?.phrases.find((p) => p.id === id) ?? (rec?.builtins === false ? undefined : builtin(id));
  }

  /**
   * Molty's filler pool now: the fillers (`filler: true`) of the agent that last answered a human
   * turn here; that agent has none → the built-ins, unless it turned them off (then no filler).
   */
  private async fillerPool(): Promise<{ pool: readonly Phrase[]; source: "agent" | "builtin"; owner?: string } | undefined> {
    const hash = await this.kv.get<string>(K.lastAnswerer);
    const rec = typeof hash === "string" ? await this.kv.get<AgentPhrases>(K.phrases(hash)) : undefined;
    const own = rec?.phrases.filter((p) => p.filler) ?? [];
    if (own.length) {
      const tok = await this.kv.get<TokenRecord>(K.token(hash!));
      return { pool: own, source: "agent", ...(tok ? { owner: tok.agent_name } : {}) };
    }
    if (rec?.builtins === false) return undefined;
    return { pool: BUILTIN_PHRASES, source: "builtin" };
  }

  /**
   * `quick {id, in_reply_to?, force?}`: a lead-in from the phrase cache (prepareSpeech "cache"),
   * answered at once ({job}). Claims the human's turn like a say. Right after Molty's own filler for
   * the same turn (and nothing from this agent since) it is skipped — `{skipped:"filled", filler}` —
   * so the human never hears two fillers in a row; `force` plays it anyway, queued behind the
   * filler. Its own follow-up say / talk queues behind it (startJob `after` / `lead`).
   */
  private async quick(hash: string, by: Who | undefined, args: JsonObject, inReplyTo?: string): Promise<Outcome> {
    const id = args.id as string;
    const phrase = await this.findPhrase(hash, id);
    if (!phrase) return fail("not_found", `no phrase "${id}": register it (molty_phrases set) or use a built-in: ${BUILTIN_PHRASES.map((p) => p.id).join(", ")}`);
    const pre = await this.claimCheck(hash, inReplyTo);
    if (!pre.ok) return pre;
    const lf = this.lastFill;
    const now = this.clock.now();
    if (args.force !== true && lf && lf.seq === this.agentSeq && now - lf.at < FILL_SKIP_MS && (inReplyTo === undefined || inReplyTo === lf.turn)) {
      return { ok: true, result: { skipped: "filled", phrase: phrase.id, filler: { phrase: lf.phrase, text: lf.text, ms_ago: now - lf.at }, hint: "Molty already said a filler for this turn: go straight to your answer (talk)" } };
    }
    const v = await this.voiceNow();
    if (!v.ok) return fail(v.error, v.message);
    const cached = await this.phraseCache.has(v.vkey, phrase.text);
    const jobArgs: JsonObject = { id: phrase.id, text: phrase.text, ...(phrase.marks ? { marks: phrase.marks } : {}) };
    const after = this.followUpOf(hash);
    const started = await this.startJob("quick", jobArgs, true, by, () => this.claimFor(hash, inReplyTo, now), { lead: hash, ...(after ? { after } : {}) });
    if (!started.ok) return started;
    if (after) this.noteFollow(after, started.result.job as string);
    return { ok: true, result: { job: started.result.job as string, phrase: phrase.id, text: phrase.text, cached } };
  }

  /** A quick phrase job (an agent's, or Molty's filler): the cached audio streamed as a say; thinking after it unless something follows. */
  private async runQuick(id: string, args: JsonObject, created: number, ready?: Promise<void>) {
    const text = args.text as string;
    const filler = args.filler === true;
    const pre = await this.prepareSpeech(text, undefined, "cache");
    const t0 = this.clock.now();
    if (ready) await ready;
    const waited = this.clock.now() - t0;
    await this.setJob(id, { status: "running" });
    const rev = this.screenRev;
    const r = "pick" in pre ? await this.speak(id, text, undefined, true, args.marks as SayMarks | undefined, pre) : pre;
    // Nothing queued behind it: back to thinking (the agent is still working on the answer) — sent
    // while this job still holds the speaker, so it never lands after the say that comes next.
    const followed = this.handoff?.after === id;
    const toWork = (args.marks as SayMarks | undefined)?.some((m) => m[1] === WORKING_MOOD);
    if (r.ok && !followed && !toWork && !(filler && args.ack === "off") && rev === this.screenRev && this.screenInFlight === 0 && this.pendingAsks.size === 0) {
      void this.sendCmd("mood", { mood: "thinking", hold_s: this.t.thinkHoldS }).then((m) => {
        if (!m.ok) this.log("auto mood failed", { device: this.deviceId, mood: "thinking", error: m.error });
      });
    }
    this.releaseAudio(id); // the queued answer's say goes out now, not after the bookkeeping below
    if (!r.ok) return this.finishJob(id, { status: "failed", error: r.error });
    const cached = "pick" in pre && !pre.fresh;
    const timings: JsonObject = { queue: r.start - created, ...(waited ? { after_lead: waited } : {}), ...r.timings, total: this.clock.now() - created };
    if (isRecord(args.turn)) timings.turn = this.answerTimings(args, r.start + (r.timings.first_audio as number));
    this.logTimings(id, "quick", timings);
    const result: JsonObject = { phrase: args.id as string, text, ms: r.ms, cached, timings };
    if (filler) Object.assign(result, { filler: true, turn: args.for_turn as string, category: args.category as string, source: args.source as string, ...(typeof args.owner === "string" ? { owner: args.owner } : {}) });
    await this.finishJob(id, { status: "done", result });
    // the turn trace: an agent's quick answering a turn (ev "reply", op quick), Molty's own filler (ev "filler")
    if (filler && typeof args.for_turn === "string") {
      const data: JsonObject = {
        job: id,
        phrase: args.id as string,
        text,
        first_audio_ms: r.start + ((r.timings.first_audio as number) ?? 0) - (args.end_at as number),
        // why this one: the utterance's category (and who classified it), whose pool it came from
        category: args.category as string,
        classifier: args.classifier as string,
        source: args.source as string,
        ...(typeof args.owner === "string" ? { owner: args.owner } : {}),
      };
      await this.activity.record({ ts: this.clock.now(), kind: "trace", ev: "filler", job: args.for_turn, data });
    } else await this.replyTrace(id, "quick", args, timings);
  }

  /** Who speaks now and in which voice: the phrase cache's key. */
  private async voiceNow(): Promise<{ ok: true; pick: Extract<VoicePick, { ok: true }>; vkey: string; ttsVoice?: string } | { ok: false; error: string; message?: string }> {
    const p = await this.pickVoice("tts");
    if (!p.ok) return p;
    const ttsVoice = p.box ? p.cfg.voice : undefined;
    return { ok: true, pick: p, vkey: `${p.name}|${ttsVoice ?? ""}`, ...(ttsVoice !== undefined ? { ttsVoice } : {}) };
  }

  /** One synthesis per voice + text at a time (warming and a play share it); the result is cached. */
  private synthPhrase(v: { pick: Extract<VoicePick, { ok: true }>; vkey: string; ttsVoice?: string }, text: string): Promise<PhraseAudio | undefined> {
    const key = `${v.vkey}\n${text}`;
    let p = this.synthing.get(key);
    if (!p) {
      p = this.doSynth(v, text).finally(() => this.synthing.delete(key));
      this.synthing.set(key, p);
    }
    return p;
  }

  private async doSynth(v: { pick: Extract<VoicePick, { ok: true }>; vkey: string; ttsVoice?: string }, text: string): Promise<PhraseAudio | undefined> {
    const feed = new PcmFeed();
    await synthesize(v.pick.provider, splitSpeech(text, { streaming: v.pick.box }), { voice: v.ttsVoice }, feed, this.clock, this.t.ttsTimeoutMs);
    if (feed.error !== undefined) {
      this.log("phrase synthesis failed", { device: this.deviceId, error: errorCode(feed.error) });
      return undefined;
    }
    let pcm: Int16Array = new Int16Array(0);
    for (const p of feed.take()) pcm = concat(pcm, p);
    if (pcm.length === 0) return undefined;
    const audio: PhraseAudio = { text, pcm, cues: feed.takeCues(), voice: v.pick.name };
    try {
      await this.phraseCache.put(v.vkey, audio);
    } catch (e) {
      this.log("phrase cache write failed", { device: this.deviceId, error: errorCode(e) });
    }
    return audio;
  }

  /** Synthesises (and caches) the texts not cached yet in the current voice, a few at a time. */
  private async warmTexts(texts: string[]): Promise<void> {
    const v = await this.voiceNow();
    if (!v.ok) return;
    const todo: string[] = [];
    for (const t of new Set(texts)) if (!(await this.phraseCache.has(v.vkey, t))) todo.push(t);
    const work = todo.slice(0, PHRASE_LIMITS.cacheEntries);
    const worker = async () => {
      for (let t = work.shift(); t !== undefined; t = work.shift()) await this.synthPhrase(v, t);
    };
    await Promise.all(Array.from({ length: Math.min(PHRASE_LIMITS.warmConcurrency, work.length) }, worker));
  }

  /** The filler pool's audio, warmed during the listen (only what is not cached yet: once per voice). */
  private async warmFillers(): Promise<void> {
    const p = await this.fillerPool();
    if (p) await this.warmTexts(p.pool.map((x) => x.text));
  }

  /**
   * The utterance's category for a context-picked filler — ONE path: Molty's reflex
   * (fillerCategory: the reflex's decision on those words, or one call of the owner's engine,
   * keywords when it is off), or RoomDeps.classifier when given. CLASSIFY_BUDGET_MS at most; a throw,
   * an unknown category or the deadline → the keywords. `by`: the engine that decided.
   */
  private async classify(text: string): Promise<{ category: UtteranceCategory; by: string }> {
    const valid = (c: unknown): c is UtteranceCategory => (UTTERANCE_CATEGORIES as readonly unknown[]).includes(c);
    try {
      if (this.classifier) {
        const c = await withTimeout(this.clock, Promise.resolve(this.classifier.classify(text)), CLASSIFY_BUDGET_MS, "classify_timeout");
        if (valid(c)) return { category: c, by: this.classifier.name };
      } else {
        const r = await withTimeout(this.clock, this.fillerCategory(text), CLASSIFY_BUDGET_MS, "classify_timeout");
        if (valid(r.category)) return { category: r.category, by: r.engine };
      }
    } catch (e) {
      this.log("filler classifier failed", { device: this.deviceId, classifier: this.classifier?.name ?? "reflex", error: errorCode(e) });
    }
    return { category: keywordCategory(text), by: "keywords" };
  }

  /** The voice changed (owner setting, the box came or went): re-synthesise what was in use, and every agent's phrases. */
  private async rewarmPhrases(): Promise<void> {
    try {
      const texts = await this.phraseCache.texts();
      for (const rec of (await this.kv.list<AgentPhrases>("phr:")).values()) for (const p of rec.phrases) texts.push(p.text);
      if (texts.length) await this.warmTexts(texts);
    } catch (e) {
      this.log("phrase rewarm failed", { device: this.deviceId, error: errorCode(e) });
    }
  }

  /** ms from now until Molty's own filler for this heard turn (absent: off, or nothing was heard). */
  private fillerIn(r: Extract<ListenResult, { ok: true }>): number | undefined {
    if (!r.fillerMs || !r.text.trim()) return undefined;
    return Math.max(0, r.endAt + r.fillerMs - this.clock.now());
  }

  /**
   * A human turn ended with words (`turn`: the listen / talk job): unless an agent does anything
   * (a job — a draft's opener too —, a mood, text, a gesture) after `seq` within the owner's delay
   * counted from the end of speech, Molty says a filler itself (fireFiller).
   */
  private armFiller(turn: string, r: Extract<ListenResult, { ok: true }>, seq: number) {
    this.cancelFiller();
    const delay = this.fillerIn(r);
    if (delay === undefined) return;
    const caps = this.liveSocket()?.getMeta()?.caps;
    if (caps && !caps.includes("say")) return;
    // the classifier and the pool lookup run NOW, in parallel with the delay: neither adds to it
    const w: FillWatch = { turn, seq, timer: undefined, endAt: r.endAt, text: r.text, category: this.classify(r.text), pool: this.fillerPool(), ...(r.ack ? { ack: r.ack } : {}) };
    void w.category.then((c) => (w.settled = c));
    w.pool.catch(() => undefined); // cancelled before its moment: nobody awaits it
    w.timer = this.clock.setTimeout(() => {
      if (this.fillWatch !== w) return;
      this.fillWatch = undefined;
      void this.fireFiller(w);
    }, delay);
    this.fillWatch = w;
  }

  private cancelFiller() {
    if (this.fillWatch) this.clock.clearTimeout(this.fillWatch.timer);
    this.fillWatch = undefined;
  }

  /** The filler's moment: only when no agent did anything since the turn and nothing owns the speaker, the screen or the floor. */
  private async fireFiller(w: FillWatch): Promise<void> {
    const skip = (why: string) => this.log("filler skipped", { device: this.deviceId, turn: w.turn, why });
    // everything that waits first; the checks and the start below run with no await in between.
    // The classifier had until now (it ran in parallel with the delay): not in yet → the keywords,
    // never a wait past the owner's delay.
    const { category, by: classifier } = w.settled ?? { category: keywordCategory(w.text), by: "keywords" };
    const src = await w.pool.catch(() => undefined); // a failed lookup: no filler, never a stray rejection
    if (!src) return skip("no_fillers"); // the answering agent turned the built-ins off and has none of its own
    const p = pickFiller(src.pool, category, this.lastFiller);
    if (!p) return skip("no_fit"); // nothing in the pool fits this kind of turn
    if (this.agentSeq !== w.seq) return skip("agent");
    if (this.audioJob || this.openTurn || this.pendingAsks.size || this.screenInFlight || !this.liveSocket()) return skip("busy");
    const id = p.id;
    const seq = this.agentSeq;
    // relay-owned lead-in: no agent, claims no turn; anyone's say / talk / quick queues right after it
    const jobArgs: JsonObject = {
      id: p.id,
      text: p.text,
      ...(p.marks ? { marks: p.marks } : {}),
      filler: true,
      for_turn: w.turn,
      end_at: w.endAt,
      category,
      classifier,
      source: src.source,
      ...(src.owner ? { owner: src.owner } : {}),
      ...(w.ack ? { ack: w.ack } : {}),
    };
    const started = await this.startJob("quick", jobArgs, true, undefined, undefined, { lead: RELAY_LEAD });
    if (!started.ok) return skip(started.error);
    this.lastFiller = id;
    this.lastFill = { turn: w.turn, job: started.result.job as string, phrase: id, text: p.text, at: this.clock.now(), seq };
  }

  // ---------------- waiter plumbing ----------------

  private waitOn(map: Map<string, Set<() => void>>, key: string, ms: number, signal?: AbortSignal): Promise<void> {
    return new Promise((resolve) => {
      let set = map.get(key);
      if (!set) map.set(key, (set = new Set()));
      const wake = () => {
        this.clock.clearTimeout(timer);
        signal?.removeEventListener("abort", wake);
        set!.delete(wake);
        if (set!.size === 0) map.delete(key);
        resolve();
      };
      const timer = this.clock.setTimeout(wake, ms);
      set.add(wake);
      // A caller that went away frees its waiter now, not at the deadline.
      signal?.addEventListener("abort", wake, { once: true });
    });
  }

  private notify(map: Map<string, Set<() => void>>, key: string) {
    for (const wake of [...(map.get(key) ?? [])]) wake();
  }
}

function viewOf(rec: JobRecord): JobView {
  const view: JobView = { job: rec.job, kind: rec.kind, status: rec.status };
  if (rec.result) view.result = rec.result;
  if (rec.error) view.error = rec.error;
  if (typeof rec.phase === "string") view.phase = rec.phase;
  if (typeof rec.detail === "string") view.detail = rec.detail;
  return view;
}

class CodedError extends Error {
  constructor(readonly code: string) {
    super(code);
  }
}

function errorCode(e: unknown): string {
  if (e instanceof CodedError) return e.code;
  const msg = e instanceof Error ? e.message : String(e);
  return msg.slice(0, 200) || "internal";
}

function withTimeout<T>(clock: Clock, p: Promise<T>, ms: number, code: string): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    const timer = clock.setTimeout(() => reject(new CodedError(code)), ms);
    p.then(
      (v) => {
        clock.clearTimeout(timer);
        resolve(v);
      },
      (e) => {
        clock.clearTimeout(timer);
        reject(e);
      },
    );
  });
}
