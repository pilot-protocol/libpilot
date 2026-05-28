// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Mock Pilot daemon for libpilot C-integration tests.
//
// This binary speaks just enough of the daemon IPC wire protocol to
// satisfy the //export entry points in libpilot's bindings.go. It is
// NOT a real daemon — there is no overlay traffic, no STUN, no
// registry. Every reply is canned. The harness uses it to drive code
// paths that need a real Unix socket and a real reply on the wire.
//
// Wire format (matches pkg/driver/ipc.go and pkg/daemon/ipc.go):
//   [uint32 BE length][uint8 cmd][payload...]
//
// There is no protocol-level handshake on a fresh connection. The
// driver's newIPCClient simply dials the socket and starts sending
// frames. No Ed25519 identity is exchanged at the IPC layer (identity
// lives at the daemon-to-daemon overlay layer, not on the local socket).
//
// Commands we respond to:
//   0x01 Bind            → 0x02 BindOK     [port(2)]
//                          (also records the bound port in a per-client
//                          set + the process-wide listenerRegistry so
//                          a peer dial can drive cmdAccept)
//   0x03 Dial            → 0x04 DialOK     [connID(4)]
//                          (if the dialed port has any listener in
//                          listenerRegistry, also pushes cmdAccept to
//                          that listener — covers PilotListenerAccept)
//   0x06 Send            → server-pushed 0x07 Recv echo
//   0x08 Close           → fire-and-forget; we silently drop
//   0x0B SendTo          → server-pushed 0x0C RecvFrom loopback echo
//                          (covers PilotRecvFrom)
//   0x0D Info            → 0x0E InfoOK     [JSON]
//   0x0F Handshake       → 0x10 HandshakeOK [JSON]  (sub-cmd dispatched)
//   0x11 ResolveHostname → 0x12 ResolveHostnameOK [JSON]
//   0x13 SetHostname     → 0x14 SetHostnameOK     [JSON]
//   0x15 SetVisibility   → 0x16 SetVisibilityOK   [JSON]
//   0x17 Deregister      → 0x18 DeregisterOK      [JSON]
//   0x19 SetTags         → 0x1A SetTagsOK         [JSON]
//   0x1B SetWebhook      → 0x1C SetWebhookOK      [JSON]
//   0x1F Network         → 0x20 NetworkOK         [JSON]
//   0x21 Health          → 0x22 HealthOK          [JSON]
//   0x23 Managed         → 0x24 ManagedOK         [JSON]
//   0x25 RotateKey       → 0x26 RotateKeyOK       [JSON]
//   0x29 Broadcast       → 0x2A BroadcastOK       []

package main

import (
	"encoding/binary"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"os/signal"
	"sync"
	"sync/atomic"
	"syscall"
)

// IPC command codes — must match pkg/driver/ipc.go and pkg/daemon/ipc.go.
const (
	cmdBind              byte = 0x01
	cmdBindOK            byte = 0x02
	cmdDial              byte = 0x03
	cmdDialOK            byte = 0x04
	cmdAccept            byte = 0x05
	cmdSend              byte = 0x06
	cmdRecv              byte = 0x07
	cmdClose             byte = 0x08
	cmdCloseOK           byte = 0x09
	cmdError             byte = 0x0A
	cmdSendTo            byte = 0x0B
	cmdRecvFrom          byte = 0x0C
	cmdInfo              byte = 0x0D
	cmdInfoOK            byte = 0x0E
	cmdHandshake         byte = 0x0F
	cmdHandshakeOK       byte = 0x10
	cmdResolveHostname   byte = 0x11
	cmdResolveHostnameOK byte = 0x12
	cmdSetHostname       byte = 0x13
	cmdSetHostnameOK     byte = 0x14
	cmdSetVisibility     byte = 0x15
	cmdSetVisibilityOK   byte = 0x16
	cmdDeregister        byte = 0x17
	cmdDeregisterOK      byte = 0x18
	cmdSetTags           byte = 0x19
	cmdSetTagsOK         byte = 0x1A
	cmdSetWebhook        byte = 0x1B
	cmdSetWebhookOK      byte = 0x1C
	cmdNetwork           byte = 0x1F
	cmdNetworkOK         byte = 0x20
	cmdHealth            byte = 0x21
	cmdHealthOK          byte = 0x22
	cmdManaged           byte = 0x23
	cmdManagedOK         byte = 0x24
	cmdRotateKey         byte = 0x25
	cmdRotateKeyOK       byte = 0x26
	cmdBroadcast         byte = 0x29
	cmdBroadcastOK       byte = 0x2A
)

