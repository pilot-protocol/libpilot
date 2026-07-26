// SPDX-License-Identifier: AGPL-3.0-or-later

// Plugin composition for the embedded daemon.
//
// This file deliberately avoids `import "C"` so the composition can be
// exercised from a Go test; embedded.go (which is cgo) calls into it.

package main

import (
	"fmt"

	"github.com/pilot-protocol/handshake"
	"github.com/pilot-protocol/pilotprotocol/pkg/daemon"
	"github.com/pilot-protocol/policy"
	"github.com/pilot-protocol/runtime"
	"github.com/pilot-protocol/trustedagents"
	"github.com/pilot-protocol/webhook"
)

// embeddedPlugins holds the in-process plugin set, so callers keep a
// handle on each service after registration.
type embeddedPlugins struct {
	trust     *trustedagents.Service
	handshake *handshake.Service
	policy    *policy.Service
	webhook   *webhook.Service
}

// registerEmbeddedPlugins constructs the plugin set, registers each
// service with rt, and installs the daemon-side adapters that the IPC
// handlers route through.
//
// This is a subset of what cmd/daemon composes, chosen for a daemon
// living inside a host application process:
//
//	trustedagents  trust decisions for incoming connections
//	handshake      manual trust handshake on port 444
//	policy         per-peer policy evaluation
//	webhook        forwards bus events to a caller-set URL
//
// The plugins cmd/daemon additionally runs are host-level concerns that
// do not apply here: skillinject writes into agent tool directories on
// the machine, and the app-store supervisor spawns and supervises child
// binaries. Neither fits a single sandboxed app process.
//
// Registration order matches cmd/daemon; actual start order is decided by
// each service's Order().
func registerEmbeddedPlugins(d *daemon.Daemon, rt *runtime.Runtime) (*embeddedPlugins, error) {
	dapi := rt.Daemon()
	p := &embeddedPlugins{}

	p.trust = trustedagents.NewService()
	if err := rt.Register(p.trust); err != nil {
		return nil, fmt.Errorf("register trustedagents: %w", err)
	}
	d.RegisterTrustChecker(p.trust)

	p.handshake = handshake.NewService(runtime.NewHandshakeRuntime(dapi))
	if err := rt.Register(p.handshake); err != nil {
		return nil, fmt.Errorf("register handshake: %w", err)
	}
	d.RegisterHandshakeService(runtime.NewHandshakeServiceAdapter(p.handshake))

	p.policy = policy.NewService(runtime.NewPolicyRuntime(dapi))
	if err := rt.Register(p.policy); err != nil {
		return nil, fmt.Errorf("register policy: %w", err)
	}
	d.RegisterPolicyManager(runtime.AsDaemonPolicyManager(p.policy.Manager()))

	// The daemon publishes lifecycle events onto its in-process bus and
	// this plugin forwards them to a URL. Registering the manager is what
	// gives Daemon.SetWebhookURL — the target of the set-webhook IPC
	// command behind PilotSetWebhook — something to route to; unregistered,
	// that call is acknowledged and then discarded. Constructed with no
	// URL: the plugin reads any persisted one on start, and callers set it
	// at runtime.
	p.webhook = webhook.NewService("")
	if err := rt.Register(p.webhook); err != nil {
		return nil, fmt.Errorf("register webhook: %w", err)
	}
	d.RegisterWebhookManager(webhookManagerAdapter{svc: p.webhook})

	return p, nil
}

// names lists the registered plugins in registration order.
func (p *embeddedPlugins) names() []string {
	if p == nil {
		return nil
	}
	return []string{
		p.trust.Name(),
		p.handshake.Name(),
		p.policy.Name(),
		p.webhook.Name(),
	}
}

// webhookManagerAdapter bridges *webhook.Service to the daemon's
// WebhookManager interface. Defined here rather than in the plugin so the
// plugin stays free of pkg/daemon imports — same split cmd/daemon uses.
type webhookManagerAdapter struct{ svc *webhook.Service }

func (a webhookManagerAdapter) SetURL(url string) { a.svc.SetURL(url) }

func (a webhookManagerAdapter) Stats() daemon.WebhookStats {
	s := a.svc.Stats()
	return daemon.WebhookStats{Dropped: s.Dropped, CircuitSkips: s.CircuitSkips}
}
