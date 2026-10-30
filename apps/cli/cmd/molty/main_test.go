package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/molty/cli/internal/molty"
	"github.com/molty/cli/internal/testdevice"
)

func TestCLIJSONAndStdin(t *testing.T) {
	var paths []string
	server := testdevice.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		paths = append(paths, r.URL.Path)
		switch r.URL.Path {
		case "/api/creature":
			fmt.Fprint(w, `{"ok":true,"current":"reader"}`)
		case "/api/creature/state":
			var patch map[string]any
			if err := json.NewDecoder(r.Body).Decode(&patch); err != nil {
				t.Error(err)
			}
			if patch["wpm"] != float64(300) || patch["playing"] != true {
				t.Error(patch)
			}
			fmt.Fprint(w, `{"ok":true,"playing":true}`)
		default:
			fmt.Fprint(w, `{"ok":true}`)
		}
	}))
	defer server.Close()
	t.Setenv("MOLTY_URL", server.URL)
	var out, stderr bytes.Buffer
	if code := run(context.Background(), []string{"show", "--json"}, strings.NewReader("stdin words"), &out, &stderr); code != 0 {
		t.Fatalf("%d %s", code, stderr.String())
	}
	var result map[string]any
	if err := json.Unmarshal(out.Bytes(), &result); err != nil || result["playing"] != true {
		t.Fatalf("%s %v", out.String(), err)
	}
	if len(paths) != 3 {
		t.Fatal(paths)
	}
}
func TestCLIValidation(t *testing.T) {
	for _, args := range [][]string{{"--json", "--unknown"}, {"--json", "--ms", "invalid"}, {"--json", "volume", "22"}, {"--json", "mood", "invalid"}, {"--json", "say"}, {"--json", "status", "extra"}, {"--json", "listen", "--ms", "0"}, {"--json", "imu", "--wav", "some.wav"}, {"--json", "ota"}, {"--json", "show", "--wpm", "99", "text"}} {
		var out, stderr bytes.Buffer
		if code := run(context.Background(), args, strings.NewReader(""), &out, &stderr); code != 1 {
			t.Fatalf("%v: code %d", args, code)
		}
		var value map[string]any
		if err := json.Unmarshal(out.Bytes(), &value); err != nil || value["ok"] != false {
			t.Fatalf("%v: %q %v", args, out.String(), err)
		}
	}
}
func TestCLIHelpAndFlags(t *testing.T) {
	o, err := parse([]string{"--device", "http://localhost", "show", "--wpm=400", "--json", "hello"})
	if err != nil || o.device != "http://localhost" || o.wpm != 400 || !o.json || len(o.args) != 2 {
		t.Fatalf("%+v %v", o, err)
	}
	o, err = parse([]string{"show", "--", "--literal"})
	if err != nil || o.args[1] != "--literal" {
		t.Fatalf("%+v %v", o, err)
	}
	for _, args := range [][]string{{"--bad"}, {"--ms"}, {"--ms", "nope"}} {
		if _, err := parse(args); err == nil {
			t.Fatal(args)
		}
	}
	var out, stderr bytes.Buffer
	if run(context.Background(), []string{"--help"}, nil, &out, &stderr) != 0 || !strings.Contains(out.String(), "mcp") {
		t.Fatal(out.String())
	}
	for _, args := range [][]string{{"--version"}, {"version"}, {"--json", "version"}} {
		out.Reset()
		if run(context.Background(), args, nil, &out, &stderr) != 0 || out.String() != "molty "+molty.Version+"\n" {
			t.Fatalf("%v: %q", args, out.String())
		}
	}
}
func TestCLIDevicePrecedenceAndError(t *testing.T) {
	server := testdevice.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { fmt.Fprint(w, `{"ok":false,"error":"mic_busy"}`) }))
	defer server.Close()
	t.Setenv("MOLTY_URL", "http://unreachable.invalid")
	var out, stderr bytes.Buffer
	code := run(context.Background(), []string{"imu", "--device", server.URL, "--json"}, nil, &out, &stderr)
	if code != 1 || !strings.Contains(out.String(), "mic_busy") {
		t.Fatalf("%d %s %s", code, out.String(), stderr.String())
	}
}

