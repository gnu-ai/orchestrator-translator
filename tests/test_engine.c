/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * test_engine.c — End-to-end unit test of the engine: the real
 * scheduler and aggregator, the dir: instance transport (a file
 * that behaves like /llm<N> is enough, PLAN.md section 7) and
 * the mem storage backend (tests only).  The assertions read the
 * in-memory twin of /db: same rows, same ids, as the production
 * backend writes them.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "contract.h"
#include "orchestrate.h"
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
          printf ("test_engine: FAIL: %s (%s:%d)\n", #cond,            \
                  __FILE__, __LINE__);                                  \
        }                                                               \
    }                                                                   \
  while (0)

#define CHECK_STR(a, b)                                                 \
  do                                                                    \
    {                                                                   \
      checks_run++;                                                     \
      if ((a) == NULL || strstr ((a), (b)) == NULL)                     \
        {                                                               \
          checks_failed++;                                              \
          printf ("test_engine: FAIL: %s contains \"%s\""               \
                  " (got \"%s\", %s:%d)\n",                             \
                  #a, (b), (a) ? (a) : "(null)",                        \
                  __FILE__, __LINE__);                                  \
        }                                                               \
    }                                                                   \
  while (0)

/* The instance simulator: three instances that answer, one that
 * stays silent (a "failed" instance, exactly what the phase 2
 * supervisor will detect in production). */
static void
seed_instance (const char *dir, int id, const char *outputs)
{
  char path[512];
  FILE *f;

  snprintf (path, sizeof path, "%s/llm%d.out", dir, id);
  f = fopen (path, "w");
  if (f == NULL)
    exit (2);
  fprintf (f, "Output:\n%s\n", outputs);
  fclose (f);
}

static void
process (const char *line)
{
  if (orc_engine_process_line (line, strlen (line)) < 0)
    {
      printf ("test_engine: FAIL: transport failure on %s\n", line);
      checks_failed++;
    }
}

int
main (void)
{
  struct orc_config cfg = { 0 };
  char command[1024];
  char dirtemplate[] = "inst-sim-XXXXXX";
  char *dir;

  /* The test owns its scratch directory of simulated instances,
   * created and removed here — nothing leaks into the tree. */
  dir = mkdtemp (dirtemplate);
  if (dir == NULL)
    {
      printf ("test_engine: FAIL: mkdtemp\n");
      return 2;
    }

  /* The test instances: two agree, one disagrees, one silent. */
  seed_instance (dir, 1, "[0]: 0.9\n[1]: 0.8");
  seed_instance (dir, 2, "[0]: 0.8\n[1]: 0.9");
  seed_instance (dir, 3, "[0]: 0.2\n[1]: 0.1");
  {
    char path[512];
    FILE *f;

    snprintf (path, sizeof path, "%s/llm4.out", dir);
    f = fopen (path, "w");                /* no output line at all */
    if (f == NULL)
      exit (2);
    fprintf (f, "Output:\n(nothing)\n");
    fclose (f);
  }

  cfg.storage = "mem";
  cfg.mount = true;
  /* The dir: transport of the test instances, from the argument. */
  {
    static char spec[512];

    snprintf (spec, sizeof spec, "dir:%s", dir);
    cfg.instio = spec;
  }
  if (orc_engine_init (&cfg) < 0)
    {
      printf ("test_engine: FAIL: engine init\n");
      return 2;
    }

  /* 1. The idle status is the initial response. */
  CHECK_STR (orc_engine_response (), "\"state\": \"idle\"");
  CHECK_STR (orc_engine_response (), "\"instances\": []");

  /* 2. A full run: 4 instances, topologies cycled, majority. */
  snprintf (command, sizeof command,
            "{\"instances\": 4, \"topologies\": [\"2,4,1\", \"2,8,1\"],"
            " \"input\": [0.5, 0.3], \"aggregate\": \"majority\"}");
  process (command);
  CHECK_STR (orc_engine_response (), "\"run_id\": 1");
  CHECK_STR (orc_engine_response (), "\"aggregate_strategy\": \"majority\"");
  CHECK_STR (orc_engine_response (), "\"output\": \"[1,1]\"");
  /* Two of the three answering instances fully agree with the
   * aggregate, one does not: 2/3. */
  CHECK_STR (orc_engine_response (), "\"confidence\": 0.667");
  CHECK_STR (orc_engine_response (), "\"state\": \"done\"");

  /* 3. The status of the run (trio type 2). */
  process ("{\"command\": \"status\"}");
  CHECK_STR (orc_engine_response (), "\"run_id\": 1");
  CHECK_STR (orc_engine_response (), "\"state\": \"done\"");
  CHECK_STR (orc_engine_response (), "\"id\": 1");
  CHECK_STR (orc_engine_response (), "\"state\": \"failed\"");
  CHECK_STR (orc_engine_response (), "\"topology\": \"2,8,1\"");

  /* 4. The mem twin of /db holds the rows, as collected —
   * durability from the first execution. */
  {
    const struct orc_mem_state *mem =
      orc_storage_mem_state (orc_engine_storage ());

    CHECK (mem != NULL);
    CHECK (mem->n_runs == 1);
    CHECK (mem->n_instances == 4);
    CHECK_STR (mem->runs[0].aggregate_strategy, "majority");
    CHECK_STR (mem->runs[0].descriptor, "\"instances\": 4");
    CHECK_STR (mem->instances[0].input, "[0.5,0.3]");
    CHECK_STR (mem->instances[0].output, "[0.9,0.8]");
    CHECK_STR (mem->instances[0].status, "ok");
    CHECK_STR (mem->instances[3].status, "failed");
    /* The instance that stayed silent persisted no output. */
    CHECK (strcmp (mem->instances[3].output, "null") == 0);
  }

  /* 5. Invalid commands are answers, never crashes. */
  process ("{\"instances\": 0}");
  CHECK_STR (orc_engine_response (), "\"invalid\": \"instances\"");
  process ("{\"select\": 1}");
  /* Like /db: the offending key IS the detail of the answer. */
  CHECK_STR (orc_engine_response (), "\"invalid\": \"select\"");

  /* 6. The result is still servable. */
  process ("{\"command\": \"result\"}");
  CHECK_STR (orc_engine_response (), "\"output\": \"[1,1]\"");

  orc_engine_shutdown ();
  /* Leave the tree exactly as we found it (distcheck checks). */
  for (int i = 1; i <= 4; i++)
    {
      char p[512];

      snprintf (p, sizeof p, "%s/llm%d.in", dir, i);
      unlink (p);
      snprintf (p, sizeof p, "%s/llm%d.out", dir, i);
      unlink (p);
    }
  rmdir (dir);

  printf ("test_engine: %d checks, %d failures\n",
          checks_run, checks_failed);
  return checks_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
