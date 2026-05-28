// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Mock Pilot registry for libpilot C-integration tests.
//
// PilotEmbeddedStart boots a real in-process pkg/daemon. During Start()
// the daemon dials the registry (TCP, length-prefixed JSON) and runs a
// register round-trip plus a follow-up self-lookup. If either fails the
// whole Start() errors out and the //export entry point returns {"error":
// "..."} — which is exactly the path the existing harness's
// test_embedded_start_invalid_json already covers.
//
// To exercise the SUCCESS branch (and let PilotInfo/PilotHealth run
// against the embedded daemon afterwards) we need a registry that
// returns plausible canned responses. Spinning up the real
// rendezvous binary brings in disk persistence + WAL + Ed25519 admin
// auth — overkill for a coverage harness. Instead this binary speaks
// JUST the message types the embedded daemon hits during Start() and
// the first ~second of life, and returns canned data that satisfies
// the daemon's response shape checks.
//
// Wire format (matches pkg/registry/wire.WriteMessage/ReadMessage):
//   [4B big-endian length][JSON body]
//
// Dispatch is by the "type" string in the request JSON. Unknown types
// return {"error": "unknown type"} so the daemon's reconnect path
// isn't taken silently.
//
// On startup the binary listens on 127.0.0.1:<port>. If -port is 0
// (default) the OS picks one and we print "MOCK_REGISTRY_ADDR=host:port"
// on stdout so the harness can read it.

package main

import (
	"bufio"
	"encoding/binary"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"os/signal"
	"sync/atomic"
	"syscall"
)

// MaxMessageSize matches pkg/registry/wire.MaxMessageSize (64 MiB).
const maxMessageSize = 64 * 1024 * 1024

// Auto-incrementing fake node IDs handed out to register requests.
// 1 is reserved so the address looks like "1:0001.0000.0001" — a valid
// non-zero Pilot address. nodeIDCounter starts at 100 so the values
// don't collide with anything the harness might hard-code.
var nodeIDCounter atomic.Uint32

func init() {
	nodeIDCounter.Store(100)
}

func readMessage(r io.Reader) (map[string]interface{}, error) {
	var lenBuf [4]byte
	if _, err := io.ReadFull(r, lenBuf[:]); err != nil {
		return nil, err
	}
	length := binary.BigEndian.Uint32(lenBuf[:])
	if length == 0 {
		return nil, fmt.Errorf("zero-length frame")
	}
	if length > maxMessageSize {
		return nil, fmt.Errorf("frame too large: %d", length)
	}
	body := make([]byte, length)
	if _, err := io.ReadFull(r, body); err != nil {
		return nil, err
	}
	var msg map[string]interface{}
	if err := json.Unmarshal(body, &msg); err != nil {
		return nil, fmt.Errorf("decode: %w", err)
	}
	return msg, nil
}

func writeMessage(w io.Writer, msg map[string]interface{}) error {
	body, err := json.Marshal(msg)
	if err != nil {
		return err
	}
	var lenBuf [4]byte
	binary.BigEndian.PutUint32(lenBuf[:], uint32(len(body)))
	if _, err := w.Write(lenBuf[:]); err != nil {
		return err
	}
	_, err = w.Write(body)
	return err
}

// addressFor returns a daemon-parseable Pilot address string.
// Format: "N:NNNN.HHHH.LLLL" — network=1, node_id split into two
// uint16s big-endian.
func addressFor(nodeID uint32) string {
	high := uint16(nodeID >> 16)
	low := uint16(nodeID & 0xFFFF)
	return fmt.Sprintf("1:0001.%04X.%04X", high, low)
}

