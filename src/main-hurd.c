/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * main-hurd.c — Entry point of the /orchestrate translator on
 * GNU/Hurd.
 *
 * The canonical trivfs translator structure, like neuron-translator
 * and data-base-translator: get the bootstrap port, reply with
 * trivfs_startup, then serve RPCs forever through the demuxer
 * (the hooks live in trivfs-hooks.c).  The command engine (the
 * scheduler, the aggregator, the storage layer) is initialized
 * before settrans gets its reply, so a mounted orchestrator is a
 * working orchestrator.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

/* GNU Mach 1.8+git20260224 (the Debian forky/sid snapshot): the
 * installed mach_host.h uses processor_name_array_t, but no
 * installed header defines it — the MIG header generation of
 * this snapshot drops the typedef.  Provide the canonical
 * definition BEFORE the Hurd headers so they compile; C tolerates
 * the identical redefinition the day the snapshot is fixed. */
#include <mach/processor_info.h>
typedef processor_info_t *processor_name_array_t;

#include <hurd.h>
#include <hurd/ports.h>
#include <hurd/trivfs.h>
#include <hurd/fsys.h>

#include <argp.h>
#include <error.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orchestrate.h"

/* trivfs requirement: the control structure filled by
 * trivfs_startup(). */
struct trivfs_control *fsys;

const char *argp_program_version = "orchestrator-translator (GNU AI) "
                                    VERSION;
const char *argp_program_bug_address = "<claire@gnu-ai.org>";
static char doc[] = "Coordination layer of the GNU AI stack"
                    " for GNU/Hurd.";

static void
print_version_hook (FILE *stream, struct argp_state *state)
{
  (void) state;

  fprintf (stream, "%s\n", argp_program_version);
  fprintf (stream, "License GPLv3+: GNU GPL version 3 or later"
           " <https://gnu.org/licenses/gpl.html>.\n");
  fprintf (stream, "This is free software: you are free to change"
           " and redistribute it.\n");
  fprintf (stream, "There is NO WARRANTY, to the extent permitted"
           " by law.\n");
}

void (*argp_program_version_hook) (FILE *, struct argp_state *)
    = print_version_hook;

/* The frozen mount conventions (PLAN.md section 3.3): instances
 * on /llm<N>, persistence through /db. */
static struct argp_option options[] = {
  {"instances", 'i', "SPEC", 0,
   "instance transport: node:/llm (default) or dir:<path>", 0},
  {"neuron", 'n', "PATH", 0,
   "neuron translator binary settrans starts (default"
   " /hurd/sigmoid-neuron-translator)", 0},
  {"no-mount", 'M', 0, 0, "reuse already mounted instances", 0},
  {"storage", 's', "SPEC", 0,
   "storage backend: db:/db (default), dbexec:<bin> or mem"
   " (tests only)", 0},
  {"help", 'h', 0, 0, "display this help and exit", 0},
  { 0 }
};

static struct orc_config cfg = { 0 };
static bool cfg_no_mount = false;

static error_t
parse_opt (int key, char *arg, struct argp_state *state)
{
  switch (key)
    {
    case 'i':
      cfg.instio = arg;
      break;
    case 'n':
      cfg.neuron_bin = arg;
      break;
    case 'M':
      cfg_no_mount = true;
      break;
    case 's':
      cfg.storage = arg;
      break;
    case 'h':
      argp_state_help (state, stdout, ARGP_HELP_STD_HELP);
      exit (EXIT_SUCCESS);
    case ARGP_KEY_SUCCESS:
      cfg.mount = !cfg_no_mount;
      break;
    default:
      return ARGP_ERR_UNKNOWN;
    }
  return 0;
}

static struct argp argp = { options, parse_opt, 0, doc, 0, 0, 0 };

int
main (int argc, char *argv[])
{
  error_t err;
  mach_port_t bootstrap;

  argp_parse (&argp, argc, argv, 0, 0, 0);

  /* Init the engine BEFORE replying to settrans: when the mount
   * returns, the transports and the storage are ready. */
  if (orc_engine_init (&cfg) < 0)
    error (1, errno, "initializing the orchestration engine");

  task_get_bootstrap_port (mach_task_self (), &bootstrap);
  if (bootstrap == MACH_PORT_NULL)
    error (1, 0, "Must be started as a translator");

  err = trivfs_startup (bootstrap, 0, 0, 0, 0, 0, &fsys);
  mach_port_deallocate (mach_task_self (), bootstrap);
  if (err)
    error (3, err, "Contacting parent failed");

  ports_manage_port_operations_one_thread (fsys->pi.bucket,
                                           trivfs_demuxer, 0);

  orc_engine_shutdown ();
  return 0;
}
