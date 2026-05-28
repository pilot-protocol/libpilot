// SPDX-License-Identifier: AGPL-3.0-or-later
//
// C-side integration harness for libpilot.
//
// Why this exists: Go's test toolchain refuses `import "C"` in _test.go
// files of packages that have //export directives. That puts roughly
// 250 of libpilot's 290 statements out of reach of `go test -cover`.
// This harness fills the gap by driving the compiled c-shared library
// directly from C, exercising every //export entry point with both
// valid and invalid inputs.
//
// Run via the cintegration/Makefile, which builds libpilot.dylib with
// `-cover -coverpkg=.` and exports GOCOVERDIR before invoking the
// harness — the coverage counters land in a directory the Makefile
// then converts to a textfmt coverage profile.
//
// Tests are grouped by export. Each test prints PASS / FAIL with a
// short reason and increments a counter. Process exits non-zero on any
// FAIL. Aborts on any Go panic that escapes the FFI boundary (which is
// itself a finding — see iter-3 audit item HIGH-5 "all 46 //export
// functions lack defer recover()").

#include "../libpilot.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int pass_count = 0;
static int fail_count = 0;

#define PASS(name)                                                             \
  do {                                                                         \
    pass_count++;                                                              \
    printf("  PASS %s\n", name);                                               \
  } while (0)

#define FAIL(name, reason)                                                     \
  do {                                                                         \
    fail_count++;                                                              \
    printf("  FAIL %s: %s\n", name, reason);                                   \
  } while (0)

// Returned C strings are owned by Go; free via FreeString.
static void free_c_string(char *s) {
  if (s != NULL) {
    FreeString(s);
  }
}

// expect_error returns 1 if the returned JSON looks like an error
// envelope, 0 otherwise. libpilot returns either an error JSON
// {"error":"..."} or a success envelope.
static int has_error(const char *json) {
  if (json == NULL) return 0;
  return strstr(json, "\"error\"") != NULL;
}

// Forward declaration so embedded-daemon tests defined above the mock-
// daemon helpers can call the predicate. Definition is co-located with
// the mock-daemon helpers further down.
static int has_no_error(const char *json);

// ---------------------------------------------------------------------------
// Handle-table edge cases
// ---------------------------------------------------------------------------

static void test_close_zero_handle(void) {
  char *err = PilotClose(0);
  if (err == NULL) {
    FAIL("close_zero_handle", "expected error, got NULL");
    return;
  }
  if (!has_error(err)) {
    FAIL("close_zero_handle", "expected error JSON");
    free_c_string(err);
    return;
  }
  PASS("close_zero_handle");
  free_c_string(err);
}

static void test_close_unknown_handle(void) {
  // Pick a handle that storeHandle never returned.
  char *err = PilotClose((uint64_t)0xDEADBEEFCAFEBABEULL);
  if (err == NULL || !has_error(err)) {
    FAIL("close_unknown_handle", "expected error, got success");
    if (err) free_c_string(err);
    return;
  }
  PASS("close_unknown_handle");
  free_c_string(err);
}

static void test_listener_close_bad_handle(void) {
  char *err = PilotListenerClose(0);
  if (err == NULL || !has_error(err)) {
    FAIL("listener_close_bad_handle", "expected error");
    if (err) free_c_string(err);
    return;
  }
  PASS("listener_close_bad_handle");
  free_c_string(err);
}

static void test_conn_close_bad_handle(void) {
  char *err = PilotConnClose(0);
  if (err == NULL || !has_error(err)) {
    FAIL("conn_close_bad_handle", "expected error");
    if (err) free_c_string(err);
    return;
  }
  PASS("conn_close_bad_handle");
  free_c_string(err);
}

// Bad-handle smoke tests for the //export functions whose only
// uncovered branch in the baseline was the `driverFromHandle(h)`
// failure path. Each one wraps one statement-of-coverage worth of
// uncovered code per binding. Keeps total coverage just over the
// 80% threshold without spinning up the mock daemon for every one.
static void test_disconnect_bad_handle(void) {
  char *err = PilotDisconnect(0, 1);
  if (err == NULL || !has_error(err)) {
    FAIL("disconnect_bad_handle", "expected error");
    if (err) free_c_string(err);
    return;
  }
  PASS("disconnect_bad_handle");
  free_c_string(err);
}

static void test_dial_timeout_bad_handle(void) {
  char addr[] = "1:0001.0001.0001:80";
  struct PilotDialTimeout_return r = PilotDialTimeout(0, addr, 100);
  if (r.r1 == NULL || !has_error(r.r1)) {
    FAIL("dial_timeout_bad_handle", "expected error");
    if (r.r1) free_c_string(r.r1);
    return;
  }
  PASS("dial_timeout_bad_handle");
  free_c_string(r.r1);
}

// Stub — actual coverage for the ParseSocketAddr error branch in
// PilotDialTimeout requires a valid handle, so the real test lives
// alongside the mock-daemon helpers further down
// (test_mock_dial_timeout_malformed_addr).
static void test_dial_timeout_malformed_addr(void) {
  // Bad handle path — same code line as test_dial_timeout_bad_handle.
  // Kept as a sentinel so future refactors don't drop the test slot.
  char addr[] = "this-is-not-a-pilot-addr";
  struct PilotDialTimeout_return r = PilotDialTimeout(0, addr, 100);
  if (r.r1 == NULL || !has_error(r.r1)) {
    FAIL("dial_timeout_malformed_addr", "expected error");
    if (r.r1) free_c_string(r.r1);
    return;
  }
  PASS("dial_timeout_malformed_addr");
  free_c_string(r.r1);
}

static void test_member_tags_set_bad_handle(void) {
  char tags[] = "[\"a\"]";
  char *err = PilotMemberTagsSet(0, 1, 1, tags);
  if (err == NULL || !has_error(err)) {
    FAIL("member_tags_set_bad_handle", "expected error");
    if (err) free_c_string(err);
    return;
  }
  PASS("member_tags_set_bad_handle");
  free_c_string(err);
}

static void test_member_tags_set_invalid_json(void) {
  // Bad handle short-circuits before the json.Unmarshal call, so use a
  // direct mock-daemon-backed test for the invalid-JSON branch (added
  // separately below). This one just exercises the bad-handle path
  // with a known-good JSON body, doubling as a regression for handle
  // validation ordering.
  char tags[] = "[\"x\"]";
  char *err = PilotMemberTagsSet(0xFEEDC0DEFACE, 1, 1, tags);
  if (err == NULL || !has_error(err)) {
    FAIL("member_tags_set_invalid_json", "expected error");
    if (err) free_c_string(err);
    return;
  }
  PASS("member_tags_set_invalid_json");
  free_c_string(err);
}

static void test_conn_set_read_deadline_bad_handle(void) {
  char *err = PilotConnSetReadDeadline(0, 0);
  if (err == NULL || !has_error(err)) {
    FAIL("conn_set_read_deadline_bad_handle", "expected error");
    if (err) free_c_string(err);
    return;
  }
  PASS("conn_set_read_deadline_bad_handle");
  free_c_string(err);
}

// ---------------------------------------------------------------------------
// Info / Health / TrustedPeers / Pending on invalid handle — every
// handle-checking function should return an error envelope without
// panicking.
// ---------------------------------------------------------------------------

static void test_info_bad_handle(void) {
  char *res = PilotInfo(0);
  if (res == NULL || !has_error(res)) {
    FAIL("info_bad_handle", "expected error");
    if (res) free_c_string(res);
    return;
  }
  PASS("info_bad_handle");
  free_c_string(res);
}

