package molty

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"github.com/molty/cli/internal/testdevice"
	"io"
	"math"
	"net/http"
	"net/http/httptest"
	"net/http/httptrace"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

type fakeRequest struct {
	Method, Path, ContentType string
	Body                      []byte
}
type fakeDevice struct {
	*httptest.Server
	mu           sync.Mutex
	creature     string
	states       map[string]Result
	requests     []fakeRequest
	wav          []byte
	failures     map[string]string
	audioStarted chan struct{}
	audioRelease <-chan struct{}
	volume       int
	delay        time.Duration
}

func testWAV(samples ...int16) []byte {
	b := make([]byte, 44+len(samples)*2)
	copy(b, "RIFF")
	binary.LittleEndian.PutUint32(b[4:], uint32(len(b)-8))
	copy(b[8:], "WAVEfmt ")
	binary.LittleEndian.PutUint32(b[16:], 16)
	binary.LittleEndian.PutUint16(b[20:], 1)
	binary.LittleEndian.PutUint16(b[22:], 1)
	binary.LittleEndian.PutUint32(b[24:], 16000)
	binary.LittleEndian.PutUint32(b[28:], 32000)
	binary.LittleEndian.PutUint16(b[32:], 2)
	binary.LittleEndian.PutUint16(b[34:], 16)
	copy(b[36:], "data")
	binary.LittleEndian.PutUint32(b[40:], uint32(len(samples)*2))
	for i, v := range samples {
		binary.LittleEndian.PutUint16(b[44+i*2:], uint16(v))
	}
	return b
}
func newFake(t *testing.T) *fakeDevice {
	t.Helper()
	f := &fakeDevice{creature: "eye", states: map[string]Result{"eye": {"pupil_size": .33, "blink": .1}, "claude": {"pulse_amp": .04}, "reader": {"wpm": 300}, "glyph": {"ok": true, "has_frame": true, "cols": 8, "rows": 8, "max_dim": 200}, "ui": {"ok": true, "creature": "ui", "nodes": 3, "suppressed": false}, "world": {"scene": "fox/gallop", "animate": true, "anim_time": 2.5}}, wav: testWAV(-32768, 0, 16384, -16384), failures: map[string]string{}}
	f.Server = testdevice.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		f.mu.Lock()
		defer f.mu.Unlock()
		f.requests = append(f.requests, fakeRequest{r.Method, r.URL.RequestURI(), r.Header.Get("Content-Type"), body})
		if failure := f.failures[r.URL.Path]; failure != "" {
			fmt.Fprintf(w, `{"ok":false,"error":%q}`, failure)
			return
		}
		var patch Result
		if len(body) > 0 && r.URL.Path != "/api/audio/volume" && strings.Contains(r.Header.Get("Content-Type"), "application/json") {
			if err := json.Unmarshal(body, &patch); err != nil {
				t.Errorf("invalid request JSON: %v", err)
			}
		}
		var result any = Result{"ok": true}
		switch r.URL.Path {
		case "/api/info":
			result = Result{"ok": true, "chip": "esp32-s3"}
		case "/api/status":
			result = Result{"ok": true, "state": "connected"}
		case "/api/imu/snapshot":
			result = Result{"ok": true, "ax": 1, "ms": 100}
		case "/api/creature":
			if r.Method == "POST" {
				f.creature, _ = patch["name"].(string)
			}
			result = Result{"ok": true, "current": f.creature, "available": []string{"eye", "claude", "reader"}}
		case "/api/creature/state":
			if r.Method == "POST" {
				if f.creature == "ui" || (f.creature == "glyph" && patch["glyphs"] == nil && patch["colors"] == nil) {
					fmt.Fprint(w, `{"ok":false,"error":"set_failed"}`)
					return
				}
				for k, v := range patch {
					f.states[f.creature][k] = v
				}
				// world reloading the reported scene resets animation playback.
				if f.creature == "world" && patch["scene"] != nil {
					f.states["world"]["anim_time"] = 0.0
				}
			}
			result = f.states[f.creature]
		case "/api/reader/text":
			if f.creature != "reader" {
				fmt.Fprint(w, `{"ok":false,"error":"wrong_creature"}`)
				return
			}
		case "/api/audio/play_wav":
			if r.Header.Get("Content-Type") != "audio/wav" {
				t.Error("wrong audio content type")
			}
			if f.audioStarted != nil {
				close(f.audioStarted)
			}
			if f.audioRelease != nil {
				<-f.audioRelease
			}
			time.Sleep(f.delay)
		case "/api/audio/record":
			if r.URL.Query().Get("rate") != "16000" {
				t.Error("wrong capture rate")
			}
			if f.audioStarted != nil {
				close(f.audioStarted)
			}
			if f.audioRelease != nil {
				<-f.audioRelease
			}
			time.Sleep(f.delay)
			w.Header().Set("Content-Type", "audio/wav")
			_, _ = w.Write(f.wav)
			return
		case "/api/audio/volume":
			volume := firmwareVolume(string(body))
			if len(body) == 0 || len(body) > 64 {
				result = Result{"ok": false, "error": "bad_request"}
			} else if volume < 0 || volume > 21 {
				result = Result{"ok": false, "error": "volume_out_of_range"}
			} else {
				f.volume = volume
			}
		default:
			t.Errorf("unexpected endpoint %s", r.URL.Path)
		}
		_ = json.NewEncoder(w).Encode(result)
	}))
	t.Cleanup(f.Close)
	return f
}
func newTestClient(t *testing.T, f *fakeDevice, cfg Config) *Client {
	t.Helper()
	cfg.Device = f.URL
	c, err := New(cfg)
	if err != nil {
		t.Fatal(err)
	}
	return c
}
func shellQuote(s string) string { return "'" + strings.ReplaceAll(s, "'", "'\"'\"'") + "'" }

