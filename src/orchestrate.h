/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * orchestrate.h — The command engine of /orchestrate.
 *
 * The whole translator behind its two faces (trivfs server on
 * GNU/Hurd, verification REPL elsewhere): one command line in,
 * one response line served by reads — the same slot mechanism
 * as /db, one cursor per reader.  The engine holds the state of
 * the last run; the status and the result are the trio JSON
 * types 2 and 3, frozen with inference-translator.
 */

#ifndef ORC_ORCHESTRATE_H
#define ORC_ORCHESTRATE_H

#include <stddef.h>

#include "contract.h"
#include "instio.h"
#include "storage.h"

/* Engine configuration, resolved once at startup (mount or REPL):
 * the instance transport spec ("node:/llm", "dir:..."), the
 * neuron translator binary (node mode), whether to mount, and
 * the storage spec ("db:/db", "dbexec:...", "mem") with the
 * optional conninfo handed to the dbexec child. */
struct orc_config
  {
    const char *instio;
    const char *neuron_bin;
    bool mount;
    const char *storage;
    const char *conninfo;
  };

/* Init from a configuration; the initial response slot is the
 * idle status.  Returns 0 or -1 with errno. */
int orc_engine_init (const struct orc_config *cfg);
void orc_engine_shutdown (void);

/* Process one complete command line (without its newline).
 * Returns 0, or -1 with errno == EIO when the transport layer
 * failed (an instance or /db unreachable); the response slot is
 * left unchanged in that case. */
int orc_engine_process_line (const char *line, size_t len);

/* The response to the last completed command (or the idle
 * status): served by reads, one cursor per reader. */
const char *orc_engine_response (void);
size_t orc_engine_response_len (void);

/* Unit tests only: the storage handle of the engine, to assert
 * the rows the run persisted (the mem backend). */
struct orc_storage *orc_engine_storage (void);

#endif /* ORC_ORCHESTRATE_H */
