/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * scheduler.h — Starting and collecting the instances of one run.
 *
 * The scheduler knows only the contracts: the instance transport
 * (instio.h) and the storage layer (storage.h).  Phase 1 runs
 * the instances sequentially — parallelism belongs to the
 * supervisor of phase 2 (PLAN.md section 5).
 */

#ifndef ORC_SCHEDULER_H
#define ORC_SCHEDULER_H

#include "contract.h"
#include "instio.h"
#include "storage.h"

/* Per-instance outcome of a run. */
struct orc_inst_out
  {
    char topology[ORC_MAX_TOPOLOGY + 1];
    bool ok;                         /* an output vector was collected */
    double out[ORC_MAX_OUTPUT];
    int n_out;
  };

struct orc_sched_result
  {
    int n;                           /* instances attempted */
    int n_ok;                        /* instances with an output */
    struct orc_inst_out inst[ORC_MAX_INSTANCES];
  };

/* Execute one descriptor over its N instances, persisting each
 * instance row as soon as it is collected (durability from the
 * first execution).  RUN_ID must already be attributed by the
 * storage layer (the engine writes the run row first).
 *
 * Returns 0, or -1 with errno == EIO when the transport layer
 * failed (an instance node unreachable, or /db unreachable) —
 * only the transport fails as a POSIX error (SPEC.md section 7). */
int orc_scheduler_run (struct orc_instio *io, struct orc_storage *st,
                        const struct orc_descriptor *desc,
                        long long run_id,
                        struct orc_sched_result *res);

#endif /* ORC_SCHEDULER_H */
