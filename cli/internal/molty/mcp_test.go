package molty

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/molty/cli/internal/testdevice"
)

type testMCP struct {
	in  *io.PipeWriter
	out *json.Decoder
}

func startMCP(t *testing.T, c *Client) *testMCP {
	t.Helper()
	inR, inW := io.Pipe()
	outR, outW := io.Pipe()
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- c.ServeMCP(ctx, inR, outW); outW.Close() }()
	t.Cleanup(func() {
		cancel()
		inW.Close()
		outR.Close()
		select {
		case err := <-done:
			if err != nil && !strings.Contains(err.Error(), "closed pipe") {
				t.Error(err)
			}
		case <-time.After(3 * time.Second):
			t.Error("MCP did not shut down")
		}
	})
	return &testMCP{in: inW, out: json.NewDecoder(outR)}
}
func (m *testMCP) send(t *testing.T, value any) {
	t.Helper()
	if err := json.NewEncoder(m.in).Encode(value); err != nil {
		t.Fatal(err)
	}
}
func (m *testMCP) read(t *testing.T) Result {
	t.Helper()
	type response struct {
		value Result
		err   error
	}
	ch := make(chan response, 1)
	go func() { var value Result; err := m.out.Decode(&value); ch <- response{value, err} }()
	select {
	case r := <-ch:
		if r.err != nil {
			t.Fatal(r.err)
		}
		return r.value
	case <-time.After(3 * time.Second):
		t.Fatal("MCP response timed out")
		return nil
	}
}
func (m *testMCP) request(t *testing.T, id int, method string, params any) Result {
	m.send(t, Result{"jsonrpc": "2.0", "id": id, "method": method, "params": params})
	return m.read(t)
}
func initializeMCP(t *testing.T, m *testMCP) {
	t.Helper()
	r := m.request(t, 1, "initialize", Result{"protocolVersion": "2025-11-25", "capabilities": Result{}, "clientInfo": Result{"name": "test", "version": "1"}})
	if r["error"] != nil {
		t.Fatal(r)
	}
	m.send(t, Result{"jsonrpc": "2.0", "method": "notifications/initialized"})
}
func toolResult(t *testing.T, r Result) Result {
	t.Helper()
	if r["error"] != nil {
		t.Fatal(r)
	}
	return r["result"].(map[string]any)
}

func TestMCPProtocolAndTools(t *testing.T) {
	f := newFake(t)
	c := newTestClient(t, f, Config{})
	m := startMCP(t, c)
	if r := m.request(t, 0, "tools/list", Result{}); r["error"] == nil {
		t.Fatal("accepted before initialize")
	}
	initializeMCP(t, m)
	r := toolResult(t, m.request(t, 2, "tools/list", Result{}))
	list := r["tools"].([]any)
	if len(list) != 13 {
		t.Fatalf("tool count %d", len(list))
	}
	expected := map[string]bool{"molty_wait": true, "molty_state": true, "molty_pair": true, "molty_status": true, "molty_creature": true, "molty_mood": true, "molty_show": true, "molty_say": true, "molty_listen": true, "molty_volume": true, "molty_imu": true, "molty_diag": true, "molty_log": true}
	for _, entry := range list {
		tool := entry.(map[string]any)
		name := tool["name"].(string)
		if !expected[name] {
			t.Fatal(name)
		}
		delete(expected, name)
		if len(tool["description"].(string)) < 40 {
			t.Fatal("short description")
		}
		schema := tool["inputSchema"].(map[string]any)
		if schema["type"] != "object" || schema["additionalProperties"] != false {
			t.Fatal(schema)
		}
	}
	for i, call := range []struct {
		name string
		args Result
	}{{"molty_status", Result{}}, {"molty_creature", Result{}}, {"molty_mood", Result{"mood": "happy"}}, {"molty_show", Result{"text": "MCP text"}}, {"molty_volume", Result{"volume": 0}}, {"molty_imu", Result{}}} {
		r := toolResult(t, m.request(t, 10+i, "tools/call", Result{"name": call.name, "arguments": call.args}))
		if r["isError"] != false || r["structuredContent"] == nil || r["content"] == nil {
			t.Fatal(r)
		}
	}
	f.mu.Lock()
	if f.states["reader"]["wpm"] != float64(300) {
		t.Error("MCP reader default not applied")
	}
	f.mu.Unlock()
	for i, args := range []any{Result{}, Result{"volume": 22}, Result{"volume": nil}, Result{"volume": "4"}, Result{"volume": 4, "extra": true}, []any{}} {
		r := toolResult(t, m.request(t, 30+i, "tools/call", Result{"name": "molty_volume", "arguments": args}))
		if r["isError"] != true {
			t.Fatalf("bad input succeeded: %v", args)
		}
	}
	if r := m.request(t, 40, "tools/call", Result{"name": "molty_reboot", "arguments": Result{}}); r["error"] == nil {
		t.Fatal("dangerous unknown tool accepted")
	}
	if r := m.request(t, 41, "unknown", Result{}); r["error"] == nil {
		t.Fatal("unknown method accepted")
	}
	f.mu.Lock()
	f.failures["/api/imu/snapshot"] = "wrong_creature"
	f.mu.Unlock()
	r = toolResult(t, m.request(t, 42, "tools/call", Result{"name": "molty_imu"}))
	if r["isError"] != true || !strings.Contains(r["structuredContent"].(map[string]any)["error"].(string), "wrong_creature") {
		t.Fatal(r)
	}
}
func TestMCPCancellationAndConcurrentPing(t *testing.T) {
	started := make(chan struct{})
	server := testdevice.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { close(started); <-r.Context().Done() }))
	defer server.Close()
	c, err := New(Config{Device: server.URL})
	if err != nil {
		t.Fatal(err)
	}
	m := startMCP(t, c)
	initializeMCP(t, m)
	m.send(t, Result{"jsonrpc": "2.0", "id": "long", "method": "tools/call", "params": Result{"name": "molty_imu"}})
	select {
	case <-started:
	case <-time.After(3 * time.Second):
		t.Fatal("tool never reached fake device")
	}
	if r := m.request(t, 2, "ping", Result{}); r["id"] != float64(2) {
		t.Fatal(r)
	}
	m.send(t, Result{"jsonrpc": "2.0", "method": "notifications/cancelled", "params": Result{"requestId": "long", "reason": "test"}})
	r := m.read(t)
	if r["id"] != "long" || toolResult(t, r)["isError"] != true {
		t.Fatal(r)
	}
}
func TestMCPMalformedRequests(t *testing.T) {
	c, err := New(Config{Device: "http://unused.invalid"})
	if err != nil {
		t.Fatal(err)
	}
	m := startMCP(t, c)
	for _, raw := range []string{`{`, `[]`, `{"jsonrpc":"1.0","method":"ping","id":1}`, `{"jsonrpc":"2.0","method":"ping","id":null}`} {
		if _, err := fmt.Fprintln(m.in, raw); err != nil {
			t.Fatal(err)
		}
		if r := m.read(t); r["error"] == nil {
			t.Fatal(r)
		}
	}
}

