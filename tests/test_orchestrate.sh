#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>
#
# End-to-end protocol test over the verification binary: the
# same engine as the mounted translator, over standard streams,
# with the test transports (dir: instances, mem storage).  It
# runs on any POSIX system, without Hurd, without PostgreSQL.
# The runs and their status/result share ONE session: the engine
# state lives with the process (a mounted translator holds it in
# the mount instead).

set -u

BINARY="${1:-../src/orchestrator-translator}"
FAIL=0
WORK=""

say() { printf '%s\n' "$*"; }
fail() { say "test_orchestrate: FAIL: $*"; FAIL=1; }

cleanup() { [ -n "$WORK" ] && rm -rf "$WORK"; }
trap cleanup EXIT INT TERM

if [ ! -x "$BINARY" ]; then
    say "test_orchestrate: SKIP: $BINARY not built"
    exit 77
fi

WORK=$(mktemp -d)

# The simulated instances: two agree, one disagrees.
for i in 1 2 3; do
    : > "$WORK/llm$i.in"
done
printf 'Output:\n[0]: 0.9\n[1]: 0.8\n' > "$WORK/llm1.out"
printf 'Output:\n[0]: 0.8\n[1]: 0.9\n' > "$WORK/llm2.out"
printf 'Output:\n[0]: 0.2\n[1]: 0.1\n' > "$WORK/llm3.out"

run() {
    printf '%s\n' "$1" | "$BINARY" --instances "dir:$WORK" \
        --storage mem 2>/dev/null
}

# --- 1. Two runs, then status and result, in one session ------------
IN='{"instances": 3, "topologies": ["2,4,1"], "input": [0.5, 0.3], "aggregate": "majority"}
{"run": {"instances": 2, "topologies": ["2,4,1"], "input": [0.5, 0.3], "aggregate": "mean"}}
{"command": "status"}
{"command": "result"}'
OUT=$(run "$IN")
[ "$?" = 0 ] || fail "the session must exit 0"

L1=$(printf '%s\n' "$OUT" | sed -n '1p')
printf '%s' "$L1" | grep -q '"run_id": 1' \
    || fail "the first run must be attributed run_id 1"
printf '%s' "$L1" | grep -q '"aggregate_strategy": "majority"' \
    || fail "the first result must name majority"
printf '%s' "$L1" | grep -q '"output": "\[1,1\]"' \
    || fail "the majority of [0.9,0.8] [0.8,0.9] [0.2,0.1] is [1,1]"
printf '%s' "$L1" | grep -q '"confidence": 0.667' \
    || fail "two of three instances agree: 0.667"

L2=$(printf '%s\n' "$OUT" | sed -n '2p')
printf '%s' "$L2" | grep -q '"run_id": 2' \
    || fail "the second run must be attributed run_id 2"
printf '%s' "$L2" | grep -q '"aggregate_strategy": "mean"' \
    || fail "the second result must name mean"
printf '%s' "$L2" | grep -q '"output": "\[0.85,0.85\]"' \
    || fail "the mean of the two first instances is [0.85,0.85]"

L3=$(printf '%s\n' "$OUT" | sed -n '3p')
printf '%s' "$L3" | grep -q '"run_id": 2' \
    || fail "the status must report the last run"
printf '%s' "$L3" | grep -q '"state": "done"' \
    || fail "the status after the runs must be done"
printf '%s' "$L3" | grep -q '"id": 2' \
    || fail "the status must list the second instance"

L4=$(printf '%s\n' "$OUT" | sed -n '4p')
printf '%s' "$L4" | grep -q '"output": "\[0.85,0.85\]"' \
    || fail "the result command must serve the last result"

# --- 2. Contract violations are answers, never crashes -------------
OUT=$(run '{"instances": 0}')
printf '%s' "$OUT" | grep -q '{"invalid": "instances"}' \
    || fail "an out-of-bounds descriptor must answer invalid instances"

OUT=$(run '{"unknown": 1}')
printf '%s' "$OUT" | grep -q '{"invalid": "unknown"}' \
    || fail "an unknown top-level key must answer with the offending key"

OUT=$(run '{"command": "result"}')
printf '%s' "$OUT" | grep -q '{"empty": true}' \
    || fail "a fresh session has no result yet"

# --- 3. The instances received the contract's lines ----------------
for i in 1 2 3; do
    grep -q '^2,4,1$' "$WORK/llm$i.in" \
        || fail "instance $i must have received its topology line"
    grep -q '^0.5,0.3$' "$WORK/llm$i.in" \
        || fail "instance $i must have received its input line"
done

# --- 4. A silent instance fails, the run survives ------------------
printf 'Output:\n(none)\n' > "$WORK/llm1.out"
OUT=$(run '{"instances": 2, "topologies": ["2,4,1"], "input": [0.5, 0.3], "aggregate": "majority"}
{"command": "status"}')
L1=$(printf '%s\n' "$OUT" | sed -n '1p')
printf '%s' "$L1" | grep -q '"state": "done"' \
    || fail "the run must survive a silent instance"
printf '%s' "$L1" | grep -q '"output": "\[1,1\]"' \
    || fail "the aggregate of the answering instance is [1,1]"
L2=$(printf '%s\n' "$OUT" | sed -n '2p')
printf '%s' "$L2" | grep -q '"state": "failed"' \
    || fail "the silent instance must be reported failed"

if [ "$FAIL" = 0 ]; then
    say "test_orchestrate: OK"
    exit 0
fi
exit 1
