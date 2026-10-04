#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>
#
# CI guest script: build and test orchestrator-translator on real
# GNU/Hurd, driven by
# gnu-ai/mistral-vm-debian-hurd (hurd_vm.py).
#
#   python3 hurd_vm.py <image> ci/guest-hurd.sh
#
# Runs as root inside the guest; the exit code of this script
# becomes the exit code of the driver.  This is the phase 1
# acceptance scenario played on real Hurd:
#
#   1. build neuron-translator, data-base-translator and
#      orchestrator-translator (the trees are served by the CI
#      host over the QEMU gateway, 10.0.2.2:8000 - see
#      .github/workflows/hurd.yml);
#   2. run the whole orchestrator suite (its integration test
#      drives the real db-translator against a disposable
#      PostgreSQL cluster);
#   3. smoke test the REAL mounted stack: settrans /db and
#      settrans /orchestrate with the node instance transport,
#      the scheduler mounting real neuron instances on /llm1..N,
#      a descriptor run with tee, the aggregate read back with
#      cat, and the whole trace verified from SQL.

set -e

# A serial console reports zero rows: psql (aligned format) and
# friends would page even a one-line output and wedge the session
# at a "(END)" prompt.  No pagers in a CI guest.
export PAGER=cat PSQL_PAGER=cat

echo "=== guest: $(uname -a)"

echo "=== installing the build dependencies"
apt-get update -qq

# Debian ports (hurd-amd64): the ssl-cert postinst passes an
# empty GID to groupadd, which breaks the whole PostgreSQL
# dependency chain.  Pre-creating the group sidesteps it.  And
# update-alternatives stumbles on a stale alternatives link
# ("cannot stat ...: Invalid argument"), which breaks the
# automake postinst and takes aclocal with it: clean the link
# first.  A dpkg left unconfigured by an earlier attempt is
# repaired the same way.
if ! grep -q '^ssl-cert:' /etc/group; then
    GID=$(awk -F: 'BEGIN{m=100} {if($3>m && $3<65534)m=$3} END{print m+1}' /etc/group)
    echo "ssl-cert:x:$GID:" >> /etc/group
fi
dpkg --configure -a >/dev/null 2>&1 || true

DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    build-essential autoconf automake pkg-config \
    libpq-dev postgresql postgresql-client ca-certificates wget

# A stale alternatives link breaks the automake postinst and
# takes aclocal with it — but removing the links when automake
# is ALREADY configured breaks aclocal just as well.  Repair
# only when the toolchain is actually missing, then verify it.
if ! command -v aclocal >/dev/null 2>&1; then
    rm -f /etc/alternatives/automake /etc/alternatives/aclocal 2>/dev/null || true
    dpkg --configure -a >/dev/null 2>&1 || true
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
        --reinstall automake >/dev/null 2>&1 || true
fi
for tool in autoreconf aclocal automake autoconf make gcc pkg-config; do
    command -v "$tool" >/dev/null 2>&1 \
        || { echo "FAIL: $tool is missing from the toolchain"; exit 1; }
done
dpkg -s postgresql >/dev/null 2>&1 \
    || { echo "FAIL: the PostgreSQL server did not configure"; exit 1; }