static void test_health_bad_handle(void) {
  char *res = PilotHealth(0);
  if (res == NULL || !has_error(res)) {
    FAIL("health_bad_handle", "expected error");
    if (res) free_c_string(res);
    return;
  }
  PASS("health_bad_handle");
  free_c_string(res);
}

static void test_trusted_peers_bad_handle(void) {
  char *res = PilotTrustedPeers(0);
  if (res == NULL || !has_error(res)) {
    FAIL("trusted_peers_bad_handle", "expected error");
    if (res) free_c_string(res);
    return;
  }
  PASS("trusted_peers_bad_handle");
  free_c_string(res);
}

static void test_pending_handshakes_bad_handle(void) {
  char *res = PilotPendingHandshakes(0);
  if (res == NULL || !has_error(res)) {
    FAIL("pending_handshakes_bad_handle", "expected error");
    if (res) free_c_string(res);
    return;
  }
  PASS("pending_handshakes_bad_handle");
  free_c_string(res);
}

// ---------------------------------------------------------------------------
// Param-validation paths that iter-3 audit flagged
// ---------------------------------------------------------------------------

// PilotConnRead bufSize bounds — bufSize=0 must not panic.
static void test_conn_read_zero_bufsize(void) {
  struct PilotConnRead_return r = PilotConnRead(0, 0);
  // We expect an error (bad handle, bad size, or both) — what we MUST
  // NOT see is a panic that aborts the harness.
  if (r.r2 == NULL || !has_error(r.r2)) {
    FAIL("conn_read_zero_bufsize", "expected error");
    if (r.r2) free_c_string(r.r2);
    return;
  }
  PASS("conn_read_zero_bufsize");
  free_c_string(r.r2);
}

// PilotConnRead with a clearly nonsensical negative bufSize.
static void test_conn_read_negative_bufsize(void) {
  struct PilotConnRead_return r = PilotConnRead(0, -1);
  if (r.r2 == NULL || !has_error(r.r2)) {
    FAIL("conn_read_negative_bufsize", "expected error");
    if (r.r2) free_c_string(r.r2);
    return;
  }
  PASS("conn_read_negative_bufsize");
  free_c_string(r.r2);
}

// PilotConnWrite with NULL data + dataLen=0 — should error cleanly,
// not crash.
static void test_conn_write_null_data(void) {
  struct PilotConnWrite_return r = PilotConnWrite(0, NULL, 0);
  if (r.r1 == NULL || !has_error(r.r1)) {
    FAIL("conn_write_null_data", "expected error");
    if (r.r1) free_c_string(r.r1);
    return;
  }
  PASS("conn_write_null_data");
  free_c_string(r.r1);
}

// PilotDial with NULL addr string — must not deref.
static void test_dial_null_addr(void) {
  struct PilotDial_return r = PilotDial(0, NULL);
  if (r.r1 == NULL || !has_error(r.r1)) {
    FAIL("dial_null_addr", "expected error");
    if (r.r1) free_c_string(r.r1);
    return;
  }
  PASS("dial_null_addr");
  free_c_string(r.r1);
}

// PilotDial with malformed addr — should reject without panic.
static void test_dial_malformed_addr(void) {
  char addr[] = "not-a-pilot-address";
  struct PilotDial_return r = PilotDial(0, addr);
  if (r.r1 == NULL || !has_error(r.r1)) {
    FAIL("dial_malformed_addr", "expected error");
    if (r.r1) free_c_string(r.r1);
    return;
  }
  PASS("dial_malformed_addr");
  free_c_string(r.r1);
}

// PilotDialTimeout — exercises the timeoutMs overflow path that iter-3
// flagged as HIGH (uint64 → time.Duration overflow). We can't observe
// the overflow directly without a live daemon, but we can call with a
// huge value and confirm it returns rather than hanging.
static void test_dial_timeout_huge(void) {
  char addr[] = "1:0001.0001.0001";
  struct PilotDialTimeout_return r = PilotDialTimeout(0, addr, (uint64_t)1ULL << 50);
  if (r.r1 == NULL || !has_error(r.r1)) {
    FAIL("dial_timeout_huge", "expected error");
    if (r.r1) free_c_string(r.r1);
    return;
  }
  PASS("dial_timeout_huge");
  free_c_string(r.r1);
}

// ---------------------------------------------------------------------------
// Connect path — refuses to dial a daemon that isn't there.
// ---------------------------------------------------------------------------

static void test_connect_missing_socket(void) {
  char path[] = "/tmp/this-socket-does-not-exist-libpilot-harness.sock";
  struct PilotConnect_return r = PilotConnect(path);
  if (r.r0 != 0) {
    FAIL("connect_missing_socket", "expected handle=0 on failure");
    if (r.r1) free_c_string(r.r1);
    return;
  }
  if (r.r1 == NULL || !has_error(r.r1)) {
    FAIL("connect_missing_socket", "expected error JSON");
    if (r.r1) free_c_string(r.r1);
    return;
  }
  PASS("connect_missing_socket");
  free_c_string(r.r1);
}

static void test_connect_null_path(void) {
  struct PilotConnect_return r = PilotConnect(NULL);
  if (r.r0 != 0 || r.r1 == NULL || !has_error(r.r1)) {
    FAIL("connect_null_path", "expected handle=0 + error");
    if (r.r1) free_c_string(r.r1);
    return;
  }
  PASS("connect_null_path");
  free_c_string(r.r1);
}

// ---------------------------------------------------------------------------
// Listen / Accept on bad handle
// ---------------------------------------------------------------------------

static void test_listen_bad_handle(void) {
  struct PilotListen_return r = PilotListen(0, 12345);
  if (r.r0 != 0 || r.r1 == NULL || !has_error(r.r1)) {
    FAIL("listen_bad_handle", "expected handle=0 + error");
    if (r.r1) free_c_string(r.r1);
    return;
  }
  PASS("listen_bad_handle");
  free_c_string(r.r1);
}

static void test_accept_bad_handle(void) {
  struct PilotListenerAccept_return r = PilotListenerAccept(0);
  if (r.r0 != 0 || r.r1 == NULL || !has_error(r.r1)) {
    FAIL("accept_bad_handle", "expected handle=0 + error");
    if (r.r1) free_c_string(r.r1);
    return;
  }
  PASS("accept_bad_handle");
  free_c_string(r.r1);
}

// ---------------------------------------------------------------------------
// String-arg endpoints with NULL — every one of these should error
// rather than segfault.
// ---------------------------------------------------------------------------

static void test_handshake_null_justification(void) {
  char *res = PilotHandshake(0, 42, NULL);
  if (res == NULL || !has_error(res)) {
    FAIL("handshake_null_justification", "expected error");
    if (res) free_c_string(res);
    return;
  }
  PASS("handshake_null_justification");
  free_c_string(res);
}

static void test_set_hostname_null(void) {
  char *res = PilotSetHostname(0, NULL);
  if (res == NULL || !has_error(res)) {
    FAIL("set_hostname_null", "expected error");
    if (res) free_c_string(res);
    return;
  }
  PASS("set_hostname_null");
  free_c_string(res);
}

static void test_resolve_hostname_null(void) {
  char *res = PilotResolveHostname(0, NULL);
  if (res == NULL || !has_error(res)) {
    FAIL("resolve_hostname_null", "expected error");
    if (res) free_c_string(res);
    return;
  }
  PASS("resolve_hostname_null");
  free_c_string(res);
}

