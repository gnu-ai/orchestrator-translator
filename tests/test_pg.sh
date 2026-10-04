#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>
#
# The phase 1 acceptance scenario, played end to end on any POSIX
# system with the test transports: a run with 2 instances, then a
# run with 8 instances, against a DISPOSABLE PostgreSQL cluster
# through the real data-base-translator (dbexec backend: the same
# line protocol, the same binary, spawned as a child).
#
# Skips (77) when the PostgreSQL tools, or a built
# db-translator binary, are not available on this machine.
#
# DBT_BINARY: where to find db-translator
# (default: the sibling data-base-translator build tree).

set -u

# The verification binary speaks the line protocol on stdio: on
# GNU/Hurd it is built as orchestrator-translator-repl; everywhere
# else orchestrator-translator IS the verification binary.
BINARY="${1:-../src/orchestrator-translator}"
if [ -z "${1:-}" ] && [ -x ../src/orchestrator-translator-repl ]; then
    BINARY=../src/orchestrator-translator-repl
fi
# The data-base-translator verification binary it drives follows
# the same convention (under GNU/Hurd the -repl name).
DBT_DEFAULT=../../../data-base-translator/build/src/db-translator
if [ -x "$DBT_DEFAULT-repl" ]; then
    DBT_DEFAULT="$DBT_DEFAULT-repl"
fi
DBT_BINARY="${DBT_BINARY:-$DBT_DEFAULT}"
FAIL=0
WORK=""
CLUSTER=""

say() { printf '%s\n' "$*"; }
fail() { say "test_pg: FAIL: $*"; FAIL=1; }

cleanup()
{
    if [ -n "$CLUSTER" ] && [ -d "$CLUSTER" ]; then
        pg_ctl -D "$CLUSTER" stop -m immediate >/dev/null 2>&1
    fi
    [ -n "$WORK" ] && rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

if [ ! -x "$BINARY" ]; then
    say "test_pg: SKIP: $BINARY not built"
    exit 77
fi
if [ ! -x "$DBT_BINARY" ]; then
    say "test_pg: SKIP: $DBT_BINARY not built"
    say "        (build data-base-translator first)"
    exit 77
fi

# The disposable cluster needs the PostgreSQL client tools.  Debian
# keeps them in a versioned directory outside PATH.
if ! command -v initdb >/dev/null 2>&1; then
    for d in /usr/lib/postgresql/*/bin /usr/local/pgsql/bin; do
        if [ -x "$d/initdb" ]; then
            PATH="$d:$PATH"
            export PATH
            break
        fi
    done
fi
for tool in initdb pg_ctl createdb psql; do
    command -v "$tool" >/dev/null 2>&1 || {
        say "test_pg: SKIP: $tool not available"
        exit 77
    }
done

WORK=$(mktemp -d)
CLUSTER="$WORK/cluster"
PORT=5498
CONNINFO="host=127.0.0.1 port=$PORT dbname=gnuai user=gnuai"

mkdir -p "$WORK/sock" "$WORK/inst"
initdb -D "$CLUSTER" -U gnuai --auth=trust >/dev/null 2>&1 \
    || { say "test_pg: FAIL: initdb"; exit 1; }
pg_ctl -D "$CLUSTER" -l "$WORK/postgres.log" \
    -o "-p $PORT -c listen_addresses=127.0.0.1 -c fsync=off -c unix_socket_directories=$WORK/sock" \
    -w start >/dev/null 2>&1 \
    || { say "test_pg: FAIL: starting the cluster"; exit 1; }
createdb -h 127.0.0.1 -p $PORT -U gnuai gnuai >/dev/null 2>&1 \
    || { fail "createdb"; }

# Every simulated instance answers with a deterministic output.
seed_out()
{
    printf 'Output:\n[0]: 0.9\n[1]: 0.8\n' > "$WORK/inst/llm$1.out"
    : > "$WORK/inst/llm$1.in"
}
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
    seed_out $i
done

STORAGE="dbexec:$DBT_BINARY"

# --- 1. One run with 2 instances ------------------------------------
IN='{"instances": 2, "topologies": ["2,4,1", "2,8,1"], "input": [0.5, 0.3], "aggregate": "majority"}
{"command": "status"}'
OUT=$(printf '%s\n' "$IN" | "$BINARY" --instances "dir:$WORK/inst" \
    --storage "$STORAGE" --conninfo "$CONNINFO" 2>/dev/null)
[ "$?" = 0 ] || fail "the 2-instance run must exit 0"
L1=$(printf '%s\n' "$OUT" | sed -n '1p')
printf '%s' "$L1" | grep -q '"run_id": 1' \
    || fail "the run must be attributed run_id 1 by the base"
printf '%s' "$L1" | grep -q '"output": "\[1,1\]"' \
    || fail "the aggregate of the two instances is [1,1]"

# --- 2. One run with 8 instances ------------------------------------
OUT=$(printf '%s\n' \
    '{"instances": 8, "topologies": ["2,4,1"], "input": [0.5, 0.3], "aggregate": "majority"}' \
    | "$BINARY" --instances "dir:$WORK/inst" \
    --storage "$STORAGE" --conninfo "$CONNINFO" 2>/dev/null)
printf '%s' "$OUT" | grep -q '"run_id": 2' \
    || fail "the 8-instance run must be attributed run_id 2"
printf '%s' "$OUT" | grep -q '"confidence": 1.000' \
    || fail "eight identical answers agree fully"

# --- 3. The trace is in PostgreSQL, read back from SQL -------------
Q="psql -h 127.0.0.1 -p $PORT -U gnuai -d gnuai -tAc"
N=$($Q "select count(*) from runs" 2>/dev/null)
[ "$N" = "2" ] || fail "runs must hold two rows (got '$N')"
N=$($Q "select count(*) from run_instances" 2>/dev/null)
[ "$N" = "10" ] || fail "run_instances must hold 2+8 rows (got '$N')"
N=$($Q "select count(*) from run_instances where status = 'ok'" 2>/dev/null)
[ "$N" = "10" ] || fail "every instance must be persisted ok"
N=$($Q "select count(*) from run_instances where run_id = 2" 2>/dev/null)
[ "$N" = "8" ] || fail "the eight instances must point at run 2"
V=$($Q "select aggregate_strategy from runs where id = 1" 2>/dev/null)
[ "$V" = "majority" ] || fail "the strategy of run 1 must be persisted"
V=$($Q "select descriptor->>'instances' from runs where id = 1" 2>/dev/null)
[ "$V" = "2" ] || fail "the descriptor must be persisted verbatim (jsonb)"
V=$($Q "select input::text from run_instances where run_id = 1 order by id limit 1" 2>/dev/null)
[ "$V" = "[0.5, 0.3]" ] || fail "the input vector must be persisted as jsonb"
V=$($Q "select output::text from run_instances where run_id = 2 order by id limit 1" 2>/dev/null)
[ "$V" = "[0.9, 0.8]" ] || fail "the output vector must be persisted as jsonb"
V=$($Q "select topology from run_instances where run_id = 1 order by id" 2>/dev/null)
EXPECT=$(printf '2,4,1\n2,8,1')
[ "$V" = "$EXPECT" ] \
    || fail "the two topologies of run 1 must be persisted in order"

if [ "$FAIL" = 0 ]; then
    say "test_pg: OK"
    exit 0
fi
exit 1
