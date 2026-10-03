/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * instio.h — Instance transport of the scheduler.
 *
 * The scheduler knows instances only through this narrow door:
 * begin, write one command line, read the state text, end.  Two
 * transports implement it (SPEC.md sections 1, 2 and 4):
 *
 *   node:<base>   production on GNU/Hurd — the scheduler mounts
 *                 neuron-translator on <base><N> with settrans and
 *                 speaks POSIX to the node (default /llm);
 *   dir:<path>    the test transport — a directory where a file
 *                 that "behaves like /llm<N>" is enough (PLAN.md
 *                 section 7): commands append to llm<N>.in, the
 *                 state text is read from llm<N>.out.
 */

#ifndef ORC_INSTIO_H
#define ORC_INSTIO_H

#include <stdbool.h>
#include <stddef.h>

#define ORC_INST_TEXT_MAX (64u * 1024u)   /* one state text read */

struct orc_instio
  {
    int mode;                       /* 0 = node, 1 = dir */
    char base[256];                 /* "<base>" or "<path>/" */
    char neuron_bin[256];           /* node mode: translator path */
    bool mount;                     /* node mode: settrans at begin */
  };

struct orc_inst
  {
    struct orc_instio *io;
    int id;
    char path[512];                 /* node, or <dir>/llm<N> prefix */
  };

/* Open a transport from a spec: "node:/llm", "dir:/tmp/inst",
 * or NULL for the default "node:/llm".  NEURON_BIN and MOUNT
 * configure the node mode (the neuron translator binary settrans
 * starts, and whether to mount at all).  Returns 0 or -1 with
 * errno. */
int orc_instio_open (struct orc_instio *io, const char *spec,
                     const char *neuron_bin, bool mount);

/* Begin the conversation with instance ID (1..N).  In node mode
 * with mount enabled: create the mount point if missing and
 * attach the neuron translator with settrans.  Returns 0 or -1
 * with errno. */
int orc_inst_begin (struct orc_instio *io, int id, struct orc_inst *inst);

/* Write ONE full command line (newline added here): a topology
 * line or an input line.  One write RPC is one command on the
 * neuron side.  Returns 0 or -1 with errno. */
int orc_inst_write (struct orc_inst *inst, const char *line);

/* Read the full state text of the instance into BUF.  Returns
 * the length, or -1 with errno. */
long orc_inst_read (struct orc_inst *inst, char *buf, size_t cap);

/* End the conversation (phase 1 keeps instances mounted for
 * reuse; stopping them belongs to the supervisor, phase 2). */
void orc_inst_end (struct orc_inst *inst);

#endif /* ORC_INSTIO_H */