static void test_set_webhook_null(void) {
  char *res = PilotSetWebhook(0, NULL);
  if (res == NULL || !has_error(res)) {
    FAIL("set_webhook_null", "expected error");
    if (res) free_c_string(res);
    return;
  }
  PASS("set_webhook_null");
  free_c_string(res);
}

static void test_set_tags_invalid_json(void) {
  char invalid[] = "{not valid json";
  char *res = PilotSetTags(0, invalid);
  if (res == NULL || !has_error(res)) {
    FAIL("set_tags_invalid_json", "expected error");
    if (res) free_c_string(res);
    return;
  }
  PASS("set_tags_invalid_json");
  free_c_string(res);
}

static void test_broadcast_negative_datalen(void) {
  char tok[] = "no-token";
  char *err = PilotBroadcast(0, 1, 1, NULL, -1, tok);
  if (err == NULL || !has_error(err)) {
    FAIL("broadcast_negative_datalen",
         "MUST reject negative datalen (iter-3 CRITICAL-2)");
    if (err) free_c_string(err);
    return;
  }
  PASS("broadcast_negative_datalen");
  free_c_string(err);
}

static void test_send_to_negative_datalen(void) {
  char addr[] = "1:0001.0001.0001:8";
  char *err = PilotSendTo(0, addr, NULL, -1);
  if (err == NULL || !has_error(err)) {
    FAIL("send_to_negative_datalen",
         "MUST reject negative datalen (iter-3 CRITICAL-2)");
    if (err) free_c_string(err);
    return;
  }
  PASS("send_to_negative_datalen");
  free_c_string(err);
}

static void test_conn_write_negative_datalen(void) {
  struct PilotConnWrite_return r = PilotConnWrite(0, NULL, -1);
  if (r.r1 == NULL || !has_error(r.r1)) {
    FAIL("conn_write_negative_datalen",
         "MUST reject negative datalen (iter-3 CRITICAL-2)");
    if (r.r1) free_c_string(r.r1);
    return;
  }
  PASS("conn_write_negative_datalen");
  free_c_string(r.r1);
}

// ---------------------------------------------------------------------------
// Embedded daemon — start with invalid config, verify error path.
// ---------------------------------------------------------------------------

static void test_embedded_start_invalid_json(void) {
  char bad_cfg[] = "{this is not valid json";
  char *res = PilotEmbeddedStart(bad_cfg);
  if (res == NULL || !has_error(res)) {
    FAIL("embedded_start_invalid_json", "expected error JSON");
    if (res) free_c_string(res);
    return;
  }
  PASS("embedded_start_invalid_json");
  free_c_string(res);
}

static void test_embedded_stop_when_not_running(void) {
  // PilotEmbeddedStop takes no args — it's a singleton. Calling it
  // before any start should error rather than panic.
  char *err = PilotEmbeddedStop();
  if (err != NULL) free_c_string(err);
  // Either result is acceptable; what matters is that we returned.
  PASS("embedded_stop_when_not_running");
}

// ---------------------------------------------------------------------------
// Embedded daemon with fake registry — full PilotEmbeddedStart success
// path. The fake registry (./mockregistry-bin) is a TCP binary that
// speaks length-prefixed JSON to satisfy the pkg/daemon Start() flow:
// it answers `register` with a canned node_id + address, and `lookup`
// with an empty networks list. STUN against -beacon-addr fails (no UDP
// listener) but pkg/daemon's discoverWithTempSocket logs that as a
// warning and falls back to the local listen address, so Start
// succeeds end-to-end.
//
// This is the only path that exercises PilotEmbeddedStart past its
// JSON-parse guard. Without a real registry the //export function
// short-circuits at the daemon.Start() error return — visible in the
// 13.6% baseline coverage of that function.
// ---------------------------------------------------------------------------

static pid_t mockreg_pid = 0;
static char mockreg_addr[64] = {0};
static char mockreg_addr_file[256] = {0};

// wait_for_file polls up to ~3 seconds for `path` to exist and be
// non-empty. Used to learn the OS-assigned port from the mock registry
// without parsing its stdout (the child renames .tmp → path so the read
// is never torn).
static int wait_for_file(const char *path, char *out, size_t outsz) {
  for (int i = 0; i < 300; i++) {
    FILE *f = fopen(path, "r");
    if (f != NULL) {
      size_t n = fread(out, 1, outsz - 1, f);
      fclose(f);
      if (n > 0) {
        out[n] = '\0';
        // Strip trailing newline if any.
        while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) {
          out[--n] = '\0';
        }
        if (n > 0) return 1;
      }
    }
    struct timespec ts = {0, 10 * 1000 * 1000}; // 10 ms
    nanosleep(&ts, NULL);
  }
  return 0;
}

static int start_mock_registry(void) {
  snprintf(mockreg_addr_file, sizeof(mockreg_addr_file),
           "/tmp/libpilot-mockreg-%d.addr", (int)getpid());
  unlink(mockreg_addr_file);

  pid_t pid = fork();
  if (pid < 0) {
    fprintf(stderr, "fork mockreg: %s\n", strerror(errno));
    return 0;
  }
  if (pid == 0) {
    execl("./mockregistry-bin", "mockregistry-bin",
          "-addr-file", mockreg_addr_file, (char *)NULL);
    fprintf(stderr, "execl mockregistry-bin: %s\n", strerror(errno));
    _exit(127);
  }
  mockreg_pid = pid;

  if (!wait_for_file(mockreg_addr_file, mockreg_addr,
                     sizeof(mockreg_addr))) {
    fprintf(stderr,
            "mockregistry-bin did not write addr file %s within 3s\n",
            mockreg_addr_file);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    mockreg_pid = 0;
    return 0;
  }
  return 1;
}

static void stop_mock_registry(void) {
  if (mockreg_pid > 0) {
    kill(mockreg_pid, SIGTERM);
    waitpid(mockreg_pid, NULL, 0);
    mockreg_pid = 0;
  }
  if (mockreg_addr_file[0] != '\0') {
    unlink(mockreg_addr_file);
  }
}

