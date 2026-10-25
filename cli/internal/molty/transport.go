package molty

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"time"
	"unicode/utf8"
)

// Transport owns complete device verbs, including transport-specific audio.
// Adapters use Client so neither CLI nor MCP duplicates routing or backends.
type Transport interface {
	Status(context.Context) (Result, error)
	Creature(context.Context, string) (Result, error)
	Mood(context.Context, string) (Result, error)
	Show(context.Context, string, int) (Result, error)
	Say(context.Context, SayOptions) (Result, error)
	Listen(context.Context, ListenOptions) (Result, error)
	Volume(context.Context, int) (Result, error)
	IMU(context.Context) (Result, error)
	Diag(ctx context.Context, timeline int) (Result, error)
	Log(ctx context.Context, since, max int) (Result, error)
	State(context.Context, Result) (Result, error)
	Events(context.Context, int, func(json.RawMessage) error) error
	Wait(context.Context, string, time.Duration) (Result, error)
	Pair(context.Context, string, string) (Result, error)
}

type Client struct{ Transport }

func New(cfg Config) (*Client, error) {
	if cfg.Transport != nil {
		return &Client{cfg.Transport}, nil
	}
	cfg, err := resolveConfig(cfg)
	if err != nil {
		return nil, err
	}
	var transport Transport
	if cfg.Relay != "" {
		transport, err = NewRelayTransport(cfg.Relay, cfg.Device, cfg.Token, cfg.HTTPClient)
	} else {
		transport, err = NewHTTPTransport(cfg)
	}
	if err != nil {
		return nil, err
	}
	return &Client{transport}, nil
}

func (c *HTTPTransport) State(ctx context.Context, patch Result) (Result, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if patch == nil {
		return c.json(ctx, "GET", "/api/creature/state", nil)
	}
	return c.json(ctx, "POST", "/api/creature/state", patch)
}
func (*HTTPTransport) Wait(context.Context, string, time.Duration) (Result, error) {
	return nil, fmt.Errorf("job polling requires relay mode")
}
func (*HTTPTransport) Pair(context.Context, string, string) (Result, error) {
	return nil, fmt.Errorf("pair requires --relay URL and --device ID")
}

var _ Transport = (*HTTPTransport)(nil)
var _ Transport = (*RelayTransport)(nil)

func validateText(text string) error {
	if strings.TrimSpace(text) == "" || !utf8.ValidString(text) || strings.ContainsRune(text, '\x00') || len(text) > 16*1024 {
		return fmt.Errorf("text must be nonempty UTF-8, without NUL, at most 16 KB")
	}
	return nil
}
