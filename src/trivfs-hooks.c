/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * trivfs-hooks.c — Hurd trivfs server routines of /orchestrate.
 *
 * The same line protocol as /db (data-base-translator), the same
 * structure as every trivfs translator of the stack:
 *
 *   - io_write assembles the bytes into complete lines; each
 *     newline terminated line is one command handed to the
 *     engine (orchestrate.c), which installs the response in the
 *     slot (a run is synchronous in phase 1: the write returns
 *     when the run is done);
 *   - io_read serves the response slot from the reader's own
 *     cursor: one cursor per open file per reader, never shared.
 *
 * Because libtrivfs's defaults abort when both read and write
 * support are on, all the io routines are provided here.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "contract.h"
#include "orchestrate.h"

#if ON_HURD == 1

#include <hurd/trivfs.h>
#include <hurd/fsys.h>
#include <hurd/hurd_types.h>

/* ---------------------------------------------------------------------
 *  TRIVFS VARIABLES (read by libtrivfs)
 * ------------------------------------------------------------------- */

int trivfs_fstype = FSTYPE_MISC;
int trivfs_fsid = 0;

int trivfs_support_read = 1;
int trivfs_support_write = 1;
int trivfs_support_exec = 0;

int trivfs_allow_open = O_READ | O_WRITE;

char *fs_help = "orchestrator-translator -- GNU AI coordination"
                " layer (/orchestrate)\n"
                "Usage: settrans -a /orchestrate"
                " orchestrator-translator";

/* ---------------------------------------------------------------------
 *  PER-OPEN STATE: one line assembly buffer and one read cursor
 *  per open file — the multi-user rule of the stack.
 * ------------------------------------------------------------------- */

struct peropen_data
  {
    char   line[ORC_MAX_LINE];
    size_t line_len;
    size_t read_offset;
  };

static error_t
peropen_create (struct trivfs_peropen *po)
{
  po->hook = calloc (1, sizeof (struct peropen_data));
  return po->hook != NULL ? 0 : ENOMEM;
}

static void
peropen_destroy (struct trivfs_peropen *po)
{
  free (po->hook);
  po->hook = NULL;
}

static void __attribute__ ((constructor))
translator_init (void)
{
  trivfs_peropen_create_hook = peropen_create;
  trivfs_peropen_destroy_hook = peropen_destroy;
}

/* ---------------------------------------------------------------------
 *  MANDATORY TRIVFS HOOKS
 * ------------------------------------------------------------------- */

void
trivfs_modify_stat (struct trivfs_protid *cred, io_statbuf_t *st)
{
  (void) cred;

  st->st_mode &= ~((mode_t) S_IFMT);
  st->st_mode |= S_IFREG;
  st->st_size = (loff_t) orc_engine_response_len ();
}

error_t
trivfs_goaway (struct trivfs_control *cntl, int flags)
{
  (void) cntl;
  (void) flags;

  orc_engine_shutdown ();
  exit (0);
}

/* ---------------------------------------------------------------------
 *  IO SERVER ROUTINES
 * ------------------------------------------------------------------- */

kern_return_t
trivfs_S_io_read (struct trivfs_protid *cred,
                  mach_port_t reply,
                  mach_msg_type_name_t replytype,
                  data_t *data,
                  mach_msg_type_number_t *datalen,
                  loff_t offs,
                  vm_size_t amount)
{
  struct peropen_data *pod;
  const char *slot;
  size_t slot_len;
  loff_t position;

  (void) reply;
  (void) replytype;

  if (cred == NULL)
    return EOPNOTSUPP;
  if (!(cred->po->openmodes & O_READ))
    return EBADF;

  pod = cred->po->hook;
  if (pod == NULL)
    return EOPNOTSUPP;

  slot = orc_engine_response ();
  slot_len = orc_engine_response_len ();

  position = offs;
  if (position == -1)
    position = (loff_t) pod->read_offset;
  if (position < 0)
    position = 0;

  if ((size_t) position >= slot_len || amount == 0)
    {
      *datalen = 0;
      return 0;
    }

  if (amount > (vm_size_t) (slot_len - (size_t) position))
    amount = (vm_size_t) (slot_len - (size_t) position);

  if (*datalen < amount)
    {
      *data = mmap (0, amount, PROT_READ | PROT_WRITE,
                    MAP_ANON | MAP_PRIVATE, -1, 0);
      if (*data == MAP_FAILED)
        return ENOMEM;
    }

  memcpy (*data, slot + position, amount);
  *datalen = (mach_msg_type_number_t) amount;

  if (offs == -1)
    pod->read_offset = (size_t) (position + (loff_t) amount);

  return 0;
}

