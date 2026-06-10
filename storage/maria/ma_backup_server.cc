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

#include "maria_def.h"
#include "ma_backup_server.h"
#include "mysqld_error.h"
#if 1 // tc_purge(), tdc_purge()
# include "sql_class.h"
# include "table_cache.h"
#endif
#include <aria_backup.h>
#include <mysqld_error.h>
#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "span.h"
#include <algorithm>
#include <functional>
#include <variant>

/*
  Implementation of functions declatred in ma_backup.h:
  BACKUP SERVER support for Aria engine
*/

namespace
{
  /* Utility class to implement the "backup step" interface when
  processing several lists. It implements the logic where an item
  is processed (copied) from the first list which has available
  items, and a "remaining" counter accumulates the number of
  items remaining to be processed on all lists, regardless of
  whether an item from that list was processed or not. */
  class Copy_from_list
  {
    int m_remaining {0};
    bool m_copy_done;
  public:
    Copy_from_list(bool copy_done= false) noexcept
    : m_copy_done(copy_done)
    {
    }

    bool copy_done() const noexcept
    {
      return m_copy_done;
    }

    int remaining() const noexcept
    {
      return m_remaining;
    }

    template<typename T, typename Fn>
    bool operator()(const T &list, std::atomic<size_t> &copied,
                    Fn copy_action) noexcept
    {
      if(!m_copy_done)
      {
        size_t idx= copied.fetch_add(1, std::memory_order_relaxed);
        if (idx < list.size())
        {
          if (copy_action(list[idx]) != 0)
            return true;
          m_copy_done= true;
          m_remaining+= static_cast<int>(list.size() - idx - 1U);
        }
      }
      else
      {
        size_t current_copied= copied.load(std::memory_order_relaxed);
        if (current_copied < list.size())
          m_remaining+= static_cast<int>(list.size() - current_copied);
      }
      return false;
    }
  };


  class Aria_backup
  {
  public:
    Aria_backup()= default;
    ~Aria_backup()
    {
#ifndef _WIN32
      if (datadir_fd >= 0)
        std::ignore= close(datadir_fd);
      if (logdir_fd >= 0)
        std::ignore= close(logdir_fd);
#endif
      if (translog_purge_disabled)
        translog_enable_purge();
    }

    bool initialize() noexcept
    {
#ifndef _WIN32
      /* Aria table files live under the server data directory
      (mysql_real_data_home), while the transaction logs and control file
      live under aria_log_dir_path (maria_data_root). These differ when
      aria_log_dir_path is set, so open and scan them separately. */
      datadir_fd= open(mysql_real_data_home, O_DIRECTORY);
      if (datadir_fd < 0)
      {
        my_error(ER_CANT_READ_DIR, MYF(0), mysql_real_data_home, errno);
        return true;
      }
      logdir_fd= open(maria_data_root, O_DIRECTORY);
      if (logdir_fd < 0)
      {
        my_error(ER_CANT_READ_DIR, MYF(0), maria_data_root, errno);
        return true;
      }
#endif // _WIN32
      assert(!translog_purge_disabled);
      translog_purge_disabled= true;
      translog_disable_purge();
      return false;
    }

    bool start_copy_dml_safe(const backup_target &target, const backup_sink &sink) noexcept
    {
      assert(translog_purge_disabled);
      if (scan_dbdirs())
        return true;
      flatten_table_lists();
      if (sink.stream == sink.NO_STREAM)
        return ensure_target_dirs(target);
      return false;
    }

    bool start_copy_unsafe() noexcept
    {
      if (scan_logs())
        return true;
      return false;
    }

    /* Copy an Aria table that is safe to be copied while concurrent DML
    is in progress. */
    int dml_safe_copy_step(const backup_target &target, const backup_sink &sink) noexcept
    {
      Copy_from_list copy_from_list;
      auto copy_table_action= [this, target, sink](const table_ref &table) noexcept
                              {
                                return copy_table(target, sink, table);
                              };
      if (copy_from_list(dml_safe_table_list, dml_safe_tables_copied,
                         copy_table_action) != 0)
        return -1;
      if (copy_from_list(unsafe_tables_list, unsafe_tables_copied,
                         copy_table_action) != 0)
        return -1;
      if (copy_from_list(misc_files, misc_files_copied,
                         [this, target, sink](const std::string &path) noexcept
                         {
                           return copy_file(target, sink, path, false);
                         }) != 0)
        return -1;
      return copy_from_list.remaining();
    }

