/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * contract.h — The frozen task descriptor and command line of
 * /orchestrate (PLAN.md sections 3.3 and 5 phase 0).
 *
 * One command is ONE complete JSON line.  Everything the caller
 * needs to know is in the data: a violation of the contract is a
 * readable "invalid" response, never a crash, never a guess — the
 * same discipline as /db.
 *
 * Pure C23/POSIX, no allocation: the caller provides the buffers.
 */

#ifndef ORC_CONTRACT_H
#define ORC_CONTRACT_H

#include <stdbool.h>
#include <stddef.h>

/* --- Limits (phase 1) ------------------------------------------------
 * The command line stays small: network contents arrive with
 * phase 3 (via httpfs), never inline.  The input vector is
 * bounded by the command line of a neuron-translator instance
 * (SPEC.md section 2). */
#define ORC_MAX_LINE       (64u * 1024u)   /* bytes per command line */
#define ORC_MAX_INSTANCES  64
#define ORC_MAX_TOPOLOGIES 64
#define ORC_MAX_TOPOLOGY   63              /* bytes, NUL excluded */
#define ORC_MAX_INPUT      64              /* values per input vector */
#define ORC_MAX_OUTPUT     256             /* values per instance output */
#define ORC_SCRATCH        (ORC_MAX_LINE + 4096u)
#define ORC_MAX_RESPONSE   2048             /* status/result JSON bound */

/* --- Commands --------------------------------------------------------- */
enum orc_command
  {
    ORC_CMD_INVALID = 0,
    ORC_CMD_RUN,          /* {"run": {...}} or a bare descriptor */
    ORC_CMD_STATUS,       /* {"command": "status"} */
    ORC_CMD_RESULT,       /* {"command": "result"} */
  };

/* --- Readable reasons of the "invalid" response ----------------------- */
enum orc_invalid
  {
    ORC_INVALID_LINE = 0,   /* not one single complete JSON object */
    ORC_INVALID_JSON,       /* malformed JSON */
    ORC_INVALID_FIELD,      /* required field missing */
    ORC_INVALID_TYPE,       /* field with the wrong JSON type */
    ORC_INVALID_VALUE,      /* value out of the contract bounds */
    ORC_INVALID_KEY,        /* unknown key */
    ORC_INVALID_TOOLONG,    /* command line exceeds ORC_MAX_LINE */
  };

const char *orc_invalid_reason (enum orc_invalid reason);

/* --- The task descriptor (frozen, SPEC.md section 6) ------------------- */
struct orc_descriptor
  {
    int  instances;                        /* 1..ORC_MAX_INSTANCES */
    int  n_topologies;
    char topologies[ORC_MAX_TOPOLOGIES][ORC_MAX_TOPOLOGY + 1];
    int  n_input;
    double input[ORC_MAX_INPUT];
    char aggregate[16];                    /* "majority" | "mean" */
    char raw[ORC_MAX_LINE];                /* verbatim JSON, persisted */
  };

struct orc_request
  {
    enum orc_command command;
    struct orc_descriptor desc;
    /* When command is ORC_CMD_INVALID: */
    enum orc_invalid invalid;
    char invalid_detail[32];
  };

/* Parse one complete command line (without its newline).
 * Always fills REQ: on failure command is ORC_CMD_INVALID. */
void orc_parse (const char *line, size_t len,
                char *scratch, size_t scratch_cap,
                struct orc_request *req);

/* --- Response builders ------------------------------------------------
 * Write one JSON line (no trailing newline) into BUF, return its
 * length; 0 when it does not fit (never truncate a JSON line). */
size_t orc_response_invalid (char *buf, size_t cap,
                             enum orc_invalid reason,
                             const char *detail);
size_t orc_response_empty (char *buf, size_t cap);

#endif /* ORC_CONTRACT_H */
