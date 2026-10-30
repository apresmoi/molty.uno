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
// Skill versions (core/version.ts): each TokenRecord holds `skill_seen`; the
// first successful authenticated outcome for an agent behind SKILL_VERSION
// carries a one-time `notice` (written atomically onto the token, never
// resurrecting a revoked one). Once per room and version, a `skill.updated`
// event is appended to the inbox when an older agent exists (`skillev`).
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
import { ACK_CAP, ACK_MODES, type AskArgs, BOX_NO_SPEECH_MS, CommandError, DEVICE_NO_SPEECH_MS, LEGACY_MAX_LISTEN_MS, LISTEN_CEILING_MS, LISTEN_WINDOW_CAP, TALK_BUDGET_MS, THINK_HOLD_S, type TalkArgs, validateAsk, validateCommand, validateTalk } from "./commands.js";
import { PcmFeed, estimateSpeechMs, splitSpeech, synthesize } from "./speech.js";
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
  decodeAudioFrame,
  encodeAudioFrame,
  isRecord,
  parseHello,
  parseTextFrame,
  type JsonObject,
} from "./protocol.js";
import { type Clock, type KV, sleep } from "./store.js";
import { AGENT_EVENT_TYPES, type AgentEvent, INBOX_LIMITS, SAMPLE_EVENTS, SKILL_EVENT, UTTERANCE_EVENT, agentEventData, eventId } from "./events.js";
import { LISTEN_ADOPT_FEATURE, LOCAL_LISTEN_RE, type TurnClaim, type TurnRecord, claimTurn, turnTimings, utteranceData } from "./turns.js";
import { FEATURES, SKILL_PATH, SKILL_VERSION, type SkillNotice, WHATS_NEW, compareVersions, skillNotice } from "./version.js";
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
  box?: { stt: boolean; tts: boolean };
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
  /** ask: after the device's own timeout_ms, how long the relay waits for its `answer` before resolving `timeout` itself. */
  askGraceMs: number;
  /** update: how long the on-screen "update?" waits for a tap (sent as timeout_ms); the relay adds askGraceMs. */
  updateConfirmMs: number;
  /** update: from the human's accept until the device is back on the new firmware. */
  updateMs: number;
  /** voice box: after the listen ended, how long its streaming STT may take for the final text. */
  boxSttFinalMs: number;
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
  askGraceMs: 5000,
  updateConfirmMs: 60000,
  updateMs: 5 * 60 * 1000,
  boxSttFinalMs: 5000,
};

/** Firmware cap: say {cues:true} and frames with "w" word cues (the text follows the voice). */
const SAY_CUES_CAP = "say_cues";
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
  /** Last skill version this agent was told about (absent = paired before versions: older than any). */
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
}

/** Owner voice settings (KV `voicecfg`). */
interface VoiceCfg {
  mode: VoiceMode;
  /** Default voice for the box when the agent gives none. */
  voice?: string;
}
/** The connected box's voice.hello (KV `vbox`). */
interface BoxInfo {
  name: string;
  version: string;
  voices: string[];
  stt: boolean;
  tts: boolean;
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

type JobKind = "say" | "listen" | "talk" | "ask" | "update";

/** How an ask ended: a tap (index + label) or no answer (reason). */
type AskAnswer = { index: number; label: string; reason?: undefined } | { index: null; label: null; reason: "timeout" | "cancelled" | "offline" };
interface PendingAsk {
  options: string[];
  resolve(a: AskAnswer): void;
}
type SpeakResult = { ok: true; start: number; ms: number; timings: JsonObject } | { ok: false; error: string };
type ListenResult =
  | { ok: true; start: number; text: string; reason: string; speech_ms?: number; duration_ms: number; peak: number; rms: number; timings: JsonObject; ack?: string }
  | { ok: false; error: string };

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
  /** Highest skill version already announced as a `skill.updated` inbox event. */
  skillEv: "skillev",
  /** Public origin of this relay (from the device upgrade request), for absolute URLs in events. */
  origin: "origin",
  devinfo: "devinfo",
  upd: "upd",
  voiceCfg: "voicecfg",
  vbox: "vbox",
  vstat: "vstat",
  /** The latest human turn (shake to talk, core/turns.ts). */
  turn: "turn",
};

