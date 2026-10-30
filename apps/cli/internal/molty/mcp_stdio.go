package molty

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"reflect"
	"sort"
	"strings"
	"sync"
	"time"
)

// Minimal offline stdio MCP transport. No HTTP listener, resources, prompts,
// command execution tools or arbitrary device endpoint access are exposed.
type mcpTool struct {
	Name        string `json:"name"`
	Description string `json:"description"`
	InputSchema Result `json:"inputSchema"`
	call        func(context.Context, json.RawMessage) (Result, error)
}

func addTool[In any](tools map[string]mcpTool, name, description string, call func(context.Context, In) (Result, error)) {
	typ := reflect.TypeFor[In]()
	properties := Result{}
	required := []string{}
	for i := 0; i < typ.NumField(); i++ {
		f := typ.Field(i)
		parts := strings.Split(f.Tag.Get("json"), ",")
		key := parts[0]
		ft := f.Type
		if ft.Kind() == reflect.Pointer {
			ft = ft.Elem()
		}
		kind := "string"
		if ft.Kind() == reflect.Map {
			kind = "object"
		}
		if ft.Kind() == reflect.Int {
			kind = "integer"
		}
		properties[key] = Result{"type": kind, "description": f.Tag.Get("jsonschema")}
		if len(parts) == 1 {
			required = append(required, key)
		}
	}
	tools[name] = mcpTool{Name: name, Description: description, InputSchema: Result{"type": "object", "properties": properties, "required": required, "additionalProperties": false}, call: func(ctx context.Context, args json.RawMessage) (Result, error) {
		if len(args) == 0 {
			args = json.RawMessage(`{}`)
		}
		var object map[string]json.RawMessage
		if err := json.Unmarshal(args, &object); err != nil || object == nil {
			return nil, fmt.Errorf("arguments must be an object")
		}
		for key, value := range object {
			if _, ok := properties[key]; !ok {
				return nil, fmt.Errorf("unknown argument %s", key)
			}
			if bytes.Equal(bytes.TrimSpace(value), []byte("null")) {
				return nil, fmt.Errorf("%s must not be null", key)
			}
		}
		for _, key := range required {
			if _, ok := object[key]; !ok {
				return nil, fmt.Errorf("missing required argument %s", key)
			}
		}
		var in In
		dec := json.NewDecoder(bytes.NewReader(args))
		dec.DisallowUnknownFields()
		if err := dec.Decode(&in); err != nil {
			return nil, fmt.Errorf("invalid arguments: %w", err)
		}
		return call(ctx, in)
	}}
}

type rpcRequest struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      json.RawMessage `json:"id"`
	Method  string          `json:"method"`
	Params  json.RawMessage `json:"params"`
}

func rpcID(id json.RawMessage) (string, bool) {
	var value any
	d := json.NewDecoder(bytes.NewReader(id))
	d.UseNumber()
	if d.Decode(&value) != nil {
		return "", false
	}
	switch value.(type) {
	case string, json.Number:
		canonical, _ := json.Marshal(value)
		return string(canonical), true
	}
	return "", false
}