func TestMCPStopsWhenContextCanceled(t *testing.T) {
	c, err := New(Config{Device: "http://unused.invalid"})
	if err != nil {
		t.Fatal(err)
	}
	input, writer := io.Pipe()
	defer writer.Close()
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- c.ServeMCP(ctx, input, io.Discard) }()
	cancel()
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("canceled MCP blocked on stdin")
	}
}

func TestMCPListenCannotChooseOutput(t *testing.T) {
	f := newFake(t)
	c := newTestClient(t, f, Config{})
	m := startMCP(t, c)
	initializeMCP(t, m)
	list := toolResult(t, m.request(t, 2, "tools/list", Result{}))["tools"].([]any)
	for _, entry := range list {
		tool := entry.(map[string]any)
		if tool["name"] == "molty_listen" {
			properties := tool["inputSchema"].(map[string]any)["properties"].(map[string]any)
			if properties["out"] != nil {
				t.Error("MCP exposes arbitrary output path")
			}
		}
	}
	path := filepath.Join(t.TempDir(), "precious.wav")
	if err := os.WriteFile(path, []byte("original"), 0600); err != nil {
		t.Fatal(err)
	}
	r := toolResult(t, m.request(t, 3, "tools/call", Result{"name": "molty_listen", "arguments": Result{"ms": 1, "out": path}}))
	if r["isError"] != true {
		t.Error("MCP accepted out")
	}
	if data, _ := os.ReadFile(path); string(data) != "original" {
		t.Error("MCP overwrote host file")
	}
	paths := map[string]bool{}
	for i := 0; i < 2; i++ {
		r = toolResult(t, m.request(t, 4+i, "tools/call", Result{"name": "molty_listen", "arguments": Result{"ms": 1}}))
		if r["isError"] != false {
			t.Fatal(r)
		}
		result := r["structuredContent"].(map[string]any)
		path := result["path"].(string)
		defer os.Remove(path)
		if paths[path] {
			t.Fatal("MCP reused recording path")
		}
		paths[path] = true
		data, err := os.ReadFile(path)
		if err != nil || !bytes.Equal(data, f.wav) {
			t.Fatal("fresh WAV missing", err)
		}
	}
}

type drainingTTS struct {
	started chan struct{}
	release <-chan struct{}
	wav     []byte
}

func (tts drainingTTS) Synthesize(ctx context.Context, _ string) ([]byte, error) {
	close(tts.started)
	select {
	case <-tts.release:
		return tts.wav, nil
	case <-ctx.Done():
		return nil, ctx.Err()
	}
}
func TestMCPEOFDrainsInflightSpeech(t *testing.T) {
	f := newFake(t)
	started := make(chan struct{})
	release := make(chan struct{})
	var once sync.Once
	finish := func() { once.Do(func() { close(release) }) }
	defer finish()
	c := newTestClient(t, f, Config{TTS: drainingTTS{started, release, f.wav}})
	in, writer := io.Pipe()
	var out bytes.Buffer
	done := make(chan error, 1)
	go func() { done <- c.ServeMCP(context.Background(), in, &out) }()
	enc := json.NewEncoder(writer)
	for _, req := range []Result{
		{"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": Result{"protocolVersion": "2025-11-25"}},
		{"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": Result{"name": "molty_say", "arguments": Result{"text": "drain me"}}},
	} {
		if err := enc.Encode(req); err != nil {
			t.Fatal(err)
		}
	}
	select {
	case <-started:
	case <-time.After(3 * time.Second):
		t.Fatal("TTS never started")
	}
	if err := writer.Close(); err != nil {
		t.Fatal(err)
	}
	select {
	case err := <-done:
		t.Fatalf("EOF cancelled in-flight TTS: %v", err)
	case <-time.After(50 * time.Millisecond):
	}
	finish()
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("EOF did not drain")
	}
	dec := json.NewDecoder(&out)
	var init, reply Result
	if err := dec.Decode(&init); err != nil {
		t.Fatal(err)
	}
	if err := dec.Decode(&reply); err != nil {
		t.Fatal(err)
	}
	if toolResult(t, reply)["isError"] != false {
		t.Fatal(reply)
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.creature != "eye" {
		t.Fatal("EOF lost restoration")
	}
	found := false
	for _, r := range f.requests {
		if r.Path == "/api/audio/play_wav" {
			found = true
		}
	}
	if !found {
		t.Fatal("EOF prevented actual playback")
	}
}
