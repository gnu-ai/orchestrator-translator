/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * scheduler.c — Starting and collecting the instances of one run.
 *
 * For each instance: mount (node mode) or open (dir mode), write
 * the topology line, write the input line, read the state text,
 * pick the output vector out of it, persist the row — then move
 * to the next instance.  A failed instance is a status ("failed"),
 * never a crash of the run: the run survives with fewer answers,
 * which is exactly what the aggregator is for (PLAN.md section
 * 5, phase 2 generalizes this to mid-run losses).
 */

#include "scheduler.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "neuron.h"

int
orc_scheduler_run (struct orc_instio *io, struct orc_storage *st,
                   const struct orc_descriptor *desc,
                   long long run_id, struct orc_sched_result *res)
{
  char input_line[ORC_MAX_INPUT * 16 + 2];
  char text[ORC_INST_TEXT_MAX];
  double out[ORC_MAX_OUTPUT];

  memset (res, 0, sizeof *res);
  res->n = desc->instances;

  if (orc_neuron_input_line (desc->input, desc->n_input, input_line,
                             sizeof input_line) == 0)
    {
      errno = EINVAL;                       /* unreachable: bounded */
      return -1;
    }

  for (int i = 0; i < desc->instances; i++)
    {
      struct orc_inst inst;
      struct orc_inst_out *o = &res->inst[i];
      const char *topology =
        desc->topologies[i % desc->n_topologies];
      long len;
      int n_out = -1;
      bool ok = false;

      strcpy (o->topology, topology);

      if (orc_inst_begin (io, i + 1, &inst) < 0)
        goto persist;                       /* transport: EIO below */
      if (orc_inst_write (&inst, topology) < 0)
        {
          orc_inst_end (&inst);
          goto persist;
        }
      if (orc_inst_write (&inst, input_line) < 0)
        {
          orc_inst_end (&inst);
          goto persist;
        }
      len = orc_inst_read (&inst, text, sizeof text);
      orc_inst_end (&inst);
      if (len < 0)
        goto persist;
      text[len < (long) sizeof text ? len : (long) sizeof text - 1]
        = '\0';
      n_out = orc_neuron_parse_outputs (text, (size_t) len, out,
                                        ORC_MAX_OUTPUT);
      if (n_out > 0)
        {
          ok = true;
          o->n_out = n_out;
          memcpy (o->out, out, (size_t) n_out * sizeof *out);
          res->n_ok++;
        }

    persist:
      o->ok = ok;
      {
        struct orc_inst_row row;
        char input_json[1024];
        char output_json[ORC_MAX_OUTPUT * 16 + 4];
        long long id;

        row.run_id = run_id;
        row.topology = o->topology;
        orc_storage_json_vector (desc->input, desc->n_input, input_json,
                                 sizeof input_json);
        row.input = input_json;
        if (ok)
          {
            orc_storage_json_vector (o->out, o->n_out, output_json,
                                     sizeof output_json);
            row.output = output_json;
          }
        else
          row.output = NULL;
        row.status = ok ? "ok" : "failed";

        /* Durability from the first execution: the row is written
         * as soon as the instance is collected.  A transport
         * failure of /db fails the run as EIO — the caller keeps
         * what was already persisted. */
        if (orc_storage_write_instance (st, &row, &id) == ORC_ST_TRANSPORT)
          {
            errno = EIO;
            return -1;
          }
        /* An INVALID/EMPTY answer of the base is a status: the
         * instance row is simply not there, the run continues
         * (the incident would be persisted by the supervisor of
         * phase 2). */
      }
    }

  return 0;
}