// MaxMessageSize matches internal/ipcutil/ipcutil.go.
const maxMessageSize = 1 << 20

// addrSize matches pkg/protocol.AddrSize — 6 bytes on the wire.
const addrSize = 6

// connIDCounter doles out fake stream-conn IDs for cmdDial replies.
var connIDCounter atomic.Uint32

// listenerRegistry tracks bound listener ports across ALL connected
// clients. When client A calls Bind(port=N) and client B later calls
// Dial(addr:N), the mock looks up port N here and pushes cmdAccept on
// client A's socket. This is what exercises PilotListenerAccept end-
// to-end: bind on one handle, dial on a second handle, accept on the
// first.
//
// Concurrency: handleConn goroutines both read and write this map. A
// dial handler holds the listener's write mutex while pushing
// cmdAccept; the listener handler does the same while pushing
// CmdRecv/CmdCloseOK on its own conn — so we never interleave frames
// on a single socket.
var (
	listenerMu       sync.Mutex
	listenerRegistry = map[uint16]*clientConn{}
)

// clientConn wraps a net.Conn with a write mutex so server-pushed
// frames from peer goroutines (a dial handler on a different connection)
// can't interleave with the conn's own reply frames.
type clientConn struct {
	c       net.Conn
	writeMu sync.Mutex
}

func readFrame(r io.Reader) ([]byte, error) {
	var lenBuf [4]byte
	if _, err := io.ReadFull(r, lenBuf[:]); err != nil {
		return nil, err
	}
	n := binary.BigEndian.Uint32(lenBuf[:])
	if n > maxMessageSize {
		return nil, fmt.Errorf("frame too large: %d", n)
	}
	buf := make([]byte, n)
	if _, err := io.ReadFull(r, buf); err != nil {
		return nil, err
	}
	return buf, nil
}

func (cc *clientConn) writeFrame(cmd byte, payload []byte) error {
	frame := make([]byte, 1+len(payload))
	frame[0] = cmd
	copy(frame[1:], payload)

	var lenBuf [4]byte
	binary.BigEndian.PutUint32(lenBuf[:], uint32(len(frame)))

	cc.writeMu.Lock()
	defer cc.writeMu.Unlock()
	if _, err := cc.c.Write(lenBuf[:]); err != nil {
		return err
	}
	_, err := cc.c.Write(frame)
	return err
}

func (cc *clientConn) writeJSON(cmd byte, v interface{}) error {
	data, err := json.Marshal(v)
	if err != nil {
		return err
	}
	return cc.writeFrame(cmd, data)
}