// ServeMCP serves newline-delimited JSON-RPC 2.0. Tool calls run asynchronously
// so notifications/cancelled can interrupt long synchronous device operations.
// A closeable input is closed on cancellation to unblock a pending read.
func (c *Client) ServeMCP(ctx context.Context, input io.Reader, output io.Writer) error {
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	tools := c.mcpTools()
	if closer, ok := input.(io.ReadCloser); ok {
		done := make(chan struct{})
		defer close(done)
		go func() {
			select {
			case <-ctx.Done():
				_ = closer.Close()
			case <-done:
			}
		}()
	}
	var writeMu sync.Mutex
	var writeErr error
	send := func(id json.RawMessage, result any, code int, message string) {
		writeMu.Lock()
		defer writeMu.Unlock()
		if writeErr != nil || ctx.Err() != nil {
			return
		}
		if len(id) == 0 {
			id = json.RawMessage(`null`)
		}
		reply := Result{"jsonrpc": "2.0", "id": id}
		if code != 0 {
			reply["error"] = Result{"code": code, "message": message}
		} else {
			reply["result"] = result
		}
		writeErr = json.NewEncoder(output).Encode(reply)
		if writeErr != nil {
			cancel()
		}
	}
	var pendingMu sync.Mutex
	pending := map[string]context.CancelFunc{}
	var wg sync.WaitGroup

	initialized := false
	scanner := bufio.NewScanner(input)
	scanner.Buffer(make([]byte, 4096), 256*1024)
	for scanner.Scan() {
		if ctx.Err() != nil {
			break
		}
		line := scanner.Bytes()
		if !json.Valid(line) {
			send(nil, nil, -32700, "Parse error")
			continue
		}
		var req rpcRequest
		if err := json.Unmarshal(line, &req); err != nil || req.JSONRPC != "2.0" || req.Method == "" {
			send(nil, nil, -32600, "Invalid Request")
			continue
		}
		notification := len(req.ID) == 0
		key, valid := rpcID(req.ID)
		if !notification && !valid {
			send(nil, nil, -32600, "Invalid request ID")
			continue
		}
		if notification {
			if req.Method == "notifications/cancelled" {
				var p struct {
					RequestID json.RawMessage `json:"requestId"`
				}
				if json.Unmarshal(req.Params, &p) == nil {
					if k, ok := rpcID(p.RequestID); ok {
						pendingMu.Lock()
						if stop := pending[k]; stop != nil {
							stop()
						}
						pendingMu.Unlock()
					}
				}
			}
			continue
		}
		if req.Method == "ping" {
			send(req.ID, Result{}, 0, "")
			continue
		}
		if req.Method == "initialize" {
			var p struct {
				ProtocolVersion string `json:"protocolVersion"`
			}
			if json.Unmarshal(req.Params, &p) != nil || p.ProtocolVersion == "" {
				send(req.ID, nil, -32602, "protocolVersion required")
				continue
			}
			if initialized {
				send(req.ID, nil, -32600, "Already initialized")
				continue
			}
			version := p.ProtocolVersion
			switch version {
			case "2025-06-18", "2025-11-25":
			default:
				version = "2025-11-25"
			}
			initialized = true
			send(req.ID, Result{"protocolVersion": version, "capabilities": Result{"tools": Result{}}, "serverInfo": Result{"name": "molty", "version": Version}, "instructions": "Tools operate a physical Molty device. Calls serialize; audio blocks the device HTTP server. Paths refer to this server machine."}, 0, "")
			continue
		}
		if !initialized {
			send(req.ID, nil, -32002, "Initialize first")
			continue
		}
		switch req.Method {
		case "tools/list":
			names := make([]string, 0, len(tools))
			for name := range tools {
				names = append(names, name)
			}
			sort.Strings(names)
			list := make([]mcpTool, 0, len(names))
			for _, name := range names {
				list = append(list, tools[name])
			}
			send(req.ID, Result{"tools": list}, 0, "")
		case "tools/call":
			var p struct {
				Name      string          `json:"name"`
				Arguments json.RawMessage `json:"arguments"`
			}
			if json.Unmarshal(req.Params, &p) != nil {
				send(req.ID, nil, -32602, "Invalid tool parameters")
				continue
			}
			tool, ok := tools[p.Name]
			if !ok {
				send(req.ID, nil, -32602, "Unknown tool")
				continue
			}
			pendingMu.Lock()
			if pending[key] != nil || len(pending) >= 64 {
				pendingMu.Unlock()
				send(req.ID, nil, -32600, "Duplicate ID or too many pending calls")
				continue
			}
			callCtx, stop := context.WithCancel(ctx)
			pending[key] = stop
			pendingMu.Unlock()
			wg.Add(1)
			go func(id json.RawMessage, key string, tool mcpTool, args json.RawMessage) {
				defer wg.Done()
				defer stop()
				defer func() { pendingMu.Lock(); delete(pending, key); pendingMu.Unlock() }()
				value, err := tool.call(callCtx, args)
				if value == nil {
					value = Result{}
				}
				if err != nil {
					value["ok"] = false
					value["error"] = err.Error()
				}
				text, marshalErr := json.Marshal(value)
				if marshalErr != nil {
					send(id, nil, -32603, marshalErr.Error())
					return
				}
				send(id, Result{"isError": err != nil, "structuredContent": value, "content": []Result{{"type": "text", "text": string(text)}}}, 0, "")
			}(append(json.RawMessage(nil), req.ID...), key, tool, p.Arguments)
		default:
			send(req.ID, nil, -32601, "Method not found")
		}
	}
	inputErr := scanner.Err()
	wasCanceled := ctx.Err() != nil
	// EOF is a half-close: finish accepted calls before shutting down.
	// A caller cancellation or malformed input may still stop pre-upload work.
	if inputErr != nil || wasCanceled {
		cancel()
	}
	drained := make(chan struct{})
	go func() { wg.Wait(); close(drained) }()
	timer := time.NewTimer(150 * time.Second)
	defer timer.Stop()
	select {
	case <-drained:
	case <-timer.C:
		cancel()
		return fmt.Errorf("MCP in-flight calls did not drain within 150s")
	}
	if inputErr != nil && !wasCanceled {
		return fmt.Errorf("MCP input: %w", inputErr)
	}
	writeMu.Lock()
	defer writeMu.Unlock()
	if writeErr != nil {
		return fmt.Errorf("MCP output: %w", writeErr)
	}
	return nil
}
