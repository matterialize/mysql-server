/*****************************************************************************

Copyright (c) 2026, MySQL worktrees contributors.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

*****************************************************************************/

/** @file fil/fil0overlay.cc
 Worktrees in overlay mode: layer registry, presence, read routing
 (limits/overlay-v3.md §2-§4, limits/overlay-v3-addendum.md). */

#include "fil0overlay.h"

#include <fcntl.h>
#include <unistd.h>

#include <map>
#include <random>

#include "fil0wt.h"
#include "fsp0fsp.h"
#include "mach0data.h"
#include "srv0srv.h"

bool srv_wt_overlay = false;
ulong srv_wt_mode = 0;

namespace {
std::mutex wt_registry_mutex;
std::map<std::string, wt_layer_t *> wt_registry;

bool is_zero(const byte *p, ulint n) {
  const auto *w = reinterpret_cast<const uint64_t *>(p);
  for (ulint i = 0; i < n / 8; ++i) {
    if (w[i] != 0) return false;
  }
  return true;
}
}  // namespace

wt_presence_t wt_overlay_t::get(page_no_t page_no) {
  const std::lock_guard<std::mutex> guard(presence_mutex);
  return page_no < presence.size() ? wt_presence_t(presence[page_no])
                                   : WT_PRESENT;
}

void wt_overlay_t::set(page_no_t page_no, wt_presence_t state) {
  const std::lock_guard<std::mutex> guard(presence_mutex);
  if (page_no < presence.size()) presence[page_no] = state;
}

std::string wt_new_uuid() {
  static std::mutex mutex;
  static std::mt19937_64 rng(std::random_device{}() ^
                             static_cast<uint64_t>(time(nullptr)));
  const std::lock_guard<std::mutex> guard(mutex);
  char buf[33];
  snprintf(buf, sizeof(buf), "%016llx%016llx",
           static_cast<unsigned long long>(rng()),
           static_cast<unsigned long long>(rng()));
  return buf;
}

std::string wt_layer_path(const std::string &uuid) {
  return std::string(WT_LAYER_DIR) + "/" + uuid + ".ibl";
}

namespace {
/** Open one layer file (registry mutex held). Its parents are acquired by
the caller, which knows them from the layer's own map. */
wt_layer_t *layer_open(const std::string &uuid, const page_size_t &page_size,
                       const Encryption_metadata &enc, size_t depth = 0) {
  /* Chains are capped (addendum A5); a damaged link can't recurse forever. */
  if (depth > WT_MAX_ANCESTORS) {
    ib::error() << "Worktree layer chain through " << uuid << " is too deep";
    return nullptr;
  }
  auto it = wt_registry.find(uuid);
  if (it != wt_registry.end()) {
    ++it->second->refs;
    return it->second;
  }
  const std::string path = wt_layer_path(uuid);
  bool success = false;
  pfs_os_file_t fd =
      os_file_create(innodb_data_file_key, path.c_str(), OS_FILE_OPEN,
                     OS_DATA_FILE, true, &success);
  if (!success) {
    ib::error() << "Worktree layer " << path << " cannot be opened";
    return nullptr;
  }
  auto layer = ut::new_withkey<wt_layer_t>(UT_NEW_THIS_FILE_PSI_KEY);
  layer->uuid = uuid;
  layer->path = path;
  layer->fd = fd;
  layer->size =
      static_cast<page_no_t>(os_file_get_size(fd) / page_size.physical());
  layer->refs = 1;

  /* A frozen overlay delta links its own parent. */
  const ulint len = page_size.physical();
  auto buf = static_cast<byte *>(ut::aligned_alloc(2 * len, len));
  wt_map_t map;
  const wt_page_reader read = [&](page_no_t no, byte *b) {
    IORequest req(IORequest::READ);
    if (no != 0 && enc.can_encrypt()) req.get_encryption_info().set(enc);
    return os_file_read_no_error_handling(req, path.c_str(), fd, b,
                                          os_offset_t{no} * len, len,
                                          nullptr) == DB_SUCCESS;
  };
  if (read(0, buf) &&
      (fsp_header_get_flags(buf) & FSP_FLAGS_MASK_WT_OVERLAY) != 0) {
    const bool ok = (wt_map_read(buf, page_size, 0, read, &map) ||
                     (read(1, buf + len) &&
                      wt_map_read(buf + len, page_size, 1, read, &map))) &&
                    map.has_link;
    if (!ok) {
      ib::error() << "Worktree layer " << path << " has no valid link";
      ut::aligned_free(buf);
      os_file_close(fd);
      ut::delete_(layer);
      return nullptr;
    }
    layer->parent = layer_open(map.link.uuid, page_size, enc, depth + 1);
    if (layer->parent == nullptr) {
      ut::aligned_free(buf);
      os_file_close(fd);
      ut::delete_(layer);
      return nullptr;
    }
  }
  ut::aligned_free(buf);
  wt_registry[uuid] = layer;
  return layer;
}

void layer_release_low(wt_layer_t *layer) {
  while (layer != nullptr && --layer->refs == 0) {
    wt_layer_t *parent = layer->parent;
    wt_registry.erase(layer->uuid);
    os_file_close(layer->fd);
    ut::delete_(layer);
    layer = parent;
  }
}
}  // namespace

