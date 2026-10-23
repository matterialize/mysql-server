/*****************************************************************************

Copyright (c) 2026, MySQL worktrees contributors.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

*****************************************************************************/

/** @file handler/handler0wt.cc
 CREATE WORKTREE: copy-on-write clone of a file-per-table tablespace
 (fork-design.md §3.1 and §12.2). */

#include <fcntl.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/clonefile.h>
#elif defined(__linux__)
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

#include <string>

#include "dict0boot.h"
#include "dict0dd.h"
#include "fil0wt.h"
#include "fsp0fsp.h"
#include "ha_innodb.h"
#include "ha_prototypes.h"
#include "ibuf0ibuf.h"
#include "log0ddl.h"
#include "my_dbug.h"
#include "mysqld_error.h"
#include "scope_guard.h"
#include "sql/dd/types/column.h"
#include "sql/dd/types/foreign_key.h"
#include "sql/dd/types/index.h"
#include "sql/sql_table.h"
#include "srv0start.h"
#include "trx0purge.h"
#include "ut0new.h"

namespace {

bool wt_error(const char *what) {
  my_error(ER_NOT_SUPPORTED_YET, MYF(0), what);
  return true;
}

bool wt_os_error(const char *op, const std::string &path) {
  my_printf_error(ER_UNKNOWN_ERROR, "CREATE WORKTREE: %s '%s' failed: %s",
                  MYF(0), op, path.c_str(), strerror(errno));
  return true;
}

/** Reflink src to dst (dst must not exist). */
int reflink(const char *src, const char *dst) {
#ifdef __APPLE__
  return clonefile(src, dst, 0);
#elif defined(__linux__)
  int in = open(src, O_RDONLY);
  if (in < 0) return -1;
  int out = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0640);
  if (out < 0) {
    close(in);
    return -1;
  }
  const int ret = ioctl(out, FICLONE, in);
  close(in);
  close(out);
  return ret;
#else
  errno = ENOTSUP;
  return -1;
#endif
}

/** fsync a file or directory; F_FULLFSYNC on macOS. */
int sync_path(const char *path, int flags) {
  int fd = open(path, flags);
  if (fd < 0) return -1;
#ifdef __APPLE__
  int ret = fcntl(fd, F_FULLFSYNC);
  if (ret != 0) ret = fsync(fd);
#else
  int ret = fsync(fd);
#endif
  close(fd);
  return ret;
}

/** Rewrite page 0 of the clone: new space id and the ancestor map. */
bool rewrite_page0(const std::string &path, space_id_t space_id,
                   space_id_t src_space_id, const page_size_t &page_size,
                   const std::vector<std::pair<space_index_t, space_index_t>> &ids,
                   const std::vector<page_no_t> &roots) {
  int fd = open(path.c_str(), O_RDWR);
  if (fd < 0) return wt_os_error("open", path);
  auto guard = create_scope_guard([fd]() { close(fd); });

  const ulint len = page_size.physical();
  auto page = static_cast<byte *>(ut::aligned_alloc(len, len));
  auto free_guard = create_scope_guard([page]() { ut::aligned_free(page); });
  if (pread(fd, page, len, 0) != static_cast<ssize_t>(len))
    return wt_os_error("read page 0 of", path);
  ut_a(fsp_header_get_space_id(page) == src_space_id);

  /* A worktree of a worktree inherits its source's ancestors; their index ids
  map straight to ours. */
  wt_map_t src_map;
  const bool src_is_wt = wt_map_read(page, page_size, &src_map);

  wt_map_t map;
  map.ancestors.push_back(src_space_id);
  map.index_map = ids;
  map.roots = roots;
  if (src_is_wt) {
    for (auto anc : src_map.ancestors) map.ancestors.push_back(anc);
    for (auto &e : src_map.index_map) {
      for (auto &id : ids) {
        if (id.first == e.second) map.index_map.emplace_back(e.first, id.second);
      }
    }
  }
  std::sort(map.index_map.begin(), map.index_map.end());

  mach_write_to_4(page + FIL_PAGE_SPACE_ID, space_id);
  mach_write_to_4(page + FSP_HEADER_OFFSET + FSP_SPACE_ID, space_id);
  if (!wt_map_write(map, page, page_size)) {
    return wt_error("CREATE WORKTREE: ancestor chain or index count too large");
  }
  wt_page_recalc_checksum(page, page_size);

  if (pwrite(fd, page, len, 0) != static_cast<ssize_t>(len))
    return wt_os_error("write page 0 of", path);
#ifdef __APPLE__
  if (fcntl(fd, F_FULLFSYNC) != 0 && fsync(fd) != 0)
    return wt_os_error("fsync", path);
#else
  if (fsync(fd) != 0) return wt_os_error("fsync", path);
#endif
  return false;
}

}  // namespace