func TestStatusShowAndVolume(t *testing.T) {
	f := newFake(t)
	c := newTestClient(t, f, Config{})
	status, err := c.Status(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if status["reachable"] != true || status["info"].(Result)["chip"] != "esp32-s3" || status["wifi"] == nil || status["creature"] == nil || status["imu"] == nil {
		t.Fatal(status)
	}
	if _, err = c.Show(context.Background(), "hello world", 350); err != nil {
		t.Fatal(err)
	}
	if f.creature != "reader" || f.states["reader"]["wpm"] != float64(350) || f.states["reader"]["playing"] != true {
		t.Fatal(f.states)
	}
	upload := f.requests[len(f.requests)-2]
	if upload.Path != "/api/reader/text" || string(upload.Body) != "hello world" || !strings.HasPrefix(upload.ContentType, "text/plain") {
		t.Fatal(upload)
	}
	if _, err = c.Volume(context.Background(), 21); err != nil {
		t.Fatal(err)
	}
	before := len(f.requests)
	for _, v := range []int{-1, 22} {
		if _, err = c.Volume(context.Background(), v); err == nil {
			t.Fatal("invalid volume succeeded")
		}
	}
	for _, text := range []string{"", strings.Repeat("a", 16385), "hello\x00there", string([]byte{0xff})} {
		if _, err = c.Show(context.Background(), text, 300); err == nil {
			t.Fatal("invalid text succeeded")
		}
	}
	for _, wpm := range []int{0, 99, 801} {
		if _, err = c.Show(context.Background(), "x", wpm); err == nil {
			t.Fatal("invalid WPM succeeded")
		}
	}
	if len(f.requests) != before {
		t.Fatal("validation reached device")
	}
}
func TestMappedMoods(t *testing.T) {
	for _, tc := range []struct {
		mood, creature, key string
		want                any
	}{{"idle", "claude", "pulse_period_s", 3.0}, {"listening", "claude", "pulse_amp", .06}, {"thinking", "claude", "pulse_period_s", .8}, {"speaking", "claude", "pulse_amp", .14}, {"happy", "claude", "body_color", "#ffce60"}, {"sleepy", "eye", "blink", .75}, {"surprised", "eye", "pupil_size", .75}} {
		t.Run(tc.mood, func(t *testing.T) {
			f := newFake(t)
			c := newTestClient(t, f, Config{})
			result, err := c.Mood(context.Background(), tc.mood)
			if err != nil {
				t.Fatal(err)
			}
			if f.creature != tc.creature || !reflect.DeepEqual(f.states[tc.creature][tc.key], tc.want) || result["mood"] != tc.mood {
				t.Fatalf("%s %v", f.creature, f.states)
			}
			if len(f.requests) != 2 || f.requests[0].Path != "/api/creature" || f.requests[1].Path != "/api/creature/state" {
				t.Fatal(f.requests)
			}
		})
	}
	if _, err := (MappedMoods{}).Map("angry"); err == nil {
		t.Fatal("invalid mood")
	}
}
func TestSayCommandPipelineAndRestoration(t *testing.T) {
	f := newFake(t)
	dir := t.TempDir()
	wavPath := filepath.Join(dir, "input.wav")
	textPath := filepath.Join(dir, "text")
	if err := os.WriteFile(wavPath, f.wav, 0600); err != nil {
		t.Fatal(err)
	}
	command := "cat > " + shellQuote(textPath) + "; cat " + shellQuote(wavPath)
	c := newTestClient(t, f, Config{TTS: CommandTTS(command)})
	text := "hello ' world; $(no shell interpolation)"
	result, err := c.Say(context.Background(), SayOptions{Text: text})
	if err != nil {
		t.Fatal(err)
	}
	got, _ := os.ReadFile(textPath)
	if string(got) != text {
		t.Fatalf("TTS stdin = %q", got)
	}
	if result["duration_ms"] != .25 {
		t.Fatal(result)
	}
	if f.creature != "eye" || f.states["eye"]["pupil_size"] != .33 {
		t.Fatal("previous state not restored")
	}
	var play *fakeRequest
	for i := range f.requests {
		r := &f.requests[i]
		if r.Path == "/api/audio/play_wav" {
			play = r
		}
	}
	if play == nil || !bytes.Equal(play.Body, f.wav) {
		t.Fatal("TTS WAV never reached device")
	}
	want := []string{"/api/creature", "/api/creature/state", "/api/creature", "/api/creature/state", "/api/audio/play_wav", "/api/creature", "/api/creature/state"}
	paths := []string{}
	for _, r := range f.requests {
		paths = append(paths, r.Path)
	}
	if !reflect.DeepEqual(paths, want) {
		t.Fatalf("%v", paths)
	}
}
func TestListenCommandPipeline(t *testing.T) {
	f := newFake(t)
	dir := t.TempDir()
	captured := filepath.Join(dir, "stt-input.wav")
	out := filepath.Join(dir, "record.wav")
	c := newTestClient(t, f, Config{STT: CommandSTT("cat > " + shellQuote(captured) + "; printf 'heard words\\n'")})
	result, err := c.Listen(context.Background(), ListenOptions{MS: 4000, Out: out})
	if err != nil {
		t.Fatal(err)
	}
	if result["text"] != "heard words" || result["path"] != out || result["peak"] != 1.0 || result["duration_ms"] != .25 {
		t.Fatal(result)
	}
	if math.Abs(result["rms"].(float64)-math.Sqrt(.375)) > 1e-9 {
		t.Fatal(result)
	}
	for _, path := range []string{out, captured} {
		got, err := os.ReadFile(path)
		if err != nil || !bytes.Equal(got, f.wav) {
			t.Fatalf("%s: %v", path, err)
		}
	}
	if f.creature != "eye" {
		t.Fatal("listening mood leaked")
	}
	info, err := os.Stat(out)
	if err != nil || info.Mode().Perm() != 0600 {
		t.Fatalf("file mode: %v %v", info, err)
	}
	for _, ms := range []int{0, -1, 10001} {
		if _, err = c.Listen(context.Background(), ListenOptions{MS: ms}); err == nil {
			t.Fatal("invalid capture duration")
		}
	}
}
func TestListenTempNoSTTAndEnvironmentSTT(t *testing.T) {
	t.Setenv("MOLTY_STT_CMD", "")
	f := newFake(t)
	c := newTestClient(t, f, Config{})
	result, err := c.Listen(context.Background(), ListenOptions{MS: 1})
	if err != nil {
		t.Fatal(err)
	}
	path := result["path"].(string)
	defer os.Remove(path)
	if _, ok := result["text"]; ok {
		t.Fatal("unexpected STT")
	}
	if got, err := os.ReadFile(path); err != nil || !bytes.Equal(got, f.wav) {
		t.Fatal("temporary recording missing")
	}
	t.Setenv("MOLTY_STT_CMD", "cat >/dev/null; printf environment")
	c = newTestClient(t, f, Config{})
	result, err = c.Listen(context.Background(), ListenOptions{MS: 1, Out: filepath.Join(t.TempDir(), "env.wav")})
	if err != nil || result["text"] != "environment" {
		t.Fatalf("%v %v", result, err)
	}
}
func TestAudioErrorsAndRestoration(t *testing.T) {
	for _, path := range []string{"/api/audio/record", "/api/audio/play_wav"} {
		t.Run(path, func(t *testing.T) {
			f := newFake(t)
			f.failures[path] = "mic_busy"
			c := newTestClient(t, f, Config{})
			var err error
			if strings.HasSuffix(path, "record") {
				_, err = c.Listen(context.Background(), ListenOptions{MS: 1})
			} else {
				p := filepath.Join(t.TempDir(), "x.wav")
				_ = os.WriteFile(p, f.wav, 0600)
				_, err = c.Say(context.Background(), SayOptions{WAV: p})
			}
			if err == nil || !strings.Contains(err.Error(), "mic_busy") {
				t.Fatalf("got %v", err)
			}
			if f.creature != "eye" {
				t.Fatal("mood not restored on error")
			}
		})
	}
	f := newFake(t)
	f.failures["/api/reader/text"] = "wrong_creature"
	c := newTestClient(t, f, Config{})
	if _, err := c.Show(context.Background(), "x", 300); err == nil || !strings.Contains(err.Error(), "wrong_creature") {
		t.Fatal(err)
	}
	dead := testdevice.NewServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {}))
	dead.Close()
	c, err := New(Config{Device: dead.URL})
	if err != nil {
		t.Fatal(err)
	}
	status, err := c.Status(context.Background())
	if err == nil || status["reachable"] != false {
		t.Fatalf("%v %v", status, err)
	}
}
func TestSTTFailureKeepsRecording(t *testing.T) {
	f := newFake(t)
	c := newTestClient(t, f, Config{STT: CommandSTT("cat >/dev/null; echo broken >&2; exit 7")})
	result, err := c.Listen(context.Background(), ListenOptions{MS: 1, Out: filepath.Join(t.TempDir(), "saved.wav")})
	if err == nil || !strings.Contains(err.Error(), "broken") {
		t.Fatal(err)
	}
	if _, err := os.Stat(result["path"].(string)); err != nil {
		t.Fatal(err)
	}
}
func TestCanonicalWAV(t *testing.T) {
	good := testWAV(-32768, 32767)
	withMetadata := append(append([]byte{}, good...), []byte("LIST\x04\x00\x00\x00abcd")...)
	binary.LittleEndian.PutUint32(withMetadata[4:], uint32(len(withMetadata)-8))
	wav, stats, err := canonicalWAV(withMetadata)
	if err != nil || !bytes.Equal(wav, good) || stats.Peak != 1 {
		t.Fatalf("metadata %v %v", stats, err)
	}
	for _, tc := range []struct {
		at    int
		value byte
	}{{20, 3}, {22, 2}, {24, 1}, {34, 8}, {40, 255}} {
		bad := append([]byte{}, good...)
		bad[tc.at] = tc.value
		if _, _, err := canonicalWAV(bad); err == nil {
			t.Fatalf("invalid field %d accepted", tc.at)
		}
	}
	for _, bad := range [][]byte{nil, good[:40], testWAV(), append(good, 0)} {
		if _, _, err := canonicalWAV(bad); err == nil {
			t.Fatal("invalid WAV accepted")
		}
	}
}
func TestSpeechCommandFailure(t *testing.T) {
	for _, command := range []CommandTTS{"echo nope >&2; exit 9", "printf invalid"} {
		f := newFake(t)
		c := newTestClient(t, f, Config{TTS: command})
		if _, err := c.Say(context.Background(), SayOptions{Text: "x"}); err == nil {
			t.Fatal("invalid TTS succeeded")
		}
		if len(f.requests) > 0 {
			t.Fatal("bad TTS contacted device")
		}
	}
}
func TestWholeVerbSerialization(t *testing.T) {
	f := newFake(t)
	c := newTestClient(t, f, Config{})
	var wg sync.WaitGroup
	for i := 0; i < 10; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			if _, err := c.Show(context.Background(), fmt.Sprintf("text %d", i), 300); err != nil {
				t.Error(err)
			}
		}(i)
	}
	wg.Wait()
	for i, r := range f.requests {
		want := []string{"/api/creature", "/api/reader/text", "/api/creature/state"}[i%3]
		if r.Path != want {
			t.Fatalf("operation interleaved at %d: %s", i, r.Path)
		}
	}
}

