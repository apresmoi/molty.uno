package molty

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/molty/cli/internal/testdevice"
)

func relayClient(t *testing.T, handler http.HandlerFunc) *Client {
	t.Helper()
	server := testdevice.NewServer(handler)
	t.Cleanup(server.Close)
	c, err := New(Config{Relay: server.URL, Device: "molty-test", Token: "secret", HTTPClient: server.Client(), TTS: forbiddenSpeech{t}, STT: forbiddenSpeech{t}})
	if err != nil {
		t.Fatal(err)
	}
	return c
}

type forbiddenSpeech struct{ t *testing.T }

func (f forbiddenSpeech) Synthesize(context.Context, string) ([]byte, error) {
	f.t.Error("relay invoked local TTS")
	return nil, fmt.Errorf("local TTS")
}
func (f forbiddenSpeech) Transcribe(context.Context, []byte) (string, error) {
	f.t.Error("relay invoked local STT")
	return "", fmt.Errorf("local STT")
}

func TestRelayRoutingAndAudio(t *testing.T) {
	var calls []string
	var polls atomic.Int32
	c := relayClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer secret" {
			t.Error("missing bearer")
		}
		switch {
		case r.URL.Path == "/v1/devices/molty-test/cmd" && r.Method == "POST":
			var cmd struct {
				Name string
				Args Result
			}
			if err := json.NewDecoder(r.Body).Decode(&cmd); err != nil {
				t.Error(err)
			}
			calls = append(calls, cmd.Name)
			want := map[string]Result{"status": {}, "imu": {}, "state": {"state": "happy"}, "mood": {"mood": "happy"}, "show": {"text": "words", "wpm": float64(320)}, "volume": {"volume": float64(9)}, "say": {"text": "hello", "voice": "test"}, "listen": {"ms": float64(4000)}}[cmd.Name]
			gotJSON, _ := json.Marshal(cmd.Args)
			wantJSON, _ := json.Marshal(want)
			if string(gotJSON) != string(wantJSON) {
				t.Errorf("%s args = %s, want %s", cmd.Name, gotJSON, wantJSON)
			}
			if cmd.Name == "say" || cmd.Name == "listen" {
				fmt.Fprint(w, `{"ok":true,"result":{"job":"j1"}}`)
			} else {
				fmt.Fprint(w, `{"ok":true,"result":{"accepted":true}}`)
			}
		case r.URL.Path == "/v1/jobs/j1" && r.Method == "GET":
			if polls.Add(1)%2 == 1 {
				fmt.Fprint(w, `{"status":"running"}`)
			} else {
				fmt.Fprint(w, `{"status":"done","result":{"text":"transcribed"}}`)
			}
		default:
			t.Errorf("unexpected route %s %s", r.Method, r.URL)
			http.NotFound(w, r)
		}
	})
	ctx := context.Background()
	actions := []func() (Result, error){func() (Result, error) { return c.Status(ctx) }, func() (Result, error) { return c.IMU(ctx) }, func() (Result, error) { return c.State(ctx, Result{"state": "happy"}) }, func() (Result, error) { return c.Mood(ctx, "happy") }, func() (Result, error) { return c.Show(ctx, "words", 320) }, func() (Result, error) { return c.Volume(ctx, 9) }}
	for _, action := range actions {
		if _, err := action(); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := c.Say(ctx, SayOptions{Text: "hello", Voice: "test"}); err != nil {
		t.Fatal(err)
	}
	out, err := c.Listen(ctx, ListenOptions{MS: 4000})
	if err != nil || out["text"] != "transcribed" || out["path"] != nil {
		t.Fatalf("%v %v", out, err)
	}
	if strings.Join(calls, ",") != "status,imu,state,mood,show,volume,say,listen" || polls.Load() != 4 {
		t.Fatalf("%v polls=%d", calls, polls.Load())
	}
	if _, err := c.Creature(ctx, "eye"); err == nil {
		t.Fatal("unsupported command sent")
	}
	if _, err := c.Say(ctx, SayOptions{WAV: "anything.wav"}); err == nil {
		t.Fatal("relay accepted WAV")
	}
	if _, err := c.Listen(ctx, ListenOptions{MS: 4000, Out: "file.wav"}); err == nil {
		t.Fatal("relay accepted local output")
	}
}

