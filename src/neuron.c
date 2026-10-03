/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * neuron.c — The frozen orchestrator-to-neuron wire formats.
 *
 * The orchestrator never interprets what a neuron computes: it
 * formats the contract's lines in, and picks the numbers out of
 * the state text out.  Everything here is deliberate, documented
 * and tested against the formats of neuron-translator (SPEC.md
 * section 2).
 */

#include "neuron.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t
orc_neuron_input_line (const double *v, int n, char *buf, size_t cap)
{
  size_t len = 0;

  for (int i = 0; i < n; i++)
    {
      int w = snprintf (buf + len, cap > len ? cap - len : 0,
                        i > 0 ? ",%.6g" : "%.6g", v[i]);

      if (w < 0 || (size_t) w >= (cap > len ? cap - len : 0))
        return 0;                           /* never truncate a line */
      len += (size_t) w;
    }
  return len;
}

/* One output line of the neuron state text looks like
 * "  [3]: 0.931223" — two spaces, the index in brackets, a colon
 * and the value.  The scan stays tolerant of the exact leading
 * whitespace but strict about the bracket shape. */
int
orc_neuron_parse_outputs (const char *text, size_t text_len,
                          double *out, int max)
{
  int highest = -1;
  size_t i = 0;

  memset (out, 0, (size_t) max * sizeof *out);

  while (i < text_len)
    {
      /* Find the next '[' that could start an output line. */
      const char *nl = memchr (text + i, '\n', text_len - i);
      size_t line_len = nl != NULL ? (size_t) (nl - (text + i))
                                   : text_len - i;
      const char *line = text + i;
      const char *lb = memchr (line, '[', line_len);

      if (lb != NULL)
        {
          char *end = NULL;
          long idx = strtol (lb + 1, &end, 10);

          if (end != lb + 1 && *end == ']'
              && idx >= 0 && idx < max)
            {
              const char *colon = strchr (end, ':');

              if (colon != NULL)
                {
                  out[idx] = strtod (colon + 1, NULL);
                  if (idx > highest)
                    highest = (int) idx;
                }
            }
        }
      if (nl == NULL)
        break;
      i = (size_t) (nl - text) + 1;
    }

  return highest >= 0 ? highest + 1 : -1;
}
