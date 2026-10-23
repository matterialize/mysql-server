/*****************************************************************************

Copyright (c) 2026, MySQL worktrees contributors.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

*****************************************************************************/

/** @file fil/fil0wt.cc
 Worktrees: ancestor map and read-time page translation. */

#include "fil0wt.h"

#include <algorithm>

#include "buf0checksum.h"
#include "dict0dict.h"
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
constexpr ulint WT_HDR_SIZE = 20;

/** Map area: the last quarter of page 0, which InnoDB never uses (the XDES
array, encryption info and SDI root pointer end well before it). */
ulint wt_map_size(const page_size_t &page_size) {
  return page_size.physical() / 4;
}

ulint wt_map_offset(const page_size_t &page_size) {
  return page_size.physical() - FIL_PAGE_DATA_END - wt_map_size(page_size);
}

bool is_btree_page(page_type_t type) {
  return type == FIL_PAGE_INDEX || type == FIL_PAGE_RTREE ||
         type == FIL_PAGE_SDI;
}
}  // namespace

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

bool wt_map_read(const byte *page0, const page_size_t &page_size,
                 wt_map_t *map) {
  const byte *p = page0 + wt_map_offset(page_size);
  if (mach_read_from_4(p + WT_HDR_MAGIC) != WT_MAP_MAGIC) return false;

  const ulint n_anc = mach_read_from_4(p + WT_HDR_N_ANCESTORS);
  const ulint n_idx = mach_read_from_4(p + WT_HDR_N_INDEXES);
  const ulint n_roots = mach_read_from_4(p + WT_HDR_N_ROOTS);
  const ulint len = n_anc * 4 + n_idx * 16 + n_roots * 4;
  if (n_anc == 0 || n_anc > WT_MAX_ANCESTORS ||
      WT_HDR_SIZE + len > wt_map_size(page_size) ||
      mach_read_from_4(p + WT_HDR_CRC) != ut_crc32(p + WT_HDR_SIZE, len)) {
    return false;
  }

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
  std::sort(map->index_map.begin(), map->index_map.end());
  return true;
}

bool wt_map_write(const wt_map_t &map, byte *page0,
                  const page_size_t &page_size) {
  const ulint len =
      map.ancestors.size() * 4 + map.index_map.size() * 16 + map.roots.size() * 4;
  if (map.ancestors.empty() || map.ancestors.size() > WT_MAX_ANCESTORS ||
      WT_HDR_SIZE + len > wt_map_size(page_size)) {
    return false;
  }

  byte *p = page0 + wt_map_offset(page_size);
  memset(p, 0, wt_map_size(page_size));
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
  mach_write_to_4(p + WT_HDR_MAGIC, WT_MAP_MAGIC);
  mach_write_to_4(p + WT_HDR_N_ANCESTORS, map.ancestors.size());
  mach_write_to_4(p + WT_HDR_N_INDEXES, map.index_map.size());
  mach_write_to_4(p + WT_HDR_N_ROOTS, map.roots.size());
  mach_write_to_4(p + WT_HDR_CRC, ut_crc32(p + WT_HDR_SIZE, len));
  return true;
}

void fil_wt_load(fil_space_t *space, const byte *page0) {
  if (space->wt_map_loaded) return;
  space->wt_map_loaded = true;

  const page_size_t page_size(space->flags);
  if (page_size.is_compressed()) return;

  auto map = ut::new_withkey<wt_map_t>(UT_NEW_THIS_FILE_PSI_KEY);
  if (wt_map_read(page0, page_size, map)) {
    space->wt_map = map;
  } else {
    ut::delete_(map);
  }
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
  ut_a(!page_size.is_compressed());
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

void wt_translate_page(const wt_map_t &map, space_id_t space_id, byte *frame) {
  mach_write_to_4(frame + FIL_PAGE_SPACE_ID, space_id);

  if (is_btree_page(fil_page_get_type(frame))) {
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

  wt_page_recalc_checksum(frame, univ_page_size);
}