    /* Copy an entity that is not safe to copy if there are concurrent
    writes to it. One entity is copied, of the first category that has
    any remaning entities to be copied. Returns the total number of
    entities to be copied in all categories. Categories in order:
     - log control file
     - log files
     - Aria tables
     - other ("miscellaneous") files
    */
    int unsafe_copy_step(const backup_target &target, const backup_sink &sink) noexcept
    {
      bool copy_done= false;

      /* If control file is always the first file copied and there is only
      one, it is never included in the "steps remaining" calculation.
      Should the order be changed, the calculation needs to be updated for
      the control file as well. */
      if (have_control_file)
      {
        bool already_copied= control_file_copied.exchange(true);
        if (!already_copied)
        {
          if (copy_control_file(target, sink) != 0)
            return -1;
          copy_done= true;
        }
      }

      Copy_from_list copy_from_list(copy_done);
      if (copy_from_list(log_files, log_files_copied,
                         [this, target, sink](const std::string &path) noexcept
                         {
                           return copy_file(target, sink, path, false);
                         }) != 0)
        return -1;

      return copy_from_list.remaining();
    }

    int end(bool /*abort*/) noexcept
    {
      assert(translog_purge_disabled);
      translog_purge_disabled= false;
      translog_enable_purge();
      return 0;
    }
  private:
#ifndef _WIN32
    /** The server data directory (Aria table files) */
    int datadir_fd{-1};
    /** The Aria log directory aria_log_dir_path (logs, control file) */
    int logdir_fd{-1};
#endif
    /** whether the Aria translog_disable_purge() is in effect */
    bool translog_purge_disabled{false};
    static constexpr const char zerobuf[511]{};
    /* All file suffixes are 4 characters long (dot and 3 letter extension) */
    static constexpr size_t suffix_len= 4;
    static constexpr const char* data_ext {MARIA_NAME_DEXT};
    static constexpr const char* index_ext {MARIA_NAME_IEXT};
    static constexpr LEX_CSTRING log_file_prefix {C_STRING_WITH_LEN("aria_log.")};
    static constexpr LEX_CSTRING tmp_prefix {C_STRING_WITH_LEN(tmp_file_prefix)};
    /* TODO: .frm failes are not Aria-specific, .MYD and .MYI are MyISAM files;
    they are copied here as a stop-gap */
    static constexpr const char* misc_exts[] {".MYD", ".MYI", ".frm"};
    static constexpr const char* control_file_name {"aria_log_control"};
    using dir_name = std::string;
    using dir_contents = std::vector<std::string>;
    using database_dir = std::pair<dir_name, dir_contents>;
    using database_dirs = std::vector<database_dir>;
    /* Transactional tables with checksum */
    database_dirs dml_safe_tables;
    /* All other Aria tables */
    database_dirs unsafe_tables;
    /* Aria log files */
    std::vector<std::string> log_files;
    std::vector<std::string> misc_files;
    /* directories in which misc files are */
    std::vector<std::string> misc_dirs;

    bool have_control_file = false;
    bool safe_files_copied = false;

    /* Refer to a string stored elsewhere */
    using dir_ref= std::string_view;
    using tablename_ref= std::string_view;
    using table_ref= std::pair<dir_ref, tablename_ref>;
    using table_list= std::vector<table_ref>;

    /* Flattened versions of dml_safe_tables and unsafe_tables. */
    table_list dml_safe_table_list;
    table_list unsafe_tables_list;
    std::atomic<size_t> dml_safe_tables_copied {0};
    std::atomic<size_t> unsafe_tables_copied {0};
    std::atomic<size_t> log_files_copied {0};
    std::atomic<size_t> misc_files_copied {0};
    std::atomic<bool> control_file_copied {false};

    ATTRIBUTE_COLD ATTRIBUTE_NOINLINE
      static int dir_error(const char *name) noexcept
    {
      my_error(ER_CANT_READ_DIR, MYF(0), name, my_errno);
      return 1;
    }

