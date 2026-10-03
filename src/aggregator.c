/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * aggregator.c — Majority vote and uniform mean (phase 1).
 *
 *   majority: per component, the bit (>= 0.5) carried by the most
 *             instances wins; a tie rounds to 1; the aggregated
 *             component is 1.0 or 0.0.
 *   mean:     the uniform arithmetic mean per component — the
 *             weighted mean is LEARNED, phase 5.
 *
 * confidence is the share of instances in FULL agreement with the
 * aggregate (SPEC.md section 7): same bits for majority, an
 * deviation under 0.25 on every component for the mean.  It is
 * an honest, deterministic, documented measure — not a learned
 * score (phase 5).
 */

#include "aggregator.h"

#include <string.h>

int
orc_aggregate (const char *strategy,
               const struct orc_sched_result *res,
               struct orc_aggregate *agg)
{
  int n_comp = 0;
  int agree = 0;

  memset (agg, 0, sizeof *agg);
  if (res->n_ok == 0)
    return -1;

  /* The widest output gives the component count; a shorter
   * instance contributes 0.0 for the missing components
   * (SPEC.md section 8). */
  for (int i = 0; i < res->n; i++)
    if (res->inst[i].ok && res->inst[i].n_out > n_comp)
      n_comp = res->inst[i].n_out;
  if (n_comp == 0)
    return -1;
  agg->n = n_comp;

  if (strcmp (strategy, "majority") == 0)
    {
      for (int j = 0; j < n_comp; j++)
        {
          int ones = 0;

          for (int i = 0; i < res->n; i++)
            if (res->inst[i].ok
                && res->inst[i].out[j] >= 0.5)
              ones++;
          /* Tie rounds to 1: the deterministic MVP choice. */
          agg->out[j] = (2 * ones >= res->n_ok) ? 1.0 : 0.0;
        }
      /* Full agreement: same bits on every component. */
      for (int i = 0; i < res->n; i++)
        {
          if (!res->inst[i].ok)
            continue;
          bool same = true;

          for (int j = 0; j < n_comp; j++)
            if ((res->inst[i].out[j] >= 0.5 ? 1.0 : 0.0)
                != agg->out[j])
              {
                same = false;
                break;
              }
          if (same)
            agree++;
        }
    }
  else                                        /* "mean" */
    {
      for (int j = 0; j < n_comp; j++)
        {
          double sum = 0.0;

          for (int i = 0; i < res->n; i++)
            if (res->inst[i].ok)
              sum += j < res->inst[i].n_out ? res->inst[i].out[j]
                                            : 0.0;
          agg->out[j] = sum / (double) res->n_ok;
        }
      /* Full agreement: within 0.25 on every component. */
      for (int i = 0; i < res->n; i++)
        {
          if (!res->inst[i].ok)
            continue;
          bool same = true;

          for (int j = 0; j < n_comp; j++)
            {
              double v = j < res->inst[i].n_out
                           ? res->inst[i].out[j] : 0.0;

              if (v - agg->out[j] > 0.25 || agg->out[j] - v > 0.25)
                {
                  same = false;
                  break;
                }
            }
          if (same)
            agree++;
        }
    }

  agg->confidence = (double) agree / (double) res->n_ok;
  return 0;
}