wt_layer_t *wt_layer_acquire(const char *uuid, const page_size_t &page_size) {
  const std::lock_guard<std::mutex> guard(wt_registry_mutex);
  return layer_open(uuid, page_size, Encryption_metadata{});
}

void wt_layer_release(wt_layer_t *layer) {
  const std::lock_guard<std::mutex> guard(wt_registry_mutex);
  layer_release_low(layer);
}

bool fil_wt_overlay_attach(fil_space_t *space, const wt_link_t &link,
                           const pfs_os_file_t &fd, os_offset_t file_size) {
  const page_size_t page_size(space->flags);
  wt_layer_t *top;
  {
    const std::lock_guard<std::mutex> guard(wt_registry_mutex);
    top = layer_open(link.uuid, page_size, space->m_encryption_metadata);
  }
  if (top == nullptr) return false;

  if (link.parent_size > top->size) {
    ib::error() << "Overlay tablespace " << space->name << " links "
                << link.parent_size << " pages of a " << top->size
                << "-page layer";
    wt_layer_release(top);
    return false;
  }
  auto ov = ut::new_withkey<wt_overlay_t>(UT_NEW_THIS_FILE_PSI_KEY);
  ov->link = link;
  for (wt_layer_t *l = top; l != nullptr; l = l->parent) ov->chain.push_back(l);
  ov->presence.assign(link.parent_size, WT_UNKNOWN);

  /* Holes are ABSENT; data pages stay UNKNOWN until read (an allocated page
  may still be zero). Pages 0 and 1 are always local. */
  const int raw = fd.m_file;
  const os_offset_t len = page_size.physical();
  const os_offset_t end =
      std::min<os_offset_t>(file_size, os_offset_t{link.parent_size} * len);
  os_offset_t pos = 0;
  while (pos < end) {
    const off_t data = lseek(raw, pos, SEEK_DATA);
    const os_offset_t data_at =
        data < 0 ? end : std::min<os_offset_t>(end, os_offset_t(data));
    for (os_offset_t p = (pos + len - 1) / len; p * len + len <= data_at; ++p) {
      ov->presence[p] = WT_ABSENT;
    }
    if (data < 0 || data_at >= end) break;
    const off_t hole = lseek(raw, data_at, SEEK_HOLE);
    pos = hole < 0 ? end : os_offset_t(hole);
  }
  for (page_no_t p : {page_no_t{0}, page_no_t{1}}) {
    if (p < ov->presence.size()) ov->presence[p] = WT_PRESENT;
  }
  /* Only durable bytes may later read as "local, non-zero" (v3 §6.1 step 2). */
  (void)fsync(raw);
  space->wt_ov = ov;
  return true;
}

