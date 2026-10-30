package device

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"time"
)

const (
	defaultTimeout = 3 * time.Second
	pingTimeout    = 2 * time.Second
)

// Client is an HTTP client for a single Molty device.
type Client struct {
	baseURL    string
	httpClient *http.Client
}

// NewClient creates a Client that speaks to the device at baseURL
// (e.g. "http://molty.local" or "http://10.13.37.1").
func NewClient(baseURL string) *Client {
	return &Client{
		baseURL: baseURL,
		httpClient: &http.Client{
			Timeout: defaultTimeout,
		},
	}
}

// Ping checks whether the device is reachable by fetching /api/eye/state.
// Returns true if the request succeeds and the response is HTTP 2xx.
func (c *Client) Ping(ctx context.Context) bool {
	ctx, cancel := context.WithTimeout(ctx, pingTimeout)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, c.baseURL+"/api/eye/state", nil)
	if err != nil {
		return false
	}
	resp, err := c.httpClient.Do(req)
	if err != nil {
		return false
	}
	resp.Body.Close()
	return resp.StatusCode >= 200 && resp.StatusCode < 300
}

// GetEyeState fetches the full eye state from the device.
// Returns the raw JSON bytes so the server layer can forward them verbatim.
func (c *Client) GetEyeState(ctx context.Context) ([]byte, error) {
	ctx, cancel := context.WithTimeout(ctx, defaultTimeout)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, c.baseURL+"/api/eye/state", nil)
	if err != nil {
		return nil, fmt.Errorf("device.GetEyeState: build request: %w", err)
	}
	resp, err := c.httpClient.Do(req)
	if err != nil {
		return nil, fmt.Errorf("device.GetEyeState: do: %w", err)
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, fmt.Errorf("device.GetEyeState: read body: %w", err)
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return nil, fmt.Errorf("device.GetEyeState: device returned %d", resp.StatusCode)
	}
	return body, nil
}

// PostEyeState forwards a partial eye-state patch to the device.
// body is passed verbatim; returns the device's response JSON.
func (c *Client) PostEyeState(ctx context.Context, body []byte) ([]byte, error) {
	ctx, cancel := context.WithTimeout(ctx, defaultTimeout)
	defer cancel()

	// Validate that the body is at least valid JSON before forwarding.
	if !json.Valid(body) {
		return nil, fmt.Errorf("device.PostEyeState: body is not valid JSON")
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodPost, c.baseURL+"/api/eye/state", bytes.NewReader(body))
	if err != nil {
		return nil, fmt.Errorf("device.PostEyeState: build request: %w", err)
	}
	req.Header.Set("Content-Type", "application/json")

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return nil, fmt.Errorf("device.PostEyeState: do: %w", err)
	}
	defer resp.Body.Close()

	respBody, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, fmt.Errorf("device.PostEyeState: read response: %w", err)
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return nil, fmt.Errorf("device.PostEyeState: device returned %d: %s", resp.StatusCode, respBody)
	}
	return respBody, nil
}
