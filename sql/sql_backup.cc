/* Copyright (c) 2026, MariaDB plc

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "my_global.h"
#include "mdl.h"
#include "mysys_err.h"
#include "sql_class.h"
#include "sql_backup.h"
#include "sql_backup_interface.h"
#include "sql_parse.h"

#ifdef _WIN32
#elif defined __APPLE__
# include <sys/attr.h>
# include <sys/clonefile.h>
# include <copyfile.h>

int copy_entire_file(int src, int dst)
{
  return fcopyfile(src, dst, nullptr, COPYFILE_ALL | COPYFILE_CLONE);
}

extern "C" int copy_file(int src, int dst, off_t)
{
  return fcopyfile(src, dst, nullptr, COPYFILE_ALL | COPYFILE_CLONE);
}

#else
using copying_step= ssize_t(int,int,size_t,off_t*);
template<copying_step step>
static ssize_t copy(int in_fd, int out_fd, off_t c) noexcept
{
  ssize_t ret;
  for (off_t offset{0};;)
  {
    off_t count= c;
    if (count > INT_MAX >> 20 << 20)
      count = INT_MAX >> 20 << 20;
    ret= step(in_fd, out_fd, size_t(count), &offset);
    if (ret < 0)
      break;
    c-= ret;
    if (!c)
      return 0;
    if (!ret)
      return -1;
  }
  return ret;
}
# if defined __linux__ || defined __FreeBSD__
/* Copy between files in a single (type of) file system */
static inline ssize_t
copy_step(int in_fd, int out_fd, size_t count, off_t *offset) noexcept
{
  return copy_file_range(in_fd, offset, out_fd, nullptr, count, 0);
}
#  define cfr(src,dst,size) copy<copy_step>(src, dst, size)
# endif
# ifdef __linux__
#  include <sys/sendfile.h>
/* Copy a file to a stream or to a regular file. */
static inline ssize_t
send_step(int in_fd, int out_fd, size_t count, off_t *offset) noexcept
{
  return sendfile(out_fd, in_fd, offset, count);
}
# else
#  include "aligned.h"
#  include <sys/mman.h>
/** Copy a file using a memory mapping.
@param in_fd   source file
@param out_fd  destination
@param count   number of bytes to copy
@return error code
@retval 0  on success
@retval 1  if a memory mapping failed */
static ssize_t mmap_copy(int in_fd, int out_fd, off_t count)
{
#if SIZEOF_SIZE_T < 8
  if (count != ssize_t(count))
    return 1;
#endif
  void *p= mmap(nullptr, count, PROT_READ, MAP_SHARED, in_fd, 0);
  if (p == MAP_FAILED)
    return 1;
  ssize_t ret;
  size_t c= size_t(count);
  for (const char *b= static_cast<const char*>(p);; b+= ret)
  {
    ret= write(out_fd, b, std::min(c, size_t(INT_MAX >> 20 << 20)));
    if (ret < 0)
      break;
    c-= ret;
    if (!c)
    {
      ret= 0;
      break;
    }
    if (!ret)
    {
      ret= -1;
      break;
    }
  }
  munmap(p, count);
  return ret;
}

static ssize_t pread_write(int in_fd, int out_fd, off_t count) noexcept
{
  constexpr size_t READ_WRITE_SIZE= 65536;
  char *b= static_cast<char*>(aligned_malloc(READ_WRITE_SIZE, 4096));
  if (!b)
    return -1;
  ssize_t ret;
  for (off_t o= 0;; o+= ret)
  {
    ret= pread(in_fd, b, ssize_t(std::min(count, off_t{READ_WRITE_SIZE})), o);
    if (ret > 0)
      ret= write(out_fd, b, ret);
    if (ret < 0)
      break;
    count-= ret;
    if (!count)
    {
      ret= 0;
      break;
    }
    if (!ret)
    {
      ret= -1;
      break;
    }
  }
  aligned_free(b);
  return ret;
}
# endif

/** Copy a file (whole content).
@param src  source file descriptor
@param dst  target to append src to
@return error code (negative)
@retval 0   on success */
extern "C" int copy_entire_file(int src, int dst)
{
  return copy_file(src, dst, lseek(src, 0, SEEK_END));
}