bool innobase_clone_table(handlerton *hton, THD *thd, const dd::Table &src_dd,
                          dd::Table *dst_dd, const char *dst_db) {
  DBUG_TRACE;
  if (srv_read_only_mode) return wt_error("CREATE WORKTREE in read-only mode");

  trx_t *trx = check_trx_exists(thd);
  innobase_register_trx(hton, thd, trx);
  trx_start_if_not_started(trx, true, UT_LOCATION_HERE);

  /* The caller holds MDL SNW on the source table. */
  dict_table_t *src =
      dd_table_open_on_id(src_dd.se_private_id(), thd, nullptr, false, true);
  if (src == nullptr) {
    my_error(ER_NO_SUCH_TABLE, MYF(0), src_dd.name().c_str(), "");
    return true;
  }
  auto close_guard =
      create_scope_guard([&]() { dd_table_close(src, thd, nullptr, false); });

  /* Outside the reflink core (fork-design.md §10): refuse clearly. */
  if (!dict_table_is_file_per_table(src))
    return wt_error(
        "CREATE WORKTREE of tables in shared or general tablespaces");
  if (dict_table_has_fts_index(src) ||
      DICT_TF2_FLAG_IS_SET(src, DICT_TF2_FTS_HAS_DOC_ID))
    return wt_error("CREATE WORKTREE of tables with FULLTEXT indexes");
  if (DICT_TF_GET_ZIP_SSIZE(src->flags) != 0)
    return wt_error("CREATE WORKTREE of ROW_FORMAT=COMPRESSED tables");
  if (dd_is_table_in_encrypted_tablespace(src))
    return wt_error("CREATE WORKTREE of encrypted tables");
  if (DICT_TF_HAS_DATA_DIR(src->flags))
    return wt_error("CREATE WORKTREE of tables with DATA DIRECTORY");
  if (src->ibd_file_missing || dict_table_is_discarded(src) ||
      src->is_corrupted())
    return wt_error("CREATE WORKTREE of discarded or corrupted tables");

  fil_space_t *src_space = fil_space_acquire(src->space);
  if (src_space == nullptr) return wt_error("CREATE WORKTREE: missing tablespace");
  const uint32_t fsp_flags = src_space->flags;
  const bool page_compressed =
      src_space->compression_type != Compression::NONE;
  fil_space_release(src_space);
  if (page_compressed)
    return wt_error("CREATE WORKTREE of tables with COMPRESSION=");

  /* New identities: table id, space id, one id per index. Roots stay. */
  table_id_t table_id;
  space_id_t space_id;
  dict_hdr_get_new_id(&table_id, nullptr, &space_id, nullptr, false);
  if (space_id == SPACE_UNKNOWN) return wt_error("CREATE WORKTREE: no space id");

  std::vector<std::pair<space_index_t, space_index_t>> ids;
  std::vector<page_no_t> roots;
  for (const dict_index_t *index = src->first_index(); index != nullptr;
       index = index->next()) {
    space_index_t new_id;
    dict_hdr_get_new_id(nullptr, &new_id, nullptr, nullptr, false);
    ids.emplace_back(index->id, new_id);
    roots.push_back(index->page);
  }
  const page_size_t page_size(fsp_flags);

  /* InnoDB name of the clone: "<dst_db>/<table>", filename-encoded. */
  char db_buf[FN_REFLEN];
  tablename_to_filename(dst_db, db_buf, sizeof(db_buf));
  const char *tname = strchr(src->name.m_name, '/');
  ut_a(tname != nullptr);
  const std::string name = std::string(db_buf) + tname;

  char *path_c = Fil_path::make_ibd_from_table_name(name);
  const std::string path(path_c);
  ut::free(path_c);
  const std::string tmp_path = path + ".wtclone";
  char *src_path_c = fil_space_get_first_path(src->space);
  const std::string src_path(src_path_c);
  ut::free(src_path_c);

  for (const auto &p : {path, tmp_path}) {
    if (access(p.c_str(), F_OK) == 0) {
      my_error(ER_TABLESPACE_EXISTS, MYF(0), p.c_str());
      return true;
    }
  }

  /* On rollback or crash before commit, recovery deletes both names. */
  for (const auto &p : {tmp_path, path}) {
    if (log_ddl->write_delete_space_log(trx, nullptr, space_id, p.c_str(),
                                        false, false) != DB_SUCCESS) {
      my_error(ER_INTERNAL_ERROR, MYF(0), "CREATE WORKTREE: DDL log");
      return true;
    }
  }
  DBUG_EXECUTE_IF("wt_crash_after_ddl_log", DBUG_SUICIDE(););

  /* Quiesce (§12.2 phase 2): no .cfg/.cfp is written. User writes are blocked
  by MDL; stop purge, merge the change buffer, then flush every dirty page so
  the file on disk is a complete image. Purge is instance-wide: keep short. */
  const bool purge_was_running = trx_purge_state() != PURGE_STATE_DISABLED &&
                                 trx_purge_state() != PURGE_STATE_STOP;
  if (purge_was_running) trx_purge_stop();
  auto purge_guard = create_scope_guard([&]() {
    if (purge_was_running) trx_purge_run();
  });

  while (ibuf_merge_space(src->space) != 0) {
  }
  buf_LRU_flush_or_remove_pages(src->space, BUF_REMOVE_FLUSH_WRITE, trx);
  fil_flush(src->space);

  /* The SDI root also has segment headers carrying the space id. */
  {
    mtr_t mtr;
    mtr_start(&mtr);
    roots.push_back(fsp_sdi_get_root_page_num(src->space, page_size, &mtr));
    mtr_commit(&mtr);
  }

  if (reflink(src_path.c_str(), tmp_path.c_str()) != 0)
    return wt_os_error("reflink (APFS clonefile / FICLONE) to", tmp_path);
  purge_guard.reset();
  DBUG_EXECUTE_IF("wt_crash_after_reflink", DBUG_SUICIDE(););

  if (rewrite_page0(tmp_path, space_id, src->space, page_size, ids, roots))
    return true;
  DBUG_EXECUTE_IF("wt_crash_after_page0", DBUG_SUICIDE(););
  DBUG_EXECUTE_IF("wt_fail_after_page0", {
    my_error(ER_UNKNOWN_ERROR, MYF(0));
    return true;
  });

  /* Only now may the startup scan see it as an .ibd (no duplicate space id). */
  if (rename(tmp_path.c_str(), path.c_str()) != 0)
    return wt_os_error("rename", tmp_path);
  const std::string dir = path.substr(0, path.rfind('/'));
  if (sync_path(dir.c_str(), O_RDONLY) != 0) return wt_os_error("fsync", dir);
  DBUG_EXECUTE_IF("wt_crash_after_rename", DBUG_SUICIDE(););

  std::string space_name(name);
  dict_name::convert_to_space(space_name);
  if (fil_ibd_open(true, FIL_TYPE_TABLESPACE, space_id, fsp_flags,
                   space_name.c_str(), path.c_str(), true, false) !=
      DB_SUCCESS) {
    my_error(ER_TABLESPACE_MISSING, MYF(0), path.c_str());
    return true;
  }

  dd::cache::Dictionary_client *client = dd::get_dd_client(thd);
  dd::Object_id dd_space_id = dd::INVALID_OBJECT_ID;
  if (dd_create_implicit_tablespace(client, space_id, name.c_str(),
                                    path.c_str(), false, dd_space_id))
    return true;

  /* SE private data: new ids, same roots. Index trx_id = this trx, so read
  views older than the clone get ER_TABLE_DEF_CHANGED instead of following
  roll pointers into undo written for the source table id (§12.1 H1). */
  dst_dd->set_se_private_id(table_id);
  for (dd::Index *dd_index : *dst_dd->indexes()) {
    dd::Properties &p = dd_index->se_private_data();
    space_index_t old_id;
    if (p.get(dd_index_key_strings[DD_INDEX_ID], &old_id)) continue;
    for (auto &id : ids) {
      if (id.first == old_id) p.set(dd_index_key_strings[DD_INDEX_ID], id.second);
    }
    dd_index->set_tablespace_id(dd_space_id);
    p.set(dd_index_key_strings[DD_INDEX_SPACE_ID], space_id);
    p.set(dd_index_key_strings[DD_TABLE_ID], table_id);
    p.set(dd_index_key_strings[DD_INDEX_TRX_ID], trx->id);
  }
  for (dd::Column *col : *dst_dd->columns()) {
    if (col->se_private_data().exists(dd_index_key_strings[DD_TABLE_ID])) {
      col->se_private_data().set(dd_index_key_strings[DD_TABLE_ID], table_id);
    }
  }

  /* The source's next counter value. InnoDB knows the autoinc column only
  once the SQL layer has opened the table, so ask the DD; the in-memory
  counter (if initialized) may be ahead of the persisted one. */
  bool has_autoinc = false;
  for (const dd::Column *col : *dst_dd->columns()) {
    has_autoinc |= col->is_auto_increment();
  }
  if (has_autoinc) {
    dict_table_autoinc_lock(src);
    const uint64_t next = std::max<uint64_t>(dict_table_autoinc_read(src),
                                             src->autoinc_persisted + 1);
    dict_table_autoinc_unlock(src);
    dd_set_autoinc(dst_dd->se_private_data(), next);
  }

  return false;
}
