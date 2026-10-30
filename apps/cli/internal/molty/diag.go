package molty

import (
	"context"
	"fmt"
	"strconv"
	"strings"
	"time"
)

// Diagnostics verbs: the device's /api/diag snapshot and its log ring.
// Log results are {next, text}; pass next back as since to continue a tail.

const (
	MaxDiagTimeline = 180
	MaxLogBytes     = 32768
	DefaultLogBytes = 12288
)

func validateDiag(timeline int) error {
	if timeline < 0 || timeline > MaxDiagTimeline {
		return fmt.Errorf("timeline must be 0..%d seconds", MaxDiagTimeline)
	}
	return nil
}

func validateLog(since, max int) error {
	if since < 0 {
		return fmt.Errorf("since must be >= 0")
	}
	if max != 0 && (max < 1024 || max > MaxLogBytes) {
		return fmt.Errorf("max must be 1024..%d bytes", MaxLogBytes)
	}
	return nil
}

func (c *HTTPTransport) Diag(ctx context.Context, timeline int) (Result, error) {
	if err := validateDiag(timeline); err != nil {
		return nil, err
	}
	path := "/api/diag"
	if timeline > 0 {
		path += "?timeline=" + strconv.Itoa(timeline)
	}
	return c.json(ctx, "GET", path, nil)
}

func (c *HTTPTransport) Log(ctx context.Context, since, max int) (Result, error) {
	if err := validateLog(since, max); err != nil {
		return nil, err
	}
	if max == 0 {
		max = DefaultLogBytes
	}
	data, err := c.device.Request(ctx, "GET", fmt.Sprintf("/api/log?since=%d&max=%d", since, max), "", nil, 10*time.Second)
	if err != nil {
		return nil, err
	}
	text := string(data)
	return Result{"next": nextSeq(text, since), "text": text}, nil
}

// nextSeq derives the continuation point from the last "<seq> <s.ms> ..." line.
func nextSeq(text string, since int) int {
	lines := strings.Split(strings.TrimRight(text, "\n"), "\n")
	for i := len(lines) - 1; i >= 0; i-- {
		if f := strings.Fields(lines[i]); len(f) > 1 {
			if n, err := strconv.Atoi(f[0]); err == nil && n >= since {
				return n + 1
			}
		}
	}
	return since
}

func (r *RelayTransport) Diag(ctx context.Context, timeline int) (Result, error) {
	if err := validateDiag(timeline); err != nil {
		return nil, err
	}
	args := Result{}
	if timeline > 0 {
		args["timeline"] = timeline
	}
	return r.command(ctx, "diag", args)
}

func (r *RelayTransport) Log(ctx context.Context, since, max int) (Result, error) {
	if err := validateLog(since, max); err != nil {
		return nil, err
	}
	args := Result{}
	if since > 0 {
		args["since"] = since
	}
	if max > 0 {
		args["max"] = max
	}
	return r.command(ctx, "log", args)
}
