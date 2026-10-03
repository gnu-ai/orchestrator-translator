/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * orchestrate.c — The command engine of /orchestrate.
 *
 * Commands (SPEC.md section 5): run a task descriptor (bare or
 * under "run"), ask for the status (trio type 2), ask for the
 * result (trio type 3).  A run is synchronous in phase 1: the
 * write returns when the run is done and the response slot
 * holds the result.
 *
 * The state is one static block, pre-allocated once: the write
 * path never allocates (the discipline of the stack).  The
 * single threaded servers (trivfs loop, REPL) serialize the
 * commands, so no lock is needed for the MVP.
 */

#include "orchestrate.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "aggregator.h"
#include "scheduler.h"

/* The engine state: configuration handles, the last run, and
 * the response slot served by reads. */
static struct
  {
    struct orc_instio io;
    struct orc_storage *storage;
    bool valid;

    /* Last run */
    bool has_run;
    long long run_id;
    enum { RUN_IDLE = 0, RUN_DONE, RUN_FAILED } run_state;
    int n_instances;
    struct
      {
        char topology[ORC_MAX_TOPOLOGY + 1];
        bool ok;
      } inst[ORC_MAX_INSTANCES];
    char strategy[16];
    struct orc_aggregate agg;       /* valid when run_state == RUN_DONE */
    int n_ok;

    /* Response slot */
    char slot[ORC_MAX_RESPONSE];
    size_t slot_len;
  } eng;

static char eng_scratch[ORC_SCRATCH];

/* ---------------------------------------------------------------------
 *  Response slot
 * ------------------------------------------------------------------- */

static void
slot_set (size_t len)
{
  eng.slot_len = len;
}

/* ---------------------------------------------------------------------
 *  Status and result (the trio JSON, SPEC.md section 7)
 * ------------------------------------------------------------------- */

static void
build_status (void)
{
  size_t n = 0;
  int w;

  if (!eng.has_run)
    {
      w = snprintf (eng.slot, sizeof eng.slot,
                    "{\"run_id\": 0, \"state\": \"idle\","
                    " \"instances\": []}");
      eng.slot_len = w > 0 && (size_t) w < sizeof eng.slot
                       ? (size_t) w : 0;
      return;
    }

  w = snprintf (eng.slot, sizeof eng.slot,
                "{\"run_id\": %lld, \"state\": \"%s\", \"instances\": [",
                eng.run_id,
                eng.run_state == RUN_DONE ? "done" : "failed");
  if (w < 0 || (size_t) w >= sizeof eng.slot)
    {
      eng.slot_len = 0;
      return;
    }
  n = (size_t) w;

  for (int i = 0; i < eng.n_instances; i++)
    {
      if (i > 0 && n + 1 < sizeof eng.slot)
        eng.slot[n++] = ',';
      w = snprintf (eng.slot + n, sizeof eng.slot - n,
                    "{\"id\": %d, \"topology\": \"%s\","
                    " \"state\": \"%s\"}",
                    i + 1, eng.inst[i].topology,
                    eng.inst[i].ok ? "done" : "failed");
      if (w < 0 || (size_t) w >= sizeof eng.slot - n)
        {
          eng.slot_len = 0;
          return;
        }
      n += (size_t) w;
    }
  w = snprintf (eng.slot + n, sizeof eng.slot - n, "]}");
  if (w < 0 || (size_t) w >= sizeof eng.slot - n)
    {
      eng.slot_len = 0;
      return;
    }
  eng.slot_len = n + (size_t) w;
}

static void
build_result (void)
{
  /* The aggregated vector, serialized as a string (the trio type
   * 3 keeps "output" a string; the richer synthesis belongs to
   * the interface). */
  char outbuf[ORC_MAX_OUTPUT * 16 + 4];
  int w;

  if (!eng.has_run || eng.run_state != RUN_DONE)
    {
      if (eng.has_run)
        {
          /* A failed run has no aggregate: an honest result. */
          w = snprintf (eng.slot, sizeof eng.slot,
                        "{\"run_id\": %lld, \"state\": \"failed\","
                        " \"aggregate_strategy\": \"%s\","
                        " \"output\": \"\", \"confidence\": 0}",
                        eng.run_id, eng.strategy);
        }
      else
        w = snprintf (eng.slot, sizeof eng.slot, "{\"empty\": true}");
      eng.slot_len = w > 0 && (size_t) w < sizeof eng.slot
                       ? (size_t) w : 0;
      return;
    }

  if (orc_storage_json_vector (eng.agg.out, eng.agg.n, outbuf,
                               sizeof outbuf) == 0)
    {
      eng.slot_len = 0;
      return;
    }
  w = snprintf (eng.slot, sizeof eng.slot,
                "{\"run_id\": %lld, \"state\": \"done\","
                " \"aggregate_strategy\": \"%s\","
                " \"output\": \"%s\", \"confidence\": %.3f}",
                eng.run_id, eng.strategy, outbuf, eng.agg.confidence);
  eng.slot_len = w > 0 && (size_t) w < sizeof eng.slot
                   ? (size_t) w : 0;
}

