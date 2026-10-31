// Remote MCP surface: Streamable HTTP transport (spec 2025-03-26 .. 2025-11-25)
// at POST /mcp, stateless, tools only, JSON responses (no SSE stream: every
// tool answers within ~8 s, or ~24 s for the blocking talk/wait calls, so the
// server never needs to stream).
// Tool names/arguments mirror the local stdio server in apps/cli/
// (internal/molty/mcp.go); relay-specific differences are stated in each
// description. Authorization: Bearer <agent_token>; molty_pair needs none.
//
// Uses only the Fetch API (Request/Response) -- no Cloudflare imports -- so a
// Node host can serve it unchanged.

import { type AgentApi, type FirmwareNotice, type Inbox, type JobView, type Outcome, type RelayBackend, bearer, tokenDevice } from "../core/api.js";
import { BUILTIN_PHRASES, FILLER_DEFAULT_MS, PHRASE_LIMITS } from "../core/phrases.js";
import { ACK_THINK_S, ACTIONS, COLORS, DEFAULT_ASK_COLORS, DEFAULT_ASK_TIMEOUT_S, DEFAULT_LISTEN_MS, LISTEN_CEILING_MS, MAX_ASK_LABEL, MAX_ASK_OPTIONS, MAX_ASK_TIMEOUT_S, MAX_DRAFTS_PER_TURN, MAX_DRAFT_NEW_WORDS, MAX_LISTEN_MS, MAX_MOOD_HOLD_S, MAX_OPENER_WORDS, MOODS, REACT_ACTIONS, REACT_MIN_GAP_MS, REACT_MOODS, TALK_BUDGET_MS, THINK_HOLD_S, VAD_LIMITS } from "../core/commands.js";
import { NOTES_LIMITS } from "../core/notes.js";

/** say / talk / ask: inline [mood] / [action] cues in the spoken text. */
const CUES_DOC = `The text may carry inline cues like [happy] or [nod] (a mood: ${MOODS.join(", ")}; or a gesture: ${ACTIONS.join(", ")}): they are never spoken nor shown, and fire when the word after them is spoken; write a literal bracket as \\[.`;

/** ms (listen) / listen_ms (talk): the window is a ceiling; leaving it out is the usual choice. */
const LISTEN_MS_DOC = `Listen window ceiling in ms: 1..${MAX_LISTEN_MS}, or 0 = unlimited (ends only on end of speech / turn, no_speech, or a ${LISTEN_CEILING_MS / 60000} min safety ceiling, reason max). Omit it: the human's own setting applies (${DEFAULT_LISTEN_MS} ms out of the box). The listen ends early when they finish.`;
import { AGENT_EVENT_TYPES, type AgentEvent, INBOX_LIMITS, eventsLine, seqOf } from "../core/events.js";
import { DEVICE_ID_RE, isRecord, type JsonObject } from "../core/protocol.js";
import { API_VERSION, RELAY_VERSION } from "../core/version.js";

/** Where agents read the skill (the relay does not serve it). */
export const SKILL_HOME = "https://github.com/apresmoi/molty.uno/tree/main/skills/molty";

export const SUPPORTED_VERSIONS = ["2025-11-25", "2025-06-18", "2025-03-26"];
export const SERVER_INFO = { name: "molty-relay", title: `Molty relay (api v${API_VERSION})`, version: RELAY_VERSION };
const MAX_BODY = 256 * 1024;
const INSTRUCTIONS =
  `Tools operate a physical Molty device through the molty-relay. Pair first with molty_pair (no token needed), then send the returned agent_token as 'Authorization: Bearer <token>'. For conversation use molty_talk: ONE call per turn speaks your reply, then listens and returns what the human said ({heard, reason}); moods (listening, thinking, speaking) are automatic, so don't call molty_mood around talking. When the human finishes, Molty acknowledges it by itself at once (a soft chirp, then thinking). If you will think for more than a moment, FIRST play a quick phrase (molty_quick {id}: cached audio, plays at once, e.g. "hmm", "let_me_think"), before you start reasoning, then answer with molty_talk (it follows the phrase seamlessly); register your own phrases once with molty_phrases. Nothing from you within a few seconds and Molty says a filler itself. Keep spoken replies to 1-2 sentences (inline cues like [happy] or [nod] in them set mood / gesture as you speak). For a decision the human can answer with a tap use molty_ask: say the question, offer 1-${MAX_ASK_OPTIONS} options of at most ${MAX_ASK_LABEL} characters ("yes", "no", "later"); answer is null on timeout/cancelled/offline. molty_say / molty_listen return a job immediately (wait:true blocks until done); poll a job with molty_wait (<= 8 s per call). ` +
  `Molty also emits events (${AGENT_EVENT_TYPES.join(", ")}) into your own inbox: unread ones ride along on every tool result as 'events' plus a 'Molty: ...' line; molty_events pulls/waits for them; molty_webhook registers an HTTPS request the relay sends you per event. A shake means the human wants to talk: Molty listens by itself and you get one 'utterance' event {turn_id, text, ...}: answer it at once with molty_talk {say, in_reply_to: turn_id} (the first agent to answer claims the turn; others get turn_claimed). While they are still talking you may get 'utterance.partial' events (context only): never speak then (human_speaking); you may molty_draft a short opener (played the instant they finish, if their last words don't change the meaning) and molty_react with a mood or a gesture (screen only). Other events: react briefly, once per moment. Full guide: the molty agent skill, ${SKILL_HOME} (Claude Code / Codex: the molty plugin). This relay speaks agent api_version ${API_VERSION} (also in molty_status relay.api_version); what this Molty can do is relay.caps. When your human asks to check Molty for updates (any wording), call molty_updates and follow its 'actions'; ask before molty_update.`;

type Prop = { type: string | string[]; description: string; enum?: string[]; items?: { type: "string"; enum?: string[] } | { anyOf: JsonObject[] }; additionalProperties?: { type: "number" } };
type Schema = { type: "object"; properties: Record<string, Prop>; required: string[]; additionalProperties: false };
interface ToolDef {
  name: string;
  description: string;
  inputSchema: Schema;
  auth: boolean;
  run(api: AgentApi, token: string, args: Record<string, unknown>): Promise<Outcome<JsonObject>>;
}