// Drives the full PilotEmbeddedStart success path against a fake
// registry. Builds a per-test data dir + IPC socket, spins up the
// in-process daemon, probes it via PilotConnect+PilotInfo, then tears
// it down with PilotEmbeddedStop.
//
// The test is conservative — any path that returns a {"error":...}
// envelope (registry dial, daemon Start, IPC socket creation) is
// treated as FAIL with the error text logged, so a regression in any
// of the upstream stages surfaces clearly instead of silently passing.
static void test_embedded_start_with_mock_registry(void) {
  if (!start_mock_registry()) {
    fail_count++;
    printf("  FAIL embedded_start_with_mock_registry: "
           "could not spawn mockregistry-bin\n");
    return;
  }
  printf("\n[mock registry] pid=%d addr=%s\n",
         (int)mockreg_pid, mockreg_addr);

  // Per-test data dir + IPC socket. /tmp avoids the macOS sun_path
  // 104-byte ceiling that TMPDIR (/var/folders/...) can blow past.
  char data_dir[256];
  char socket_path[256];
  snprintf(data_dir, sizeof(data_dir),
           "/tmp/libpilot-emb-%d", (int)getpid());
  snprintf(socket_path, sizeof(socket_path),
           "/tmp/libpilot-emb-%d.sock", (int)getpid());

  // Best-effort clean of any stale artefacts from a prior run.
  unlink(socket_path);
  mkdir(data_dir, 0700);

  // Build the config JSON. registry_addr points at the fake registry
  // we just spawned. beacon_addr is a closed TCP port — STUN will fail
  // but daemon.Start() treats that as a warning and falls back to the
  // local listen address. keepalive_sec is large so the trustRepublish
  // loop never fires during the test window. trust_auto_approve avoids
  // the trustedagents plugin's curated list (not registered in
  // embedded mode anyway).
  char config_json[1024];
  snprintf(config_json, sizeof(config_json),
           "{"
           "\"data_dir\":\"%s\","
           "\"socket_path\":\"%s\","
           "\"registry_addr\":\"%s\","
           "\"beacon_addr\":\"127.0.0.1:1\","
           "\"trust_auto_approve\":true,"
           "\"keepalive_sec\":3600,"
           "\"version\":\"mock-embed-1\""
           "}",
           data_dir, socket_path, mockreg_addr);

  char *start_res = PilotEmbeddedStart(config_json);
  if (start_res == NULL) {
    FAIL("embedded_start_with_mock_registry",
         "PilotEmbeddedStart returned NULL");
    stop_mock_registry();
    return;
  }
  if (has_error(start_res)) {
    FAIL("embedded_start_with_mock_registry", start_res);
    free_c_string(start_res);
    // Best-effort stop in case the start half-succeeded.
    char *stop_err = PilotEmbeddedStop();
    if (stop_err) free_c_string(stop_err);
    stop_mock_registry();
    return;
  }
  PASS("embedded_start_with_mock_registry");
  free_c_string(start_res);

  // Calling PilotEmbeddedStart a second time while the singleton is
  // live must error — covers the `embedded.node != nil` early-return
  // branch at the top of PilotEmbeddedStart.
  char *dup_res = PilotEmbeddedStart(config_json);
  if (dup_res == NULL || !has_error(dup_res)) {
    FAIL("embedded_start_double_call", "expected error JSON");
  } else if (strstr(dup_res, "already started") == NULL) {
    FAIL("embedded_start_double_call",
         "expected error to mention already started");
  } else {
    PASS("embedded_start_double_call");
  }
  if (dup_res) free_c_string(dup_res);

  // Connect a driver handle to the freshly-booted embedded daemon and
  // probe both Info and Health. This exercises the same IPC socket
  // pkg/daemon.IPC creates inside Start(), end-to-end, without going
  // through any mock-side IPC. Covers the "embedded boots a real
  // socket the driver can dial" guarantee.
  struct PilotConnect_return conn = PilotConnect(socket_path);
  if (conn.r0 == 0) {
    FAIL("embedded_info_via_driver",
         conn.r1 ? conn.r1 : "PilotConnect failed");
    if (conn.r1) free_c_string(conn.r1);
  } else {
    if (conn.r1) free_c_string(conn.r1);
    char *info = PilotInfo(conn.r0);
    if (!has_no_error(info)) {
      FAIL("embedded_info_via_driver", info ? info : "null");
    } else {
      PASS("embedded_info_via_driver");
    }
    if (info) free_c_string(info);

    char *health = PilotHealth(conn.r0);
    if (!has_no_error(health)) {
      FAIL("embedded_health_via_driver", health ? health : "null");
    } else {
      PASS("embedded_health_via_driver");
    }
    if (health) free_c_string(health);

    char *closed = PilotClose(conn.r0);
    if (closed) free_c_string(closed);
  }

  // Tear down the embedded daemon. PilotEmbeddedStop returns either
  // {"status":"stopped"} or {"status":"stopped","warning":"..."} — both
  // count as success (plugin teardown is best-effort).
  char *stop_res = PilotEmbeddedStop();
  if (stop_res == NULL || has_error(stop_res)) {
    FAIL("embedded_stop_after_start",
         stop_res ? stop_res : "null");
  } else {
    PASS("embedded_stop_after_start");
  }
  if (stop_res) free_c_string(stop_res);

  // Clean filesystem artefacts. The data dir gets rm-rf'd on the
  // process exit anyway, but tidy up so back-to-back runs don't pile
  // up identity.json files.
  unlink(socket_path);

  stop_mock_registry();
}

// Exercises the "data_dir required" guard — first call after a clean
// state, with a config that parses but omits data_dir. PilotEmbeddedStart
// must reject this without booting the daemon.
static void test_embedded_start_missing_data_dir(void) {
  const char *cfg = "{\"socket_path\":\"/tmp/x.sock\"}";
  char *res = PilotEmbeddedStart((char *)cfg);
  if (res == NULL || !has_error(res)) {
    FAIL("embedded_start_missing_data_dir", "expected error JSON");
    if (res) free_c_string(res);
    return;
  }
  if (strstr(res, "data_dir") == NULL) {
    FAIL("embedded_start_missing_data_dir",
         "expected error to mention data_dir");
    free_c_string(res);
    return;
  }
  PASS("embedded_start_missing_data_dir");
  free_c_string(res);
}

// Exercises the "socket_path required" guard with the data_dir branch
// already satisfied. Also confirms the embeddedConfig.defaults() runs:
// the call should fail on missing socket, not on missing registry.
static void test_embedded_start_missing_socket_path(void) {
  const char *cfg = "{\"data_dir\":\"/tmp\"}";
  char *res = PilotEmbeddedStart((char *)cfg);
  if (res == NULL || !has_error(res)) {
    FAIL("embedded_start_missing_socket_path", "expected error JSON");
    if (res) free_c_string(res);
    return;
  }
  if (strstr(res, "socket_path") == NULL) {
    FAIL("embedded_start_missing_socket_path",
         "expected error to mention socket_path");
    free_c_string(res);
    return;
  }
  PASS("embedded_start_missing_socket_path");
  free_c_string(res);
}

// Calls PilotEmbeddedStart with a NULL configJSON pointer. The
// unmarshalCString helper has an explicit nil branch this exercises.
static void test_embedded_start_null_config(void) {
  char *res = PilotEmbeddedStart(NULL);
  if (res == NULL || !has_error(res)) {
    FAIL("embedded_start_null_config", "expected error JSON");
    if (res) free_c_string(res);
    return;
  }
  PASS("embedded_start_null_config");
  free_c_string(res);
}

// ---------------------------------------------------------------------------
// Free path — passing NULL must not crash.
// ---------------------------------------------------------------------------

static void test_free_null(void) {
  FreeString(NULL);
  PASS("free_null");
}

// ---------------------------------------------------------------------------
// Mock-daemon-backed tests
//
// These spawn ./mockdaemon-bin (built by `make mock-daemon`) as a child
// process, point it at a temp Unix socket, and drive every //export
// entry point that needs a connected handle.
//
// Lifecycle: fork+exec the binary, wait until the socket file appears,
// run the tests, then SIGTERM the child. The Makefile guarantees the
// binary is built before `make run`.
// ---------------------------------------------------------------------------

static pid_t mock_pid = 0;
static char mock_socket_path[256] = {0};

// wait_for_socket polls up to ~2 seconds for the Unix socket to appear.
static int wait_for_socket(const char *path) {
  for (int i = 0; i < 200; i++) {
    struct stat st;
    if (stat(path, &st) == 0) return 1;
    struct timespec ts = {0, 10 * 1000 * 1000}; // 10 ms
    nanosleep(&ts, NULL);
  }
  return 0;
}

