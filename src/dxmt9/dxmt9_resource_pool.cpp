#include "dxmt9_resource_pool.hpp"

#include "dxmt9/assert.hpp"
#include "dxmt9_debug_trace.hpp"
#include "dxmt9_format_convert.hpp"
#include "dxmt9_metal_labels.hpp"
#include "dxmt9_perf_counters.hpp"
#include "dxmt9_queue.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <span>
#include <sstream>
#include <vector>

namespace dxmt9::resources {

BufferRecord* Pool::findBuffer(u64 handle) noexcept {
  return bufferArena_.find(handle);
}

const BufferRecord* Pool::findBuffer(u64 handle) const noexcept {
  return bufferArena_.find(handle);
}

TextureRecord* Pool::findTexture(u64 handle) noexcept {
  return textureArena_.find(handle);
}

const TextureRecord* Pool::findTexture(u64 handle) const noexcept {
  return textureArena_.find(handle);
}

SurfaceRecord* Pool::findSurface(u64 handle) noexcept {
  return surfaceArena_.find(handle);
}

const SurfaceRecord* Pool::findSurface(u64 handle) const noexcept {
  return surfaceArena_.find(handle);
}

namespace {
struct TextureSubresource {
  u32 mipLevel = 0;
  u32 slice = 0;
  bool valid = true;
};

TextureSubresource decodeTextureSubresource(const core::TextureDesc& desc, u32 subresource) {
  const u32 mipLevels = std::max(1u, desc.levels);
  if (desc.type == core::TextureType::Cube) {
    const u32 slice = subresource / mipLevels;
    if (slice >= 6u) {
      return {.mipLevel = 0, .slice = 0, .valid = false};
    }
    return {.mipLevel = subresource % mipLevels, .slice = slice, .valid = true};
  }
  if (subresource >= mipLevels) {
    return {.mipLevel = 0, .slice = 0, .valid = false};
  }
  return {.mipLevel = subresource, .slice = 0, .valid = true};
}

bool shouldTraceResourceHandle(core::Handle handle) {
  return core::metalqueue::queueTraceEnabled() || dxmt9::debug::shouldTraceTexture(handle);
}

void traceTextureCreate(core::TextureHandle handle, const core::TextureDesc& desc,
                        bool hasMetalTexture, bool needsStagingBlit,
                        bool isManagedDiscrete) {
  if (!shouldTraceResourceHandle(handle)) {
    return;
  }
  std::ostringstream out;
  out << "[dxmt9-resource] texture create handle=0x" << std::hex << handle.value
      << std::dec << " size=" << desc.width << "x" << desc.height << "x" << desc.depth
      << " levels=" << desc.levels
      << " usage=0x" << std::hex << static_cast<u32>(desc.usage) << std::dec
      << " fmt=" << static_cast<u32>(desc.format)
      << " pool=" << static_cast<u32>(desc.pool)
      << " type=" << static_cast<u32>(desc.type)
      << " metal=" << (hasMetalTexture ? 1 : 0)
      << " staging_blit=" << (needsStagingBlit ? 1 : 0)
      << " managed_discrete=" << (isManagedDiscrete ? 1 : 0);
  core::metalqueue::emitTextureTraceLine(out.str());
}

void traceSurfaceCreate(core::SurfaceHandle handle, const core::SurfaceDesc& desc,
                        bool hasMetalTexture) {
  if (!core::metalqueue::queueTraceEnabled()) {
    return;
  }
  std::ostringstream out;
  out << "[dxmt9-resource] surface create handle=0x" << std::hex << handle.value
      << std::dec << " size=" << desc.width << "x" << desc.height
      << " usage=0x" << std::hex << static_cast<u32>(desc.usage) << std::dec
      << " fmt=" << static_cast<u32>(desc.format)
      << " pool=" << static_cast<u32>(desc.pool)
      << " msaa=" << static_cast<u32>(desc.multiSampleType)
      << " metal=" << (hasMetalTexture ? 1 : 0);
  core::metalqueue::emitTextureTraceLine(out.str());
}

void traceSurfaceAlias(core::SurfaceHandle handle,
                       core::TextureHandle texture,
                       u32 subresource,
                       TextureSubresource decoded,
                       const core::SurfaceDesc& desc,
                       bool usedParentTexture,
                       bool hasMetalTexture) {
  if (!shouldTraceResourceHandle(texture)) {
    return;
  }
  std::ostringstream out;
  out << "[dxmt9-resource] surface alias handle=0x" << std::hex << handle.value
      << " texture=0x" << texture.value << std::dec
      << " subresource=" << subresource
      << " mip=" << decoded.mipLevel
      << " slice=" << decoded.slice
      << " size=" << desc.width << "x" << desc.height
      << " usage=0x" << std::hex << static_cast<u32>(desc.usage) << std::dec
      << " fmt=" << static_cast<u32>(desc.format)
      << " parent=" << (usedParentTexture ? 1 : 0)
      << " metal=" << (hasMetalTexture ? 1 : 0);
  core::metalqueue::emitTextureTraceLine(out.str());
}

// R-BACK-14.4 — heap-backed records propagate their last-used seqId
// back to HeapManager before the WMT::Reference releases the
// suballocation. Surfaces never live on a heap (RT/DS attachments
// always allocate direct per R-BACK-14.2), so the surface overload
// is a no-op.
template <typename Record>
void releaseHeapIfBacked(HeapManager* /*heapManager*/, const Record& /*record*/) {
  // SurfaceRecord (and any future record type without isHeapBacked)
  // hits this default — no heap accounting needed.
}

void releaseHeapIfBacked(HeapManager* heapManager, const BufferRecord& record) {
  if (heapManager && record.isHeapBacked && record.heap.handle != 0) {
    heapManager->releaseHeapMember(record.heap.handle, record.lastUsedSeqId);
  }
}

void releaseHeapIfBacked(HeapManager* heapManager, const TextureRecord& record) {
  if (heapManager && record.isHeapBacked && record.heap.handle != 0) {
    heapManager->releaseHeapMember(record.heap.handle, record.lastUsedSeqId);
  }
}

template <typename Arena>
void gcArena(Arena& arena, u64 completedSeqId, HeapManager* heapManager) {
  arena.reclaimCompleted(completedSeqId, [completedSeqId, heapManager](const auto& record) {
    // TLA+: NoUseAfterFree
    DXMT_ASSERT(record.lastUsedSeqId <= completedSeqId);
    releaseHeapIfBacked(heapManager, record);
  });
}
}  // namespace

void Pool::reclaimCompleted(u64 completedSeqId) {
  gcArena(bufferArena_, completedSeqId, &heapManager_);
  gcArena(textureArena_, completedSeqId, &heapManager_);
  // Surfaces never use heap allocation (RT/DS ineligible per
  // R-BACK-14.2). Pass nullptr; the type-dispatched overload above
  // is a no-op for SurfaceRecord either way.
  gcArena(surfaceArena_, completedSeqId, nullptr);
  // R-BACK-14.4 / 14.5 — retire heap instances whose live-member count
  // reached zero AND whose final member's lastUsedSeqId has been
  // GPU-completed. Each retirement bumps `heap_compaction_count`.
  heapManager_.retireFreedHeaps(completedSeqId);
}

bool Pool::markBufferDestroyAndGc(u64 handleValue, u64 completedSeqId) {
  auto* record = bufferArena_.find(handleValue);
  if (!record) {
    return false;
  }
  record->destroyPending = true;
  reclaimCompleted(completedSeqId);
  return true;
}

bool Pool::markTextureDestroyAndGc(u64 handleValue, u64 completedSeqId) {
  auto* record = textureArena_.find(handleValue);
  if (!record) {
    return false;
  }
  record->destroyPending = true;
  reclaimCompleted(completedSeqId);
  return true;
}

bool Pool::markSurfaceDestroyAndGc(u64 handleValue, u64 completedSeqId) {
  auto* record = surfaceArena_.find(handleValue);
  if (!record) {
    return false;
  }
  record->destroyPending = true;
  reclaimCompleted(completedSeqId);
  return true;
}

core::BufferHandle Pool::createBuffer(WMT::Device device, const core::BufferDesc& desc) {
  BufferRecord record;
  record.desc = desc;
  record.shadow.resize(static_cast<std::size_t>(desc.size));
  if (desc.pool != core::Pool::SystemMem && desc.pool != core::Pool::Scratch) {
    WMTBufferInfo info{};
    info.length = desc.size;
    // R-BACK-5.7 / 5.8: buffer storage selection. The current buffer path
    // keeps everything Shared (DEFAULT+DYNAMIC rename ring lives on
    // Shared per R-BACK-5.8; MANAGED on unified memory is also Shared per
    // R-BACK-5.7). DEFAULT non-DYNAMIC buffers are bound through the
    // Shared shadow today — promoting them to Private is tracked
    // separately as buffer-side R-BACK-5.7. The discrete-MANAGED buffer
    // path is the same TODO item; the texture path below is what
    // R-BACK-5.7 immediately requires.
    info.options = WMTResourceStorageModeShared;
    // R-BACK-14.* — try the small-resource heap before falling through
    // to a direct device allocation. classify() rejects RT/DS/Dynamic
    // and footprints over the threshold; allocBuffer further rejects
    // when no heap satisfies the request and grow fails. On any of
    // those rejections the direct allocation below runs unchanged and
    // we count the fallback so the regression dashboard catches an
    // overly-conservative heuristic.
    const auto eligibility = heapManager_.classifyBuffer(info.length, desc.pool, desc.usage);
    if (eligibility.eligible) {
      WMT::Heap heap{};
      auto heapBuf = heapManager_.allocBuffer(eligibility.family, info.length,
                                                info.options, heap);
      if (heapBuf) {
        record.buffer = std::move(heapBuf);
        record.isHeapBacked = true;
        record.heap = heap;
        // Heap-backed Shared buffers do not currently expose a
        // host-mapped pointer through the bridge; CPU writes go through
        // the shadow-copy path (uploadBufferData mirrors via shadow
        // when contents is null). Keep contents nullptr to flag that.
        record.contents = nullptr;
        perf::countMetalBuffer(static_cast<std::size_t>(info.length));
      } else {
        perf::countHeapDirectFallback();
      }
    }
    if (!record.buffer) {
      record.buffer = device.newBuffer(info);
      if (record.buffer) {
        perf::countMetalBuffer(static_cast<std::size_t>(info.length));
      }
      record.contents = info.memory.ptr;  // shared mode: contents ptr returned in info
    }
  }
  const auto handle = bufferArena_.insert(std::move(record));
  if (auto* stored = bufferArena_.find(handle.value); stored && stored->buffer) {
    stored->buffer.setLabel(labels::makeLabelStringFmt(
        "pool_buf_h0x%llx_len%llu",
        static_cast<unsigned long long>(handle.value),
        static_cast<unsigned long long>(desc.size)));
    if (stored->isHeapBacked) {
      heapManager_.retainHeapMember(stored->heap.handle, 0);
    }
  }
  return handle;
}

core::TextureHandle Pool::createTexture(WMT::Device device,
                                          const core::BackendLimits& limits,
                                          const core::TextureDesc& desc) {
  TextureRecord record;
  record.desc = desc;
  if (desc.pool != core::Pool::SystemMem && desc.pool != core::Pool::Scratch) {
    WMTTextureInfo info{};
    info.type = convert::toTextureType(desc.type, false);
    info.pixel_format = convert::toPixelFormat(desc.format, limits);
    info.width = std::max(1u, desc.width);
    info.height = std::max(1u, desc.height);
    info.depth = std::max(1u, desc.depth);
    info.mipmap_level_count = std::max(1u, desc.levels);
    info.sample_count = 1;
    info.array_length = 1;
    // R-BACK-5.7: select storage mode from the cached unified-memory
    // probe. On Apple Silicon, MANAGED collapses to Shared (no staging);
    // on discrete, MANAGED becomes Managed (staging blit is wired up
    // below). Selection is one-shot at create time.
    info.options = convert::toResourceOptions(desc.pool, desc.usage, hasUnifiedMemory_);
    info.usage = convert::toTextureUsage(desc);
    // R-BACK-14.* — heap-eligibility probe. The footprint estimate uses
    // bytesPerPixel + width*height*depth (compressed-format rows are
    // tiny in absolute terms — well under the 64 KB threshold either
    // way), giving a portable upper bound for the per-mip-0 footprint
    // without consulting Metal. classifyTexture rejects RT/DS, Dynamic,
    // and footprints above kHeapEligibilityFootprintBytes; on a
    // discrete-memory device the MANAGED branch is also rejected
    // (Shared heap profile would mismatch).
    const auto bpp = core::bytesPerPixel(desc.format);
    const auto footprint = static_cast<std::uint64_t>(bpp) *
                            static_cast<std::uint64_t>(info.width) *
                            static_cast<std::uint64_t>(info.height) *
                            static_cast<std::uint64_t>(info.depth);
    const auto eligibility = heapManager_.classifyTexture(footprint, desc.pool, desc.usage);
    if (eligibility.eligible) {
      WMT::Heap heap{};
      auto heapTex = heapManager_.allocTexture(eligibility.family, info, heap);
      if (heapTex) {
        record.texture = std::move(heapTex);
        record.isHeapBacked = true;
        record.heap = heap;
      } else {
        perf::countHeapDirectFallback();
      }
    }
    if (!record.texture) {
      record.texture = device.newTexture(info);
    }
    // Both Private and Managed (discrete) reach the texture through a
    // staging-blit upload path — for Private because the CPU cannot
    // directly write to it, for Managed-discrete because we must pump
    // bytes across the device-local-memory boundary once and have an
    // observable counter (`countManagedTextureUploadBlit`) for the
    // R-BACK-5.7 regression check (Apple Silicon must keep that counter
    // at 0). Shared storage uses `replaceRegion` directly — no blit.
    record.needsStagingBlit = (info.options == WMTResourceStorageModePrivate) ||
                              (info.options == WMTResourceStorageModeManaged);
    record.isManagedDiscrete = (info.options == WMTResourceStorageModeManaged);
  }
  const auto handle = textureArena_.insert(std::move(record));
  auto* stored = textureArena_.find(handle.value);
  if (stored && stored->texture) {
    stored->texture.setLabel(labels::makeLabelStringFmt(
        "pool_tex_h0x%llx_fmt%u_%ux%ux%u_l%u",
        static_cast<unsigned long long>(handle.value),
        static_cast<unsigned>(desc.format),
        std::max(1u, desc.width),
        std::max(1u, desc.height),
        std::max(1u, desc.depth),
        std::max(1u, desc.levels)));
  }
  if (stored && stored->isHeapBacked) {
    heapManager_.retainHeapMember(stored->heap.handle, 0);
  }
  traceTextureCreate(handle, desc, stored && stored->texture,
                     stored ? stored->needsStagingBlit : false,
                     stored ? stored->isManagedDiscrete : false);
  return handle;
}

core::SurfaceHandle Pool::createSurface(WMT::Device device,
                                          const core::BackendLimits& limits,
                                          const core::SurfaceDesc& desc) {
  SurfaceRecord record;
  record.desc = desc;
  if (desc.pool != core::Pool::SystemMem && desc.pool != core::Pool::Scratch) {
    const uint32_t sc = std::max(1u, core::sampleCount(desc.multiSampleType));
    WMTTextureInfo info{};
    info.type = convert::toTextureType(core::TextureType::TwoD,
                                         desc.multiSampleType != core::MultiSampleType::None);
    info.pixel_format = convert::toPixelFormat(desc.format, limits);
    info.width = std::max(1u, desc.width);
    info.height = std::max(1u, desc.height);
    info.depth = 1;
    info.mipmap_level_count = 1;
    info.sample_count = sc;
    info.array_length = 1;
    // Surfaces are RT/DS attachments; the storage mode is independent of
    // pool/usage and effectively always Private on every device. The
    // hasUnifiedMemory_ branch only matters for the MANAGED case which
    // surfaces never carry, but we pass the flag for symmetry with the
    // texture path so a future surface category change picks the right
    // mode automatically.
    info.options = convert::toResourceOptions(desc.pool, desc.usage, hasUnifiedMemory_);
    info.usage = convert::toTextureUsage(desc);
    record.texture = device.newTexture(info);
    if (sc > 1) {
      WMTTextureInfo resolveInfo = info;
      resolveInfo.sample_count = 1;
      resolveInfo.type = WMTTextureType2D;
      resolveInfo.usage =
          static_cast<WMTTextureUsage>(WMTTextureUsageShaderRead | WMTTextureUsageRenderTarget);
      record.resolveTexture = device.newTexture(resolveInfo);
    }
  }
  const auto handle = surfaceArena_.insert(std::move(record));
  auto* stored = surfaceArena_.find(handle.value);
  if (stored) {
    if (stored->texture) {
      stored->texture.setLabel(labels::makeLabelStringFmt(
          "pool_rt_h0x%llx_fmt%u_%ux%u_msaa%u",
          static_cast<unsigned long long>(handle.value),
          static_cast<unsigned>(desc.format),
          std::max(1u, desc.width),
          std::max(1u, desc.height),
          std::max(1u, core::sampleCount(desc.multiSampleType))));
    }
    if (stored->resolveTexture) {
      stored->resolveTexture.setLabel(labels::makeLabelStringFmt(
          "pool_rt_resolve_h0x%llx_fmt%u_%ux%u",
          static_cast<unsigned long long>(handle.value),
          static_cast<unsigned>(desc.format),
          std::max(1u, desc.width),
          std::max(1u, desc.height)));
    }
  }
  traceSurfaceCreate(handle, desc, stored && stored->texture);
  return handle;
}

core::SurfaceHandle Pool::createSurfaceForTexture(core::TextureHandle textureHandle,
                                                    u32 level,
                                                    const core::SurfaceDesc& desc) {
  auto* textureRecord = findTexture(textureHandle.value);
  if (!textureRecord || !textureRecord->texture) {
    return {};
  }
  SurfaceRecord record;
  record.desc = desc;
  record.aliasTexture = textureHandle;
  const auto subresource = decodeTextureSubresource(textureRecord->desc, level);
  if (!subresource.valid) {
    return {};
  }
  record.level = 0;
  record.slice = 0;
  bool usedParentTexture = false;
  WMT::Texture parentTexture{textureRecord->texture.handle};
  if (textureRecord->desc.type != core::TextureType::Cube &&
      subresource.mipLevel == 0 && desc.width == textureRecord->desc.width &&
      desc.height == textureRecord->desc.height) {
    record.texture = WMT::Reference<WMT::Texture>(parentTexture);
    usedParentTexture = true;
  } else {
    WMTTextureSwizzleChannels swizzle{
        WMTTextureSwizzleRed, WMTTextureSwizzleGreen,
        WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha};
    uint64_t gpuId = 0;
    const auto viewType = textureRecord->desc.type == core::TextureType::Cube
                              ? WMTTextureType2D
                              : parentTexture.textureType();
    auto view = parentTexture.newTextureView(parentTexture.pixelFormat(),
                                               viewType,
                                               subresource.mipLevel, 1,
                                               subresource.slice, 1, swizzle, gpuId);
    if (!view && textureRecord->desc.type == core::TextureType::Cube) {
      return {};
    }
    record.texture = view ? std::move(view) : WMT::Reference<WMT::Texture>(parentTexture);
    usedParentTexture = !view;
  }
  const auto handle = surfaceArena_.insert(std::move(record));
  const auto* stored = surfaceArena_.find(handle.value);
  traceSurfaceAlias(handle, textureHandle, level, subresource, desc, usedParentTexture,
                    stored && stored->texture);
  return handle;
}

namespace {

std::span<const std::uint8_t> normalizeUploadBytes(core::Format format, u32 width, u32 height,
                                                     u32 pitch,
                                                     std::span<const std::uint8_t> bytes,
                                                     std::vector<std::uint8_t>& scratch) {
  if (bytes.empty()) {
    return bytes;
  }
  switch (format) {
    case core::Format::X8R8G8B8:
    case core::Format::X8B8G8R8: {
      const std::size_t rowBytes = static_cast<std::size_t>(pitch);
      const std::size_t expected = rowBytes * static_cast<std::size_t>(height);
      if (expected == 0 || bytes.size() < expected) {
        return bytes;
      }
      scratch.assign(bytes.begin(), bytes.begin() + expected);
      for (u32 y = 0; y < height; ++y) {
        std::uint8_t* row = scratch.data() + static_cast<std::size_t>(y) * rowBytes;
        for (u32 x = 0; x < width; ++x) {
          row[static_cast<std::size_t>(x) * 4 + 3] = 0xffu;
        }
      }
      return scratch;
    }
    case core::Format::X1R5G5B5: {
      const std::size_t rowBytes = static_cast<std::size_t>(pitch);
      const std::size_t expected = rowBytes * static_cast<std::size_t>(height);
      if (expected == 0 || bytes.size() < expected) {
        return bytes;
      }
      scratch.assign(bytes.begin(), bytes.begin() + expected);
      for (u32 y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<std::uint16_t*>(scratch.data() +
                                                       static_cast<std::size_t>(y) * rowBytes);
        for (u32 x = 0; x < width; ++x) {
          row[x] |= 0x8000u;
        }
      }
      return scratch;
    }
    default:
      return bytes;
  }
}

}  // namespace

std::optional<Pool::StagingCopy>
Pool::stageTextureUpload(WMT::Device device,
                          core::TextureHandle handle,
                          u32 level,
                          u32 width,
                          u32 height,
                          u32 pitch,
                          const std::uint8_t* bytes,
                          std::size_t byteCount) {
  auto* record = findTexture(handle.value);
  if (!record || !record->texture || byteCount == 0) {
    return std::nullopt;
  }

  const auto normalized = normalizeUploadBytes(record->desc.format, width, height, pitch,
                                                  {bytes, byteCount}, textureUploadScratch_);

  const auto subresource = decodeTextureSubresource(record->desc, level);
  if (!subresource.valid) {
    return std::nullopt;
  }
  WMT::Texture texture{record->texture.handle};
  const u32 mipLevel = subresource.mipLevel;
  const u32 slice = subresource.slice;
  const u32 mipWidth = std::max(1u, width);
  const u32 mipHeight = std::max(1u, height);

  if (!record->needsStagingBlit) {
    // Shared-mode (DEFAULT+DYNAMIC, MANAGED on unified memory, etc.):
    // CPU write straight to the destination; no blit and no counter
    // increment. The Apple-Silicon MANAGED branch lands here per
    // R-BACK-5.7 — `countManagedTextureUploadBlit` must remain 0.
    WMTOrigin origin{0, 0, 0};
    WMTSize size{mipWidth, mipHeight, 1};
    texture.replaceRegion(origin, size, mipLevel, slice, normalized.data(), pitch, 0);
    return std::nullopt;
  }

  // Staging-blit path: Private destinations always; Managed destinations
  // on discrete-memory devices (R-BACK-5.7). We populate a Shared staging
  // texture from CPU bytes and queue a blit into the destination — the
  // Initializer flushes the batch before each render chunk.
  // R-BACK-5.7 counter: managedTextureUploadBlitCount/Bytes advance only
  // when the destination is Managed (discrete path); Private destinations
  // (DEFAULT non-DYNAMIC) do not contribute to that counter.
  if (record->isManagedDiscrete) {
    perf::countManagedTextureUploadBlit(byteCount);
  }
  WMTTextureInfo stagingInfo{};
  stagingInfo.type = WMTTextureType2D;
  stagingInfo.pixel_format = texture.pixelFormat();
  stagingInfo.width = mipWidth;
  stagingInfo.height = mipHeight;
  stagingInfo.depth = 1;
  stagingInfo.mipmap_level_count = 1;
  stagingInfo.sample_count = 1;
  stagingInfo.array_length = 1;
  stagingInfo.options = WMTResourceStorageModeShared;
  stagingInfo.usage = WMTTextureUsageShaderRead;
  auto stagingTexture = device.newTexture(stagingInfo);
  if (!stagingTexture) {
    return std::nullopt;
  }
  WMT::Texture{stagingTexture.handle}.setLabel(labels::makeLabelStringFmt(
      "pool_staging_h0x%llx_l%u_%ux%u",
      static_cast<unsigned long long>(record->texture.handle),
      mipLevel, mipWidth, mipHeight));
  {
    WMTOrigin origin{0, 0, 0};
    WMTSize size{mipWidth, mipHeight, 1};
    WMT::Texture{stagingTexture.handle}.replaceRegion(origin, size, 0, 0,
                                                       normalized.data(), pitch, 0);
  }
  return StagingCopy{std::move(stagingTexture), texture, mipLevel, slice, mipWidth, mipHeight};
}

void Pool::uploadTextureLevel(WMT::Device device,
                               WMT::CommandQueue queue,
                               core::TextureHandle handle,
                               u32 level,
                               u32 width,
                               u32 height,
                               u32 pitch,
                               const std::uint8_t* bytes,
                               std::size_t byteCount) {
  auto* record = findTexture(handle.value);
  if (!record || !record->texture || byteCount == 0) {
    return;
  }

  const auto normalized = normalizeUploadBytes(record->desc.format, width, height, pitch,
                                                  {bytes, byteCount}, textureUploadScratch_);

  const auto subresource = decodeTextureSubresource(record->desc, level);
  if (!subresource.valid) {
    return;
  }
  WMT::Texture texture{record->texture.handle};
  const u32 mipLevel = subresource.mipLevel;
  const u32 slice = subresource.slice;
  const u32 mipWidth = std::max(1u, width);
  const u32 mipHeight = std::max(1u, height);

  if (!record->needsStagingBlit) {
    WMTOrigin origin{0, 0, 0};
    WMTSize size{mipWidth, mipHeight, 1};
    texture.replaceRegion(origin, size, mipLevel, slice, normalized.data(), pitch, 0);
    return;
  }

  // Staging-blit path (Private; or Managed on a discrete-memory device).
  // Allocates a Shared staging texture, uploads CPU bytes into it, then
  // blits to the destination synchronously on `queue`. This synchronous
  // entry point is currently unused in production (the Initializer's
  // batched flush is the sole caller path), but mirrors the deferred
  // `stageTextureUpload` so the two stay aligned. The R-BACK-5.7 counter
  // advance lives only on the deferred path which is what the encoder
  // actually drives — adding it here would double-count.
  WMTTextureInfo stagingInfo{};
  stagingInfo.type = WMTTextureType2D;
  stagingInfo.pixel_format = texture.pixelFormat();
  stagingInfo.width = mipWidth;
  stagingInfo.height = mipHeight;
  stagingInfo.depth = 1;
  stagingInfo.mipmap_level_count = 1;
  stagingInfo.sample_count = 1;
  stagingInfo.array_length = 1;
  stagingInfo.options = WMTResourceStorageModeShared;
  stagingInfo.usage = WMTTextureUsageShaderRead;
  auto stagingTexture = device.newTexture(stagingInfo);
  if (!stagingTexture) {
    return;
  }
  WMT::Texture{stagingTexture.handle}.setLabel(labels::makeLabelStringFmt(
      "pool_staging_h0x%llx_l%u_%ux%u",
      static_cast<unsigned long long>(record->texture.handle),
      mipLevel, mipWidth, mipHeight));
  {
    WMTOrigin origin{0, 0, 0};
    WMTSize size{mipWidth, mipHeight, 1};
    WMT::Texture{stagingTexture.handle}.replaceRegion(origin, size, 0, 0,
                                                       normalized.data(), pitch, 0);
  }
  auto commandBuffer = queue.commandBuffer();
  if (!commandBuffer) {
    return;
  }
  perf::countCommandBuffer();
  auto blit = commandBuffer.blitCommandEncoder();
  if (!blit) {
    return;
  }
  // R-BACK-14.3 — destination texture may be heap-backed; mirror the
  // useHeap pattern from the deferred-upload Initializer path so the
  // heap-resident bookkeeping stays consistent on both upload entry
  // points.
  heapManager_.forEachHeapInstance([&blit](WMT::Heap heap) {
    blit.useHeap(heap);
    perf::countUseHeap();
  });
  WMTOrigin origin{0, 0, 0};
  WMTSize size{mipWidth, mipHeight, 1};
  blit.copyFromTextureToTexture(WMT::Texture{stagingTexture.handle}, 0, 0,
                                 origin, size, texture, slice, mipLevel, origin);
  blit.endEncoding();
  commandBuffer.commit();
  const auto started = std::chrono::steady_clock::now();
  commandBuffer.waitUntilCompleted();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  perf::countSyncWait(
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
}

void Pool::markBufferUse(core::Handle handle, u64 seqId) {
  if (!handle) return;
  if (auto* rec = findBuffer(handle.value)) {
    rec->lastUsedSeqId = std::max(rec->lastUsedSeqId, seqId);
  }
}

void Pool::markTextureUse(core::Handle handle, u64 seqId) {
  if (!handle) return;
  if (auto* rec = findTexture(handle.value)) {
    rec->lastUsedSeqId = std::max(rec->lastUsedSeqId, seqId);
  }
}

void Pool::markSurfaceUse(core::Handle handle, u64 seqId) {
  if (!handle) return;
  if (auto* rec = findSurface(handle.value)) {
    rec->lastUsedSeqId = std::max(rec->lastUsedSeqId, seqId);
  }
}

void Pool::markDrawResources(const core::FlatDrawStateRecord& hot, u64 seqId) {
  markBufferUse(hot.indexBuffer, seqId);
  for (auto handle : hot.streamBuffers) {
    markBufferUse(handle, seqId);
  }
  for (auto handle : hot.textures) {
    markTextureUse(handle, seqId);
  }
  for (const auto& rt : hot.colorAttachments) {
    markSurfaceUse(rt.handle, seqId);
  }
  markSurfaceUse(hot.depthStencil.handle, seqId);
}

void Pool::markClearResources(const core::ClearDesc& desc, u64 seqId) {
  if (desc.clearColor) {
    for (const auto& attachment : desc.colorAttachments) {
      markSurfaceUse(attachment.handle, seqId);
    }
  }
  if (desc.clearDepth || desc.clearStencil) {
    markSurfaceUse(desc.depthStencil.handle, seqId);
  }
}

void Pool::markSurfaceCopyResources(const core::SurfaceCopyDesc& desc, u64 seqId) {
  markSurfaceUse(desc.source, seqId);
  markSurfaceUse(desc.destination, seqId);
}

void Pool::markStretchResources(const core::StretchRectDesc& desc, u64 seqId) {
  markSurfaceUse(desc.source, seqId);
  markSurfaceUse(desc.destination, seqId);
}

void Pool::markReadbackResources(const core::ReadbackDesc& desc, u64 seqId) {
  markSurfaceUse(desc.source, seqId);
  markSurfaceUse(desc.destination, seqId);
}

void Pool::markColorFillResources(const core::ColorFillDesc& desc, u64 seqId) {
  markSurfaceUse(desc.destination, seqId);
}

bool Pool::uploadBufferData(u64 handleValue, const std::uint8_t* bytes, std::size_t byteCount) {
  auto* record = findBuffer(handleValue);
  if (!record) {
    return false;
  }
  record->shadow.assign(bytes, bytes + byteCount);
  if (!record->buffer || byteCount == 0 || !record->contents) {
    return true;
  }
  const std::size_t copySize = std::min(byteCount, static_cast<std::size_t>(record->desc.size));
  std::memcpy(record->contents, bytes, copySize);
  return true;
}

u64 Pool::mapWaitSeqId(core::BufferHandle handle, u32 flags) const noexcept {
  if ((flags & core::UsageDiscard) != 0 || (flags & core::UsageNoOverwrite) != 0) {
    return 0;
  }
  auto* record = findBuffer(handle.value);
  if (!record) {
    return 0;
  }
  return record->lastUsedSeqId;
}

void* Pool::finalizeBufferMap(core::BufferHandle handle, u32 flags) {
  auto* record = findBuffer(handle.value);
  if (!record) {
    return nullptr;
  }
  if ((flags & core::UsageDiscard) != 0) {
    std::fill(record->shadow.begin(), record->shadow.end(), 0);
    if (record->contents) {
      std::memset(record->contents, 0, record->shadow.size());
    }
  }
  if (record->contents) {
    return record->contents;
  }
  return record->shadow.empty() ? nullptr : record->shadow.data();
}

}  // namespace dxmt9::resources
