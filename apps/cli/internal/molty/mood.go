package molty

import (
	"context"
	"fmt"
	"time"
)

// MoodPlan can express either today's switch-and-patch mapping or a future
// firmware-native state patch (Creature empty means keep the active creature).
type MoodPlan struct {
	Creature string
	Patch    Result
}
type MoodMapper interface {
	Map(string) (MoodPlan, error)
}
type MappedMoods struct{}

func (MappedMoods) Map(mood string) (MoodPlan, error) {
	type pulse struct {
		amp, period float64
		color       string
	}
	pulses := map[string]pulse{
		"idle":      {.04, 3, "#d49070"},
		"listening": {.06, 1.5, "#60b8ff"},
		"thinking":  {.10, .8, "#b090ef"},
		"working":   {.10, .8, "#b090ef"},
		"speaking":  {.14, .5, "#70d8a0"},
		"happy":     {.12, 1, "#ffce60"},
	}
	if p, ok := pulses[mood]; ok {
		return MoodPlan{"claude", Result{"pulse_amp": p.amp, "pulse_period_s": p.period, "body_color": p.color, "bg_color": "#101018", "eye_color": "#101018", "scale": 0.7, "dynamics": true}}, nil
	}
	if mood == "sleepy" || mood == "surprised" {
		pupil, blink, inner, outer := .20, .75, "#8090c0", "#303850"
		if mood == "surprised" {
			pupil, blink, inner, outer = .75, 0, "#ffe090", "#b07030"
		}
		return MoodPlan{"eye", Result{"pupil_size": pupil, "blink": blink, "x": 0, "y": 0, "autoblink": mood != "sleepy", "dynamics": false, "iris": Result{"inner_color": inner, "outer_color": outer}}}, nil
	}
	return MoodPlan{}, fmt.Errorf("unknown mood %q; use idle, listening, thinking, speaking, happy, sleepy, surprised", mood)
}

func (c *HTTPTransport) mood(ctx context.Context, mood string) (Result, error) {
	p, err := c.mapper.Map(mood)
	if err != nil {
		return nil, err
	}
	if p.Creature != "" {
		if _, err = c.creature(ctx, p.Creature); err != nil {
			return nil, err
		}
	}
	state, err := c.json(ctx, "POST", "/api/creature/state", p.Patch)
	if err != nil {
		return nil, err
	}
	return Result{"ok": true, "mood": mood, "creature": p.Creature, "state": state}, nil
}
func (c *HTTPTransport) Mood(ctx context.Context, mood string) (Result, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.mood(ctx, mood)
}

// Snapshot the actual creature/state rather than trusting process-local mood
// bookkeeping; this works across independent CLI invocations as well.
func (c *HTTPTransport) temporaryMood(ctx context.Context, mood string) (func(time.Duration) error, error) {
	creature, err := c.creature(ctx, "")
	if err != nil {
		return nil, err
	}
	name, ok := creature["current"].(string)
	if !ok || name == "" {
		return nil, fmt.Errorf("device has no current creature")
	}
	var state Result
	// These GETs describe a frame, script, or running scene, rather than a
	// writable snapshot. Reposting world.scene also reloads its animation.
	switch name {
	case "glyph", "ui", "world":
	default:
		state, err = c.json(ctx, "GET", "/api/creature/state", nil)
		if err != nil {
			return nil, err
		}
		delete(state, "ok")
	}
	restore := func(timeout time.Duration) error {
		// Restoration must still run after caller cancellation, but remain bounded.
		cleanup, cancel := context.WithTimeout(context.WithoutCancel(ctx), timeout)
		defer cancel()
		if _, err := c.jsonTimeout(cleanup, "POST", "/api/creature", Result{"name": name}, timeout); err != nil {
			return fmt.Errorf("restore creature: %w", err)
		}
		if state == nil {
			return nil
		}
		if _, err := c.jsonTimeout(cleanup, "POST", "/api/creature/state", state, timeout); err != nil {
			return fmt.Errorf("restore state: %w", err)
		}
		return nil
	}
	if _, err := c.mood(ctx, mood); err != nil {
		return restore, err
	}
	return restore, nil
}
