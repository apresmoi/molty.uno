package molty

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
	"time"
)

// RelayTransport uses the agent REST surface; it never contacts local firmware
// or starts local speech processes. Credentials are never sent on redirects.
type RelayTransport struct {
	base, device, token string
	http                *http.Client
}

func NewRelayTransport(base, device, token string, client *http.Client) (*RelayTransport, error) {
	u, err := url.Parse(base)
	if err != nil || u.Host == "" || (u.Scheme != "http" && u.Scheme != "https") || u.User != nil || u.RawQuery != "" || u.Fragment != "" {
		return nil, fmt.Errorf("invalid relay URL")
	}
	if device == "" || strings.ContainsAny(device, "/?#\\\x00") || device == "." || device == ".." {
		return nil, fmt.Errorf("relay requires a valid --device ID")
	}
	if strings.ContainsAny(token, "\r\n") {
		return nil, fmt.Errorf("invalid relay token")
	}
	if client == nil {
		client = &http.Client{}
	}
	hc := *client
	hc.CheckRedirect = func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }
	return &RelayTransport{strings.TrimRight(base, "/"), device, token, &hc}, nil
}

func (r *RelayTransport) request(ctx context.Context, method, path string, body any, auth bool) (Result, error) {
	if auth && r.token == "" {
		return nil, fmt.Errorf("relay authentication requires --token, MOLTY_TOKEN, or a paired link")
	}
	var data []byte
	var err error
	if body != nil {
		data, err = json.Marshal(body)
		if err != nil {
			return nil, err
		}
	}
	req, err := http.NewRequestWithContext(ctx, method, r.base+path, bytes.NewReader(data))
	if err != nil {
		return nil, err
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	if auth {
		req.Header.Set("Authorization", "Bearer "+r.token)
	}
	resp, err := r.http.Do(req)
	if err != nil {
		return nil, fmt.Errorf("relay %s: %w", path, err)
	}
	defer resp.Body.Close()
	data, err = io.ReadAll(io.LimitReader(resp.Body, 64*1024+1))
	if err != nil {
		return nil, fmt.Errorf("relay response: %w", err)
	}
	if len(data) > 64*1024 {
		return nil, fmt.Errorf("relay response exceeds 64 KB")
	}
	if resp.StatusCode == 401 || resp.StatusCode == 403 {
		return nil, fmt.Errorf("relay authentication rejected (HTTP %d): %s", resp.StatusCode, data)
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return nil, fmt.Errorf("relay HTTP %d: %s", resp.StatusCode, data)
	}
	out, err := decode(data)
	if err != nil {
		return nil, err
	}
	if fmt.Sprint(out["code"]) == "4401" || fmt.Sprint(out["error"]) == "4401" {
		return nil, fmt.Errorf("relay authentication rejected (4401)")
	}
	if out["ok"] == false || out["error"] != nil {
		return nil, fmt.Errorf("relay error: %v", out)
	}
	return out, nil
}
func (r *RelayTransport) command(ctx context.Context, name string, args Result) (Result, error) {
	ctx, cancel := context.WithTimeout(ctx, 8*time.Second)
	defer cancel()
	if args == nil {
		args = Result{}
	}
	out, err := r.request(ctx, "POST", "/v1/devices/"+url.PathEscape(r.device)+"/cmd", Result{"name": name, "args": args}, true)
	if err != nil {
		return nil, err
	}
	// Accept direct results as well as the device res envelope.
	if value, ok := out["result"].(map[string]any); ok {
		return value, nil
	}
	return out, nil
}
func (r *RelayTransport) Status(ctx context.Context) (Result, error) {
	return r.command(ctx, "status", nil)
}
func (r *RelayTransport) IMU(ctx context.Context) (Result, error) { return r.command(ctx, "imu", nil) }
func (*RelayTransport) Creature(context.Context, string) (Result, error) {
	return nil, fmt.Errorf("creature list/switch is not specified by relay v0; use state")
}
func (*RelayTransport) Events(context.Context, int, func(json.RawMessage) error) error {
	return fmt.Errorf("events are not specified by relay REST v0")
}
func (r *RelayTransport) State(ctx context.Context, patch Result) (Result, error) {
	return r.command(ctx, "state", patch)
}
func (r *RelayTransport) Mood(ctx context.Context, mood string) (Result, error) {
	if _, err := (MappedMoods{}).Map(mood); err != nil {
		return nil, err
	}
	return r.command(ctx, "mood", Result{"mood": mood})
}
func (r *RelayTransport) Show(ctx context.Context, text string, wpm int) (Result, error) {
	if err := validateText(text); err != nil {
		return nil, err
	}
	if wpm < 100 || wpm > 800 {
		return nil, fmt.Errorf("wpm must be 100..800")
	}
	return r.command(ctx, "show", Result{"text": text, "wpm": wpm})
}
func (r *RelayTransport) Volume(ctx context.Context, volume int) (Result, error) {
	if volume < 0 || volume > 21 {
		return nil, fmt.Errorf("volume must be 0..21")
	}
	return r.command(ctx, "volume", Result{"volume": volume})
}
func (r *RelayTransport) audio(ctx context.Context, name string, args Result, async bool) (Result, error) {
	// A caller's earlier deadline always wins, including submission time.
	if _, bounded := ctx.Deadline(); !bounded {
		var cancel context.CancelFunc
		ctx, cancel = context.WithTimeout(ctx, 2*time.Minute)
		defer cancel()
	}
	out, err := r.command(ctx, name, args)
	if err != nil {
		return nil, err
	}
	job, ok := out["job"].(string)
	if !ok || job == "" {
		return nil, fmt.Errorf("relay %s did not return a job", name)
	}
	if async {
		return out, nil
	}
	for {
		out, err = r.Wait(ctx, job, 8*time.Second)
		if err != nil {
			return Result{"job": job}, err
		}
		if out["status"] != "pending" {
			return out, nil
		}
	}
}
func (r *RelayTransport) Say(ctx context.Context, opts SayOptions) (Result, error) {
	if opts.WAV != "" {
		return nil, fmt.Errorf("relay say accepts text, not local WAV files")
	}
	if err := validateText(opts.Text); err != nil {
		return nil, err
	}
	args := Result{"text": opts.Text}
	if opts.Voice != "" {
		args["voice"] = opts.Voice
	}
	return r.audio(ctx, "say", args, opts.Async)
}
func (r *RelayTransport) Listen(ctx context.Context, opts ListenOptions) (Result, error) {
	if opts.MS < 1 || opts.MS > 10000 {
		return nil, fmt.Errorf("ms must be 1..10000")
	}
	if opts.Out != "" || opts.Force {
		return nil, fmt.Errorf("relay listen returns text; --out and --force require HTTP mode")
	}
	return r.audio(ctx, "listen", Result{"ms": opts.MS}, opts.Async)
}

// Wait polls for at most eight seconds. Its own budget expiry is pending;
// cancellation or expiry of the caller's context remains an error.
func (r *RelayTransport) Wait(ctx context.Context, job string, timeout time.Duration) (Result, error) {
	if job == "" || job == "." || job == ".." || strings.ContainsAny(job, "/?#\\\x00") {
		return nil, fmt.Errorf("invalid job ID")
	}
	if timeout < 0 || timeout > 8*time.Second {
		return nil, fmt.Errorf("timeout_s must be 0..8")
	}
	pending := Result{"job": job, "status": "pending"}
	if timeout == 0 {
		return pending, ctx.Err()
	}
	pollCtx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	for {
		out, err := r.request(pollCtx, "GET", "/v1/jobs/"+url.PathEscape(job), nil, true)
		if err != nil {
			if ctx.Err() != nil {
				return nil, ctx.Err()
			}
			if errors.Is(pollCtx.Err(), context.DeadlineExceeded) {
				return pending, nil
			}
			return nil, err
		}
		status, _ := out["status"].(string)
		switch status {
		case "done", "completed":
			if result, ok := out["result"].(map[string]any); ok {
				return result, nil
			}
			return out, nil
		case "failed", "error", "cancelled":
			return nil, fmt.Errorf("relay job %s: %v", job, out)
		case "pending", "queued", "running":
		default:
			return nil, fmt.Errorf("relay job %s: missing or unknown status %q", job, status)
		}
		timer := time.NewTimer(100 * time.Millisecond)
		select {
		case <-pollCtx.Done():
			timer.Stop()
			if ctx.Err() != nil {
				return nil, ctx.Err()
			}
			return pending, nil
		case <-timer.C:
		}
	}
}
func (r *RelayTransport) Pair(ctx context.Context, code, name string) (Result, error) {
	if len(code) != 8 || strings.Trim(code, "ABCDEFGHIJKLMNOPQRSTUVWXYZ23456789") != "" {
		return nil, fmt.Errorf("link code must be 8 characters A-Z or 2-9")
	}
	if strings.TrimSpace(name) == "" {
		return nil, fmt.Errorf("agent name is required")
	}
	ctx, cancel := context.WithTimeout(ctx, 120*time.Second)
	defer cancel()
	out, err := r.request(ctx, "POST", "/v1/pair", Result{"device_id": r.device, "link_code": code, "agent_name": name}, false)
	if err != nil {
		return nil, err
	}
	token, _ := out["agent_token"].(string)
	if token == "" || out["device_id"] != r.device {
		return nil, fmt.Errorf("relay pair returned missing token or mismatched device_id")
	}
	return out, nil
}