kern_return_t
trivfs_S_io_readable (struct trivfs_protid *cred,
                      mach_port_t reply,
                      mach_msg_type_name_t replytype,
                      vm_size_t *amount)
{
  struct peropen_data *pod;

  (void) reply;
  (void) replytype;

  if (cred == NULL)
    return EOPNOTSUPP;
  if (!(cred->po->openmodes & O_READ))
    return EBADF;

  pod = cred->po->hook;
  if (pod == NULL)
    return EOPNOTSUPP;

  *amount = (vm_size_t) (orc_engine_response_len ()
                         - pod->read_offset);
  return 0;
}

kern_return_t
trivfs_S_io_write (struct trivfs_protid *cred,
                   mach_port_t reply,
                   mach_msg_type_name_t replytype,
                   const_data_t data,
                   mach_msg_type_number_t datalen,
                   loff_t offs,
                   vm_size_t *amt)
{
  struct peropen_data *pod;
  size_t i;

  (void) reply;
  (void) replytype;
  (void) offs;

  if (cred == NULL)
    return EOPNOTSUPP;
  if (!(cred->po->openmodes & O_WRITE))
    return EBADF;

  pod = cred->po->hook;
  if (pod == NULL)
    return EOPNOTSUPP;

  for (i = 0; i < (size_t) datalen; i++)
    {
      if (data[i] == '\n')
        {
          if (orc_engine_process_line (pod->line, pod->line_len) < 0)
            {
              /* Only the transport fails as a POSIX error; the
               * next line starts clean. */
              pod->line_len = 0;
              *amt = (vm_size_t) i;
              return EIO;
            }
          pod->line_len = 0;
          pod->read_offset = 0;   /* the writer reads its answer */
          continue;
        }

      if (pod->line_len >= ORC_MAX_LINE - 1)
        {
          orc_engine_process_line (pod->line, ORC_MAX_LINE);
          pod->line_len = 0;      /* answered "toolong" */
          while (i < (size_t) datalen && data[i] != '\n')
            i++;
          if (i < (size_t) datalen && data[i] == '\n')
            {
              pod->read_offset = 0;
              continue;
            }
          break;
        }

      pod->line[pod->line_len++] = (char) data[i];
    }

  *amt = (vm_size_t) datalen;
  return 0;
}

kern_return_t
trivfs_S_io_seek (struct trivfs_protid *cred,
                  mach_port_t reply,
                  mach_msg_type_name_t replytype,
                  loff_t offs,
                  int whence,
                  loff_t *new_offs)
{
  struct peropen_data *pod;

  (void) reply;
  (void) replytype;

  if (cred == NULL)
    return EOPNOTSUPP;

  pod = cred->po->hook;
  if (pod == NULL)
    return EOPNOTSUPP;

  switch (whence)
    {
    case SEEK_SET:
      break;
    case SEEK_CUR:
      offs += (loff_t) pod->read_offset;
      break;
    case SEEK_END:
      offs += (loff_t) orc_engine_response_len ();
      break;
    default:
      return EINVAL;
    }

  if (offs < 0)
    return EINVAL;

  pod->read_offset = (size_t) offs;
  *new_offs = offs;
  return 0;
}

kern_return_t
trivfs_S_io_select (struct trivfs_protid *cred,
                    mach_port_t reply,
                    mach_msg_type_name_t replytype,
                    int *type)
{
  (void) reply;
  (void) replytype;

  if (cred == NULL)
    return EOPNOTSUPP;

  if (*type & ~(SELECT_READ | SELECT_WRITE))
    return EINVAL;

  return 0;
}

kern_return_t
trivfs_S_io_select_timeout (struct trivfs_protid *cred,
                            mach_port_t reply,
                            mach_msg_type_name_t replytype,
                            int *type,
                            struct timespec *tsp)
{
  (void) tsp;

  return trivfs_S_io_select (cred, reply, replytype, type);
}

#endif /* ON_HURD == 1 */
