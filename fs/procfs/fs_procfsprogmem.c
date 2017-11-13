/****************************************************************************
 * fs/procfs/fs_procfsprogmem.c
 *
 *   Copyright (C) 2017 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/kmalloc.h>
#include <nuttx/progmem.h>
#include <nuttx/fs/fs.h>
#include <nuttx/fs/procfs.h>

#if defined(CONFIG_ARCH_HAVE_PROGMEM) && !defined(CONFIG_FS_PROCFS_EXCLUDE_PROGMEM)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Determines the size of an intermediate buffer that must be large enough
 * to handle the longest line generated by this logic.
 */

#define PROGMEM_LINELEN 54

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* This structure describes one open "file" */

struct progmem_file_s
{
  struct procfs_file_s base;      /* Base open file structure */
  unsigned int linesize;          /* Number of valid characters in line[] */
  char line[PROGMEM_LINELEN];     /* Pre-allocated buffer for formatted lines */
};

struct progmem_info_s
{
  int arena;                      /* Total size of available progmem. */
  int ordblks;                    /* This is the number of free chunks */
  int mxordblk;                   /* Size of the largest free chunk */
  int uordblks;                   /* Total size of memory for allocated chunks */
  int fordblks;                   /* Total size of memory for free chunks.*/
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void    progmem_getinfo(FAR struct progmem_info_s *progmem);

/* File system methods */

static int     progmem_open(FAR struct file *filep, FAR const char *relpath,
                 int oflags, mode_t mode);
static int     progmem_close(FAR struct file *filep);
static ssize_t progmem_read(FAR struct file *filep, FAR char *buffer,
                 size_t buflen);
static int     progmem_dup(FAR const struct file *oldp,
                 FAR struct file *newp);
static int     progmem_stat(FAR const char *relpath, FAR struct stat *buf);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* See fs_mount.c -- this structure is explicitly externed there.
 * We use the old-fashioned kind of initializers so that this will compile
 * with any compiler.
 */

const struct procfs_operations progmem_operations =
{
  progmem_open,   /* open */
  progmem_close,  /* close */
  progmem_read,   /* read */
  NULL,           /* write */
  progmem_dup,    /* dup */
  NULL,           /* opendir */
  NULL,           /* closedir */
  NULL,           /* readdir */
  NULL,           /* rewinddir */
  progmem_stat    /* stat */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: progmem_getinfo
 *
 * Description:
 *   The moral equivalent of mallinfo() for prog mem
 *
 *   TODO Max block size only works on uniform prog mem
 *
 ****************************************************************************/

static void progmem_getinfo(FAR struct progmem_info_s *progmem)
{
  size_t page = 0;
  size_t stpage = 0xffff;
  size_t pagesize = 0;
  ssize_t status;

  progmem->arena    = 0;
  progmem->fordblks = 0;
  progmem->uordblks = 0;
  progmem->mxordblk = 0;

  for (status = 0, page = 0; status >= 0; page++)
    {
      status   = up_progmem_ispageerased(page);
      pagesize = up_progmem_pagesize(page);

      progmem->arena += pagesize;

      /* Is this beginning of new free space section */

      if (status == 0)
        {
          if (stpage == 0xffff)
            {
              stpage = page;
            }

          progmem->fordblks += pagesize;
        }
      else if (status != 0)
        {
          progmem->uordblks += pagesize;

          if (stpage != 0xffff && up_progmem_isuniform())
            {
              stpage = page - stpage;
              if (stpage > progmem->mxordblk)
                {
                  progmem->mxordblk = stpage;
                }

              stpage = 0xffff;
            }
        }
    }

  progmem->mxordblk *= pagesize;
}

/****************************************************************************
 * Name: progmem_open
 ****************************************************************************/

static int progmem_open(FAR struct file *filep, FAR const char *relpath,
                        int oflags, mode_t mode)
{
  FAR struct progmem_file_s *procfile;

  finfo("Open '%s'\n", relpath);

  /* PROCFS is read-only.  Any attempt to open with any kind of write
   * access is not permitted.
   *
   * REVISIT:  Write-able proc files could be quite useful.
   */

  if ((oflags & O_WRONLY) != 0 || (oflags & O_RDONLY) == 0)
    {
      ferr("ERROR: Only O_RDONLY supported\n");
      return -EACCES;
    }

  /* "progmem" is the only acceptable value for the relpath */

  if (strcmp(relpath, "progmem") != 0)
    {
      ferr("ERROR: relpath is '%s'\n", relpath);
      return -ENOENT;
    }

  /* Allocate a container to hold the file attributes */

  procfile = (FAR struct progmem_file_s *)
    kmm_zalloc(sizeof(struct progmem_file_s));
  if (procfile == NULL)
    {
      ferr("ERROR: Failed to allocate file attributes\n");
      return -ENOMEM;
    }

  /* Save the attributes as the open-specific state in filep->f_priv */

  filep->f_priv = (FAR void *)procfile;
  return OK;
}

/****************************************************************************
 * Name: progmem_close
 ****************************************************************************/

static int progmem_close(FAR struct file *filep)
{
  FAR struct progmem_file_s *procfile;

  /* Recover our private data from the struct file instance */

  procfile = (FAR struct progmem_file_s *)filep->f_priv;
  DEBUGASSERT(procfile);

  /* Release the file attributes structure */

  kmm_free(procfile);
  filep->f_priv = NULL;
  return OK;
}

/****************************************************************************
 * Name: progmem_read
 ****************************************************************************/

static ssize_t progmem_read(FAR struct file *filep, FAR char *buffer,
                           size_t buflen)
{
  FAR struct progmem_file_s *procfile;
  struct progmem_info_s progmem;
  size_t linesize;
  size_t copysize;
  size_t totalsize;
  off_t offset;

  finfo("buffer=%p buflen=%d\n", buffer, (int)buflen);

  DEBUGASSERT(filep != NULL && buffer != NULL && buflen > 0);
  offset = filep->f_pos;

  /* Recover our private data from the struct file instance */

  procfile = (FAR struct progmem_file_s *)filep->f_priv;
  DEBUGASSERT(procfile);

  /* The first line is the headers */

  linesize  = snprintf(procfile->line, PROGMEM_LINELEN,
                       "             total       used       free    largest\n");
  copysize  = procfs_memcpy(procfile->line, linesize, buffer, buflen,
                            &offset);
  totalsize = copysize;

  if (totalsize < buflen)
    {
      buffer += copysize;
      buflen -= copysize;

      /* The second line is the memory data */

      progmem_getinfo(&progmem);

      linesize   = snprintf(procfile->line, PROGMEM_LINELEN,
                            "Prog:  %11d%11d%11d%11d\n",
                            progmem.arena, progmem.uordblks, progmem.fordblks,
                            progmem.mxordblk);
      copysize   = procfs_memcpy(procfile->line, linesize, buffer, buflen,
                                 &offset);
      totalsize += copysize;
    }

  /* Update the file offset */

  filep->f_pos += totalsize;
  return totalsize;
}

/****************************************************************************
 * Name: progmem_dup
 *
 * Description:
 *   Duplicate open file data in the new file structure.
 *
 ****************************************************************************/

static int progmem_dup(FAR const struct file *oldp, FAR struct file *newp)
{
  FAR struct progmem_file_s *oldattr;
  FAR struct progmem_file_s *newattr;

  finfo("Dup %p->%p\n", oldp, newp);

  /* Recover our private data from the old struct file instance */

  oldattr = (FAR struct progmem_file_s *)oldp->f_priv;
  DEBUGASSERT(oldattr);

  /* Allocate a new container to hold the task and attribute selection */

  newattr = (FAR struct progmem_file_s *)
    kmm_malloc(sizeof(struct progmem_file_s));
  if (!newattr)
    {
      ferr("ERROR: Failed to allocate file attributes\n");
      return -ENOMEM;
    }

  /* The copy the file attributes from the old attributes to the new */

  memcpy(newattr, oldattr, sizeof(struct progmem_file_s));

  /* Save the new attributes in the new file structure */

  newp->f_priv = (FAR void *)newattr;
  return OK;
}

/****************************************************************************
 * Name: progmem_stat
 *
 * Description: Return information about a file or directory
 *
 ****************************************************************************/

static int progmem_stat(FAR const char *relpath, FAR struct stat *buf)
{
  /* "progmem" is the only acceptable value for the relpath */

  if (strcmp(relpath, "progmem") != 0)
    {
      ferr("ERROR: relpath is '%s'\n", relpath);
      return -ENOENT;
    }

  /* "progmem" is the name for a read-only file */

  memset(buf, 0, sizeof(struct stat));
  buf->st_mode = S_IFREG | S_IROTH | S_IRGRP | S_IRUSR;
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#endif /* CONFIG_ARCH_HAVE_PROGMEM && !CONFIG_FS_PROCFS_EXCLUDE_PROGMEM */