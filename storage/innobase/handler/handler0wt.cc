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

#include <map>
#include <mutex>
#include <string>

#include "api0api.h"
#include "api0misc.h"
#include "dict0boot.h"
#include "dict0dd.h"
#include "fts0fts.h"
#include "fts0priv.h"
#include "fts0types.h"
#include "fil0wt.h"
#include "fsp0fsp.h"
#include "ha_innodb.h"
#include "ha_prototypes.h"
#include "ibuf0ibuf.h"
#include "log0ddl.h"
#include "mtr0log.h"
#include "os0file.h"
#include "my_dbug.h"
#include "mysqld_error.h"
#include "scope_guard.h"
#include "sql/dd/dd.h"
#include "sql/debug_sync.h"
#include "sql/dd/dictionary.h"
#include "sql/dd/impl/sdi.h"
#include "sql/dd/types/column.h"
#include "sql/dd/types/partition.h"
#include "sql/dd/types/partition_index.h"
#include "sql/dd/types/schema.h"
#include "sql/dd/types/table.h"
#include "sql/dd/types/foreign_key.h"
#include "sql/dd/types/index.h"
#include "sql/sql_table.h"
#include "srv0start.h"
#include "trx0purge.h"
#include "ut0new.h"

/** FTS common auxiliary table suffixes (fts0fts.cc). */
extern const char *fts_common_tables[];

/** Serializes master key rotation (ha_innodb.cc). */
extern ib_mutex_t master_key_id_mutex;

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

/** Report a failed reflink; a filesystem without file clones gets an error
that says so. */
bool wt_reflink_error(const std::string &path) {
  if (errno == EOPNOTSUPP || errno == ENOTSUP || errno == EXDEV ||
      errno == EINVAL || errno == ENOTTY) {
    const std::string what =
        "CREATE WORKTREE on a filesystem without copy-on-write file clones "
        "(reflink) at '" +
        path.substr(0, path.rfind('/')) +
        "'; it needs APFS, XFS with reflink=1, or btrfs";
    my_error(ER_NOT_SUPPORTED_YET, MYF(0), what.c_str());
    return true;
  }
  return wt_os_error("reflink (APFS clonefile / FICLONE) to", path);
}

/** Check that dir's filesystem can clone files before anything is quiesced
or written, so an unsupported filesystem fails cleanly. */
bool probe_reflink(const std::string &dir) {
  const std::string a = dir + "/#wt_probe", b = a + ".clone";
  unlink(a.c_str());
  unlink(b.c_str());
  const int fd = open(a.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0640);
  if (fd < 0) return wt_os_error("create", a);
  const bool written = write(fd, "x", 1) == 1;
  close(fd);
  const int ret = written ? reflink(a.c_str(), b.c_str()) : -1;
  const int err = errno;
  unlink(a.c_str());
  unlink(b.c_str());
  errno = err;
  return ret != 0 && wt_reflink_error(b);
}

/** fsync a file or directory. Plain fsync, not F_FULLFSYNC: the same
durability InnoDB itself uses for data and redo files on macOS. */
int sync_path(const char *path, int flags) {
  int fd = open(path, flags);
  if (fd < 0) return -1;
  int ret = fsync(fd);
  close(fd);
  return ret;
}

using Id_map = std::vector<std::pair<space_index_t, space_index_t>>;

