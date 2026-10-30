package molty

import (
	"context"
	"fmt"
	"os"
	"time"
)

type emptyInput struct{}
type creatureInput struct {
	Name string `json:"name,omitempty" jsonschema:"Omit to list current and available creatures; otherwise switch to a registered name (eye, claude, glyph, world, reader, ui)."`
}
type moodInput struct {
	Mood string `json:"mood" jsonschema:"One of idle, listening, thinking, speaking, happy, sleepy, surprised."`
}
type showInput struct {
	Text string `json:"text" jsonschema:"Nonempty UTF-8 text, at most 16384 bytes, to display using the RSVP reader."`
	WPM  *int   `json:"wpm,omitempty" jsonschema:"Words per minute (100..800); default 300."`
}
type sayInput struct {
	Text  string `json:"text,omitempty" jsonschema:"Text to synthesize with the server-configured TTS. Provide exactly one of text or wav."`
	Voice string `json:"voice,omitempty" jsonschema:"Optional relay TTS voice."`
	WAV   string `json:"wav,omitempty" jsonschema:"Path on the server machine to a 16 kHz int16 mono PCM WAV, at most 4 MB; bypasses TTS."`
}
type listenInput struct {
	MS *int `json:"ms,omitempty" jsonschema:"Relay: listen window ceiling in ms, 1..30000 or 0 = unlimited; omit for the device's own setting (30000 out of the box). HTTP: capture duration 1..10000, default 4000."`
}
type volumeInput struct {
	Volume int `json:"volume" jsonschema:"Speaker volume, integer 0..21."`
}

type diagInput struct {
	Timeline int `json:"timeline,omitempty" jsonschema:"Seconds of 1 Hz timeline to include, 0..180 (default 0)."`
}
type logInput struct {
	Since int `json:"since,omitempty" jsonschema:"First log sequence number to return (default 0 = oldest kept)."`
	Max   int `json:"max,omitempty" jsonschema:"Maximum bytes of text, 1024..32768 (default 12288)."`
}

type stateInput struct {
	State Result `json:"state,omitempty" jsonschema:"State patch; omit to read state."`
}
type waitInput struct {
	Job     string `json:"job" jsonschema:"Relay job ID returned by say or listen."`
	Timeout *int   `json:"timeout_s,omitempty" jsonschema:"Polling budget, integer 0..8 seconds; default 8."`
}
type pairInput struct {
	Code string `json:"link_code" jsonschema:"Eight-character link code from the selected device."`
	Name string `json:"agent_name" jsonschema:"Agent name shown on the selected device for approval."`
}

