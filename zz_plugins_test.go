// SPDX-License-Identifier: AGPL-3.0-or-later

package main

// Composition tests for the embedded plugin set. Like zz_internal_test.go
// this file avoids `import "C"`, so it exercises registerEmbeddedPlugins
// directly rather than the cgo entry point that calls it.

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/pilot-protocol/pilotprotocol/pkg/daemon"
	"github.com/pilot-protocol/runtime"
)

// newTestDaemon builds a daemon that is never started — New() only
// allocates in-memory state, so nothing binds a socket or touches the
// network here.
func newTestDaemon(t *testing.T) *daemon.Daemon {
	t.Helper()
	dir := t.TempDir()
	return daemon.New(daemon.Config{
		SocketPath:   filepath.Join(dir, "pilot.sock"),
		IdentityPath: filepath.Join(dir, "identity.json"),
	})
}

func TestRegisterEmbeddedPluginsRegistersWebhook(t *testing.T) {
	d := newTestDaemon(t)
	rt := runtime.New(d.DaemonAPI())

	p, err := registerEmbeddedPlugins(d, rt)
	if err != nil {
		t.Fatalf("registerEmbeddedPlugins: %v", err)
	}
	if p.webhook == nil {
		t.Fatal("webhook service was not constructed")
	}

	want := []string{"trustedagents", "handshake", "policy", "webhook"}
	got := p.names()
	if len(got) != len(want) {
		t.Fatalf("plugin names = %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("plugin names = %v, want %v", got, want)
		}
	}
}

// SetWebhookURL on the daemon only does anything once a WebhookManager is
// registered; unregistered it returns without touching the plugin. The
// plugin persists every URL it is handed, so the persisted file is the
// observable proof that the call reached it.
func TestEmbeddedSetWebhookURLReachesThePlugin(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)

	d := newTestDaemon(t)
	rt := runtime.New(d.DaemonAPI())
	if _, err := registerEmbeddedPlugins(d, rt); err != nil {
		t.Fatalf("registerEmbeddedPlugins: %v", err)
	}

	const url = "https://example.com/pilot-hook"
	d.SetWebhookURL(url)

	data, err := os.ReadFile(filepath.Join(home, ".pilot", "webhook_url"))
	if err != nil {
		t.Fatalf("SetWebhookURL did not reach the webhook plugin: %v", err)
	}
	if got := strings.TrimSpace(string(data)); got != url {
		t.Fatalf("persisted URL = %q, want %q", got, url)
	}

	// Clearing must reach it too.
	d.SetWebhookURL("")
	if _, err := os.Stat(filepath.Join(home, ".pilot", "webhook_url")); !os.IsNotExist(err) {
		t.Fatalf("clearing the webhook left the persisted URL in place (err = %v)", err)
	}
}

// Registering twice on the same runtime must surface the failure rather
// than leaving a half-composed daemon behind.
func TestRegisterEmbeddedPluginsReportsRegistryErrors(t *testing.T) {
	d := newTestDaemon(t)
	rt := runtime.New(d.DaemonAPI())
	if _, err := registerEmbeddedPlugins(d, rt); err != nil {
		t.Fatalf("first registration: %v", err)
	}
	if err := rt.StartPlugins(t.Context()); err != nil {
		t.Fatalf("StartPlugins: %v", err)
	}
	t.Cleanup(func() { _ = rt.StopPlugins(t.Context()) })

	// The registry refuses registration once it has started.
	if _, err := registerEmbeddedPlugins(d, rt); err == nil {
		t.Fatal("expected an error registering into a started runtime")
	}
}

func TestNilEmbeddedPluginsHasNoNames(t *testing.T) {
	var p *embeddedPlugins
	if got := p.names(); got != nil {
		t.Fatalf("names() = %v, want nil", got)
	}
}