/** Rewrite the two header pages of a clone: page 0 (new space id, primary
ancestor map) and page 1 (the change buffer bitmap: re-stamped, redundant map
copy, §12.1 H5). Both are fsynced before the file may get its .ibd name. Page 1
of an encrypted space is encrypted with the tablespace key, which the clone
shares; page 0 never is.
@param[out] overflow  set if the map does not fit in the header pages: they
then carry the new space id and no map, and write_overflow() completes them */
bool rewrite_header_pages(const std::string &path, space_id_t space_id,
                          space_id_t src_space_id, const page_size_t &page_size,
                          const Encryption_metadata &enc, const wt_map_t &map,
                          bool *overflow) {
  int fd = open(path.c_str(), O_RDWR);
  if (fd < 0) return wt_os_error("open", path);
  auto guard = create_scope_guard([fd]() { close(fd); });
  pfs_os_file_t file;
#ifdef UNIV_PFS_IO
  file.m_psi = nullptr;
#endif
  file.m_file = fd;

  const ulint len = page_size.physical();
  auto pages = static_cast<byte *>(ut::aligned_alloc(2 * len, len));
  auto free_guard = create_scope_guard([pages]() { ut::aligned_free(pages); });
  byte *page0 = pages;
  byte *page1 = pages + len;
  IORequest read1(IORequest::READ);
  if (enc.can_encrypt()) read1.get_encryption_info().set(enc);
  if (pread(fd, page0, len, 0) != static_cast<ssize_t>(len) ||
      os_file_read(read1, path.c_str(), file, page1, len, len) != DB_SUCCESS)
    return wt_os_error("read header pages of", path);
  ut_a(fsp_header_get_space_id(page0) == src_space_id);
  ut_a(fil_page_get_type(page1) == FIL_PAGE_IBUF_BITMAP);

  *overflow = false;
  for (page_no_t no : {0, 1}) {
    byte *page = no == 0 ? page0 : page1;
    mach_write_to_4(page + FIL_PAGE_SPACE_ID, space_id);
    if (!*overflow && !wt_map_write(map, page, page_size, no)) *overflow = true;
  }
  if (*overflow) {
    for (page_no_t no : {0, 1}) {
      memset((no == 0 ? page0 : page1) + wt_map_offset(page_size, no), 0,
             wt_map_size(page_size, no));
    }
  }
  mach_write_to_4(page0 + FSP_HEADER_OFFSET + FSP_SPACE_ID, space_id);
  wt_page_recalc_checksum(page0, page_size);
  wt_page_recalc_checksum(page1, page_size);

  IORequest write1(IORequest::WRITE);
  if (enc.can_encrypt()) write1.get_encryption_info().set(enc);
  if (pwrite(fd, page0, len, 0) != static_cast<ssize_t>(len) ||
      os_file_write(write1, path.c_str(), file, page1, len, len) != DB_SUCCESS)
    return wt_os_error("write header pages of", path);
  if (fsync(fd) != 0) return wt_os_error("fsync", path);
  return false;
}

/** Refuse what the reflink core cannot clone (fork-design.md §10). */
bool check_clonable(dict_table_t *src) {
  if (!dict_table_is_file_per_table(src))
    return wt_error(
        "CREATE WORKTREE of tables in shared or general tablespaces");
  if (DICT_TF_HAS_DATA_DIR(src->flags))
    return wt_error("CREATE WORKTREE of tables with DATA DIRECTORY");
  if (src->ibd_file_missing || dict_table_is_discarded(src) ||
      src->is_corrupted())
    return wt_error("CREATE WORKTREE of discarded or corrupted tables");
  fil_space_t *space = fil_space_acquire(src->space);
  if (space == nullptr) return wt_error("CREATE WORKTREE: missing tablespace");
  const bool page_compressed = space->compression_type != Compression::NONE;
  fil_space_release(space);
  if (page_compressed)
    return wt_error("CREATE WORKTREE of tables with COMPRESSION=");
  return false;
}

/** What the clone of one InnoDB table (a table, a partition, or an FTS
auxiliary table) needs for its DD entries. */
struct Space_clone {
  dict_table_t *src{};
  std::string name;  ///< InnoDB name of the clone, "<db>/<table>"
  table_id_t table_id{};
  space_id_t space_id{};
  Id_map ids;  ///< source index id -> clone index id
  std::vector<page_no_t> roots;
  std::string path, tmp_path, src_path;
  dd::Object_id dd_space_id{dd::INVALID_OBJECT_ID};
};

/** Phase 1 (before the quiesce): new identities and the DDL log entries that
delete both file names if we roll back or crash before the commit. */
bool prepare_clone(trx_t *trx, dict_table_t *src, const std::string &name,
                   Space_clone *c) {
  if (check_clonable(src)) return true;
  c->src = src;
  c->name = name;
  dict_hdr_get_new_id(&c->table_id, nullptr, &c->space_id, nullptr, false);
  if (c->space_id == SPACE_UNKNOWN) return wt_error("CREATE WORKTREE: no space id");
  for (const dict_index_t *index = src->first_index(); index != nullptr;
       index = index->next()) {
    space_index_t new_id;
    dict_hdr_get_new_id(nullptr, &new_id, nullptr, nullptr, false);
    c->ids.emplace_back(index->id, new_id);
    c->roots.push_back(index->page);
  }

  char *path = Fil_path::make_ibd_from_table_name(name);
  c->path = path;
  ut::free(path);
  c->tmp_path = c->path + ".wtclone";
  char *src_path = fil_space_get_first_path(src->space);
  c->src_path = src_path;
  ut::free(src_path);

  for (const auto &p : {c->path, c->tmp_path}) {
    if (access(p.c_str(), F_OK) == 0) {
      my_error(ER_TABLESPACE_EXISTS, MYF(0), p.c_str());
      return true;
    }
  }
  for (const auto &p : {c->tmp_path, c->path}) {
    if (log_ddl->write_delete_space_log(trx, nullptr, c->space_id, p.c_str(),
                                        false, false) != DB_SUCCESS) {
      my_error(ER_INTERNAL_ERROR, MYF(0), "CREATE WORKTREE: DDL log");
      return true;
    }
  }
  return false;
}

