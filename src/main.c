/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * main.c — Entry point of the verification build (non-Hurd POSIX).
 *
 * USAGE
 * -----
 *      orchestrator-translator --version
 *      orchestrator-translator --help
 *      orchestrator-translator [OPTIONS] < commands.jsonl
 *
 * On GNU/Hurd the binary is the trivfs translator mounted by
 * settrans (see main-hurd.c); everywhere else the same engine
 * runs over standard streams: one command line in, one response
 * line out.  This is how the whole orchestration is tested on
 * any POSIX system — the instances and the storage come from the
 * test transports (dir:, mem/dbexec), the code is the same.
 *
 * OPTIONS
 * -------
 *      --instances SPEC    node:/llm (default) or dir:<path>
 *      --neuron PATH       neuron translator binary (node mode)
 *      --no-mount          reuse already mounted instances
 *      --storage SPEC      db:/db (default), dbexec:<bin>, mem
 *      --conninfo STRING   passed to the dbexec child
 *
 * Transport failures are reported on stderr with the EIO they
 * stand for, and make the exit status 1; the loop itself never
 * stops (the engine retries on the next command).
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orchestrate.h"
#include "storage.h"

static void
print_version (void)
{
    printf ("orchestrator-translator (GNU AI) %s\n", VERSION);
    printf ("License GPLv3+: GNU GPL version 3 or later"
            " <https://gnu.org/licenses/gpl.html>.\n");
    printf ("This is free software: you are free to change"
            " and redistribute it.\n");
    printf ("There is NO WARRANTY, to the extent permitted by law.\n");
}

static void
print_help (void)
{
    printf ("Usage: orchestrator-translator [OPTION]...\n");
    printf ("Coordination layer of the GNU AI stack.\n");
    printf ("\n");
    printf ("  -h, --help          display this help and exit\n");
    printf ("  -V, --version       output version information"
            " and exit\n");
    printf ("      --instances S   instance transport:"
            " node:/llm (default) | dir:<path>\n");
    printf ("      --neuron PATH   neuron translator binary"
            " (node mode)\n");
    printf ("      --no-mount      do not settrans;"
            " reuse mounted instances\n");
    printf ("      --storage S     db:/db (default) | dbexec:<bin>"
            " | mem\n");
    printf ("      --conninfo STR  libpq string handed to the"
            " dbexec child\n");
    printf ("\n");
    printf ("On GNU/Hurd, the binary is the /orchestrate"
            " translator:\n");
    printf ("  settrans -a /orchestrate orchestrator-translator\n");
    printf ("Elsewhere it reads command lines on stdin and answers\n");
    printf ("one response line per command on stdout.\n");
    printf ("\n");
    printf ("Report bugs at"
        " <https://github.com/gnu-ai/orchestrator-translator/issues>.\n");
}

/* Hand written option parsing, shared in spirit with the Hurd
 * entry point: no argp dependency, the same GNU answers. */
static bool
handle_options (int argc, char *argv[], struct orc_config *cfg)
{
    for (int i = 1; i < argc; i++)
        {
            if (strcmp (argv[i], "--version") == 0
                || strcmp (argv[i], "-V") == 0)
                {
                    print_version ();
                    return true;
                }
            if (strcmp (argv[i], "--help") == 0
                || strcmp (argv[i], "-h") == 0)
                {
                    print_help ();
                    return true;
                }
            if (strcmp (argv[i], "--no-mount") == 0)
                {
                    cfg->mount = false;
                    continue;
                }
            if (i + 1 >= argc)
                {
                    fprintf (stderr, "orchestrator-translator:"
                             " %s requires an argument\n", argv[i]);
                    exit (EXIT_FAILURE);
                }
            if (strcmp (argv[i], "--instances") == 0)
                cfg->instio = argv[++i];
            else if (strcmp (argv[i], "--neuron") == 0)
                cfg->neuron_bin = argv[++i];
            else if (strcmp (argv[i], "--storage") == 0)
                cfg->storage = argv[++i];
            else if (strcmp (argv[i], "--conninfo") == 0)
                cfg->conninfo = argv[++i];
            else
                {
                    fprintf (stderr, "orchestrator-translator:"
                             " unknown option %s\n", argv[i]);
                    exit (EXIT_FAILURE);
                }
        }
    return false;
}

int
main (int argc, char *argv[])
{
    struct orc_config cfg = { 0 };
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int transport_failures = 0;

    cfg.mount = true;                        /* node mode default */

    if (handle_options (argc, argv, &cfg))
        return EXIT_SUCCESS;

    if (orc_engine_init (&cfg) < 0)
        {
            fprintf (stderr, "orchestrator-translator: %s\n",
                     strerror (errno));
            return EXIT_FAILURE;
        }

    /* One command line in, one response line out: the same
     * engine the trivfs hooks run. */
    while ((n = getline (&line, &cap, stdin)) >= 0)
        {
            size_t len = (size_t) n;

            if (len > 0 && line[len - 1] == '\n')
                len--;
            if (orc_engine_process_line (line, len) < 0)
                {
                    fprintf (stderr,
                             "orchestrator-translator:"
                             " transport failure (EIO)\n");
                    transport_failures++;
                }
            else
                {
                    fwrite (orc_engine_response (), 1,
                            orc_engine_response_len (), stdout);
                    fputc ('\n', stdout);
                    fflush (stdout);
                }
        }

    free (line);
    orc_engine_shutdown ();
    return transport_failures > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