// start_mock_daemon forks the mock binary and waits for it to bind.
// Returns 1 on success, 0 on failure.
static int start_mock_daemon(void) {
  // Build a temp socket path. Avoid TMPDIR weirdness on darwin (long
  // /var/folders paths can exceed sun_path's 104-byte limit).
  snprintf(mock_socket_path, sizeof(mock_socket_path),
           "/tmp/libpilot-mock-%d.sock", (int)getpid());

  // Best-effort unlink of any stale socket.
  unlink(mock_socket_path);

  pid_t pid = fork();
  if (pid < 0) {
    fprintf(stderr, "fork: %s\n", strerror(errno));
    return 0;
  }
  if (pid == 0) {
    // Child: exec the mock binary.
    execl("./mockdaemon-bin", "mockdaemon-bin",
          "-socket", mock_socket_path, (char *)NULL);
    // execl only returns on failure.
    fprintf(stderr, "execl mockdaemon-bin: %s\n", strerror(errno));
    _exit(127);
  }

  mock_pid = pid;
  if (!wait_for_socket(mock_socket_path)) {
    fprintf(stderr, "mock daemon did not bind socket %s within 2s\n",
            mock_socket_path);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    mock_pid = 0;
    return 0;
  }
  return 1;
}

static void stop_mock_daemon(void) {
  if (mock_pid > 0) {
    kill(mock_pid, SIGTERM);
    int status = 0;
    waitpid(mock_pid, &status, 0);
    mock_pid = 0;
  }
  if (mock_socket_path[0] != '\0') {
    unlink(mock_socket_path);
  }
}

// Each mock-daemon-backed test connects a fresh handle, runs one
// command, and disconnects. Keeps tests independent and lets the
// coverage profile show each export was actually reached.

static uint64_t mock_connect_or_fail(const char *test_name) {
  struct PilotConnect_return r = PilotConnect(mock_socket_path);
  if (r.r0 == 0) {
    FAIL(test_name, "PilotConnect against mock daemon failed");
    if (r.r1) {
      printf("    err: %s\n", r.r1);
      free_c_string(r.r1);
    }
    return 0;
  }
  if (r.r1) free_c_string(r.r1);
  return r.r0;
}

static void mock_close(uint64_t h) {
  char *err = PilotClose(h);
  if (err) free_c_string(err);
}

// has_no_error returns 1 if the JSON looks like a success envelope.
static int has_no_error(const char *json) {
  return json != NULL && !has_error(json);
}

static void test_mock_connect_close(void) {
  uint64_t h = mock_connect_or_fail("mock_connect_close");
  if (!h) return;
  mock_close(h);
  PASS("mock_connect_close");
}

static void test_mock_info(void) {
  uint64_t h = mock_connect_or_fail("mock_info");
  if (!h) return;
  char *res = PilotInfo(h);
  if (!has_no_error(res)) {
    FAIL("mock_info", res ? res : "null");
  } else if (strstr(res, "mock-daemon") == NULL) {
    FAIL("mock_info", "expected canned hostname");
  } else {
    PASS("mock_info");
  }
  if (res) free_c_string(res);
  mock_close(h);
}

static void test_mock_health(void) {
  uint64_t h = mock_connect_or_fail("mock_health");
  if (!h) return;
  char *res = PilotHealth(h);
  if (!has_no_error(res)) {
    FAIL("mock_health", res ? res : "null");
  } else {
    PASS("mock_health");
  }
  if (res) free_c_string(res);
  mock_close(h);
}

static void test_mock_listen(void) {
  uint64_t h = mock_connect_or_fail("mock_listen");
  if (!h) return;
  struct PilotListen_return r = PilotListen(h, 31337);
  if (r.r0 == 0 || (r.r1 && has_error(r.r1))) {
    FAIL("mock_listen", r.r1 ? r.r1 : "no listener handle");
  } else {
    PASS("mock_listen");
    char *cerr = PilotListenerClose(r.r0);
    if (cerr) free_c_string(cerr);
  }
  if (r.r1) free_c_string(r.r1);
  mock_close(h);
}

static void test_mock_dial_and_conn_io(void) {
  uint64_t h = mock_connect_or_fail("mock_dial_io");
  if (!h) return;

  // The mock accepts any well-formed Pilot address. Format:
  // "N:NNNN.HHHH.LLLL:PORT".
  char addr[] = "1:0001.0002.0003:80";
  struct PilotDial_return d = PilotDial(h, addr);
  if (d.r0 == 0 || (d.r1 && has_error(d.r1))) {
    FAIL("mock_dial", d.r1 ? d.r1 : "no conn handle");
    if (d.r1) free_c_string(d.r1);
    mock_close(h);
    return;
  }
  PASS("mock_dial");
  if (d.r1) free_c_string(d.r1);

  // Write some bytes; the mock echoes via server-pushed CmdRecv.
  const char payload[] = "hello mock";
  struct PilotConnWrite_return w =
      PilotConnWrite(d.r0, (void *)payload, (int)strlen(payload));
  if (w.r1 && has_error(w.r1)) {
    FAIL("mock_conn_write", w.r1);
    free_c_string(w.r1);
  } else if (w.r0 != (int)strlen(payload)) {
    FAIL("mock_conn_write", "short write");
  } else {
    PASS("mock_conn_write");
  }
  if (w.r1) free_c_string(w.r1);

  // Read the echo back.
  struct PilotConnRead_return rd = PilotConnRead(d.r0, 64);
  if (rd.r2 && has_error(rd.r2)) {
    FAIL("mock_conn_read", rd.r2);
    free_c_string(rd.r2);
  } else if (rd.r0 != (int)strlen(payload) || rd.r1 == NULL ||
             memcmp(rd.r1, payload, strlen(payload)) != 0) {
    FAIL("mock_conn_read", "echo mismatch");
    if (rd.r1) free_c_string(rd.r1);
  } else {
    PASS("mock_conn_read");
    free_c_string(rd.r1);
  }

  char *cerr = PilotConnClose(d.r0);
  if (cerr) {
    FAIL("mock_conn_close", cerr);
    free_c_string(cerr);
  } else {
    PASS("mock_conn_close");
  }

  mock_close(h);
}

static void test_mock_trusted_peers(void) {
  uint64_t h = mock_connect_or_fail("mock_trusted_peers");
  if (!h) return;
  char *res = PilotTrustedPeers(h);
  if (!has_no_error(res)) {
    FAIL("mock_trusted_peers", res ? res : "null");
  } else {
    PASS("mock_trusted_peers");
  }
  if (res) free_c_string(res);
  mock_close(h);
}

static void test_mock_pending_handshakes(void) {
  uint64_t h = mock_connect_or_fail("mock_pending_handshakes");
  if (!h) return;
  char *res = PilotPendingHandshakes(h);
  if (!has_no_error(res)) {
    FAIL("mock_pending_handshakes", res ? res : "null");
  } else {
    PASS("mock_pending_handshakes");
  }
  if (res) free_c_string(res);
  mock_close(h);
}

static void test_mock_handshake_send(void) {
  uint64_t h = mock_connect_or_fail("mock_handshake_send");
  if (!h) return;
  char just[] = "just-for-coverage";
  char *res = PilotHandshake(h, 42, just);
  if (!has_no_error(res)) {
    FAIL("mock_handshake_send", res ? res : "null");
  } else {
    PASS("mock_handshake_send");
  }
  if (res) free_c_string(res);
  mock_close(h);
}

static void test_mock_approve_reject_revoke(void) {
  uint64_t h = mock_connect_or_fail("mock_approve_reject_revoke");
  if (!h) return;

  char *a = PilotApproveHandshake(h, 7);
  if (!has_no_error(a)) {
    FAIL("mock_approve", a ? a : "null");
  } else {
    PASS("mock_approve");
  }
  if (a) free_c_string(a);

  char reason[] = "no thanks";
  char *r = PilotRejectHandshake(h, 7, reason);
  if (!has_no_error(r)) {
    FAIL("mock_reject", r ? r : "null");
  } else {
    PASS("mock_reject");
  }
  if (r) free_c_string(r);

  char *rv = PilotRevokeTrust(h, 7);
  if (!has_no_error(rv)) {
    FAIL("mock_revoke", rv ? rv : "null");
  } else {
    PASS("mock_revoke");
  }
  if (rv) free_c_string(rv);

  mock_close(h);
}