/** Phase 2 (quiesced): merge the change buffer and flush the source so its
file is a complete image, then reflink it under the temporary name. */
bool snapshot_clone(trx_t *trx, Space_clone *c) {
  const space_id_t space = c->src->space;
  while (ibuf_merge_space(space) != 0) {
  }
  buf_LRU_flush_or_remove_pages(space, BUF_REMOVE_FLUSH_WRITE, trx);
  fil_flush(space);

  /* The SDI root also has segment headers carrying the space id. */
  mtr_t mtr;
  mtr_start(&mtr);
  c->roots.push_back(
      fsp_sdi_get_root_page_num(space, page_size_t(c->src->flags), &mtr));
  mtr_commit(&mtr);

  if (reflink(c->src_path.c_str(), c->tmp_path.c_str()) != 0)
    return wt_reflink_error(c->tmp_path);
  return false;
}

/** Phase 3 (source released): stamp the clone, publish it as an .ibd,
register it, and create its DD tablespace. */
/** The clone's ancestor map: its source, then the source's own ancestors,
whose index ids map straight to ours. */
bool build_map(const Space_clone &c, const wt_map_t *src_map, wt_map_t *map) {
  map->self = c.space_id;
  map->generation = 1;
  map->ancestors.push_back(c.src->space);
  map->index_map = c.ids;
  map->roots = c.roots;
  if (src_map != nullptr) {
    for (auto anc : src_map->ancestors) map->ancestors.push_back(anc);
    for (auto &e : src_map->index_map) {
      for (auto &id : c.ids) {
        if (id.first == e.second) map->index_map.emplace_back(e.first, id.second);
      }
    }
  }
  std::sort(map->index_map.begin(), map->index_map.end());
  if (map->ancestors.size() > WT_MAX_ANCESTORS)
    return wt_error("CREATE WORKTREE: ancestor chain too long");
  return false;
}

/** Complete a clone whose map does not fit in its header pages (§10.1 item
5): allocate overflow pages with the file's own allocator, so nothing is ever
allocated over them, write two copies of the map there, and point page 0 and
page 1 at one copy each. This runs on the temporary file name and without
redo, then flushes, so recovery never sees it half done: a crash leaves only a
temporary file, which the DDL log deletes. */
bool write_overflow(trx_t *trx, Space_clone *c, uint32_t fsp_flags,
                    const wt_map_t &map) {
  const page_size_t page_size(fsp_flags);
  const std::string space_name = c->name + "#wtclone";
  if (fil_ibd_open(true, FIL_TYPE_TABLESPACE, c->space_id, fsp_flags,
                   space_name.c_str(), c->tmp_path.c_str(), true, false) !=
      DB_SUCCESS) {
    my_error(ER_TABLESPACE_MISSING, MYF(0), c->tmp_path.c_str());
    return true;
  }
  auto close_guard =
      create_scope_guard([&]() { fil_close_tablespace(c->space_id); });

  fil_space_t *space = fil_space_acquire(c->space_id);
  /* The allocator reads pages that still carry the ancestor's id. */
  fil_wt_set(space, map);
  const auto bytes = wt_map_serialize(map);
  const ulint cap = wt_overflow_capacity(page_size);
  const ulint n = (bytes.size() + cap - 1) / cap;
  bool ok = true;

  ut_d(fil_space_inc_redo_skipped_count(c->space_id));
  mtr_t mtr;
  mtr.start();
  mtr.set_log_mode(MTR_LOG_NO_REDO);
  mtr_x_lock_space(space, &mtr);
  std::vector<page_no_t> copies[2];
  for (auto &copy : copies) {
    for (ulint i = 0; ok && i < n; ++i) {
      buf_block_t *block = fsp_alloc_wt_page(c->space_id, page_size, &mtr);
      if (block == nullptr) {
        ok = false;
        break;
      }
      mlog_write_string(block->frame + FIL_PAGE_DATA, bytes.data() + i * cap,
                        std::min(cap, bytes.size() - i * cap), &mtr);
      copy.push_back(block->page.id.page_no());
    }
  }
  std::vector<byte> tmp(page_size.physical());
  for (page_no_t no : {0, 1}) {
    if (!ok) break;
    buf_block_t *block = buf_page_get(page_id_t(c->space_id, no), page_size,
                                      RW_X_LATCH, UT_LOCATION_HERE, &mtr);
    memcpy(tmp.data(), block->frame, tmp.size());
    ok = wt_stub_write(map, bytes.size(), copies[no], tmp.data(), page_size, no);
    if (ok) {
      const ulint off = wt_map_offset(page_size, no);
      mlog_write_string(block->frame + off, tmp.data() + off,
                        wt_map_size(page_size, no), &mtr);
    }
  }
  mtr.commit();
  ut_d(fil_space_dec_redo_skipped_count(c->space_id));
  fil_space_release(space);
  if (!ok) return wt_error("CREATE WORKTREE: index count too large for the page size");

  buf_LRU_flush_or_remove_pages(c->space_id, BUF_REMOVE_FLUSH_WRITE, trx);
  fil_flush(c->space_id);
  return false;
}