const items = (p: Prop) => (p.items && "type" in p.items ? p.items : undefined);

const VAD_PROPS: Schema["properties"] = {
  end_silence_ms: { type: "integer", description: `Device end-of-speech: silence that ends the turn, ${VAD_LIMITS.end_silence_ms[0]}..${VAD_LIMITS.end_silence_ms[1]} ms (device default 700).` },
  no_speech_ms: { type: "integer", description: `Device end-of-speech: give up when nobody starts talking within ${VAD_LIMITS.no_speech_ms[0]}..${VAD_LIMITS.no_speech_ms[1]} ms (device default 4000).` },
  vad: { type: "boolean", description: "Device end-of-speech detection on or off (record the whole window). Leave it out: with the owner's voice box, semantic end of turn is automatic (the box decides; device VAD off); otherwise the device's detection is on." },
  ring_color: { type: "string", description: `Colour of the listening ring on Molty's screen: ${Object.keys(COLORS).join(", ")} or "#rrggbb". Default: the device's setting.` },
  ack: { type: ["boolean", "string"], description: `Molty's own acknowledgement when the human finishes (a soft chirp + thinking, ${ACK_THINK_S} s max, before you answer): true (default) = the human's setting; "silent" = thinking without the chirp; false = none. Leave it out unless you acknowledge yourself.` },
};

/** say / talk / ask: the agent's own latency report (core/commands.ts clientTiming). */
const CLIENT_TIMING: Prop = {
  type: "object",
  additionalProperties: { type: "number" },
  description: "Optional: your own latency for this turn, in ms (up to 8 keys, lower case, numbers 0..3600000), e.g. {received_ms, model_ms}: event received -> this call, your model's run. Include it to help debug latency: it lands in the turn's trace (activity log), never on Molty.",
};
const IN_REPLY_TO: Prop = {
  type: "string",
  description: "The turn_id of the `utterance` you are answering (the human shook Molty and spoke). The first agent to answer a turn claims it; other agents get turn_claimed for 60 s. Adds timings.turn {id, reply, first_audio} (ms after the utterance).",
};

/** say/listen with `wait: true` block on their job (bounded), returning what molty_wait would. */
async function commandWait(api: AgentApi, t: string, name: "say" | "listen", a: Record<string, unknown>): Promise<Outcome<JsonObject>> {
  const { wait, ...args } = a;
  const start = Date.now();
  const out = await api.command(t, name, args);
  if (!out.ok || wait !== true) return out;
  return jobOutcome(await api.job(t, out.result.job as string, Math.max(0, TALK_BUDGET_MS - (Date.now() - start))));
}

/** Shapes a job view like molty_wait: done -> result + {job, status}, failed -> error, else pending. */
function jobOutcome(out: Outcome<JobView>): Outcome<JsonObject> {
  if (!out.ok) return out;
  const j = out.result;
  if (j.status === "done") return { ok: true, result: { ...(j.result ?? {}), job: j.job, status: "done" } as JsonObject };
  if (j.status === "failed") return { ok: false, error: j.error ?? "failed", message: typeof j.detail === "string" ? j.detail : `job ${j.job} failed` };
  return { ok: true, result: { job: j.job, status: "pending", state: j.status, ...(typeof j.phase === "string" ? { phase: j.phase } : {}) } };
}

const obj = (properties: Schema["properties"], required: string[] = []): Schema => ({ type: "object", properties, required, additionalProperties: false });

