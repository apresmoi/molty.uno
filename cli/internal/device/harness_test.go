package device

import (
	"context"
	"encoding/json"
	"fmt"
	"github.com/molty/cli/internal/testdevice"
	"net/http"
	"net/url"
	"reflect"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/coder/websocket"
)

type roundTripFunc func(*http.Request) (*http.Response, error)

func (f roundTripFunc) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

func TestHarnessURLFallbackAndPrecedence(t *testing.T) {
	var seen []string
	usb := testdevice.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { http.Error(w, "unavailable", 503) }))
	defer usb.Close()
	wifi := testdevice.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { fmt.Fprint(w, `{"ok":true}`) }))
	defer wifi.Close()
	client := &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		seen = append(seen, r.URL.Host+r.URL.Path)
		dest := wifi.URL
		if r.URL.Host == "10.13.37.1" {
			dest = usb.URL
		}
		target, _ := url.Parse(dest)
		clone := r.Clone(r.Context())
		u := *r.URL
		u.Scheme = target.Scheme
		u.Host = target.Host
		clone.URL = &u
		return http.DefaultTransport.RoundTrip(clone)
	})}
	h, err := NewHarness("", "", client)
	if err != nil {
		t.Fatal(err)
	}
	if _, err = h.Request(context.Background(), "POST", "/api/audio/volume", "application/json", []byte(`{"volume":1}`), time.Second); err != nil {
		t.Fatal(err)
	}
	want := []string{"10.13.37.1/api/info", "molty.local/api/info", "molty.local/api/audio/volume"}
	if !reflect.DeepEqual(seen, want) {
		t.Fatalf("fallback: got %v want %v", seen, want)
	}
	if h.URL() != "http://molty.local" {
		t.Fatal(h.URL())
	}
	for _, tc := range []struct{ flag, env, want string }{{wifi.URL, usb.URL, wifi.URL}, {"", wifi.URL, wifi.URL}} {
		h, err := NewHarness(tc.flag, tc.env, nil)
		if err != nil {
			t.Fatal(err)
		}
		if h.URL() != tc.want {
			t.Fatalf("precedence got %s want %s", h.URL(), tc.want)
		}
	}
}
func TestHarnessNoMutationReplay(t *testing.T) {
	var posts atomic.Int32
	s := testdevice.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method == "POST" {
			posts.Add(1)
			http.Error(w, "failed", 500)
			return
		}
		fmt.Fprint(w, `{"ok":true}`)
	}))
	defer s.Close()
	h, _ := NewHarness(s.URL, "", nil)
	if _, err := h.Request(context.Background(), "POST", "/api/creature", "application/json", []byte(`{}`), time.Second); err == nil {
		t.Fatal("expected error")
	}
	if posts.Load() != 1 {
		t.Fatal("mutation replayed")
	}
}
func TestHarnessSerializesUntilBodyConsumed(t *testing.T) {
	var active, max atomic.Int32
	s := testdevice.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		n := active.Add(1)
		defer active.Add(-1)
		for {
			old := max.Load()
			if n <= old || max.CompareAndSwap(old, n) {
				break
			}
		}
		// Flush headers before the delay: a lock ending at Do() is insufficient.
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(200)
		w.(http.Flusher).Flush()
		time.Sleep(8 * time.Millisecond)
		fmt.Fprint(w, `{"ok":true}`)
	}))
	defer s.Close()
	h, _ := NewHarness(s.URL, "", nil)
	var wg sync.WaitGroup
	for range 12 {
		wg.Add(1)
		go func() {
			defer wg.Done()
			if _, err := h.Request(context.Background(), "GET", "/api/info", "", nil, time.Second); err != nil {
				t.Error(err)
			}
		}()
	}
	wg.Wait()
	if max.Load() != 1 {
		t.Fatalf("%d simultaneous requests", max.Load())
	}
}
func TestHarnessErrors(t *testing.T) {
	for _, tc := range []struct {
		code        int
		body, match string
	}{{200, `{"ok":false,"error":"mic_busy"}`, "mic_busy"}, {409, `{"ok":false,"error":"wrong_creature"}`, "wrong_creature"}} {
		t.Run(tc.match, func(t *testing.T) {
			s := testdevice.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(tc.code); fmt.Fprint(w, tc.body) }))
			defer s.Close()
			h, _ := NewHarness(s.URL, "", nil)
			_, err := h.Request(context.Background(), "GET", "/api/info", "", nil, time.Second)
			if err == nil || !strings.Contains(err.Error(), tc.match) {
				t.Fatalf("got %v", err)
			}
		})
	}
	dead := testdevice.NewServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {}))
	dead.Close()
	h, _ := NewHarness(dead.URL, "", nil)
	if _, err := h.Request(context.Background(), "GET", "/api/info", "", nil, time.Second); err == nil {
		t.Fatal("unreachable device succeeded")
	}
	for _, base := range []string{"garbage", "ftp://localhost", "http://localhost/extra", "http://localhost?x=y", "http://user:pass@localhost"} {
		if _, err := NewHarness(base, "", nil); err == nil {
			t.Fatalf("accepted %s", base)
		}
	}
}
func TestHarnessEvents(t *testing.T) {
	s := testdevice.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/ws/imu" {
			t.Errorf("path %s", r.URL.Path)
			return
		}
		conn, err := websocket.Accept(w, r, nil)
		if err != nil {
			t.Error(err)
			return
		}
		defer conn.CloseNow()
		for i := 0; i < 3; i++ {
			if err := conn.Write(r.Context(), websocket.MessageText, []byte(fmt.Sprintf(`{"ax":%d,"ms":%d}`, i, i))); err != nil {
				return
			}
		}
		_ = conn.Close(websocket.StatusNormalClosure, "")
	}))
	defer s.Close()
	h, _ := NewHarness(s.URL, "", nil)
	var frames []string
	err := h.Events(context.Background(), 3, func(frame json.RawMessage) error { frames = append(frames, string(frame)); return nil })
	if err != nil {
		t.Fatal(err)
	}
	if len(frames) != 3 || frames[2] != `{"ax":2,"ms":2}` {
		t.Fatal(frames)
	}
}
