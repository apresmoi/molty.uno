package device

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"

	"github.com/coder/websocket"
)

// Harness serializes device traffic, including response consumption. A selected
// URL is pinned: mutations are never retried against another address.
// Serialization is per instance; other processes must coordinate externally.
type Harness struct {
	mu         sync.Mutex
	base       string
	candidates []string
	http       *http.Client
}

func NewHarness(explicit, environment string, client *http.Client) (*Harness, error) {
	candidates := []string{"http://10.13.37.1", "http://molty.local"}
	base := explicit
	if base == "" {
		base = environment
	}
	if base != "" {
		candidates = []string{base}
	}
	for i, candidate := range candidates {
		u, err := url.Parse(candidate)
		if err != nil || u.Host == "" || (u.Scheme != "http" && u.Scheme != "https") || u.User != nil || u.RawQuery != "" || u.Fragment != "" || (u.Path != "" && u.Path != "/") {
			return nil, fmt.Errorf("invalid device base URL %q", candidate)
		}
		candidates[i] = strings.TrimRight(candidate, "/")
	}
	if client == nil {
		client = &http.Client{}
	}
	// Redirects can change the endpoint or replay a mutation. Refuse them.
	hc := *client
	hc.CheckRedirect = func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }
	h := &Harness{candidates: candidates, http: &hc}
	if base != "" {
		h.base = candidates[0]
	}
	return h, nil
}

func (h *Harness) resolve(ctx context.Context) error {
	if h.base != "" {
		return nil
	}
	var errs []error
	for _, base := range h.candidates {
		data, err := h.do(ctx, base, http.MethodGet, "/api/info", "", nil, 3*time.Second)
		if err == nil {
			var info map[string]any
			if json.Unmarshal(data, &info) != nil || info == nil {
				err = fmt.Errorf("%s/api/info: invalid JSON object", base)
			}
		}
		if err == nil {
			h.base = base
			return nil
		}
		errs = append(errs, err)
		if ctx.Err() != nil {
			break
		}
	}
	return fmt.Errorf("device unreachable: %w", errors.Join(errs...))
}

// Request is private to harness callers; the public CLI/MCP expose only named verbs.
func (h *Harness) Request(ctx context.Context, method, path, contentType string, body []byte, timeout time.Duration) ([]byte, error) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	if err := h.resolve(ctx); err != nil {
		return nil, err
	}
	return h.do(ctx, h.base, method, path, contentType, body, timeout)
}

func (h *Harness) URL() string { h.mu.Lock(); defer h.mu.Unlock(); return h.base }

func (h *Harness) do(ctx context.Context, base, method, path, contentType string, body []byte, timeout time.Duration) ([]byte, error) {
	ctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, method, base+path, bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	if contentType != "" {
		req.Header.Set("Content-Type", contentType)
	}
	resp, err := h.http.Do(req)
	if err != nil {
		return nil, fmt.Errorf("%s %s: %w", method, base+path, err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(io.LimitReader(resp.Body, 4*1024*1024+1))
	if err != nil {
		return nil, fmt.Errorf("%s: read response: %w", path, err)
	}
	if len(data) > 4*1024*1024 {
		return nil, fmt.Errorf("%s: response exceeds 4 MB", path)
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return nil, fmt.Errorf("%s: HTTP %d: %s", path, resp.StatusCode, data)
	}
	var result struct {
		OK    *bool  `json:"ok"`
		Error string `json:"error"`
	}
	if json.Unmarshal(data, &result) == nil && result.OK != nil && !*result.OK {
		return nil, fmt.Errorf("%s: device error: %s (%s)", path, result.Error, data)
	}
	return data, nil
}

// Events reserves the connection for an entire stream, avoiding concurrent HTTP
// traffic from this client. n=0 streams until cancellation; each frame is JSON.
func (h *Harness) Events(ctx context.Context, n int, emit func(json.RawMessage) error) error {
	if n < 0 {
		return fmt.Errorf("n must be nonnegative")
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	if err := h.resolve(ctx); err != nil {
		return err
	}
	base := h.base
	if strings.HasPrefix(base, "https:") {
		base = "wss:" + strings.TrimPrefix(base, "https:")
	} else {
		base = "ws:" + strings.TrimPrefix(base, "http:")
	}
	dialCtx, cancel := context.WithTimeout(ctx, 5*time.Second)
	conn, resp, err := websocket.Dial(dialCtx, base+"/ws/imu", &websocket.DialOptions{HTTPClient: h.http})
	cancel()
	if err != nil {
		if resp != nil {
			resp.Body.Close()
		}
		return fmt.Errorf("IMU websocket: %w", err)
	}
	defer conn.CloseNow()
	for i := 0; n == 0 || i < n; i++ {
		frameCtx, stop := context.WithTimeout(ctx, 10*time.Second)
		typ, data, err := conn.Read(frameCtx)
		stop()
		if err != nil {
			return fmt.Errorf("IMU websocket: %w", err)
		}
		if typ != websocket.MessageText || !json.Valid(data) {
			return fmt.Errorf("IMU websocket: expected JSON text frame")
		}
		if err := emit(json.RawMessage(data)); err != nil {
			return err
		}
	}
	return nil
}
