/*****************************************************************************

Copyright (c) 2026, MySQL worktrees contributors.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

*****************************************************************************/

/** @file include/fil0wt.h
 Worktrees: copy-on-write clones of file-per-table tablespaces.

 A worktree tablespace is a reflink of an ancestor's .ibd. Only page 0 is
 rewritten at clone time (new space id plus the ancestor map below). Every
 other page keeps the ancestor's space id and index ids on disk until it is
 modified; it is translated in RAM when it is read into the buffer pool, and
 reaches disk with its new identity only when it is really modified. */

#ifndef fil0wt_h
#define fil0wt_h

#include <utility>
#include <vector>

#include "fil0fil.h"
#include "page0size.h"

/** "WTM1" */
constexpr uint32_t WT_MAP_MAGIC = 0x57544D31;

/** Longest ancestor chain a worktree may have. */
constexpr size_t WT_MAX_ANCESTORS = 16;

/** Ancestor map of a worktree tablespace, stored in the unused tail of page 0
and, as a redundant copy, of page 1 (§12.1 H5). On disk (big-endian): magic,
n_ancestors, n_indexes, n_roots, crc32 of the rest, self space id,
generation, then ancestor space ids (4 bytes each), (old index id, new index
id) pairs (8 + 8 bytes), and B-tree root page numbers incl. the SDI root (4). */
struct wt_map_t {
  /** The tablespace this map belongs to (lets startup tell a worktree's own
  copy from an ancestor's, and identify a file whose page 0 is lost). */
  space_id_t self{};
  /** Bumped on every rewrite; the newer of the two copies wins. */
  uint64_t generation{};
  /** Space ids whose pages this tablespace still contains. */
  std::vector<space_id_t> ancestors;
  /** Ancestor index id -> this tablespace's index id, sorted by the first. */
  std::vector<std::pair<space_index_t, space_index_t>> index_map;
  /** Root pages: their segment headers carry the space id too. */
  std::vector<page_no_t> roots;

  /** @return true if pages stamped with space_id belong to this tablespace */
  bool is_ancestor(space_id_t space_id) const;

  /** @return the index id to use for ancestor index id, or 0 if unmapped */
  space_index_t map_index(space_index_t old_id) const;
};

/** Parse the ancestor map in page 0.
@param[in]  page0  page 0 as read from disk
@param[in]  page_size  tablespace page size
@param[out] map  the map
@return true if page 0 carries a valid map */
bool wt_map_read(const byte *page0, const page_size_t &page_size,
                 wt_map_t *map);

/** Write the ancestor map into page 0 (the caller recomputes the checksum).
@return false if the map does not fit */
[[nodiscard]] bool wt_map_write(const wt_map_t &map, byte *page0,
                                const page_size_t &page_size);

/** Load a tablespace's ancestor map from page 0. Called once, when the first
file of the tablespace is opened, before any page of it is read.
@param[in,out]  space  tablespace
@param[in]  page0  page 0 as read from disk */
void fil_wt_load(fil_space_t *space, const byte *page0, const byte *page1);

/** Read the map copy in page 1 of a file whose page 0 may be lost.
@param[in]  page1  page 1 as read from disk (checksum already validated)
@param[in]  page_size  page size
@param[out] self  the file's own space id
@param[out] ancestors  ancestor space ids (never a valid id for this file)
@return true if page 1 carries a valid map copy */
bool wt_page1_self(const byte *page1, const page_size_t &page_size,
                   space_id_t *self, std::vector<space_id_t> *ancestors);

/** @return the ancestor map of a tablespace, or nullptr if it is not a
worktree. Valid while the caller holds a reference to the space. */
const wt_map_t *fil_wt_map(const fil_space_t *space);

/** @return true if LOB refs / pages stamped with ancestor are valid in
space_id (equal or ancestor). */
bool fil_wt_is_same_or_ancestor(space_id_t space_id, space_id_t ancestor);

/** Durable record of worktree ancestors, "#wt_ancestors" in the datadir: one
line "<ancestor space id> <path of the ancestor's own file>" per ancestor of
every worktree ever created. Appended and fsynced before a clone's file gets
its .ibd name. Startup uses it so that a file whose identity pages are lost is
never identified as an ancestor (fork-design.md §10.1 item 4).
@return false on success */
[[nodiscard]] bool wt_ancestors_record(
    const std::vector<std::pair<space_id_t, std::string>> &ancestors);

/** May a file at path that votes for space_id be identified as space_id?
False if space_id is a worktree ancestor and path is not its own file. */
bool wt_ancestors_allow(space_id_t space_id, const std::string &path);

/** Translate a page read from an ancestor into this tablespace's identity, in
RAM only: header space id, index id, root segment headers, then checksum. The
caller has already validated the checksum of the bytes read from disk.
@param[in]  map  ancestor map of the tablespace
@param[in]  space_id  this tablespace
@param[in,out]  frame  uncompressed page frame */
void wt_translate_page(const wt_map_t &map, space_id_t space_id, byte *frame);

/** Recompute the page checksum in RAM, the way a flush would. */
void wt_page_recalc_checksum(byte *frame, const page_size_t &page_size);

#endif /* fil0wt_h */