// dispatch handles a single decoded request and returns the response
// map. Reply shape is the union of every field the daemon's response
// readers parse; sending extras is harmless because json.Unmarshal into
// map[string]interface{} ignores them.
func dispatch(req map[string]interface{}) map[string]interface{} {
	msgType, _ := req["type"].(string)
	log.Printf("mock-registry: req type=%s", msgType)

	switch msgType {
	case "register":
		nodeID := nodeIDCounter.Add(1)
		return map[string]interface{}{
			"status":        "ok",
			"node_id":       nodeID,
			"address":       addressFor(nodeID),
			"observed_addr": "",
			// list-of-beacons hint the daemon's first beaconRefreshTick
			// may read; safe to return empty.
			"beacons": []interface{}{},
			// no networks at registration time
			"networks": []interface{}{},
		}

	case "heartbeat":
		// trustRepublishLoop runs the first heartbeat 0-5s after start.
		// In a tight harness lifecycle the test will Stop() the daemon
		// before this fires — but answer ok defensively.
		return map[string]interface{}{
			"status": "ok",
		}

	case "lookup":
		// Daemon calls Lookup(self) from nodeNetworksFresh during
		// loadNetworkPolicies. It only reads "networks". An empty list
		// is the right answer for a freshly registered node.
		nodeID, _ := req["node_id"].(float64)
		return map[string]interface{}{
			"status":   "ok",
			"node_id":  uint32(nodeID),
			"address":  addressFor(uint32(nodeID)),
			"networks": []interface{}{},
			"tags":     []interface{}{},
		}

	case "resolve":
		// Same shape as lookup with a real_addr field — the daemon
		// won't dial it during a bare Start, but other handles might.
		nodeID, _ := req["node_id"].(float64)
		return map[string]interface{}{
			"status":    "ok",
			"node_id":   uint32(nodeID),
			"address":   addressFor(uint32(nodeID)),
			"real_addr": "127.0.0.1:0",
			"networks":  []interface{}{},
		}

	case "set_visibility", "set_hostname", "deregister":
		return map[string]interface{}{"status": "ok"}

	case "list_beacons":
		return map[string]interface{}{
			"status":  "ok",
			"beacons": []interface{}{},
		}

	case "get_network_policy":
		// loadNetworkPolicies skips on err — return an empty policy.
		return map[string]interface{}{
			"status":        "ok",
			"allowed_ports": []interface{}{},
		}

	case "list_nodes":
		return map[string]interface{}{
			"status": "ok",
			"nodes":  []interface{}{},
		}

	default:
		// Unknown types: return an error envelope so the daemon's call
		// site sees a non-nil err and the loop continues. The registry
		// JSON convention is {"error": "<msg>"} (no "status").
		return map[string]interface{}{
			"error": fmt.Sprintf("mock-registry: unknown type %q", msgType),
		}
	}
}

func handleConn(c net.Conn) {
	defer c.Close()
	log.Printf("mock-registry: client connected from %s", c.RemoteAddr())
	defer log.Printf("mock-registry: client %s disconnected", c.RemoteAddr())

	// bufio.Reader smooths over small kernel buffer boundaries on the
	// 4-byte length prefix.
	br := bufio.NewReader(c)
	for {
		req, err := readMessage(br)
		if err != nil {
			if err != io.EOF {
				log.Printf("mock-registry: read: %v", err)
			}
			return
		}
		resp := dispatch(req)
		if err := writeMessage(c, resp); err != nil {
			log.Printf("mock-registry: write: %v", err)
			return
		}
	}
}

func main() {
	port := flag.Int("port", 0, "TCP port to listen on (0 = OS picks)")
	addrFile := flag.String("addr-file", "",
		"path to write the bound host:port after listen (default: stdout only)")
	flag.Parse()

	log.SetOutput(os.Stderr)
	log.SetPrefix("[mockregistry] ")

	addr := fmt.Sprintf("127.0.0.1:%d", *port)
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		log.Fatalf("listen %s: %v", addr, err)
	}
	defer ln.Close()

	// Tell the harness exactly which port we bound so it can build the
	// embedded config. Print on stdout (separate stream from log
	// output) and flush immediately. With -port=0 stdout is the only
	// way the parent learns the port — but stdout is buffered when the
	// parent dup2's it onto a pipe, and the harness wants a simple
	// poll-this-file pattern. -addr-file <path> writes the address
	// atomically (write to <path>.tmp, then rename) so the parent can
	// stat-then-read without a torn-write race.
	boundAddr := ln.Addr().String()
	fmt.Printf("MOCK_REGISTRY_ADDR=%s\n", boundAddr)
	os.Stdout.Sync()
	if *addrFile != "" {
		tmp := *addrFile + ".tmp"
		if err := os.WriteFile(tmp, []byte(boundAddr), 0o644); err != nil {
			log.Fatalf("write addr-file %s: %v", tmp, err)
		}
		if err := os.Rename(tmp, *addrFile); err != nil {
			log.Fatalf("rename addr-file %s: %v", *addrFile, err)
		}
	}
	log.Printf("listening on %s (pid=%d)", boundAddr, os.Getpid())

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-sigCh
		log.Println("shutting down")
		_ = ln.Close()
		os.Exit(0)
	}()

	for {
		c, err := ln.Accept()
		if err != nil {
			log.Printf("accept: %v", err)
			return
		}
		go handleConn(c)
	}
}
