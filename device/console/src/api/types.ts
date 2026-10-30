// Wire shapes of the device's HTTP API (device/firmware/main/net/http.c
// + device/creatures/mole_fluid.c). Only the fields this console reads are typed strictly;
// everything else stays open so firmware can add fields without breaking us.

export interface DeviceInfo {
  ok: true;
  chip: string;
  mac: string;
  flash_mb: number;
  psram_mb: number;
  free_heap: number;
  free_psram: number;
  uptime_ms: number;
  reset_reason?: string;
  boot_count?: number;
  idf_version: string;
  fw_build: string;
  fw?: string;
  /** board.json "name", e.g. waveshare-esp32-s3-touch-lcd-1.46 (release assets are named after it). */
  board?: string;
  [k: string]: unknown;
}

/** GET /api/update (firmware that can update itself; older firmware: UnsupportedError). */
export interface UpdateState {
  ok: true;
  state: 'idle' | 'downloading' | 'flashing' | 'rebooting' | 'failed';
  version?: string;
  progress?: number;
  error?: string;
}

/** POST /api/ota result (the app image is written to the spare slot, then the device reboots). */
export interface OtaResult { ok: true; written: number; slot: string; reboot: boolean }

export type WifiState = 'setup_needed' | 'connecting' | 'connected' | 'disconnected' | 'unknown';

export interface WifiStatus {
  ok: true;
  state: WifiState;
  ssid?: string;
  ip?: string;
  rssi?: number;
}

export interface WifiNetwork {
  ssid: string;
  rssi: number;
  open: boolean;
}

export interface WifiConnectResult {
  ok: boolean;
  ip?: string;
  rssi?: number;
  error?: string;
  wifi_status?: number;
}

export interface CreatureRegistry {
  ok: true;
  current: string;
  available: string[];
}

/** Fluid creature state (GET/POST /api/creature/state while current == "fluid"). */
export interface FluidState {
  ok: true;
  creature: string;
  state: string; // blob | thinking | text | creature | effect:<name>
  autoplay?: boolean;
  text?: string;
  wpm?: number;
  palette?: string;
  /** The listening ring's default colour: "#rrggbb" or "accent" (newer firmware). */
  ring_color?: string;
  /** The listen window an agent gets when it gives none: 0 = unlimited or 5000..30000 ms (newer firmware). */
  listen_ms?: number;
  /** What Molty does itself when you finish speaking: "chirp" | "thinking" | "off" (newer firmware). */
  ack?: string;
  /** Whether a shake makes Molty start listening by itself (newer firmware). */
  shake_talk?: boolean;
  /** How much Molty moves by itself when nobody drives it: "lively" | "calm" | "off" (newer firmware). */
  idle_life?: string;
  word?: string;
  word_idx?: number;
  nwords?: number;
  fps?: number;
  [k: string]: unknown;
}

/** Partial order accepted by POST /api/creature/state for the fluid creature. */
export interface FluidOrder {
  state?: string; // blob | thinking | text | creature | autoplay | effect:ripple|vortex|wave
  text?: string;
  wpm?: number; // 100..800
  palette?: string; // green | amber | pastel
  ring_color?: string; // "#rrggbb" | "accent" — the listening ring's default, kept on the device
  listen_ms?: number; // 0 (unlimited) | 5000..30000 — the default listen window, kept on the device
  ack?: string; // "chirp" | "thinking" | "off" — the turn acknowledgement, kept on the device
  shake_talk?: boolean; // a shake starts a listen by itself (shake to talk), kept on the device
  idle_life?: string; // "lively" | "calm" | "off" — small idle actions when nobody drives it, kept on the device
  ripple?: { x: number; y: number; strength?: number }; // x,y in -1..1, y down
}

/** GET /api/link and POST /api/link {relay_url} (docs/AGENT-LINK.md "REST envelopes").
 *  relay_url is the ws:// or wss:// origin the device dials; agents use its http(s) twin. */
export interface LinkInfo {
  device_id: string;
  link_code: string;
  relay_url: string;
  connected: boolean;
  auth_failed?: boolean;
  close_code?: number;
  /** A pairing request waiting for a tap on Molty's screen (newer firmware). */
  pair?: PairRequest | null;
  /** The last decided pairing request; seq increments per decision (newer firmware). */
  last_pair?: PairResult | null;
}

export interface PairRequest { agent_name: string; code: string; expires_in_s?: number }
export interface PairResult { seq: number; agent_name: string; ok: boolean; reason?: string }

export interface Frame {
  width: number;
  height: number;
  rgb: Uint8Array; // packed RGB888, width*height*3
}

/** GET /api/diag (firmware mole_diag.c). Times in ms. */
export interface FrameStats {
  n: number; fps?: number; p50_ms?: number; p95_ms?: number; p99_ms?: number; max_ms?: number; mean_ms?: number;
  hist?: Array<{ le: string; n: number }>;
  total?: number; over50_total?: number; over100_total?: number; over250_total?: number; max_total_ms?: number; max_total_at_s?: number;
  work_ms?: number; draw_ms?: number; flush_ms?: number;
}
export interface HeapStats { free: number; min_free: number; largest: number }
export interface Diag {
  uptime_ms: number; reset_reason: string; fw: string; coredump?: string;
  frames: { sim: FrameStats; panel: FrameStats; touch_to_panel: FrameStats };
  heap: { internal: HeapStats; dma: HeapStats; psram: HeapStats; alloc_failures: number; last_fail_size: number; last_fail_caps: number };
  stalls: { probe_core0_max_ms: number; probe_core1_max_ms: number; sampler_late_max_ms: number; gap_events: number };
  net: { rssi: number; wifi_ps: string; gw_ping_sent: number; gw_ping_lost: number; gw_rtt_last_ms: number; link_connected: boolean; audio_busy: boolean };
  tasks: Array<{ name: string; cpu: number; core: number; prio: number; stack_free: number }>;
  log: { next_seq: number; prev_source: string; persists: number };
  timeline?: Array<Array<number | string>>;
  timeline_cols?: string;
}
