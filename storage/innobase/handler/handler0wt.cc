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
#include <sys/statvfs.h>
#ifdef __APPLE__
#include <sys/clonefile.h>
#elif defined(__linux__)
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

#include <dirent.h>

#include <chrono>
#include <map>
#include <set>
#include <thread>
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
#include "fil0overlay.h"
#include "log0buf.h"
#include "log0chkp.h"
#include "log0files_governor.h"
#include "trx0sys.h"
#include "buf0dblwr.h"
#include "buf0flu.h"
#include <sys/stat.h>
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
#include "sql/sql_class.h"
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
  /* A list: ENOTSUP and EOPNOTSUPP are the same value on Linux, not macOS. */
  const int err = errno;
  bool unsupported = false;
  for (int code : {EOPNOTSUPP, ENOTSUP, EXDEV, EINVAL, ENOTTY})
    unsupported |= err == code;
  if (unsupported) {
    const std::string where =
        path.find('/') == std::string::npos
            ? std::string("in the data directory")
            : "at '" + path.substr(0, path.rfind('/')) + "'";
    const std::string what =
        "CREATE WORKTREE on a filesystem without copy-on-write file clones "
        "(reflink) " +
        where + "; it needs APFS, XFS with reflink=1, or btrfs";
    my_error(ER_NOT_SUPPORTED_YET, MYF(0), what.c_str());
    return true;
  }
  return wt_os_error("reflink (APFS clonefile / FICLONE) to", path);
}

/** Check that the datadir's filesystem can clone files before anything is
quiesced or written, so an unsupported filesystem fails cleanly. The probe is
not in the new schema's directory: a crash before the unlink would leave a file
there that the DDL log does not know, and rolling back the schema would fail. */
bool probe_reflink(THD *thd) {
  /* The filesystem doesn't change under a running server, and the probe's
  file operations, once per table, contended with each other in concurrent
  creates. */
  static std::atomic<bool> works{false};
  if (works.load()) return false;
  const std::string a = "#wt_probe_" + std::to_string(thd_get_thread_id(thd));
  const std::string b = a + ".clone";
  unlink(a.c_str());
  unlink(b.c_str());
  const int fd = open(a.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0640);
  if (fd < 0) return wt_os_error("create", a);
  const bool written = write(fd, "x", 1) == 1;
  close(fd);
  DBUG_EXECUTE_IF("wt_crash_in_probe", DBUG_SUICIDE(););
  const int ret = written ? reflink(a.c_str(), b.c_str()) : -1;
  const int err = errno;
  unlink(a.c_str());
  unlink(b.c_str());
  errno = err;
  if (ret == 0) works.store(true);
  return ret != 0 && wt_reflink_error(b);
}

/** fsync a directory. */
int sync_dir(const char *dir) {
  const int fd = open(dir, O_RDONLY);
  if (fd < 0) return -1;
  const int ret = fsync(fd);
  close(fd);
  return ret;
}