func TestDefaultMacSayBackend(t *testing.T) {
	f := newFake(t)
	dir := t.TempDir()
	wavPath := filepath.Join(dir, "source.wav")
	textPath := filepath.Join(dir, "stdin")
	argsPath := filepath.Join(dir, "args")
	_ = os.WriteFile(wavPath, f.wav, 0600)
	// Stand in for the OS executable: verify argv, stdin and the output-file
	// contract without contacting a device or using an installed system voice.
	script := "#!/bin/sh\nprintf '%s\\n' \"$@\" > " + shellQuote(argsPath) + "\ncat > " + shellQuote(textPath) + "\ncp " + shellQuote(wavPath) + " \"$2\"\n"
	if err := os.WriteFile(filepath.Join(dir, "say"), []byte(script), 0700); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", dir+string(os.PathListSeparator)+os.Getenv("PATH"))
	c := newTestClient(t, f, Config{})
	if _, err := c.Say(context.Background(), SayOptions{Text: "default voice"}); err != nil {
		t.Fatal(err)
	}
	args, _ := os.ReadFile(argsPath)
	if !strings.Contains(string(args), "--data-format=LEI16@16000\n--file-format=WAVE\n") {
		t.Fatal(string(args))
	}
	text, _ := os.ReadFile(textPath)
	if string(text) != "default voice" {
		t.Fatal(string(text))
	}
}