func TestCLIListenOutputForce(t *testing.T) {
	wav := make([]byte, 46)
	copy(wav, "RIFF")
	binary.LittleEndian.PutUint32(wav[4:], 38)
	copy(wav[8:], "WAVEfmt ")
	binary.LittleEndian.PutUint32(wav[16:], 16)
	binary.LittleEndian.PutUint16(wav[20:], 1)
	binary.LittleEndian.PutUint16(wav[22:], 1)
	binary.LittleEndian.PutUint32(wav[24:], 16000)
	binary.LittleEndian.PutUint32(wav[28:], 32000)
	binary.LittleEndian.PutUint16(wav[32:], 2)
	binary.LittleEndian.PutUint16(wav[34:], 16)
	copy(wav[36:], "data")
	binary.LittleEndian.PutUint32(wav[40:], 2)
	server := testdevice.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/api/audio/record":
			w.Header().Set("Content-Type", "audio/wav")
			_, _ = w.Write(wav)
		case "/api/creature":
			fmt.Fprint(w, `{"ok":true,"current":"eye"}`)
		case "/api/creature/state":
			fmt.Fprint(w, `{"ok":true,"pupil_size":0.33}`)
		default:
			t.Errorf("unexpected request: %s", r.URL.Path)
		}
	}))
	defer server.Close()
	for _, tc := range []struct {
		name   string
		force  []string
		suffix string
		exists bool
		want   int
	}{
		{"new", nil, ".wav", false, 0},
		{"protect", nil, ".wav", true, 1},
		{"force", []string{"--force"}, ".wav", true, 0},
		{"false", []string{"--force=false"}, ".wav", true, 1},
		{"suffix", []string{"--force"}, ".txt", true, 1},
	} {
		t.Run(tc.name, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "out"+tc.suffix)
			if tc.exists {
				if err := os.WriteFile(path, []byte("original"), 0600); err != nil {
					t.Fatal(err)
				}
			}
			args := []string{"--device", server.URL, "--json", "listen", "--ms", "1", "--out", path}
			args = append(args, tc.force...)
			var out, stderr bytes.Buffer
			code := run(context.Background(), args, nil, &out, &stderr)
			if code != tc.want {
				t.Fatalf("code %d: %s %s", code, &out, &stderr)
			}
			data, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			if code == 0 && !bytes.Equal(data, wav) {
				t.Fatal("output WAV missing")
			}
			if code != 0 && string(data) != "original" {
				t.Fatal("protected file changed")
			}
		})
	}
	if _, err := parse([]string{"listen", "--force"}); err != nil {
		t.Fatal(err)
	}
	var out, stderr bytes.Buffer
	if run(context.Background(), []string{"--device", server.URL, "--json", "imu", "--force"}, nil, &out, &stderr) != 1 {
		t.Fatal("force accepted for imu")
	}
}

func TestCLIRelayPairAndSavedSelection(t *testing.T) {
	t.Setenv("HOME", t.TempDir())
	for _, key := range []string{"MOLTY_RELAY", "MOLTY_DEVICE", "MOLTY_TOKEN"} {
		t.Setenv(key, "")
	}
	var commands int
	server := testdevice.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/v1/pair" {
			var args map[string]any
			_ = json.NewDecoder(r.Body).Decode(&args)
			if args["link_code"] != "ABCDEFG2" || args["agent_name"] != "CLI" || args["device_id"] != "molty-cli" {
				t.Error(args)
			}
			fmt.Fprint(w, `{"agent_token":"cli-token","device_id":"molty-cli"}`)
			return
		}
		commands++
		if r.URL.Path != "/v1/devices/molty-cli/cmd" || r.Header.Get("Authorization") != "Bearer cli-token" {
			t.Errorf("%s %s", r.URL, r.Header.Get("Authorization"))
		}
		fmt.Fprint(w, `{"ok":true,"fw":"relay-test"}`)
	}))
	defer server.Close()
	var out, stderr bytes.Buffer
	if code := run(context.Background(), []string{"pair", "--relay", server.URL, "--device", "molty-cli", "--code", "ABCDEFG2", "--name", "CLI", "--json"}, nil, &out, &stderr); code != 0 {
		t.Fatalf("%d %s %s", code, &out, &stderr)
	}
	if strings.Contains(out.String(), "cli-token") {
		t.Fatal("token printed")
	}
	stat, err := os.Stat(filepath.Join(os.Getenv("HOME"), ".config/molty/link.json"))
	if err != nil || stat.Mode().Perm() != 0600 {
		t.Fatalf("%v %v", stat, err)
	}
	out.Reset()
	stderr.Reset()
	if code := run(context.Background(), []string{"status", "--json"}, nil, &out, &stderr); code != 0 || !strings.Contains(out.String(), "relay-test") || commands != 1 {
		t.Fatalf("%d %s %s commands=%d", code, &out, &stderr, commands)
	}
}