// handleConn services one driver connection.
func handleConn(raw net.Conn) {
	cc := &clientConn{c: raw}
	defer raw.Close()
	log.Printf("mock: client connected from %s", raw.RemoteAddr())

	// Ports this connection has bound. Used to clean up listenerRegistry
	// on disconnect so a stale closed conn never gets pushed to.
	boundPorts := map[uint16]struct{}{}
	defer func() {
		listenerMu.Lock()
		for p := range boundPorts {
			if existing, ok := listenerRegistry[p]; ok && existing == cc {
				delete(listenerRegistry, p)
			}
		}
		listenerMu.Unlock()
	}()

	for {
		msg, err := readFrame(raw)
		if err != nil {
			if err != io.EOF {
				log.Printf("mock: read frame: %v", err)
			}
			return
		}
		if len(msg) < 1 {
			continue
		}
		cmd := msg[0]
		payload := msg[1:]
		log.Printf("mock: recv cmd=0x%02X payload=%dB", cmd, len(payload))

		switch cmd {
		case cmdInfo:
			_ = cc.writeJSON(cmdInfoOK, map[string]interface{}{
				"node_id":  uint32(0x12345678),
				"hostname": "mock-daemon",
				"version":  "mock-0.1.0",
				"peers":    []interface{}{},
			})

		case cmdHealth:
			_ = cc.writeJSON(cmdHealthOK, map[string]interface{}{
				"ok":          true,
				"uptime_secs": 0,
			})

		case cmdBind:
			if len(payload) < 2 {
				sendError(cc, "bind: missing port")
				continue
			}
			port := binary.BigEndian.Uint16(payload[0:2])
			respBody := make([]byte, 2)
			binary.BigEndian.PutUint16(respBody[0:2], port)
			_ = cc.writeFrame(cmdBindOK, respBody)

			// Record this port as bound by this client. Drives the
			// cmdAccept path when a peer dials the port. Last-bind-
			// wins on collisions (harness shouldn't collide).
			listenerMu.Lock()
			listenerRegistry[port] = cc
			listenerMu.Unlock()
			boundPorts[port] = struct{}{}

		case cmdDial:
			// payload: [Addr(6)][port(2)]
			if len(payload) < addrSize+2 {
				sendError(cc, "dial: missing address/port")
				continue
			}
			dstPort := binary.BigEndian.Uint16(payload[addrSize : addrSize+2])
			connID := connIDCounter.Add(1)

			respBody := make([]byte, 4)
			binary.BigEndian.PutUint32(respBody[0:4], connID)
			_ = cc.writeFrame(cmdDialOK, respBody)

			// If anyone has bound dstPort, push a cmdAccept frame on
			// that listener's socket. Format:
			//   [port(2)][connID(4)][remoteAddr(6)][remotePort(2)]
			// remoteAddr/remotePort identify the dialer; for the mock
			// we use a canned synthetic peer address (the listener's
			// driver code only parses the bytes, doesn't validate them
			// against any directory). connID is the SAME one we just
			// handed to the dialer — in a real daemon, the listener's
			// accepted Conn and the dialer's Conn share an end-to-end
			// stream identifier; reusing it here keeps the harness
			// simple. The peer driver code keeps its own handle table
			// keyed by connID so there's no collision in practice.
			listenerMu.Lock()
			listener, ok := listenerRegistry[dstPort]
			listenerMu.Unlock()
			if ok && listener != nil {
				acceptBody := make([]byte, 2+4+addrSize+2)
				binary.BigEndian.PutUint16(acceptBody[0:2], dstPort)
				binary.BigEndian.PutUint32(acceptBody[2:6], connID)
				// Synthetic remote addr 1:0001.AAAA.BBBB
				binary.BigEndian.PutUint16(acceptBody[6:8], 0x0001)
				binary.BigEndian.PutUint32(acceptBody[8:12], 0xAAAABBBB)
				binary.BigEndian.PutUint16(acceptBody[12:14], 0xDEAD)
				_ = listener.writeFrame(cmdAccept, acceptBody)
			}

		case cmdSend:
			// payload: [connID(4)][data...]
			if len(payload) < 4 {
				continue
			}
			connID := binary.BigEndian.Uint32(payload[0:4])
			data := payload[4:]
			// Echo back via server-pushed CmdRecv so PilotConnRead can
			// pull the bytes off the wire.
			recvBody := make([]byte, 4+len(data))
			binary.BigEndian.PutUint32(recvBody[0:4], connID)
			copy(recvBody[4:], data)
			_ = cc.writeFrame(cmdRecv, recvBody)

		case cmdClose:
			// Fire-and-forget; driver does not wait for a reply on Close.
			// Push CmdCloseOK so the driver closes its recvCh and a
			// subsequent Read returns io.EOF.
			if len(payload) >= 4 {
				respBody := make([]byte, 4)
				copy(respBody, payload[0:4])
				_ = cc.writeFrame(cmdCloseOK, respBody)
			}

		case cmdSendTo:
			// Datagram loopback: synthesize a cmdRecvFrom back to the
			// sender so PilotRecvFrom on the same handle observes it.
			//
			// cmdSendTo payload: [dstAddr(6)][dstPort(2)][data...]
			// cmdRecvFrom payload: [srcAddr(6)][srcPort(2)][dstPort(2)][data...]
			//
			// We treat the original destination as the "source" of the
			// reflected datagram (loopback semantics) and assign a
			// canned srcPort of 0xDEAD. The driver doesn't validate
			// addresses against a directory at this layer, so any
			// well-formed 6-byte addr is fine.
			if len(payload) < addrSize+2 {
				continue
			}
			dstAddr := payload[0:addrSize]
			dstPort := binary.BigEndian.Uint16(payload[addrSize : addrSize+2])
			data := payload[addrSize+2:]

			recvBody := make([]byte, addrSize+2+2+len(data))
			copy(recvBody[0:addrSize], dstAddr) // srcAddr := original dst
			binary.BigEndian.PutUint16(recvBody[addrSize:addrSize+2], 0xDEAD)
			binary.BigEndian.PutUint16(recvBody[addrSize+2:addrSize+4], dstPort)
			copy(recvBody[addrSize+4:], data)
			_ = cc.writeFrame(cmdRecvFrom, recvBody)

		case cmdHandshake:
			// payload: [subCmd(1)][rest...]
			if len(payload) < 1 {
				sendError(cc, "handshake: missing sub-cmd")
				continue
			}
			sub := payload[0]
			switch sub {
			case 0x01, 0x02, 0x03, 0x06, 0x07: // send/approve/reject/revoke/wait
				_ = cc.writeJSON(cmdHandshakeOK, map[string]interface{}{
					"ok": true,
				})
			case 0x04: // pending
				_ = cc.writeJSON(cmdHandshakeOK, map[string]interface{}{
					"pending": []interface{}{},
				})
			case 0x05: // trusted
				_ = cc.writeJSON(cmdHandshakeOK, map[string]interface{}{
					"trusted": []interface{}{},
				})
			default:
				sendError(cc, fmt.Sprintf("handshake: unknown sub 0x%02X", sub))
			}

		case cmdResolveHostname:
			_ = cc.writeJSON(cmdResolveHostnameOK, map[string]interface{}{
				"hostname": string(payload),
				"node_id":  uint32(0x0BADF00D),
			})

		case cmdSetHostname:
			_ = cc.writeJSON(cmdSetHostnameOK, map[string]interface{}{
				"hostname": string(payload),
			})

		case cmdSetVisibility:
			public := len(payload) >= 1 && payload[0] == 1
			_ = cc.writeJSON(cmdSetVisibilityOK, map[string]interface{}{
				"public": public,
			})

		case cmdDeregister:
			_ = cc.writeJSON(cmdDeregisterOK, map[string]interface{}{"ok": true})

		case cmdSetTags:
			_ = cc.writeJSON(cmdSetTagsOK, map[string]interface{}{"ok": true})

		case cmdSetWebhook:
			_ = cc.writeJSON(cmdSetWebhookOK, map[string]interface{}{
				"url": string(payload),
			})

		case cmdNetwork:
			// payload: [subCmd(1)][rest...] — all variants reply with
			// a canned OK map.
			_ = cc.writeJSON(cmdNetworkOK, map[string]interface{}{
				"ok":      true,
				"members": []interface{}{},
			})

		case cmdManaged:
			_ = cc.writeJSON(cmdManagedOK, map[string]interface{}{
				"ok":     true,
				"status": "idle",
			})

		case cmdRotateKey:
			_ = cc.writeJSON(cmdRotateKeyOK, map[string]interface{}{
				"rotated": true,
			})

		case cmdBroadcast:
			_ = cc.writeFrame(cmdBroadcastOK, nil)

		default:
			log.Printf("mock: unhandled cmd 0x%02X", cmd)
			sendError(cc, fmt.Sprintf("unknown cmd 0x%02X", cmd))
		}
	}
}