bool publish_clone(THD *thd, trx_t *trx, dd::Object_id src_dd_table_id,
                   dd::Object_id src_dd_space_id, Space_clone *c) {
  fil_space_t *src_space = fil_space_acquire(c->src->space);
  const uint32_t fsp_flags = src_space->flags;
  const Encryption_metadata enc = src_space->m_encryption_metadata;
  wt_map_t map;
  const bool too_long = build_map(*c, fil_wt_map(src_space), &map);
  fil_space_release(src_space);
  if (too_long) return true;
  const page_size_t page_size(fsp_flags);

  bool overflow;
  if (rewrite_header_pages(c->tmp_path, c->space_id, c->src->space, page_size,
                           enc, map, &overflow))
    return true;
  if (overflow && write_overflow(trx, c, fsp_flags, map)) return true;
  DBUG_EXECUTE_IF("wt_crash_after_page0", DBUG_SUICIDE(););
  DBUG_EXECUTE_IF("wt_fail_after_page0", {
    my_error(ER_UNKNOWN_ERROR, MYF(0));
    return true;
  });

  /* Before the file can be found by the startup scan, record which ids it
  inherits pages from, and where those ancestors' own files are. */
  {
    std::vector<std::pair<space_id_t, std::string>> ancestors{
        {c->src->space, c->src_path}};
    fil_space_t *space = fil_space_acquire(c->src->space);
    const wt_map_t *src_map = fil_wt_map(space);
    std::vector<space_id_t> older;
    if (src_map != nullptr) older = src_map->ancestors;
    fil_space_release(space);
    {
      for (auto anc : older) {
        char *p = fil_space_get_first_path(anc);
        if (p != nullptr) {
          ancestors.emplace_back(anc, p);
          ut::free(p);
        } else {
          ancestors.emplace_back(anc, "");  // gone: no file may claim it
        }
      }
    }
    if (wt_ancestors_record(ancestors))
      return wt_os_error("record ancestors in", "#wt_ancestors");
  }

  /* Only now may the startup scan see it as an .ibd (no duplicate space id). */
  if (rename(c->tmp_path.c_str(), c->path.c_str()) != 0)
    return wt_os_error("rename", c->tmp_path);
  const std::string dir = c->path.substr(0, c->path.rfind('/'));
  if (sync_path(dir.c_str(), O_RDONLY) != 0) return wt_os_error("fsync", dir);
  DBUG_EXECUTE_IF("wt_crash_after_rename", DBUG_SUICIDE(););
  /* Kill with some files of a multi-file clone published and some not. */
  DBUG_EXECUTE_IF("wt_crash_after_third_rename", {
    static int renames = 0;
    if (++renames == 3) DBUG_SUICIDE();
  });

  std::string space_name(c->name);
  dict_name::convert_to_space(space_name);
  if (fil_ibd_open(true, FIL_TYPE_TABLESPACE, c->space_id, fsp_flags,
                   space_name.c_str(), c->path.c_str(), true, false) !=
      DB_SUCCESS) {
    my_error(ER_TABLESPACE_MISSING, MYF(0), c->path.c_str());
    return true;
  }

  /* The clone still carries the source's SDI records (they name the source
  schema); drop them. The clone's own are written when its DD objects are
  stored. FTS auxiliary tables have no SDI. */
  /* Not every file holds every record (a partitioned table's SDI is in only
  some partitions), so delete only the keys that are there. */
  sdi_vector_t keys;
  ib_sdi_vector_t ib_keys;
  ib_keys.sdi_vector = &keys;
  if (ib_sdi_get_keys(c->space_id, &ib_keys, trx) != DB_SUCCESS) {
    my_error(ER_INTERNAL_ERROR, MYF(0), "CREATE WORKTREE: SDI cleanup");
    return true;
  }
  for (const sdi_key_t &key : keys.m_vec) {
    if (!(key.type == SDI_TYPE_TABLE && key.id == src_dd_table_id) &&
        !(key.type == SDI_TYPE_TABLESPACE && key.id == src_dd_space_id))
      continue;
    ib_sdi_key_t ib_key;
    ib_key.sdi_key = &key;
    if (ib_sdi_delete(c->space_id, &ib_key, trx) != DB_SUCCESS) {
      my_error(ER_INTERNAL_ERROR, MYF(0), "CREATE WORKTREE: SDI cleanup");
      return true;
    }
  }

  /* The SDI reads above opened the file and loaded its map: it must be ours. */
  fil_space_t *space = fil_space_acquire(c->space_id);
  const wt_map_t *loaded = fil_wt_map(space);
  const bool map_ok = loaded != nullptr && loaded->self == c->space_id &&
                      loaded->index_map == map.index_map;
  fil_space_release(space);
  if (!map_ok) {
    my_error(ER_INTERNAL_ERROR, MYF(0), "CREATE WORKTREE: map not readable");
    return true;
  }

  return dd_create_implicit_tablespace(dd::get_dd_client(thd), c->space_id,
                                       c->name.c_str(), c->path.c_str(), false,
                                       c->dd_space_id);
}

