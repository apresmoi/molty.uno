package molty

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"
	"net/http/httptrace"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

const maxWAV = 4 * 1024 * 1024
const audioSlack = 10 * time.Second

func addRestoreWarning(result *Result, err error) {
	if err != nil {
		if *result == nil {
			*result = Result{}
		}
		(*result)["restore_warning"] = err.Error()
	}
}

type TTS interface {
	Synthesize(context.Context, string) ([]byte, error)
}
type STT interface {
	Transcribe(context.Context, []byte) (string, error)
}
type CommandTTS string
type CommandSTT string
type MacSay struct{}

type limitedBuffer struct {
	buffer bytes.Buffer
	limit  int
}

func (b *limitedBuffer) Write(p []byte) (int, error) {
	if len(p) > b.limit-b.buffer.Len() {
		return 0, fmt.Errorf("command output exceeds %d bytes", b.limit)
	}
	return b.buffer.Write(p)
}
func runCommand(ctx context.Context, cmd *exec.Cmd, input []byte, limit int) ([]byte, error) {
	ctx, cancel := context.WithTimeout(ctx, 2*time.Minute)
	defer cancel()
	cmd.Stdin = bytes.NewReader(input)
	var stdout = limitedBuffer{limit: limit}
	var stderr = limitedBuffer{limit: 8192}
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	cmd.WaitDelay = 2 * time.Second
	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("start speech command: %w", err)
	}
	done := make(chan struct{})
	go func() {
		select {
		case <-ctx.Done():
			_ = cmd.Process.Kill()
		case <-done:
		}
	}()
	err := cmd.Wait()
	close(done)
	if err != nil {
		return nil, fmt.Errorf("speech command: %w: %s", err, strings.TrimSpace(stderr.buffer.String()))
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	return stdout.buffer.Bytes(), nil
}
func (command CommandTTS) Synthesize(ctx context.Context, text string) ([]byte, error) {
	return runCommand(ctx, exec.CommandContext(ctx, "/bin/sh", "-c", string(command)), []byte(text), maxWAV)
}
func (command CommandSTT) Transcribe(ctx context.Context, wav []byte) (string, error) {
	data, err := runCommand(ctx, exec.CommandContext(ctx, "/bin/sh", "-c", string(command)), wav, 1024*1024)
	return strings.TrimSpace(string(data)), err
}
func (MacSay) Synthesize(ctx context.Context, text string) ([]byte, error) {
	dir, err := os.MkdirTemp("", "molty-tts-*")
	if err != nil {
		return nil, err
	}
	defer os.RemoveAll(dir)
	path := filepath.Join(dir, "speech.wav")
	_, err = runCommand(ctx, exec.CommandContext(ctx, "say", "-o", path, "--data-format=LEI16@16000", "--file-format=WAVE"), []byte(text), 8192)
	if err != nil {
		return nil, fmt.Errorf("macOS say unavailable or failed (use --tts-cmd or --wav): %w", err)
	}
	return readWAVFile(path)
}
func readWAVFile(path string) ([]byte, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	data, err := io.ReadAll(io.LimitReader(f, maxWAV+1))
	if err != nil {
		return nil, err
	}
	if len(data) > maxWAV {
		return nil, fmt.Errorf("WAV exceeds 4 MB")
	}
	return data, nil
}

type AudioStats struct {
	DurationMS float64 `json:"duration_ms"`
	Peak       float64 `json:"peak"`
	RMS        float64 `json:"rms"`
}