/** Device-facing name of a voice box pairing (the console shows it next to the code). */
const voiceLabel = (name: string) => (/^voice box\b/i.test(name) ? name : `voice box · ${name}`);



const fail = (error: string, message?: string, details?: JsonObject): { ok: false; error: string; message?: string; details?: JsonObject } => ({
  ok: false,
  error,
  ...(message === undefined ? {} : { message }),
  ...(details ? { details } : {}),
});

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
  /** Cache of "skillev >= SKILL_VERSION" (KV stays authoritative). */
  private skillAnnounced = false;
  private cmdSeq = 0;
  private updTimer: unknown;

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
    this.box = new RemoteVoiceProvider(() => this.boxSocket(), this.clock);
    this.activity = new ActivityLog({ deviceId: this.deviceId, kv: this.kv, clock: this.clock, log: this.log });
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
    if (sock.getMeta()?.stage === "voice") return this.box.drop(sock);
    if (sock.getMeta()?.stage !== "live") return;
    // The runtime may still list the closing socket; anything else live is a replacement.
    if (this.liveSocket(sock)) return;
    this.dropInFlight("device_offline");
  }

  private liveSocket(except?: DeviceSocket): DeviceSocket | undefined {
    return this.host.sockets().find((s) => (!except || !s.same(except)) && s.getMeta()?.stage === "live");
  }

  private dropInFlight(error: string) {
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
    // After a deploy the device reconnects: tell push-mode agents the skill changed.
    await this.announceSkill();
  }

  /** The host records the public origin it was reached on (device upgrade request). */
  async noteOrigin(origin: string): Promise<void> {
    if (!/^https?:\/\/[A-Za-z0-9.:[\]-]{1,253}$/.test(origin)) return;
    if ((await this.kv.get<string>(K.origin)) !== origin) await this.kv.put(K.origin, origin);
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
    for (const prefix of ["tok:", "pair:", "job:", "ev:", "cur:", "whq:", ACTIVITY_PREFIX]) {
      for (const key of (await this.kv.list(prefix)).keys()) await this.kv.delete(key);
    }
    for (const key of [K.device, K.link, K.pairCurrent, K.evSeq, K.evRate, K.skillEv, K.origin, K.devinfo, K.upd, K.voiceCfg, K.vbox, K.vstat, K.turn]) await this.kv.delete(key);
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
    await this.dropDeliveries(a.hash);
    if (a.rec.role === "voice") await this.forgetBox((m) => m.tok === a.hash, "voice box token revoked");
    return { ok: true, result: { revoked: true } };
  }

  async command(token: string, name: string, args: unknown): Promise<Outcome> {
    return this.logged(token, typeof name === "string" ? name.slice(0, 32) : "?", args, () => this.commandNow(token, name, args));
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
    const live = this.liveSocket();
    if (!live) return fail("device_offline");
    if (cmd.job) {
      // A device that lists its caps and leaves say/listen out has no speaker/mic
      // (firmware/boards/): refuse before spending a TTS call. No caps = older firmware.
      const caps = live.getMeta()?.caps;
      if (caps && !caps.includes(cmd.name)) {
        return fail("unsupported", `this device has no ${cmd.name === "say" ? "speaker" : "microphone"}`);
      }
      if (cmd.name === "listen") {
        const w = this.listenWindow(cmd.args.ms as number | undefined);
        if (!w.ok) return w;
      }
      if (cmd.name === "say") {
        // a human turn is open: the first answer claims it, other agents get turn_claimed (not audio_busy)
        const c = await this.claim(hash, cmd.in_reply_to);
        if (!c.ok) return c;
        if (c.result.turn) return this.startJob("say", { ...cmd.args, turn: c.result.turn as JsonObject, called: this.clock.now() }, true, by);
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
        const result: PairResult = { status: "approved", agent_token: rec.token, device_id: this.deviceId, agent_name: rec.agent_name, code: rec.code };
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
        // A new agent just learned the current skill (skill.md / MCP instructions).
        await this.kv.put<TokenRecord>(K.token(hash), { agent_name: rec.agent_name, created: this.clock.now(), skill_seen: SKILL_VERSION });
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
    const out = await this.runJob(jobId, waitMs);
    // A failed job is an error to every surface: nothing rides along on it.
    if (out.ok && out.result.status === "failed") return out;
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
    const live = this.liveSocket();
    if (!live) return fail("device_offline");
    const caps = live.getMeta()?.caps;
    if (caps && args.say !== undefined && !caps.includes("say")) return fail("unsupported", "this device has no speaker");
    if (caps && args.listen && !caps.includes("listen")) return fail("unsupported", "this device has no microphone");
    if (args.listen) {
      const w = this.listenWindow(args.listen_ms);
      if (!w.ok) return w;
    }
    // a human turn is open: the first answer claims it, other agents get turn_claimed (not audio_busy)
    const c = await this.claim(a.hash, args.in_reply_to);
    if (!c.ok) return c;
    const turn: JsonObject = c.result.turn ? { turn: c.result.turn, called: t0 } : {};
    const started = await this.startJob("talk", { ...(args as unknown as JsonObject), ...turn }, true, this.who(a));
    if (!started.ok) return started;
    const job = started.result.job as string;
    const out = await this.runJob(job, Math.max(0, this.t.talkBudgetMs - (this.clock.now() - t0)));
    if (!out.ok) return out;
    const v = out.result;
    if (v.status === "failed") return fail(v.error ?? "failed", `talk job ${job} failed`, { job });
    if (v.status === "done") return this.ride(a, { ok: true, result: { ...(v.result ?? {}), job, status: "done" } });
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
    // Only the spoken part needs the speaker; waiting for the tap holds no audio lock.
    const started = await this.startJob("ask", args as unknown as JsonObject, args.say !== undefined, this.who(a));
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
    // Reporting the skill counts as telling it: mark it seen (atomically, never resurrecting a revoked token).
    const yours = await this.kv.update<TokenRecord, number | undefined>(K.token(a.hash), (cur) =>
      cur ? { next: { ...cur, skill_seen: SKILL_VERSION }, result: cur.skill_seen ?? 1 } : { result: undefined },
    );
    if (yours === undefined) return fail("unauthorized");
    const origin = (await this.kv.get<string>(K.origin)) ?? "";
    const skill = {
      yours,
      latest: SKILL_VERSION,
      whats_new: WHATS_NEW.filter((e) => e.version > yours).map((e) => ({ ...e })),
      url: `${origin}${SKILL_PATH}`,
    };
    const fw = firmwareBlock(await this.facts(), await this.releases.latest("wait"));
    // Likewise the firmware release it reports.
    if (fw.update_available && fw.latest) {
      const latest = fw.latest;
      await this.kv.update<TokenRecord, void>(K.token(a.hash), (cur) =>
        cur && !(cur.fw_seen && (compareVersions(cur.fw_seen, latest) ?? -1) >= 0) ? { next: { ...cur, fw_seen: latest }, result: undefined } : { result: undefined },
      );
    }
    const result = { skill, firmware: fw, actions: updateActions(skill, fw) };
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
    if (!u || this.clock.now() < u.deadline) return;
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
  private async startJob(kind: JobKind, args: JsonObject, audio = true, by?: Who): Promise<Outcome> {
    if (audio && this.audioJob) return fail("audio_busy", `job ${this.audioJob} is still running`);
    const id = `${this.deviceId}.j_${b64urlEncode(randomBytes(12))}`;
    const now = this.clock.now();
    if (audio) this.audioJob = id;
    this.tasks.add(id);
    await this.kv.put<JobRecord>(K.job(id), { job: id, kind, status: "queued", created: now, updated: now, ...(by ? { by } : {}) });
    void this.purgeJobs(now);
    const run =
      kind === "say"
        ? this.runSay(id, args, now)
        : kind === "listen"
          ? this.runListen(id, args, now)
          : kind === "ask"
            ? this.runAsk(id, args as unknown as AskArgs, now)
            : kind === "update"
              ? this.runUpdate(id, args)
              : this.runTalk(id, args as unknown as TalkArgs, now);
    run
      .catch((e: unknown) => this.finishJob(id, { status: "failed", error: errorCode(e) }))
      .finally(() => {
        this.tasks.delete(id);
        if (this.audioJob === id) this.audioJob = undefined;
      });
    return { ok: true, result: { job: id } };
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
    const ended = await this.setJob(id, patch);
    if (ended) await this.activity.record(jobDraft(ended, ended.by));
    this.tasks.delete(id);
    if (this.audioJob === id) this.audioJob = undefined;
    this.notify(this.jobWaiters, id);
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

  private async runSay(id: string, args: JsonObject, created: number) {
    await this.setJob(id, { status: "running" });
    const text = args.text as string;
    const r = await this.speak(id, text, typeof args.voice === "string" ? args.voice : undefined, false);
    if (!r.ok) return this.finishJob(id, { status: "failed", error: r.error });
    const timings: JsonObject = { queue: r.start - created, ...r.timings, total: this.clock.now() - created };
    if (isRecord(args.turn)) timings.turn = this.answerTimings(args, r.start + (r.timings.first_audio as number));
    this.logTimings(id, "say", timings);
    await this.finishJob(id, { status: "done", result: { ms: r.ms, text, timings } });
  }

  private async runListen(id: string, args: JsonObject, created: number) {
    await this.setJob(id, { status: "running" });
    const { ms, trigger, ...listenArgs } = args;
    const r = await this.record(id, ms as number | undefined, listenArgs);
    if (!r.ok) return this.finishJob(id, { status: "failed", error: r.error });
    if (trigger === "shake") await this.emitUtterance(id, r);
    const timings: JsonObject = { queue: r.start - created, ...r.timings, total: this.clock.now() - created };
    this.logTimings(id, "listen", timings);
    const extra: JsonObject = r.speech_ms !== undefined ? { speech_ms: r.speech_ms } : {};
    await this.finishJob(id, { status: "done", result: { text: r.text, duration_ms: r.duration_ms, peak: r.peak, rms: r.rms, reason: r.reason, ...extra, timings } });
  }

  /** say (optional) then listen, one job holding the audio lock across both, so nothing slips in between. */
  private async runTalk(id: string, args: TalkArgs, created: number) {
    await this.setJob(id, { status: "running" });
    const reply = args as unknown as JsonObject; // talk() adds {turn, called} when it answers a human turn
    const ms: JsonObject = {};
    const timings: JsonObject = { queue: this.clock.now() - created };
    const result: JsonObject = {};
    if (args.say !== undefined) {
      // The device's say.done is required before the mic opens (it must not record its own
      // voice); none within playback + sayDoneGraceMs fails the turn with playback_timeout.
      const s = await this.speak(id, args.say, args.voice, true);
      if (!s.ok) return this.finishJob(id, { status: "failed", error: s.error });
      result.said = args.say;
      ms.tts_first_audio = (s.timings.first_audio as number) ?? 0;
      ms.speech = (s.timings.play as number) ?? 0;
      timings.say = s.timings;
      if (reply.turn) timings.turn = this.answerTimings(reply, s.start + (ms.tts_first_audio as number));
    } else if (reply.turn) timings.turn = this.answerTimings(reply);
    if (args.listen) {
      const l = await this.record(id, args.listen_ms, { ...args.vad, ...(args.ring_color !== undefined ? { ring_color: args.ring_color } : {}), ...(args.ack !== undefined ? { ack: args.ack } : {}) });
      if (!l.ok) return this.finishJob(id, { status: "failed", error: l.error });
      Object.assign(result, { heard: l.text, reason: l.reason, duration_ms: l.duration_ms, peak: l.peak, rms: l.rms });
      if (l.speech_ms !== undefined) result.speech_ms = l.speech_ms;
      ms.listen = (l.timings.record as number) ?? 0;
      ms.stt = (l.timings.stt as number) ?? 0;
      timings.listen = l.timings;
    } else {
      result.reason = "spoken";
    }
    ms.total = this.clock.now() - created;
    timings.total = ms.total;
    this.logTimings(id, "talk", timings);
    await this.finishJob(id, { status: "done", result: { ...result, ms, timings } });
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
    if (args.say !== undefined) {
      const s = await this.speak(id, args.say, args.voice, true);
      if (this.audioJob === id) this.audioJob = undefined;
      if (!s.ok) return this.finishJob(id, { status: "failed", error: s.error });
      result.said = args.say;
      ms.tts_first_audio = (s.timings.first_audio as number) ?? 0;
      ms.speech = (s.timings.play as number) ?? 0;
      timings.say = s.timings;
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
  private async speak(id: string, text: string, voice: string | undefined, untilDone: boolean): Promise<SpeakResult> {
    const t0 = this.clock.now();
    const pick = await this.pickVoice("tts");
    if (!pick.ok) return { ok: false, error: pick.error };
    const feed = new PcmFeed();
    // The agent's voice passes through; with none, the box uses the owner's default box voice.
    const ttsVoice = voice ?? (pick.box ? pick.cfg.voice : undefined);
    // The box streams: its first audio does not wait on the text's length, so sentences are not cut
    // apart for it (every cut is a seam of trailing + leading silence and a prosody restart).
    void synthesize(pick.provider, splitSpeech(text, { streaming: pick.box }), { voice: ttsVoice }, feed, this.clock, this.t.ttsTimeoutMs);
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
    const early = pick.box;
    let sock: DeviceSocket | undefined;
    let cuesOn = false;
    const sayCmd = (ms: number) => {
      const deviceArgs: JsonObject = { job: id, text, ms, rate: SAMPLE_RATE };
      if (voice) deviceArgs.voice = voice;
      // Firmware with say_cues shows each word when the speaker reaches it: the frames carry
      // word cues [word, sample] on the stream's timeline (the box's own timing, else estimated).
      cuesOn = !!sock!.getMeta()?.caps?.includes(SAY_CUES_CAP);
      if (cuesOn) deviceArgs.cues = true;
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
      if (pick.box) await this.noteBoxStats({ tts_first_audio_ms: tFirst - t0, tts_at: tEnd });
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
        },
      };
    } catch (e) {
      return { ok: false, error: errorCode(e) };
    } finally {
      feed.end(); // stops synthesis nobody will play
    }
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
    const t0 = this.clock.now();
    let pick = await this.pickVoice("stt");
    if (!pick.ok) return { ok: false, error: pick.error };
    const buf: ListenBuffer = { job: id, chunks: [], bytes: 0, ended: false, screenRev: this.screenRev };
    let fallback: string | undefined;
    // Semantic end of turn by default: when the voice box serves this listen and the firmware
    // can be stopped by it (listen_stop), the box's end of turn is THE end — the device's
    // silence VAD is turned off (it heard Molty's own tail and cut humans off), and the box
    // also calls no_speech. Not when the agent tuned the device VAD itself (vad /
    // end_silence_ms), nor on the cloud path or older firmware: those keep the device VAD.
    const semantic = pick.box && extra.vad === undefined && extra.end_silence_ms === undefined && !!this.liveSocket()?.getMeta()?.caps?.includes("listen_stop");
    // Streaming STT (voice box): opened before the mic, so every chunk goes out the moment it arrives.
    if (pick.provider.sttStream) {
      try {
        buf.stream = pick.provider.sttStream({
          onTurnEnd: (reason) => this.onTurnEnd(buf, reason),
          onFail: () => this.onBoxFail(buf),
          ...(semantic ? { noSpeechMs: typeof extra.no_speech_ms === "number" ? extra.no_speech_ms : BOX_NO_SPEECH_MS } : {}),
        });
        if (semantic && !buf.boxFailed) {
          buf.semantic = true;
          extra = { ...extra, vad: false };
        }
      } catch (e) {
        if (pick.mode !== "auto") return { ok: false, error: errorCode(e) };
        // auto: the box vanished between the pick and the open: the cloud, batch.
        fallback = errorCode(e);
        pick = { ...pick, provider: this.voice, name: this.voice.name, box: false };
      }
    }
    try {
      return await this.recordInto(buf, pick, id, ms, extra, t0, fallback);
    } finally {
      buf.stream?.abort(); // no-op once its final text is in
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
      if (this.listenBuf === buf) this.listenBuf = undefined;
    }
    const tEnd = buf.endAt ?? this.clock.now();
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
    const buf = this.listenBuf;
    if (!frame || !buf || buf.ended || frame.header.s !== buf.job) return;
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
    if (typeof data.listen === "string" && LOCAL_LISTEN_RE.test(data.listen)) {
      const job = await this.adoptShake(data.listen);
      if (job) Object.assign(clean, { listening: true, turn_id: job });
    }
    await this.appendEvent("shake", clean);
    await this.activity.device("shake", clean);
  }

  /** The shake's listen as a relay listen job (`cmd listen {adopt}`), or declined. Returns the job id. */
  private async adoptShake(local: string): Promise<string | undefined> {
    const caps = this.liveSocket()?.getMeta()?.caps;
    const started = !caps || caps.includes("listen") ? await this.startJob("listen", { adopt: local, trigger: "shake" }) : fail("unsupported");
    if (started.ok) return started.result.job as string;
    // audio busy (a say / listen crossed the shake): the device ends its listen quietly
    void this.sendCmd("listen_stop", { job: local, reason: "no_speech" }).then((r) => {
      if (!r.ok) this.log("shake listen decline failed", { device: this.deviceId, error: r.error });
    });
    this.log("shake listen declined", { device: this.deviceId, error: started.error });
    return undefined;
  }

  /**
   * The shake's listen ended. Heard words → the latest turn (KV, before the event so the
   * fastest answer finds it) and ONE `utterance` event to every agent (inbox + webhooks).
   * Nobody spoke (no_speech, or STT heard nothing): nothing at all.
   */
  private async emitUtterance(id: string, r: Extract<ListenResult, { ok: true }>) {
    const text = r.text.trim();
    if (!text) return;
    await this.kv.put<TurnRecord>(K.turn, { id, at: this.clock.now() });
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
    // agent-facing, one per turn: not subject to the device event rate cap
    await this.appendEvent(UTTERANCE_EVENT, data, true);
    await this.activity.record({ ts: this.clock.now(), kind: "device", ev: "utterance", job: id, data: { text: text.slice(0, 500), reason: r.reason, trigger: "shake" } });
  }

  /** A say / talk against the latest turn (claimTurn), the claim written atomically. */
  private async claim(hash: string, inReplyTo: string | undefined): Promise<Outcome<{ turn?: JsonObject }>> {
    const now = this.clock.now();
    const c = await this.kv.update<TurnRecord, TurnClaim>(K.turn, (cur) => {
      const out = claimTurn(cur, hash, inReplyTo, now);
      return out.ok && out.next ? { next: out.next, result: out } : { result: out };
    });
    if (!c.ok) return fail("turn_claimed", `another agent is already answering the human's turn ${c.turn}: stay quiet (or answer the next one)`, { turn_id: c.turn });
    return { ok: true, result: c.turn ? { turn: c.turn as unknown as JsonObject } : {} };
  }

  /** timings.turn of an answer: the call and its first audio, in ms after the utterance. */
  private answerTimings(args: JsonObject, firstAudioAt?: number): JsonObject {
    const turn = args.turn as unknown as { id: string; at: number };
    return turnTimings(turn, args.called as number, firstAudioAt);
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
        if (meta.box) this.box.onText(sock, frame);
        return;
      default:
        return;
    }
  }

  private async onBoxHello(sock: DeviceSocket, meta: SocketMeta, f: Record<string, unknown>) {
    const str = (v: unknown, max: number) => typeof v === "string" && v.length > 0 && v.length <= max && !/[\u0000-\u001f\u007f]/.test(v);
    const voices = Array.isArray(f.voices) ? f.voices.filter((v): v is string => str(v, 200)).slice(0, 64) : [];
    if (!str(f.name, 64) || (f.version !== undefined && !str(f.version, 32)) || typeof f.stt !== "boolean" || typeof f.tts !== "boolean") {
      return sock.close(CLOSE_PROTOCOL, "malformed voice.hello");
    }
    // Unpaired since the upgrade (revoke / unpair / forget close it too, but never trust a stale socket).
    if (!meta.tok || !(await this.kv.get<TokenRecord>(K.token(meta.tok)))) return sock.close(CLOSE_UNAUTHORIZED, "voice box token revoked");
    const now = this.clock.now();
    sock.setMeta({ ...meta, box: { stt: f.stt, tts: f.tts } });
    await this.kv.put<BoxInfo>(K.vbox, { name: f.name as string, version: (f.version as string | undefined) ?? "", voices, stt: f.stt, tts: f.tts, since: now });
    const cfg = await this.voiceCfg();
    sock.sendText(JSON.stringify({ t: "voice.welcome", device_id: this.deviceId, mode: cfg.mode, voice: cfg.voice ?? null }));
    this.log("voice box connected", { device: this.deviceId, stt: f.stt, tts: f.tts });
    await this.activity.record({ ts: now, kind: "voice_box", ev: "connect", data: { name: f.name as string, stt: f.stt, tts: f.tts } });
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
      await this.kv.put<VoiceCfg>(K.voiceCfg, cfg);
      await this.activity.record({ ts: this.clock.now(), kind: "settings", ev: "voice", data: { mode: cfg.mode, voice: cfg.voice ?? null } });
    }
    return { ok: true, result: await this.voiceView() };
  }

  // ---------------- activity log (core/activity.ts) ----------------

  /** Owner: the whole merged timeline, or set "log transcripts" (link-code auth, like voiceSettings). */
  async ownerActivity(linkCode: string, req: ActivityOwnerRequest): Promise<Outcome> {
    const denied = await this.ownerAuth(linkCode);
    if (denied) return denied;
    if (req.action === "set") {
      if (typeof req.transcripts !== "boolean") return fail("invalid_args", "transcripts must be true or false");
      const before = await this.activity.settings();
      await this.activity.setTranscripts(req.transcripts);
      if (before.transcripts !== req.transcripts) await this.activity.record({ ts: this.clock.now(), kind: "settings", ev: "activity", data: { transcripts: req.transcripts } });
    }
    const q = parseActivityQuery(req.action === "get" ? req.query : {});
    if (typeof q === "string") return fail("invalid_args", q);
    const page = await this.activity.query(q, { owner: true });
    const cfg = await this.activity.settings();
    return {
      ok: true,
      result: { ...page, transcripts: cfg.transcripts, limits: { max_age_h: ACTIVITY_LIMITS.maxAgeMs / 3600000, max_entries: ACTIVITY_LIMITS.chunkEntries * ACTIVITY_LIMITS.maxChunks } } as unknown as JsonObject,
    };
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
    return {
      mode: cfg.mode,
      voice: cfg.voice ?? null,
      cloud: this.voice.name,
      serving: { tts: await serving("tts"), stt: await serving("stt") },
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

  /** Serialized so concurrent frames cannot race the sequence counter. `relay`: relay-generated, not rate-limited. */
  private appendEvent(type: string, data: JsonObject, relay = false): Promise<void> {
    const run = this.evChain.then(() => this.doAppend(type, data, relay));
    this.evChain = run.catch(() => {});
    return run;
  }

  private async doAppend(type: string, data: JsonObject, relay: boolean) {
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
    const ev: AgentEvent = { id: eventId(seq), type, ts: now, data };
    await this.kv.put(K.ev(seq), ev);
    for (const [key, old] of await this.kv.list<AgentEvent>("ev:")) {
      const oldSeq = Number(key.slice(3));
      if (oldSeq <= seq - INBOX_LIMITS.keep || now - old.ts > INBOX_LIMITS.maxAgeMs) await this.kv.delete(key);
    }
    this.notify(this.inboxWaiters, "inbox");
    let queued = false;
    for (const [key, tok] of await this.kv.list<TokenRecord>("tok:")) {
      if (!tok.webhook || tok.webhook.disabled || (tok.webhook.events && !tok.webhook.events.includes(type))) continue;
      if (type === SKILL_EVENT && !skillNotice(tok.skill_seen)) continue; // already current
      const hash = key.slice(4);
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
      .filter((e) => e.seq > from && now - e.ev.ts <= INBOX_LIMITS.maxAgeMs);
    const page = unread.slice(0, limit);
    return { events: page.map((e) => e.ev), next: page.length ? page[page.length - 1].seq : from, more: unread.length > page.length, cursor };
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

  /** Everything that rides along on a successful authenticated outcome: unread events, then the skill and firmware notices. */
  private async ride<T>(a: { rec: TokenRecord; hash: string }, out: Outcome<T>): Promise<Outcome<T>> {
    if (!out.ok) return out;
    await this.announceSkill();
    return this.withFwNotice(a, await this.withNotice(a, await this.withInbox(a.hash, out)));
  }

  /** What the relay knows about the device: the live socket's hello, else the last one stored. */
  private async facts(): Promise<DeviceFacts> {
    const meta = this.liveSocket()?.getMeta();
    if (meta) return { fw: meta.fw ?? null, board: meta.board ?? null, caps: meta.caps ?? null, online: true };
    const d = await this.kv.get<DevInfo>(K.devinfo);
    return { fw: d?.fw ?? null, board: d?.board ?? null, caps: d?.caps ?? null, online: false };
  }

  /**
   * One-time "newer firmware released" notice, same rules as the skill notice (success
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

  /**
   * One-time skill notice, same rule as events: only on success, and marked
   * seen in the same step it is handed over. Atomic on the CURRENT token
   * record: a token revoked meanwhile is never recreated, and two concurrent
   * calls cannot both deliver it.
   */
  private async withNotice<T>(a: { rec: TokenRecord; hash: string }, out: Outcome<T>): Promise<Outcome<T>> {
    if (!out.ok || !skillNotice(a.rec.skill_seen)) return out;
    const notice = await this.kv.update<TokenRecord, SkillNotice | undefined>(K.token(a.hash), (cur) => {
      if (!cur) return { result: undefined };
      const n = skillNotice(cur.skill_seen);
      if (!n) return { result: undefined };
      return { next: { ...cur, skill_seen: SKILL_VERSION }, result: n };
    });
    return notice ? { ...out, notice } : out;
  }

  /**
   * Once per room and skill version: append a `skill.updated` event when some
   * paired agent is behind, so pull/webhook agents hear it without calling.
   */
  private async announceSkill(): Promise<void> {
    if (this.skillAnnounced) return;
    if (((await this.kv.get<number>(K.skillEv)) ?? 0) >= SKILL_VERSION) {
      this.skillAnnounced = true;
      return;
    }
    // New tokens are minted current, so the set of older agents only shrinks.
    const behind = [...(await this.kv.list<TokenRecord>("tok:")).values()].some((t) => t.role !== "voice" && skillNotice(t.skill_seen));
    const won = await this.kv.update<number, boolean>(K.skillEv, (cur) => ((cur ?? 0) >= SKILL_VERSION ? { result: false } : { next: SKILL_VERSION, result: true }));
    this.skillAnnounced = true;
    if (!won || !behind) return;
    const n = skillNotice(undefined)!;
    const origin = await this.kv.get<string>(K.origin);
    await this.appendEvent(SKILL_EVENT, { version: n.version, whats_new: n.whats_new, url: `${origin ?? ""}${SKILL_PATH}` }, true);
  }

  async events(token: string, req: EventsRequest): Promise<Outcome> {
    return this.logged(token, "events", { since: req.since ?? null, wait_ms: req.wait_ms }, () => this.eventsNow(token, req));
  }

  private async eventsNow(token: string, req: EventsRequest): Promise<Outcome> {
    const a = await this.agentAuth(token);
    if ("ok" in a) return a;
    await this.announceSkill();
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
          return this.withFwNotice(a, await this.withNotice(a, { ok: true, result: { events: r.events as unknown as JsonObject[], next: eventId(r.next), more: r.more } }));
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
    const types = parsed.value.events ?? AGENT_EVENT_TYPES;
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
    if (next !== Infinity) await this.scheduler.setAlarm(Math.max(next, this.clock.now()));
  }

  /** Host alarm handler: delivers due webhook requests, then reschedules. */
  async alarm(): Promise<void> {
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
    const ev = await this.kv.get<AgentEvent>(K.ev(d.seq));
    if (!before?.webhook || before.webhook.disabled || !ev || (before.webhook.events && !before.webhook.events.includes(ev.type))) return this.kv.delete(key);
    const hook = before.webhook;
    let error: DeliveryError | undefined;
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
        if (res.status < 200 || res.status > 299) throw new WebhookFailure(`http_${res.status}`);
      });
    } catch (e) {
      error = categorize(e);
    }
    // Delivery stats are written atomically onto the CURRENT record, and only if
    // it still holds this webhook: never resurrect or overwrite a newer config.
    const now = this.clock.now();
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
