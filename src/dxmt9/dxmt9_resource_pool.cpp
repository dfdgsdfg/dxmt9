#include "dxmt9_resource_pool.hpp"

#include "dxmt9/assert.hpp"
#include "dxmt9_debug_trace.hpp"
#include "dxmt9_format_convert.hpp"
#include "dxmt9_metal_labels.hpp"
#include "dxmt9_perf_counters.hpp"
#include "dxmt9_queue.hpp"
#include "util/config/config.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <span>
#include <sstream>
#include <vector>

namespace dxmt9::resources {
namespace {

bool dynamicBufferRenameEnabled() {
  static const bool enabled =
      !dxmt9::util::getenvFlag("DXMT9_DISABLE_DYNAMIC_BUFFER_RENAME");
  return enabled;
}

}  // namespace

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
  arena.reclaimCompleted(completedSeqId, [heapManager, completedSeqId](const auto& record) {
    (void)completedSeqId;
    // TLA+ NoUseAfterFree (R-VERIF-3.1) — the watermark gate that also
    // implements R-VERIF-3.4 EncoderPointerStable in C++: by the time
    // record.lastUsedSeqId <= completedSeqId, no in-flight encoder can
    // still be dereferencing this record's pointer (chunk-N encoder
    // marks its resources with lastUsedSeqId = N, and N > completedSeqId
    // until the chunk completes).
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
  if (!bufferArena_.update(handleValue, [](BufferRecord& record) {
        record.destroyPending = true;
      })) {
    return false;
  }
  reclaimCompleted(completedSeqId);
  return true;
}

bool Pool::markTextureDestroyAndGc(u64 handleValue, u64 completedSeqId) {
  if (!textureArena_.update(handleValue, [](TextureRecord& record) {
        record.destroyPending = true;
      })) {
    return false;
  }
  reclaimCompleted(completedSeqId);
  return true;
}

bool Pool::markSurfaceDestroyAndGc(u64 handleValue, u64 completedSeqId) {
  if (!surfaceArena_.update(handleValue, [](SurfaceRecord& record) {
        record.destroyPending = true;
      })) {
    return false;
  }
  reclaimCompleted(completedSeqId);
  return true;
}

core::BufferHandle Pool::createBuffer(WMT::Device device, const core::BufferDesc& desc) {
  BufferRecord record;
  record.desc = desc;
  record.shadow.resize(static_cast<std::size_t>(desc.size));
  // R-BACK-5.8 — DEFAULT + UsageDynamic can use the per-handle rename
  // ring. Tag at create time so the unsafe rename experiment can branch
  // without re-checking pool/usage on every DISCARD lock. Heap-backed
  // allocation is mutually exclusive with the rename ring (heap
  // classifyBuffer rejects UsageDynamic per R-BACK-14.2), so the
  // create-time allocation always lands in the ring's first slot.
  record.isDynamicRename =
      desc.pool == core::Pool::Default && (desc.usage & core::UsageDynamic) != 0u;
  // R-BACK-5.11 — MANAGED locks write the CPU-authoritative Buffer shadow and
  // never wait for an in-flight Metal allocation. Writable unlock rotates the
  // Shared backing before copying the full shadow.
  record.isManagedVersioned = desc.pool == core::Pool::Managed;
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
        // R-BACK-14.4 — count direct (non-heap) allocations as use-resource
        // lifetime events. The ratio against `useHeap` is the heap-pool
        // adoption signal: a high direct count means small-resource heap
        // pooling is not absorbing as much of the allocation traffic as
        // R-BACK-5.9 / R-BACK-14.* expect. This is a per-allocation proxy
        // until per-bind counting in the encoder lands.
        perf::countUseResource();
      }
      record.contents = info.memory.ptr;  // shared mode: contents ptr returned in info
    }
    // R-BACK-5.8 / 5.11 — seed the version ring with the create-time
    // allocation. We always seed (even when newBuffer returned
    // NULL_OBJECT_HANDLE in test environments) so the bookkeeping shape
    // stays uniform.
    if (record.hasVersionedBacking()) {
      BufferRenameRingEntry entry;
      entry.buffer = record.buffer;
      entry.contents = record.contents;
      entry.lastUsedSeqId = 0;
      record.renameRing.push_back(std::move(entry));
      record.renameActiveIndex = 0;
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

bool Pool::exportSharedBuffer(core::BufferHandle handle,
                              SharedBufferBacking& out) const {
  return bufferArena_.inspect(handle.value, [&](const BufferRecord& record) {
    out.buffer = record.buffer;
    out.contents = record.contents;
  });
}

core::BufferHandle Pool::importSharedBuffer(const core::BufferDesc& desc,
                                             const SharedBufferBacking& backing) {
  if (!backing.buffer) {
    return {};
  }
  BufferRecord record;
  record.desc = desc;
  record.buffer = backing.buffer;
  record.contents = backing.contents;
  record.shadow.resize(static_cast<std::size_t>(desc.size));
  record.isDynamicRename =
      desc.pool == core::Pool::Default && (desc.usage & core::UsageDynamic) != 0u;
  record.isManagedVersioned = desc.pool == core::Pool::Managed;
  if (record.hasVersionedBacking()) {
    BufferRenameRingEntry entry;
    entry.buffer = record.buffer;
    entry.contents = record.contents;
    record.renameRing.push_back(std::move(entry));
  }
  const auto handle = bufferArena_.insert(std::move(record));
  if (auto* stored = bufferArena_.find(handle.value); stored && stored->buffer) {
    stored->buffer.setLabel(labels::makeLabelStringFmt(
        "pool_shared_buf_h0x%llx_len%llu",
        static_cast<unsigned long long>(handle.value),
        static_cast<unsigned long long>(desc.size)));
  }
  return handle;
}

core::TextureHandle Pool::createTexture(WMT::Device device,
                                          const core::BackendLimits& limits,
                                          const core::TextureDesc& desc) {
  TextureRecord record;
  record.desc = desc;
  // R-BACK-5.7 / R-BACK-5.10: every pool — including SYSTEMMEM and
  // SCRATCH — gets a Shared-mode Metal texture backing. SYSTEMMEM in
  // particular is the standard D3D9 staging path: app does
  // `CreateTexture(SYSTEMMEM)` → `LockRect/Unlock` → `UpdateTexture(sysmem,
  // default)`. UpdateTexture funnels into `submitSurfaceCopy`, which
  // does a GPU blit between two Metal textures and early-returns if
  // either side is null. Skipping the SYSTEMMEM Metal allocation here
  // (the previous behavior) silently dropped every UpdateTexture upload,
  // leaving the destination DEFAULT texture uninitialized — visible as
  // black/garbage triangles in conf-d3d9-triangle and equivalent SDK
  // samples. `toResourceOptions` already returns Shared for SYSTEMMEM,
  // so the storage mode is correct on both unified and discrete devices.
  {
    const auto formatPolicy = convert::toFormatMetalPolicy(desc, limits);
    WMTTextureInfo info{};
    info.type = convert::toTextureType(desc.type, false);
    info.pixel_format = formatPolicy.pixelFormat;
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
    info.usage = formatPolicy.usage;
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
    if (convert::formatNeedsShaderReadSwizzle(desc.format) &&
        !formatPolicy.needsShaderReadView &&
        ((desc.usage & core::UsageRenderTarget) != 0 ||
         (desc.usage & core::UsageDepthStencil) != 0)) {
      perf::countTexturePixelFormatViewSuppressedRt(footprint);
    }
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
    if (record.texture && formatPolicy.needsShaderReadView) {
      uint64_t gpuId = 0;
      record.shaderReadTexture = record.texture.newTextureView(
          info.pixel_format, info.type, 0, info.mipmap_level_count,
          0, convert::toShaderReadViewSliceCount(desc.type),
          formatPolicy.shaderReadSwizzle, gpuId);
    }
    if (record.texture && formatPolicy.supportsSrgbView) {
      uint64_t gpuId = 0;
      record.srgbShaderReadTexture = record.texture.newTextureView(
          formatPolicy.srgbPixelFormat, info.type, 0, info.mipmap_level_count,
          0, convert::toShaderReadViewSliceCount(desc.type),
          formatPolicy.needsShaderReadView
              ? formatPolicy.shaderReadSwizzle
              : WMTTextureSwizzleChannels{WMTTextureSwizzleRed, WMTTextureSwizzleGreen,
                                          WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha},
          gpuId);
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
  if (stored && stored->shaderReadTexture) {
    stored->shaderReadTexture.setLabel(labels::makeLabelStringFmt(
        "pool_tex_shader_view_h0x%llx_fmt%u",
        static_cast<unsigned long long>(handle.value),
        static_cast<unsigned>(desc.format)));
  }
  if (stored && stored->srgbShaderReadTexture) {
    stored->srgbShaderReadTexture.setLabel(labels::makeLabelStringFmt(
        "pool_tex_srgb_shader_view_h0x%llx_fmt%u",
        static_cast<unsigned long long>(handle.value),
        static_cast<unsigned>(desc.format)));
  }
  if (stored && stored->isHeapBacked) {
    heapManager_.retainHeapMember(stored->heap.handle, 0);
  }
  traceTextureCreate(handle, desc, stored && stored->texture,
                     stored ? stored->needsStagingBlit : false,
                     stored ? stored->isManagedDiscrete : false);
  return handle;
}

bool Pool::exportSharedTexture(core::TextureHandle handle,
                               SharedTextureBacking& out) const {
  return textureArena_.inspect(handle.value, [&](const TextureRecord& record) {
    out.texture = record.texture;
    out.shaderReadTexture = record.shaderReadTexture;
    out.srgbShaderReadTexture = record.srgbShaderReadTexture;
    out.needsStagingBlit = record.needsStagingBlit;
    out.isManagedDiscrete = record.isManagedDiscrete;
  });
}

core::TextureHandle Pool::importSharedTexture(const core::TextureDesc& desc,
                                               const SharedTextureBacking& backing) {
  if (!backing.texture) {
    return {};
  }
  TextureRecord record;
  record.desc = desc;
  record.texture = backing.texture;
  record.shaderReadTexture = backing.shaderReadTexture;
  record.srgbShaderReadTexture = backing.srgbShaderReadTexture;
  record.needsStagingBlit = backing.needsStagingBlit;
  record.isManagedDiscrete = backing.isManagedDiscrete;
  const auto handle = textureArena_.insert(std::move(record));
  if (auto* stored = textureArena_.find(handle.value); stored && stored->texture) {
    stored->texture.setLabel(labels::makeLabelStringFmt(
        "pool_shared_tex_h0x%llx_fmt%u",
        static_cast<unsigned long long>(handle.value),
        static_cast<unsigned>(desc.format)));
  }
  return handle;
}

core::SurfaceHandle Pool::createSurface(WMT::Device device,
                                          const core::BackendLimits& limits,
                                          const core::SurfaceDesc& desc) {
  SurfaceRecord record;
  record.desc = desc;
  // R-FORMAT-12: a D3DFMT_NULL render target is colorless — it has no
  // color storage. Allocate no Metal texture; the record exists only as a
  // zero-cost marker so SetRenderTarget can bind it and the render-pass
  // builder (dxmt9_draw_encoder.mm::beginRenderPass) omits its color
  // attachment, leaving the bound depth/stencil as the effective target.
  if (desc.format == core::Format::NullRt) {
    const auto handle = surfaceArena_.insert(std::move(record));
    traceSurfaceCreate(handle, desc, /*hasTexture=*/false);
    return handle;
  }
  // Surfaces match the texture storage policy (R-BACK-5.7): every pool
  // gets a Metal texture so `submitSurfaceCopy` can blit between any
  // pair, including SYSTEMMEM-side staging surfaces. Storage mode comes
  // from `toResourceOptions` — Shared for SYSTEMMEM/SCRATCH on every
  // device, so a CPU-driven `replaceRegion` works on Lock/Unlock.
  {
    const auto formatPolicy = convert::toFormatMetalPolicy(desc, limits);
    const uint32_t sc = std::max(1u, core::sampleCount(desc.multiSampleType));
    WMTTextureInfo info{};
    info.type = convert::toTextureType(core::TextureType::TwoD,
                                         desc.multiSampleType != core::MultiSampleType::None);
    info.pixel_format = formatPolicy.pixelFormat;
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
    info.usage = formatPolicy.usage;
    record.texture = device.newTexture(info);
    if (record.texture && formatPolicy.supportsSrgbView) {
      WMTTextureSwizzleChannels swizzle{
          WMTTextureSwizzleRed, WMTTextureSwizzleGreen,
          WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha};
      uint64_t gpuId = 0;
      record.srgbTexture = record.texture.newTextureView(
          formatPolicy.srgbPixelFormat, record.texture.textureType(), 0, 1, 0, 1, swizzle, gpuId);
    }
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

bool Pool::exportSharedSurface(core::SurfaceHandle handle,
                               SharedSurfaceBacking& out) const {
  return surfaceArena_.inspect(handle.value, [&](const SurfaceRecord& record) {
    out.texture = record.texture;
    out.srgbTexture = record.srgbTexture;
    out.resolveTexture = record.resolveTexture;
  });
}

core::SurfaceHandle Pool::importSharedSurface(const core::SurfaceDesc& desc,
                                               const SharedSurfaceBacking& backing) {
  if (!backing.texture && desc.format != core::Format::NullRt) {
    return {};
  }
  SurfaceRecord record;
  record.desc = desc;
  record.texture = backing.texture;
  record.srgbTexture = backing.srgbTexture;
  record.resolveTexture = backing.resolveTexture;
  const auto handle = surfaceArena_.insert(std::move(record));
  traceSurfaceCreate(handle, desc, static_cast<bool>(backing.texture));
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
  if (record.texture) {
    const auto srgbFormat = convert::toSrgbPixelFormat(parentTexture.pixelFormat());
    if (srgbFormat != parentTexture.pixelFormat()) {
      WMTTextureSwizzleChannels swizzle{
          WMTTextureSwizzleRed, WMTTextureSwizzleGreen,
          WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha};
      uint64_t gpuId = 0;
      const auto viewType = textureRecord->desc.type == core::TextureType::Cube
                                ? WMTTextureType2D
                                : parentTexture.textureType();
      record.srgbTexture = parentTexture.newTextureView(
          srgbFormat, viewType, subresource.mipLevel, 1,
          subresource.slice, 1, swizzle, gpuId);
    }
  }
  const auto handle = surfaceArena_.insert(std::move(record));
  const auto* stored = surfaceArena_.find(handle.value);
  traceSurfaceAlias(handle, textureHandle, level, subresource, desc, usedParentTexture,
                    stored && stored->texture);
  return handle;
}

namespace {

std::span<const std::uint8_t> normalizeUploadBytes(core::Format format, u32 width, u32 height,
                                                     u32 depth, u32 pitch,
                                                     u32 slicePitch,
                                                     std::span<const std::uint8_t> bytes,
                                                     std::vector<std::uint8_t>& scratch) {
  if (bytes.empty()) {
    return bytes;
  }
  switch (format) {
    case core::Format::X8R8G8B8:
    case core::Format::X8B8G8R8: {
      const std::size_t rowBytes = static_cast<std::size_t>(pitch);
      const std::size_t imageBytes = static_cast<std::size_t>(slicePitch);
      const std::size_t expected = imageBytes * static_cast<std::size_t>(depth);
      if (expected == 0 || bytes.size() < expected) {
        return bytes;
      }
      scratch.assign(bytes.begin(), bytes.begin() + expected);
      for (u32 z = 0; z < depth; ++z) {
        auto* slice = scratch.data() + static_cast<std::size_t>(z) * imageBytes;
        for (u32 y = 0; y < height; ++y) {
          std::uint8_t* row = slice + static_cast<std::size_t>(y) * rowBytes;
          for (u32 x = 0; x < width; ++x) {
            row[static_cast<std::size_t>(x) * 4 + 3] = 0xffu;
          }
        }
      }
      return scratch;
    }
    case core::Format::X1R5G5B5: {
      const std::size_t rowBytes = static_cast<std::size_t>(pitch);
      const std::size_t imageBytes = static_cast<std::size_t>(slicePitch);
      const std::size_t expected = imageBytes * static_cast<std::size_t>(depth);
      if (expected == 0 || bytes.size() < expected) {
        return bytes;
      }
      scratch.assign(bytes.begin(), bytes.begin() + expected);
      for (u32 z = 0; z < depth; ++z) {
        auto* slice = scratch.data() + static_cast<std::size_t>(z) * imageBytes;
        for (u32 y = 0; y < height; ++y) {
          auto* row = reinterpret_cast<std::uint16_t*>(
              slice + static_cast<std::size_t>(y) * rowBytes);
          for (u32 x = 0; x < width; ++x) {
            row[x] |= 0x8000u;
          }
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
                          u32 depth,
                          u32 pitch,
                          u32 slicePitch,
                          const std::uint8_t* bytes,
                          std::size_t byteCount) {
  auto* record = findTexture(handle.value);
  if (!record || !record->texture || byteCount == 0) {
    return std::nullopt;
  }

  const auto normalized =
      normalizeUploadBytes(record->desc.format, width, height, depth, pitch,
                           slicePitch, {bytes, byteCount},
                           textureUploadScratch_);

  const auto subresource = decodeTextureSubresource(record->desc, level);
  if (!subresource.valid) {
    return std::nullopt;
  }
  WMT::Texture texture{record->texture.handle};
  const u32 mipLevel = subresource.mipLevel;
  const u32 slice = subresource.slice;
  const u32 mipWidth = std::max(1u, width);
  const u32 mipHeight = std::max(1u, height);
  const u32 mipDepth = std::max(1u, depth);
  const u32 imagePitch =
      slicePitch != 0 ? slicePitch
                      : pitch * core::formatRowCount(record->desc.format, height);
  const u32 uploadImagePitch =
      record->desc.type == core::TextureType::Volume ? imagePitch : 0u;

  if (!record->needsStagingBlit) {
    // Shared-mode (DEFAULT+DYNAMIC, MANAGED on unified memory, etc.):
    // CPU write straight to the destination; no blit and no counter
    // increment. The Apple-Silicon MANAGED branch lands here per
    // R-BACK-5.7 — `countManagedTextureUploadBlit` must remain 0.
    WMTOrigin origin{0, 0, 0};
    WMTSize size{mipWidth, mipHeight, mipDepth};
    texture.replaceRegion(origin, size, mipLevel, slice, normalized.data(), pitch,
                          uploadImagePitch);
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
  stagingInfo.type = record->desc.type == core::TextureType::Volume
                         ? WMTTextureType3D
                         : WMTTextureType2D;
  stagingInfo.pixel_format = texture.pixelFormat();
  stagingInfo.width = mipWidth;
  stagingInfo.height = mipHeight;
  stagingInfo.depth = mipDepth;
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
    WMTSize size{mipWidth, mipHeight, mipDepth};
    WMT::Texture{stagingTexture.handle}.replaceRegion(origin, size, 0, 0,
                                                       normalized.data(), pitch,
                                                       uploadImagePitch);
  }
  StagingCopy out;
  out.stagingTexture = std::move(stagingTexture);
  out.destTexture = texture;
  out.mipLevel = mipLevel;
  out.slice = slice;
  out.width = mipWidth;
  out.height = mipHeight;
  out.depth = mipDepth;
  out.destIsHeapBacked = record->isHeapBacked;
  out.destHeap = record->heap.handle;
  return out;
}

void Pool::uploadTextureLevel(WMT::Device device,
                               WMT::CommandQueue queue,
                               core::TextureHandle handle,
                               u32 level,
                               u32 width,
                               u32 height,
                               u32 depth,
                               u32 pitch,
                               u32 slicePitch,
                               const std::uint8_t* bytes,
                               std::size_t byteCount) {
  auto* record = findTexture(handle.value);
  if (!record || !record->texture || byteCount == 0) {
    return;
  }

  const auto normalized =
      normalizeUploadBytes(record->desc.format, width, height, depth, pitch,
                           slicePitch, {bytes, byteCount},
                           textureUploadScratch_);

  const auto subresource = decodeTextureSubresource(record->desc, level);
  if (!subresource.valid) {
    return;
  }
  WMT::Texture texture{record->texture.handle};
  const u32 mipLevel = subresource.mipLevel;
  const u32 slice = subresource.slice;
  const u32 mipWidth = std::max(1u, width);
  const u32 mipHeight = std::max(1u, height);
  const u32 mipDepth = std::max(1u, depth);
  const u32 imagePitch =
      slicePitch != 0 ? slicePitch
                      : pitch * core::formatRowCount(record->desc.format, height);
  const u32 uploadImagePitch =
      record->desc.type == core::TextureType::Volume ? imagePitch : 0u;

  if (!record->needsStagingBlit) {
    WMTOrigin origin{0, 0, 0};
    WMTSize size{mipWidth, mipHeight, mipDepth};
    texture.replaceRegion(origin, size, mipLevel, slice, normalized.data(), pitch,
                          uploadImagePitch);
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
  stagingInfo.type = record->desc.type == core::TextureType::Volume
                         ? WMTTextureType3D
                         : WMTTextureType2D;
  stagingInfo.pixel_format = texture.pixelFormat();
  stagingInfo.width = mipWidth;
  stagingInfo.height = mipHeight;
  stagingInfo.depth = mipDepth;
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
    WMTSize size{mipWidth, mipHeight, mipDepth};
    WMT::Texture{stagingTexture.handle}.replaceRegion(origin, size, 0, 0,
                                                       normalized.data(), pitch,
                                                       uploadImagePitch);
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
  // R-BACK-14.3 — bound-resource walk: this synchronous blit references
  // exactly one source (ephemeral staging texture, never heap-backed)
  // and one destination (the Pool TextureRecord we already resolved).
  // The source contributes nothing to the useHeap set; the destination
  // contributes its heap iff it was placed on one. Mirrors the
  // render-encoder dedup pattern in beginRenderPass — a single-element
  // walk here, no broad iteration over every live heap instance.
  if (record->isHeapBacked && record->heap.handle != 0) {
    blit.useHeap(record->heap);
    perf::countUseHeap();
  }
  WMTOrigin origin{0, 0, 0};
  WMTSize size{mipWidth, mipHeight, mipDepth};
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
  bufferArena_.update(handle.value, [seqId](BufferRecord& rec) {
    rec.lastUsedSeqId = std::max(rec.lastUsedSeqId, seqId);
    if (rec.hasVersionedBacking() &&
        rec.renameActiveIndex < rec.renameRing.size()) {
      auto& active = rec.renameRing[rec.renameActiveIndex];
      active.lastUsedSeqId = std::max(active.lastUsedSeqId, seqId);
      // TLA+: BufferBackingVersioning.LogicalWatermarkCoversEveryBacking.
      DXMT_ASSERT(active.lastUsedSeqId <= rec.lastUsedSeqId);
    }
  });
}

void Pool::markBufferSnapshotUse(core::Handle handle,
                                 const core::DrawBufferBindingSnapshot& snapshot,
                                 u64 seqId) {
  if (!handle || !snapshot.valid()) return;
  bufferArena_.update(handle.value, [seqId, snapshot](BufferRecord& rec) {
    rec.lastUsedSeqId = std::max(rec.lastUsedSeqId, seqId);
    if (!rec.hasVersionedBacking()) {
      return;
    }
    for (auto& entry : rec.renameRing) {
      if (entry.buffer && entry.buffer.handle == snapshot.metalHandle) {
        entry.lastUsedSeqId = std::max(entry.lastUsedSeqId, seqId);
        // TLA+: BufferBackingVersioning.LogicalWatermarkCoversEveryBacking.
        DXMT_ASSERT(entry.lastUsedSeqId <= rec.lastUsedSeqId);
        return;
      }
    }
  });
}

core::DrawBufferBindingSnapshot
Pool::snapshotBufferBinding(core::Handle handle) const noexcept {
  core::DrawBufferBindingSnapshot snapshot{};
  if (!handle) return snapshot;
  bufferArena_.inspect(handle.value, [&snapshot](const BufferRecord& rec) {
    if (!rec.hasVersionedBacking() || !rec.buffer) {
      return;
    }
    snapshot.metalHandle = rec.buffer.handle;
    snapshot.contentsAddress =
        static_cast<u64>(reinterpret_cast<std::uintptr_t>(rec.contents));
    snapshot.byteSize = rec.desc.size;
    snapshot.contentRevision = rec.contentRevision;
  });
  return snapshot;
}

void Pool::markTextureUse(core::Handle handle, u64 seqId) {
  if (!handle) return;
  textureArena_.update(handle.value, [seqId](TextureRecord& rec) {
    rec.lastUsedSeqId = std::max(rec.lastUsedSeqId, seqId);
  });
}

void Pool::markSurfaceUse(core::Handle handle, u64 seqId) {
  if (!handle) return;
  surfaceArena_.update(handle.value, [seqId](SurfaceRecord& rec) {
    rec.lastUsedSeqId = std::max(rec.lastUsedSeqId, seqId);
  });
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

void Pool::markDepthResolveResources(const core::DepthResolveDesc& desc, u64 seqId) {
  // R-FORMAT-11: both endpoints resolve through the surface table — the
  // encoder calls findSurface() on each (msaaDepth = render-pass depth
  // texture, intzDest = the INTZ texture's level-0 surface used as the
  // resolve target). Mirrors markStretchResources / markReadbackResources.
  markSurfaceUse(desc.msaaDepth, seqId);
  markSurfaceUse(desc.intzDest, seqId);
}

ReorderedIndexBufferLookup Pool::findReorderedIndexBuffer(
    u64 sourceHandle,
    ReorderedIndexBufferCacheKey key,
    u64 seqId,
    u64 completedSeqId) {
  ReorderedIndexBufferLookup result{};
  if (sourceHandle == 0) {
    return result;
  }

  bufferArena_.update(sourceHandle, [&](BufferRecord& record) {
    if (key.sourceRevision == 0) {
      key.sourceRevision = record.contentRevision;
    }

    auto& entries = record.reorderedIndexCache;
    entries.erase(
        std::remove_if(entries.begin(), entries.end(), [&](const auto& entry) {
          return ((!entry.buffer && !entry.rejected) ||
                  entry.key.sourceRevision != record.contentRevision) &&
                 entry.lastUsedSeqId <= completedSeqId;
        }),
        entries.end());

    for (auto& entry : entries) {
      if (entry.key == key && (entry.buffer || entry.rejected)) {
        entry.lastUsedSeqId = std::max(entry.lastUsedSeqId, seqId);
        result.buffer = WMT::Buffer{entry.buffer.handle};
        result.byteCount = entry.byteCount;
        result.hit = true;
        result.rejected = entry.rejected;
        return;
      }
    }
  });

  return result;
}

bool Pool::rememberRejectedReorderedIndexBuffer(
    u64 sourceHandle,
    ReorderedIndexBufferCacheKey key,
    u64 seqId,
    u64 completedSeqId) {
  bool remembered = false;
  if (sourceHandle == 0) {
    return remembered;
  }

  bufferArena_.update(sourceHandle, [&](BufferRecord& record) {
    if (key.sourceRevision == 0) {
      key.sourceRevision = record.contentRevision;
    }

    auto& entries = record.reorderedIndexCache;
    entries.erase(
        std::remove_if(entries.begin(), entries.end(), [&](const auto& entry) {
          return ((!entry.buffer && !entry.rejected) ||
                  entry.key.sourceRevision != record.contentRevision) &&
                 entry.lastUsedSeqId <= completedSeqId;
        }),
        entries.end());

    for (auto& entry : entries) {
      if (entry.key == key) {
        entry.lastUsedSeqId = std::max(entry.lastUsedSeqId, seqId);
        if (!entry.buffer) {
          entry.byteCount = 0;
          entry.rejected = true;
        }
        remembered = true;
        return;
      }
    }

    ReorderedIndexBufferCacheEntry entry{};
    entry.key = key;
    entry.lastUsedSeqId = seqId;
    entry.rejected = true;
    entries.push_back(std::move(entry));
    remembered = true;

    constexpr std::size_t kMaxReorderedIndexEntriesPerSource = 64;
    if (entries.size() > kMaxReorderedIndexEntriesPerSource) {
      entries.erase(
          std::remove_if(entries.begin(), entries.end(), [&](const auto& oldEntry) {
            return oldEntry.lastUsedSeqId <= completedSeqId &&
                   oldEntry.key != key;
          }),
          entries.end());
    }
    if (entries.size() > kMaxReorderedIndexEntriesPerSource) {
      const auto oldestRejected = std::min_element(
          entries.begin(), entries.end(), [&](const auto& lhs, const auto& rhs) {
            if (lhs.key == key) {
              return false;
            }
            if (rhs.key == key) {
              return true;
            }
            if (lhs.rejected != rhs.rejected) {
              return lhs.rejected;
            }
            return lhs.lastUsedSeqId < rhs.lastUsedSeqId;
          });
      if (oldestRejected != entries.end() &&
          oldestRejected->rejected &&
          oldestRejected->key != key) {
        entries.erase(oldestRejected);
      }
    }
  });

  return remembered;
}

ReorderedIndexBufferLookup Pool::getOrCreateReorderedIndexBuffer(
    WMT::Device device,
    u64 sourceHandle,
    ReorderedIndexBufferCacheKey key,
    std::span<const std::uint8_t> bytes,
    u64 seqId,
    u64 completedSeqId) {
  ReorderedIndexBufferLookup result{};
  if (!device || sourceHandle == 0 || bytes.empty()) {
    return result;
  }

  bufferArena_.update(sourceHandle, [&](BufferRecord& record) {
    if (key.sourceRevision == 0) {
      key.sourceRevision = record.contentRevision;
    }

    auto& entries = record.reorderedIndexCache;
    entries.erase(
        std::remove_if(entries.begin(), entries.end(), [&](const auto& entry) {
          return ((!entry.buffer && !entry.rejected) ||
                  entry.key.sourceRevision != record.contentRevision) &&
                 entry.lastUsedSeqId <= completedSeqId;
        }),
        entries.end());

    ReorderedIndexBufferCacheEntry* rejectedEntry = nullptr;
    for (auto& entry : entries) {
      if (entry.key != key) {
        continue;
      }
      if (entry.buffer) {
        entry.lastUsedSeqId = std::max(entry.lastUsedSeqId, seqId);
        result.buffer = WMT::Buffer{entry.buffer.handle};
        result.byteCount = entry.byteCount;
        result.hit = true;
        return;
      }
      if (entry.rejected) {
        rejectedEntry = &entry;
        break;
      }
    }

    WMTBufferInfo info{};
    info.length = static_cast<u64>(bytes.size());
    info.options = WMTResourceStorageModeShared;
    info.memory.set(const_cast<std::uint8_t*>(bytes.data()));
    auto buffer = device.newBuffer(info);
    if (!buffer) {
      return;
    }
    perf::countMetalBuffer(bytes.size());
    perf::countUseResource();
    buffer.setLabel(labels::makeLabelStringFmt(
        "pool_reordered_ib_h0x%llx_s%u_n%llu",
        static_cast<unsigned long long>(sourceHandle),
        key.startIndex,
        static_cast<unsigned long long>(key.indexCount)));

    ReorderedIndexBufferCacheEntry entry{};
    auto& targetEntry = rejectedEntry ? *rejectedEntry : entry;
    targetEntry.key = key;
    targetEntry.buffer = std::move(buffer);
    targetEntry.byteCount = static_cast<u64>(bytes.size());
    targetEntry.lastUsedSeqId = seqId;
    targetEntry.rejected = false;
    result.buffer = WMT::Buffer{targetEntry.buffer.handle};
    result.byteCount = targetEntry.byteCount;
    result.created = true;
    if (!rejectedEntry) {
      entries.push_back(std::move(entry));
    }

    constexpr std::size_t kMaxReorderedIndexEntriesPerSource = 64;
    if (entries.size() > kMaxReorderedIndexEntriesPerSource) {
      entries.erase(
          std::remove_if(entries.begin(), entries.end(), [&](const auto& oldEntry) {
            return oldEntry.lastUsedSeqId <= completedSeqId &&
                   oldEntry.key != key;
          }),
          entries.end());
    }
  });

  return result;
}

u64 Pool::mapWaitSeqId(core::BufferHandle handle, u32 flags) const noexcept {
  if ((flags & core::UsageNoOverwrite) != 0) {
    return 0;
  }
  const bool discard = (flags & core::UsageDiscard) != 0;
  u64 waitSeqId = 0;
  bufferArena_.inspect(handle.value, [discard, &waitSeqId](const BufferRecord& record) {
    // R-BACK-5.11 — the core Buffer's CPU shadow is authoritative for every
    // MANAGED lock. Read-only locks only inspect it; writable locks publish it
    // into an idle/fresh backing at unlock, so neither needs a GPU wait here.
    if (record.isManagedVersioned) {
      return;
    }
    // Draw submissions snapshot the concrete Metal backing for dynamic
    // buffers, so DISCARD can rotate away from in-flight storage instead of
    // waiting for the logical BufferHandle to drain.
    if (discard && record.isDynamicRename && dynamicBufferRenameEnabled()) {
      return;
    }
    waitSeqId = record.lastUsedSeqId;
  });
  return waitSeqId;
}

// R-BACK-5.8 / 5.11 — select an idle Shared backing or append a fresh
// allocation instead of blocking on prior GPU completion. The logical
// record's lastUsedSeqId is an aggregate destruction watermark and must remain
// monotonic; each ring entry owns the concrete backing reuse watermark.
namespace {
enum class BufferBackingSelection {
  ActiveIdle,
  ReusedIdle,
  Fresh,
};

BufferBackingSelection rotateBufferBacking(WMT::Device device,
                                            BufferRecord& record,
                                            u64 completedSeqId) {
  DXMT_ASSERT(record.hasVersionedBacking());
  DXMT_ASSERT(record.renameActiveIndex < record.renameRing.size());
  // Fast path: the active slot is itself idle. The DISCARD only needs
  // a fresh write surface, not a different allocation — return without
  // touching the ring shape. Zero-fill happens in finalizeBufferMap.
  if (record.renameActiveIndex < record.renameRing.size() &&
      record.renameRing[record.renameActiveIndex].lastUsedSeqId <= completedSeqId) {
    return BufferBackingSelection::ActiveIdle;
  }
  // Look for any other idle entry to rotate into.
  for (std::size_t i = 0; i < record.renameRing.size(); ++i) {
    if (i == record.renameActiveIndex) {
      continue;
    }
    if (record.renameRing[i].lastUsedSeqId <= completedSeqId) {
      // TLA+: BufferBackingVersioning.NoUploadOverwriteInFlight.
      DXMT_ASSERT(record.renameRing[i].lastUsedSeqId <= completedSeqId);
      record.renameActiveIndex = static_cast<u32>(i);
      auto& entry = record.renameRing[i];
      record.buffer = entry.buffer;
      record.contents = entry.contents;
      return BufferBackingSelection::ReusedIdle;
    }
  }
  // No idle entry anywhere — fresh-allocate per R-BACK-5.8. Do NOT
  // block on GPU completion; the spec explicitly requires growth over
  // wait.
  WMTBufferInfo info{};
  info.length = record.desc.size;
  info.options = WMTResourceStorageModeShared;
  auto fresh = device.newBuffer(info);
  BufferRenameRingEntry entry;
  entry.buffer = std::move(fresh);
  entry.contents = info.memory.ptr;
  entry.lastUsedSeqId = 0;
  if (entry.buffer) {
    perf::countMetalBuffer(static_cast<std::size_t>(info.length));
    perf::countUseResource();
  }
  record.renameRing.push_back(std::move(entry));
  record.renameActiveIndex = static_cast<u32>(record.renameRing.size() - 1);
  auto& active = record.renameRing.back();
  record.buffer = active.buffer;
  record.contents = active.contents;
  return BufferBackingSelection::Fresh;
}
}  // namespace

bool Pool::uploadBufferData(WMT::Device device,
                            u64 handleValue,
                            const std::uint8_t* bytes,
                            std::size_t byteCount,
                            u64 completedSeqId) {
  return bufferArena_.update(handleValue, [&](BufferRecord& record) {
    if (record.isManagedVersioned && !record.renameRing.empty()) {
      const auto selection =
          rotateBufferBacking(device, record, completedSeqId);
      // TLA+: BufferBackingVersioning.NoUploadOverwriteInFlight.
      DXMT_ASSERT(record.renameActiveIndex < record.renameRing.size());
      DXMT_ASSERT(record.renameRing[record.renameActiveIndex].lastUsedSeqId <=
                  completedSeqId);
      perf::countManagedBufferUpload(static_cast<u64>(byteCount));
      switch (selection) {
        case BufferBackingSelection::ActiveIdle:
          perf::countManagedBufferBackingInPlace();
          break;
        case BufferBackingSelection::ReusedIdle:
          perf::countManagedBufferBackingReuse();
          break;
        case BufferBackingSelection::Fresh:
          perf::countManagedBufferBackingFresh();
          break;
      }
    }

    ++record.contentRevision;
    const std::size_t copySize =
        std::min(byteCount, static_cast<std::size_t>(record.desc.size));
    if (record.shadow.size() != static_cast<std::size_t>(record.desc.size)) {
      record.shadow.resize(static_cast<std::size_t>(record.desc.size));
    }
    if (copySize != 0) {
      std::memcpy(record.shadow.data(), bytes, copySize);
    }
    if (copySize == 0 || !record.contents) {
      return;
    }
    std::memcpy(record.contents, bytes, copySize);
  });
}

void* Pool::finalizeBufferMap(WMT::Device device,
                              core::BufferHandle handle,
                              u32 flags,
                              u64 completedSeqId) {
  void* result = nullptr;
  bufferArena_.update(handle.value, [&](BufferRecord& record) {
    // R-BACK-5.8 — rotate before zero-fill so the bytes we clear belong to the
    // freshly active allocation, not the old (possibly still-in-flight) one.
    if ((flags & core::UsageDiscard) != 0 && record.isDynamicRename &&
        dynamicBufferRenameEnabled() &&
        !record.renameRing.empty()) {
      rotateBufferBacking(device, record, completedSeqId);
    }
    // MANAGED DISCARD still operates on the CPU-authoritative core shadow.
    // Touching the live Metal contents here would corrupt an in-flight draw;
    // its fresh/idle backing is selected only when writable unlock uploads.
    if ((flags & core::UsageDiscard) != 0 && !record.isManagedVersioned) {
      ++record.contentRevision;
      std::fill(record.shadow.begin(), record.shadow.end(), 0);
      if (record.contents) {
        std::memset(record.contents, 0, record.shadow.size());
      }
    }
    result = record.contents ? record.contents
                             : (record.shadow.empty() ? nullptr : record.shadow.data());
  });
  return result;
}

}  // namespace dxmt9::resources
