/*****************************************************************************

Copyright (c) 2026, MySQL worktrees contributors.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

*****************************************************************************/

/** @file fil/fil0wt.cc
 Worktrees: ancestor map and read-time page translation. */

#include "fil0wt.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include "buf0checksum.h"
#include "dict0dict.h"
#include "fsp0fsp.h"
#include "os0enc.h"
#include "fsp0types.h"
#include "mach0data.h"
#include "page0types.h"
#include "srv0srv.h"
#include "ut0crc32.h"

namespace {
constexpr ulint WT_HDR_MAGIC = 0;
constexpr ulint WT_HDR_N_ANCESTORS = 4;
constexpr ulint WT_HDR_N_INDEXES = 8;
constexpr ulint WT_HDR_N_ROOTS = 12;
constexpr ulint WT_HDR_CRC = 16;
/* Covered by the CRC from here on. */
constexpr ulint WT_HDR_SELF = 20;
constexpr ulint WT_HDR_GENERATION = 24;
constexpr ulint WT_HDR_SIZE = 32;

/* Overflow stub: magic, map length, n_pages, crc, then (CRC-covered) self,
generation and the page numbers. */
constexpr uint32_t WT_STUB_MAGIC = 0x57544F31; /* "WTO1" */
/* "WTO2": a stub followed by the overlay link. Its pages may be holes, read
through the chain (a frozen file's map pages live in its layer), and no pages
means the map is page 1's inline map. */
constexpr uint32_t WT_STUB_MAGIC_LINK = 0x57544F32;
constexpr ulint WT_STUB_LEN = 4;
constexpr ulint WT_STUB_N_PAGES = 8;
constexpr ulint WT_STUB_CRC = 12;
constexpr ulint WT_STUB_SELF = 16;
constexpr ulint WT_STUB_GENERATION = 20;
constexpr ulint WT_STUB_SIZE = 28;

/** IBUF_BITS_PER_PAGE (ibuf0ibuf.cc) */
constexpr ulint WT_IBUF_BITS_PER_PAGE = 4;

bool is_btree_page(page_type_t type) {
  return type == FIL_PAGE_INDEX || type == FIL_PAGE_RTREE ||
         type == FIL_PAGE_SDI;
}

/** Parse a map in its on-disk format from p (avail bytes). */
/* Link section of a "WTM2" map: uuid (32 hex), parent space id, parent
size, freeze LSN. */
constexpr ulint WT_LINK_BYTES = 32 + 4 + 4 + 8;

bool wt_map_parse(const byte *p, ulint avail, wt_map_t *map) {
  if (avail < WT_HDR_SIZE) return false;
  const uint32_t magic = mach_read_from_4(p + WT_HDR_MAGIC);
  if (magic != WT_MAP_MAGIC && magic != WT_MAP_MAGIC_LINK) return false;
  const bool has_link = magic == WT_MAP_MAGIC_LINK;

  const ulint n_anc = mach_read_from_4(p + WT_HDR_N_ANCESTORS);
  const ulint n_idx = mach_read_from_4(p + WT_HDR_N_INDEXES);
  const ulint n_roots = mach_read_from_4(p + WT_HDR_N_ROOTS);
  /* A frozen stock source links its layer but has no ancestors. */
  if ((n_anc == 0 && !has_link) || n_anc > WT_MAX_ANCESTORS || n_idx > avail ||
      n_roots > avail) {
    return false;
  }
  const ulint len = n_anc * 4 + n_idx * 16 + n_roots * 4 +
                    (has_link ? WT_LINK_BYTES : 0);
  if (WT_HDR_SIZE + len > avail ||
      mach_read_from_4(p + WT_HDR_CRC) !=
          ut_crc32(p + WT_HDR_SELF, WT_HDR_SIZE - WT_HDR_SELF + len)) {
    return false;
  }

  map->self = mach_read_from_4(p + WT_HDR_SELF);
  map->generation = mach_read_from_8(p + WT_HDR_GENERATION);
  map->ancestors.clear();
  map->index_map.clear();
  map->roots.clear();
  const byte *q = p + WT_HDR_SIZE;
  for (ulint i = 0; i < n_anc; ++i, q += 4) {
    map->ancestors.push_back(mach_read_from_4(q));
  }
  for (ulint i = 0; i < n_idx; ++i, q += 16) {
    map->index_map.emplace_back(mach_read_from_8(q), mach_read_from_8(q + 8));
  }
  for (ulint i = 0; i < n_roots; ++i, q += 4) {
    map->roots.push_back(mach_read_from_4(q));
  }
  map->has_link = has_link;
  map->link = wt_link_t{};
  if (has_link) {
    /* A layer name is 32 lowercase hex digits: never a path. */
    for (ulint i = 0; i < 32; ++i) {
      const byte ch = q[i];
      if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    }
    memcpy(map->link.uuid, q, 32);
    map->link.uuid[32] = '\0';
    map->link.parent_space = mach_read_from_4(q + 32);
    map->link.parent_size = mach_read_from_4(q + 36);
    map->link.freeze_lsn = mach_read_from_8(q + 40);
  }
  std::sort(map->index_map.begin(), map->index_map.end());
  return true;
}

/** Parse an overflow stub; pages may be nullptr. */
bool wt_stub_parse(const byte *p, ulint avail, space_id_t *self,
                   uint64_t *generation, ulint *map_len,
                   std::vector<page_no_t> *pages, wt_link_t *link = nullptr) {
  if (avail < WT_STUB_SIZE) return false;
  const uint32_t magic = mach_read_from_4(p);
  if (magic != WT_STUB_MAGIC && magic != WT_STUB_MAGIC_LINK) return false;
  const bool has_link = magic == WT_STUB_MAGIC_LINK;
  const ulint n = mach_read_from_4(p + WT_STUB_N_PAGES);
  const ulint extra = has_link ? WT_LINK_BYTES : 0;
  /* No pages: the map is page 1's inline map (a flattened file keeps it with
  no link). */
  if (n > avail || WT_STUB_SIZE + n * 4 + extra > avail ||
      mach_read_from_4(p + WT_STUB_CRC) !=
          ut_crc32(p + WT_STUB_SELF,
                   WT_STUB_SIZE - WT_STUB_SELF + n * 4 + extra)) {
    return false;
  }
  if (link != nullptr) {
    *link = wt_link_t{};
    if (has_link) {
      const byte *q = p + WT_STUB_SIZE + n * 4;
      for (ulint i = 0; i < 32; ++i) {
        const byte ch = q[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
      }
      memcpy(link->uuid, q, 32);
      link->uuid[32] = '\0';
      link->parent_space = mach_read_from_4(q + 32);
      link->parent_size = mach_read_from_4(q + 36);
      link->freeze_lsn = mach_read_from_8(q + 40);
    }
  }
  *self = mach_read_from_4(p + WT_STUB_SELF);
  *generation = mach_read_from_8(p + WT_STUB_GENERATION);
  *map_len = mach_read_from_4(p + WT_STUB_LEN);
  if (pages != nullptr) {
    pages->clear();
    for (ulint i = 0; i < n; ++i) {
      const page_no_t no = mach_read_from_4(p + WT_STUB_SIZE + i * 4);
      if (no <= 1 || no == FIL_NULL) return false;
      pages->push_back(no);
    }
  }
  return true;
}
}  // namespace

/** Map area of header page page_no. Uncompressed: the last quarter of the page
(on page 0 the XDES array, encryption info and SDI root pointer end well
before it; on page 1 the change buffer bitmap ends at about half the page).
Compressed pages are as small as 1 KB, where that rule would overlap header
data, so the area starts right after it. */
ulint wt_map_offset(const page_size_t &page_size, page_no_t page_no) {
  if (!page_size.is_compressed()) {
    return page_size.physical() - FIL_PAGE_DATA_END - page_size.physical() / 4;
  }
  const ulint end =
      page_no == 0
          ? fsp_header_get_encryption_progress_offset(page_size) +
                Encryption::OPERATION_INFO_SIZE + Encryption::PROGRESS_INFO_SIZE
          : FIL_PAGE_DATA + UT_BITS_IN_BYTES(page_size.physical() *
                                             WT_IBUF_BITS_PER_PAGE);
  return ut_calc_align(end, 8);
}

ulint wt_map_size(const page_size_t &page_size, page_no_t page_no) {
  return page_size.physical() - FIL_PAGE_DATA_END -
         wt_map_offset(page_size, page_no);
}

bool wt_map_t::is_ancestor(space_id_t space_id) const {
  return std::find(ancestors.begin(), ancestors.end(), space_id) !=
         ancestors.end();
}

space_index_t wt_map_t::map_index(space_index_t old_id) const {
  auto it = std::lower_bound(
      index_map.begin(), index_map.end(), old_id,
      [](const auto &e, space_index_t id) { return e.first < id; });
  return it != index_map.end() && it->first == old_id ? it->second : 0;
}

bool wt_map_read(const byte *page, const page_size_t &page_size,
                 page_no_t page_no, const wt_page_reader &read, wt_map_t *map) {
  const byte *p = page + wt_map_offset(page_size, page_no);
  const ulint avail = wt_map_size(page_size, page_no);
  if (wt_map_parse(p, avail, map)) return true;

  space_id_t self;
  uint64_t generation;
  ulint len;
  std::vector<page_no_t> pages;
  wt_link_t link;
  if (!read ||
      !wt_stub_parse(p, avail, &self, &generation, &len, &pages, &link))
    return false;
  const bool has_link = link.uuid[0] != '\0';
  auto buf = static_cast<byte *>(
      ut::aligned_alloc(page_size.physical(), page_size.physical()));
  if (pages.empty()) {
    /* The map is page 1's inline map; the stub's link is the current one. */
    const bool ok = page_no == 0 && read(1, buf) &&
                    wt_map_parse(buf + wt_map_offset(page_size, 1),
                                 wt_map_size(page_size, 1), map) &&
                    map->self == self && map->generation == generation;
    ut::aligned_free(buf);
    if (ok) {
      map->has_link = has_link;
      map->link = link;
    }
    return ok;
  }
  const ulint cap = wt_overflow_capacity(page_size);
  if (len > pages.size() * cap) {
    ut::aligned_free(buf);
    return false;
  }

  std::vector<byte> bytes;
  bool ok = true;
  for (page_no_t no : pages) {
    if (!read(no, buf) || mach_read_from_4(buf + FIL_PAGE_OFFSET) != no ||
        mach_read_from_4(buf + FIL_PAGE_SPACE_ID) != self) {
      ok = false;
      break;
    }
    bytes.insert(bytes.end(), buf + FIL_PAGE_DATA, buf + FIL_PAGE_DATA + cap);
  }
  ut::aligned_free(buf);
  ok = ok && wt_map_parse(bytes.data(), len, map) && map->self == self &&
       map->generation == generation;
  /* A stub's link is the current one (a frozen file keeps its old map pages
  in its layer, with the link they had then; a flattened one has none). */
  if (ok) {
    map->has_link = has_link;
    map->link = link;
  }
  return ok;
}

bool wt_overflow_pages_of(const byte *page, const page_size_t &page_size,
                          page_no_t page_no, ulint *map_len,
                          std::vector<page_no_t> *pages) {
  space_id_t self;
  uint64_t generation;
  return wt_stub_parse(page + wt_map_offset(page_size, page_no),
                       wt_map_size(page_size, page_no), &self, &generation,
                       map_len, pages) &&
         !pages->empty();
}

bool wt_link_peek(const byte *page, const page_size_t &page_size,
                  page_no_t page_no, wt_link_t *link) {
  const byte *p = page + wt_map_offset(page_size, page_no);
  const ulint avail = wt_map_size(page_size, page_no);
  wt_map_t map;
  if (wt_map_parse(p, avail, &map)) {
    if (!map.has_link) return false;
    *link = map.link;
    return true;
  }
  space_id_t self;
  uint64_t generation;
  ulint len;
  return wt_stub_parse(p, avail, &self, &generation, &len, nullptr, link) &&
         link->uuid[0] != '\0';
}

std::vector<byte> wt_map_serialize(const wt_map_t &map) {
  const ulint len = map.ancestors.size() * 4 + map.index_map.size() * 16 +
                    map.roots.size() * 4 + (map.has_link ? WT_LINK_BYTES : 0);
  std::vector<byte> out(WT_HDR_SIZE + len);
  byte *p = out.data();
  byte *q = p + WT_HDR_SIZE;
  for (auto id : map.ancestors) {
    mach_write_to_4(q, id);
    q += 4;
  }
  for (auto &e : map.index_map) {
    mach_write_to_8(q, e.first);
    mach_write_to_8(q + 8, e.second);
    q += 16;
  }
  for (auto page_no : map.roots) {
    mach_write_to_4(q, page_no);
    q += 4;
  }
  if (map.has_link) {
    memcpy(q, map.link.uuid, 32);
    mach_write_to_4(q + 32, map.link.parent_space);
    mach_write_to_4(q + 36, map.link.parent_size);
    mach_write_to_8(q + 40, map.link.freeze_lsn);
  }
  mach_write_to_4(p + WT_HDR_MAGIC,
                  map.has_link ? WT_MAP_MAGIC_LINK : WT_MAP_MAGIC);
  mach_write_to_4(p + WT_HDR_N_ANCESTORS, map.ancestors.size());
  mach_write_to_4(p + WT_HDR_N_INDEXES, map.index_map.size());
  mach_write_to_4(p + WT_HDR_N_ROOTS, map.roots.size());
  mach_write_to_4(p + WT_HDR_SELF, map.self);
  mach_write_to_8(p + WT_HDR_GENERATION, map.generation);
  mach_write_to_4(p + WT_HDR_CRC,
                  ut_crc32(p + WT_HDR_SELF, WT_HDR_SIZE - WT_HDR_SELF + len));
  return out;
}

bool wt_map_write(const wt_map_t &map, byte *page, const page_size_t &page_size,
                  page_no_t page_no) {
  if ((map.ancestors.empty() && !map.has_link) ||
      map.ancestors.size() > WT_MAX_ANCESTORS)
    return false;
  const auto bytes = wt_map_serialize(map);
  if (bytes.size() > wt_map_size(page_size, page_no)) return false;
  byte *p = page + wt_map_offset(page_size, page_no);
  memset(p, 0, wt_map_size(page_size, page_no));
  memcpy(p, bytes.data(), bytes.size());
  return true;
}

ulint wt_overflow_capacity(const page_size_t &page_size) {
  return page_size.physical() - FIL_PAGE_DATA - FIL_PAGE_DATA_END;
}

bool wt_stub_write(const wt_map_t &map, ulint map_len,
                   const std::vector<page_no_t> &pages, byte *page,
                   const page_size_t &page_size, page_no_t page_no) {
  const ulint extra = map.has_link ? WT_LINK_BYTES : 0;
  if (WT_STUB_SIZE + pages.size() * 4 + extra > wt_map_size(page_size, page_no))
    return false;
  byte *p = page + wt_map_offset(page_size, page_no);
  memset(p, 0, wt_map_size(page_size, page_no));
  mach_write_to_4(p, map.has_link ? WT_STUB_MAGIC_LINK : WT_STUB_MAGIC);
  mach_write_to_4(p + WT_STUB_LEN, map_len);
  mach_write_to_4(p + WT_STUB_N_PAGES, pages.size());
  mach_write_to_4(p + WT_STUB_SELF, map.self);
  mach_write_to_8(p + WT_STUB_GENERATION, map.generation);
  for (ulint i = 0; i < pages.size(); ++i) {
    mach_write_to_4(p + WT_STUB_SIZE + i * 4, pages[i]);
  }
  if (map.has_link) {
    byte *q = p + WT_STUB_SIZE + pages.size() * 4;
    memcpy(q, map.link.uuid, 32);
    mach_write_to_4(q + 32, map.link.parent_space);
    mach_write_to_4(q + 36, map.link.parent_size);
    mach_write_to_8(q + 40, map.link.freeze_lsn);
  }
  mach_write_to_4(p + WT_STUB_CRC,
                  ut_crc32(p + WT_STUB_SELF,
                           WT_STUB_SIZE - WT_STUB_SELF + pages.size() * 4 +
                               extra));
  return true;
}

void fil_wt_load(fil_space_t *space, const byte *page0, const byte *page1,
                 const wt_page_reader &read) {
  if (space->wt_map_loaded) return;
  space->wt_map_loaded = true;

  const page_size_t page_size(space->flags);

  /* Two copies (page 0, page 1); use the newest valid one that is ours. A
  copy whose self id is not this space belongs to an ancestor's file. */
  auto map = ut::new_withkey<wt_map_t>(UT_NEW_THIS_FILE_PSI_KEY);
  wt_map_t copy;
  const bool ok0 = wt_map_read(page0, page_size, 0, read, map) &&
                   map->self == space->id;
  const bool ok1 = page1 != nullptr &&
                   wt_map_read(page1, page_size, 1, read, &copy) &&
                   copy.self == space->id;
  if (ok1 && (!ok0 || copy.generation > map->generation)) {
    if (!ok0 && mach_read_from_4(page0 + wt_map_offset(page_size, 0)) != 0) {
      ib::warn() << "Worktree map in page 0 of tablespace " << space->name
                 << " is damaged; using the copy in page 1.";
    }
    *map = copy;
  } else if (!ok0) {
    ut::delete_(map);
    return;
  }
  space->wt_map = map;
}

void fil_wt_set(fil_space_t *space, const wt_map_t &map) {
  ut::delete_(space->wt_map);
  space->wt_map = ut::new_withkey<wt_map_t>(UT_NEW_THIS_FILE_PSI_KEY, map);
  space->wt_map_loaded = true;
}

bool wt_page1_self(const byte *page1, const page_size_t &page_size,
                   space_id_t *self) {
  const byte *p = page1 + wt_map_offset(page_size, 1);
  const ulint avail = wt_map_size(page_size, 1);
  wt_map_t map;
  if (wt_map_parse(p, avail, &map)) {
    *self = map.self;
    return true;
  }
  uint64_t generation;
  ulint len;
  return wt_stub_parse(p, avail, self, &generation, &len, nullptr);
}

const wt_map_t *fil_wt_map(const fil_space_t *space) {
  return space == nullptr ? nullptr : space->wt_map;
}

bool fil_wt_is_same_or_ancestor(space_id_t space_id, space_id_t ancestor) {
  if (space_id == ancestor) return true;
  fil_space_t *space = fil_space_acquire_silent(space_id);
  if (space == nullptr) return false;
  const bool ok = space->wt_map != nullptr && space->wt_map->is_ancestor(ancestor);
  fil_space_release(space);
  return ok;
}

void wt_page_recalc_checksum(byte *frame, const page_size_t &page_size) {
  if (page_size.is_compressed()) {
    /* As buf_flush_update_zip_checksum(), without touching FIL_PAGE_LSN. */
    BlockReporter reporter(false, nullptr, univ_page_size, false);
    mach_write_to_4(frame + FIL_PAGE_SPACE_OR_CHKSUM,
                    reporter.calc_zip_checksum(
                        frame, page_size.physical(),
                        static_cast<srv_checksum_algorithm_t>(
                            srv_checksum_algorithm)));
    return;
  }
  uint32_t checksum = BUF_NO_CHECKSUM_MAGIC;

  switch ((srv_checksum_algorithm_t)srv_checksum_algorithm) {
    case SRV_CHECKSUM_ALGORITHM_CRC32:
    case SRV_CHECKSUM_ALGORITHM_STRICT_CRC32:
      checksum = buf_calc_page_crc32(frame);
      mach_write_to_4(frame + FIL_PAGE_SPACE_OR_CHKSUM, checksum);
      break;
    case SRV_CHECKSUM_ALGORITHM_INNODB:
    case SRV_CHECKSUM_ALGORITHM_STRICT_INNODB:
      checksum = (uint32_t)buf_calc_page_new_checksum(frame);
      mach_write_to_4(frame + FIL_PAGE_SPACE_OR_CHKSUM, checksum);
      checksum = (uint32_t)buf_calc_page_old_checksum(frame);
      break;
    case SRV_CHECKSUM_ALGORITHM_NONE:
    case SRV_CHECKSUM_ALGORITHM_STRICT_NONE:
      mach_write_to_4(frame + FIL_PAGE_SPACE_OR_CHKSUM, checksum);
      break;
  }
  mach_write_to_4(frame + page_size.logical() - FIL_PAGE_END_LSN_OLD_CHKSUM,
                  checksum);
}

namespace {
/** Patch the identity fields of one copy of a page (header bytes only). */
void wt_patch_header(const wt_map_t &map, space_id_t space_id, byte *frame) {
  mach_write_to_4(frame + FIL_PAGE_SPACE_ID, space_id);

  if (!is_btree_page(fil_page_get_type(frame))) return;
  byte *id_ptr = frame + PAGE_HEADER + PAGE_INDEX_ID;
  const space_index_t new_id = map.map_index(mach_read_from_8(id_ptr));
  /* Unmapped: the SDI index (its id is a constant) or a free page of a
  dropped index, which nothing references. */
  if (new_id != 0) mach_write_to_8(id_ptr, new_id);

  const page_no_t page_no = mach_read_from_4(frame + FIL_PAGE_OFFSET);
  if (std::find(map.roots.begin(), map.roots.end(), page_no) !=
      map.roots.end()) {
    for (ulint seg : {PAGE_BTR_SEG_LEAF, PAGE_BTR_SEG_TOP}) {
      byte *hdr = frame + PAGE_HEADER + seg + FSEG_HDR_SPACE;
      if (map.is_ancestor(mach_read_from_4(hdr))) {
        mach_write_to_4(hdr, space_id);
      }
    }
  }
}
}  // namespace

void wt_translate_page(const wt_map_t &map, space_id_t space_id,
                       const page_size_t &page_size, byte *frame, byte *zip) {
  if (frame != nullptr) wt_patch_header(map, space_id, frame);
  if (zip != nullptr) {
    wt_patch_header(map, space_id, zip);
    wt_page_recalc_checksum(zip, page_size);
  } else {
    /* The frame of a compressed page is never written; only zip is. */
    wt_page_recalc_checksum(frame, univ_page_size);
  }
}

namespace {
constexpr const char *WT_ANCESTORS_FILE = "#wt_ancestors";
std::mutex wt_ancestors_mutex;
}  // namespace

bool wt_ancestors_record(
    const std::vector<std::pair<space_id_t, std::string>> &ancestors) {
  /* Lines already made durable by this process: clones of the same source
  need no write and no fsync. */
  static std::set<std::string> durable;
  const std::lock_guard<std::mutex> guard(wt_ancestors_mutex);
  std::string text;
  std::vector<std::string> lines;
  for (const auto &[id, path] : ancestors) {
    std::string line = std::to_string(id) + " " + path + "\n";
    if (durable.count(line) == 0) {
      text += line;
      lines.push_back(std::move(line));
    }
  }
  if (lines.empty()) return false;
  const int fd = open(WT_ANCESTORS_FILE, O_WRONLY | O_CREAT | O_APPEND, 0640);
  if (fd < 0) return true;
  const bool fail = write(fd, text.data(), text.size()) !=
                        static_cast<ssize_t>(text.size()) ||
                    fsync(fd) != 0;
  close(fd);
  if (!fail) durable.insert(lines.begin(), lines.end());
  return fail;
}

bool wt_ancestors_allow(space_id_t space_id, const std::string &path) {
  /* Read on every call: this only runs for a file whose page 0 is unreadable. */
  const std::lock_guard<std::mutex> guard(wt_ancestors_mutex);
  std::ifstream in(WT_ANCESTORS_FILE);
  bool is_ancestor = false;
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream fields(line);
    space_id_t id;
    std::string own_path;
    if (!(fields >> id) || id != space_id) continue;
    std::getline(fields >> std::ws, own_path);
    if (Fil_path::is_same_as(own_path, path)) return true;  // its own file
    is_ancestor = true;
  }
  return !is_ancestor;
}