/** SE private data of one clone's indexes: new ids, same roots. Index trx_id =
this trx, so read views older than the clone get ER_TABLE_DEF_CHANGED instead
of following roll pointers into undo written for the source table (H1). */
template <typename Index_collection>
void set_index_data(Index_collection *indexes, const Space_clone &c,
                    trx_id_t trx_id) {
  for (auto *dd_index : *indexes) {
    dd::Properties &p = dd_index->se_private_data();
    space_index_t old_id;
    if (p.get(dd_index_key_strings[DD_INDEX_ID], &old_id)) continue;
    for (auto &id : c.ids) {
      if (id.first == old_id) p.set(dd_index_key_strings[DD_INDEX_ID], id.second);
    }
    dd_index->set_tablespace_id(c.dd_space_id);
    p.set(dd_index_key_strings[DD_INDEX_SPACE_ID], c.space_id);
    p.set(dd_index_key_strings[DD_TABLE_ID], c.table_id);
    p.set(dd_index_key_strings[DD_INDEX_TRX_ID], trx_id);
  }
}

void set_column_table_id(dd::Table *dd_table, table_id_t table_id) {
  for (dd::Column *col : *dd_table->columns()) {
    if (col->se_private_data().exists(dd_index_key_strings[DD_TABLE_ID])) {
      col->se_private_data().set(dd_index_key_strings[DD_TABLE_ID], table_id);
    }
  }
}

uint64_t next_autoinc(dict_table_t *src) {
  dict_table_autoinc_lock(src);
  const uint64_t next = std::max<uint64_t>(dict_table_autoinc_read(src),
                                           src->autoinc_persisted + 1);
  dict_table_autoinc_unlock(src);
  return next;
}

dd::Object_id first_index_space(const dd::Table &t) {
  for (const dd::Index *i : t.indexes()) return i->tablespace_id();
  return dd::INVALID_OBJECT_ID;
}

dd::Object_id first_index_space(const dd::Partition &p) {
  for (const dd::Partition_index *i : p.indexes()) return i->tablespace_id();
  return dd::INVALID_OBJECT_ID;
}

/** "<dst_db>/<rest of src name>", filename-encoded. */
std::string clone_name(const char *dst_db, const dict_table_t *src) {
  char db_buf[FN_REFLEN];
  tablename_to_filename(dst_db, db_buf, sizeof(db_buf));
  const char *tname = strchr(src->name.m_name, '/');
  ut_a(tname != nullptr);
  return std::string(db_buf) + tname;
}