type testMapper struct{}

func (testMapper) Map(mood string) (MoodPlan, error) {
	return MoodPlan{Patch: Result{"mood": mood}}, nil
}
func TestNativeMapperNeedsNoCallerChange(t *testing.T) {
	f := newFake(t)
	c := newTestClient(t, f, Config{Mapper: testMapper{}})
	if _, err := c.Mood(context.Background(), "happy"); err != nil {
		t.Fatal(err)
	}
	if len(f.requests) != 1 || f.requests[0].Path != "/api/creature/state" || f.states["eye"]["mood"] != "happy" {
		t.Fatal(f.requests)
	}
}
func TestAudioDeadlinesAllowClipDuration(t *testing.T) {
	f := newFake(t)
	wav := testWAV(make([]int16, 16000*6)...)
	path := filepath.Join(t.TempDir(), "long.wav")
	_ = os.WriteFile(path, wav, 0600)
	transport := http.DefaultTransport
	c := newTestClient(t, f, Config{HTTPClient: &http.Client{Transport: clientRoundTrip(func(r *http.Request) (*http.Response, error) {
		if r.URL.Path == "/api/audio/play_wav" || r.URL.Path == "/api/audio/record" {
			deadline, ok := r.Context().Deadline()
			if !ok || time.Until(deadline) < 15*time.Second || time.Until(deadline) > 16*time.Second {
				t.Errorf("audio timeout too short: %v", time.Until(deadline))
			}
		}
		return transport.RoundTrip(r)
	})}})
	if _, err := c.Say(context.Background(), SayOptions{WAV: path}); err != nil {
		t.Fatal(err)
	}
	if _, err := c.Listen(context.Background(), ListenOptions{MS: 6000, Out: filepath.Join(t.TempDir(), "out.wav")}); err != nil {
		t.Fatal(err)
	}
}