    int scan_dbdirs() noexcept
    {
      /* Scan the server data directory for Aria table files. */
      MY_DIR *data_dir= my_dir(mysql_real_data_home, MYF(MY_WANT_STAT));
      if (!data_dir)
        return dir_error(mysql_real_data_home);
      int fail= 0;
      for (const fileinfo &fi :
             st_::span<const fileinfo>{data_dir->dir_entry,
                                       data_dir->number_of_files})
        if ((fi.mystat->st_mode & S_IFMT) == S_IFDIR)
        {
          fail= scan_database_dir(fi.name);
          if (fail != 0)
            goto func_exit;
        }
    func_exit:
      my_dirend(data_dir);
      return fail;
    }

    int scan_database_dir(const char* dir_name) noexcept
    {
      const char* base_dir = maria_data_root;
      const std::string dir_path= build_path(base_dir, dir_name);
      MY_DIR *dir_info= my_dir(dir_path.c_str(), MYF(MY_WANT_STAT));
      if (!dir_info)
        return dir_error(dir_path.c_str());
      int fail= 0;
      dir_contents safe;
      dir_contents unsafe;
      for (const fileinfo &fi :
             st_::span<const fileinfo>{dir_info->dir_entry,
                                       dir_info->number_of_files})
      {
        const char* filename= fi.name;
        size_t filename_len = strlen(filename);
        if (filename_len >= suffix_len)
        {
          const char* suffix = filename + filename_len - suffix_len;
          if(match_suffix(suffix, index_ext))
          {
            if (!is_tmp_table(filename))
            {
              auto is_safe = is_safe_table(dir_name, filename);
              if (std::holds_alternative<bool>(is_safe))
              {
                std::string table_name(filename, filename_len - suffix_len);
                if (std::get<bool>(is_safe))
                  safe.push_back(std::move(table_name));
                else
                  unsafe.push_back(std::move(table_name));
              }
              else
              {
                fail= std::get<int>(is_safe);
                goto finish;
              }
            }
          }
          else if (match_misc_ext(suffix) || !strcmp(filename, "db.opt"))
          {
            if(misc_dirs.empty() || misc_dirs.back() != dir_name)
              misc_dirs.emplace_back(dir_name);
            misc_files.push_back(build_path(dir_name, filename));
          }
        }
      }
      if(!fail)
      {
        if (!safe.empty())
          dml_safe_tables.emplace_back(dir_name, std::move(safe));
        if (!unsafe.empty())
          unsafe_tables.emplace_back(dir_name, std::move(unsafe));
      }
    finish:
      my_dirend(dir_info);
      return fail;
    }

    static bool is_tmp_table(const char* filename) noexcept
    {
      return begins_with(filename, tmp_prefix);
    }

    void flatten_table_lists() noexcept
    {
      flatten_table_list(dml_safe_tables, dml_safe_table_list);
      flatten_table_list(unsafe_tables, unsafe_tables_list);
    }

    static void flatten_table_list(const database_dirs& dirs, table_list& list) noexcept
    {
      for (const database_dir& dir : dirs)
      {
        for (const std::string& table : dir.second)
          list.emplace_back(dir.first, table);
      }
    }

    int scan_logs() noexcept
    {
      const char *base_dir= maria_data_root;
      MY_DIR *dir_info= my_dir(base_dir, MYF(MY_WANT_STAT));
      if (!dir_info)
        return dir_error(base_dir);
      for (const fileinfo &fi :
             st_::span<const fileinfo>{dir_info->dir_entry,
                                       dir_info->number_of_files})
        if (begins_with(fi.name, log_file_prefix))
          log_files.emplace_back(fi.name);
        else if (strcmp(fi.name, "aria_log_control") == 0)
          have_control_file = true;
      my_dirend(dir_info);
      return 0;
    }

