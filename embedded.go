// SPDX-License-Identifier: AGPL-3.0-or-later

// Embedded daemon entry points.
//
// The other half of this package (bindings.go) is a thin RPC client
// that talks to an out-of-process daemon over a Unix socket. That
// model fits desktop SDKs (Python, Node) where the daemon is a
// separate long-running process. It does NOT fit iOS: one app =
// one process, no sibling daemon binary, no system-wide socket.
//
// PilotEmbeddedStart boots a daemon directly inside the host process
// (the goroutine model is unchanged from cmd/daemon). Internally it
// still opens a Unix socket so the existing 45 Pilot* RPC functions
// in bindings.go work unchanged — same wire protocol, just a
// different addressing space.
//
// Lifecycle: PilotEmbeddedStart → PilotConnect(socketPath) → use
// driver functions → PilotClose(handle) → PilotEmbeddedStop.

package main

/*
#include <stdlib.h>
#include <stdint.h>
*/
import "C"

import (
	"context"
	"encoding/json"
	"fmt"
	"path/filepath"
	"sync"
	"time"

	"github.com/TeoSlayer/pilotprotocol/pkg/daemon"

	"github.com/pilot-protocol/runtime"
)

type embeddedNode struct {
	d  *daemon.Daemon
	rt *runtime.Runtime
}

var embedded struct {
	sync.Mutex
	node *embeddedNode
}

// EmbeddedConfig is what callers supply via JSON in PilotEmbeddedStart.
// Keep it minimal and JSON-friendly so the Swift/Obj-C side can build
// it with a dictionary literal.
type embeddedConfig struct {
	DataDir          string `json:"data_dir"`           // absolute, host-writable
	SocketPath       string `json:"socket_path"`        // ≤ 100 bytes; use relative if abs is too long
	RegistryAddr     string `json:"registry_addr"`      // default 34.71.57.205:9000
	BeaconAddr       string `json:"beacon_addr"`        // default 34.71.57.205:9001
	TrustAutoApprove bool   `json:"trust_auto_approve"` // accept all incoming handshakes
	KeepaliveSec     int    `json:"keepalive_sec"`      // default 30; lower → faster handshake polling
	Version          string `json:"version"`            // surfaced in Info(); cosmetic
}

func (c *embeddedConfig) defaults() {
	if c.RegistryAddr == "" {
		c.RegistryAddr = "34.71.57.205:9000"
	}
	if c.BeaconAddr == "" {
		c.BeaconAddr = "34.71.57.205:9001"
	}
	if c.KeepaliveSec <= 0 {
		c.KeepaliveSec = 30
	}
	if c.Version == "" {
		c.Version = "embedded"
	}
}

// Boot the embedded daemon. configJSON is a JSON-encoded embeddedConfig.
// Returns JSON: on success {"address","node_id","public_key","socket"},
// on failure {"error": "..."}.
//
// Idempotent only in the trivial sense — calling twice without Stop
// returns an error.
//
//export PilotEmbeddedStart
func PilotEmbeddedStart(configJSON *C.char) *C.char {
	embedded.Lock()
	defer embedded.Unlock()
	if embedded.node != nil {
		return errJSON(fmt.Errorf("embedded daemon already started"))
	}

	var cfg embeddedConfig
	if err := unmarshalCString(configJSON, &cfg); err != nil {
		return errJSON(fmt.Errorf("parse config: %w", err))
	}
	cfg.defaults()
	if cfg.DataDir == "" {
		return errJSON(fmt.Errorf("data_dir required"))
	}
	if cfg.SocketPath == "" {
		return errJSON(fmt.Errorf("socket_path required"))
	}

	identityPath := filepath.Join(cfg.DataDir, "identity.json")

	// TODO(libpilot): the embedded daemon boot path depends on
	// d.DaemonAPI() (TeoSlayer/pilotprotocol#155 — "satisfy daemonapi.Daemon
	// via adapter") which has not landed on web4 main. Once #155 merges,
	// restore the daemon.New + runtime.New + plugin registration block
	// removed in this stub. Until then PilotEmbeddedStart returns a clear
	// runtime error so the C ABI surface keeps compiling.
	_ = identityPath
	return errJSON(fmt.Errorf("embedded daemon not implemented yet — blocked on web4 #155 (daemon.DaemonAPI adapter)"))
}

// Tear down the embedded daemon and all plugins. Safe to call when
// not started — returns {"status":"not_started"}.
//
//export PilotEmbeddedStop
func PilotEmbeddedStop() *C.char {
	embedded.Lock()
	n := embedded.node
	embedded.node = nil
	embedded.Unlock()

	if n == nil {
		return okJSON(map[string]string{"status": "not_started"})
	}

	if err := n.d.Stop(); err != nil {
		return errJSON(fmt.Errorf("daemon stop: %w", err))
	}
	stopCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := n.rt.StopPlugins(stopCtx); err != nil {
		// Not fatal — daemon is already down, plugin teardown is
		// best-effort. Surface the warning to the caller.
		return okJSON(map[string]string{
			"status":  "stopped",
			"warning": err.Error(),
		})
	}
	return okJSON(map[string]string{"status": "stopped"})
}

// unmarshalCString is a tiny helper to JSON-decode a C string into v.
func unmarshalCString(s *C.char, v interface{}) error {
	if s == nil {
		return fmt.Errorf("nil")
	}
	return json.Unmarshal([]byte(C.GoString(s)), v)
}
