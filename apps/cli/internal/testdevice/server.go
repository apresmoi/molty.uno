// Package testdevice provides httptest servers over net.Pipe for environments
// that prohibit binding TCP ports. It is imported only by tests. Requests still
// traverse net/http's real client/server and websocket upgrade implementations.
package testdevice

import (
	"context"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"sync"
	"sync/atomic"
)

var registry sync.Map
var sequence atomic.Uint64
var install sync.Once

type memoryAddr string

func (a memoryAddr) Network() string { return "pipe" }
func (a memoryAddr) String() string  { return string(a) }

type listener struct {
	addr     memoryAddr
	incoming chan net.Conn
	closed   chan struct{}
	once     sync.Once
}

func (l *listener) Addr() net.Addr { return l.addr }
func (l *listener) Close() error {
	l.once.Do(func() { registry.Delete(string(l.addr)); close(l.closed) })
	return nil
}
func (l *listener) Accept() (net.Conn, error) {
	select {
	case conn := <-l.incoming:
		return conn, nil
	case <-l.closed:
		return nil, net.ErrClosed
	}
}
func dial(ctx context.Context, _, addr string) (net.Conn, error) {
	value, ok := registry.Load(addr)
	if !ok {
		return nil, fmt.Errorf("test device %s unreachable", addr)
	}
	l := value.(*listener)
	client, server := net.Pipe()
	select {
	case l.incoming <- server:
		return client, nil
	case <-l.closed:
		client.Close()
		server.Close()
		return nil, net.ErrClosed
	case <-ctx.Done():
		client.Close()
		server.Close()
		return nil, ctx.Err()
	}
}
func NewServer(handler http.Handler) *httptest.Server {
	// Fail closed for unknown hosts: these tests cannot accidentally dial a board.
	install.Do(func() { http.DefaultTransport = &http.Transport{DialContext: dial} })
	addr := memoryAddr(fmt.Sprintf("molty-test-%d.invalid:80", sequence.Add(1)))
	l := &listener{addr: addr, incoming: make(chan net.Conn), closed: make(chan struct{})}
	registry.Store(string(addr), l)
	server := &httptest.Server{Listener: l, Config: &http.Server{Handler: handler}}
	server.Start()
	server.Client().Transport = &http.Transport{DialContext: dial}
	return server
}