export const TOOLS: ToolDef[] = [
  {
    name: "molty_pair",
    description:
      "Pair with a Molty device: no token needed. Give device_id, link_code (8 chars from the device console's 'Connect an agent' block) and agent_name; the device shows agent_name plus a 4-digit code and its owner taps to accept. Waits up to wait_s (default 8, max 110); if still undecided returns status pending with pair_id and code: call again with device_id and pair_id to keep waiting (the request lives 120 s). On approval returns agent_token (shown once; store it) for all other tools.",
    inputSchema: obj(
      {
        device_id: { type: "string", description: "Device id, e.g. molty-k7m2q9xa4c." },
        link_code: { type: "string", description: "Eight-character link code [A-Z2-9] shown in the device console." },
        agent_name: { type: "string", description: "Agent name shown on the device for approval (1..64 chars)." },
        pair_id: { type: "string", description: "Resume a pending pairing returned by an earlier call (instead of link_code/agent_name)." },
        wait_s: { type: "integer", description: "Seconds to wait for the on-device decision, 0..110; default 8." },
      },
      ["device_id"],
    ),
    auth: false,
    async run(api, _token, a) {
      const wait = a.wait_s === undefined ? 8 : (a.wait_s as number);
      if (!Number.isInteger(wait) || wait < 0 || wait > 110) return { ok: false, error: "invalid_args", message: "wait_s must be 0..110" };
      if (a.pair_id === undefined && (a.link_code === undefined || a.agent_name === undefined)) {
        return { ok: false, error: "invalid_args", message: "give link_code and agent_name, or pair_id" };
      }
      const out = await api.pair({ device_id: a.device_id as string, link_code: a.link_code as string | undefined, agent_name: a.agent_name as string | undefined, pair_id: a.pair_id as string | undefined, wait_ms: wait * 1000 });
      if (!out.ok) return out;
      const r = out.result;
      if (r.status === "approved") return { ok: true, result: { ...r, usage: "Send 'Authorization: Bearer <agent_token>' on every MCP request." } };
      return { ok: true, result: { ...r, hint: "Not decided yet: call molty_pair again with device_id and pair_id." } };
    },
  },
  {
    name: "molty_status",
    description: "Return the device's status (fw, uptime, creature, state, mood, fps, rssi) plus relay info: online, connected_since, caps, voice provider and the latest device events.",
    inputSchema: obj({}),
    auth: true,
    run: (api, t) => api.command(t, "status", {}),
  },
  {
    name: "molty_mood",
    description: `Show ${MOODS.join(", ")}. Replaces the current display. Quick moods (happy, surprised) revert on their own; listening/thinking/speaking persist. hold_s sets how long it stays (0..${MAX_MOOD_HOLD_S} s; 0 = until the next mood).`,
    inputSchema: obj(
      {
        mood: { type: "string", description: `One of ${MOODS.join(", ")}.` },
        hold_s: { type: "number", description: `Optional seconds to hold the mood before reverting, 0..${MAX_MOOD_HOLD_S}; 0 = until the next mood.` },
      },
      ["mood"],
    ),
    auth: true,
    run: (api, t, a) => api.command(t, "mood", a),
  },
  {
    name: "molty_act",
    description: `Play a one-shot gesture over the current mood: ${ACTIONS.join(", ")}. It never changes what is shown; while words or a question cover the creature it waits up to 3 s, then is dropped. To gesture while speaking, put [nod] etc. in the say text instead.`,
    inputSchema: obj({ action: { type: "string", description: `One of ${ACTIONS.join(", ")}.`, enum: [...ACTIONS] } }, ["action"]),
    auth: true,
    run: (api, t, a) => api.command(t, "act", a),
  },
  {
    name: "molty_show",
    description: "Display up to 16 KB of text with the speed reader at WPM (default 300). Returns after starting, not after reading completes.",
    inputSchema: obj(
      {
        text: { type: "string", description: "Nonempty UTF-8 text, at most 16384 bytes, to display using the RSVP reader." },
        wpm: { type: "integer", description: "Words per minute (100..800); default 300." },
      },
      ["text"],
    ),
    auth: true,
    run: (api, t, a) => api.command(t, "show", a),
  },
  {
    name: "molty_say",
    description:
      "Speak text through the device speaker using the relay's TTS (streamed: audio starts after the first sentence is synthesised). Returns {job} immediately; poll molty_wait until done ({ms, text, timings}), or pass wait:true to block until done. The device shows the speaking mood and the text while audio plays. One audio job (say/listen/talk) at a time; audio_busy otherwise. For a conversation turn use molty_talk instead. " + CUES_DOC,
    inputSchema: obj(
      {
        text: { type: "string", description: "Text to synthesize, at most 4096 bytes." },
        voice: { type: "string", description: "Optional relay TTS voice (provider-specific, e.g. luna for Workers AI Aura)." },
        in_reply_to: IN_REPLY_TO,
        client_timing: CLIENT_TIMING,
        wait: { type: "boolean", description: `true: block until spoken (at most ${TALK_BUDGET_MS / 1000} s, then {job, status:"pending"} for molty_wait).` },
      },
      ["text"],
    ),
    auth: true,
    run: (api, t, a) => commandWait(api, t, "say", a),
  },
  {
    name: "molty_quick",
    description: `Play a quick phrase NOW: its audio is cached in Molty's voice, so it starts at once (no speech synthesis) and the call returns at once: {job, phrase, text, cached}. Use it the moment you know you will think for more than a moment, BEFORE you start reasoning; then answer with molty_talk / molty_say, which queues right after the phrase (no audio_busy). Built-ins: ${BUILTIN_PHRASES.map((p) => `${p.id} ("${p.text}")`).join(", ")}; register your own with molty_phrases. Claims the human's turn like molty_say (in_reply_to). If Molty already said its own filler for this turn (nobody answered within the owner's delay, ${FILLER_DEFAULT_MS} ms by default) it is skipped: {skipped:"filled", filler:{phrase, text, ms_ago}} — go straight to your answer (force:true plays it anyway).`,
    inputSchema: obj(
      {
        id: { type: "string", description: 'Phrase id: yours (molty_phrases) or a built-in, e.g. "hmm".' },
        in_reply_to: IN_REPLY_TO,
        force: { type: "boolean", description: "true: play even right after Molty's own filler." },
      },
      ["id"],
    ),
    auth: true,
    run: (api, t, a) => api.command(t, "quick", a),
  },
  {
    name: "molty_phrases",
    description: `Your own quick phrases for molty_quick, in your voice and personality: list (default), set (upsert {id, text} items; their audio is synthesised now, in Molty's current voice, and re-synthesised when the voice changes) or delete (id, or ids; ids:["*"] = all yours). At most ${PHRASE_LIMITS.perAgent} phrases, each at most ${PHRASE_LIMITS.textMax} characters (inline cues allowed); an id of yours shadows a built-in of the same id. Returns {phrases:[{id, text, ready}], builtins:[{id, text, ready, shadowed?}], limits, voice} (ready = cached: plays with zero delay). Mark some filler:true (with for: question | request | hard | any) and Molty's own filler — when you are slow to answer a turn — is yours, picked to fit what the human said; builtins:false turns the built-ins off for you. Set them once, e.g. [{"id":"hmm","text":"Hmm…","filler":true},{"id":"think","text":"Ooh, let me think about that.","filler":true,"for":"hard"}].`,
    inputSchema: obj({
      action: { type: "string", enum: ["list", "set", "delete"], description: "list (default without phrases), set (default with phrases), delete." },
      phrases: {
        type: "array",
        items: {
          anyOf: [
            {
              type: "object",
              properties: { id: { type: "string" }, text: { type: "string" }, filler: { type: "boolean" }, for: { type: "string", enum: ["question", "request", "hard", "any"] } },
              required: ["id", "text"],
              additionalProperties: false,
            },
          ],
        },
        description: `set: 1..${PHRASE_LIMITS.perAgent} {id, text, filler?, for?}: id 1..24 of a-z 0-9 _ -, text 1..${PHRASE_LIMITS.textMax} characters; filler:true = Molty may say it as its own filler while you think (after a turn you answered); for = the turns it fits: question | request | hard | any (default).`,
      },
      builtins: { type: "boolean", description: "set: false = you never use or see the built-in phrases (Molty's filler for you then uses only your filler:true phrases, or none); true = back on." },
      id: { type: "string", description: "delete: one phrase id." },
      ids: { type: "array", items: { type: "string" }, description: 'delete: several phrase ids, or ["*"] for all of yours.' },
    }),
    auth: true,
    run: (api, t, a) => api.phrases(t, a),
  },
  {
    name: "molty_listen",
    description: `Record the device microphone until the human finishes (end of speech / end of turn; the window is only a ceiling: the human's own setting, ${DEFAULT_LISTEN_MS} ms out of the box, at most ${MAX_LISTEN_MS}, or unlimited) and transcribe it with the relay's STT. Returns {job} immediately; molty_wait returns {text, duration_ms, peak, rms, reason, speech_ms?, timings}; wait:true blocks until done. The device shows listening while recording and thinking after speech ends. No audio file is returned.`,
    inputSchema: obj({ ms: { type: "integer", description: LISTEN_MS_DOC }, ...VAD_PROPS, wait: { type: "boolean", description: `true: block until transcribed (at most ${TALK_BUDGET_MS / 1000} s).` } }),
    auth: true,
    run: (api, t, a) => commandWait(api, t, "listen", a),
  },
  {
    name: "molty_talk",
    description: `One conversation turn in ONE call: speaks 'say' (if given), then listens and returns what the human said: {heard, said?, reason: silence|turn_end|no_speech|max|ended|error (turn_end = they finished, like silence), speech_ms?, ms:{tts_first_audio, speech, listen, stt, total}}. Moods are automatic (speaking, listening, then thinking until your next command, ${THINK_HOLD_S} s max): don't call molty_mood around it. Blocks at most ${TALK_BUDGET_MS / 1000} s; a longer turn returns {job, status:"pending"}: finish it with molty_wait. Keep 'say' to 1-2 sentences. ${CUES_DOC}`,
    inputSchema: obj({
      say: { type: "string", description: "What to say first (at most 4096 bytes). Omit to just listen." },
      listen: { type: "boolean", description: "Listen after speaking; default true. false = just speak (and wait until spoken)." },
      listen_ms: { type: "integer", description: LISTEN_MS_DOC },
      voice: { type: "string", description: "Optional relay TTS voice." },
      in_reply_to: IN_REPLY_TO,
      client_timing: CLIENT_TIMING,
      ...VAD_PROPS,
    }),
    auth: true,
    run: (api, t, a) => api.talk(t, a),
  },
  {
    name: "molty_ask",
    description: `Ask the human a question they answer with a TAP on Molty's round screen: speaks 'say' (if given; put the question there), then shows 1-${MAX_ASK_OPTIONS} options and returns which one they tapped: {answer: label|null, index: int|null, reason?: timeout|cancelled|offline, said?, ms:{tts_first_audio?, speech?, answer, total}}. Options must be SHORT: 1-${MAX_ASK_LABEL} printable ASCII characters each, unique (good: "yes", "no", "later", "idk", "ok"; bad: "Yes, please do it", "maybe later"). 1 option = one centred button ("ok", "done"); 2 = halves; 3-${MAX_ASK_OPTIONS} = pie slices. answer null: nobody tapped within timeout_s (timeout), another command replaced the question (cancelled), or Molty went offline. Use molty_talk instead for open-ended answers. Blocks at most ${TALK_BUDGET_MS / 1000} s; longer returns {job, status:"pending"}: finish it with molty_wait. ${CUES_DOC}`,
    inputSchema: obj(
      {
        say: { type: "string", description: "The question, spoken first (at most 4096 bytes). Recommended." },
        options: {
          type: "array",
          items: {
            anyOf: [
              { type: "string" },
              { type: "object", properties: { label: { type: "string" }, color: { type: "string" } }, required: ["label"], additionalProperties: false },
            ],
          },
          description: `1..${MAX_ASK_OPTIONS} answers: a label (1..${MAX_ASK_LABEL} printable ASCII characters, unique ignoring case) or {label, color} with color one of ${Object.keys(COLORS).join(", ")} or "#rrggbb", e.g. [{"label":"yes","color":"green"},{"label":"no","color":"red"}]. Default colours by position: ${DEFAULT_ASK_COLORS.join(", ")}.`,
        },
        timeout_s: { type: "integer", description: `Seconds the options stay up waiting for a tap, 1..${MAX_ASK_TIMEOUT_S}; default ${DEFAULT_ASK_TIMEOUT_S}.` },
        voice: { type: "string", description: "Optional relay TTS voice for 'say'." },
        client_timing: CLIENT_TIMING,
      },
      ["options"],
    ),
    auth: true,
    run: (api, t, a) => api.ask(t, a),
  },
  {
    name: "molty_draft",
    description: `While the human is STILL TALKING (after an 'utterance.partial' {turn_id, seq, text_so_far}): hold a short spoken opener for your answer. The relay synthesises it now and plays it the instant they finish (right after Molty's chirp), claiming the turn for you — but only if the final words add at most max_new_words beyond that partial and none of them turns the meaning (but, not, actually, …); otherwise you get 'draft.dropped' {reason} and answer the 'utterance' as usual. On 'draft.used' your opener is playing: CONTINUE it (don't repeat it) with molty_talk / molty_say {in_reply_to: turn_id}, which queues right behind it with no gap. Openers must fit any ending: "Sure —", "Good question.", "Let me check." (<= ${MAX_OPENER_WORDS} words). A new draft replaces yours (<= ${MAX_DRAFTS_PER_TURN} per turn). Returns {turn_id, based_on, status:"held", max_new_words (the relay caps it with its own calibrated threshold)}; turn_closed when they already finished.`,
    inputSchema: obj(
      {
        turn_id: { type: "string", description: "The open turn's id (utterance.partial turn_id)." },
        based_on: { type: "integer", description: "The seq of the partial your opener answers." },
        opener: { type: "string", description: `The opener, spoken as is: one short line (<= ${MAX_OPENER_WORDS} words) that fits whatever they add.` },
        max_new_words: { type: "integer", description: `Drop the draft if the final transcript adds more than this many words after based_on's text (0..${MAX_DRAFT_NEW_WORDS}; default and ceiling: the relay's calibrated threshold).` },
        voice: { type: "string", description: "Optional relay TTS voice (use the same for the follow-up)." },
      },
      ["turn_id", "based_on", "opener"],
    ),
    auth: true,
    run: (api, t, a) => api.draft(t, a),
  },
  {
    name: "molty_react",
    description: `While the human is STILL TALKING (an open turn, after 'utterance.partial'): show a reaction on Molty's screen over its listening animation — no sound (the mic is open) — for ~1.5 s, then it listens on. mood: ${REACT_MOODS.join(", ")}${REACT_ACTIONS.length ? `; or action: ${REACT_ACTIONS.join(", ")}` : ""}. At most one per ${REACT_MIN_GAP_MS / 1000} s per Molty (rate_limited with retry_after_ms). Sparingly: a nod to what they just said, not a running commentary. turn_closed when they already finished; unsupported on firmware without the react cap.`,
    inputSchema: obj(
      {
        turn_id: { type: "string", description: "The open turn's id (utterance.partial turn_id)." },
        mood: { type: "string", description: `One of ${REACT_MOODS.join(", ")}.`, enum: [...REACT_MOODS] },
        ...(REACT_ACTIONS.length ? { action: { type: "string", description: `One of ${REACT_ACTIONS.join(", ")}.`, enum: [...REACT_ACTIONS] } } : {}),
      },
      ["turn_id"],
    ),
    auth: true,
    run: (api, t, a) => api.react(t, a),
  },
  {
    name: "molty_notes",
    description: `Your PRIVATE notes on this Molty, kept by the relay for your token only (no other agent and not the owner's log can read the values): keep your behaviour ledger here when you have no memory of your own — what you tried (a react, an opener, a joke) and how the human responded — and read it at the start of a session. action: list (keys + sizes), get {key}, put {key, value: any JSON}, delete {key}. Limits: ${NOTES_LIMITS.keys} keys, ${NOTES_LIMITS.valueBytes / 1024} KB per value, ${NOTES_LIMITS.totalBytes / 1024} KB in all (too_large beyond). Deleted with your token.`,
    inputSchema: obj(
      {
        action: { type: "string", description: "list, get, put or delete.", enum: ["list", "get", "put", "delete"] },
        key: { type: "string", description: "1..64 characters: letters, digits and . _ : - (e.g. ledger, ledger:reacts)." },
        value: { type: ["string", "number", "boolean", "object", "array"], description: "put: the value, any JSON (e.g. a list of {tried, response} entries)." },
      },
      ["action"],
    ),
    auth: true,
    run: (api, t, a) => api.notes(t, a as never),
  },
  {
    name: "molty_updates",
    description:
      "Check Molty for updates, in one call: use it whenever your human asks to check Molty for updates (any wording). Returns {api_version, firmware:{current, board, online, latest|null, status: current|update_available|no_build_for_board|no_release|unknown, update_available, notes_url, needed_for:[{feature, what, needs, released}], can_self_update, release_check}, actions:[plain-language steps]}. Follow 'actions' in order: when firmware is behind ASK your human before calling molty_update. Marks the reported release as seen for you. The skill itself updates with the molty plugin, not through the relay.",
    inputSchema: obj({}),
    auth: true,
    run: (api, t) => api.updates(t),
  },
  {
    name: "molty_update",
    description: `Ask Molty to install the latest official firmware release itself. Only after your human said yes: Molty also asks them on its screen (the version, e.g. "v0.2.0", or "later"), then downloads, verifies (sha256), installs and restarts (about a minute; it rolls back on failure). No arguments: the relay picks the release's image for this board. Returns {status:"up_to_date", current, latest} when there is nothing newer; else a job: done {outcome:"updated", from, to} or {outcome:"declined", reason?}; errors update_failed, rolled_back, update_timeout. Blocks at most ${TALK_BUDGET_MS / 1000} s, then {job, status:"pending", phase: confirm|accepted|downloading|flashing|rebooting}: keep calling molty_wait. Firmware without the 'update' cap: unsupported, with 'how' (the console's Update).`,
    inputSchema: obj({}),
    auth: true,
    run: (api, t) => api.update(t, {}),
  },
  {
    name: "molty_wait",
    description: "Wait at most timeout_s (0..8, default 8) for a say/listen/talk/ask/update job. Returns the job result with status done, status pending if still queued/running (call again), or an error if it failed.",
    inputSchema: obj(
      {
        job: { type: "string", description: "Job id returned by molty_say, molty_listen, molty_talk, molty_ask or molty_update." },
        timeout_s: { type: "integer", description: "Polling budget, integer 0..8 seconds; default 8." },
      },
      ["job"],
    ),
    auth: true,
    async run(api, t, a): Promise<Outcome<JsonObject>> {
      const s = a.timeout_s === undefined ? 8 : (a.timeout_s as number);
      if (!Number.isInteger(s) || s < 0 || s > 8) return { ok: false, error: "invalid_args", message: "timeout_s must be 0..8" };
      return jobOutcome(await api.job(t, a.job as string, s * 1000));
    },
  },
  {
    name: "molty_events",
    description: `Your unread Molty events (${AGENT_EVENT_TYPES.join(", ")}), oldest first: {events:[{id, type, ts, data}], next, more}. Default since = your read cursor (each event is returned to you once; it advances). wait_s (0..${INBOX_LIMITS.mcpWaitS}, default 0) long-polls until an event arrives. since = an event id (e.g. e41) re-reads after it. shake {peak_g, reversals, ms, listening?, turn_id?} (listening: Molty is already listening, wait for the utterance); utterance {turn_id, text, reason, ms:{speech, stt}, trigger, device:{caps, mood, voice}, claimed?} (the human's words after a shake: answer with molty_talk {say, in_reply_to: turn_id}; claimed:true = another agent's opener already answers it); utterance.partial {turn_id, seq, text_so_far, new_text, elapsed_ms} (they are STILL talking: context only, never answer it — molty_draft / molty_react at most); draft.used {turn_id, based_on, opener, new_words} / draft.dropped {turn_id, based_on, reason, new_words?} (your draft's fate, to you only); tap {x, y} in 0..1; button {id: accept|reject} (physical buttons: acknowledge / dismiss what you showed, never an answer); answer {job, index, label, reason?} (how a molty_ask ended).`,
    inputSchema: obj({
      since: { type: "string", description: "Event id to read after, e.g. e41; default your cursor." },
      wait_s: { type: "integer", description: `Seconds to wait for a new event, 0..${INBOX_LIMITS.mcpWaitS}; default 0.` },
    }),
    auth: true,
    async run(api, t, a) {
      const wait = a.wait_s === undefined ? 0 : (a.wait_s as number);
      if (!Number.isInteger(wait) || wait < 0 || wait > INBOX_LIMITS.mcpWaitS) return { ok: false, error: "invalid_args", message: `wait_s must be 0..${INBOX_LIMITS.mcpWaitS}` };
      const since = a.since === undefined ? undefined : seqOf(a.since);
      if (a.since !== undefined && since === undefined) return { ok: false, error: "invalid_args", message: "since must be an event id like e41" };
      return api.events(t, { since, wait_ms: wait * 1000 });
    },
  },
  {
    name: "molty_webhook",
    description:
      "Have the relay call YOU on each event: an HTTPS request you define. url (https, public host); method POST (default) | GET | PUT | PATCH; headers (your own auth; stored, never shown again); body template + content_type. Placeholders in url and body: {{event.type}} {{event.id}} {{event.ts}} {{event.data}} (JSON) {{event.data.<field>}} (nested up to 3 deep, e.g. {{event.data.ms.speech}}; an utterance's words: \"{{event.data.text}}\") {{device}} {{text}} (e.g. 'Molty was shaken'), escaped for JSON / form / URL. No body = {\"device\",\"event\"} JSON. A missing field renders its {{event.data.x|default}}, else null in JSON / empty elsewhere. events (optional) limits the types sent. Every request is signed: Molty-Signature: t=<ms>,v2=hex(hmac_sha256(secret, t+'\\n'+METHOD+'\\n'+path?query+'\\n'+body)); the secret is returned once. Returns a dry run; the template is validated against every event type it will receive. Omit url to read the current webhook; remove:true deletes it. Retries 1 s/10 s/60 s; disabled after 20 consecutive failures (re-register to re-enable).",
    inputSchema: obj({
      url: { type: "string", description: "https URL the relay requests per event; may contain placeholders (URL-encoded)." },
      method: { type: "string", description: "POST (default), GET, PUT or PATCH.", enum: ["POST", "GET", "PUT", "PATCH"] },
      headers: { type: "object", description: "Extra request headers, e.g. your API key. Values are stored and never echoed." },
      body: { type: "string", description: "Body template (max 4096 bytes); default is the relay's signed {device, event} JSON." },
      content_type: { type: "string", description: "Body media type; application/json escapes placeholders as JSON strings." },
      events: { type: "array", items: { type: "string", enum: AGENT_EVENT_TYPES }, description: `Event types to send (default all): ${AGENT_EVENT_TYPES.join(", ")}.` },
      remove: { type: "boolean", description: "true removes your webhook." },
    }),
    auth: true,
    run(api, t, a) {
      if (a.remove === true) return api.webhook(t, { action: "delete" });
      if (a.url === undefined) return api.webhook(t, { action: "get" });
      const { remove: _r, ...spec } = a;
      return api.webhook(t, { action: "set", spec });
    },
  },
  {
    name: "molty_state",
    description: "Read creature state (omit state) or patch it (state = object of creature fields, same semantics as the device's /api/creature/state).",
    inputSchema: obj({ state: { type: "object", description: "State patch; omit to read state." } }),
    auth: true,
    run: (api, t, a) => api.command(t, "state", a.state === undefined ? {} : { state: a.state }),
  },
  {
    name: "molty_volume",
    description: "Set the physical speaker volume from 0 (mute) to 21 (maximum).",
    inputSchema: obj({ volume: { type: "integer", description: "Speaker volume, integer 0..21." } }, ["volume"]),
    auth: true,
    run: (api, t, a) => api.command(t, "volume", a),
  },
  {
    name: "molty_imu",
    description: "Read one IMU snapshot: ax/ay/az acceleration in g, gx/gy/gz angular velocity in degrees/second, ms device sample timestamp.",
    inputSchema: obj({}),
    auth: true,
    run: (api, t) => api.command(t, "imu", {}),
  },
  {
    name: "molty_diag",
    description:
      "Device health: sim/panel frame-time histograms (fps, p50/p95/p99/max ms, frames over 50 ms), touch-to-panel latency, per-task CPU %, internal/PSRAM heap free and low-water, allocation failures, stall probes, Wi-Fi/gateway ping and link state. timeline (0..180) adds that many seconds of the 1 Hz timeline.",
    inputSchema: obj({ timeline: { type: "integer", description: "Seconds of 1 Hz timeline to include, 0..180 (default 0)." } }),
    auth: true,
    run: (api, t, a) => api.command(t, "diag", a),
  },
  {
    name: "molty_activity",
    description:
      "Debug yourself: YOUR OWN activity on this Molty, newest last — one entry per call you made (op, args summary, ok/error, ms), per job you started (stage timings, voice provider box/cloud + fallback, reason, heard/said unless the owner turned transcripts off), Molty's own log lines tagged with your jobs (device_log: level E/W/I/EVT, text), and Molty's connects/disconnects (close codes) around your jobs. Never other agents' entries. Use it when a turn failed or felt slow: molty_activity {job} on the job id from the error (failures carry it as 'job'), or errors:true. {entries:[{id, ts, kind: call|job|device|device_log|pair, ev?, op?, job?, args?, ok?, error?, ms?, data?}], next, more}; pass next back as since for newer entries. Kept 24 h.",
    inputSchema: obj({
      since: { type: "string", description: "Entry id (e.g. a42) to read after, or a unix time in ms; default: the newest entries." },
      limit: { type: "integer", description: "Entries to return, 1..500 (default 200)." },
      kind: { type: "string", description: "Comma-separated kinds: call, job, device, device_log, pair." },
      job: { type: "string", description: "Only this job's entries (a job id from molty_talk / molty_say / an error's 'job')." },
      errors: { type: "boolean", description: "true: only failures (failed calls / jobs, device E lines)." },
    }),
    auth: true,
    run: (api, t, a) => api.activityLog(t, a),
  },
  {
    name: "molty_log",
    description:
      "Tail the device log ring (ESP_LOG lines + structured EVT events: link, audio, stalls, heap, Wi-Fi). Returns {next, text}; pass next back as since to continue. max is 1024..32768 bytes (default 12288).",
    inputSchema: obj({
      since: { type: "integer", description: "First log sequence number to return (default 0 = oldest kept)." },
      max: { type: "integer", description: "Maximum bytes of text, 1024..32768." },
    }),
    auth: true,
    run: (api, t, a) => api.command(t, "log", a),
  },
];

