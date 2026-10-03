/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * storage_db.h — The data-base-translator backend (internal).
 *
 * Two transports of the SAME /db line contract: the translator
 * node (production, GNU/Hurd), or the db-translator verification
 * binary spawned as a child (integration on any POSIX system).
 */

#ifndef ORC_STORAGE_DB_H
#define ORC_STORAGE_DB_H

#include <sys/types.h>

#include "storage.h"

struct orc_db
  {
    int mode;                     /* 0 = node, 1 = child process */
    int fd;                       /* node mode */
    pid_t pid;                    /* child mode */
    int in_fd;                    /* child stdin  (we write) */
    int out_fd;                   /* child stdout (we read) */
    char bin[256];
  };

int orc_db_open (struct orc_db *db, const char *spec,
                 const char *conninfo);
void orc_db_close (struct orc_db *db);

enum orc_storage_status orc_db_write_run (struct orc_db *db,
                                         const struct orc_run_row *row,
                                         long long *id);
enum orc_storage_status orc_db_write_instance (
                                        struct orc_db *db,
                                        const struct orc_inst_row *row,
                                        long long *id);

#endif /* ORC_STORAGE_DB_H */
