package molty

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// Link is the persisted single-device relay connection.
type Link struct {
	Relay  string `json:"relay"`
	Device string `json:"device_id"`
	Token  string `json:"agent_token"`
}

// DefaultRelay is the CLI's default agent-link relay, used by `molty pair` when
// neither --relay nor MOLTY_RELAY is set. Same value as the firmware's
// CONFIG_MOLTY_DEFAULT_RELAY and the console's DEFAULT_RELAY (docs/AGENT-LINK.md).
const DefaultRelay = "https://molty-relay.alicenet.workers.dev"

func linkPath() (string, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(home, ".config", "molty", "link.json"), nil
}
func resolveConfig(cfg Config) (Config, error) {
	// An explicit direct URL remains an escape hatch from a saved relay link.
	if cfg.Relay == "" && (strings.HasPrefix(cfg.Device, "http://") || strings.HasPrefix(cfg.Device, "https://")) {
		return cfg, nil
	}
	relay, device, token := cfg.Relay, cfg.Device, cfg.Token
	if relay == "" {
		relay = os.Getenv("MOLTY_RELAY")
	}
	if device == "" {
		device = os.Getenv("MOLTY_DEVICE")
	}
	if token == "" {
		token = os.Getenv("MOLTY_TOKEN")
	}
	// A complete explicit/environment configuration does not depend on disk.
	if relay == "" || device == "" || token == "" {
		path, err := linkPath()
		if err != nil {
			return cfg, err
		}
		data, err := os.ReadFile(path)
		if err != nil && !os.IsNotExist(err) {
			return cfg, fmt.Errorf("read relay link: %w", err)
		}
		if err == nil {
			var link Link
			if err := json.Unmarshal(data, &link); err != nil {
				return cfg, fmt.Errorf("read relay link: %w", err)
			}
			if relay == "" {
				relay = link.Relay
			}
			if device == "" {
				device = link.Device
			}
			if token == "" {
				token = link.Token
			}
		}
	}
	cfg.Relay, cfg.Device, cfg.Token = relay, device, token
	return cfg, nil
}

// SaveLink atomically publishes credentials using a private temporary file.
func SaveLink(link Link) error {
	if link.Relay == "" || link.Device == "" || link.Token == "" {
		return fmt.Errorf("incomplete relay link")
	}
	path, err := linkPath()
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return err
	}
	file, err := os.CreateTemp(filepath.Dir(path), ".link-*")
	if err != nil {
		return err
	}
	defer os.Remove(file.Name())
	data, err := json.MarshalIndent(link, "", "  ")
	if err == nil {
		_, err = file.Write(append(data, '\n'))
	}
	if closeErr := file.Close(); err == nil {
		err = closeErr
	}
	if err != nil {
		return err
	}
	return os.Rename(file.Name(), path)
}

// PairAndSave waits for approval and saves credentials without printing the token.
func (c *Client) PairAndSave(ctx context.Context, code, name string) (Result, error) {
	result, err := c.Pair(ctx, code, name)
	if err != nil {
		return nil, err
	}
	relay, ok := c.Transport.(*RelayTransport)
	if !ok {
		return nil, fmt.Errorf("pair requires relay mode")
	}
	token, _ := result["agent_token"].(string)
	if err := SaveLink(Link{relay.base, relay.device, token}); err != nil {
		return nil, err
	}
	return Result{"ok": true, "device_id": relay.device, "saved": true}, nil
}