const TOOL_MAP = new Map(TOOLS.map((t) => [t.name, t]));

/** Same checks as the Go addTool: object, no unknown/null keys, required present, primitive types. */
export function checkArgs(schema: Schema, raw: unknown): string | undefined {
  const args = raw === undefined ? {} : raw;
  if (!isRecord(args)) return "arguments must be an object";
  for (const [k, v] of Object.entries(args)) {
    const prop = schema.properties[k];
    if (!prop) return `unknown argument ${k}`;
    if (v === null) return `${k} must not be null`;
    const ok =
      prop.type === "string"
        ? typeof v === "string"
        : prop.type === "integer"
          ? Number.isInteger(v)
          : prop.type === "number"
            ? typeof v === "number" && Number.isFinite(v)
          : prop.type === "object"
            ? isRecord(v)
            : prop.type === "boolean"
              ? typeof v === "boolean"
              : prop.type === "array"
                ? Array.isArray(v) && ((prop.items !== undefined && "anyOf" in prop.items) || v.every((x) => typeof x === "string" && (!items(prop)?.enum || items(prop)!.enum!.includes(x))))
                : true;
    // anyOf items (molty_ask options): the command's own validation checks each one
    if (!ok && prop.type === "array") return `${k} must be a list of ${items(prop)?.enum ? items(prop)!.enum!.join(", ") : prop.items && "anyOf" in prop.items ? "items" : "strings"}`;
    if (!ok) return `${k} must be ${prop.type === "integer" ? "an integer" : `a${prop.type === "object" ? "n" : ""} ${prop.type}`}`;
    if (prop.enum && !prop.enum.includes(v as string)) return `${k} must be one of ${prop.enum.join(", ")}`;
  }
  for (const k of schema.required) if (!(k in args)) return `missing required argument ${k}`;
  return undefined;
}

