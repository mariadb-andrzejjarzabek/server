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

#ifdef __APPLE__
# include <sys/attr.h>
# include <sys/clonefile.h>
# include <copyfile.h>
#endif

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef _WIN32
/* You have to use CopyFileEx() and friends manually */
#elif defined __APPLE__

inline
int copy_entire_file(int src, int dst)
{
  return fcopyfile(src, dst, nullptr, COPYFILE_ALL | COPYFILE_CLONE);
}

inline
int copy_file(int src, int dst, off_t)
{
  return fcopyfile(src, dst, nullptr, COPYFILE_ALL | COPYFILE_CLONE);
}

#else
/** Copy a file.
@param src  source file descriptor
@param dst  target to append src to
@param size amount of data to be copied
@return error code (negative)
@retval 0   on success */
int copy_file(int src, int dst, off_t size);


/** Copy an entire file.
@param src  source file descriptor
@param dst  target to append src to
@return error code (negative)
@retval 0   on success */
int copy_entire_file(int src, int dst);
#endif

#ifdef __cplusplus
}
#endif
