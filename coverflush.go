// SPDX-License-Identifier: AGPL-3.0-or-later
//
//go:build coverflush

// coverflush adds a single //export, PilotCoverFlush, that the
// cintegration/ C harness calls just before exit. Without it, Go's
// coverage counters never make it to disk: when a c-shared library is
// loaded by a C program, the C `exit()` path skips Go's runtime
// at-exit handlers (which is what normally flushes covcounters.*).
//
// This file is build-tagged `coverflush` so the helper does NOT ship
// in production builds — it's purely a coverage-tooling escape hatch.
// The Makefile in cintegration/ adds `-tags coverflush` when building
// the instrumented dylib.

package main

/*
#include <stdlib.h>
*/
import "C"

import (
	"os"
	"runtime/coverage"
)

//export PilotCoverFlush
func PilotCoverFlush() *C.char {
	dir := os.Getenv("GOCOVERDIR")
	if dir == "" {
		return C.CString(`{"error":"GOCOVERDIR not set"}`)
	}
	if err := coverage.WriteCountersDir(dir); err != nil {
		return C.CString(`{"error":"` + err.Error() + `"}`)
	}
	return nil
}
