# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

# The unit tests run the real engine on any POSIX system:
# test_contract (the frozen contract and the wire formats),
# test_engine (a full run over the dir: instance transport with
# the mem storage twin of /db).  test_cli.sh checks the GNU base
# commands, test_orchestrate.sh runs the protocol end to end over
# the verification binary, and test_pg.sh plays the phase 1
# acceptance scenario (2 and 8 instances) against a disposable
# PostgreSQL cluster through the real data-base-translator,
# skipping (77) when the tools are not available.

AM_CPPFLAGS = -I$(top_srcdir)/src
AM_CFLAGS = $(C23_CFLAGS) -Wall -Wextra

TESTS = test_contract test_engine test_cli.sh test_orchestrate.sh \
        test_pg.sh

check_PROGRAMS = test_contract test_engine

test_contract_SOURCES = test_contract.c
test_contract_LDADD = $(top_builddir)/src/liborchcore.a

test_engine_SOURCES = test_engine.c
test_engine_LDADD = $(top_builddir)/src/liborchcore.a

EXTRA_DIST = test_cli.sh test_orchestrate.sh test_pg.sh