// canonicalWAV strips metadata to match the firmware's small WAV parser,
// which otherwise treats bytes after the data chunk as audio samples.
func canonicalWAV(data []byte) ([]byte, AudioStats, error) {
	var stats AudioStats
	fail := func() ([]byte, AudioStats, error) {
		return nil, stats, fmt.Errorf("WAV must be valid 16 kHz int16 mono PCM, nonempty, at most 4 MB")
	}
	if len(data) < 44 || len(data) > maxWAV || string(data[:4]) != "RIFF" || string(data[8:12]) != "WAVE" || uint64(binary.LittleEndian.Uint32(data[4:8]))+8 != uint64(len(data)) {
		return fail()
	}
	var pcm []byte
	validFmt := false
	for pos := 12; pos < len(data); {
		if len(data)-pos < 8 {
			return fail()
		}
		size := uint64(binary.LittleEndian.Uint32(data[pos+4 : pos+8]))
		end := uint64(pos) + 8 + size
		if end > uint64(len(data)) {
			return fail()
		}
		chunk := data[pos+8 : int(end)]
		switch string(data[pos : pos+4]) {
		case "fmt ":
			if validFmt || len(chunk) < 16 {
				return fail()
			}
			validFmt = binary.LittleEndian.Uint16(chunk) == 1 && binary.LittleEndian.Uint16(chunk[2:]) == 1 && binary.LittleEndian.Uint32(chunk[4:]) == 16000 && binary.LittleEndian.Uint32(chunk[8:]) == 32000 && binary.LittleEndian.Uint16(chunk[12:]) == 2 && binary.LittleEndian.Uint16(chunk[14:]) == 16
			if !validFmt {
				return fail()
			}
		case "data":
			if pcm != nil {
				return fail()
			}
			pcm = chunk
		}
		pos = int(end + size%2)
		if pos > len(data) {
			return fail()
		}
	}
	if !validFmt || len(pcm) == 0 || len(pcm)%2 != 0 {
		return fail()
	}
	wav := make([]byte, 44+len(pcm))
	copy(wav, "RIFF")
	binary.LittleEndian.PutUint32(wav[4:], uint32(len(wav)-8))
	copy(wav[8:], "WAVEfmt ")
	binary.LittleEndian.PutUint32(wav[16:], 16)
	binary.LittleEndian.PutUint16(wav[20:], 1)
	binary.LittleEndian.PutUint16(wav[22:], 1)
	binary.LittleEndian.PutUint32(wav[24:], 16000)
	binary.LittleEndian.PutUint32(wav[28:], 32000)
	binary.LittleEndian.PutUint16(wav[32:], 2)
	binary.LittleEndian.PutUint16(wav[34:], 16)
	copy(wav[36:], "data")
	binary.LittleEndian.PutUint32(wav[40:], uint32(len(pcm)))
	copy(wav[44:], pcm)
	sum := 0.0
	for i := 0; i < len(pcm); i += 2 {
		v := float64(int16(binary.LittleEndian.Uint16(pcm[i:]))) / 32768
		stats.Peak = math.Max(stats.Peak, math.Abs(v))
		sum += v * v
	}
	stats.DurationMS = float64(len(pcm)) / 32
	stats.RMS = math.Sqrt(sum / float64(len(pcm)/2))
	return wav, stats, nil
}

type SayOptions struct {
	Text  string
	WAV   string
	Voice string
	Async bool // Relay MCP submits without waiting; HTTP keeps synchronous playback.
}

