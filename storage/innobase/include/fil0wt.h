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

#include <functional>
#include <utility>
#include <vector>

#include "fil0fil.h"
#include "page0size.h"

/** "WTM1" */
constexpr uint32_t WT_MAP_MAGIC = 0x57544D31;

/** "WTM2": a map followed by an overlay link section. */
constexpr uint32_t WT_MAP_MAGIC_LINK = 0x57544D32;

/** Longest ancestor chain a worktree may have. */
constexpr size_t WT_MAX_ANCESTORS = 16;

/** Ancestor map of a worktree tablespace, stored in the unused tail of page 0
and, as a redundant copy, of page 1 (§12.1 H5). On disk (big-endian): magic,
n_ancestors, n_indexes, n_roots, crc32 of the rest, self space id,
generation, then ancestor space ids (4 bytes each), (old index id, new index
id) pairs (8 + 8 bytes), and B-tree root page numbers incl. the SDI root (4).

When the map does not fit in a header page's tail (small compressed pages,
long chains), the tail holds an overflow stub instead (fork-design.md §10.1
item 5): magic "WTO1", the byte length of the map, the number of overflow
pages, crc32 of the rest, self, generation, then the overflow page numbers.
The map bytes (same format as above) fill the data area of those pages, which
the file's own allocator has marked used. Page 0 and page 1 point to separate
copies. */
/** The overlay link (limits/overlay-v3.md §3, addendum A3): an overlay's
immediate parent layer. Stored as a section of the map ("WTM2"), so it is
inline or in overflow pages like the map, and copied in page 1. */
struct wt_link_t {
  /** Parent layer name (#wtlayers/<uuid>.ibl), 32 hex digits; the link's
  generation identity (every freeze or live link mints a new one). */
  char uuid[33]{};
  /** Space id the parent's own pages are stamped with. */
  space_id_t parent_space{};
  /** Size of the parent in pages when the link was made. */
  page_no_t parent_size{};
  /** F: the parent's pages have LSN <= F; local pages written after the
  creation have LSN > F (v3 I6). */
  lsn_t freeze_lsn{};
};

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
  /** Overlay mode: the parent link ("WTM2"; ancestors may then be empty). */
  bool has_link{};
  wt_link_t link;

  /** @return true if pages stamped with space_id belong to this tablespace */
  bool is_ancestor(space_id_t space_id) const;

  /** @return the index id to use for ancestor index id, or 0 if unmapped */
  space_index_t map_index(space_index_t old_id) const;
};

/** Reads page page_no of the tablespace's file as stored (decrypted). */
using wt_page_reader = std::function<bool(page_no_t page_no, byte *page)>;

/** Parse the ancestor map in header page page_no (0 or 1), inline or through
its overflow pages.
@param[in]  page  the header page as read from disk
@param[in]  page_size  tablespace page size
@param[in]  page_no  0 or 1
@param[in]  read  reads overflow pages (may be empty: then only an inline map
                  is accepted)
@param[out] map  the map
@return true if the page carries a valid map */
bool wt_map_read(const byte *page, const page_size_t &page_size,
                 page_no_t page_no, const wt_page_reader &read, wt_map_t *map);

/** Write the ancestor map inline into header page page_no (the caller
recomputes the checksum).
@return false if the map does not fit */
[[nodiscard]] bool wt_map_write(const wt_map_t &map, byte *page,
                                const page_size_t &page_size,
                                page_no_t page_no);

/** Offset and size of the map area of header page page_no (0 or 1). */
ulint wt_map_offset(const page_size_t &page_size, page_no_t page_no);
ulint wt_map_size(const page_size_t &page_size, page_no_t page_no);

/** The overlay link of header page page_no, from its inline map or its
stub, without reading anything else (the chain must be attached before the
map's overflow pages can be read).
@return true if the page carries a link */
bool wt_link_peek(const byte *page, const page_size_t &page_size,
                  page_no_t page_no, wt_link_t *link);

/** The overflow pages and map length named by the stub in header page
page_no. @return false if the page has no stub with pages */
bool wt_overflow_pages_of(const byte *page, const page_size_t &page_size,
                          page_no_t page_no, ulint *map_len,
                          std::vector<page_no_t> *pages);

/** The map in its on-disk format, for overflow pages. */
std::vector<byte> wt_map_serialize(const wt_map_t &map);

/** Map bytes held by one overflow page. */
ulint wt_overflow_capacity(const page_size_t &page_size);

/** Write an overflow stub into header page page_no.
@return false if the stub does not fit */
[[nodiscard]] bool wt_stub_write(const wt_map_t &map, ulint map_len,
                                 const std::vector<page_no_t> &pages, byte *page,
                                 const page_size_t &page_size,
                                 page_no_t page_no);

/** Load a tablespace's ancestor map from its header pages. Called once, when
the first file of the tablespace is opened, before any page of it is read.
@param[in,out]  space  tablespace
@param[in]  page0  page 0 as read from disk
@param[in]  page1  page 1 as read from disk (decrypted), or nullptr
@param[in]  read  reads overflow pages */
void fil_wt_load(fil_space_t *space, const byte *page0, const byte *page1,
                 const wt_page_reader &read);

/** Install a map known in memory (a clone being written, before its header
pages point to their overflow pages). */
void fil_wt_set(fil_space_t *space, const wt_map_t &map);

/** The file's own space id from the map copy in page 1 of a file whose page
0 may be lost (inline map or overflow stub).
@param[in]  page1  page 1 as read from disk (checksum already validated)
@param[in]  page_size  page size
@param[out] self  the file's own space id
@return true if page 1 carries a valid map copy */
bool wt_page1_self(const byte *page1, const page_size_t &page_size,
                   space_id_t *self);

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
caller has already validated the checksum of the bytes read from disk. A
compressed page keeps its FIL and page headers uncompressed at the same offsets
in both copies, so both are patched alike.
@param[in]  map  ancestor map of the tablespace
@param[in]  space_id  this tablespace
@param[in]  page_size  tablespace page size
@param[in,out]  frame  uncompressed page frame, or nullptr
@param[in,out]  zip  compressed page, or nullptr */
void wt_translate_page(const wt_map_t &map, space_id_t space_id,
                       const page_size_t &page_size, byte *frame, byte *zip);

/** Recompute the page checksum in RAM, the way a flush would. */
void wt_page_recalc_checksum(byte *frame, const page_size_t &page_size);

#endif /* fil0wt_h */