/** Copy a file.
@param src  source file descriptor
@param dst  target to append src to
@param size amount of data to be copied
@return error code (negative)
@retval 0   on success */
extern "C" int copy_file(int src, int dst, off_t size)
{
  ssize_t ret;
# ifdef cfr
  if (!(ret= cfr(src, dst, size)))
    return int(ret);
#  ifdef __linux__
  if (errno == EOPNOTSUPP)
#  endif
# endif
# ifdef __linux__ // starting with Linux 2.6.33, we can rely on sendfile(2)
    ret= copy<send_step>(src, dst, size);
# else
  if ((ret= mmap_copy(src, dst, size)) == 1)
    ret= pread_write(src, dst, size);
# endif
  assert(ret <= 0);
  return int(ret);
}
#endif

static my_bool backup_start(THD *thd, plugin_ref plugin, void *dst) noexcept
{
  handlerton *hton= plugin_hton(plugin);
  if (hton->backup_start)
    return hton->backup_start(thd, *static_cast<backup_target*>(dst));
  return false;
}

static my_bool backup_end(THD *thd, plugin_ref plugin, void *arg) noexcept
{
  handlerton *hton= plugin_hton(plugin);
  if (hton->backup_end)
    return hton->backup_end(thd, arg != nullptr);
  return false;
}

static my_bool backup_step(THD *thd, plugin_ref plugin, void *) noexcept
{
  handlerton *hton= plugin_hton(plugin);
  int res= 0;
  if (hton->backup_step)
    while ((res= hton->backup_step(thd)))
      if (res < 0)
        break;
  return res != 0;
}

static my_bool backup_finalize(THD *thd, plugin_ref plugin, void *dst) noexcept
{
  handlerton *hton= plugin_hton(plugin);
  if (hton->backup_finalize)
    return hton->backup_finalize(thd, *static_cast<backup_target*>(dst));
  return 0;
}

bool Sql_cmd_backup::execute(THD *thd)
{
  if (check_global_access(thd, RELOAD_ACL) ||
      check_global_access(thd, SELECT_ACL) ||
      error_if_data_home_dir(target.str, "BACKUP SERVER TO"))
    return true;

  if (thd->current_backup_stage != BACKUP_FINISHED)
  {
    my_error(ER_BACKUP_LOCK_IS_ACTIVE, MYF(0));
    return true;
  }

  /* Block concurrent BACKUP SERVER and BACKUP STAGE */
  MDL_request mdl_request;
  MDL_REQUEST_INIT(&mdl_request, MDL_key::BACKUP, "", "", MDL_BACKUP_START,
                   MDL_EXPLICIT);

  if (thd->mdl_context.acquire_lock(&mdl_request,
                                    thd->variables.lock_wait_timeout))
    return true;

  if (my_mkdir(target.str, 0755, MYF(MY_WME)))
  {
#ifndef _WIN32
  err_exit:
#endif
    thd->mdl_context.release_lock(mdl_request.ticket);
    return true;
  }

#ifdef _WIN32
  backup_target dir{target.str, INVALID_HANDLE_VALUE};
#else
  backup_target dir{open(target.str, O_DIRECTORY), true};
  if (dir.fd < 0)
  {
    my_error(EE_CANT_MKDIR, MYF(ME_BELL), target.str, errno);
    goto err_exit;
  }
#endif

  bool fail= plugin_foreach_with_mask(thd, backup_start,
                                      MYSQL_STORAGE_ENGINE_PLUGIN,
                                      PLUGIN_IS_DELETED|PLUGIN_IS_READY, &dir);

  /* The backup_step may be invoked in multiple concurrent threads.
  At the time backup_end is invoked, all backup_step will have to complete. */
  if (!fail)
    fail= plugin_foreach_with_mask(thd, backup_step,
                                   MYSQL_STORAGE_ENGINE_PLUGIN,
                                   PLUGIN_IS_DELETED|PLUGIN_IS_READY, nullptr);

  fail=
    thd->mdl_context.upgrade_shared_lock(mdl_request.ticket,
                                         MDL_BACKUP_WAIT_COMMIT,
                                         thd->variables.lock_wait_timeout) ||
    plugin_foreach_with_mask(thd, backup_end, MYSQL_STORAGE_ENGINE_PLUGIN,
                             PLUGIN_IS_DELETED|PLUGIN_IS_READY,
                             reinterpret_cast<void*>(fail)) || fail;

  /* The final part must not interfere with the use of the server datadir.
  Release the locks. */
  thd->mdl_context.release_lock(mdl_request.ticket);
  fail= plugin_foreach_with_mask(thd, backup_finalize,
                                 MYSQL_STORAGE_ENGINE_PLUGIN,
                                 PLUGIN_IS_DELETED|PLUGIN_IS_READY, &dir) ||
    fail;
#ifndef _WIN32
  close(dir.fd);
#endif

  if (!fail)
    my_ok(thd);
  return fail;
}