func sendError(cc *clientConn, msg string) {
	// CmdError payload format per pkg/daemon/ipc.go sendError: [code(2)][msg].
	body := make([]byte, 2+len(msg))
	body[0] = 0x00
	body[1] = 0x01
	copy(body[2:], msg)
	_ = cc.writeFrame(cmdError, body)
}

func main() {
	socketPath := flag.String("socket", "", "Unix socket path to listen on (required)")
	flag.Parse()
	if *socketPath == "" {
		fmt.Fprintln(os.Stderr, "usage: mockdaemon -socket /tmp/path.sock")
		os.Exit(2)
	}

	log.SetOutput(os.Stderr)
	log.SetPrefix("[mockdaemon] ")

	// Best-effort unlink of any stale socket.
	_ = os.Remove(*socketPath)

	ln, err := net.Listen("unix", *socketPath)
	if err != nil {
		log.Fatalf("listen %s: %v", *socketPath, err)
	}
	defer ln.Close()
	defer os.Remove(*socketPath)

	log.Printf("listening on %s (pid=%d)", *socketPath, os.Getpid())

	// Wire up clean shutdown on SIGINT/SIGTERM.
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-sigCh
		log.Println("shutting down")
		_ = ln.Close()
		_ = os.Remove(*socketPath)
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
