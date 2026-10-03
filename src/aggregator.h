/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * aggregator.h — Merging the collected outputs into the single
 * official result (PLAN.md section 1).
 */

#ifndef ORC_AGGREGATOR_H
#define ORC_AGGREGATOR_H

#include "scheduler.h"

struct orc_aggregate
  {
    double out[ORC_MAX_OUTPUT];
    int n;
    double confidence;               /* full-agreement share */
  };

/* Aggregate the OK instances of RES with the frozen strategy
 * ("majority" | "mean", SPEC.md section 8).  Returns 0, or -1
 * when there is nothing to aggregate (no instance succeeded). */
int orc_aggregate (const char *strategy,
                   const struct orc_sched_result *res,
                   struct orc_aggregate *agg);

#endif /* ORC_AGGREGATOR_H */