func TestCLIRelayEventsExec(t *testing.T) {
	if _, err := os.Stat("/bin/sh"); err != nil {
		t.Skip("needs sh")
	}
	calls := 0
	server := testdevice.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls++
		if r.URL.Path != "/v1/devices/molty-k7m2q9xa4c/events" {
			http.NotFound(w, r)
			return
		}
		switch r.Header.Get("Authorization") {
		case "Bearer good":
			fmt.Fprint(w, `{"ok":true,"events":[{"id":"e1","type":"shake","ts":1,"data":{"peak_g":2}},{"id":"e2","type":"tap","ts":2,"data":{"x":0,"y":1}}],"next":"e2"}`)
		default:
			w.WriteHeader(401)
			fmt.Fprint(w, `{"ok":false,"error":"unauthorized"}`)
		}
	}))
	defer server.Close()
	dir := t.TempDir()
	t.Setenv("HOME", dir)
	t.Setenv("MOLTY_RELAY", server.URL)
	t.Setenv("MOLTY_DEVICE", "molty-k7m2q9xa4c")
	t.Setenv("MOLTY_TOKEN", "good")
	log := filepath.Join(dir, "events.log")
	var out, stderr bytes.Buffer
	cmd := `printf '%s ' "$MOLTY_EVENT_TYPE" >> ` + log + `; cat >> ` + log + `; [ "$MOLTY_EVENT_ID" != e2 ]`
	if code := run(context.Background(), []string{"events", "--n", "2", "--exec", cmd}, strings.NewReader(""), &out, &stderr); code != 0 {
		t.Fatalf("%d %s", code, stderr.String())
	}
	lines := strings.Split(strings.TrimSpace(out.String()), "\n")
	if len(lines) != 2 || !strings.Contains(lines[0], `"id":"e1"`) || !strings.Contains(lines[1], `"type":"tap"`) {
		t.Fatalf("stdout %q", out.String())
	}
	data, err := os.ReadFile(log)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(string(data), `shake {"data":{"peak_g":2},"id":"e1"`) || !strings.Contains(string(data), "\ntap {") {
		t.Fatalf("exec log %q", data)
	}
	// A failing --exec command is reported, not fatal.
	if !strings.Contains(stderr.String(), "--exec for e2") {
		t.Fatalf("stderr %q", stderr.String())
	}
	// A rejected token ends the stream with exit 1 (no reconnect loop).
	t.Setenv("MOLTY_TOKEN", "revoked")
	out.Reset()
	stderr.Reset()
	before := calls
	if code := run(context.Background(), []string{"--json", "events"}, strings.NewReader(""), &out, &stderr); code != 1 || calls-before != 1 || !strings.Contains(out.String(), "authentication rejected") {
		t.Fatalf("code %d calls %d out %q", code, calls-before, out.String())
	}
	if code := run(context.Background(), []string{"--json", "status", "--exec", "x"}, strings.NewReader(""), &out, &stderr); code != 1 {
		t.Fatal("--exec outside events must be rejected")
	}
}