type clientRoundTrip func(*http.Request) (*http.Response, error)

func (f clientRoundTrip) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }
func TestRecordingSurvivesRestoreFailure(t *testing.T) {
	f := newFake(t)
	transport := http.DefaultTransport
	c := newTestClient(t, f, Config{HTTPClient: &http.Client{Transport: clientRoundTrip(func(r *http.Request) (*http.Response, error) {
		if r.URL.Path == "/api/audio/record" {
			response, err := transport.RoundTrip(r)
			f.mu.Lock()
			f.failures["/api/creature"] = "restore_failed"
			f.mu.Unlock()
			return response, err
		}
		return transport.RoundTrip(r)
	})}})
	path := filepath.Join(t.TempDir(), "record.wav")
	result, err := c.Listen(context.Background(), ListenOptions{MS: 1, Out: path})
	if err != nil || !strings.Contains(fmt.Sprint(result["restore_warning"]), "restore_failed") || result["path"] != path {
		t.Fatalf("%v %v", result, err)
	}
	if data, err := os.ReadFile(path); err != nil || !bytes.Equal(data, f.wav) {
		t.Fatal("recording lost")
	}
}
func TestRestoreAfterCallerCancellation(t *testing.T) {
	f := newFake(t)
	transport := http.DefaultTransport
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	c := newTestClient(t, f, Config{HTTPClient: &http.Client{Transport: clientRoundTrip(func(r *http.Request) (*http.Response, error) {
		if r.URL.Path == "/api/audio/play_wav" {
			cancel()
			select {
			case <-r.Context().Done():
				return nil, r.Context().Err()
			case <-time.After(time.Second):
				t.Error("upload ignored cancellation")
				return nil, fmt.Errorf("upload ignored cancellation")
			}
		}
		return transport.RoundTrip(r)
	})}})
	path := filepath.Join(t.TempDir(), "x.wav")
	_ = os.WriteFile(path, f.wav, 0600)
	if _, err := c.Say(ctx, SayOptions{WAV: path}); err == nil {
		t.Fatal("canceled call succeeded")
	}
	if f.creature != "eye" {
		t.Fatal("cancellation prevented restoration")
	}
}

func TestEmptyRecordingFails(t *testing.T) {
	f := newFake(t)
	f.wav = nil
	c := newTestClient(t, f, Config{})
	if _, err := c.Listen(context.Background(), ListenOptions{MS: 1, Out: filepath.Join(t.TempDir(), "bad.wav")}); err == nil {
		t.Fatal("empty recording reported success")
	}
}
func TestCommandOutputBound(t *testing.T) {
	_, err := runCommand(context.Background(), exec.CommandContext(context.Background(), "/bin/sh", "-c", "printf 123456789"), nil, 4)
	if err == nil {
		t.Fatal("subprocess output bypassed limit")
	}
}