static void test_mock_resolve_set_hostname(void) {
  uint64_t h = mock_connect_or_fail("mock_resolve_set_hostname");
  if (!h) return;

  char host[] = "alice";
  char *r = PilotResolveHostname(h, host);
  if (!has_no_error(r)) {
    FAIL("mock_resolve_hostname", r ? r : "null");
  } else {
    PASS("mock_resolve_hostname");
  }
  if (r) free_c_string(r);

  char *s = PilotSetHostname(h, host);
  if (!has_no_error(s)) {
    FAIL("mock_set_hostname", s ? s : "null");
  } else {
    PASS("mock_set_hostname");
  }
  if (s) free_c_string(s);

  mock_close(h);
}

static void test_mock_visibility_deregister(void) {
  uint64_t h = mock_connect_or_fail("mock_visibility");
  if (!h) return;

  char *v = PilotSetVisibility(h, 1);
  if (!has_no_error(v)) {
    FAIL("mock_set_visibility", v ? v : "null");
  } else {
    PASS("mock_set_visibility");
  }
  if (v) free_c_string(v);

  char *d = PilotDeregister(h);
  if (!has_no_error(d)) {
    FAIL("mock_deregister", d ? d : "null");
  } else {
    PASS("mock_deregister");
  }
  if (d) free_c_string(d);

  mock_close(h);
}

static void test_mock_set_tags_webhook(void) {
  uint64_t h = mock_connect_or_fail("mock_tags_webhook");
  if (!h) return;

  char tags[] = "[\"alpha\",\"beta\"]";
  char *t = PilotSetTags(h, tags);
  if (!has_no_error(t)) {
    FAIL("mock_set_tags", t ? t : "null");
  } else {
    PASS("mock_set_tags");
  }
  if (t) free_c_string(t);

  char url[] = "https://example.test/webhook";
  char *w = PilotSetWebhook(h, url);
  if (!has_no_error(w)) {
    FAIL("mock_set_webhook", w ? w : "null");
  } else {
    PASS("mock_set_webhook");
  }
  if (w) free_c_string(w);

  mock_close(h);
}

static void test_mock_network_list(void) {
  uint64_t h = mock_connect_or_fail("mock_network_list");
  if (!h) return;
  char *res = PilotNetworkList(h);
  if (!has_no_error(res)) {
    FAIL("mock_network_list", res ? res : "null");
  } else {
    PASS("mock_network_list");
  }
  if (res) free_c_string(res);
  mock_close(h);
}

static void test_mock_network_join_leave_members(void) {
  uint64_t h = mock_connect_or_fail("mock_network_jlm");
  if (!h) return;

  char token[] = "";
  char *j = PilotNetworkJoin(h, 1, token);
  if (!has_no_error(j)) {
    FAIL("mock_network_join", j ? j : "null");
  } else {
    PASS("mock_network_join");
  }
  if (j) free_c_string(j);

  char *m = PilotNetworkMembers(h, 1);
  if (!has_no_error(m)) {
    FAIL("mock_network_members", m ? m : "null");
  } else {
    PASS("mock_network_members");
  }
  if (m) free_c_string(m);

  char *l = PilotNetworkLeave(h, 1);
  if (!has_no_error(l)) {
    FAIL("mock_network_leave", l ? l : "null");
  } else {
    PASS("mock_network_leave");
  }
  if (l) free_c_string(l);

  mock_close(h);
}

static void test_mock_managed(void) {
  uint64_t h = mock_connect_or_fail("mock_managed");
  if (!h) return;

  char *s = PilotManagedStatus(h, 1);
  if (!has_no_error(s)) {
    FAIL("mock_managed_status", s ? s : "null");
  } else {
    PASS("mock_managed_status");
  }
  if (s) free_c_string(s);

  char *f = PilotManagedForceCycle(h, 1);
  if (!has_no_error(f)) {
    FAIL("mock_managed_force_cycle", f ? f : "null");
  } else {
    PASS("mock_managed_force_cycle");
  }
  if (f) free_c_string(f);

  char *r = PilotManagedReconcile(h, 1);
  if (!has_no_error(r)) {
    FAIL("mock_managed_reconcile", r ? r : "null");
  } else {
    PASS("mock_managed_reconcile");
  }
  if (r) free_c_string(r);

  mock_close(h);
}

static void test_mock_rotate_key(void) {
  uint64_t h = mock_connect_or_fail("mock_rotate_key");
  if (!h) return;
  char *r = PilotRotateKey(h);
  if (!has_no_error(r)) {
    FAIL("mock_rotate_key", r ? r : "null");
  } else {
    PASS("mock_rotate_key");
  }
  if (r) free_c_string(r);
  mock_close(h);
}

static void test_mock_broadcast(void) {
  uint64_t h = mock_connect_or_fail("mock_broadcast");
  if (!h) return;
  char tok[] = "admin-token";
  char payload[] = "broadcast-payload";
  char *r = PilotBroadcast(h, 1, 80, payload, (int)strlen(payload), tok);
  if (!has_no_error(r)) {
    FAIL("mock_broadcast", r ? r : "null");
  } else {
    PASS("mock_broadcast");
  }
  if (r) free_c_string(r);
  mock_close(h);
}

static void test_mock_send_to(void) {
  uint64_t h = mock_connect_or_fail("mock_send_to");
  if (!h) return;
  char addr[] = "1:0001.0002.0003:80";
  char data[] = "datagram";
  char *r = PilotSendTo(h, addr, data, (int)strlen(data));
  if (r != NULL) {
    FAIL("mock_send_to", r);
    free_c_string(r);
  } else {
    PASS("mock_send_to");
  }
  mock_close(h);
}

static void test_mock_wait_for_trust(void) {
  uint64_t h = mock_connect_or_fail("mock_wait_for_trust");
  if (!h) return;
  // Use 0 ms timeout — mock replies immediately, regardless.
  char *r = PilotWaitForTrust(h, 42, 0);
  if (!has_no_error(r)) {
    FAIL("mock_wait_for_trust", r ? r : "null");
  } else {
    PASS("mock_wait_for_trust");
  }
  if (r) free_c_string(r);
  mock_close(h);
}

static void test_mock_conn_set_read_deadline(void) {
  uint64_t h = mock_connect_or_fail("mock_set_read_deadline");
  if (!h) return;
  char addr[] = "1:0001.0002.0003:80";
  struct PilotDial_return d = PilotDial(h, addr);
  if (d.r0 == 0) {
    FAIL("mock_set_read_deadline", "dial failed");
    if (d.r1) free_c_string(d.r1);
    mock_close(h);
    return;
  }
  if (d.r1) free_c_string(d.r1);

  // Clear deadline (0) then set a far-future one.
  char *e1 = PilotConnSetReadDeadline(d.r0, 0);
  if (e1) {
    FAIL("mock_set_read_deadline_clear", e1);
    free_c_string(e1);
  } else {
    PASS("mock_set_read_deadline_clear");
  }

  char *e2 = PilotConnSetReadDeadline(d.r0, (int64_t)1ULL << 60);
  if (e2) {
    FAIL("mock_set_read_deadline_future", e2);
    free_c_string(e2);
  } else {
    PASS("mock_set_read_deadline_future");
  }

  char *cerr = PilotConnClose(d.r0);
  if (cerr) free_c_string(cerr);
  mock_close(h);
}