func (c *HTTPTransport) Say(ctx context.Context, opts SayOptions) (result Result, err error) {
	if (strings.TrimSpace(opts.Text) == "") == (opts.WAV == "") {
		return nil, fmt.Errorf("provide exactly one of text or WAV path")
	}
	var wav []byte
	if opts.WAV != "" {
		wav, err = readWAVFile(opts.WAV)
	} else {
		wav, err = c.tts.Synthesize(ctx, opts.Text)
	}
	if err != nil {
		return nil, err
	}
	wav, stats, err := canonicalWAV(wav)
	if err != nil {
		return nil, err
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	restoreBudget := audioSlack
	restore, err := c.temporaryMood(ctx, "speaking")
	if restore != nil {
		defer func() { addRestoreWarning(&result, restore(restoreBudget)) }()
	}
	if err != nil {
		return nil, err
	}
	duration := time.Duration(stats.DurationMS * float64(time.Millisecond))
	data, restoreBudget, err := c.audioRequest(ctx, "POST", "/api/audio/play_wav", "audio/wav", wav, duration)
	if err != nil {
		return nil, err
	}
	result, err = decode(data)
	if err == nil {
		result["duration_ms"] = stats.DurationMS
	}
	return result, err
}

type ListenOptions struct {
	MS    int
	Out   string
	Force bool
	Async bool
}

func validateOutputPath(path string, force bool) error {
	if path == "" {
		return nil
	}
	if filepath.Ext(path) != ".wav" {
		return fmt.Errorf("output path requires a .wav suffix")
	}
	if !force {
		if _, err := os.Lstat(path); err == nil {
			return fmt.Errorf("output exists; use --force to replace it: %s", path)
		} else if !os.IsNotExist(err) {
			return err
		}
	}
	return nil
}

// Listen validates before capture. Publication itself must still refuse files
// created after that check, which is why a pre-rename existence check is unsafe.
func saveWAV(path string, wav []byte, force bool) (string, error) {
	dir := ""
	if path != "" {
		dir = filepath.Dir(path)
	}
	f, err := os.CreateTemp(dir, "molty-record-*.wav")
	if err != nil {
		return "", err
	}
	tmp := f.Name()
	keep := false
	defer func() {
		if !keep {
			_ = os.Remove(tmp)
		}
	}()
	if _, err = f.Write(wav); err != nil {
		f.Close()
		return "", err
	}
	if err = f.Close(); err != nil {
		return "", err
	}
	if path != "" {
		if force {
			err = os.Rename(tmp, path)
		} else {
			// Atomic no-replace publication, including files/symlinks created
			// after validation. Temp and destination share a filesystem.
			err = os.Link(tmp, path)
			if err == nil {
				_ = os.Remove(tmp)
			}
		}
		if err != nil {
			return "", err
		}
		tmp = path
	}
	keep = true
	return filepath.Abs(tmp)
}
func (c *HTTPTransport) Listen(ctx context.Context, opts ListenOptions) (result Result, err error) {
	if err := validateOutputPath(opts.Out, opts.Force); err != nil {
		return nil, err
	}
	if opts.MS < 1 || opts.MS > 10000 {
		return nil, fmt.Errorf("ms must be 1..10000")
	}
	var restoreErr error
	defer func() { addRestoreWarning(&result, restoreErr) }()
	c.mu.Lock()
	var wav []byte
	capture := func() (err error) {
		restoreBudget := audioSlack
		restore, err := c.temporaryMood(ctx, "listening")
		if restore != nil {
			defer func() { restoreErr = restore(restoreBudget) }()
		}
		if err != nil {
			return err
		}
		wav, restoreBudget, err = c.audioRequest(ctx, "GET", fmt.Sprintf("/api/audio/record?ms=%d&rate=16000", opts.MS), "", nil, time.Duration(opts.MS)*time.Millisecond)
		return err
	}
	captureErr := capture()
	c.mu.Unlock()
	if len(wav) == 0 && captureErr != nil {
		return nil, captureErr
	}
	wav, stats, err := canonicalWAV(wav)
	if err != nil {
		return nil, errors.Join(captureErr, err)
	}
	path, err := saveWAV(opts.Out, wav, opts.Force)
	if err != nil {
		return nil, errors.Join(captureErr, err)
	}
	result = Result{"ok": true, "path": path, "duration_ms": stats.DurationMS, "peak": stats.Peak, "rms": stats.RMS}
	if c.stt != nil {
		text, sttErr := c.stt.Transcribe(ctx, wav)
		if sttErr != nil {
			return result, errors.Join(captureErr, fmt.Errorf("recording saved at %s; STT failed: %w", path, sttErr))
		}
		result["text"] = text
	}
	return result, captureErr
}

// Firmware cannot cancel audio after receiving the request. Keep cancellation
// during upload, then drain its synchronous response with an independent bound.
// On a lost response, cleanup also has enough time to queue behind that audio.
func (c *HTTPTransport) audioRequest(ctx context.Context, method, path, contentType string, body []byte, duration time.Duration) ([]byte, time.Duration, error) {
	audioCtx, cancel := context.WithTimeout(context.WithoutCancel(ctx), duration+audioSlack)
	defer cancel()
	var mu sync.Mutex
	sent := false
	finish := time.Now().Add(duration)
	stop := context.AfterFunc(ctx, func() {
		mu.Lock()
		defer mu.Unlock()
		if !sent {
			cancel()
		}
	})
	defer stop()
	if ctx.Err() != nil {
		cancel()
	}
	trace := &httptrace.ClientTrace{WroteRequest: func(info httptrace.WroteRequestInfo) {
		if info.Err == nil {
			mu.Lock()
			sent = true
			finish = time.Now().Add(duration)
			mu.Unlock()
		}
	}}
	audioCtx = httptrace.WithClientTrace(audioCtx, trace)
	data, err := c.device.Request(audioCtx, method, path, contentType, body, duration+audioSlack)
	mu.Lock()
	remaining := max(time.Duration(0), time.Until(finish))
	mu.Unlock()
	return data, remaining + audioSlack, err
}