// Match tusb_ncm_main.c json_get_str (7-byte output) followed by atoi;
// in particular JSON numbers fall back to atoi of the whole object (=0).
func firmwareVolume(body string) int {
	if i := strings.IndexByte(body, 0); i >= 0 {
		body = body[:i]
	}
	value := body
	if i := strings.Index(body, `"volume"`); i >= 0 {
		tail := strings.TrimLeft(body[i+len(`"volume"`):], " :")
		if strings.HasPrefix(tail, `"`) {
			value = ""
			for k := 1; k < len(tail) && tail[k] != '"' && len(value) < 7; k++ {
				if tail[k] == 92 && k+1 < len(tail) {
					k++
				}
				value += string(tail[k])
			}
		}
	}
	value = strings.TrimLeft(value, " \t\n\r\v\f")
	n := 0
	if len(value) > 0 && (value[0] == '-' || value[0] == '+') {
		n++
	}
	start := n
	for n < len(value) && value[n] >= '0' && value[n] <= '9' {
		n++
	}
	if n == start {
		return 0
	}
	v, _ := strconv.Atoi(value[:n])
	return v
}

func TestVolumeFirmwareParser(t *testing.T) {
	for body, want := range map[string]int{`{"volume":10}`: 0, `{"volume":"10"}`: 10, "10": 10, " +21junk": 21} {
		if got := firmwareVolume(body); got != want {
			t.Fatalf("%q: %d != %d", body, got, want)
		}
	}
	f := newFake(t)
	c := newTestClient(t, f, Config{})
	for _, want := range []int{10, 21, 1, 0} {
		if _, err := c.Volume(context.Background(), want); err != nil {
			t.Fatal(err)
		}
		f.mu.Lock()
		got := f.volume
		request := f.requests[len(f.requests)-1]
		f.mu.Unlock()
		if got != want {
			t.Fatalf("firmware volume = %d; want %d (body %s)", got, want, request.Body)
		}
		if request.ContentType != "text/plain" || string(request.Body) != strconv.Itoa(want) {
			t.Fatalf("wrong volume wire format: %+v", request)
		}
	}
}

func TestAudioRestoresNonRoundTripCreatures(t *testing.T) {
	for _, creature := range []string{"glyph", "ui", "world"} {
		for _, verb := range []string{"say", "listen"} {
			t.Run(creature+"/"+verb, func(t *testing.T) {
				f := newFake(t)
				f.creature = creature
				before, _ := json.Marshal(f.states[creature])
				c := newTestClient(t, f, Config{})
				var result Result
				var err error
				if verb == "say" {
					path := filepath.Join(t.TempDir(), "input.wav")
					if err := os.WriteFile(path, f.wav, 0600); err != nil {
						t.Fatal(err)
					}
					result, err = c.Say(context.Background(), SayOptions{WAV: path})
				} else {
					result, err = c.Listen(context.Background(), ListenOptions{MS: 1, Out: filepath.Join(t.TempDir(), "out.wav")})
				}
				if err != nil || result["restore_warning"] != nil {
					t.Fatalf("audio failed: %v %v", result, err)
				}
				f.mu.Lock()
				defer f.mu.Unlock()
				if f.creature != creature {
					t.Fatalf("creature = %s", f.creature)
				}
				after, _ := json.Marshal(f.states[creature])
				if !bytes.Equal(before, after) {
					t.Fatalf("state corrupted: %s -> %s", before, after)
				}
				if f.requests[len(f.requests)-1].Path != "/api/creature" {
					t.Fatal("non-round-trip state POSTed on restore")
				}
			})
		}
	}
}