// mcpTools exposes named operations; speech backend commands are startup config.
func (c *Client) mcpTools() map[string]mcpTool {
	s := make(map[string]mcpTool)
	addTool(s, "molty_status", "Relay: return status command result. HTTP: check reachability and merge info, Wi-Fi, creatures and IMU; report partial failures.", func(ctx context.Context, _ emptyInput) (Result, error) { return c.Status(ctx) })
	addTool(s, "molty_creature", "HTTP only: list available/active creatures or switch by name. Changes the physical display; relay v0 has no creature command.", func(ctx context.Context, in creatureInput) (Result, error) { return c.Creature(ctx, in.Name) })
	addTool(s, "molty_mood", "Show idle, listening, thinking, speaking, happy, sleepy or surprised. Relay sends mood; HTTP maps it to creature/visual state. Replaces the current display.", func(ctx context.Context, in moodInput) (Result, error) { return c.Mood(ctx, in.Mood) })
	addTool(s, "molty_show", "Display up to 16 KB of text at WPM (default 300). Relay sends show; HTTP switches to reader, uploads text, restarts and plays. Returns after starting.", func(ctx context.Context, in showInput) (Result, error) {
		wpm := 300
		if in.WPM != nil {
			wpm = *in.WPM
		}
		return c.Show(ctx, in.Text, wpm)
	})
	addTool(s, "molty_say", "Relay: submit text and optional voice; returns job immediately (use molty_wait). HTTP: speak text using server-configured TTS, or play a local WAV. Blocks through playback (up to about 141 seconds); device HTTP is busy meanwhile. Shows speaking mood then restores prior creature/state on success or failure (glyph/ui/world: switch only); restoration failures are restore_warning, not audio errors. WAV must be 16 kHz int16 mono PCM, at most 4 MB.", func(ctx context.Context, in sayInput) (Result, error) {
		return c.Say(ctx, SayOptions{Text: in.Text, WAV: in.WAV, Voice: in.Voice, Async: true})
	})
	addTool(s, "molty_listen", "Relay: submit capture and return job immediately (it ends when the human finishes; the window is only a ceiling); molty_wait returns transcribed text, with no local file or STT. HTTP: record microphone audio (default 4000 ms, maximum 10000). Blocks during capture, shows listening mood, restores previous creature/state (glyph/ui/world: switch only); restoration failures are restore_warning. Always creates a fresh private temporary WAV; no output path argument. Returns local WAV path, duration_ms, normalized peak/RMS (0..1), plus text only if STT configured at startup. mic_busy means a microphone websocket must be closed first. Paths refer to the server machine.", func(ctx context.Context, in listenInput) (Result, error) {
		ms := 4000
		if in.MS != nil {
			ms = *in.MS
		}
		return c.Listen(ctx, ListenOptions{MS: ms, DeviceDefault: in.MS == nil, Async: true})
	})
	addTool(s, "molty_volume", "Set the physical speaker volume from 0 (mute) to 21 (maximum).", func(ctx context.Context, in volumeInput) (Result, error) { return c.Volume(ctx, in.Volume) })
	addTool(s, "molty_diag", "Device health: sim/panel frame-time histograms (fps, p50/p95/p99/max ms), touch-to-panel latency, per-task CPU %, internal/PSRAM heap free and low-water, allocation failures, stall probes, Wi-Fi/gateway ping and link state. timeline (0..180) adds that many seconds of the 1 Hz timeline.", func(ctx context.Context, in diagInput) (Result, error) { return c.Diag(ctx, in.Timeline) })
	addTool(s, "molty_log", "Tail the device log ring (ESP_LOG lines + structured EVT events). Returns {next, text}; pass next back as since. max is 1024..32768 bytes.", func(ctx context.Context, in logInput) (Result, error) { return c.Log(ctx, in.Since, in.Max) })
	addTool(s, "molty_imu", "Read one IMU snapshot: ax/ay/az acceleration in g, gx/gy/gz angular velocity in degrees/second, ms device sample timestamp.", func(ctx context.Context, _ emptyInput) (Result, error) { return c.IMU(ctx) })
	addTool(s, "molty_state", "Read or patch creature state on the selected device.", func(ctx context.Context, in stateInput) (Result, error) { return c.State(ctx, in.State) })
	addTool(s, "molty_wait", "Poll a relay job for at most 8 seconds; returns its result or status pending. Caller cancellation is honored.", func(ctx context.Context, in waitInput) (Result, error) {
		timeout := 8
		if in.Timeout != nil {
			timeout = *in.Timeout
		}
		if timeout < 0 || timeout > 8 {
			return nil, fmt.Errorf("timeout_s must be 0..8")
		}
		return c.Wait(ctx, in.Job, time.Duration(timeout)*time.Second)
	})
	addTool(s, "molty_pair", "Pair the configured relay device using its link code and on-device approval (up to 120s). Returns agent_token; does not save it.", func(ctx context.Context, in pairInput) (Result, error) { return c.Pair(ctx, in.Code, in.Name) })
	return s
}
func (c *Client) RunMCP(ctx context.Context) error { return c.ServeMCP(ctx, os.Stdin, os.Stdout) }
