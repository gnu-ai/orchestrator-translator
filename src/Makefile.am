# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

bin_PROGRAMS = orchestrator-translator

# --- Phase 0: the compilable skeleton --------------------------------
#
# The translator /orchestrate (scheduler, supervisor, evaluator,
# aggregator — PLAN.md section 2) arrives with phase 1: it starts N
# neuron-translator instances by settrans and archives everything
# in PostgreSQL through /db.  The skeleton answers the GNU base
# commands so that the binary is inspectable on any POSIX system,
# Hurd or not.

orchestrator_translator_SOURCES = main.c

AM_CPPFLAGS = -I$(top_srcdir)
AM_CFLAGS = $(C23_CFLAGS) -Wall -Wextra
