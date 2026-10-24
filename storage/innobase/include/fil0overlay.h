/*****************************************************************************

Copyright (c) 2026, MySQL worktrees contributors.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

*****************************************************************************/

/** @file include/fil0overlay.h
 Worktrees in overlay mode (limits/overlay-v3.md, limits/overlay-v3-addendum.md):
 for filesystems without reflink. An overlay tablespace is a sparse file the
 size of its parent: page 0 and every page it has written are local, every
 other page is a hole, and a hole is read from the parent layer chain.
 Layers are immutable files in #wtlayers/ (hard links). */

#ifndef fil0overlay_h
#define fil0overlay_h

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include "fil0fil.h"
#include "fil0wt.h"
#include "os0file.h"
#include "page0size.h"

/** FSP flag bit 15 (the first unused bit) marks an overlay file. A stock
server rejects the file instead of reading its holes as zeros. */
constexpr uint32_t FSP_FLAGS_MASK_WT_OVERLAY = 1U << FSP_FLAGS_POS_UNUSED;

/** Images allowed per thread between two log_free_check() calls, on top of
the stock per-thread margin (v3 §5.2 O5). */
constexpr ulint WT_K_INT = 16;

/** Pages imaged by the first-dirty hook, and images beyond WT_K_INT in one
free-check interval (they draw on stock's extra margin). */
extern std::atomic<uint64_t> wt_images;
extern std::atomic<uint64_t> wt_image_overdrafts;

/** Images since this thread's last log_free_check(). */
extern thread_local ulint wt_img_since_check;

/** Overlay tablespaces attached now. While any exist, settings that would
break their crash safety are refused (limits/overlay-v3.md §5.4). */
extern std::atomic<int> wt_overlay_spaces;

/** Layer names in #wtlayers (set by GC, adjusted by links and aborts). */
extern std::atomic<int> wt_layer_names;

/** @return true while any overlay may exist: one is attached, or a layer
(which only an overlay can reference) is on disk. */
inline bool wt_overlays_exist() {
  return wt_overlay_spaces.load() > 0 || wt_layer_names.load() > 0;
}

/** @return true if the space is an overlay (its first file was opened) */
bool fil_space_is_wt_overlay(space_id_t space_id);

/** An overlay or live-linked space was freed: run layer GC after the DDL. */
extern std::atomic<bool> wt_gc_wanted;

/** Overlay mode was enabled at startup (innodb_worktree_mode). */
extern bool srv_wt_overlay;

/** innodb_worktree_mode: 0 AUTO, 1 REFLINK, 2 OVERLAY. */
extern ulong srv_wt_mode;

/** Page presence in an overlay, RAM only (v3 §2). */
enum wt_presence_t : uint8_t {
  WT_UNKNOWN = 0,
  WT_ABSENT = 1,
  WT_PRESENT = 2
};

/** An immutable layer file, shared by every overlay that reaches it. */
struct wt_layer_t {
  std::string uuid;
  std::string path;
  pfs_os_file_t fd{};
  /** Size in pages. */
  page_no_t size{};
  /** The layer's own parent if it is a frozen overlay delta, else nullptr. */
  wt_layer_t *parent{};
  /** Overlays and child layers that reach this layer. */
  int refs{};
};

/** State of an overlay tablespace (fil_space_t::wt_ov). */
struct wt_overlay_t {
  wt_link_t link;
  /** Parent first, then its parents. */
  std::vector<wt_layer_t *> chain;
  /** Leaf mutex: never held while acquiring another latch (v3 §2). */
  std::mutex presence_mutex;
  /** One entry per page in [0, link.parent_size). */
  std::vector<uint8_t> presence;

  wt_presence_t get(page_no_t page_no);
  void set(page_no_t page_no, wt_presence_t state);
};

/** Attach the chain of an overlay space and initialize presence from the
file's holes. Called when its first file is opened, before its map is loaded
(the map's overflow pages may be in the chain).
@return false if a layer is unusable (the caller fails the open) */
bool fil_wt_overlay_attach(fil_space_t *space, const wt_link_t &link,
                           const pfs_os_file_t &fd, os_offset_t file_size);