func TestAudioRestoreFailuresAreWarnings(t *testing.T) {
	for _, verb := range []string{"say", "listen"} {
		for _, endpoint := range []string{"/api/creature", "/api/creature/state"} {
			for _, audioFails := range []bool{false, true} {
				t.Run(fmt.Sprintf("%s/%s/audioFails=%t", verb, endpoint, audioFails), func(t *testing.T) {
					f := newFake(t)
					if audioFails {
						path := "/api/audio/play_wav"
						if verb == "listen" {
							path = "/api/audio/record"
						}
						f.failures[path] = "audio_failed"
					}
					transport := http.DefaultTransport
					c := newTestClient(t, f, Config{HTTPClient: &http.Client{Transport: clientRoundTrip(func(r *http.Request) (*http.Response, error) {
						response, err := transport.RoundTrip(r)
						if strings.HasPrefix(r.URL.Path, "/api/audio/") {
							f.mu.Lock()
							f.failures[endpoint] = "restore_failed"
							f.mu.Unlock()
						}
						return response, err
					})}})
					var result Result
					var err error
					if verb == "say" {
						path := filepath.Join(t.TempDir(), "input.wav")
						if err := os.WriteFile(path, f.wav, 0600); err != nil {
							t.Fatal(err)
						}
						result, err = c.Say(context.Background(), SayOptions{WAV: path})
					} else {
						result, err = c.Listen(context.Background(), ListenOptions{MS: 1, Out: filepath.Join(t.TempDir(), "out.wav")})
					}
					if !strings.Contains(fmt.Sprint(result["restore_warning"]), "restore_failed") {
						t.Fatalf("warning missing: %v %v", result, err)
					}
					if audioFails {
						if err == nil || !strings.Contains(err.Error(), "audio_failed") || strings.Contains(err.Error(), "restore_failed") {
							t.Fatalf("restore changed audio error: %v", err)
						}
					} else if err != nil || result["ok"] != true {
						t.Fatalf("restore turned success into failure: %v %v", result, err)
					}
				})
			}
		}
	}
}

func TestListenOutputSafety(t *testing.T) {
	for _, name := range []string{"existing.wav", "wrong.txt", "no-extension", "existing-link.wav"} {
		t.Run(name, func(t *testing.T) {
			f := newFake(t)
			c := newTestClient(t, f, Config{})
			dir := t.TempDir()
			path := filepath.Join(dir, name)
			original := []byte("do not overwrite")
			target := path
			if name == "existing-link.wav" {
				target = filepath.Join(dir, "target")
				if err := os.Symlink(target, path); err != nil {
					t.Fatal(err)
				}
			}
			if err := os.WriteFile(target, original, 0600); err != nil {
				t.Fatal(err)
			}
			_, err := c.Listen(context.Background(), ListenOptions{MS: 1, Out: path})
			if err == nil {
				t.Error("unsafe output accepted")
			}
			data, _ := os.ReadFile(target)
			if !bytes.Equal(data, original) {
				t.Error("existing file replaced")
			}
			if len(f.requests) != 0 {
				t.Error("invalid output started capture")
			}
		})
	}
}

func TestListenRefusesFileCreatedDuringCapture(t *testing.T) {
	f := newFake(t)
	path := filepath.Join(t.TempDir(), "race.wav")
	transport := http.DefaultTransport
	c := newTestClient(t, f, Config{HTTPClient: &http.Client{Transport: clientRoundTrip(func(r *http.Request) (*http.Response, error) {
		response, err := transport.RoundTrip(r)
		if r.URL.Path == "/api/audio/record" {
			if e := os.WriteFile(path, []byte("concurrent writer"), 0600); e != nil {
				t.Error(e)
			}
		}
		return response, err
	})}})
	if _, err := c.Listen(context.Background(), ListenOptions{MS: 1, Out: path}); err == nil {
		t.Error("racing output replaced")
	}
	if data, _ := os.ReadFile(path); string(data) != "concurrent writer" {
		t.Fatal("concurrent file destroyed")
	}
}