/** Make #wtlayers with the first layer (durably), so a data directory that
never had one stays as stock leaves it: --initialize refuses a non-empty one.
@return false on error (errno set) */
bool layer_dir_ready() {
  if (mkdir(WT_LAYER_DIR, 0750) == 0) return sync_dir(".") == 0;
  return errno == EEXIST;
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
                          const std::vector<byte> &head, bool *overflow) {
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
  if (!head.empty()) {
    memcpy(pages, head.data(), 2 * len);
  } else {
    IORequest read1(IORequest::READ);
    if (enc.can_encrypt()) read1.get_encryption_info().set(enc);
    if (pread(fd, page0, len, 0) != static_cast<ssize_t>(len) ||
        os_file_read(read1, path.c_str(), file, page1, len, len) != DB_SUCCESS)
      return wt_os_error("read header pages of", path);
  }
  ut_a(fsp_header_get_space_id(page0) == src_space_id);
  ut_a(fil_page_get_type(page1) == FIL_PAGE_IBUF_BITMAP);

  *overflow = false;
  for (page_no_t no : {0, 1}) {
    byte *page = no == 0 ? page0 : page1;
    mach_write_to_4(page + FIL_PAGE_SPACE_ID, space_id);
    /* (Page 1's copy only where page 1 has a map area.) */
    if (!*overflow && wt_map_size(page_size, no) > 0 &&
        !wt_map_write(map, page, page_size, no))
      *overflow = true;
  }
  if (*overflow) {
    /* An overlay needs its link from its first open (the chain is read
    through it); the rest of the map follows in overflow pages. */
    wt_map_t link_only;
    link_only.self = map.self;
    link_only.generation = map.generation;
    link_only.has_link = map.has_link;
    link_only.link = map.link;
    for (page_no_t no : {0, 1}) {
      byte *page = no == 0 ? page0 : page1;
      if (wt_map_size(page_size, no) == 0) continue;
      memset(page + wt_map_offset(page_size, no), 0, wt_map_size(page_size, no));
      if (map.has_link) ut_a(wt_map_write(link_only, page, page_size, no));
    }
  }
  if (map.has_link) {
    /* Overlay file (fil0overlay.h): a stock server refuses it. */
    mach_write_to_4(page0 + FSP_HEADER_OFFSET + FSP_SPACE_FLAGS,
                    fsp_header_get_flags(page0) | FSP_FLAGS_MASK_WT_OVERLAY);
  }
  mach_write_to_4(page0 + FSP_HEADER_OFFSET + FSP_SPACE_ID, space_id);
  wt_page_recalc_checksum(page0, page_size);
  wt_page_recalc_checksum(page1, page_size);

  IORequest write1(IORequest::WRITE);
  if (enc.can_encrypt()) write1.get_encryption_info().set(enc);
  {
#ifdef __APPLE__
    /* APFS serializes the first write to a clone (it takes private
    references on the shared extents) and degrades when many threads try at
    once: 50 concurrent creates were 2.6 times slower than one after the
    other (milestone 5). Take turns. */
    static std::mutex first_write_mutex;
    const std::lock_guard<std::mutex> guard(first_write_mutex);
#endif
    if (pwrite(fd, page0, len, 0) != static_cast<ssize_t>(len))
      return wt_os_error("write header pages of", path);
  }
  if (os_file_write(write1, path.c_str(), file, page1, len, len) != DB_SUCCESS)
    return wt_os_error("write header pages of", path);
#ifndef __linux__
  /* Linux: made durable by one syncfs() before the commit (sql_worktree),
  not a journal commit per file. */
  if (fsync(fd) != 0) return wt_os_error("fsync", path);
#endif
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
  /* Overlay mode: the parent link (layer name, source size, F). */
  bool overlay{};
  bool src_read_only{};
  wt_link_t link;
  /* The source's operation slot, while this clone holds it, and whether it
  shares it with other creates (a read-only source linked live). */
  fil_space_t *slot{};
  bool slot_shared{};
  /* The layer this clone pinned for GC (wt_pinned_layers), if any. */
  std::string pinned;
  /* Reflink mode: the source's pages 0 and 1 (page 1 decrypted) as cloned. */
  std::vector<byte> head;
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

/** Overlay mode: excludes GC and other freezes during F0-F6, and guards the
per-space operation slots (limits/overlay-v3.md §7.1). */
std::mutex wt_layer_mutex;

/** Layers just linked by a CREATE WORKTREE in progress, before its clone's
header names them: GC roots. Protected by wt_layer_mutex. */
std::set<std::string> wt_pinned_layers;

/** Take the operation slot of a space (v3 I8), waiting up to
lock_wait_timeout. @return true on timeout (error reported) */
bool wt_op_acquire(THD *thd, fil_space_t *space, uint8_t op, bool shared = false) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(thd_lock_wait_timeout(thd));
  for (;;) {
    {
      const std::lock_guard<std::mutex> guard(wt_layer_mutex);
      if (shared && space->wt_op == 0) {
        ++space->wt_op_shared;
        return false;
      }
      if (!shared && space->wt_op == 0 && space->wt_op_shared == 0) {
        space->wt_op = op;
        return false;
      }
    }
    if (std::chrono::steady_clock::now() > deadline || thd_killed(thd)) {
      my_error(ER_LOCK_WAIT_TIMEOUT, MYF(0));
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void wt_op_release(fil_space_t *space, bool shared = false) {
  const std::lock_guard<std::mutex> guard(wt_layer_mutex);
  if (shared) {
    --space->wt_op_shared;
    return;
  }
  space->wt_op = 0;
}

/** Write a space's dirty pages and fsync its file, for a space nothing can
modify meanwhile. buf_LRU_flush_or_remove_pages(BUF_REMOVE_FLUSH_WRITE) scans
every flush list and waits for every pending write of the instance, per
table: seconds per create after a refresh or under write load (milestone 5,
F2/F3). @return true on interruption (error reported) */
bool wt_flush_space(space_id_t id, trx_t *trx) {
  fil_space_t *space = fil_space_acquire(id);
  const bool interrupted = buf_LRU_flush_space(space, trx);
  fil_space_release(space);
  if (interrupted) {
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
    return true;
  }
  fil_flush(id);
  return false;
}

/** The number of extents of a file, counted up to limit + 1, or -1 if the
filesystem can't tell. */
long wt_file_extents(int fd, off_t size, long limit) {
#ifdef __APPLE__
  long n = 0;
  for (off_t off = 0; off < size && n <= limit; ++n) {
    log2phys l2p{};
    l2p.l2p_contigbytes = size - off;
    l2p.l2p_devoffset = off;
    if (fcntl(fd, F_LOG2PHYS_EXT, &l2p) != 0 || l2p.l2p_contigbytes <= 0)
      return -1;
    off += l2p.l2p_contigbytes;
  }
  return n;
#elif defined(__linux__)
  (void)size;
  (void)limit;
  fiemap fm{};
  fm.fm_length = FIEMAP_MAX_OFFSET;
  if (ioctl(fd, FS_IOC_FIEMAP, &fm) != 0) return -1;
  return fm.fm_mapped_extents;
#else
  return -1;
#endif
}

/** Plain-copy a file (never a clone: the point is new contiguous blocks)
and fsync the copy. @return false on error (errno set) */
bool wt_copy_file(const std::string &from, const std::string &to) {
  const int in = open(from.c_str(), O_RDONLY);
  if (in < 0) return false;
  auto in_guard = create_scope_guard([in]() { close(in); });
  const int out = open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0640);
  if (out < 0) return false;
  auto out_guard = create_scope_guard([out]() { close(out); });
  std::vector<byte> buf(4 << 20);
  for (off_t off = 0;;) {
    const ssize_t n = pread(in, buf.data(), buf.size(), off);
    if (n < 0) return false;
    if (n == 0) break;
    if (pwrite(out, buf.data(), n, off) != n) return false;
    off += n;
  }
  return fsync(out) == 0;
}

/** Reflink mode: rewrite a fragmented file of a read-only schema
contiguously. Writing a file that clones share splits its extents, and on
APFS the first write to a clone (CREATE WORKTREE writes page 0 of each)
costs about 6 us per extent of its source, so a refresh in place made every
later create slow, for good (milestone 5, F2). The copy has the same bytes,
so readers and clones taken meanwhile see the same data either way.
Problems are only logged: this is an optimization. */
void wt_compact_space(trx_t *trx, space_id_t id) {
  fil_space_t *space = fil_space_acquire(id);
  char *p = fil_space_get_first_path(id);
  const std::string path(p), tmp = path + ".compact.wttmp";
  const std::string dir = path.substr(0, path.rfind('/'));
  ut::free(p);
  auto release = create_scope_guard([space]() { fil_space_release(space); });

  /* Nothing writes the read-only source but purge and change buffer
  merges: stop one, do the other, and flush (the refresh's writes split the
  extents only once they reach the file). A write slipping in anyway shows
  in the counters checked under the gate, and the copy is retried. */
  trx_purge_stop();
  auto purge_guard = create_scope_guard([]() { trx_purge_run(); });
  const auto flush = [&]() {
    while (ibuf_merge_space(id) != 0) {
    }
    if (buf_LRU_flush_space(space, trx)) return false;
    fil_flush(id);
    return true;
  };
  if (!flush()) return;

  const int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) return;
  struct stat st;
  const bool have_size = fstat(fd, &st) == 0;
  /* Contiguous enough: at most one extent per MiB (at least 64). */
  const long limit = std::max<long>(64, have_size ? st.st_size >> 20 : 0);
  const long extents = have_size ? wt_file_extents(fd, st.st_size, limit) : -1;
  close(fd);
  if (extents <= limit) return;

  struct statvfs vfs;
  if (statvfs(dir.c_str(), &vfs) != 0 ||
      static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize <
          static_cast<uint64_t>(st.st_size) + (1ULL << 30)) {
    ib::warn() << "Worktree compaction of " << path
               << " skipped: not enough free space";
    return;
  }

  for (int attempt = 1; attempt <= 3; ++attempt) {
    if (attempt > 1 && !flush()) return;
    int64_t mc1, mc;
    page_no_t size1, size;
    fil_wt_counters(id, &mc1, &size1);
    unlink(tmp.c_str());
    if (!wt_copy_file(path, tmp)) {
      ib::warn() << "Worktree compaction of " << path << " skipped: copy failed: "
                 << strerror(errno);
      unlink(tmp.c_str());
      return;
    }
    if (!fil_wt_gate_close(id, std::chrono::seconds(10), &mc, &size)) {
      fil_wt_gate_open(id);
      unlink(tmp.c_str());
      continue;
    }
    if (space->wt_n_dirty.load() != 0 || mc != mc1 || size != size1 ||
        !fil_wt_flush_gated(id)) {
      fil_wt_gate_open(id);
      unlink(tmp.c_str());
      continue;
    }
    DBUG_EXECUTE_IF("wt_crash_compact_before_rename", DBUG_SUICIDE(););
    if (rename(tmp.c_str(), path.c_str()) != 0) {
      fil_wt_gate_open(id);
      unlink(tmp.c_str());
      ib::warn() << "Worktree compaction of " << path << " skipped: rename: "
                 << strerror(errno);
      return;
    }
    if (sync_dir(dir.c_str()) != 0)
      ib::fatal(UT_LOCATION_HERE) << "fsync of " << dir << " failed in a compaction";
    DBUG_EXECUTE_IF("wt_crash_compact_after_rename", DBUG_SUICIDE(););
    /* The old inode must get no more writes: they would be lost. */
    if (!fil_wt_reopen_same_gated(id))
      ib::fatal(UT_LOCATION_HERE) << "Cannot reopen " << path << " after compaction";
    fil_wt_gate_open(id);
    ib::info() << "Worktree compaction of " << path << ": more than " << limit
               << " extents, rewritten contiguously";
    return;
  }
  ib::warn() << "Worktree compaction of " << path << " skipped: the file kept changing";
}

/** Read two header pages of a file, page 1 through the tablespace key. */
bool read_header_pages(const std::string &path, const page_size_t &page_size,
                       const Encryption_metadata &enc, byte *pages) {
  const int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) return wt_os_error("open", path);
  auto guard = create_scope_guard([fd]() { close(fd); });
  pfs_os_file_t file;
#ifdef UNIV_PFS_IO
  file.m_psi = nullptr;
#endif
  file.m_file = fd;
  const ulint len = page_size.physical();
  IORequest read1(IORequest::READ);
  if (enc.can_encrypt()) read1.get_encryption_info().set(enc);
  if (pread(fd, pages, len, 0) != static_cast<ssize_t>(len) ||
      os_file_read(read1, path.c_str(), file, pages + len, len, len) !=
          DB_SUCCESS)
    return wt_os_error("read header pages of", path);
  return false;
}

/** The reuse rule (v3 §7.1): an overlay with nothing local but pages 0 and 1
and nothing dirty can be linked through its own parent without a freeze. */
bool overlay_reusable(fil_space_t *space) {
  wt_overlay_t *ov = space->wt_ov;
  if (ov == nullptr || space->size != ov->link.parent_size) return false;
  /* Frozen and unwritten since needs no hole scan (APFS reports none). */
  if (!ov->fresh.load()) {
    const std::lock_guard<std::mutex> guard(ov->presence_mutex);
    for (size_t p = 2; p < ov->presence.size(); ++p) {
      if (ov->presence[p] != WT_ABSENT) return false;
    }
  }
  ulint dirty = 0;
  for (ulint i = 0; i < srv_buf_pool_instances; ++i) {
    dirty += buf_pool_get_dirty_pages_count(buf_pool_from_array(i), space->id,
                                            nullptr);
  }
  return dirty == 0;
}

/** Freeze a writable source (v3 §7.1 F0-F6, addendum A3): its current file
becomes the immutable layer U, and the source continues, under the same space
id, as an overlay over U. On success c->link is what a worktree of it links. */
#ifdef UNIV_DEBUG
/** Dirty page n of a space with a logged no-op write (fault injection). */
static void wt_debug_dirty(fil_space_t *space, page_no_t n) {
  const page_size_t page_size(space->flags);
  mtr_t mtr;
  mtr.start();
  buf_block_t *b = buf_page_get(page_id_t(space->id, n), page_size, RW_X_LATCH,
                                UT_LOCATION_HERE, &mtr);
  byte *field = b->frame + FIL_PAGE_TYPE;
  mlog_write_ulint(field, mach_read_from_2(field), MLOG_2BYTES, &mtr);
  mtr.commit();
  log_buffer_flush_to_disk(); /* durable: recovery must replay it */
}
#endif /* UNIV_DEBUG */

