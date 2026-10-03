/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * instio.c — Instance transport of the scheduler.
 *
 * node mode: the scheduler allocates the mount point, starts the
 * neuron translator with settrans (fork + exec + wait), then
 * speaks plain POSIX to the node: one write RPC per command line,
 * one full read for the state text.  dir mode: a file that
 * behaves like /llm<N> (PLAN.md section 7) — commands append to
 * llm<N>.in, the state text is read from llm<N>.out.
 *
 * No allocation in the conversation path: the caller owns the
 * buffers.
 */

#include "instio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int
orc_instio_open (struct orc_instio *io, const char *spec,
                 const char *neuron_bin, bool mount)
{
  memset (io, 0, sizeof *io);
  io->mount = mount;

  if (spec == NULL)
    spec = "node:/llm";
  if (neuron_bin == NULL)
    neuron_bin = "/hurd/sigmoid-neuron-translator";

  if (strncmp (spec, "node:", 5) == 0)
    {
      io->mode = 0;
      spec += 5;
    }
  else if (strncmp (spec, "dir:", 4) == 0)
    {
      io->mode = 1;
      spec += 4;
    }
  else
    {
      errno = EINVAL;
      return -1;
    }

  if (*spec == '\0')
    {
      errno = EINVAL;
      return -1;
    }
  if (strlen (spec) >= sizeof io->base - 8)
    {
      errno = ENAMETOOLONG;
      return -1;
    }
  strcpy (io->base, spec);
  if (io->mode == 1)
    {
      /* dir mode: the base always ends with '/', so the instance
       * prefix is a plain concatenation. */
      size_t n = strlen (io->base);

      if (n > 0 && io->base[n - 1] != '/')
        {
          io->base[n] = '/';
          io->base[n + 1] = '\0';
        }
    }
  if (strlen (neuron_bin) >= sizeof io->neuron_bin)
    {
      errno = ENAMETOOLONG;
      return -1;
    }
  strcpy (io->neuron_bin, neuron_bin);
  return 0;
}

/* node mode: make sure the mount point exists, then attach the
 * neuron translator.  settrans replies when the translator is up,
 * so the wait is bounded by the translator itself. */
static int
mount_instance (struct orc_instio *io, const char *path)
{
  pid_t pid;
  int status;

  /* Allocate the node if missing: a plain file is a valid mount
   * point for a trivfs translator. */
  int fd = open (path, O_CREAT | O_WRONLY | O_EXCL, 0666);

  if (fd >= 0)
    close (fd);
  else if (errno != EEXIST)
    return -1;

  pid = fork ();
  if (pid < 0)
    return -1;
  if (pid == 0)
    {
      /* The child becomes settrans; -a starts the translator
       * now, so the mount returns when the node is served. */
      execlp ("settrans", "settrans", "-a", path, io->neuron_bin,
              (char *) NULL);
      _exit (127);
    }
  if (waitpid (pid, &status, 0) < 0)
    return -1;
  if (!WIFEXITED (status) || WEXITSTATUS (status) != 0)
    {
      errno = ENOENT;                       /* the mount failed */
      return -1;
    }
  return 0;
}

int
orc_inst_begin (struct orc_instio *io, int id, struct orc_inst *inst)
{
  memset (inst, 0, sizeof *inst);
  inst->io = io;
  inst->id = id;

  if (id < 1 || (size_t) id >= sizeof inst->path)
    {
      errno = EINVAL;
      return -1;
    }

  if (io->mode == 0)
    {
      snprintf (inst->path, sizeof inst->path, "%s%d", io->base, id);
      if (io->mount && mount_instance (io, inst->path) < 0)
        return -1;
    }
  else
    {
      snprintf (inst->path, sizeof inst->path, "%sllm%d", io->base,
                id);
    }
  return 0;
}

int
orc_inst_write (struct orc_inst *inst, const char *line)
{
  size_t len = strlen (line);
  const char *suffix = "\n";
  char stackbuf[128];
  char *heapbuf = NULL;
  char *wbuf;
  int fd;
  int rc = 0;

  if (len + 1 >= sizeof stackbuf)
    {
      heapbuf = malloc (len + 2);
      if (heapbuf == NULL)
        return -1;
      wbuf = heapbuf;
    }
  else
    wbuf = stackbuf;
  memcpy (wbuf, line, len);
  memcpy (wbuf + len, suffix, 2);

  if (inst->io->mode == 0)
    {
      /* One write RPC is one command on the neuron side. */
      fd = open (inst->path, O_WRONLY);
      if (fd < 0)
        rc = -1;
      else
        {
          ssize_t w = write (fd, wbuf, len + 1);

          if (w != (ssize_t) (len + 1))
            rc = -1;
          close (fd);
          if (rc == 0)
            {
              /* The instance answers EINVAL on its standard error
               * when it does not understand a command; the write
               * itself succeeds, the failure shows up as an empty
               * or unchanged state text — the scheduler marks the
               * instance failed on a missing output (SPEC.md 2).
               */
            }
        }
    }
  else
    {
      char inpath[560];

      snprintf (inpath, sizeof inpath, "%s.in", inst->path);
      fd = open (inpath, O_WRONLY | O_APPEND | O_CREAT, 0666);
      if (fd < 0)
        rc = -1;
      else
        {
          ssize_t w = write (fd, wbuf, len + 1);

          if (w != (ssize_t) (len + 1))
            rc = -1;
          close (fd);
        }
    }

  free (heapbuf);
  return rc;
}

long
orc_inst_read (struct orc_inst *inst, char *buf, size_t cap)
{
  char pathbuf[560];
  const char *rpath;
  int fd;
  size_t total = 0;

  if (inst->io->mode == 0)
    rpath = inst->path;
  else
    {
      snprintf (pathbuf, sizeof pathbuf, "%s.out", inst->path);
      rpath = pathbuf;
    }

  fd = open (rpath, O_RDONLY);
  if (fd < 0)
    return -1;

  for (;;)
    {
      ssize_t r = read (fd, buf + total, cap - total);

      if (r < 0)
        {
          close (fd);
          return -1;
        }
      if (r == 0)
        break;
      total += (size_t) r;
      if (total >= cap)
        {
          /* Keep room for the NUL the caller may want. */
          total = cap - 1;
          break;
        }
    }
  close (fd);
  if (total < cap)
    buf[total] = '\0';
  return (long) total;
}

void
orc_inst_end (struct orc_inst *inst)
{
  /* Phase 1 leaves the instances mounted for the next run; the
   * supervisor of phase 2 decides when to stop them. */
  (void) inst;
}