/** An FTS auxiliary table to clone: its source dict table and DD object, and
the names it gets in the clone. */
struct Fts_aux {
  dict_table_t *src{};
  std::string src_dd_name;
  std::string dst_name;  ///< InnoDB name
  Space_clone clone;
};

/** Auxiliary table names of an FTS table (fts0sql.cc naming), for the given
parent name, parent id and FTS index ids. */
std::vector<std::string> fts_aux_names(
    const dict_table_t *table, const char *parent, table_id_t table_id,
    const std::vector<space_index_t> &fts_index_ids) {
  std::vector<std::string> names;
  char name[MAX_FULL_NAME_LEN];
  fts_table_t ft{};
  ft.table = table;  // only checked for non-null; the name uses the ids below
  ft.parent = parent;
  ft.table_id = table_id;
  ft.type = FTS_COMMON_TABLE;
  for (const char **suffix = fts_common_tables; *suffix != nullptr; ++suffix) {
    ft.suffix = *suffix;
    fts_get_table_name(&ft, name);
    names.emplace_back(name);
  }
  ft.type = FTS_INDEX_TABLE;
  for (auto index_id : fts_index_ids) {
    ft.index_id = index_id;
    for (ulint i = 0; fts_index_selector[i].value != 0; ++i) {
      ft.suffix = fts_index_selector[i].suffix;
      fts_get_table_name(&ft, name);
      names.emplace_back(name);
    }
  }
  return names;
}

/** Take (hold = true) or drop a clone's hold on an FTS table: the first
hold removes it from the optimize thread, the last release adds it back. */
void fts_hold(dict_table_t *table, bool hold) {
  static std::mutex mutex;
  static std::map<table_id_t, int> holds;
  const std::lock_guard<std::mutex> guard(mutex);
  if (hold) {
    if (holds[table->id]++ == 0) fts_optimize_remove_table(table);
  } else if (--holds[table->id] == 0) {
    holds.erase(table->id);
    dict_sys_mutex_enter();
    fts_optimize_add_table(table);
    dict_sys_mutex_exit();
  }
}

}  // namespace

