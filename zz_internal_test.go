// SPDX-License-Identifier: AGPL-3.0-or-later

package main

// This file deliberately avoids `import "C"`. Go's test toolchain
// refuses to compile `_test.go` files that pull in cgo (see "use of
// cgo in test … not supported"). Every helper exercised here uses
// pure Go types in its signature, even though the package itself is a
// c-shared cgo build. Anything whose signature carries a C.* type
// (errJSON, okJSON, driverFromHandle, all //export wrappers,
// unmarshalCString) can only be reached from C-side integration
// tests.

import (
	"sync"
	"testing"
)

// ---------------------------------------------------------------------
// Handle table: storeHandle / loadHandle / deleteHandle
//
// These three power every PilotXxx wrapper's argument lookup. They are
// also the only spot where the package holds onto Go heap objects that
// the C side references by uint64 token — getting them wrong leaks
// memory or hands out dangling handles. Worth testing exhaustively.
// ---------------------------------------------------------------------

func TestStoreHandleAssignsUniqueMonotonicIDs(t *testing.T) {
	const N = 50
	ids := make([]uint64, N)
	for i := 0; i < N; i++ {
		ids[i] = storeHandle(i)
	}
	for i := 1; i < N; i++ {
		if ids[i] <= ids[i-1] {
			t.Fatalf("expected monotonic IDs; ids[%d]=%d ids[%d]=%d",
				i-1, ids[i-1], i, ids[i])
		}
	}
	for _, id := range ids {
		deleteHandle(id)
	}
}

func TestStoreHandleNeverReturnsZero(t *testing.T) {
	// init() seeds next=1, so the very first store can never hand
	// out 0. Zero is reserved as "no handle" in the C ABI.
	id := storeHandle("first")
	defer deleteHandle(id)
	if id == 0 {
		t.Fatal("storeHandle returned the reserved zero id")
	}
}

func TestStoreHandlePreservesIdentity(t *testing.T) {
	type marker struct{ name string }
	want := &marker{name: "round-trip"}
	id := storeHandle(want)
	defer deleteHandle(id)

	got, ok := loadHandle(id)
	if !ok {
		t.Fatalf("loadHandle returned !ok for id %d", id)
	}
	gotMarker, ok := got.(*marker)
	if !ok {
		t.Fatalf("loaded value is %T, want *marker", got)
	}
	if gotMarker != want {
		t.Fatalf("identity mismatch: got %p want %p", gotMarker, want)
	}
	if gotMarker.name != "round-trip" {
		t.Fatalf("value mutated through table; got %q", gotMarker.name)
	}
}

func TestStoreHandleAllowsNil(t *testing.T) {
	// Storing nil is legal — the table maps to interface{} and a
	// nil entry must still be retrievable (the C side may store
	// "no driver yet" sentinels).
	id := storeHandle(nil)
	defer deleteHandle(id)
	v, ok := loadHandle(id)
	if !ok {
		t.Fatal("loadHandle returned !ok for a nil-valued handle")
	}
	if v != nil {
		t.Fatalf("expected nil value; got %v", v)
	}
}

func TestLoadHandleMissingReturnsFalse(t *testing.T) {
	if _, ok := loadHandle(0); ok {
		t.Fatal("loadHandle(0) reported ok; zero is never assigned")
	}
	if _, ok := loadHandle(1 << 60); ok {
		t.Fatal("loadHandle of a never-stored id should return false")
	}
}

func TestDeleteHandleRemovesEntry(t *testing.T) {
	id := storeHandle("sentinel")
	if _, ok := loadHandle(id); !ok {
		t.Fatalf("precondition: handle %d should exist", id)
	}
	deleteHandle(id)
	if _, ok := loadHandle(id); ok {
		t.Fatalf("loadHandle(%d) still ok after deleteHandle", id)
	}
}

func TestDeleteHandleIsIdempotent(t *testing.T) {
	// The C side is allowed to drop a handle twice (e.g. PilotClose
	// followed by an explicit FreeString during shutdown). The
	// second delete must be a silent no-op, not a panic.
	id := storeHandle("twice")
	deleteHandle(id)
	deleteHandle(id) // would panic if delete weren't idempotent
}

func TestDeleteHandleUnknownIDDoesNotPanic(t *testing.T) {
	deleteHandle(1 << 62) // never assigned; must be a no-op
}

func TestHandleTableMultipleConcurrentStores(t *testing.T) {
	// Race-detector hook: hammer the table from many goroutines and
	// confirm the sync.RWMutex pairing in store/load/delete holds up.
	// Run with `go test -race` to make this meaningful.
	const writers = 8
	const perWriter = 200
	var wg sync.WaitGroup
	wg.Add(writers)

	collected := make(chan uint64, writers*perWriter)
	for w := 0; w < writers; w++ {
		go func(seed int) {
			defer wg.Done()
			for i := 0; i < perWriter; i++ {
				id := storeHandle(seed*1000 + i)
				v, ok := loadHandle(id)
				if !ok || v.(int) != seed*1000+i {
					t.Errorf("round-trip mismatch for id %d", id)
					return
				}
				collected <- id
			}
		}(w)
	}
	wg.Wait()
	close(collected)

	seen := make(map[uint64]struct{}, writers*perWriter)
	for id := range collected {
		if _, dup := seen[id]; dup {
			t.Fatalf("duplicate id %d returned from storeHandle", id)
		}
		seen[id] = struct{}{}
		deleteHandle(id)
	}
}

