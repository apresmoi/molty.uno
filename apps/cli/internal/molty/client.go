// Package molty implements the shared device verbs used by the CLI and MCP.
package molty

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"

	"github.com/molty/cli/internal/device"
)

type Result = map[string]any

type Config struct {
	Device     string
	Relay      string
	Token      string
	Transport  Transport
	HTTPClient *http.Client
	Mapper     MoodMapper
	TTS        TTS
	STT        STT
	// Warn receives non-fatal notices (relay events reconnects); nil = silent.
	Warn io.Writer
}

type HTTPTransport struct {
	mu     sync.Mutex // Serialize whole verbs, so creature switch + patch are atomic.
	device *device.Harness
	mapper MoodMapper
	tts    TTS
	stt    STT
}

// NewHTTPTransport constructs the existing direct-device implementation.
func NewHTTPTransport(cfg Config) (*HTTPTransport, error) {
	d, err := device.NewHarness(cfg.Device, os.Getenv("MOLTY_URL"), cfg.HTTPClient)
	if err != nil {
		return nil, err
	}
	if cfg.Mapper == nil {
		cfg.Mapper = MappedMoods{}
	}
	if cfg.TTS == nil {
		cfg.TTS = MacSay{}
	}
	if cfg.STT == nil && os.Getenv("MOLTY_STT_CMD") != "" {
		cfg.STT = CommandSTT(os.Getenv("MOLTY_STT_CMD"))
	}
	return &HTTPTransport{device: d, mapper: cfg.Mapper, tts: cfg.TTS, stt: cfg.STT}, nil
}

func (c *HTTPTransport) json(ctx context.Context, method, path string, value any) (Result, error) {
	return c.jsonTimeout(ctx, method, path, value, 5*time.Second)
}

func (c *HTTPTransport) jsonTimeout(ctx context.Context, method, path string, value any, timeout time.Duration) (Result, error) {
	var body []byte
	var err error
	if value != nil {
		body, err = json.Marshal(value)
		if err != nil {
			return nil, err
		}
	}
	data, err := c.device.Request(ctx, method, path, "application/json", body, timeout)
	if err != nil {
		return nil, err
	}
	return decode(data)
}

func decode(data []byte) (Result, error) {
	var result Result
	if err := json.Unmarshal(data, &result); err != nil {
		return nil, fmt.Errorf("invalid device JSON: %w", err)
	}
	if result == nil {
		return nil, fmt.Errorf("device returned null JSON")
	}
	return result, nil
}

func (c *HTTPTransport) Status(ctx context.Context) (Result, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	out := Result{"reachable": false}
	info, err := c.json(ctx, "GET", "/api/info", nil)
	if err != nil {
		out["error"] = err.Error()
		return out, err
	}
	out["reachable"] = true
	out["device"] = c.device.URL()
	out["info"] = info
	// Retain partial status if an older firmware lacks an endpoint.
	failures := Result{}
	for _, endpoint := range []struct{ key, path string }{{"wifi", "/api/status"}, {"creature", "/api/creature"}, {"imu", "/api/imu/snapshot"}} {
		value, err := c.json(ctx, "GET", endpoint.path, nil)
		if err != nil {
			failures[endpoint.key] = err.Error()
		} else {
			out[endpoint.key] = value
		}
	}
	out["ok"] = len(failures) == 0
	if len(failures) > 0 {
		out["errors"] = failures
		return out, fmt.Errorf("incomplete status: %v", failures)
	}
	return out, nil
}

func (c *HTTPTransport) creature(ctx context.Context, name string) (Result, error) {
	if name == "" {
		return c.json(ctx, "GET", "/api/creature", nil)
	}
	if len(name) > 31 || strings.ContainsRune(name, '\x00') {
		return nil, fmt.Errorf("invalid creature name")
	}
	return c.json(ctx, "POST", "/api/creature", Result{"name": name})
}
func (c *HTTPTransport) Creature(ctx context.Context, name string) (Result, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.creature(ctx, name)
}
func (c *HTTPTransport) IMU(ctx context.Context) (Result, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.json(ctx, "GET", "/api/imu/snapshot", nil)
}
func (c *HTTPTransport) Volume(ctx context.Context, volume int) (Result, error) {
	if volume < 0 || volume > 21 {
		return nil, fmt.Errorf("volume must be 0..21")
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	data, err := c.device.Request(ctx, "POST", "/api/audio/volume", "text/plain", []byte(fmt.Sprint(volume)), 5*time.Second)
	if err != nil {
		return nil, err
	}
	return decode(data)
}
func (c *HTTPTransport) Show(ctx context.Context, text string, wpm int) (Result, error) {
	if err := validateText(text); err != nil {
		return nil, err
	}
	if wpm < 100 || wpm > 800 {
		return nil, fmt.Errorf("wpm must be 100..800")
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	if _, err := c.creature(ctx, "reader"); err != nil {
		return nil, err
	}
	if _, err := c.device.Request(ctx, "POST", "/api/reader/text", "text/plain; charset=utf-8", []byte(text), 5*time.Second); err != nil {
		return nil, err
	}
	return c.json(ctx, "POST", "/api/creature/state", Result{"wpm": wpm, "start_wpm": wpm, "end_wpm": wpm, "ramp_up": 0, "ramp_down": 0, "restart": true, "playing": true})
}
func (c *HTTPTransport) Events(ctx context.Context, n int, emit func(json.RawMessage) error) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.device.Events(ctx, n, emit)
}