/** If buf (page_no of space, as read from the local file) is all zeros,
replace it with the page from the chain. For metadata reads at open (the
map's overflow pages), outside the buffer pool.
@return false on a read error */
bool fil_wt_read_chain(fil_space_t *space, page_no_t page_no,
                       const page_size_t &page_size, byte *buf);

/** Free the overlay state of a space being freed. */
void fil_wt_overlay_free(fil_space_t *space);

/** Read routing (v3 §4), called in Fil_shard::do_io under the shard mutex for
a read of an overlay space. Chooses where the first read goes.
@param[in]  space  overlay space
@param[in]  page_no  page
@param[in,out]  handle  local handle in, chosen handle out
@return source tag: 0 local, k > 0 chain layer k - 1 */
uint8_t fil_wt_route_read(fil_space_t *space, page_no_t page_no,
                          pfs_os_file_t *handle);

/** Finish a routed read before complete_io (v3 §4 rule 2): if what was read
is all zeros, read the next sources synchronously; publish presence.
@param[in]  space  overlay space
@param[in]  req  the read request (encryption info)
@param[in]  name  local file name (messages)
@param[in]  page_no  page
@param[in]  page_size  page size
@param[in,out]  buf  page buffer (physical size)
@param[in]  src  tag returned by fil_wt_route_read
@return final source tag: 0 local, k chain layer k - 1, 255 zero everywhere */
uint8_t fil_wt_read_done(fil_space_t *space, const IORequest &req,
                         const char *name, page_no_t page_no,
                         const page_size_t &page_size, byte *buf,
                         uint8_t src);

/** A write of page_no was submitted: it is local from now on. */
void fil_wt_note_write(fil_space_t *space, page_no_t page_no);

/** @return true if pages of space with page_no must be imaged by the
first-dirty hook (v3 §5.2): an overlay page not yet PRESENT in the parent
range, or any page >= 1 of a freezing space. */
bool fil_wt_needs_image(fil_space_t *space, page_no_t page_no);

/** Mark page_no PRESENT (after an image, or a page created in the mtr). */
void fil_wt_set_present(fil_space_t *space, page_no_t page_no);

/** Open (or reuse) the layer named uuid and its parents.
@return the layer with a reference taken, or nullptr */
wt_layer_t *wt_layer_acquire(const char *uuid, const page_size_t &page_size);

/** Drop a reference taken by wt_layer_acquire. */
void wt_layer_release(wt_layer_t *layer);

/** Directory of layer files, relative to the datadir. */
constexpr const char *WT_LAYER_DIR = "#wtlayers";

/** @return a new random layer name (32 hex digits) */
std::string wt_new_uuid();

/** @return path of a layer file */
std::string wt_layer_path(const std::string &uuid);

struct buf_block_t;
struct mtr_t;

/** Log a full image of an X-latched page in mtr (the first-dirty hook's
record; flatten adopts chain pages with it). */
void wt_mtr_log_image(mtr_t *mtr, const buf_block_t *block);

/** Flatten's last step (v3 §8 step 5): the space is no longer an overlay.
Detach its chain and drop its map. */
void fil_wt_detach(space_id_t space_id);

/* Freeze support in fil0fil.cc (limits/overlay-v3.md §7.1). */

/** Read the file's modification counter and the space size (F1). */
void fil_wt_counters(space_id_t space_id, int64_t *mc, page_no_t *size);

/** Close the freeze gate and drain pending I/O, fsync and extension (F4b).
@return false on timeout; the gate is then still closed */
bool fil_wt_gate_close(space_id_t space_id, std::chrono::milliseconds timeout,
                       int64_t *mc, page_no_t *size);

/** Open the gate (F6, or abort). */
void fil_wt_gate_open(space_id_t space_id);

/** fsync the old file under the closed gate (F4e). */
bool fil_wt_flush_gated(space_id_t space_id);

/** Swap the space's handle to the file now at its path, and attach its new
chain (F5a, F5b, F5d). */
bool fil_wt_reopen_gated(space_id_t space_id, const wt_link_t &link);

#endif /* fil0overlay_h */