static void test_mock_network_invite_polls(void) {
  uint64_t h = mock_connect_or_fail("mock_network_invite_polls");
  if (!h) return;

  char *i = PilotNetworkInvite(h, 1, 0xCAFE);
  if (!has_no_error(i)) {
    FAIL("mock_network_invite", i ? i : "null");
  } else {
    PASS("mock_network_invite");
  }
  if (i) free_c_string(i);

  char *p = PilotNetworkPollInvites(h);
  if (!has_no_error(p)) {
    FAIL("mock_network_poll_invites", p ? p : "null");
  } else {
    PASS("mock_network_poll_invites");
  }
  if (p) free_c_string(p);

  char *r = PilotNetworkRespondInvite(h, 1, 1);
  if (!has_no_error(r)) {
    FAIL("mock_network_respond_invite", r ? r : "null");
  } else {
    PASS("mock_network_respond_invite");
  }
  if (r) free_c_string(r);

  mock_close(h);
}

static void test_mock_policy(void) {
  uint64_t h = mock_connect_or_fail("mock_policy");
  if (!h) return;

  char *g = PilotPolicyGet(h, 1);
  if (!has_no_error(g)) {
    FAIL("mock_policy_get", g ? g : "null");
  } else {
    PASS("mock_policy_get");
  }
  if (g) free_c_string(g);

  char policy[] = "{\"rules\":[]}";
  char *s = PilotPolicySet(h, 1, policy);
  if (!has_no_error(s)) {
    FAIL("mock_policy_set", s ? s : "null");
  } else {
    PASS("mock_policy_set");
  }
  if (s) free_c_string(s);

  mock_close(h);
}

static void test_mock_member_tags(void) {
  uint64_t h = mock_connect_or_fail("mock_member_tags");
  if (!h) return;

  char *g = PilotMemberTagsGet(h, 1, 0xBEEF);
  if (!has_no_error(g)) {
    FAIL("mock_member_tags_get", g ? g : "null");
  } else {
    PASS("mock_member_tags_get");
  }
  if (g) free_c_string(g);

  char tags[] = "[\"role:worker\"]";
  char *s = PilotMemberTagsSet(h, 1, 0xBEEF, tags);
  if (!has_no_error(s)) {
    FAIL("mock_member_tags_set", s ? s : "null");
  } else {
    PASS("mock_member_tags_set");
  }
  if (s) free_c_string(s);

  mock_close(h);
}

static void test_mock_disconnect(void) {
  uint64_t h = mock_connect_or_fail("mock_disconnect");
  if (!h) return;
  // Disconnect by ID is fire-and-forget on the wire — error only on
  // bad handle.
  char *r = PilotDisconnect(h, 12345);
  if (r != NULL) {
    FAIL("mock_disconnect", r);
    free_c_string(r);
  } else {
    PASS("mock_disconnect");
  }
  mock_close(h);
}

// PilotRecvFrom — exercises the datagram-receive happy path.
//
// The mock daemon reflects every cmdSendTo back to the same client as
// a cmdRecvFrom (loopback semantics). We send a datagram, then call
// PilotRecvFrom and verify the JSON envelope contains our payload and
// the synthetic src_port=0xDEAD that the mock injects.
static void test_mock_recv_from(void) {
  uint64_t h = mock_connect_or_fail("mock_recv_from");
  if (!h) return;

  char addr[] = "1:0001.0002.0003:80";
  char data[] = "loopback-dg";
  char *send_err = PilotSendTo(h, addr, data, (int)strlen(data));
  if (send_err != NULL) {
    FAIL("mock_recv_from", send_err);
    free_c_string(send_err);
    mock_close(h);
    return;
  }

  // The reflected cmdRecvFrom is server-pushed; it lands on the driver's
  // dgCh asynchronously. PilotRecvFrom blocks on that channel.
  char *res = PilotRecvFrom(h);
  if (!has_no_error(res)) {
    FAIL("mock_recv_from", res ? res : "null");
    if (res) free_c_string(res);
    mock_close(h);
    return;
  }

  // Payload bytes are base64 over JSON; rather than decoding, just check
  // for fields we know the mock fills in. src_port=0xDEAD=57005.
  if (strstr(res, "\"src_port\":57005") == NULL) {
    FAIL("mock_recv_from", "expected src_port=57005 (0xDEAD)");
    printf("    got: %s\n", res);
    free_c_string(res);
    mock_close(h);
    return;
  }
  if (strstr(res, "\"dst_port\":80") == NULL) {
    FAIL("mock_recv_from", "expected dst_port=80 echoed back");
    printf("    got: %s\n", res);
    free_c_string(res);
    mock_close(h);
    return;
  }
  PASS("mock_recv_from");
  free_c_string(res);
  mock_close(h);
}

// PilotListenerAccept — exercises the listener-accept happy path with
// TWO concurrent client handles against the same mock daemon.
//
// Handle A: PilotListen(port=42017). The mock records the bind in its
//           process-wide listenerRegistry.
// Handle B: PilotDial("...:42017"). The mock replies cmdDialOK to B AND
//           pushes a cmdAccept frame onto A's socket.
// Handle A: PilotListenerAccept returns a new Conn — the accepted side.
//
// This is the only path that actually flows a cmdAccept through the
// driver's readLoop into a Listener's per-port channel — every other
// PilotListenerAccept callsite in this harness hits the error branch.
static void test_mock_listener_accept(void) {
  const uint16_t port = 42017;

  uint64_t a = mock_connect_or_fail("mock_listener_accept_listen");
  if (!a) return;
  uint64_t b = mock_connect_or_fail("mock_listener_accept_dial");
  if (!b) {
    mock_close(a);
    return;
  }

  struct PilotListen_return ln = PilotListen(a, port);
  if (ln.r0 == 0 || (ln.r1 && has_error(ln.r1))) {
    FAIL("mock_listener_accept", ln.r1 ? ln.r1 : "listen failed");
    if (ln.r1) free_c_string(ln.r1);
    mock_close(a);
    mock_close(b);
    return;
  }
  if (ln.r1) free_c_string(ln.r1);

  // Build the dial addr: "1:0001.0002.0003:42017". The mock parses the
  // 6-byte addr + 2-byte port from the wire — the text form is only
  // used by libpilot to construct the bytes.
  char dial_addr[64];
  snprintf(dial_addr, sizeof(dial_addr), "1:0001.0002.0003:%u",
           (unsigned)port);
  struct PilotDial_return d = PilotDial(b, dial_addr);
  if (d.r0 == 0 || (d.r1 && has_error(d.r1))) {
    FAIL("mock_listener_accept", d.r1 ? d.r1 : "dial failed");
    if (d.r1) free_c_string(d.r1);
    char *lc = PilotListenerClose(ln.r0);
    if (lc) free_c_string(lc);
    mock_close(a);
    mock_close(b);
    return;
  }
  if (d.r1) free_c_string(d.r1);

  // The dial triggers the mock to push cmdAccept onto handle A's socket.
  // PilotListenerAccept blocks until that frame lands.
  struct PilotListenerAccept_return acc = PilotListenerAccept(ln.r0);
  if (acc.r0 == 0 || (acc.r1 && has_error(acc.r1))) {
    FAIL("mock_listener_accept", acc.r1 ? acc.r1 : "accept failed");
    if (acc.r1) free_c_string(acc.r1);
    char *cc = PilotConnClose(d.r0);
    if (cc) free_c_string(cc);
    char *lc = PilotListenerClose(ln.r0);
    if (lc) free_c_string(lc);
    mock_close(a);
    mock_close(b);
    return;
  }
  if (acc.r1) free_c_string(acc.r1);
  PASS("mock_listener_accept");

  // Tear down the accepted conn + the dialer conn + the listener.
  char *ac = PilotConnClose(acc.r0);
  if (ac) free_c_string(ac);
  char *dc = PilotConnClose(d.r0);
  if (dc) free_c_string(dc);
  char *lc = PilotListenerClose(ln.r0);
  if (lc) free_c_string(lc);

  mock_close(a);
  mock_close(b);
}

