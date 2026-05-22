// SPDX-License-Identifier: AGPL-3.0-or-later

package main

import "testing"

// TestMainPackageCompiles is a minimal smoke test for the libpilot
// CGo `c-shared` package. Every exported symbol in this package uses
// C types (*C.char, C.uint64_t, unsafe.Pointer) in its signature, so
// the //export functions cannot be invoked from a Go test. The most
// useful thing a Go test can assert here is that the package builds
// and links — which `go test` already does to produce the test binary.
// If main.go fails to compile, this test fails to build.
func TestMainPackageCompiles(t *testing.T) {
	t.Log("libpilot main package compiles and links")
}
