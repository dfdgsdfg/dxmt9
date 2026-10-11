#pragma once

#include "dxmt9/device_c.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace dxmt9::d3d9 {

// Cold arm observation, not content-validity or replay authority. Logical
// storage aliases name the original wire generation, never a Metal pointer.
inline constexpr std::uint64_t kAttachmentHistoryMagic = 0x31484154523944ull;
inline constexpr std::uint32_t kAttachmentHistoryMaxRows = 4096u;
struct RenderTapeAttachmentHistoryHeader {
  std::uint64_t magic = kAttachmentHistoryMagic;
  std::uint32_t version = 1u;
  std::uint32_t headerBytes = 64u;
  std::uint64_t token = 0u;
  std::uint64_t epoch = 0u;
  std::uint64_t queue = 0u;
  std::uint64_t completedSequence = 0u;
  std::uint64_t completedInitializer = 0u;
  std::uint32_t count = 0u;
  // bit 0: post-Present discard; bit 1: current backbuffer exists.
  std::uint32_t flags = 0u;
};
struct RenderTapeAttachmentHistoryRow {
  D9CWireObjectIdentity surface{};
  D9CWireObjectIdentity storage{};
  std::uint32_t subresource = 0u;
  // Queue color-load history only, not depth/stencil or content validity.
  std::uint32_t touched = 0u;
  std::uint32_t currentBackbuffer = 0u;
  // Zero explicitly means unknown. Neither touched nor successful readback
  // establishes defined GPU contents. No nonzero authority is implemented.
  std::uint32_t contentAuthority = 0u;
};
static_assert(sizeof(RenderTapeAttachmentHistoryHeader) == 64u);
static_assert(sizeof(RenderTapeAttachmentHistoryRow) == 48u);
static_assert(std::is_trivially_copyable_v<RenderTapeAttachmentHistoryHeader>);
static_assert(std::is_trivially_copyable_v<RenderTapeAttachmentHistoryRow>);

inline bool renderTapeAttachmentHistoryValid(
    const RenderTapeAttachmentHistoryHeader& h,
    std::span<const RenderTapeAttachmentHistoryRow> rows) noexcept {
  if (h.magic != kAttachmentHistoryMagic || h.version != 1u || h.headerBytes != 64u ||
      !h.token || !h.epoch || !h.queue || h.flags > 3u ||
      h.count != rows.size() || rows.size() > kAttachmentHistoryMaxRows ||
      ((h.flags & 1u) && !(h.flags & 2u))) return false;
  std::uint64_t previous = 0u;
  bool current = false;
  for (const auto& row : rows) {
    if (row.surface.kind != D9C_CHUNK_HANDLE_KIND_SURFACE ||
        !row.surface.generation || row.surface.objectId <= previous ||
        !row.storage.generation || !row.storage.objectId ||
        (row.storage.kind != D9C_CHUNK_HANDLE_KIND_SURFACE &&
         row.storage.kind != D9C_CHUNK_HANDLE_KIND_TEXTURE) ||
        row.touched > 1u || row.currentBackbuffer > 1u || row.contentAuthority)
      return false;
    if (row.storage.kind == D9C_CHUNK_HANDLE_KIND_SURFACE &&
        (row.subresource || row.storage.objectId != row.surface.objectId ||
         row.storage.generation != row.surface.generation)) return false;
    previous = row.surface.objectId;
    current |= row.currentBackbuffer != 0u;
  }
  return current == ((h.flags & 2u) != 0u);
}

inline bool encodeRenderTapeAttachmentHistory(
    const RenderTapeAttachmentHistoryHeader& h,
    std::span<const RenderTapeAttachmentHistoryRow> rows,
    std::vector<std::byte>& out) noexcept {
  out.clear();
  if (!renderTapeAttachmentHistoryValid(h, rows)) return false;
  try {
    std::vector<std::byte> bytes(sizeof(h) + rows.size_bytes());
    std::memcpy(bytes.data(), &h, sizeof(h));
    if (!rows.empty()) std::memcpy(bytes.data()+sizeof(h), rows.data(), rows.size_bytes());
    out = std::move(bytes);
    return true;
  } catch (...) { return false; }
}

inline bool decodeRenderTapeAttachmentHistory(std::span<const std::byte> bytes,
    RenderTapeAttachmentHistoryHeader& h,
    std::vector<RenderTapeAttachmentHistoryRow>& rows) noexcept {
  h = {}; rows.clear();
  if (bytes.size() < sizeof(h)) return false;
  RenderTapeAttachmentHistoryHeader candidate;
  std::memcpy(&candidate, bytes.data(), sizeof(candidate));
  if (candidate.count > kAttachmentHistoryMaxRows ||
      bytes.size() != sizeof(h) + std::size_t(candidate.count)*sizeof(RenderTapeAttachmentHistoryRow))
    return false;
  try {
    std::vector<RenderTapeAttachmentHistoryRow> pending(candidate.count);
    if (!pending.empty()) std::memcpy(pending.data(), bytes.data()+sizeof(h), pending.size()*sizeof(pending[0]));
    if (!renderTapeAttachmentHistoryValid(candidate, pending)) return false;
    h = candidate; rows = std::move(pending); return true;
  } catch (...) { return false; }
}
} // namespace dxmt9::d3d9