/* ---------------------------------------------------------------------
 *  One run
 * ------------------------------------------------------------------- */

/* Execute one descriptor: persist the run first (durability from
 * the first execution), run the instances, aggregate, serve the
 * result.  Returns 0, or -1 with errno == EIO (transport only). */
static int
do_run (const struct orc_descriptor *desc)
{
  struct orc_run_row run_row;
  long long run_id = 0;
  struct orc_sched_result sched;
  struct orc_aggregate agg;

  run_row.descriptor = desc->raw;
  run_row.aggregate_strategy = desc->aggregate;
  {
    enum orc_storage_status st =
      orc_storage_write_run (eng.storage, &run_row, &run_id);

    if (st == ORC_ST_TRANSPORT)
      {
        errno = EIO;                        /* transport only */
        return -1;
      }
    if (st != ORC_ST_OK)
      {
        /* The base refused the row (a constraint is a status):
         * the run is not started, the caller learns why in the
         * response line — no EIO, no partial state. */
        slot_set (orc_response_invalid (eng.slot, sizeof eng.slot,
                                        ORC_INVALID_VALUE, "run"));
        return 0;
      }
  }

  if (orc_scheduler_run (&eng.io, eng.storage, desc, run_id, &sched) < 0)
    {
      errno = EIO;
      return -1;
    }

  /* Record the state of the last run, then aggregate. */
  eng.has_run = true;
  eng.run_id = run_id;
  eng.n_instances = sched.n;
  eng.n_ok = sched.n_ok;
  strcpy (eng.strategy, desc->aggregate);
  for (int i = 0; i < sched.n && i < ORC_MAX_INSTANCES; i++)
    {
      strcpy (eng.inst[i].topology, sched.inst[i].topology);
      eng.inst[i].ok = sched.inst[i].ok;
    }

  if (orc_aggregate (desc->aggregate, &sched, &agg) < 0)
    eng.run_state = RUN_FAILED;             /* no instance answered */
  else
    {
      eng.run_state = RUN_DONE;
      eng.agg = agg;
    }

  build_result ();
  return 0;
}

/* ---------------------------------------------------------------------
 *  Init and command processing
 * ------------------------------------------------------------------- */

int
orc_engine_init (const struct orc_config *cfg)
{
  struct orc_config defaults = { 0 };

  if (cfg == NULL)
    cfg = &defaults;

  if (orc_instio_open (&eng.io, cfg->instio, cfg->neuron_bin,
                       cfg->mount) < 0)
    return -1;
  if (orc_storage_open (&eng.storage, cfg->storage, cfg->conninfo) < 0)
    return -1;
  eng.valid = true;
  eng.has_run = false;
  eng.run_state = RUN_IDLE;

  build_status ();                           /* the idle slot */
  return 0;
}

void
orc_engine_shutdown (void)
{
  if (!eng.valid)
    return;
  orc_storage_close (eng.storage);
  eng.storage = NULL;
  eng.valid = false;
}

int
orc_engine_process_line (const char *line, size_t len)
{
  struct orc_request req;

  while (len > 0 && line[len - 1] == '\r')
    len--;
  if (len >= ORC_MAX_LINE)
    {
      slot_set (orc_response_invalid (eng.slot, sizeof eng.slot,
                                       ORC_INVALID_TOOLONG, NULL));
      return 0;
    }

  orc_parse (line, len, eng_scratch, sizeof eng_scratch, &req);

  switch (req.command)
    {
    case ORC_CMD_INVALID:
      slot_set (orc_response_invalid (eng.slot, sizeof eng.slot,
                                       req.invalid,
                                       req.invalid_detail));
      return 0;

    case ORC_CMD_STATUS:
      build_status ();
      return 0;

    case ORC_CMD_RESULT:
      if (!eng.has_run)
        slot_set (orc_response_empty (eng.slot, sizeof eng.slot));
      else
        build_result ();
      return 0;

    case ORC_CMD_RUN:
      return do_run (&req.desc);

    default:
      errno = EINVAL;
      return -1;
    }
}

const char *
orc_engine_response (void)
{
  return eng.slot;
}

size_t
orc_engine_response_len (void)
{
  return eng.slot_len;
}

struct orc_storage *
orc_engine_storage (void)
{
  return eng.storage;
}