bool overlay_freeze(THD *thd, trx_t *trx, Space_clone *c, bool force = false) {
  const space_id_t s = c->src->space;
  fil_space_t *space = fil_space_acquire(s);
  auto release = create_scope_guard([space]() { fil_space_release(space); });
  const page_size_t page_size(space->flags);
  const ulint len = page_size.physical();
  const Encryption_metadata enc = space->m_encryption_metadata;

  if (!force && !space->wt_linked && overlay_reusable(space)) {
    c->link = space->wt_ov->link;
    return false;
  }

  for (int attempt = 1;; ++attempt) {
    std::unique_lock<std::mutex> layer_lock(wt_layer_mutex);
    /* F0 */
    if (srv_thread_is_active(srv_threads.m_trx_recovery_rollback))
      return wt_error(
          "CREATE WORKTREE in overlay mode while recovered transactions are "
          "being rolled back");
    ib::info() << "Worktree freeze of " << space->name << " begins";
    space->wt_freezing.store(true);
    auto unfreeze = create_scope_guard([space]() {
      space->wt_freezing.store(false);
    });

    /* F1: every page with modification LSN <= F that was dirty is written. */
    buf_LRU_flush_or_remove_pages(s, BUF_REMOVE_FLUSH_WRITE, trx);
    fil_flush(s);
    int64_t mc1;
    page_no_t size1;
    fil_wt_counters(s, &mc1, &size1);
    const lsn_t F = log_get_lsn(*log_sys);
    DBUG_EXECUTE_IF("wt_crash_freeze_after_flush", DBUG_SUICIDE(););
    DEBUG_SYNC(thd, "wt_freeze_after_flush");
#ifdef UNIV_DEBUG
    /* v3 §11.22: what background work may do to the source between F1 and
    F4 (CREATE WORKTREE's MDL keeps SQL writes out). First attempt only. */
    if (attempt == 1) {
      DBUG_EXECUTE_IF("wt_freeze_inject_dirty", wt_debug_dirty(space, 0););
      DBUG_EXECUTE_IF("wt_freeze_inject_write", {
        wt_debug_dirty(space, 0);
        buf_LRU_flush_or_remove_pages(s, BUF_REMOVE_FLUSH_WRITE, trx);
      });
      DBUG_EXECUTE_IF("wt_freeze_inject_extend",
                      (void)fil_space_extend(space, size1 + 64););
      DBUG_EXECUTE_IF("wt_freeze_inject_commit_ref",
                      space->wt_commit_refs.fetch_add(1););
    }
#endif /* UNIV_DEBUG */

    /* F2 */
    const std::string uuid = wt_new_uuid();
    const std::string layer = wt_layer_path(uuid);
    if (!layer_dir_ready()) return wt_os_error("create", WT_LAYER_DIR);
    if (link(c->src_path.c_str(), layer.c_str()) != 0)
      return wt_os_error("hard-link the source as layer", layer);
    wt_layer_names.fetch_add(1);
    if (sync_dir(WT_LAYER_DIR) != 0) return wt_os_error("fsync", WT_LAYER_DIR);
    DBUG_EXECUTE_IF("wt_crash_freeze_after_link", DBUG_SUICIDE(););

    /* F3: the new file: pages 0 and 1 (the witness), linking U. */
    wt_link_t new_link;
    snprintf(new_link.uuid, sizeof(new_link.uuid), "%s", uuid.c_str());
    new_link.parent_space = s;
    new_link.parent_size = size1;
    new_link.freeze_lsn = F;
    wt_map_t map;
    if (const wt_map_t *old = fil_wt_map(space); old != nullptr) map = *old;
    map.self = s;
    if (map.generation == 0) map.generation = 1;
    map.has_link = true;
    map.link = new_link;

    const std::string tmp = c->src_path + ".wttmp";
    std::vector<byte> buf(3 * len + len);
    byte *pages = reinterpret_cast<byte *>(ut_align(buf.data(), len));
    if (read_header_pages(c->src_path, page_size, enc, pages)) return true;
    byte *page0 = pages, *page1 = pages + len;
    std::vector<byte> old0(page0, page0 + len), old1(page1, page1 + len);
    /* An old overflow map stays where it is (now in U), read through it. */
    ulint stub_len = 0;
    std::vector<page_no_t> stub0, stub1;
    /* Page 1 carries a copy (the witness) only where it has a map area. */
    const bool p1 = wt_map_size(page_size, 1) > 0;
    bool ok = wt_map_write(map, page0, page_size, 0) &&
              (!p1 || wt_map_write(map, page1, page_size, 1));
    if (!ok) {
      ok = wt_overflow_pages_of(old0.data(), page_size, 0, &stub_len, &stub0) &&
           wt_stub_write(map, stub_len, stub0, page0, page_size, 0) &&
           (!p1 ||
            (wt_overflow_pages_of(old1.data(), page_size, 1, &stub_len, &stub1) &&
             wt_stub_write(map, stub_len, stub1, page1, page_size, 1)));
    }
    if (!ok && p1) {
      /* The map was inline: keep it inline in page 1 (larger), with a stub
      in page 0 that carries the link. */
      ok = wt_map_write(map, page1, page_size, 1) &&
           wt_stub_write(map, 0, {}, page0, page_size, 0);
    }
    if (!ok)
      return wt_error(
          "CREATE WORKTREE: the source's map and link do not fit its page size");
    mach_write_to_4(page0 + FSP_HEADER_OFFSET + FSP_SPACE_FLAGS,
                    fsp_header_get_flags(page0) | FSP_FLAGS_MASK_WT_OVERLAY);
    wt_page_recalc_checksum(page0, page_size);
    wt_page_recalc_checksum(page1, page_size);
    {
      const int fd = open(tmp.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0640);
      if (fd < 0) return wt_os_error("create", tmp);
      auto guard = create_scope_guard([fd]() { close(fd); });
      pfs_os_file_t file;
#ifdef UNIV_PFS_IO
      file.m_psi = nullptr;
#endif
      file.m_file = fd;
      IORequest write1(IORequest::WRITE);
      if (enc.can_encrypt()) write1.get_encryption_info().set(enc);
      if (pwrite(fd, page0, len, 0) != static_cast<ssize_t>(len) ||
          os_file_write(write1, tmp.c_str(), file, page1, len, len) !=
              DB_SUCCESS ||
          ftruncate(fd, os_offset_t{size1} * len) != 0 || fsync(fd) != 0)
        return wt_os_error("write", tmp);
    }
    DBUG_EXECUTE_IF("wt_crash_freeze_after_tmp", DBUG_SUICIDE(););

    /* F4 (a): pages 0 and 1 resident (buffer-fixed, not latched) before the
    gate closes. They are X-latched only once I/O has drained: a read
    completing meanwhile may merge the change buffer, which latches page 1
    (the bitmap) in the I/O thread and holds up every completion queued
    behind it (v3 §11.18 (d)). Changes to them before the latch are dirty
    pages, which (d) catches. */
    mtr_t mtr;
    mtr.start();
    mtr_x_lock_space(space, &mtr);
    buf_block_t *b0 = buf_page_get(page_id_t(s, 0), page_size, RW_NO_LATCH,
                                   UT_LOCATION_HERE, &mtr);
    buf_block_t *b1 = buf_page_get(page_id_t(s, 1), page_size, RW_NO_LATCH,
                                   UT_LOCATION_HERE, &mtr);
    const auto abort = [&](const char *why) {
      ib::info() << "Worktree freeze of " << space->name << " aborted (" << why
                 << "), attempt " << attempt;
      fil_wt_gate_open(s);
      mtr.commit();
      DBUG_EXECUTE_IF("wt_crash_freeze_in_abort", DBUG_SUICIDE(););
      unlink(layer.c_str());
      wt_layer_names.fetch_sub(1);
      (void)sync_dir(WT_LAYER_DIR);
      unlink(tmp.c_str());
    };
    /* (b) */
    int64_t mc;
    page_no_t size;
    bool clean = fil_wt_gate_close(s, std::chrono::milliseconds(1000), &mc, &size);
    /* (a') the latches, briefly retried: a holder that needs I/O meanwhile
    cannot let go while the gate is closed. */
    const auto x_latch = [&](buf_block_t *b) {
      const auto until =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
      while (!rw_lock_x_lock_nowait(&b->lock, UT_LOCATION_HERE)) {
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
      buf_block_buf_fix_inc(b, UT_LOCATION_HERE);
      mtr_memo_push(&mtr, b, MTR_MEMO_PAGE_X_FIX);
      return true;
    };
    clean = clean && x_latch(b0) && x_latch(b1);
    /* (c) mtrs past the hook but not yet on the flush list */
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (clean && space->wt_commit_refs.load() != 0) {
      if (std::chrono::steady_clock::now() > deadline) clean = false;
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    DBUG_EXECUTE_IF("wt_freeze_inject_commit_ref", {
      if (attempt == 1) space->wt_commit_refs.fetch_sub(1);
    });
    DBUG_EXECUTE_IF("wt_freeze_force_abort", {
      static int forced = 0;
      if (forced++ == 0) clean = false;
    });
    /* (d) */
    ulint dirty = 0;
    for (ulint i = 0; clean && i < srv_buf_pool_instances; ++i) {
      dirty += buf_pool_get_dirty_pages_count(buf_pool_from_array(i), s,
                                              nullptr);
    }
    clean = clean && dirty == 0 && mc == mc1 && size == size1;
    /* (e) */
    clean = clean && fil_wt_flush_gated(s);
    if (!clean) {
      /* (f) */
      abort("the source changed or was busy");
      if (attempt < 3) continue;
      return wt_error("CREATE WORKTREE: the source stayed busy during its freeze");
    }
    DBUG_EXECUTE_IF("wt_crash_freeze_in_gate", DBUG_SUICIDE(););
    /* (g) the commit point */
    if (rename(tmp.c_str(), c->src_path.c_str()) != 0) {
      abort("rename failed");
      return wt_os_error("rename", tmp);
    }
    const std::string dir = c->src_path.substr(0, c->src_path.rfind('/'));
    if (sync_dir(dir.c_str()) != 0)
      ib::fatal(UT_LOCATION_HERE) << "fsync of " << dir << " failed in a freeze";
    DBUG_EXECUTE_IF("wt_crash_freeze_after_rename", DBUG_SUICIDE(););

    /* F5: the source is now an overlay over U. */
    if (!fil_wt_reopen_gated(s, new_link))
      ib::fatal(UT_LOCATION_HERE)
          << "Cannot reopen " << c->src_path << " after its freeze";
    fil_wt_set(space, map);
    DEBUG_SYNC(thd, "wt_freeze_after_reopen");
    /* A read-only source linked live is now a new inode: no longer a layer. */
    space->wt_linked = false;
    for (auto [block, page] : {std::pair{b0, page0}, std::pair{b1, page1}}) {
      memcpy(block->frame, page, len);
      if (page_size.is_compressed()) memcpy(block->page.zip.data, page, len);
    }
    if (chmod(layer.c_str(), 0444) != 0)
      ib::warn() << "chmod 0444 " << layer << ": " << strerror(errno);

    /* F6 (the gate opens while still freezing: a page dirtied meanwhile is
    imaged, which is merely conservative) */
    fil_wt_gate_open(s);
    /* v3 §6.4 row 11: a page dirtied after the F4 check, before F6. */
    DBUG_EXECUTE_IF("wt_freeze_inject_dirty_late", wt_debug_dirty(space, 3););
    unfreeze.reset();
    space->wt_freezing.store(false);
    mtr.commit();
    layer_lock.unlock();
    DBUG_EXECUTE_IF("wt_crash_freeze_after_open", DBUG_SUICIDE(););
    ib::info() << "Worktree freeze of " << space->name << " done";
    c->link = new_link;
    return false;
  }
}

/** Flatten an overlay (limits/overlay-v3.md §8): adopt every page it still
reads from its chain, then drop the link, leaving a standalone stock-format
file. The caller holds MDL >= SHARED_READ on the table. */
bool overlay_flatten(THD *thd, trx_t *trx, dict_table_t *table) {
  const space_id_t s = table->space;
  fil_space_t *space = fil_space_acquire(s);
  auto release = create_scope_guard([space]() { fil_space_release(space); });
  if (space->wt_ov == nullptr) return false;
  if (wt_op_acquire(thd, space, 2)) return true;
  auto slot = create_scope_guard([space]() { wt_op_release(space); });
  ib::info() << "Worktree flatten of " << space->name << " begins";
  DBUG_EXECUTE_IF("wt_crash_flatten_begin", DBUG_SUICIDE(););
  const page_size_t page_size(space->flags);
  const std::string gen_uuid = space->wt_ov->link.uuid;
  const uint64_t gen = space->wt_gen;
  const page_no_t parent_size = space->wt_ov->link.parent_size;

  /* Step 1: adopt-only mtrs, each logging the page's full image. */
  std::vector<page_no_t> need;
  for (page_no_t n = 1; n < parent_size; ++n) {
    if (space->wt_ov->get(n) == WT_PRESENT) continue;
    log_free_check();
    mtr_t mtr;
    mtr.start();
    /* Every page of the range, free ones too (their content is whatever the
    layer holds; a page reused later is initialised). */
    buf_block_t *block =
        buf_page_get_gen(page_id_t(s, n), page_size, RW_X_LATCH, nullptr,
                         Page_fetch::POSSIBLY_FREED, UT_LOCATION_HERE, &mtr);
    /* The crash-loop state: bytes in zip.data that the frame lacks. */
    DBUG_EXECUTE_IF("wt_flatten_stale_zip", {
      if (page_size.is_compressed() &&
          page_is_uncompressed_type(block->page.zip.data))
        block->page.zip.data[FIL_PAGE_LSN + 7] |= 1;
    });
    DBUG_EXECUTE_IF("wt_flatten_stale_zip_index", {
      if (page_size.is_compressed() &&
          fil_page_get_type(block->frame) == FIL_PAGE_TYPE_ALLOCATED &&
          mach_read_from_4(block->frame + FIL_PAGE_OFFSET) == 0)
        mach_write_to_2(block->page.zip.data + FIL_PAGE_TYPE, FIL_PAGE_INDEX);
    });
    /* Test the bytes wt_mtr_log_image() logs: for an uncompressed-type page
    of a compressed space the frame is authoritative (flush types the page by
    its frame and copies it over zip.data), and zip.data may hold stale bytes,
    type included, over a zero frame. */
    const bool zip = page_size.is_compressed() &&
                     !page_is_uncompressed_type(block->frame);
    const byte *bytes = zip ? block->page.zip.data : block->frame;
    const ulint end = page_size.physical() - (zip ? 0 : FIL_PAGE_DATA_END);
    bool zero = true;
    for (ulint i = FIL_PAGE_OFFSET; zero && i < end; ++i)
      zero = bytes[i] == 0;
    if (!zero) {
      fil_wt_set_present(space, n);
      wt_mtr_log_image(&mtr, block);
      need.push_back(n);
    }
    mtr.commit();
    DBUG_EXECUTE_IF("wt_crash_flatten_adopt", {
      if (n > parent_size / 2) DBUG_SUICIDE();
    });
  }
  /* Step 2: the adopted pages are durable locally. */
  buf_LRU_flush_or_remove_pages(s, BUF_REMOVE_FLUSH_WRITE, trx);
  fil_flush(s);
  DBUG_EXECUTE_IF("wt_crash_flatten_after_flush", DBUG_SUICIDE(););
  /* Step 3: exhaustive check. */
  for (page_no_t n : need) {
    if (space->wt_ov->get(n) != WT_PRESENT)
      return wt_error("CREATE WORKTREE: flatten lost a page; retry");
  }
  DBUG_EXECUTE_IF("wt_crash_flatten_after_check", DBUG_SUICIDE(););
  DEBUG_SYNC(thd, "wt_flatten_after_check");
  DBUG_EXECUTE_IF("wt_flatten_bump_gen", ++space->wt_gen;);
  /* Step 4: generation check, then clear the link and the map (logged). */
  {
    mtr_t mtr;
    mtr.start();
    mtr_x_lock_space(space, &mtr);
    buf_block_t *b0 = buf_page_get(page_id_t(s, 0), page_size, RW_X_LATCH,
                                   UT_LOCATION_HERE, &mtr);
    buf_block_t *b1 = buf_page_get(page_id_t(s, 1), page_size, RW_X_LATCH,
                                   UT_LOCATION_HERE, &mtr);
    if (space->wt_gen != gen || gen_uuid != space->wt_ov->link.uuid) {
      mtr.commit();
      return wt_error("CREATE WORKTREE: flatten raced a freeze (backstop)");
    }
    /* The map loses its link but keeps the ancestors: records still carry
    their ids (LOB references). A file that was never a worktree has none. */
    const ulint len = page_size.physical();
    std::vector<byte> p0(b0->frame, b0->frame + len), p1(b1->frame, b1->frame + len);
    for (auto [p, no] : {std::pair{p0.data(), page_no_t{0}}, std::pair{p1.data(), page_no_t{1}}})
      memset(p + wt_map_offset(page_size, no), 0, wt_map_size(page_size, no));
    wt_map_t map;
    if (const wt_map_t *cur = fil_wt_map(space); cur != nullptr) map = *cur;
    map.has_link = false;
    map.link = wt_link_t{};
    if (!map.ancestors.empty()) {
      ulint stub_len = 0;
      std::vector<page_no_t> stub0, stub1;
      bool ok = wt_map_write(map, p0.data(), page_size, 0);
      if (ok) {
        (void)wt_map_write(map, p1.data(), page_size, 1);
      } else if (wt_overflow_pages_of(b0->frame, page_size, 0, &stub_len, &stub0)) {
        ok = wt_stub_write(map, stub_len, stub0, p0.data(), page_size, 0) &&
             (!wt_overflow_pages_of(b1->frame, page_size, 1, &stub_len, &stub1) ||
              wt_stub_write(map, stub_len, stub1, p1.data(), page_size, 1));
      } else {
        ok = wt_map_write(map, p1.data(), page_size, 1) &&
             wt_stub_write(map, 0, {}, p0.data(), page_size, 0);
      }
      if (!ok) {
        mtr.commit();
        return wt_error("ALTER WORKTREE MATERIALIZE: the map does not fit");
      }
    }
    for (auto [block, p, no] : {std::tuple{b0, p0.data(), page_no_t{0}},
                                std::tuple{b1, p1.data(), page_no_t{1}}}) {
      const ulint off = wt_map_offset(page_size, no);
      if (wt_map_size(page_size, no) > 0)
        mlog_write_string(block->frame + off, p + off, wt_map_size(page_size, no), &mtr);
    }
    mlog_write_ulint(b0->frame + FSP_HEADER_OFFSET + FSP_SPACE_FLAGS,
                     fsp_header_get_flags(b0->frame) & ~FSP_FLAGS_MASK_WT_OVERLAY,
                     MLOG_4BYTES, &mtr);
    fil_wt_clear_overlay_flag(s);
    mtr.commit();
  }
  DBUG_EXECUTE_IF("wt_crash_flatten_after_clear", DBUG_SUICIDE(););
  /* Step 5: durable, then the space stands alone. */
  buf_LRU_flush_or_remove_pages(s, BUF_REMOVE_FLUSH_WRITE, trx);
  fil_flush(s);
  DBUG_EXECUTE_IF("wt_crash_flatten_after_final_flush", DBUG_SUICIDE(););
  fil_wt_detach(s);
  ib::info() << "Worktree flatten of " << space->name << " done";
  return false;
}

std::vector<std::string> fts_aux_names(
    const dict_table_t *table, const char *parent, table_id_t table_id,
    const std::vector<space_index_t> &fts_index_ids);

/** Every InnoDB table behind a DD table: the table or its leaf partitions,
and FTS auxiliary tables. Opened tables are appended to opened. */
bool wt_tables_of(THD *thd, const dd::Table &dd_table,
                  std::vector<dict_table_t *> *opened,
                  std::vector<MDL_ticket *> *mdls) {
  const auto open_id = [&](dd::Object_id id) {
    /* Its own MDL (FTS aux and partitions have none from the statement),
    released by the caller after closing. */
    MDL_ticket *mdl = nullptr;
    dict_table_t *t = dd_table_open_on_id(id, thd, &mdl, false, true);
    if (mdl != nullptr) mdls->push_back(mdl);
    if (t != nullptr) opened->push_back(t);
    return t;
  };
  if (dd_table.partition_type() != dd::Table::PT_NONE) {
    for (const dd::Partition *p : dd_table.leaf_partitions()) {
      if (open_id(p->se_private_id()) == nullptr) return true;
    }
    return false;
  }
  dict_table_t *t = open_id(dd_table.se_private_id());
  if (t == nullptr) return true;
  if (t->fts != nullptr && dict_table_has_fts_index(t)) {
    std::vector<space_index_t> ids;
    for (const dict_index_t *index = t->first_index(); index != nullptr;
         index = index->next()) {
      if (index->type & DICT_FTS) ids.push_back(index->id);
    }
    for (const auto &name : fts_aux_names(t, t->name.m_name, t->id, ids)) {
      MDL_ticket *mdl = nullptr;
      dict_table_t *aux = dd_table_open_on_name(thd, &mdl, name.c_str(),
                                                false, DICT_ERR_IGNORE_NONE);
      if (mdl != nullptr) mdls->push_back(mdl);
      if (aux != nullptr) opened->push_back(aux);
    }
  }
  return false;
}

/** Overlay mode (addendum A2): create the clone as a sparse file of the
source's size holding only pages 0 and 1 (rewritten in phase 3). A read-only
source's live file is hard-linked into #wtlayers as the parent layer; a
writable source was frozen first and c->link already names its layer. */
bool overlay_snapshot(Space_clone *c, bool live) {
  const int src = open(c->src_path.c_str(), O_RDONLY);
  if (src < 0) return wt_os_error("open", c->src_path);
  auto src_guard = create_scope_guard([src]() { close(src); });
  struct stat st;
  if (fstat(src, &st) != 0) return wt_os_error("stat", c->src_path);
  const page_size_t page_size(c->src->flags);
  const ulint len = page_size.physical();
  ut_a(st.st_size % len == 0);

  const std::string uuid = live ? wt_new_uuid() : std::string(c->link.uuid);
  if (live) {
    const std::string layer = wt_layer_path(uuid);
    if (!layer_dir_ready()) return wt_os_error("create", WT_LAYER_DIR);
    /* Until the clone's header pages name it (publish_clone), nothing on
    disk does: a GC run by a concurrent DROP would delete it. */
    {
      const std::lock_guard<std::mutex> guard(wt_layer_mutex);
      wt_pinned_layers.insert(uuid);
    }
    c->pinned = uuid;
    if (link(c->src_path.c_str(), layer.c_str()) != 0)
      return wt_os_error("hard-link the source as layer", layer);
    wt_layer_names.fetch_add(1);
    /* Made durable once for all tables before the commit (sql_worktree). */
    DBUG_EXECUTE_IF("wt_crash_after_layer_link", DBUG_SUICIDE(););
    DEBUG_SYNC(current_thd, "wt_after_layer_link");
  }

  const int dst = open(c->tmp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0640);
  if (dst < 0) return wt_os_error("create", c->tmp_path);
  auto dst_guard = create_scope_guard([dst]() { close(dst); });
  std::vector<byte> pages(2 * len);
  if (pread(src, pages.data(), 2 * len, 0) != static_cast<ssize_t>(2 * len) ||
      pwrite(dst, pages.data(), 2 * len, 0) != static_cast<ssize_t>(2 * len) ||
      ftruncate(dst, st.st_size) != 0)
    return wt_os_error("write", c->tmp_path);

  if (live) {
    snprintf(c->link.uuid, sizeof(c->link.uuid), "%s", uuid.c_str());
    c->link.parent_space = c->src->space;
    c->link.parent_size = static_cast<page_no_t>(st.st_size / len);
    /* The source is flushed: every page in its file has LSN <= F. */
    c->link.freeze_lsn = log_get_lsn(*log_sys);
  }
  return false;
}

/** Phase 2 (quiesced): merge the change buffer and flush the source so its
file is a complete image, then reflink it under the temporary name. */
bool snapshot_clone(THD *thd, trx_t *trx, Space_clone *c) {
  const space_id_t space = c->src->space;
  while (ibuf_merge_space(space) != 0) {
  }
  if (wt_flush_space(space, trx)) return true;

  /* The SDI root also has segment headers carrying the space id. */
  mtr_t mtr;
  mtr_start(&mtr);
  c->roots.push_back(
      fsp_sdi_get_root_page_num(space, page_size_t(c->src->flags), &mtr));
  mtr_commit(&mtr);

  if (c->overlay) {
    /* Depth cap (addendum A5): a source whose chain or ancestor list is
    full is flattened first, so its clone stays within the cap. */
    {
      fil_space_t *src = fil_space_acquire(c->src->space);
      const bool full =
          src->wt_ov != nullptr &&
          (src->wt_ov->chain.size() + 1 >= WT_MAX_ANCESTORS ||
           (src->wt_map != nullptr &&
            src->wt_map->ancestors.size() + 1 >= WT_MAX_ANCESTORS));
      fil_space_release(src);
      if (full && overlay_flatten(thd, trx, c->src)) return true;
    }
    /* The slot is held through phase 3 (v3 I8: no flatten meanwhile).
    Creates linking a read-only source live share it: each links its own
    layer name and changes nothing they share. Taking turns per table made
    50 concurrent creates from one golden take minutes on ext4. */
    fil_space_t *src = fil_space_acquire(c->src->space);
    const bool busy = wt_op_acquire(thd, src, 1, c->src_read_only);
    if (!busy) {
      c->slot = src;
      c->slot_shared = c->src_read_only;
    }
    fil_space_release(src);
    if (busy) return true;
    if (c->src_read_only) return overlay_snapshot(c, true);
    return overlay_freeze(thd, trx, c) || overlay_snapshot(c, false);
  }
  if (reflink(c->src_path.c_str(), c->tmp_path.c_str()) != 0)
    return wt_reflink_error(c->tmp_path);
  /* Its header pages, read now (still quiesced) from the source, whose
  pages are cached; read back from the new clone, a fresh inode, they would
  miss the cache. */
  fil_space_t *src = fil_space_acquire(space);
  const page_size_t page_size(src->flags);
  const Encryption_metadata enc = src->m_encryption_metadata;
  fil_space_release(src);
  const ulint len = page_size.physical();
  auto pages = static_cast<byte *>(ut::aligned_alloc(2 * len, len));
  auto free_guard = create_scope_guard([pages]() { ut::aligned_free(pages); });
  if (read_header_pages(c->src_path, page_size, enc, pages)) return true;
  c->head.assign(pages, pages + 2 * len);
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
  map->has_link = c.overlay;
  map->link = c.link;
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
      create_scope_guard([&]() { (void)fil_close_tablespace(c->space_id); });

  /* An overlay attaches its chain at the first open, from the link-only map
  in its header pages; the full map then replaces that one. */
  if (c->overlay && !fil_space_open(c->space_id)) {
    my_error(ER_TABLESPACE_MISSING, MYF(0), c->tmp_path.c_str());
    return true;
  }
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
  for (page_no_t no : {0, 1}) {
    auto &copy = copies[no];
    /* (No copy for a page 1 without a map area.) */
    for (ulint i = 0; ok && wt_map_size(page_size, no) > 0 && i < n; ++i) {
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
    if (wt_map_size(page_size, no) == 0) continue;
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
  /* Overflow pages written in the buffer pool only, not flushed. */
  DBUG_EXECUTE_IF("wt_crash_in_overflow", DBUG_SUICIDE(););
  fil_space_release(space);
  if (!ok) return wt_error("CREATE WORKTREE: index count too large for the page size");

  return wt_flush_space(c->space_id, trx);
}

bool publish_clone(THD *thd, trx_t *trx, dd::Object_id src_dd_table_id,
                   dd::Object_id src_dd_space_id, Space_clone *c) {
  fil_space_t *src_space = fil_space_acquire(c->src->space);
  const uint32_t fsp_flags =
      c->overlay ? src_space->flags | FSP_FLAGS_MASK_WT_OVERLAY
                 : src_space->flags & ~FSP_FLAGS_MASK_WT_OVERLAY;
  const Encryption_metadata enc = src_space->m_encryption_metadata;
  wt_map_t map;
  const bool too_long = build_map(*c, fil_wt_map(src_space), &map);
  fil_space_release(src_space);
  if (too_long) return true;
  const page_size_t page_size(fsp_flags);

  bool overflow;
  if (rewrite_header_pages(c->tmp_path, c->space_id, c->src->space, page_size,
                           enc, map, c->head, &overflow))
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
  /* The schema directory is fsynced once, before the commit (sql_worktree). */
  if (rename(c->tmp_path.c_str(), c->path.c_str()) != 0)
    return wt_os_error("rename", c->tmp_path);
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

  /* A read-only source's live file is now a layer: never written again. */
  if (c->overlay && c->src_read_only) {
    fil_space_t *src_space = fil_space_acquire(c->src->space);
    src_space->wt_linked = true;
    fil_space_release(src_space);
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

namespace {
/** Overlay preconditions (limits/overlay-v3.md §5.4, §5.2 O5). */
/** The v3 §5.4 configuration an overlay's crash safety rests on. At startup
(layers exist) the redo margin and the redo state are not known yet: they are
checked at CREATE WORKTREE and by their own SET guards.
@return why the configuration is unsafe, or nullptr */
const char *overlay_config_problem(bool at_startup) {
  if (srv_thread_concurrency == 0)
    return "innodb_thread_concurrency=0 (set it > 0: the redo margin that "
           "bounds first-dirty images needs it)";
  if (!at_startup && !log_sys->concurrency_margin_is_safe.load())
    return "an unsafe redo concurrency margin (raise innodb_redo_log_capacity "
           "or lower innodb_thread_concurrency)";
  if (dblwr::g_mode != dblwr::Mode::ON && dblwr::g_mode != dblwr::Mode::TRUEE &&
      dblwr::g_mode != dblwr::Mode::DETECT_AND_RECOVER)
    return "no full doublewrite buffer (innodb_doublewrite must be ON or "
           "DETECT_AND_RECOVER)";
  if (srv_checksum_algorithm == SRV_CHECKSUM_ALGORITHM_NONE ||
      srv_checksum_algorithm == SRV_CHECKSUM_ALGORITHM_STRICT_NONE)
    return "innodb_checksum_algorithm=none";
  if (srv_unix_file_flush_method == SRV_UNIX_NOSYNC ||
      srv_unix_file_flush_method == SRV_UNIX_O_DIRECT_NO_FSYNC ||
      srv_unix_file_flush_method == SRV_UNIX_LITTLESYNC)
    return "an innodb_flush_method that does not fsync data files";
  if (!at_startup && !mtr_t::s_logging.is_enabled()) return "redo disabled";
  if (UNIV_PAGE_SIZE > UNIV_PAGE_SIZE_DEF) return "32 or 64 KB pages";
  return nullptr;
}

bool overlay_check_config() {
  if (const char *why = overlay_config_problem(false)) {
    const std::string what = std::string("CREATE WORKTREE in overlay mode with ") + why;
    return wt_error(what.c_str());
  }
  return false;
}

/** Wait until purge has processed every transaction numbered below target,
so that it never writes a source that is about to be linked live (addendum
A2). @return true on timeout (error reported) */
/** Read-only schemas whose history purge has drained since they became
read-only (in memory: a restart forgets them). */
std::mutex wt_purged_mutex;
std::set<dd::Object_id> wt_purged_schemas;
/** Reflink mode: for a read-only schema, the transaction number its first
CREATE WORKTREE saw; all of its history is older. */
std::map<dd::Object_id, trx_id_t> wt_purge_targets;

bool wt_schema_purged(dd::Object_id schema_id) {
  const std::lock_guard<std::mutex> guard(wt_purged_mutex);
  return wt_purged_schemas.count(schema_id) != 0;
}

void wt_schema_purged_set(dd::Object_id schema_id, bool purged) {
  const std::lock_guard<std::mutex> guard(wt_purged_mutex);
  if (purged) {
    wt_purged_schemas.insert(schema_id);
  } else {
    wt_purged_schemas.erase(schema_id);
    wt_purge_targets.erase(schema_id);
  }
}

/** What purge has applied, as purge_sys->done records it in debug builds
(limit, or iter while limit is unset), read with no batch in flight:
stopping purge waits for the running one. */
trx_id_t wt_purge_done() {
  trx_purge_stop();
  rw_lock_s_lock(&purge_sys->latch, UT_LOCATION_HERE);
  const trx_id_t done = purge_sys->limit.trx_no != 0 ? purge_sys->limit.trx_no
                                                     : purge_sys->iter.trx_no;
  rw_lock_s_unlock(&purge_sys->latch);
  trx_purge_run();
  return done;
}

/** Reflink mode: has purge passed every transaction that can have written
the read-only schema? Checked without waiting, once per statement, and
remembered once true (wt_schema_purged). */
bool wt_purged_probe(THD *thd, dd::Object_id schema_id) {
  static thread_local std::pair<const THD *, query_id_t> probed{nullptr, 0};
  if (probed.first == thd && probed.second == thd->query_id) return false;
  probed = {thd, thd->query_id};
  trx_id_t target;
  {
    const std::lock_guard<std::mutex> guard(wt_purged_mutex);
    target = wt_purge_targets.emplace(schema_id, trx_sys_get_next_trx_id_or_no())
                 .first->second;
  }
  if (trx_sys->rseg_history_len.load() != 0 && wt_purge_done() < target)
    return false;
  wt_schema_purged_set(schema_id, true);
  return true;
}

bool overlay_wait_purged(THD *thd, trx_id_t target, bool report = true) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(thd_lock_wait_timeout(thd));
  for (;;) {
    if (trx_sys->rseg_history_len.load() == 0) return false;
    if (wt_purge_done() >= target) return false;
    if (std::chrono::steady_clock::now() > deadline || thd_killed(thd)) {
      if (report) my_error(ER_LOCK_WAIT_TIMEOUT, MYF(0));
      return true;
    }
    srv_purge_wakeup();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

/** Is the schema of src read-only? */
bool schema_read_only(THD *thd, const dd::Table &src) {
  const dd::Schema *schema = nullptr;
  return !dd::get_dd_client(thd)->acquire(src.schema_id(), &schema) &&
         schema != nullptr && schema->read_only();
}
}  // namespace

bool innobase_wt_select_mode() {
  struct stat st;
  const bool have_dir = stat(WT_LAYER_DIR, &st) == 0;
  /* Layer files, not the directory: GC can leave an empty one. */
  bool have_layers = false;
  if (DIR *d = have_dir ? opendir(WT_LAYER_DIR) : nullptr; d != nullptr) {
    for (struct dirent *e; !have_layers && (e = readdir(d)) != nullptr;) {
      const size_t n = strlen(e->d_name);
      have_layers = n > 4 && strcmp(e->d_name + n - 4, ".ibl") == 0;
    }
    closedir(d);
  }
  if (srv_read_only_mode) {
    /* Nothing is created or written; routing works from the files alone. */
    srv_wt_overlay = false;
    return false;
  }
  if (srv_wt_mode == 1 && have_layers) {
    ib::error() << "The data directory has worktree layers (" << WT_LAYER_DIR
                << "/); start with innodb_worktree_mode=AUTO or OVERLAY";
    return true;
  }
  bool overlay = srv_wt_mode == 2 || (srv_wt_mode == 0 && have_layers);
  if (srv_wt_mode == 0 && !overlay) {
    /* The datadir's filesystem: copy-on-write file clones? */
    const char *a = "#wt_probe_startup", *b = "#wt_probe_startup.clone";
    unlink(a);
    unlink(b);
    const int fd = open(a, O_WRONLY | O_CREAT | O_EXCL, 0640);
    bool cloned = false;
    if (fd >= 0) {
      const bool written = write(fd, "x", 1) == 1;
      close(fd);
      cloned = written && reflink(a, b) == 0;
    }
    unlink(a);
    unlink(b);
    overlay = !cloned;
  }
  if (overlay) {
    /* Layers are hard links (v3 §3); #wtlayers waits for the first one. */
    const std::string a = "#wt_probe_link", b = a + ".link";
    unlink(a.c_str());
    unlink(b.c_str());
    const int fd = open(a.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0640);
    const bool linked = fd >= 0 && link(a.c_str(), b.c_str()) == 0;
    if (fd >= 0) close(fd);
    unlink(a.c_str());
    unlink(b.c_str());
    if (!linked) {
      ib::error() << "Worktree overlay mode needs hard links in the data "
                     "directory's filesystem";
      return true;
    }
  }
  if (overlay && have_layers) {
    /* v3 §5.4: overlays exist, so their configuration is checked now. */
    if (const char *why = overlay_config_problem(true)) {
      ib::error() << "The data directory has worktree overlays and the server "
                     "is configured with "
                  << why << "; refusing to start";
      return true;
    }
  }
  srv_wt_overlay = overlay;
  wt_images_margin.store(overlay && have_layers);
  /* innodb_worktree_mode shows the mode chosen (AUTO resolves at startup). */
  srv_wt_mode = overlay ? 2 : 1;
  ib::info() << "CREATE WORKTREE uses "
             << (overlay ? "overlay files over immutable layers"
                         : "copy-on-write file clones");
  return false;
}

bool innobase_clone_table(handlerton *hton, THD *thd, const dd::Table &src_dd,
                          dd::Table *dst_dd, const char *dst_db) {
  DBUG_TRACE;
  if (srv_read_only_mode) return wt_error("CREATE WORKTREE in read-only mode");
  const bool overlay = srv_wt_overlay;
  /* Widen the redo margin before the first overlay exists (as a SET of
  innodb_thread_concurrency does), so the check below sees it. */
  if (overlay && !wt_images_margin.exchange(true))
    log_files_thread_concurrency_updated(*log_sys);
  if (overlay && overlay_check_config()) return true;
  const bool read_only = schema_read_only(thd, src_dd);
  const bool src_read_only = overlay && read_only;

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

  if (!overlay && probe_reflink(thd)) return true;

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
  auto slot_guard = create_scope_guard([&]() {
    /* By now each clone's header names its layer, or it is rolled back. */
    {
      const std::lock_guard<std::mutex> guard(wt_layer_mutex);
      for (const auto &c : clones) wt_pinned_layers.erase(c.pinned);
      for (const auto &a : aux) wt_pinned_layers.erase(a.clone.pinned);
    }
    for (auto &c : clones) {
      if (c.slot != nullptr) wt_op_release(c.slot, c.slot_shared);
    }
    for (auto &a : aux) {
      if (a.clone.slot != nullptr) wt_op_release(a.clone.slot, a.clone.slot_shared);
    }
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

  /* A live link must never be written again: purge must be done with every
  transaction that may still touch the source (addendum A2). */
  for (auto &c : clones) {
    c.overlay = overlay;
    c.src_read_only = src_read_only;
  }
  for (auto &a : aux) {
    a.clone.overlay = overlay;
    a.clone.src_read_only = src_read_only;
  }
  /* A read-only schema gets no new writes: purge of its history is waited
  for once, normally when it became read-only (innobase_worktree_read_only),
  else here, once per statement. */
  if (src_read_only && !wt_schema_purged(src_dd.schema_id())) {
    static thread_local std::pair<const THD *, query_id_t> waited{nullptr, 0};
    if (waited.first != thd || waited.second != thd->query_id) {
      if (overlay_wait_purged(thd, trx_sys_get_next_trx_id_or_no())) return true;
      waited = {thd, thd->query_id};
      wt_schema_purged_set(src_dd.schema_id(), true);
    }
  }

  /* Purge can't write a read-only source whose history it has passed, so it
  needn't be stopped: stopping it for every table waited for purge batches,
  much of a create's time under write load (milestone 5, F3). */
  bool purged = read_only && wt_schema_purged(src_dd.schema_id());
  if (read_only && !purged && !overlay)
    purged = wt_purged_probe(thd, src_dd.schema_id());

  /* Quiesce (§12.2 phase 2): no .cfg/.cfp is written. User writes are blocked
  by MDL; stop purge (instance-wide, reference counted, so keep it short),
  then snapshot every file of this table. */
  {
    const bool purge_stopped =
        !purged && trx_purge_state() != PURGE_STATE_DISABLED;
    if (purge_stopped) trx_purge_stop();
    auto purge_guard = create_scope_guard([&]() {
      if (purge_stopped) trx_purge_run();
    });
    for (auto &c : clones) {
      if (snapshot_clone(thd, trx, &c)) return true;
    }
    for (auto &a : aux) {
      if (snapshot_clone(thd, trx, &a.clone)) return true;
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

void innobase_worktree_read_only(handlerton *, THD *thd,
                                 dd::Object_id schema_id, bool read_only) {
  /* Set only by a wait that succeeded in this read-only period. */
  wt_schema_purged_set(schema_id, false);
  if (!read_only || !srv_wt_overlay) return;
  /* (No error on timeout: the ALTER has committed; CREATE waits instead.) */
  if (!overlay_wait_purged(thd, trx_sys_get_next_trx_id_or_no(), false))
    wt_schema_purged_set(schema_id, true);
}

bool innobase_worktree_refresh(handlerton *, THD *thd, const dd::Table &table) {
  if (!srv_wt_overlay) return false;
  /* Writable from now on: a later read-only period waits for purge again. */
  wt_schema_purged_set(table.schema_id(), false);
  trx_t *trx = check_trx_exists(thd);
  std::vector<dict_table_t *> opened;
  std::vector<MDL_ticket *> mdls;
  auto close = create_scope_guard([&]() {
    for (auto *t : opened) dd_table_close(t, thd, nullptr, false);
    for (auto *m : mdls) dd_mdl_release(thd, &m);
  });
  if (wt_tables_of(thd, table, &opened, &mdls)) return true;
  /* Refresh (addendum A2): every table whose live file is a layer is frozen
  before the schema becomes writable. */
  for (dict_table_t *t : opened) {
    fil_space_t *space = fil_space_acquire(t->space);
    const bool linked = space->wt_linked;
    fil_space_release(space);
    if (!linked) continue;
    Space_clone c;
    c.src = t;
    char *path = fil_space_get_first_path(t->space);
    c.src_path = path;
    ut::free(path);
    fil_space_t *slot_space = fil_space_acquire(t->space);
    if (wt_op_acquire(thd, slot_space, 1)) {
      fil_space_release(slot_space);
      return true;
    }
    ib::info() << "Worktree refresh of " << slot_space->name << " begins";
    const bool failed = overlay_freeze(thd, trx, &c, true);
    DBUG_EXECUTE_IF("wt_crash_refresh_after_freeze", DBUG_SUICIDE(););
    if (!failed) ib::info() << "Worktree refresh of " << slot_space->name << " done";
    wt_op_release(slot_space);
    fil_space_release(slot_space);
    if (failed) return true;
  }
  return false;
}

bool innobase_worktree_materialize(handlerton *, THD *thd,
                                   const dd::Table &table) {
  /* v3 §5.4: read-only and force-recovery starts never flatten. */
  if (srv_read_only_mode || srv_force_recovery > 0)
    return wt_error(
        "ALTER WORKTREE ... MATERIALIZE with innodb_read_only or "
        "innodb_force_recovery");
  trx_t *trx = check_trx_exists(thd);
  std::vector<dict_table_t *> opened;
  std::vector<MDL_ticket *> mdls;
  auto close = create_scope_guard([&]() {
    for (auto *t : opened) dd_table_close(t, thd, nullptr, false);
    for (auto *m : mdls) dd_mdl_release(thd, &m);
  });
  if (wt_tables_of(thd, table, &opened, &mdls)) return true;
  for (dict_table_t *t : opened) {
    if (overlay_flatten(thd, trx, t)) return true;
  }
  return false;
}

bool innobase_worktree_compact(handlerton *, THD *thd, const dd::Table &table) {
  if (srv_read_only_mode || srv_force_recovery > 0) return false;
  trx_t *trx = check_trx_exists(thd);
  std::vector<dict_table_t *> opened;
  std::vector<MDL_ticket *> mdls;
  auto close = create_scope_guard([&]() {
    for (auto *t : opened) dd_table_close(t, thd, nullptr, false);
    for (auto *m : mdls) dd_mdl_release(thd, &m);
  });
  if (wt_tables_of(thd, table, &opened, &mdls)) return true;
  for (dict_table_t *t : opened) {
    if (!dict_table_is_file_per_table(t)) continue;
    if (srv_wt_overlay) {
      /* Overlay files are never shared clones: just write out what the
      refresh left dirty now, not in the first CREATE WORKTREE after it. */
      std::ignore = wt_flush_space(t->space, trx);
    } else {
      wt_compact_space(trx, t->space);
    }
  }
  return false;
}

std::atomic<bool> wt_gc_wanted{false};

namespace {
/** Read pages 0 and 1 of a file (raw) and the page size from its flags. */
bool wt_read_head(const std::string &path, std::vector<byte> *head,
                  page_size_t *size, ino_t *ino) {
  const int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;
  auto guard = create_scope_guard([fd]() { close(fd); });
  struct stat st;
  if (fstat(fd, &st) != 0) return false;
  *ino = st.st_ino;
  head->assign(2 * UNIV_PAGE_SIZE_MAX, 0);
  const ssize_t n = pread(fd, head->data(), head->size(), 0);
  if (n < static_cast<ssize_t>(UNIV_ZIP_SIZE_MIN)) return false;
  const uint32_t flags = fsp_header_get_flags(head->data());
  if (!fsp_flags_is_valid(flags)) return false;
  *size = page_size_t(flags);
  return true;
}

/** The link named by a file's header pages, if it is an overlay. */
enum class Head { PLAIN, LINKED, BAD };
Head wt_head_link(const std::vector<byte> &head, const page_size_t &size,
                  wt_link_t *link) {
  if ((fsp_header_get_flags(head.data()) & FSP_FLAGS_MASK_WT_OVERLAY) == 0)
    return Head::PLAIN;
  return wt_link_peek(head.data(), size, 0, link) ||
                 wt_link_peek(head.data() + size.physical(), size, 1, link)
             ? Head::LINKED
             : Head::BAD;
}

bool has_suffix(const std::string &name, const char *suffix) {
  const size_t n = strlen(suffix);
  return name.size() > n && name.compare(name.size() - n, n, suffix) == 0;
}
}  // namespace

void innobase_wt_gc(bool startup) {
  if (!srv_wt_overlay) {
    /* Reflink mode has no layers: only a compaction's copy, left over by a
    crash before its rename (overlay mode removes it with the .wttmp files
    below). */
    if (!startup || srv_force_recovery > 0) return;
    DIR *d = opendir(".");
    if (d == nullptr) return;
    while (dirent *e = readdir(d)) {
      const std::string dir = e->d_name;
      if (dir == "." || dir == "..") continue;
      DIR *sd = opendir(dir.c_str());
      if (sd == nullptr) continue;
      while (dirent *f = readdir(sd)) {
        const std::string name = f->d_name, path = dir + "/" + name;
        if (!has_suffix(name, ".compact.wttmp")) continue;
        ib::info() << "Removing " << path << " (an interrupted compaction)";
        unlink(path.c_str());
      }
      closedir(sd);
    }
    closedir(d);
    return;
  }
  const std::lock_guard<std::mutex> guard(wt_layer_mutex);
  const auto gc_start = std::chrono::steady_clock::now();
  size_t gc_files = 0;
  auto gc_log = create_scope_guard([&]() {
    const double secs = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - gc_start)
                            .count();
    if (secs > 0.5)
      ib::info() << "Worktree GC: read " << gc_files << " file heads in " << secs
                 << " s";
  });
  /* Layer files and their inodes. */
  std::map<std::string, ino_t> layers;
  if (DIR *d = opendir(WT_LAYER_DIR); d != nullptr) {
    while (dirent *e = readdir(d)) {
      const std::string name = e->d_name;
      if (!has_suffix(name, ".ibl")) continue;
      struct stat st;
      if (stat(wt_layer_path(name.substr(0, name.size() - 4)).c_str(), &st) == 0)
        layers[name.substr(0, name.size() - 4)] = st.st_ino;
    }
    closedir(d);
  }
  /* Roots: every tablespace file of every schema, and the temporary file of
  a CREATE WORKTREE in progress. A freeze's temporary file is left over only
  by a crash before its commit point: deleted at startup. */
  std::vector<std::string> todo;
  std::vector<std::pair<ino_t, space_id_t>> ibds;
  bool unsafe = false;
  if (DIR *d = opendir("."); d != nullptr) {
    while (dirent *e = readdir(d)) {
      const std::string dir = e->d_name;
      if (dir == "." || dir == ".." || dir == WT_LAYER_DIR) continue;
      DIR *sd = opendir(dir.c_str());
      if (sd == nullptr) continue;
      while (dirent *f = readdir(sd)) {
        const std::string name = f->d_name, path = dir + "/" + name;
        if (startup && srv_force_recovery == 0 && has_suffix(name, ".wttmp")) {
          ib::info() << "Removing " << path
                     << " (an interrupted freeze or compaction)";
          unlink(path.c_str());
          continue;
        }
        if (!has_suffix(name, ".ibd") && !has_suffix(name, ".wtclone") &&
            !has_suffix(name, ".wttmp"))
          continue;
        std::vector<byte> head;
        page_size_t size(univ_page_size);
        ino_t ino;
        ++gc_files;
        if (!wt_read_head(path, &head, &size, &ino)) continue;
        if (has_suffix(name, ".ibd"))
          ibds.emplace_back(ino, fsp_header_get_space_id(head.data()));
        wt_link_t link;
        switch (wt_head_link(head, size, &link)) {
          case Head::LINKED:
            todo.push_back(link.uuid);
            break;
          case Head::BAD:
            ib::warn() << "Worktree GC: " << path << " has an unreadable link";
            unsafe = true;
            break;
          case Head::PLAIN:
            break;
        }
      }
      closedir(sd);
    }
    closedir(d);
  }
  /* Layers of creates in progress are roots too. */
  todo.insert(todo.end(), wt_pinned_layers.begin(), wt_pinned_layers.end());
  /* Reachable layers, transitively; fail-safe on any bad link (v3 §9). */
  std::set<std::string> reached;
  while (!todo.empty() && !unsafe) {
    const std::string uuid = todo.back();
    todo.pop_back();
    if (!reached.insert(uuid).second) continue;
    if (layers.count(uuid) == 0) {
      ib::warn() << "Worktree GC: layer " << uuid << " is linked but missing";
      unsafe = true;
      break;
    }
    std::vector<byte> head;
    page_size_t size(univ_page_size);
    ino_t ino;
    wt_link_t link;
    if (!wt_read_head(wt_layer_path(uuid), &head, &size, &ino)) {
      unsafe = true;
      break;
    }
    switch (wt_head_link(head, size, &link)) {
      case Head::LINKED:
        todo.push_back(link.uuid);
        break;
      case Head::BAD:
        unsafe = true;
        break;
      case Head::PLAIN:
        break;
    }
  }
  if (unsafe) {
    ib::warn() << "Worktree GC skipped: a link could not be read";
    wt_layer_names.store(static_cast<int>(layers.size()));
    return;
  }
  std::set<ino_t> live;
  int kept = 0;
  for (const auto &[uuid, ino] : layers) {
    if (reached.count(uuid) != 0) {
      live.insert(ino);
      ++kept;
      continue;
    }
    /* v3 §5.4: a force-recovery start deletes nothing. */
    if (srv_force_recovery > 0) {
      ++kept;
      continue;
    }
    ib::info() << "Worktree GC: removing unreferenced layer " << uuid;
    unlink(wt_layer_path(uuid).c_str());
  }
  wt_layer_names.store(kept);
  /* A read-only source whose live file is still a layer must not be
  written (addendum A2); one whose layers are gone is an ordinary table. */
  for (const auto &[ino, space_id] : ibds) {
    fil_space_t *space = fil_space_acquire_silent(space_id);
    if (space == nullptr) continue;
    space->wt_linked = live.count(ino) != 0;
    fil_space_release(space);
  }
}