    bool ensure_target_dirs(const backup_target &target) noexcept
    {
      using string = std::string;
      std::vector<const string*> dirs;
      for (const database_dir &dir : dml_safe_tables)
        dirs.push_back(&dir.first);
      for (const database_dir &dir : unsafe_tables)
        dirs.push_back(&dir.first);
      for (const string &dir: misc_dirs)
        dirs.push_back(&dir);
      std::sort(dirs.begin(), dirs.end(),
        [](const string *a, const string *b) { return *a < *b; });
      auto dirs_end = std::unique(dirs.begin(), dirs.end(),
        [](const string *a, const string *b) { return *a == *b; });
      for (auto it = dirs.begin(); it != dirs_end; ++it)
      {
        if (ensure_target_subdir(target, (*it)->c_str()))
          return true;
      }
      return false;
    }

    /*
       Create directory in the target directory if it does not exist.
       Return 0 on success, non-0 on failure. Set errno in case of failure
    */
    int ensure_target_subdir(const backup_target &target, const char *name)
      noexcept
    {
#ifdef _WIN32
      const std::string dir_path= build_path(target.path, name);
      if (!CreateDirectory(dir_path.c_str(), nullptr))
      {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS)
        {
          my_osmaperr(err);
          return 1;
        }
      }
#else
      if (likely(!mkdirat(target.fd, name, 0777) || errno == EEXIST))
        return 0;
#endif
      my_error(ER_CANT_CREATE_FILE, MYF(0), name, errno);
      return 1;
    }

    /* Returns result or error code. */
    std::variant<bool, int> is_safe_table(const char* dir_name, const char* myi_file_name)
    {
      ARIA_TABLE_CAPABILITIES cap;
#ifndef _WIN32
      std::string path= std::string(dir_name) + "/" + myi_file_name;
      File fd= openat(datadir_fd, path.c_str(), O_RDONLY);
      if (fd < 0)
      {
        my_errno= errno;
        my_error(ER_CANT_OPEN_FILE, MYF(0), path.c_str(), errno);
      }
#else
      std::string path= std::string(maria_data_root) + "/" +
        dir_name + "/" + myi_file_name;
      File fd= my_open(path.c_str(), O_RDONLY, MYF(MY_WME));
#endif
      if (fd < 0)
      {
        return my_errno;
      }
      std::variant<bool, int> result;
      mysql_mutex_lock(&THR_LOCK_maria);
      int fail = aria_get_capabilities(fd, myi_file_name, &cap);
      if (fail)
      {
        my_error(ER_FILE_CORRUPT, MYF(0), path.c_str());
        result= fail;
        goto end;
      }
      result = cap.transactional && cap.checksum;
      aria_free_capabilities(&cap);
end:
      mysql_mutex_unlock(&THR_LOCK_maria);
#ifndef _WIN32
      close(fd);
#else
      my_close(fd, MYF(0));
#endif
      return result;
    }

    int copy_table(const backup_target &target, const backup_sink &sink,
                   const table_ref& table) noexcept
    {
      dir_ref dir_name = table.first;
      tablename_ref table_name = table.second;
      std::string index_path;
      index_path.reserve(dir_name.size() + table_name.size() + 5);
      index_path= dir_name;
      index_path += '/';
      index_path.append(table_name.begin(), table_name.end());
      std::string data_path;
      data_path.reserve(dir_name.size() + table_name.size() + 5);
      data_path= index_path;
      index_path+= index_ext;
      data_path+= data_ext;
      return copy_file(target, sink, index_path, false) ||
             copy_file(target, sink, data_path, false);
    }

    int copy_control_file(const backup_target &target, const backup_sink &sink)
      noexcept
    {
      if (!have_control_file)
        return 0;
      return copy_file(target, sink, control_file_name, true);
    }

    int copy_file(const backup_target &target, const backup_sink &sink,
                  const std::string &path, bool is_log) const noexcept
    {
      return copy_file(target, sink, path.c_str(), is_log);
    }

