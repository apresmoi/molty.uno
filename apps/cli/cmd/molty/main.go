// molty drives a Molty device directly, or serves the same verbs over MCP stdio.
package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"os/signal"
	"runtime"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/molty/cli/internal/molty"
)

const usage = `Usage: molty [--device URL|ID] [--relay URL --token T] [--json] <command> [options]
Commands:
  pair [--relay URL] --device ID --code LINKCODE --name NAME   (default relay: molty.DefaultRelay)
                            Wait for on-device approval and save a private link
  status                    Reachability, firmware, Wi-Fi, creature and IMU
  creature [name]           List or switch creature
  mood <mood>               idle|listening|thinking|speaking|happy|sleepy|surprised
  show [text] [--wpm 300]    Display text (stdin when omitted)
  say <text> | --wav PATH    Speak via macOS say or play a WAV
  listen [--ms N] [--out PATH.wav] [--force]      Record WAV (HTTP: N 1..10000, 4000) or transcribe via the relay (N 0..30000, 0 = unlimited; default the device's setting)
  volume <0..21>            Set speaker volume
  imu                       One IMU sample
  diag [--timeline S]       Frame-time histograms, task CPU %, heap, stalls, link (S<=180 s of 1 Hz timeline)
  log [--since N] [--max B] [--follow]   Device log ring (ESP_LOG + EVT events); --follow tails it
  events [--n N] [--exec CMD]  JSON lines; n=0 streams until interrupted. Relay: your
                            Molty events (shake/tap/button) via long-poll, reconnecting;
                            direct HTTP: IMU frames. --exec runs CMD (sh -c) per event
                            with the event JSON on stdin; exits 1 on a rejected token
  mcp                       MCP stdio server (stdout reserved for JSON-RPC)
  version                   Print the CLI version (also --version)
Options may appear before or after the command:
  --device URL|ID           Direct HTTP URL or relay device ID
  --relay URL --token T      Relay connection (env MOLTY_RELAY/DEVICE/TOKEN, saved link)
  --timeout DURATION        Caller deadline (default relay audio 2m; otherwise unlimited)
  --json                    JSON output, including errors
  --force                   Allow listen --out to replace an existing .wav file
  --tts-cmd COMMAND         Trusted shell command: text stdin -> WAV stdout
  --stt-cmd COMMAND         Trusted shell command: WAV stdin -> text stdout
                            (defaults to MOLTY_STT_CMD)
`

type options struct {
	device, relay, token, code, name, tts, stt, wav, out, exec string
	timeout                                                    time.Duration
	json, force, follow                                        bool
	wpm, ms, n, timeline, since, max                           int
	args                                                       []string
	set                                                        map[string]bool
}

// errVersion asks run to print the version (--version).
var errVersion = errors.New("version requested")

