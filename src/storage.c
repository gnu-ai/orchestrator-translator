/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * storage.c — Backend dispatch and the in-memory backend.
 *
 * The in-memory backend is the test twin of /db: same statuses,
 * same sequential ids, same rows — so the unit tests of the
 * scheduler and the engine assert against exactly what the
 * production backend writes (PLAN.md section 4).
 */

#include "storage.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage_db.h"

struct orc_storage
  {
    int mode;                       /* 0 = mem, 1 = db */
    struct orc_mem_state mem;
    struct orc_db db;               /* db mode handle */
  };

int
orc_storage_open (struct orc_storage **out, const char *spec,
                  const char *conninfo)
{
  struct orc_storage *st = calloc (1, sizeof *st);

  if (st == NULL)
    return -1;
  st->mode = 0;

  if (spec != NULL && strcmp (spec, "mem") == 0)
    {
      /* in memory: nothing to prepare */
    }
  else
    {
      if (orc_db_open (&st->db, spec != NULL ? spec : "db:/db",
                       conninfo) < 0)
        {
          int e = errno;

          free (st);
          errno = e;
          return -1;
        }
      st->mode = 1;
    }

  *out = st;
  return 0;
}

void
orc_storage_close (struct orc_storage *st)
{
  if (st == NULL)
    return;
  if (st->mode == 1)
    orc_db_close (&st->db);
  free (st);
}

/* --- In-memory backend --------------------------------------------- */

static enum orc_storage_status
mem_write_run (struct orc_storage *st, const struct orc_run_row *row,
               long long *id)
{
  struct orc_mem_state *m = &st->mem;

  if (m->n_runs >= 128)
    return ORC_ST_INVALID;
  snprintf (m->runs[m->n_runs].descriptor,
            sizeof m->runs[0].descriptor, "%s", row->descriptor);
  snprintf (m->runs[m->n_runs].aggregate_strategy,
            sizeof m->runs[0].aggregate_strategy, "%s",
            row->aggregate_strategy);
  m->n_runs++;
  m->last_run_id++;
  *id = m->last_run_id;
  return ORC_ST_OK;
}

static enum orc_storage_status
mem_write_instance (struct orc_storage *st, const struct orc_inst_row *row,
                    long long *id)
{
  struct orc_mem_state *m = &st->mem;

  if (m->n_instances >= 512)
    return ORC_ST_INVALID;
  m->instances[m->n_instances].run_id = row->run_id;
  snprintf (m->instances[m->n_instances].topology,
            sizeof m->instances[0].topology, "%s", row->topology);
  snprintf (m->instances[m->n_instances].input,
            sizeof m->instances[0].input, "%s",
            row->input != NULL ? row->input : "null");
  snprintf (m->instances[m->n_instances].output,
            sizeof m->instances[0].output, "%s",
            row->output != NULL ? row->output : "null");
  snprintf (m->instances[m->n_instances].status,
            sizeof m->instances[0].status, "%s", row->status);
  m->n_instances++;
  m->instances[m->n_instances - 1].run_id = row->run_id;
  *id = m->n_instances;
  return ORC_ST_OK;
}

/* --- Dispatch -------------------------------------------------------- */

enum orc_storage_status
orc_storage_write_run (struct orc_storage *st,
                      const struct orc_run_row *row, long long *id)
{
  if (st->mode == 1)
    return orc_db_write_run (&st->db, row, id);
  return mem_write_run (st, row, id);
}

enum orc_storage_status
orc_storage_write_instance (struct orc_storage *st,
                            const struct orc_inst_row *row,
                            long long *id)
{
  if (st->mode == 1)
    return orc_db_write_instance (&st->db, row, id);
  return mem_write_instance (st, row, id);
}

const struct orc_mem_state *
orc_storage_mem_state (struct orc_storage *st)
{
  return st->mode == 0 ? &st->mem : NULL;
}

size_t
orc_storage_json_vector (const double *v, int n, char *buf, size_t cap)
{
  size_t len = 0;

  if (cap < 2)
    return 0;
  buf[len++] = '[';
  for (int i = 0; i < n; i++)
    {
      int w = snprintf (buf + len, cap - len, i > 0 ? ",%.6g" : "%.6g",
                        v[i]);

      if (w < 0 || (size_t) w >= cap - len)
        return 0;
      len += (size_t) w;
    }
  if (len + 2 > cap)
    return 0;
  buf[len++] = ']';
  buf[len] = '\0';
  return len;
}
