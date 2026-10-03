/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * storage.h — The unique storage layer of the orchestrator
 * (PLAN.md section 4): storage_write / storage_read.
 *
 * The default backend is data-base-translator — the orchestrator
 * knows /db and the JSON line contract, never a SQL client.  The
 * in-memory backend exists for the unit tests only, never in
 * production (PLAN.md section 4).  The expected constraints of
 * the base are statuses; only the transport layer fails.
 */

#ifndef ORC_STORAGE_H
#define ORC_STORAGE_H

#include <stdbool.h>
#include <stddef.h>

enum orc_storage_status
  {
    ORC_ST_OK = 0,
    ORC_ST_DUPLICATE,
    ORC_ST_INVALID,
    ORC_ST_EMPTY,
    ORC_ST_TRANSPORT,
  };

/* Rows of the frozen schema (data-base-translator PLAN section 6).
 * JSON fields are passed as JSON text: the orchestrator carries
 * them, the base owns them. */
struct orc_run_row
  {
    const char *descriptor;          /* JSON object text */
    const char *aggregate_strategy;
  };

struct orc_inst_row
  {
    long long run_id;
    const char *topology;
    const char *input;              /* JSON array text */
    const char *output;             /* JSON array text or NULL */
    const char *status;             /* "ok" | "failed" */
  };

struct orc_storage;

/* Open a backend from a spec (SPEC.md section 4):
 *   "db:/db"       default — the /db translator node;
 *   "dbexec:<bin>" integration — the db-translator verification
 *                  binary spawned as a child process, same line
 *                  protocol (CONNINFO, when not NULL, is passed to
 *                  it as --conninfo);
 *   "mem"          unit tests only.
 * Returns 0 or -1 with errno. */
int orc_storage_open (struct orc_storage **out, const char *spec,
                      const char *conninfo);
void orc_storage_close (struct orc_storage *st);

enum orc_storage_status orc_storage_write_run (struct orc_storage *st,
                                               const struct orc_run_row *row,
                                               long long *id);
enum orc_storage_status orc_storage_write_instance (
                                    struct orc_storage *st,
                                    const struct orc_inst_row *row,
                                    long long *id);

/* --- In-memory backend introspection (unit tests only) ------------- */
struct orc_mem_state
  {
    int n_runs;
    int n_instances;
    long long last_run_id;
    struct
      {
        char descriptor[512];
        char aggregate_strategy[16];
      } runs[128];
    struct
      {
        long long run_id;
        char topology[64];
        char input[512];
        char output[512];
        char status[16];
      } instances[512];
  };

const struct orc_mem_state *orc_storage_mem_state (struct orc_storage *st);

/* Render "[1.5,0.25]" JSON array text from a double vector —
 * the wire format of input/output in run_instances rows.
 * Returns the length, or 0 when it does not fit. */
size_t orc_storage_json_vector (const double *v, int n,
                               char *buf, size_t cap);

#endif /* ORC_STORAGE_H */