echo "=== fetching the trees from the CI host (siblings, as the"
echo "    default test paths expect a developer tree)"
# The build and the test suite run as the unprivileged "builder"
# user: initdb and pg_ctl refuse to run as root, and a suite
# closer to a developer machine is a more honest CI.
useradd -m -s /bin/sh builder 2>/dev/null || true
rm -rf /home/builder/neuron-translator /home/builder/data-base-translator \
       /home/builder/orchestrator-translator /home/builder/*.tar.gz
cd /home/builder
for repo in neuron-translator data-base-translator orchestrator-translator; do
    wget -q -O "$repo.tar.gz" "http://10.0.2.2:8000/$repo.tar.gz" \
        || { echo "FAIL: the CI host does not serve $repo on :8000"; exit 1; }
    tar xzf "$repo.tar.gz"
done
chown -R builder:builder /home/builder/neuron-translator \
                              /home/builder/data-base-translator \
                              /home/builder/orchestrator-translator

echo "=== building the three trees, then make check, as builder"
su builder -c '
    cd data-base-translator \
        && ./autogen.sh && ./configure && make \
    && cd ../neuron-translator \
        && ./autogen.sh && ./configure && make \
    && cd ../orchestrator-translator \
        && ./autogen.sh && ./configure && make \
    && export DBT_BINARY=../../data-base-translator/src/db-translator-repl \
    && if ! make check; then
           echo "=== the failing test logs:";
           for l in tests/*.log; do
               if grep -q "FAIL" "$l" 2>/dev/null; then
                   echo "--- $l";
                   tail -30 "$l";
               fi;
           done;
           exit 1;
       fi
    echo "=== the skipped tests, with their reasons:";
    for l in tests/*.log; do
        if grep -q "SKIP" "$l" 2>/dev/null; then
            echo "--- $l";
            grep "SKIP" "$l" | head -2;
        fi;
    done;
    exit 0'

echo "=== smoke test: the whole stack, really mounted"
cd /home/builder/orchestrator-translator
# Debian runs PostgreSQL 18 as the "main" cluster.  Hurd has no
# reliable peer credentials on the local socket, so the cluster
# of this TEST machine answers "trust" on local connections, and
# root gets the role the translator will connect as.
pg_ctlcluster 18 main start || true
# Hurd has no reliable peer credentials on the local socket, and
# the config of a cluster left half-written by an unclean shutdown
# of an earlier CI run can even be dropped by the next fsck.  The
# cluster of this TEST machine therefore gets a minimal,
# unconditionally rewritten trust-on-localhost pg_hba.conf.
cat > /etc/postgresql/18/main/pg_hba.conf <<'HBAEOF'
# CI test cluster: every local connection is trusted.
local   all             all                                     trust
host    all             all             127.0.0.1/32            trust
host    all             all             ::1/128                 trust
HBAEOF
pg_ctlcluster 18 main restart || pg_ctlcluster 18 main start
psql -U postgres -tAc "create role root login superuser" 2>&1 || true
dropdb -U postgres --if-exists gnuai 2>&1 || true
createdb -U postgres -O root gnuai 2>&1 || true
echo "step: connecting as root (this is what /db does):"
psql -d gnuai -tAc "select current_user"

rm -rf /db /orchestrate
touch /db /orchestrate
echo "step: mount points ready"

settrans -a /db ../data-base-translator/src/db-translator \
    --conninfo "dbname=gnuai"
sleep 2

settrans -a /orchestrate ./src/orchestrator-translator \
    --storage db:/db \
    --instances node:/llm \
    --neuron /home/builder/neuron-translator/src/sigmoid-neuron-translator
sleep 2

echo "=== status of the idle orchestrator"
cat /orchestrate

echo "=== one descriptor, 2 instances, majority: plain POSIX in"
echo '{"instances": 2, "topologies": ["2,4,1", "2,8,1"], "input": [0.5, 0.3], "aggregate": "majority"}' | tee /orchestrate
echo "=== the aggregated result, plain POSIX out"
cat /orchestrate

echo "=== the status (trio type 2)"
echo '{"command": "status"}' | tee /orchestrate
cat /orchestrate

echo "=== the instance nodes answered (their state text)"
cat /llm1
cat /llm2

echo "=== the trace is in PostgreSQL"
N=$(psql -d gnuai -tAc "select count(*) from runs")
[ "$N" = "1" ] || { echo "FAIL: runs must hold one row, got '$N'"; exit 1; }
V=$(psql -d gnuai -tAc "select aggregate_strategy from runs where id = 1")
[ "$V" = "majority" ] || { echo "FAIL: wrong strategy '$V'"; exit 1; }
N=$(psql -d gnuai -tAc "select count(*) from run_instances where run_id = 1")
[ "$N" = "2" ] || { echo "FAIL: run 1 must hold two instances, got '$N'"; exit 1; }
N=$(psql -d gnuai -tAc "select count(*) from run_instances where status = 'ok'")
[ "$N" = "2" ] || { echo "FAIL: both instances must be ok, got '$N'"; exit 1; }
N=$(psql -d gnuai -tAc "select count(*) from run_instances where output is not null")
[ "$N" = "2" ] || { echo "FAIL: both instances must have persisted their output, got '$N'"; exit 1; }
V=$(psql -d gnuai -tAc "select output::text from run_instances where run_id = 1 order by id limit 1")
echo "first instance output: $V"
echo "runs=1, instances=2, both ok with outputs"

echo "=== unmounting"
settrans -g /orchestrate
settrans -g /db
sleep 1

echo "=== all good"