type RpcMessage = { jsonrpc?: unknown; id?: unknown; method?: unknown; params?: unknown };
const rpcError = (id: unknown, code: number, message: string) => ({ jsonrpc: "2.0", id: id ?? null, error: { code, message } });
const rpcResult = (id: unknown, result: unknown) => ({ jsonrpc: "2.0", id, result });

/**
 * Unread events ride along additively: `events` (+ `more_events`) in the
 * structured result and one extra text item ("Molty: was shaken ... 2 min ago").
 * Results without unread events are byte-for-byte what they were before.
 */
function toolResult(out: Outcome<JsonObject>) {
  const value: JsonObject = out.ok ? { ok: true, ...out.result } : { ok: false, error: out.error, ...(out.message ? { message: out.message } : {}), ...(out.details ?? {}) };
  let line: string | undefined;
  if (out.inbox && out.inbox.events.length > 0 && !("events" in value)) {
    value.events = out.inbox.events as unknown as JsonObject[];
    if (out.inbox.more) value.more_events = true;
    line = eventsLine(out.inbox.events, out.inbox.now, out.inbox.more);
  } else if (out.ok && Array.isArray(value.events) && value.events.length > 0 && "next" in value) {
    line = eventsLine(value.events as unknown as AgentEvent[], Date.now(), value.more === true); // molty_events itself
  }
  // One-time firmware release notice (already marked seen): structured field + one line.
  if (out.fw_notice && !("firmware_update" in value)) value.firmware_update = out.fw_notice as unknown as JsonObject;
  const content = [{ type: "text", text: JSON.stringify(value) }];
  if (line) content.push({ type: "text", text: line });
  if (out.fw_notice) content.push({ type: "text", text: firmwareLine(out.fw_notice) });
  return { content, structuredContent: value, isError: !out.ok };
}