// Partition flags first to allow options on either side of the command.
func parse(args []string) (options, error) {
	o := options{set: map[string]bool{}}
	fs := flag.NewFlagSet("molty", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	fs.StringVar(&o.device, "device", "", "device URL or relay device ID")
	fs.StringVar(&o.relay, "relay", "", "relay URL")
	fs.StringVar(&o.token, "token", "", "agent token")
	fs.StringVar(&o.code, "code", "", "pair link code")
	fs.StringVar(&o.name, "name", "", "pair agent name")
	fs.DurationVar(&o.timeout, "timeout", 2*time.Minute, "caller timeout")
	fs.StringVar(&o.tts, "tts-cmd", "", "TTS command")
	fs.StringVar(&o.stt, "stt-cmd", os.Getenv("MOLTY_STT_CMD"), "STT command")
	fs.StringVar(&o.wav, "wav", "", "WAV path")
	fs.StringVar(&o.out, "out", "", "recording path")
	fs.BoolVar(&o.force, "force", false, "replace an existing output WAV")
	fs.BoolVar(&o.json, "json", false, "JSON output")
	fs.IntVar(&o.wpm, "wpm", 300, "reader WPM")
	fs.IntVar(&o.ms, "ms", 4000, "capture milliseconds")
	fs.IntVar(&o.n, "n", 0, "event count")
	fs.IntVar(&o.timeline, "timeline", 0, "diag timeline seconds")
	fs.IntVar(&o.since, "since", 0, "log sequence to start at")
	fs.IntVar(&o.max, "max", 0, "log bytes per read")
	fs.BoolVar(&o.follow, "follow", false, "keep tailing the log")
	fs.StringVar(&o.exec, "exec", "", "command per event")
	for i := 0; i < len(args); i++ {
		arg := args[i]
		if arg == "--" {
			o.args = append(o.args, args[i+1:]...)
			break
		}
		if !strings.HasPrefix(arg, "-") || arg == "-" {
			o.args = append(o.args, arg)
			continue
		}
		name := strings.TrimLeft(strings.SplitN(arg, "=", 2)[0], "-")
		if name == "h" || name == "help" {
			return o, flag.ErrHelp
		}
		if name == "version" {
			return o, errVersion
		}
		f := fs.Lookup(name)
		if f == nil {
			return o, fmt.Errorf("unknown flag %s", arg)
		}
		value := "true"
		if parts := strings.SplitN(arg, "=", 2); len(parts) == 2 {
			value = parts[1]
		} else if name != "json" && name != "force" && name != "follow" {
			i++
			if i == len(args) {
				return o, fmt.Errorf("%s requires a value", arg)
			}
			value = args[i]
		}
		if err := fs.Set(name, value); err != nil {
			return o, err
		}
		o.set[name] = true
	}
	return o, nil
}
func run(ctx context.Context, args []string, stdin io.Reader, stdout, stderr io.Writer) int {
	o, err := parse(args)
	if errors.Is(err, flag.ErrHelp) {
		fmt.Fprint(stdout, usage)
		return 0
	}
	if errors.Is(err, errVersion) || (err == nil && len(o.args) == 1 && o.args[0] == "version") {
		fmt.Fprintf(stdout, "molty %s\n", molty.Version)
		return 0
	}
	if err == nil && len(o.args) == 0 {
		err = fmt.Errorf("command required")
	}
	var result molty.Result
	if err == nil {
		if o.timeout <= 0 {
			err = fmt.Errorf("timeout must be positive")
		}
		if o.set["timeout"] {
			var cancel context.CancelFunc
			ctx, cancel = context.WithTimeout(ctx, o.timeout)
			defer cancel()
		}
		cfg := molty.Config{Device: o.device, Relay: o.relay, Token: o.token, Warn: stderr}
		// pair starts a new link: without --relay or MOLTY_RELAY it uses the default relay.
		if len(o.args) > 0 && o.args[0] == "pair" && cfg.Relay == "" && os.Getenv("MOLTY_RELAY") == "" {
			cfg.Relay = molty.DefaultRelay
		}
		if o.tts != "" {
			cfg.TTS = molty.CommandTTS(o.tts)
		}
		if o.stt != "" {
			cfg.STT = molty.CommandSTT(o.stt)
		}
		var c *molty.Client
		if err == nil {
			c, err = molty.New(cfg)
		}
		if err == nil {
			command := o.args[0]
			pos := o.args[1:]
			allowed := map[string]string{"wpm": "show", "wav": "say", "ms": "listen", "out": "listen", "force": "listen", "n": "events", "exec": "events", "code": "pair", "name": "pair", "timeline": "diag", "since": "log", "max": "log", "follow": "log"}
			for option, verb := range allowed {
				if o.set[option] && command != verb {
					err = fmt.Errorf("--%s is only valid for %s", option, verb)
					break
				}
			}
			if err == nil {
				noArgs := func() bool {
					if len(pos) != 0 {
						err = fmt.Errorf("%s takes no positional arguments", command)
						return false
					}
					return true
				}
				switch command {
				case "pair":
					if noArgs() {
						result, err = c.PairAndSave(ctx, o.code, o.name)
					}
				case "mcp":
					if noArgs() {
						err = c.RunMCP(ctx)
						if err == nil {
							return 0
						}
					}
				case "status":
					if noArgs() {
						result, err = c.Status(ctx)
					}
				case "imu":
					if noArgs() {
						result, err = c.IMU(ctx)
					}
				case "diag":
					if noArgs() {
						result, err = c.Diag(ctx, o.timeline)
					}
				case "log":
					if noArgs() {
						since := o.since
						for {
							result, err = c.Log(ctx, since, o.max)
							if err != nil || !o.follow {
								break
							}
							if o.json {
								_ = json.NewEncoder(stdout).Encode(result)
							} else {
								fmt.Fprint(stdout, result["text"])
							}
							if n, ok := result["next"].(float64); ok {
								since = int(n)
							} else if n, ok := result["next"].(int); ok {
								since = n
							}
							select {
							case <-ctx.Done():
								return 0
							case <-time.After(time.Second):
							}
						}
						if err == nil && !o.json {
							fmt.Fprint(stdout, result["text"])
							return 0
						}
					}
				case "creature":
					if len(pos) > 1 {
						err = fmt.Errorf("creature takes at most one name")
					} else {
						name := ""
						if len(pos) == 1 {
							name = pos[0]
						}
						result, err = c.Creature(ctx, name)
					}
				case "mood":
					if len(pos) != 1 {
						err = fmt.Errorf("mood requires one mood")
					} else {
						result, err = c.Mood(ctx, pos[0])
					}
				case "volume":
					if len(pos) != 1 {
						err = fmt.Errorf("volume requires one integer")
					} else {
						var v int
						v, err = strconv.Atoi(pos[0])
						if err == nil {
							result, err = c.Volume(ctx, v)
						}
					}
				case "show":
					text := strings.Join(pos, " ")
					if len(pos) == 0 || (len(pos) == 1 && pos[0] == "-") {
						var data []byte
						data, err = io.ReadAll(io.LimitReader(stdin, 16*1024+1))
						text = string(data)
					}
					if err == nil {
						result, err = c.Show(ctx, text, o.wpm)
					}
				case "say":
					result, err = c.Say(ctx, molty.SayOptions{Text: strings.Join(pos, " "), WAV: o.wav})
				case "listen":
					if noArgs() {
						result, err = c.Listen(ctx, molty.ListenOptions{MS: o.ms, DeviceDefault: !o.set["ms"], Out: o.out, Force: o.force})
					}
				case "events":
					if noArgs() {
						err = c.Events(ctx, o.n, func(frame json.RawMessage) error {
							if err := json.NewEncoder(stdout).Encode(frame); err != nil {
								return err
							}
							if o.exec != "" {
								runExec(ctx, o.exec, frame, stderr)
							}
							return nil
						})
						if err == nil {
							return 0
						}
					}
				default:
					err = fmt.Errorf("unknown command %q", command)
				}
			}
		}
	}
	if err != nil {
		if result == nil {
			result = molty.Result{}
		}
		result["ok"] = false
		result["error"] = err.Error()
		if o.json && (len(o.args) == 0 || o.args[0] != "mcp") {
			_ = json.NewEncoder(stdout).Encode(result)
		} else {
			fmt.Fprintln(stderr, "molty:", err)
		}
		return 1
	}
	encoder := json.NewEncoder(stdout)
	if !o.json {
		encoder.SetIndent("", "  ")
	}
	if err = encoder.Encode(result); err != nil {
		fmt.Fprintln(stderr, err)
		return 1
	}
	return 0
}

// runExec runs the --exec command for one event: the event JSON on stdin,
// MOLTY_EVENT_ID / MOLTY_EVENT_TYPE in the environment, its output on our
// stderr (stdout stays pure JSON lines). A failing command is reported, not fatal.
func runExec(ctx context.Context, command string, frame json.RawMessage, stderr io.Writer) {
	var ev struct{ ID, Type string }
	_ = json.Unmarshal(frame, &ev)
	shell, flag := "sh", "-c"
	if runtime.GOOS == "windows" {
		shell, flag = "cmd", "/C"
	}
	cmd := exec.CommandContext(ctx, shell, flag, command)
	cmd.Stdin = bytes.NewReader(append(append([]byte{}, frame...), '\n'))
	cmd.Stdout, cmd.Stderr = stderr, stderr
	cmd.Env = append(os.Environ(), "MOLTY_EVENT_ID="+ev.ID, "MOLTY_EVENT_TYPE="+ev.Type)
	if err := cmd.Run(); err != nil && ctx.Err() == nil {
		fmt.Fprintf(stderr, "molty: --exec for %s: %v\n", ev.ID, err)
	}
}

func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	os.Exit(run(ctx, os.Args[1:], os.Stdin, os.Stdout, os.Stderr))
}