bool innobase_clone_table(handlerton *hton, THD *thd, const dd::Table &src_dd,
                          dd::Table *dst_dd, const char *dst_db) {
  DBUG_TRACE;
  if (srv_read_only_mode) return wt_error("CREATE WORKTREE in read-only mode");

  trx_t *trx = check_trx_exists(thd);
  innobase_register_trx(hton, thd, trx);
  trx_start_if_not_started(trx, true, UT_LOCATION_HERE);
  dd::cache::Dictionary_client *client = dd::get_dd_client(thd);

  /* One clone per InnoDB table behind this DD table: the table itself or each
  leaf partition, plus FTS auxiliary tables. The caller holds MDL SNW on the
  source table, so none of them can change underneath us. */
  std::vector<dict_table_t *> opened;
  auto close_guard = create_scope_guard([&]() {
    for (auto *t : opened) dd_table_close(t, thd, nullptr, false);
  });
  const auto open_src = [&](dd::Object_id id) -> dict_table_t * {
    dict_table_t *t = dd_table_open_on_id(id, thd, nullptr, false, true);
    if (t == nullptr) {
      my_error(ER_NO_SUCH_TABLE, MYF(0), src_dd.name().c_str(), "");
    } else {
      opened.push_back(t);
    }
    return t;
  };

  const bool partitioned = src_dd.partition_type() != dd::Table::PT_NONE;
  std::vector<Space_clone> clones;
  std::vector<dd::Partition *> dst_parts;
  std::vector<const dd::Partition *> src_parts;
  if (partitioned) {
    for (const dd::Partition *p : src_dd.leaf_partitions()) src_parts.push_back(p);
    for (dd::Partition *p : *dst_dd->leaf_partitions()) dst_parts.push_back(p);
    ut_a(src_parts.size() == dst_parts.size());
    clones.resize(src_parts.size());
    for (size_t i = 0; i < src_parts.size(); ++i) {
      dict_table_t *t = open_src(src_parts[i]->se_private_id());
      if (t == nullptr ||
          prepare_clone(trx, t, clone_name(dst_db, t), &clones[i]))
        return true;
    }
  } else {
    clones.resize(1);
    dict_table_t *t = open_src(src_dd.se_private_id());
    if (t == nullptr || prepare_clone(trx, t, clone_name(dst_db, t), &clones[0]))
      return true;
  }

  if (probe_reflink(clones[0].path.substr(0, clones[0].path.rfind('/'))))
    return true;

  /* FULLTEXT (§10.3): the auxiliary tables are hidden InnoDB tables with their
  own files, named after the parent and FTS index ids; they are cloned under
  the names derived from the clone's new ids. */
  dict_table_t *fts_src = partitioned ? nullptr : clones[0].src;
  std::vector<Fts_aux> aux;
  char src_db[FN_REFLEN];
  const bool has_fts =
      fts_src != nullptr && fts_src->fts != nullptr &&
      (dict_table_has_fts_index(fts_src) ||
       DICT_TF2_FLAG_IS_SET(fts_src, DICT_TF2_FTS_HAS_DOC_ID));
  if (has_fts) {
    std::vector<space_index_t> src_fts_ids, dst_fts_ids;
    for (const dict_index_t *index = fts_src->first_index(); index != nullptr;
         index = index->next()) {
      if (!(index->type & DICT_FTS)) continue;
      src_fts_ids.push_back(index->id);
      for (auto &id : clones[0].ids) {
        if (id.first == index->id) dst_fts_ids.push_back(id.second);
      }
    }
    const auto src_names =
        fts_aux_names(fts_src, fts_src->name.m_name, fts_src->id, src_fts_ids);
    const auto dst_names = fts_aux_names(fts_src, clones[0].name.c_str(),
                                         clones[0].table_id, dst_fts_ids);
    /* Opening an auxiliary table may load it from the DD: lock it first
    (statement-long, as its DD object stays cached until the end). */
    char src_db_enc[FN_REFLEN];
    const char *slash = strchr(fts_src->name.m_name, '/');
    snprintf(src_db_enc, sizeof(src_db_enc), "%.*s",
             static_cast<int>(slash - fts_src->name.m_name),
             fts_src->name.m_name);
    filename_to_tablename(src_db_enc, src_db, sizeof(src_db));
    aux.resize(src_names.size());
    for (size_t i = 0; i < src_names.size(); ++i) {
      MDL_request src_req;
      MDL_REQUEST_INIT(&src_req, MDL_key::TABLE, src_db,
                       strchr(src_names[i].c_str(), '/') + 1, MDL_SHARED,
                       MDL_TRANSACTION);
      if (thd->mdl_context.acquire_lock(&src_req,
                                        thd->variables.lock_wait_timeout))
        return true;
      aux[i].src = dd_table_open_on_name(thd, nullptr, src_names[i].c_str(),
                                         false, DICT_ERR_IGNORE_NONE);
      if (aux[i].src == nullptr && src_fts_ids.empty()) {
        /* FTS_DOC_ID without any FULLTEXT index: no auxiliary tables. */
        aux.clear();
        break;
      }
      if (aux[i].src == nullptr) {
        my_error(ER_NO_SUCH_TABLE, MYF(0), src_names[i].c_str(), "");
        return true;
      }
      opened.push_back(aux[i].src);
      aux[i].src_dd_name = strchr(src_names[i].c_str(), '/') + 1;
      aux[i].dst_name = dst_names[i];
      if (prepare_clone(trx, aux[i].src, dst_names[i], &aux[i].clone))
        return true;
    }

    /* Quiesce additions: keep the optimize thread away from the table and
    write the FTS cache out, so documents committed out of order are not
    missed by the clone. Concurrent clones of one table share the hold. */
    fts_hold(fts_src, true);
    if (fts_src->fts->cache != nullptr &&
        fts_sync_table(fts_src, false, true, false) != DB_SUCCESS) {
      fts_hold(fts_src, false);
      my_error(ER_INTERNAL_ERROR, MYF(0), "CREATE WORKTREE: FTS sync");
      return true;
    }
  }
  auto fts_guard = create_scope_guard([&]() {
    if (has_fts) fts_hold(fts_src, false);
  });
  DBUG_EXECUTE_IF("wt_crash_after_ddl_log", DBUG_SUICIDE(););

  /* Encrypted (§10.4): the clone shares the source's tablespace key, copied
with page 0. Keep the master key from rotating until the clone is registered,
so both files carry the key encrypted under the same master key. */
  bool encrypted = false;
  for (auto &c : clones) encrypted |= dd_is_table_in_encrypted_tablespace(c.src);
  if (encrypted) mutex_enter(&master_key_id_mutex);
  auto key_guard = create_scope_guard([&]() {
    if (encrypted) mutex_exit(&master_key_id_mutex);
  });

  /* Quiesce (§12.2 phase 2): no .cfg/.cfp is written. User writes are blocked
  by MDL; stop purge (instance-wide, reference counted, so keep it short),
  then snapshot every file of this table. */
  {
    const bool purge_stopped = trx_purge_state() != PURGE_STATE_DISABLED;
    if (purge_stopped) trx_purge_stop();
    auto purge_guard = create_scope_guard([&]() {
      if (purge_stopped) trx_purge_run();
    });
    for (auto &c : clones) {
      if (snapshot_clone(trx, &c)) return true;
    }
    for (auto &a : aux) {
      if (snapshot_clone(trx, &a.clone)) return true;
    }
  }
  DBUG_EXECUTE_IF("wt_crash_after_reflink", DBUG_SUICIDE(););
  DEBUG_SYNC(thd, "wt_after_snapshot");

  for (size_t i = 0; i < clones.size(); ++i) {
    const dd::Object_id src_space =
        partitioned ? first_index_space(*src_parts[i]) : first_index_space(src_dd);
    if (publish_clone(thd, trx, src_dd.id(), src_space, &clones[i])) return true;
  }

  /* DD entries. */
  if (partitioned) {
    for (size_t i = 0; i < clones.size(); ++i) {
      dst_parts[i]->set_se_private_id(clones[i].table_id);
      set_index_data(dst_parts[i]->indexes(), clones[i], trx->id);
    }
    /* Partitioned tables keep the first partition's id on the columns. */
    set_column_table_id(dst_dd, clones[0].table_id);
  } else {
    dst_dd->set_se_private_id(clones[0].table_id);
    set_index_data(dst_dd->indexes(), clones[0], trx->id);
    set_column_table_id(dst_dd, clones[0].table_id);
  }

  /* The source's next counter value. InnoDB knows the autoinc column only
  once the SQL layer has opened the table, so ask the DD; the in-memory
  counter (if initialized) may be ahead of the persisted one. */
  bool has_autoinc = false;
  for (const dd::Column *col : *dst_dd->columns()) {
    has_autoinc |= col->is_auto_increment();
  }
  if (has_autoinc) {
    uint64_t next = 0;
    for (auto &c : clones) next = std::max(next, next_autoinc(c.src));
    dd_set_autoinc(dst_dd->se_private_data(), next);
  }

  /* FTS auxiliary DD tables: deep copies (SDI round trip) under the new names,
  stored in the same DD transaction as the parent. */
  if (!aux.empty()) {
    const dd::Schema *dst_schema = nullptr;
    if (client->acquire(dst_db, &dst_schema) || dst_schema == nullptr)
      return true;

    for (auto &a : aux) {
      const char *dst_tname = strchr(a.dst_name.c_str(), '/') + 1;
      /* Statement-long locks: the DD objects acquired below stay cached
      until the statement ends. */
      MDL_ticket *dst_mdl = nullptr;
      if (dd::acquire_exclusive_table_mdl(thd, dst_db, dst_tname, false,
                                          &dst_mdl))
        return true;
      const dd::Table *src_aux_dd = nullptr;
      if (client->acquire(src_db, a.src_dd_name.c_str(), &src_aux_dd) ||
          src_aux_dd == nullptr) {
        my_error(ER_NO_SUCH_TABLE, MYF(0), src_db, a.src_dd_name.c_str());
        return true;
      }
      if (publish_clone(thd, trx, src_aux_dd->id(),
                        first_index_space(*src_aux_dd), &a.clone))
        return true;

      const dd::Sdi_type sdi = dd::serialize(thd, *src_aux_dd, src_db);
      std::unique_ptr<dd::Table> dst_aux(dd::create_object<dd::Table>());
      if (dd::deserialize(thd, sdi, dst_aux.get(), dd::CheckDefaultCompatibility,
                          nullptr))
        return true;
      dst_aux->set_name(dst_tname);
      dst_aux->set_schema_id(dst_schema->id());
      dst_aux->set_hidden(src_aux_dd->hidden());
      dst_aux->set_se_private_id(a.clone.table_id);
      set_index_data(dst_aux->indexes(), a.clone, trx->id);
      set_column_table_id(dst_aux.get(), a.clone.table_id);
      if (client->store(dst_aux.get())) return true;
    }
  }

  return false;
}