/** The one-line MCP text item for a new firmware release. */
export const firmwareLine = (n: FirmwareNotice): string =>
  `Molty firmware ${n.latest} is out (Molty runs ${n.current}). Mention it to your human once and ask before updating; molty_updates has the details.`;

async function callTool(backend: RelayBackend, token: string | undefined, params: unknown) {
  if (!isRecord(params) || typeof params.name !== "string") return { error: [-32602, "Invalid tool parameters"] as const };
  const tool = TOOL_MAP.get(params.name);
  if (!tool) return { error: [-32602, `Unknown tool ${params.name}`] as const };
  const problem = checkArgs(tool.inputSchema, params.arguments);
  if (problem) return { result: toolResult({ ok: false, error: "invalid_args", message: problem }) };
  const args = (params.arguments ?? {}) as Record<string, unknown>;
  let deviceId: string | undefined;
  if (tool.auth) {
    deviceId = tokenDevice(token);
    if (!deviceId) return { result: toolResult({ ok: false, error: "unauthorized", message: "missing or malformed bearer agent_token; pair with molty_pair first" }) };
  } else {
    deviceId = args.device_id as string;
    if (!DEVICE_ID_RE.test(deviceId)) return { result: toolResult({ ok: false, error: "invalid_args", message: "device_id must look like molty-xxxxxxxxxx" }) };
  }
  try {
    // Ride-along events were already marked read by the backend: whatever a
    // tool does to reshape the outcome (molty_wait builds new ones), the
    // inbox it received must reach the caller.
    // Same for the firmware notice.
    const { api, taken, fwNotices } = capturingInbox(backend.device(deviceId));
    let out = await tool.run(api, token ?? "", args);
    if (taken.length && !out.inbox) out = { ...out, inbox: mergeInbox(taken) };
    if (fwNotices.length && !out.fw_notice) out = { ...out, fw_notice: fwNotices[fwNotices.length - 1] };
    return { result: toolResult(out) };
  } catch (e) {
    return { result: toolResult({ ok: false, error: "internal", message: e instanceof Error ? e.message : String(e) }) };
  }
}