func TestRelayPairApprovalAndSave(t *testing.T) {
	t.Setenv("HOME", t.TempDir())
	entered, approve := make(chan struct{}), make(chan struct{})
	server := testdevice.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != "POST" || r.URL.Path != "/v1/pair" || r.Header.Get("Authorization") != "" {
			t.Error("pair route/auth")
		}
		var args Result
		_ = json.NewDecoder(r.Body).Decode(&args)
		if args["device_id"] != "molty-test" || args["link_code"] != "ABCDEFG2" || args["agent_name"] != "Tester" {
			t.Error(args)
		}
		close(entered)
		<-approve
		fmt.Fprint(w, `{"device_id":"molty-test","agent_token":"approved"}`)
	}))
	defer server.Close()
	relay, err := NewRelayTransport(server.URL, "molty-test", "", server.Client())
	if err != nil {
		t.Fatal(err)
	}
	c := &Client{relay}
	done := make(chan error, 1)
	go func() {
		out, err := c.PairAndSave(context.Background(), "ABCDEFG2", "Tester")
		if err == nil && out["agent_token"] != nil {
			err = fmt.Errorf("pair output leaked token")
		}
		done <- err
	}()
	<-entered
	path, _ := linkPath()
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Fatal("token saved before approval")
	}
	close(approve)
	if err := <-done; err != nil {
		t.Fatal(err)
	}
	stat, err := os.Stat(path)
	if err != nil || stat.Mode().Perm() != 0600 {
		t.Fatalf("private link: %v %v", stat, err)
	}
	cfg, err := resolveConfig(Config{})
	if err != nil || cfg.Token != "approved" || cfg.Relay != server.URL || cfg.Device != "molty-test" {
		t.Fatalf("%+v %v", cfg, err)
	}
	// Replacing an existing permissive file still yields a private inode.
	if err := os.Chmod(path, 0644); err != nil {
		t.Fatal(err)
	}
	if err := SaveLink(Link{server.URL, "molty-test", "new"}); err != nil {
		t.Fatal(err)
	}
	stat, _ = os.Stat(path)
	if stat.Mode().Perm() != 0600 {
		t.Fatal(stat.Mode())
	}
}

func TestRelayConfigPrecedence(t *testing.T) {
	t.Setenv("HOME", t.TempDir())
	for _, key := range []string{"MOLTY_RELAY", "MOLTY_DEVICE", "MOLTY_TOKEN"} {
		t.Setenv(key, "")
	}
	if err := SaveLink(Link{"https://saved.invalid", "saved", "saved-token"}); err != nil {
		t.Fatal(err)
	}
	t.Setenv("MOLTY_RELAY", "https://env.invalid")
	t.Setenv("MOLTY_DEVICE", "env")
	t.Setenv("MOLTY_TOKEN", "env-token")
	cfg, err := resolveConfig(Config{Relay: "https://flag.invalid", Device: "flag", Token: "flag-token"})
	if err != nil || cfg.Relay != "https://flag.invalid" || cfg.Device != "flag" || cfg.Token != "flag-token" {
		t.Fatalf("%+v %v", cfg, err)
	}
	cfg, err = resolveConfig(Config{})
	if err != nil || cfg.Relay != "https://env.invalid" || cfg.Device != "env" || cfg.Token != "env-token" {
		t.Fatalf("%+v %v", cfg, err)
	}
	t.Setenv("MOLTY_TOKEN", "")
	cfg, err = resolveConfig(Config{})
	if err != nil || cfg.Token != "saved-token" {
		t.Fatalf("%+v %v", cfg, err)
	}
	c, err := New(Config{Device: "http://direct.invalid"})
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := c.Transport.(*HTTPTransport); !ok {
		t.Fatal("explicit direct URL lost")
	}
	if err := os.Remove(filepath.Join(os.Getenv("HOME"), ".config/molty/link.json")); err != nil {
		t.Fatal(err)
	}
	for _, key := range []string{"MOLTY_RELAY", "MOLTY_DEVICE", "MOLTY_TOKEN"} {
		t.Setenv(key, "")
	}
	c, err = New(Config{})
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := c.Transport.(*HTTPTransport); !ok {
		t.Fatal("direct fallback lost")
	}
}

func TestRelayWaitTimeoutAndCancellation(t *testing.T) {
	c := relayClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == "POST" {
			fmt.Fprint(w, `{"job":"slow"}`)
		} else {
			fmt.Fprint(w, `{"status":"pending"}`)
		}
	})
	started := time.Now()
	out, err := c.Wait(context.Background(), "slow", 30*time.Millisecond)
	if err != nil || out["status"] != "pending" || time.Since(started) > time.Second {
		t.Fatalf("%v %v", out, err)
	}
	for _, verb := range []string{"wait", "say", "listen", "pair"} {
		ctx, cancel := context.WithTimeout(context.Background(), 20*time.Millisecond)
		switch verb {
		case "wait":
			_, err = c.Wait(ctx, "slow", 8*time.Second)
		case "say":
			out, err = c.Say(ctx, SayOptions{Text: "hello"})
		case "listen":
			out, err = c.Listen(ctx, ListenOptions{MS: 4000})
		case "pair":
			_, err = c.Pair(ctx, "ABCDEFG2", "Tester") // Wrong response must fail, not save.
		}
		cancel()
		if err == nil {
			t.Fatalf("%s accepted incomplete operation", verb)
		}
		if verb != "pair" && !errors.Is(err, context.DeadlineExceeded) {
			t.Fatalf("%s: %v", verb, err)
		}
	}
	if _, err := c.Wait(context.Background(), "slow", 9*time.Second); err == nil {
		t.Fatal("accepted >8 seconds")
	}
}

