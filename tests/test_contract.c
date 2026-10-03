/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * test_contract.c — Unit tests of the frozen contract of
 * /orchestrate: descriptor parsing, command parsing, the
 * orchestrator-to-neuron wire formats, and the aggregation.
 *
 * Home made harness (CHECK macros and counters, C23/POSIX
 * only), the convention of the stack.  The tests derive from the
 * CONTRACT (SPEC.md), never from the implementation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aggregator.h"
#include "contract.h"
#include "neuron.h"
#include "storage.h"

static int checks_run = 0;
static int checks_failed = 0;

#define CHECK(cond)                                                     \
  do                                                                    \
    {                                                                   \
      checks_run++;                                                     \
      if (!(cond))                                                      \
        {                                                               \
          checks_failed++;                                              \
          printf ("test_contract: FAIL: %s (%s:%d)\n", #cond,          \
                  __FILE__, __LINE__);                                  \
        }                                                               \
    }                                                                   \
  while (0)

#define CHECK_STR(a, b)                                                 \
  do                                                                    \
    {                                                                   \
      checks_run++;                                                     \
      if ((a) == NULL || strcmp ((a), (b)) != 0)                        \
        {                                                               \
          checks_failed++;                                              \
          printf ("test_contract: FAIL: %s == \"%s\""                   \
                  " (got \"%s\", %s:%d)\n",                             \
                  #a, (b), (a) ? (a) : "(null)",                        \
                  __FILE__, __LINE__);                                  \
        }                                                               \
    }                                                                   \
  while (0)

static char scratch[ORC_SCRATCH];
static struct orc_request req;

static void
parse (const char *line)
{
  orc_parse (line, strlen (line), scratch, sizeof scratch, &req);
}

/* ------------------------------------------------------------------- */

static void
test_descriptor_valid (void)
{
  parse ("{\"instances\": 3,"
         " \"topologies\": [\"10,20,5\", \"10,30,5\", \"10,20,10\"],"
         " \"input\": [0.5, 0.3, 0.9, 0.1],"
         " \"aggregate\": \"majority\"}");
  CHECK (req.command == ORC_CMD_RUN);
  CHECK (req.desc.instances == 3);
  CHECK (req.desc.n_topologies == 3);
  CHECK_STR (req.desc.topologies[0], "10,20,5");
  CHECK_STR (req.desc.topologies[2], "10,20,10");
  CHECK (req.desc.n_input == 4);
  CHECK (req.desc.input[0] == 0.5 && req.desc.input[3] == 0.1);
  CHECK_STR (req.desc.aggregate, "majority");
  /* The descriptor is kept verbatim for runs.descriptor. */
  CHECK (req.desc.raw[0] == '{');
  CHECK (strstr (req.desc.raw, "\"aggregate\"") != NULL);

  /* The same descriptor under "run". */
  parse ("{\"run\": {\"instances\": 2, \"topologies\": [\"2,4,1\"],"
         " \"input\": [0.5, 0.3], \"aggregate\": \"mean\"}}");
  CHECK (req.command == ORC_CMD_RUN);
  CHECK (req.desc.instances == 2);
  CHECK_STR (req.desc.aggregate, "mean");
  CHECK (req.desc.raw[0] == '{');
  CHECK (strstr (req.desc.raw, "\"instances\"") != NULL);
}

static void
test_commands (void)
{
  parse ("{\"command\": \"status\"}");
  CHECK (req.command == ORC_CMD_STATUS);

  parse ("{ \"command\" : \"result\" }");
  CHECK (req.command == ORC_CMD_RESULT);

  parse ("{\"command\": \"replay\"}");
  CHECK (req.command == ORC_CMD_INVALID);
  CHECK (req.invalid == ORC_INVALID_VALUE);
  CHECK_STR (req.invalid_detail, "command");
}