/** Wraps an AgentApi so every inbox and notice any call returns is recorded. */
function capturingInbox(api: AgentApi): { api: AgentApi; taken: Inbox[]; fwNotices: FirmwareNotice[] } {
  const taken: Inbox[] = [];
  const fwNotices: FirmwareNotice[] = [];
  const keep = async <T extends Outcome<any>>(p: Promise<T>) => {
    const out = await p;
    if (out.inbox && out.inbox.events.length) taken.push(out.inbox);
    if (out.fw_notice) fwNotices.push(out.fw_notice);
    return out;
  };
  return {
    taken,
    fwNotices,
    api: {
      pair: (req) => keep(api.pair(req)),
      command: (t, n, a) => keep(api.command(t, n, a)),
      job: (t, j, w) => keep(api.job(t, j, w)),
      revoke: (t) => keep(api.revoke(t)),
      events: (t, r) => keep(api.events(t, r)),
      webhook: (t, r) => keep(api.webhook(t, r)),
      talk: (t, r) => keep(api.talk(t, r)),
      ask: (t, r) => keep(api.ask(t, r)),
      updates: (t) => keep(api.updates(t)),
      update: (t, r) => keep(api.update(t, r)),
      activityLog: (t, r) => keep(api.activityLog(t, r)),
      draft: (t, r) => keep(api.draft(t, r)),
      react: (t, r) => keep(api.react(t, r)),
      notes: (t, r) => keep(api.notes(t, r)),
      phrases: (t, r) => keep(api.phrases(t, r)),
    },
  };
}