    int copy_file(const backup_target &target, const backup_sink &sink,
                  const char *path, bool is_log) const noexcept
    {
#ifndef _WIN32
      int ret_val{0};
      int src_fd{openat(is_log ? logdir_fd : datadir_fd, path, O_RDONLY)};
      if (src_fd < 0)
      {
        my_error(ER_CANT_OPEN_FILE, MYF(0), path, errno);
        return 1;
      }
      int tgt_fd{sink.stream};
      if (tgt_fd == sink.NO_STREAM)
      {
        tgt_fd= openat(target.fd, path,
                       O_CREAT | O_EXCL | O_WRONLY, 0666);
        if (tgt_fd < 0)
        {
          my_error(ER_CANT_CREATE_FILE, MYF(0), path, errno);
          ret_val= 1;
        }
        else
        {
          ret_val= copy_entire_file(src_fd, tgt_fd);
          if (ret_val | close(tgt_fd))
          {
          write_error:
            my_error(ER_ERROR_ON_WRITE, MYF(0), path, errno);
            ret_val= 1;
          }
        }
      }
      else
      {
        uint64_t end= uint64_t(lseek(src_fd, 0, SEEK_END));
        if (backup_stream_start(tgt_fd, path, 0644, end, nullptr, 0) ||
            backup_stream_append(src_fd, tgt_fd, 0, end))
          goto write_error;
        if (size_t pad= size_t(end) & 511)
          if (backup_stream_write(tgt_fd, zerobuf, 512 - pad))
            goto write_error;
      }

      close(src_fd);
      return ret_val;
#else
      const std::string src_path= build_path(is_log ? maria_data_root : mysql_real_data_home, path);

      if (sink.stream == sink.NO_STREAM)
      {
        const std::string dest_path= build_path(target.path, path);
        if (!CopyFileEx(src_path.c_str(), dest_path.c_str(), nullptr, nullptr, nullptr,
                        COPY_FILE_NO_BUFFERING))
        {
          my_osmaperr(GetLastError());
          my_error(ER_CANT_CREATE_FILE, MYF(0), dest_path.c_str(), errno);
          return 1;
        }
      }
      else
      {
        HANDLE src, dst{sink.stream};
        for (;;)
        {
          src= CreateFile(src_path.c_str(), GENERIC_READ,
                          FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                          my_win_file_secattr(), OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
          if (src != INVALID_HANDLE_VALUE)
            break;
          switch (GetLastError()) {
          case ERROR_SHARING_VIOLATION:
          case ERROR_LOCK_VIOLATION:
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
          }

          my_osmaperr(GetLastError());
          my_error(ER_FILE_NOT_FOUND, MYF(ME_ERROR_LOG), src_path.c_str(),
                   errno);
          return -1;
        }

        LARGE_INTEGER li;
        if (!GetFileSizeEx(src, &li))
        {
        write_error:
          my_osmaperr(GetLastError());
          my_error(ER_ERROR_ON_WRITE, MYF(0), path, errno);
          if (src != INVALID_HANDLE_VALUE)
            CloseHandle(src);
          return -1;
        }

        if (backup_stream_start(dst, path, 0644, li.QuadPart, nullptr, 0) ||
            backup_stream_append_plain(src, dst, 0, li.QuadPart))
          goto write_error;

        if (size_t pad= size_t(li.LowPart) & 511)
          if (backup_stream_write(dst, zerobuf, 512 - pad))
            goto write_error;
        if (!CloseHandle(src))
        {
          src= INVALID_HANDLE_VALUE;
          goto write_error;
        }
      }
      return 0;
#endif
    }

    static bool match_suffix(const char* suffix1, const char* suffix2) noexcept
    {
      return !memcmp(suffix1, suffix2, suffix_len);
    }

    /* Match if suffix is one of the "other" extensions we need to copy */
    static bool match_misc_ext(const char* suffix_str) noexcept
    {
      uint32_t suffix;
      static_assert (suffix_len == sizeof(suffix));
      memcpy(&suffix, suffix_str, suffix_len);
      switch (suffix) {
#ifdef WORDS_BIGENDIAN
      case 0x2e41524d: /* .ARM ENGINE=ARCHIVE metadata */
      case 0x2e41525a: /* .ARZ ENGINE=ARCHIVE compressed data */
      case 0x2e43534d: /* .CSM ENGINE=CSV metadata */
      case 0x2e435356: /* .CSV ENGINE=CSV data ("comma separated values") */
      case 0x2e4d5247: /* .MRG ENGINE=MRG_MyISAM */
      case 0x2e4d5944: /* .MYD ENGINE=MyISAM data heap */
      case 0x2e4d5949: /* .MYI ENGINE=MyISAM indexes */
      case 0x2e66726d: /* .frm form (SHOW CREATE TABLE) */
      case 0x2e706172: /* .par PARTITION metadata */
#else
      case 0x4d52412e: /* .ARM ENGINE=ARCHIVE metadata */
      case 0x5a52412e: /* .ARZ ENGINE=ARCHIVE compressed data */
      case 0x4d53432e: /* .CSM ENGINE=CSV metadata */
      case 0x5653432e: /* .CSV ENGINE=CSV data ("comma separated values") */
      case 0x47524d2e: /* .MRG ENGINE=MRG_MyISAM */
      case 0x44594d2e: /* .MYD ENGINE=MyISAM data heap */
      case 0x49594d2e: /* .MYI ENGINE=MyISAM indexes */
      case 0x6d72662e: /* .frm form (SHOW CREATE TABLE) */
      case 0x7261702e: /* .par PARTITION metadata */
#endif
        return true;
      default:
        return false;
      }
    }

    static bool begins_with(const char* str, const LEX_CSTRING &prefix) noexcept
    {
      return strncmp(str, prefix.str, prefix.length) == 0;
    }

    static std::string build_path(const char *base_path, const char *filename) noexcept
    {
      std::string path;
      const size_t base_len= strlen(base_path);
      const size_t filename_len= strlen(filename);
      path.reserve(base_len + filename_len + 1);
      path.append(base_path, base_len);
      path+= '/';
      path.append(filename, filename_len);
      return path;
    }
  };
}