// PilotDialTimeout against the mock daemon — exercises the
// ParseSocketAddr success path AND the d.DialAddrTimeout path that the
// bad-handle test could never reach. Uses a non-zero timeout so the
// timeoutMs cast doesn't trip the overflow check.
static void test_mock_dial_timeout(void) {
  uint64_t h = mock_connect_or_fail("mock_dial_timeout");
  if (!h) return;
  char addr[] = "1:0001.0002.0003:80";
  struct PilotDialTimeout_return r = PilotDialTimeout(h, addr, 5000);
  if (r.r0 == 0 || (r.r1 && has_error(r.r1))) {
    FAIL("mock_dial_timeout", r.r1 ? r.r1 : "no conn handle");
    if (r.r1) free_c_string(r.r1);
    mock_close(h);
    return;
  }
  PASS("mock_dial_timeout");
  if (r.r1) free_c_string(r.r1);
  char *cc = PilotConnClose(r.r0);
  if (cc) free_c_string(cc);
  mock_close(h);
}

// Drives the ParseSocketAddr error branch in PilotDialTimeout. Requires
// a valid handle so the driverFromHandle path passes, then a malformed
// addr to fail ParseSocketAddr.
static void test_mock_dial_timeout_bad_addr(void) {
  uint64_t h = mock_connect_or_fail("mock_dial_timeout_bad_addr");
  if (!h) return;
  char addr[] = "not-a-valid-socket-addr";
  struct PilotDialTimeout_return r = PilotDialTimeout(h, addr, 100);
  if (r.r0 != 0 || r.r1 == NULL || !has_error(r.r1)) {
    FAIL("mock_dial_timeout_bad_addr", "expected parse error");
    if (r.r1) free_c_string(r.r1);
    mock_close(h);
    return;
  }
  PASS("mock_dial_timeout_bad_addr");
  free_c_string(r.r1);
  mock_close(h);
}

// Drives the json.Unmarshal error branch in PilotMemberTagsSet. The
// handle-validation path is already covered; this one carries a malformed
// JSON body so the second early-return executes.
static void test_mock_member_tags_set_bad_json(void) {
  uint64_t h = mock_connect_or_fail("mock_member_tags_set_bad_json");
  if (!h) return;
  char bad[] = "{not a list}";
  char *err = PilotMemberTagsSet(h, 1, 1, bad);
  if (err == NULL || !has_error(err)) {
    FAIL("mock_member_tags_set_bad_json", "expected json error");
    if (err) free_c_string(err);
    mock_close(h);
    return;
  }
  if (strstr(err, "invalid tags JSON") == NULL) {
    FAIL("mock_member_tags_set_bad_json",
         "expected error to mention invalid tags JSON");
    free_c_string(err);
    mock_close(h);
    return;
  }
  PASS("mock_member_tags_set_bad_json");
  free_c_string(err);
  mock_close(h);
}

// ---------------------------------------------------------------------------
// Run all
// ---------------------------------------------------------------------------

int main(void) {
  printf("libpilot C integration harness\n");
  printf("===============================\n");

  // Handle-table edges
  test_close_zero_handle();
  test_close_unknown_handle();
  test_listener_close_bad_handle();
  test_conn_close_bad_handle();
  test_disconnect_bad_handle();
  test_dial_timeout_bad_handle();
  test_dial_timeout_malformed_addr();
  test_member_tags_set_bad_handle();
  test_member_tags_set_invalid_json();
  test_conn_set_read_deadline_bad_handle();

  // Info / health / queries
  test_info_bad_handle();
  test_health_bad_handle();
  test_trusted_peers_bad_handle();
  test_pending_handshakes_bad_handle();

  // Param validation
  test_conn_read_zero_bufsize();
  test_conn_read_negative_bufsize();
  test_conn_write_null_data();
  test_conn_write_negative_datalen();
  test_dial_null_addr();
  test_dial_malformed_addr();
  test_dial_timeout_huge();
  test_broadcast_negative_datalen();
  test_send_to_negative_datalen();

  // Connect / listen / accept on bad inputs
  test_connect_missing_socket();
  test_connect_null_path();
  test_listen_bad_handle();
  test_accept_bad_handle();

  // String-arg endpoints with NULL
  test_handshake_null_justification();
  test_set_hostname_null();
  test_resolve_hostname_null();
  test_set_webhook_null();
  test_set_tags_invalid_json();

  // Embedded daemon
  test_embedded_start_invalid_json();
  test_embedded_stop_when_not_running();
  test_embedded_start_null_config();
  test_embedded_start_missing_data_dir();
  test_embedded_start_missing_socket_path();
  // Full-boot test uses ./mockregistry-bin — spawned, queried, killed
  // inside the test. Runs last in this group so a hard fail (mock
  // binary missing, daemon Start panics) doesn't poison earlier checks.
  test_embedded_start_with_mock_registry();

  // Free
  test_free_null();

  // ----- Mock-daemon-backed tests -----
  // Spawn the mock binary in a child process, then drive every
  // //export endpoint that needs a connected handle.
  if (start_mock_daemon()) {
    printf("\n[mock daemon] pid=%d socket=%s\n", (int)mock_pid,
           mock_socket_path);

    test_mock_connect_close();
    test_mock_info();
    test_mock_health();
    test_mock_listen();
    test_mock_dial_and_conn_io();
    test_mock_trusted_peers();
    test_mock_pending_handshakes();
    test_mock_handshake_send();
    test_mock_approve_reject_revoke();
    test_mock_resolve_set_hostname();
    test_mock_visibility_deregister();
    test_mock_set_tags_webhook();
    test_mock_network_list();
    test_mock_network_join_leave_members();
    test_mock_managed();
    test_mock_rotate_key();
    test_mock_broadcast();
    test_mock_send_to();
    test_mock_disconnect();
    test_mock_wait_for_trust();
    test_mock_conn_set_read_deadline();
    test_mock_network_invite_polls();
    test_mock_policy();
    test_mock_member_tags();
    test_mock_recv_from();
    test_mock_listener_accept();
    test_mock_dial_timeout();
    test_mock_dial_timeout_bad_addr();
    test_mock_member_tags_set_bad_json();

    stop_mock_daemon();
  } else {
    fail_count++;
    printf("  FAIL mock_daemon_startup: could not spawn mockdaemon-bin "
           "(run `make mock-daemon` first)\n");
  }

  printf("===============================\n");
  printf("PASS: %d\n", pass_count);
  printf("FAIL: %d\n", fail_count);

  // Flush coverage counters before exit. PilotCoverFlush exists only
  // in coverage builds (see ../coverflush.go, build-tagged
  // `coverflush`). The Makefile passes `-tags coverflush` when
  // building the instrumented dylib, so this symbol resolves.
  char *cov_err = PilotCoverFlush();
  if (cov_err != NULL) {
    printf("cover flush: %s\n", cov_err);
    free_c_string(cov_err);
  }

  return fail_count == 0 ? 0 : 1;
}