function mergeInbox(list: Inbox[]): Inbox {
  return { events: list.flatMap((i) => i.events), more: list.some((i) => i.more), now: Math.max(...list.map((i) => i.now)) };
}

async function handleOne(msg: unknown, backend: RelayBackend, token: string | undefined): Promise<object | undefined> {
  if (!isRecord(msg)) return rpcError(null, -32600, "Invalid Request");
  const m = msg as RpcMessage;
  const isRequest = "id" in m && m.id !== null && m.id !== undefined;
  if (typeof m.method !== "string") {
    // A JSON-RPC response from the client (we never send requests) -- ignore.
    return isRequest || !("result" in m || "error" in m) ? rpcError(m.id, -32600, "Invalid Request") : undefined;
  }
  if (m.jsonrpc !== "2.0") return isRequest ? rpcError(m.id, -32600, "Invalid Request") : undefined;
  if (!isRequest) return undefined; // notifications (initialized, cancelled, ...) need no reply
  if (typeof m.id !== "string" && typeof m.id !== "number") return rpcError(null, -32600, "Invalid request ID");
  switch (m.method) {
    case "initialize": {
      const requested = isRecord(m.params) ? m.params.protocolVersion : undefined;
      if (typeof requested !== "string" || !requested) return rpcError(m.id, -32602, "protocolVersion required");
      const version = SUPPORTED_VERSIONS.includes(requested) ? requested : SUPPORTED_VERSIONS[0];
      return rpcResult(m.id, { protocolVersion: version, capabilities: { tools: { listChanged: false } }, serverInfo: SERVER_INFO, instructions: INSTRUCTIONS });
    }
    case "ping":
      return rpcResult(m.id, {});
    case "tools/list":
      return rpcResult(m.id, { tools: TOOLS.map(({ name, description, inputSchema }) => ({ name, description, inputSchema })) });
    case "tools/call": {
      const out = await callTool(backend, token, m.params);
      return "error" in out && out.error ? rpcError(m.id, out.error[0], out.error[1]) : rpcResult(m.id, out.result);
    }
    default:
      return rpcError(m.id, -32601, "Method not found");
  }
}

const json = (body: unknown, status = 200, extra: Record<string, string> = {}) =>
  new Response(JSON.stringify(body), { status, headers: { "content-type": "application/json", ...extra } });

export async function handleMcp(request: Request, backend: RelayBackend): Promise<Response> {
  if (request.method === "GET" || request.method === "DELETE") {
    // No server-initiated SSE stream and no sessions in this stateless server.
    return new Response("Method Not Allowed", { status: 405, headers: { allow: "POST" } });
  }
  if (request.method !== "POST") return new Response("Method Not Allowed", { status: 405, headers: { allow: "POST" } });
  const version = request.headers.get("mcp-protocol-version");
  if (version && !SUPPORTED_VERSIONS.includes(version)) return json(rpcError(null, -32600, `Unsupported MCP-Protocol-Version ${version}`), 400);
  const text = await request.text();
  if (text.length > MAX_BODY) return json(rpcError(null, -32600, "Request too large"), 413);
  let body: unknown;
  try {
    body = JSON.parse(text);
  } catch {
    return json(rpcError(null, -32700, "Parse error"), 400);
  }
  const token = bearer(request.headers.get("authorization"));
  if (Array.isArray(body)) {
    // Batches were allowed in 2025-03-26 (removed later); accept for older clients.
    if (body.length === 0) return json(rpcError(null, -32600, "Invalid Request"), 400);
    const replies = (await Promise.all(body.map((m) => handleOne(m, backend, token)))).filter(Boolean);
    return replies.length ? json(replies) : new Response(null, { status: 202 });
  }
  const reply = await handleOne(body, backend, token);
  return reply ? json(reply) : new Response(null, { status: 202 });
}