func TestHandleTableMixedStoreLoadDelete(t *testing.T) {
	// Interleave stores, loads, and deletes from independent
	// goroutines so the race detector can hunt for missing locks
	// on any of the three operations.
	const goroutines = 16
	const ops = 100

	var wg sync.WaitGroup
	wg.Add(goroutines)
	for g := 0; g < goroutines; g++ {
		go func(seed int) {
			defer wg.Done()
			local := make([]uint64, 0, ops)
			for i := 0; i < ops; i++ {
				id := storeHandle(seed*ops + i)
				local = append(local, id)
				if v, ok := loadHandle(id); !ok || v.(int) != seed*ops+i {
					t.Errorf("g=%d round-trip mismatch id=%d", seed, id)
					return
				}
			}
			// Delete every handle we own; concurrent goroutines
			// must not see any cross-contamination.
			for _, id := range local {
				deleteHandle(id)
			}
		}(g)
	}
	wg.Wait()
}

func TestHandleTableDistinctIDsForIdenticalValues(t *testing.T) {
	// Two stores of the same Go value must produce distinct
	// handles — the C side relies on token identity, not value
	// equality, to distinguish driver instances from each other.
	a := storeHandle("same-value")
	b := storeHandle("same-value")
	defer deleteHandle(a)
	defer deleteHandle(b)
	if a == b {
		t.Fatalf("storeHandle returned the same id %d for two stores", a)
	}
}

// ---------------------------------------------------------------------
// embeddedConfig.defaults()
//
// PilotEmbeddedStart relies on this to fill blanks before booting the
// daemon; getting it wrong silently wires a different rendezvous than
// the caller expects.
// ---------------------------------------------------------------------

func TestEmbeddedConfigDefaultsFillsBlanks(t *testing.T) {
	var c embeddedConfig
	c.defaults()
	if c.RegistryAddr == "" {
		t.Fatal("RegistryAddr default not applied")
	}
	if c.BeaconAddr == "" {
		t.Fatal("BeaconAddr default not applied")
	}
	if c.KeepaliveSec <= 0 {
		t.Fatalf("KeepaliveSec default not applied: %d", c.KeepaliveSec)
	}
	if c.Version == "" {
		t.Fatal("Version default not applied")
	}
}

func TestEmbeddedConfigDefaultsKeepsExplicitValues(t *testing.T) {
	c := embeddedConfig{
		RegistryAddr: "registry.example:9999",
		BeaconAddr:   "beacon.example:8888",
		KeepaliveSec: 17,
		Version:      "custom",
	}
	c.defaults()
	if c.RegistryAddr != "registry.example:9999" {
		t.Fatalf("registry addr overwritten: %s", c.RegistryAddr)
	}
	if c.BeaconAddr != "beacon.example:8888" {
		t.Fatalf("beacon addr overwritten: %s", c.BeaconAddr)
	}
	if c.KeepaliveSec != 17 {
		t.Fatalf("keepalive overwritten: %d", c.KeepaliveSec)
	}
	if c.Version != "custom" {
		t.Fatalf("version overwritten: %s", c.Version)
	}
}

func TestEmbeddedConfigDefaultsNegativeKeepaliveClamped(t *testing.T) {
	// The guard is `<= 0`, so negatives must be clamped to the
	// default — otherwise daemon.Config would receive a negative
	// duration and silently skew keepalive timers.
	c := embeddedConfig{KeepaliveSec: -5}
	c.defaults()
	if c.KeepaliveSec <= 0 {
		t.Fatalf("expected positive keepalive after defaults; got %d", c.KeepaliveSec)
	}
}

func TestEmbeddedConfigDefaultsZeroKeepaliveReplaced(t *testing.T) {
	c := embeddedConfig{KeepaliveSec: 0}
	c.defaults()
	if c.KeepaliveSec == 0 {
		t.Fatal("zero KeepaliveSec was not replaced with default")
	}
}

func TestEmbeddedConfigDefaultsPreservesDataAndSocketPaths(t *testing.T) {
	// defaults() must never touch fields the caller cares about
	// for filesystem location.
	c := embeddedConfig{
		DataDir:    "/var/lib/pilot",
		SocketPath: "/tmp/pilot.sock",
	}
	c.defaults()
	if c.DataDir != "/var/lib/pilot" {
		t.Fatalf("DataDir overwritten: %s", c.DataDir)
	}
	if c.SocketPath != "/tmp/pilot.sock" {
		t.Fatalf("SocketPath overwritten: %s", c.SocketPath)
	}
}

func TestEmbeddedConfigDefaultsRegistryAddrFormat(t *testing.T) {
	// Sanity check: the default registry/beacon must be a
	// host:port-shaped string so daemon.New won't reject them. We
	// don't validate the host is reachable.
	var c embeddedConfig
	c.defaults()
	if !containsColon(c.RegistryAddr) {
		t.Fatalf("default RegistryAddr %q missing :port", c.RegistryAddr)
	}
	if !containsColon(c.BeaconAddr) {
		t.Fatalf("default BeaconAddr %q missing :port", c.BeaconAddr)
	}
}

func containsColon(s string) bool {
	for i := 0; i < len(s); i++ {
		if s[i] == ':' {
			return true
		}
	}
	return false
}