void *aria_backup_start(THD *thd, const backup_target *target,
                        backup_phase phase, const backup_sink *sink) noexcept
{
  Aria_backup *aria_backup {};
  if (phase == BACKUP_PHASE_PREPARE_START)
  {
    return 0;
  }
  else if (phase == BACKUP_PHASE_START)
  {
    assert(!sink->ha_data);
    aria_backup= new Aria_backup();
    if (aria_backup->initialize())
    {
      delete aria_backup;
      goto error;
    }
    return aria_backup;
  }

  assert (sink->ha_data != reinterpret_cast<void*>(-1));
  aria_backup= static_cast<Aria_backup*>(sink->ha_data);
  assert(aria_backup);
  switch(phase)
  {
#if 1 // FIXME: invoke these only for Aria, MyISAM, CSV but not others
  case BACKUP_PHASE_NO_DML_NON_TRANS:
    /* FIXME: Would be better to selectively purge only the tables we need. */
    tc_purge();
    tdc_purge(true);
    break;
#endif
  case BACKUP_PHASE_NO_DDL:
#if 1 // FIXME: invoke these only for Aria, MyISAM, CSV but not others
    tc_purge();
    tdc_purge(true);
#endif
    if (aria_backup->start_copy_dml_safe(*target, *sink))
      goto error;
    break;
  case BACKUP_PHASE_NO_COMMIT:
    if (aria_backup->start_copy_unsafe())
      goto error;
    break;
  default:
    break;
  }
  return sink->ha_data;
error:
  return reinterpret_cast<void*>(-1);
}


int aria_backup_step(THD*, const backup_target *target, backup_phase phase,
                     const backup_sink *sink) noexcept
{
  assert (sink->ha_data != reinterpret_cast<void*>(-1));
  Aria_backup *aria_backup= static_cast<Aria_backup*>(sink->ha_data);
  assert(aria_backup);
  switch (phase)
  {
  case BACKUP_PHASE_NO_DDL:
    return aria_backup->dml_safe_copy_step(*target, *sink);
  case BACKUP_PHASE_NO_COMMIT:
    return aria_backup->unsafe_copy_step(*target, *sink);
  default:
    return 0;
  }
}

int aria_backup_end(THD *thd, const backup_target *target, backup_phase phase,
                    const backup_sink *sink) noexcept
{
  assert (sink->ha_data != reinterpret_cast<void*>(-1));
  Aria_backup *aria_backup= static_cast<Aria_backup*>(sink->ha_data);
  switch (phase) {
  case BACKUP_PHASE_NO_COMMIT:
    assert(aria_backup);
    return aria_backup->end(thd);
  case BACKUP_PHASE_FINISH:
    delete aria_backup;
    /* fall through */
  default:
    return 0;
  }
}