bool fil_wt_read_chain(fil_space_t *space, page_no_t page_no,
                       const page_size_t &page_size, byte *buf) {
  wt_overlay_t *ov = space->wt_ov;
  const ulint len = page_size.physical();
  if (page_no == 0 || page_no >= ov->presence.size() || !is_zero(buf, len))
    return true;
  for (wt_layer_t *layer : ov->chain) {
    if (page_no >= layer->size) continue;
    IORequest req(IORequest::READ);
    if (space->m_encryption_metadata.can_encrypt())
      req.get_encryption_info().set(space->m_encryption_metadata);
    if (os_file_read_no_error_handling(req, layer->path.c_str(), layer->fd, buf,
                                       os_offset_t{page_no} * len, len,
                                       nullptr) != DB_SUCCESS)
      return false;
    if (!is_zero(buf, len)) return true;
  }
  return true;
}

void fil_wt_overlay_free(fil_space_t *space) {
  if (space->wt_ov == nullptr) return;
  if (!space->wt_ov->chain.empty()) wt_layer_release(space->wt_ov->chain[0]);
  ut::delete_(space->wt_ov);
  space->wt_ov = nullptr;
}

uint8_t fil_wt_route_read(fil_space_t *space, page_no_t page_no,
                          pfs_os_file_t *handle) {
  wt_overlay_t *ov = space->wt_ov;
  if (page_no == 0 || page_no >= ov->presence.size()) return 0;
  switch (ov->get(page_no)) {
    case WT_PRESENT:
    case WT_UNKNOWN:
      return 0;
    case WT_ABSENT:
      break;
  }
  *handle = ov->chain[0]->fd;
  return 1;
}

uint8_t fil_wt_read_done(fil_space_t *space, const IORequest &req,
                         const char *name, page_no_t page_no,
                         const page_size_t &page_size, byte *buf,
                         uint8_t src) {
  wt_overlay_t *ov = space->wt_ov;
  if (page_no == 0 || page_no >= ov->presence.size()) return 0;
  const ulint len = page_size.physical();
  if (src == 0) {
    if (!is_zero(buf, len)) {
      ov->set(page_no, WT_PRESENT);
      return 0;
    }
    ov->set(page_no, WT_ABSENT);
  } else if (!is_zero(buf, len)) {
    return src;
  }
  /* Zero at the source tried: the next layers, synchronously (never resubmit
  os_aio from an I/O handler thread, v3 §4). */
  for (size_t k = src; k < ov->chain.size(); ++k) {
    wt_layer_t *layer = ov->chain[k];
    if (page_no >= layer->size) continue;
    IORequest read(req);
    if (os_file_read_no_error_handling(read, layer->path.c_str(), layer->fd,
                                       buf, os_offset_t{page_no} * len, len,
                                       nullptr) != DB_SUCCESS) {
      ib::fatal(UT_LOCATION_HERE)
          << "Cannot read page " << page_no << " of worktree layer "
          << layer->path << " for " << name;
    }
    if (!is_zero(buf, len)) return static_cast<uint8_t>(k + 1);
  }
  memset(buf, 0, len);
  return 255;
}

void fil_wt_note_write(fil_space_t *space, page_no_t page_no) {
  if (page_no != 0) space->wt_ov->set(page_no, WT_PRESENT);
}

bool fil_wt_needs_image(fil_space_t *space, page_no_t page_no) {
  if (page_no == 0) return false;
  if (space->wt_freezing.load()) return true;
  wt_overlay_t *ov = space->wt_ov;
  return ov != nullptr && page_no < ov->presence.size() &&
         ov->get(page_no) != WT_PRESENT;
}

void fil_wt_set_present(fil_space_t *space, page_no_t page_no) {
  if (space->wt_ov != nullptr) space->wt_ov->set(page_no, WT_PRESENT);
}
