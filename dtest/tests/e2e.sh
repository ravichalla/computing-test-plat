#!/usr/bin/env bash
# End-to-end test of the real binaries: two agents on loopback, one controller run each.
# Usage: e2e.sh /path/to/dtest-agent /path/to/dtest-ctl
set -euo pipefail

AGENT="$1"
CTL="$2"
tmp="$(mktemp -d)"
pids=()
cleanup() {
    for p in "${pids[@]:-}"; do kill "$p" 2>/dev/null || true; done
    wait 2>/dev/null || true
    rm -rf "$tmp"
}
trap cleanup EXIT

pass() { echo "  ok: $1"; }
die() { echo "  FAILED: $1" >&2; [[ -f "${2:-}" ]] && sed 's/^/    | /' "$2" >&2; exit 1; }

export DTEST_TOKEN="e2e-secret"

# start_agent NAME [extra agent args...]  -> sets ADDR to host:port
start_agent() {
    local name="$1"; shift
    "$AGENT" --listen 127.0.0.1:0 --allow /bin/ --allow /usr/bin/ --slots 2 "$@" >"$tmp/$name.log" 2>&1 &
    pids+=("$!")
    LAST_PID="$!"
    for _ in $(seq 1 100); do
        grep -q "listening on" "$tmp/$name.log" 2>/dev/null && break
        sleep 0.1
    done
    ADDR="$(grep -oE '127\.0\.0\.1:[0-9]+' "$tmp/$name.log" | head -1)"
    [[ -n "$ADDR" ]] || die "agent $name did not start" "$tmp/$name.log"
}

echo "dtest e2e"
start_agent a --label role=a; A="$ADDR"; PID_A="$LAST_PID"
start_agent b --label role=b; B="$ADDR"; PID_B="$LAST_PID"

# --- 1. an all-green job across both agents, with JUnit output
cat >"$tmp/green.json" <<JSON
{"name":"green","defaults":{"timeout_s":10},"tasks":[
  {"name":"hello","argv":["/bin/sh","-c","echo hello"]},
  {"name":"burst","argv":["/bin/sh","-c","sleep 0.1"],"repeat":8},
  {"name":"only-b","argv":["/bin/sh","-c","echo b"],"requires":["role=b"]}
]}
JSON
"$CTL" --jobs "$tmp/green.json" --agent "$A" --agent "$B" --junit "$tmp/green.xml" >"$tmp/green.out" 2>&1 \
    || die "green job should exit 0" "$tmp/green.out"
grep -q '10 tasks: 10 passed' "$tmp/green.out" || die "expected 10 passed" "$tmp/green.out"
grep -q 'tests="10" failures="0"' "$tmp/green.xml" || die "junit counts wrong" "$tmp/green.xml"
grep -E 'PASS\] only-b on' "$tmp/green.out" | grep -q "$B" || die "only-b must run on agent b ($B)" "$tmp/green.out"
pass "green job passes; label routing and JUnit output work"

# --- 2. a failing task makes the controller exit 1 and shows the agent's stderr
cat >"$tmp/red.json" <<JSON
{"tasks":[
  {"name":"good","argv":["/bin/sh","-c","true"]},
  {"name":"bad","argv":["/bin/sh","-c","echo diagnostic-text >&2; exit 3"],"retries":1}
]}
JSON
rc=0; "$CTL" --jobs "$tmp/red.json" --agent "$A" --agent "$B" >"$tmp/red.out" 2>&1 || rc=$?
[[ $rc -eq 1 ]] || die "failing job should exit 1 (got $rc)" "$tmp/red.out"
grep -q '\[FAIL\] bad' "$tmp/red.out" || die "expected [FAIL] bad" "$tmp/red.out"
grep -q '2 attempts' "$tmp/red.out" || die "retry should have happened" "$tmp/red.out"
grep -q 'diagnostic-text' "$tmp/red.out" || die "stderr should be shown" "$tmp/red.out"
pass "failing task exits 1, is retried, and shows stderr"

# --- 3. wrong token: nothing reachable
rc=0; DTEST_TOKEN=wrong "$CTL" --jobs "$tmp/green.json" --agent "$A" >"$tmp/tok.out" 2>&1 || rc=$?
[[ $rc -eq 1 ]] || die "wrong token should exit 1 (got $rc)" "$tmp/tok.out"
grep -q 'no agent is reachable' "$tmp/tok.out" || die "expected 'no agent is reachable'" "$tmp/tok.out"
pass "wrong token is rejected"

# --- 4. agent b dies: the run still completes on a; the task that needs b is reported, not hung
kill "$PID_B"; wait "$PID_B" 2>/dev/null || true
rc=0; "$CTL" --jobs "$tmp/green.json" --agent "$A" --agent "$B" --connect-timeout 2 >"$tmp/down.out" 2>&1 || rc=$?
[[ $rc -eq 1 ]] || die "expected exit 1 because only-b cannot run (got $rc)" "$tmp/down.out"
grep -q 'warning: agent .* unreachable' "$tmp/down.out" || die "expected unreachable warning" "$tmp/down.out"
grep -q '\[SKIPPED\] only-b' "$tmp/down.out" || die "only-b should be skipped, not hang" "$tmp/down.out"
grep -q '9 passed' "$tmp/down.out" || die "the other 9 tasks should pass on agent a" "$tmp/down.out"
pass "dead agent is tolerated; task that needs it is reported as unschedulable"

# --- 5. job-file validation
echo '{"tasks":[{"name":"x","argv":["/bin/true"],"retires":1}]}' >"$tmp/typo.json"
rc=0; "$CTL" --jobs "$tmp/typo.json" --validate >"$tmp/typo.out" 2>&1 || rc=$?
[[ $rc -eq 2 ]] || die "typo'd job file should exit 2 (got $rc)" "$tmp/typo.out"
grep -q "unknown key 'retires'" "$tmp/typo.out" || die "expected typo message" "$tmp/typo.out"
pass "job-file typos are rejected"

# --- 6. agent refuses to start unsafely
rc=0; env -u DTEST_TOKEN "$AGENT" --listen 127.0.0.1:0 --allow /bin/ >"$tmp/unsafe1.out" 2>&1 || rc=$?
[[ $rc -eq 2 ]] || die "agent without an auth choice should refuse (rc=$rc)" "$tmp/unsafe1.out"
rc=0; "$AGENT" --listen 127.0.0.1:0 --token x >"$tmp/unsafe2.out" 2>&1 || rc=$?
[[ $rc -eq 2 ]] || die "agent without a command policy should refuse (rc=$rc)" "$tmp/unsafe2.out"
pass "agent refuses to start without auth and a command policy"

echo "dtest e2e: all passed"
