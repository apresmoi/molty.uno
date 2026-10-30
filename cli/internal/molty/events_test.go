package molty

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"strings"
	"sync"
	"testing"
	"time"
)

func fastBackoff(t *testing.T) {
	t.Helper()
	min, max := eventsBackoffMin, eventsBackoffMax
	eventsBackoffMin, eventsBackoffMax = time.Millisecond, 4*time.Millisecond
	t.Cleanup(func() { eventsBackoffMin, eventsBackoffMax = min, max })
}

// The fake relay answers each events request from a script, recording queries.
func scriptedRelay(t *testing.T, steps []func(w http.ResponseWriter)) (*Client, *[]string) {
	t.Helper()
	var mu sync.Mutex
	var queries []string
	c := relayClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/v1/devices/molty-test/events" || r.Method != "GET" || r.Header.Get("Authorization") != "Bearer secret" {
			t.Errorf("unexpected %s %s", r.Method, r.URL)
			http.NotFound(w, r)
			return
		}
		mu.Lock()
		i := len(queries)
		queries = append(queries, r.URL.RawQuery)
		mu.Unlock()
		if i >= len(steps) {
			<-r.Context().Done() // hold the long-poll until the client gives up
			return
		}
		steps[i](w)
	})
	return c, &queries
}

func body(s string) func(http.ResponseWriter) {
	return func(w http.ResponseWriter) { fmt.Fprint(w, s) }
}
func status(code int, s string) func(http.ResponseWriter) {
	return func(w http.ResponseWriter) { w.WriteHeader(code); fmt.Fprint(w, s) }
}

func TestRelayEventsFollowsCursorAndReconnects(t *testing.T) {
	fastBackoff(t)
	c, queries := scriptedRelay(t, []func(http.ResponseWriter){
		body(`{"ok":true,"events":[{"id":"e1","type":"shake","ts":1,"data":{"peak_g":2.4,"reversals":5,"ms":900}},{"id":"e2","type":"tap","ts":2,"data":{"x":0.5,"y":0.5}}],"next":"e2","more":false}`),
		body(`{"ok":true,"events":[],"next":"e2","more":false}`), // long-poll timed out empty
		status(503, `{"ok":false,"error":"internal"}`),
		status(429, `{"ok":false,"error":"rate"}`),
		body(`{"ok":true,"events":[{"id":"e3","type":"button","ts":3,"data":{"id":"accept"}}],"next":"e3","more":false}`),
	})
	var warn bytes.Buffer
	c.Transport.(*RelayTransport).Warn = &warn
	var got []map[string]any
	err := c.Events(context.Background(), 3, func(raw json.RawMessage) error {
		var ev map[string]any
		if err := json.Unmarshal(raw, &ev); err != nil {
			t.Fatal(err)
		}
		got = append(got, ev)
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 3 || got[0]["id"] != "e1" || got[2]["type"] != "button" || got[0]["data"].(map[string]any)["peak_g"] != 2.4 {
		t.Fatalf("events %v", got)
	}
	// First request resumes at the relay's cursor; afterwards it passes the last next.
	want := []string{"wait_s=25", "wait_s=25&since=e2", "wait_s=25&since=e2", "wait_s=25&since=e2", "wait_s=25&since=e2"}
	if strings.Join(*queries, " | ") != strings.Join(want, " | ") {
		t.Fatalf("queries %q", *queries)
	}
	if strings.Count(warn.String(), "reconnecting") != 2 {
		t.Fatalf("warn %q", warn.String())
	}
}

func TestRelayEventsStopsOnAuthAndClientErrors(t *testing.T) {
	fastBackoff(t)
	for _, tc := range []struct {
		step func(http.ResponseWriter)
		auth bool
	}{
		{status(401, `{"ok":false,"error":"unauthorized"}`), true},
		{status(403, `{"ok":false,"error":"unauthorized"}`), true},
		{status(400, `{"ok":false,"error":"invalid_args"}`), false},
		{status(404, `{"ok":false,"error":"not_found"}`), false},
	} {
		c, queries := scriptedRelay(t, []func(http.ResponseWriter){tc.step})
		err := c.Events(context.Background(), 0, func(json.RawMessage) error { return nil })
		if err == nil || errors.Is(err, ErrRelayAuth) != tc.auth || len(*queries) != 1 {
			t.Fatalf("err=%v auth=%v queries=%v", err, tc.auth, *queries)
		}
	}
}

func TestRelayEventsCancelDuringLongPollIsClean(t *testing.T) {
	c, _ := scriptedRelay(t, nil)
	ctx, cancel := context.WithTimeout(context.Background(), 50*time.Millisecond)
	defer cancel()
	if err := c.Events(ctx, 0, func(json.RawMessage) error { return nil }); err != nil {
		t.Fatal(err)
	}
}