func TestRelayAuthAndJobErrors(t *testing.T) {
	for _, tc := range []struct {
		name   string
		status int
		body   string
		want   string
	}{
		{"unauthorized", 401, `{"error":"invalid_token"}`, "authentication"},
		{"wire auth", 200, `{"code":4401,"error":"unauthorized"}`, "4401"},
		{"failed", 200, `{"status":"failed","error":"voice unavailable"}`, "voice unavailable"},
		{"malformed", 200, `{"wat":true}`, "unknown status"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			c := relayClient(t, func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(tc.status); fmt.Fprint(w, tc.body) })
			_, err := c.Wait(context.Background(), "j", time.Second)
			if err == nil || !strings.Contains(err.Error(), tc.want) {
				t.Fatalf("%v", err)
			}
		})
	}
}

func TestRelayMCPAsyncAndWait(t *testing.T) {
	var polls atomic.Int32
	c := relayClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == "POST" {
			fmt.Fprint(w, `{"job":"mcp-job"}`)
		} else {
			polls.Add(1)
			fmt.Fprint(w, `{"status":"done","result":{"text":"heard"}}`)
		}
	})
	m := startMCP(t, c)
	initializeMCP(t, m)
	for i, name := range []string{"molty_say", "molty_listen"} {
		args := Result{"text": "hello"}
		if name == "molty_listen" {
			args = Result{"ms": 10000}
		}
		out := toolResult(t, m.request(t, i+2, "tools/call", Result{"name": name, "arguments": args}))
		if out["isError"] == true || out["structuredContent"].(map[string]any)["job"] != "mcp-job" || polls.Load() != 0 {
			t.Fatal(out)
		}
	}
	out := toolResult(t, m.request(t, 4, "tools/call", Result{"name": "molty_wait", "arguments": Result{"job": "mcp-job", "timeout_s": 8}}))
	if out["isError"] == true || out["structuredContent"].(map[string]any)["text"] != "heard" || polls.Load() != 1 {
		t.Fatal(out)
	}
	out = toolResult(t, m.request(t, 5, "tools/call", Result{"name": "molty_wait", "arguments": Result{"job": "mcp-job", "timeout_s": 9}}))
	if out["isError"] != true {
		t.Fatal("MCP accepted >8s", out)
	}
}

func TestRelayPairRejectionAndTimeoutDoNotSave(t *testing.T) {
	for _, mode := range []string{"reject", "timeout", "mismatch"} {
		t.Run(mode, func(t *testing.T) {
			t.Setenv("HOME", t.TempDir())
			c := relayClient(t, func(w http.ResponseWriter, r *http.Request) {
				switch mode {
				case "reject":
					fmt.Fprint(w, `{"ok":false,"error":"pair_rejected"}`)
				case "mismatch":
					fmt.Fprint(w, `{"agent_token":"secret","device_id":"other-device"}`)
				case "timeout":
					select {
					case <-r.Context().Done():
					case <-time.After(100 * time.Millisecond):
					}
				}
			})
			ctx, cancel := context.WithTimeout(context.Background(), 30*time.Millisecond)
			defer cancel()
			_, err := c.PairAndSave(ctx, "ABCDEFG2", "Tester")
			if err == nil {
				t.Fatal("pair unexpectedly succeeded")
			}
			if mode == "timeout" && !errors.Is(err, context.DeadlineExceeded) {
				t.Fatal(err)
			}
			path, _ := linkPath()
			if _, err := os.Stat(path); !os.IsNotExist(err) {
				t.Fatal("unsuccessful pair saved link")
			}
		})
	}
}

func TestRelayRequestBudgetsAndRedirects(t *testing.T) {
	var paired atomic.Bool
	c := relayClient(t, func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "http://must-not-follow.invalid", http.StatusTemporaryRedirect)
	})
	// Inspect actual client contexts without opening sockets; still delegate all
	// successful calls through the httptest HTTP stack in the other tests.
	relay := c.Transport.(*RelayTransport)
	relay.http.Transport = clientRoundTrip(func(req *http.Request) (*http.Response, error) {
		deadline, ok := req.Context().Deadline()
		upper := 8 * time.Second
		if req.URL.Path == "/v1/pair" {
			upper = 120 * time.Second
			paired.Store(true)
		}
		if !ok || time.Until(deadline) > upper || time.Until(deadline) < upper-time.Second {
			t.Errorf("request budget %v (max %v)", time.Until(deadline), upper)
		}
		return nil, fmt.Errorf("budget sentinel")
	})
	_, _ = c.Pair(context.Background(), "ABCDEFG2", "Tester")
	_, _ = c.Wait(context.Background(), "job", 8*time.Second)
	_, _ = c.Status(context.Background())
	if !paired.Load() {
		t.Fatal("pair not attempted")
	}
	relay.http.Transport = http.DefaultTransport
	if _, err := c.Status(context.Background()); err == nil || !strings.Contains(err.Error(), "307") {
		t.Fatalf("redirect not rejected: %v", err)
	}
	relay.token = ""
	if _, err := c.Status(context.Background()); err == nil || !strings.Contains(err.Error(), "authentication") {
		t.Fatalf("missing token: %v", err)
	}
}