static void
test_descriptor_invalid (void)
{
  parse ("");
  CHECK (req.command == ORC_CMD_INVALID);
  CHECK (req.invalid == ORC_INVALID_LINE);

  parse ("garbage");
  CHECK (req.invalid == ORC_INVALID_LINE);

  parse ("{}");
  CHECK (req.invalid == ORC_INVALID_FIELD);

  parse ("[]");
  CHECK (req.invalid == ORC_INVALID_LINE);

  parse ("{\"instances\": 3}");
  CHECK (req.invalid == ORC_INVALID_FIELD);
  CHECK_STR (req.invalid_detail, "topologies");

  parse ("{\"instances\": 0, \"topologies\": [\"2,4\"],"
         " \"input\": [1], \"aggregate\": \"mean\"}");
  CHECK (req.invalid == ORC_INVALID_VALUE);
  CHECK_STR (req.invalid_detail, "instances");

  parse ("{\"instances\": 3, \"topologies\": [\"2,4\"],"
         " \"input\": [], \"aggregate\": \"mean\"}");
  CHECK (req.invalid == ORC_INVALID_FIELD);
  CHECK_STR (req.invalid_detail, "input");

  parse ("{\"instances\": 3, \"topologies\": [],"
         " \"input\": [1], \"aggregate\": \"mean\"}");
  CHECK (req.invalid == ORC_INVALID_FIELD);

  parse ("{\"instances\": 3, \"topologies\": [\"2,4\"],"
         " \"input\": [1], \"aggregate\": \"borda\"}");
  CHECK (req.invalid == ORC_INVALID_VALUE);
  CHECK_STR (req.invalid_detail, "aggregate");

  parse ("{\"instances\": 3, \"topologies\": [\"2,4\"],"
         " \"input\": [1], \"aggregate\": \"mean\", \"oops\": 1}");
  CHECK (req.invalid == ORC_INVALID_KEY);
  CHECK_STR (req.invalid_detail, "oops");

  parse ("{\"instances\": \"three\", \"topologies\": [\"2,4\"],"
         " \"input\": [1], \"aggregate\": \"mean\"}");
  CHECK (req.invalid == ORC_INVALID_TYPE);
  CHECK_STR (req.invalid_detail, "instances");

  parse ("{\"instances\": 3, \"topologies\": [\"2,4\"],"
         " \"input\": [1], \"aggregate\": \"mean\"");
  CHECK (req.invalid == ORC_INVALID_JSON);

  parse ("{\"instances\": 3, \"topologies\": [\"2,4\"],"
         " \"input\": [1], \"aggregate\": \"mean\"} trailing");
  CHECK (req.invalid == ORC_INVALID_LINE);
}

/* ------------------------------------------------------------------- */

static void
test_neuron_formats (void)
{
  char buf[256];
  double v[3] = { 0.5, 0.25, 1.0 };
  double out[8];

  CHECK (orc_neuron_input_line (v, 3, buf, sizeof buf) > 0);
  CHECK_STR (buf, "0.5,0.25,1");

  /* The neuron state text: outputs as "[i]: value" lines. */
  CHECK (orc_neuron_parse_outputs (
           "LLM Sigmoid Neuron Translator\n"
           "Output:\n  [0]: 0.931223\n  [1]: 0.004321\n"
           "Statistics:\n", 90, out, 8) == 2);
  CHECK (out[0] > 0.931222 && out[0] < 0.931224);
  CHECK (out[1] > 0.004320 && out[1] < 0.004322);

  /* No output line: the instance gave nothing. */
  CHECK (orc_neuron_parse_outputs ("no output here", 14, out, 8) == -1);

  /* Sparse indexes: missing components stay 0.0. */
  CHECK (orc_neuron_parse_outputs ("[2]: 0.5\n", 9, out, 8) == 3);
  CHECK (out[2] == 0.5 && out[0] == 0.0 && out[1] == 0.0);
}

/* ------------------------------------------------------------------- */

static void
test_aggregation (void)
{
  struct orc_sched_result res;
  struct orc_aggregate agg;

  /* Majority: two instances say 1, one says 0 -> 1. */
  memset (&res, 0, sizeof res);
  res.n = 3;
  res.n_ok = 3;
  for (int i = 0; i < 3; i++)
    {
      res.inst[i].ok = true;
      res.inst[i].n_out = 2;
    }
  res.inst[0].out[0] = 0.9; res.inst[0].out[1] = 0.9;
  res.inst[1].out[0] = 0.8; res.inst[1].out[1] = 0.1;
  res.inst[2].out[0] = 0.2; res.inst[2].out[1] = 0.9;
  CHECK (orc_aggregate ("majority", &res, &agg) == 0);
  CHECK (agg.n == 2);
  CHECK (agg.out[0] == 1.0 && agg.out[1] == 1.0);
  /* Only instance 0 agrees on both components: 1/3. */
  CHECK (agg.confidence > 0.32 && agg.confidence < 0.34);

  /* Mean: the uniform average. */
  CHECK (orc_aggregate ("mean", &res, &agg) == 0);
  CHECK (agg.out[0] > 0.62 && agg.out[0] < 0.64);
  CHECK (agg.out[1] > 0.62 && agg.out[1] < 0.64);
  /* The mean is [0.633,0.633]: instance 0 deviates by 0.267
   * (> 0.25) on both components, so no instance is in full
   * agreement — the measure is honest, not generous. */
  CHECK (agg.confidence == 0.0);

  /* Nothing to aggregate. */
  memset (&res, 0, sizeof res);
  CHECK (orc_aggregate ("majority", &res, &agg) == -1);
}

/* ------------------------------------------------------------------- */

static void
test_json_vector (void)
{
  char buf[64];
  double v[3] = { 1.5, 0.25, -2 };

  CHECK (orc_storage_json_vector (v, 3, buf, sizeof buf) > 0);
  CHECK_STR (buf, "[1.5,0.25,-2]");
  CHECK (orc_storage_json_vector (v, 3, buf, 4) == 0);
}

int
main (void)
{
  test_descriptor_valid ();
  test_commands ();
  test_descriptor_invalid ();
  test_neuron_formats ();
  test_aggregation ();
  test_json_vector ();

  printf ("test_contract: %d checks, %d failures\n",
          checks_run, checks_failed);
  return checks_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