// The fake holds its firmware lock through the entire audio operation and
// ignores HTTP disconnects once audio starts, just like the synchronous httpd.
func TestAudioCancellationWaitsForFirmware(t *testing.T) {
	for _, verb := range []string{"say", "listen"} {
		for _, mode := range []string{"cancel", "timeout"} {
			t.Run(verb+"/"+mode, func(t *testing.T) {
				f := newFake(t)
				f.audioStarted = make(chan struct{})
				// Use the actual WAV/capture duration; firmware ignores caller
				// cancellation and keeps its HTTP worker busy for this time.
				f.wav = testWAV(make([]int16, 16000*600/1000)...)
				f.delay = 600 * time.Millisecond
				sent := make(chan struct{})
				earlyRestore := make(chan struct{}, 1)
				var sentOnce sync.Once
				transport := http.DefaultTransport
				c := newTestClient(t, f, Config{HTTPClient: &http.Client{Transport: clientRoundTrip(func(r *http.Request) (*http.Response, error) {
					if strings.HasPrefix(r.URL.Path, "/api/audio/") {
						trace := &httptrace.ClientTrace{WroteRequest: func(info httptrace.WroteRequestInfo) {
							if info.Err == nil {
								sentOnce.Do(func() { close(sent) })
							}
						}}
						r = r.WithContext(httptrace.WithClientTrace(r.Context(), trace))
					} else if r.URL.Path == "/api/creature" && r.Method == "POST" {
						select {
						case <-sent:
							select {
							case earlyRestore <- struct{}{}:
							default:
							}
						default:
						}
					}
					return transport.RoundTrip(r)
				})}})
				var ctx context.Context
				var cancel context.CancelFunc
				if mode == "timeout" {
					ctx, cancel = context.WithTimeout(context.Background(), 200*time.Millisecond)
				} else {
					ctx, cancel = context.WithCancel(context.Background())
				}
				defer cancel()
				done := make(chan error, 1)
				go func() {
					if verb == "say" {
						path := filepath.Join(t.TempDir(), "input.wav")
						if err := os.WriteFile(path, f.wav, 0600); err != nil {
							done <- err
							return
						}
						_, err := c.Say(ctx, SayOptions{WAV: path})
						done <- err
					} else {
						_, err := c.Listen(ctx, ListenOptions{MS: 600, Out: filepath.Join(t.TempDir(), "out.wav")})
						done <- err
					}
				}()
				select {
				case <-sent:
				case <-time.After(3 * time.Second):
					t.Fatal("audio not uploaded")
				}
				select {
				case <-f.audioStarted:
				case <-time.After(3 * time.Second):
					t.Fatal("firmware did not start audio")
				}
				if mode == "cancel" {
					cancel()
				}
				select {
				case <-ctx.Done():
				case <-time.After(time.Second):
					t.Fatal("caller did not expire")
				}
				select {
				case <-earlyRestore:
					t.Error("restore queued before firmware completed audio")
				case err := <-done:
					t.Errorf("audio returned before firmware completion: %v", err)
				case <-time.After(50 * time.Millisecond):
				}
				// A later command must follow audio AND restoration, without late restores.
				next := make(chan error, 1)
				go func() { _, err := c.Show(context.Background(), "after audio", 300); next <- err }()
				select {
				case err := <-done:
					if err != nil {
						t.Error(err)
					}
				case <-time.After(3 * time.Second):
					t.Fatal("audio did not finish")
				}
				select {
				case err := <-next:
					if err != nil {
						t.Error(err)
					}
				case <-time.After(3 * time.Second):
					t.Fatal("next command blocked")
				}
				f.mu.Lock()
				defer f.mu.Unlock()
				if f.creature != "reader" {
					t.Fatalf("late restore replaced next command: %s", f.creature)
				}
			})
		}
	}
}

func TestAudioRestoreBudgetAfterLostResponse(t *testing.T) {
	f := newFake(t)
	f.audioStarted = make(chan struct{})
	release := make(chan struct{})
	var once sync.Once
	finish := func() { once.Do(func() { close(release) }) }
	defer finish()
	f.audioRelease = release
	transport := http.DefaultTransport
	restoreSeen := make(chan time.Duration, 1)
	c := newTestClient(t, f, Config{HTTPClient: &http.Client{Transport: clientRoundTrip(func(r *http.Request) (*http.Response, error) {
		if r.URL.Path == "/api/audio/play_wav" {
			deadline, ok := r.Context().Deadline()
			if !ok || time.Until(deadline) < 29*time.Second || time.Until(deadline) > 30*time.Second {
				t.Errorf("audio budget must be duration+10s: %v", time.Until(deadline))
			}
			// A lost HTTP response does not stop firmware playback.
			lost, stop := context.WithCancel(r.Context())
			defer stop()
			go func() {
				select {
				case <-f.audioStarted:
					stop()
				case <-lost.Done():
				}
			}()
			return transport.RoundTrip(r.WithContext(lost))
		}
		if r.URL.Path == "/api/creature" && r.Method == "POST" {
			select {
			case <-f.audioStarted:
				deadline, _ := r.Context().Deadline()
				restoreSeen <- time.Until(deadline)
			default:
			}
		}
		return transport.RoundTrip(r)
	})}})
	path := filepath.Join(t.TempDir(), "long.wav")
	_ = os.WriteFile(path, testWAV(make([]int16, 16000*20)...), 0600)
	done := make(chan error, 1)
	go func() { _, err := c.Say(context.Background(), SayOptions{WAV: path}); done <- err }()
	select {
	case budget := <-restoreSeen:
		if budget < 29*time.Second || budget > 30*time.Second {
			t.Errorf("restore budget lost remaining clip duration: %s", budget)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("restore not attempted")
	}
	finish()
	select {
	case err := <-done:
		if err == nil {
			t.Error("lost response must report audio error")
		}
	case <-time.After(3 * time.Second):
		t.Fatal("restore did not complete")
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.creature != "eye" {
		t.Fatal("speaking mood leaked")
	}
}
