#include "dxmt9_blit_encoders.hpp"

#include "dxmt9/assert.hpp"
#include "dxmt9_command_queue.hpp"
#include "dxmt9_format_convert.hpp"
#include "dxmt9_metal_labels.hpp"
#include "dxmt9_perf_counters.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>

namespace dxmt9::encoders {

namespace {

WMT::Reference<WMT::SamplerState> makeLinearOrNearestSampler(WMT::Reference<WMT::Device> device,
                                                                bool linear) {
  WMTSamplerInfo info{};
  auto f = linear ? WMTSamplerMinMagFilterLinear : WMTSamplerMinMagFilterNearest;
  info.min_filter = f;
  info.mag_filter = f;
  info.mip_filter = WMTSamplerMipFilterNotMipmapped;
  info.s_address_mode = WMTSamplerAddressModeClampToEdge;
  info.t_address_mode = WMTSamplerAddressModeClampToEdge;
  info.r_address_mode = WMTSamplerAddressModeClampToEdge;
  info.normalized_coords = true;
  return device.newSamplerState(info);
}

void attachCounterSampleBuffers(
    WMTRenderPassInfo& passInfo,
    std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments) {
  constexpr std::size_t kMaxSampleBufferAttachments =
      sizeof(passInfo.sample_buffer_attachments) /
      sizeof(passInfo.sample_buffer_attachments[0]);
  const auto attachmentCount =
      std::min(sampleBufferAttachments.size(), kMaxSampleBufferAttachments);
  for (std::size_t i = 0; i < attachmentCount; ++i) {
    passInfo.sample_buffer_attachments[i] = sampleBufferAttachments[i];
  }
  passInfo.num_sample_buffer_attachments =
      static_cast<std::uint8_t>(attachmentCount);
}

bool canCopyStretchRect(const resources::SurfaceRecord& src,
                        const resources::SurfaceRecord& dst,
                        const core::StretchRectDesc& stretch) {
  const auto srcW = std::max(0, stretch.sourceRect.right - stretch.sourceRect.left);
  const auto srcH = std::max(0, stretch.sourceRect.bottom - stretch.sourceRect.top);
  const auto dstW = std::max(0, stretch.destinationRect.right - stretch.destinationRect.left);
  const auto dstH = std::max(0, stretch.destinationRect.bottom - stretch.destinationRect.top);
  return src.desc.format == dst.desc.format &&
         srcW == dstW &&
         srcH == dstH &&
         stretch.sourceSampleCount == 1 &&
         stretch.destinationSampleCount == 1 &&
         !src.resolveTexture &&
         !dst.resolveTexture;
}

bool isFullscreenStretch(const resources::SurfaceRecord& dst,
                         const core::StretchRectDesc& stretch) {
  return stretch.destinationRect.left == 0 &&
         stretch.destinationRect.top == 0 &&
         stretch.destinationRect.right == static_cast<int32_t>(dst.desc.width) &&
         stretch.destinationRect.bottom == static_cast<int32_t>(dst.desc.height);
}

// R-BACK-14.3 — issue `useHeap` once per heap instance that actually
// backs a resource referenced by this blit. Mirrors the render-encoder
// pattern (see beginRenderPass in dxmt9_render_pass_encoder.mm): walk the small
// set of resources the blit will touch, consult each record's
// `isHeapBacked` flag, and dedup heap handles through a fixed-size
// array so the encoder-open path stays allocation-free. Surfaces don't
// participate in heap pooling (R-BACK-14.1), but a Surface created via
// createSurfaceForTexture aliases a TextureRecord that may be
// heap-backed — so the surface helper resolves through `aliasTexture`
// and then checks the parent TextureRecord. A blit references at most a
// source + destination resource, so a 2-slot dedup buffer is plenty.
constexpr std::size_t kMaxBlitBoundHeaps = 2;
struct UsedHeapSet {
  std::array<obj_handle_t, kMaxBlitBoundHeaps> heaps{};
  std::size_t count = 0;
  void push(obj_handle_t h) {
    if (h == 0) return;
    for (std::size_t i = 0; i < count; ++i) {
      if (heaps[i] == h) return;
    }
    if (count < heaps.size()) {
      heaps[count++] = h;
    }
  }
};

void considerTexture(resources::Pool& pool, core::Handle handle, UsedHeapSet& set) {
  if (!handle) return;
  // `isHeapBacked`/`heap` are `immutable-after-init` — see the identical
  // comment beside `beginRenderPass`'s `considerBuffer`/`considerTexture` in
  // dxmt9_render_pass_encoder.mm.
  if (auto* rec = pool.findTexture(handle.value); rec && rec->isHeapBacked) {
    set.push(rec->heap.handle);
  }
}

void considerSurface(resources::Pool& pool, core::Handle handle, UsedHeapSet& set) {
  if (!handle) return;
  auto* surface = pool.findSurface(handle.value);
  if (!surface) return;
  // SurfaceRecord itself is never heap-backed (RT/DS attachments always
  // allocate direct), but a surface created via createSurfaceForTexture
  // aliases a TextureRecord that may live on a heap; the underlying
  // Metal texture is the same object, so blits to/from that surface
  // touch the parent's heap-backed memory.
  if (surface->aliasTexture) {
    considerTexture(pool, surface->aliasTexture, set);
  }
}

void emitUseHeap(WMT::BlitCommandEncoder& blit, const UsedHeapSet& set) {
  for (std::size_t i = 0; i < set.count; ++i) {
    blit.useHeap(WMT::Heap{set.heaps[i]});
    perf::countUseHeap();
  }
}

bool readbackRawColorTexture(CommandQueue& queue, resources::Pool& pool,
                          WMT::Reference<WMT::Device> device,
                          core::SurfaceHandle source,
                          WMT::Reference<WMT::Texture> sourceTexture,
                          WMTPixelFormat expectedFormat,
                          core::Rect sourceRect, std::uint32_t sourceLevel,
                          core::ReadbackPixels& pixels) {
  if ((expectedFormat != WMTPixelFormatBGRA8Unorm &&
       expectedFormat != WMTPixelFormatRGBA16Float &&
       expectedFormat != WMTPixelFormatR32Float) ||
      !sourceTexture || sourceTexture.pixelFormat() != expectedFormat ||
      sourceRect.left < 0 || sourceRect.top < 0 ||
      sourceRect.right <= sourceRect.left || sourceRect.bottom <= sourceRect.top)
    return false;
  const auto width = static_cast<std::uint32_t>(sourceRect.right - sourceRect.left);
  const auto height = static_cast<std::uint32_t>(sourceRect.bottom - sourceRect.top);
  const std::uint32_t bytesPerPixel =
      expectedFormat == WMTPixelFormatRGBA16Float ? 8u : 4u;
  if (width > std::numeric_limits<std::uint32_t>::max() / bytesPerPixel) return false;
  const auto pitch = width * bytesPerPixel;
  const auto byteCount = static_cast<std::uint64_t>(pitch) * height;
  if (byteCount > std::numeric_limits<std::size_t>::max()) return false;
  WMTBufferInfo info{};
  info.length = byteCount;
  info.options = WMTResourceStorageModeShared;
  auto buffer = device.newBuffer(info);
  if (!buffer || !info.memory.ptr) return false;
  perf::countMetalBuffer(static_cast<std::size_t>(byteCount));
  auto command = queue.newCommandBuffer();
  if (!command) return false;
  auto blit = command.blitCommandEncoder();
  if (!blit) return false;
  blit.setLabel(labels::makeLabelStringFmt(
      "Blit[ReadbackSurface %s raw src=0x%llx]",
      expectedFormat == WMTPixelFormatRGBA16Float ? "RGBA16Float"
          : expectedFormat == WMTPixelFormatR32Float ? "R32Float" : "BGRA8Unorm",
      static_cast<unsigned long long>(source.value)));
  UsedHeapSet set;
  considerSurface(pool, source, set);
  emitUseHeap(blit, set);
  blit.copyFromTextureToBuffer(
      sourceTexture, 0u, sourceLevel,
      WMTOrigin{static_cast<std::uint64_t>(sourceRect.left),
                static_cast<std::uint64_t>(sourceRect.top), 0u},
      WMTSize{width, height, 1u}, buffer, 0u, pitch, byteCount);
  blit.endEncoding();
  command.commit();
  const auto started = std::chrono::steady_clock::now();
  command.waitUntilCompleted();
  perf::countSyncWait(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - started).count()));
  if (command.status() != WMTCommandBufferStatusCompleted) return false;
  pixels.bytes.resize(static_cast<std::size_t>(byteCount));
  std::memcpy(pixels.bytes.data(), info.memory.ptr, pixels.bytes.size());
  pixels.pitch = pitch;
  return true;
}

}  // namespace

void encodeReadback(WMT::CommandBuffer& commandBuffer,
                    resources::Pool& pool,
                    const core::ReadbackDesc& readback) {
  auto* src = pool.findSurface(readback.source.value);
  auto* dst = pool.findSurface(readback.destination.value);
  if (!src || !dst || !src->texture) {
    return;
  }
  auto blit = commandBuffer.blitCommandEncoder();
  if (!blit) return;
  blit.setLabel(labels::makeLabelStringFmt(
      "Blit[Readback src=0x%llx dst=0x%llx]",
      static_cast<unsigned long long>(readback.source.value),
      static_cast<unsigned long long>(readback.destination.value)));
  {
    UsedHeapSet set;
    considerSurface(pool, readback.source, set);
    considerSurface(pool, readback.destination, set);
    emitUseHeap(blit, set);
  }
  WMT::Texture sourceTexture{src->resolveTexture ? src->resolveTexture.handle : src->texture.handle};
  const uint32_t w =
      static_cast<uint32_t>(std::max(1, readback.sourceRect.right - readback.sourceRect.left));
  const uint32_t h =
      static_cast<uint32_t>(std::max(1, readback.sourceRect.bottom - readback.sourceRect.top));
  if (!dst->texture) {
    blit.endEncoding();
    return;
  }
  WMTOrigin srcOrigin{static_cast<uint64_t>(readback.sourceRect.left),
                       static_cast<uint64_t>(readback.sourceRect.top), 0};
  WMTSize srcSize{w, h, 1};
  WMTOrigin dstOrigin{0, 0, 0};
  blit.copyFromTextureToTexture(sourceTexture, 0, readback.sourceLevel,
                                 srcOrigin, srcSize,
                                 WMT::Texture{dst->texture.handle}, 0, 0, dstOrigin);
  blit.endEncoding();
}

void encodeStretchRect(WMT::CommandBuffer& commandBuffer,
                        resources::Pool& pool,
                        pipeline::Cache& pipelineCache,
                        WMT::Reference<WMT::Device> device,
                        const core::BackendLimits& limits,
                        WMT::Reference<WMT::BinaryArchive>* archive,
                        const std::string* archivePath,
                        const core::StretchRectDesc& stretch,
                        std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments) {
  auto* src = pool.findSurface(stretch.source.value);
  auto* dst = pool.findSurface(stretch.destination.value);
  if (!src || !dst || !src->texture || !dst->texture) {
    return;
  }
  const bool fullscreenStretch = isFullscreenStretch(*dst, stretch);
  if (canCopyStretchRect(*src, *dst, stretch)) {
    auto blit = commandBuffer.blitCommandEncoder();
    if (!blit) return;
    blit.setLabel(labels::makeLabelStringFmt(
        "Blit[StretchRect src=0x%llx dst=0x%llx]",
        static_cast<unsigned long long>(stretch.source.value),
        static_cast<unsigned long long>(stretch.destination.value)));
    {
      UsedHeapSet set;
      considerSurface(pool, stretch.source, set);
      considerSurface(pool, stretch.destination, set);
      emitUseHeap(blit, set);
    }
    const auto width = static_cast<uint32_t>(
        std::max(1, stretch.sourceRect.right - stretch.sourceRect.left));
    const auto height = static_cast<uint32_t>(
        std::max(1, stretch.sourceRect.bottom - stretch.sourceRect.top));
    WMTOrigin srcOrigin{static_cast<uint64_t>(stretch.sourceRect.left),
                         static_cast<uint64_t>(stretch.sourceRect.top), 0};
    WMTSize srcSize{width, height, 1};
    WMTOrigin dstOrigin{static_cast<uint64_t>(stretch.destinationRect.left),
                         static_cast<uint64_t>(stretch.destinationRect.top), 0};
    blit.copyFromTextureToTexture(WMT::Texture{src->texture.handle}, 0, src->level,
                                   srcOrigin, srcSize,
                                   WMT::Texture{dst->texture.handle}, 0, dst->level,
                                   dstOrigin);
    blit.endEncoding();
    perf::countStretchBlitCopy();
    if (fullscreenStretch) {
      perf::countStretchFullscreen();
    }
    return;
  }
  WMTRenderPassInfo passInfo{};
  passInfo.colors[0].texture = dst->texture.handle;
  passInfo.colors[0].load_action = WMTLoadActionLoad;
  passInfo.colors[0].store_action = WMTStoreActionStore;
  if (dst->resolveTexture) {
    passInfo.colors[0].resolve_texture = dst->resolveTexture.handle;
    passInfo.colors[0].store_action = WMTStoreActionMultisampleResolve;
  }
  attachCounterSampleBuffers(passInfo, sampleBufferAttachments);
  auto encoder = commandBuffer.renderCommandEncoder(passInfo);
  if (!encoder) return;
  encoder.setLabel(labels::makeLabelStringFmt(
      "Stretch[src=0x%llx,dst=0x%llx]",
      static_cast<unsigned long long>(stretch.source.value),
      static_cast<unsigned long long>(stretch.destination.value)));
  const auto pixelFormat = static_cast<u32>(convert::toPixelFormat(dst->desc.format, limits));
  auto pipeline = pipelineCache.getOrBuildStretchPipeline(device, stretch, pixelFormat,
                                                            archive, archivePath).get();
  if (!pipeline) {
    encoder.endEncoding();
    return;
  }
  encoder.setRenderPipelineState(pipeline);
  encoder.setFragmentTexture(WMT::Texture{src->texture.handle}, 0);
  auto sampler = makeLinearOrNearestSampler(device, stretch.linear);
  if (sampler) encoder.setFragmentSamplerState(sampler, 0);
  encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
  encoder.endEncoding();
  perf::countStretchRenderPass();
  if (fullscreenStretch) {
    perf::countStretchFullscreen();
  }
}

void encodeSurfaceCopy(WMT::CommandBuffer& commandBuffer,
                       resources::Pool& pool,
                       pipeline::Cache& pipelineCache,
                       WMT::Reference<WMT::Device> device,
                       const core::BackendLimits& limits,
                       WMT::Reference<WMT::BinaryArchive>* archive,
                       const std::string* archivePath,
                       const core::SurfaceCopyDesc& copy,
                       std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments) {
  auto* src = pool.findSurface(copy.source.value);
  auto* dst = pool.findSurface(copy.destination.value);
  if (!src || !dst || !src->texture || !dst->texture) {
    return;
  }
  const uint32_t srcW =
      static_cast<uint32_t>(std::max(1, copy.sourceRect.right - copy.sourceRect.left));
  const uint32_t srcH =
      static_cast<uint32_t>(std::max(1, copy.sourceRect.bottom - copy.sourceRect.top));
  const uint32_t dstW =
      static_cast<uint32_t>(std::max(1, copy.destinationRect.right - copy.destinationRect.left));
  const uint32_t dstH =
      static_cast<uint32_t>(std::max(1, copy.destinationRect.bottom - copy.destinationRect.top));
  if (srcW == dstW && srcH == dstH) {
    auto blit = commandBuffer.blitCommandEncoder();
    if (!blit) return;
    blit.setLabel(labels::makeLabelStringFmt(
        "Blit[SurfaceCopy src=0x%llx dst=0x%llx]",
        static_cast<unsigned long long>(copy.source.value),
        static_cast<unsigned long long>(copy.destination.value)));
    {
      UsedHeapSet set;
      considerSurface(pool, copy.source, set);
      considerSurface(pool, copy.destination, set);
      emitUseHeap(blit, set);
    }
    WMTOrigin srcOrigin{static_cast<uint64_t>(copy.sourceRect.left),
                         static_cast<uint64_t>(copy.sourceRect.top), 0};
    WMTSize srcSize{srcW, srcH, 1};
    WMTOrigin dstOrigin{static_cast<uint64_t>(copy.destinationRect.left),
                         static_cast<uint64_t>(copy.destinationRect.top), 0};
    blit.copyFromTextureToTexture(WMT::Texture{src->texture.handle}, 0, copy.sourceLevel,
                                   srcOrigin, srcSize,
                                   WMT::Texture{dst->texture.handle}, 0, copy.destinationLevel,
                                   dstOrigin);
    blit.endEncoding();
    return;
  }
  core::StretchRectDesc stretch{};
  stretch.source = copy.source;
  stretch.destination = copy.destination;
  stretch.sourceRect = copy.sourceRect;
  stretch.destinationRect = copy.destinationRect;
  stretch.linear = true;
  stretch.sourceSampleCount =
      src->desc.multiSampleType == core::MultiSampleType::None ? 1u : core::sampleCount(src->desc.multiSampleType);
  stretch.destinationSampleCount =
      dst->desc.multiSampleType == core::MultiSampleType::None ? 1u : core::sampleCount(dst->desc.multiSampleType);
  encodeStretchRect(commandBuffer, pool, pipelineCache, device, limits, archive,
                    archivePath, stretch, sampleBufferAttachments);
}

bool encodeGenerateMipmaps(WMT::CommandBuffer& commandBuffer,
                           resources::Pool& pool,
                           const core::GenerateMipmapsDesc& generate) {
  auto* record = pool.findTexture(generate.texture.value);
  if (!record || !record->texture) {
    return false;
  }
  if (record->physicalMipLevels <= 1u) {
    return true;
  }
  auto blit = commandBuffer.blitCommandEncoder();
  if (!blit) {
    return false;
  }
  blit.setLabel(labels::makeLabelStringFmt(
      "Blit[GenerateMipmaps texture=0x%llx]",
      static_cast<unsigned long long>(generate.texture.value)));
  if (record->isHeapBacked && record->heap.handle != 0) {
    blit.useHeap(record->heap);
    perf::countUseHeap();
  }
  blit.generateMipmaps(WMT::Texture{record->texture.handle});
  blit.endEncoding();
  return true;
}

void encodeColorFill(WMT::CommandBuffer& commandBuffer,
                      resources::Pool& pool,
                      pipeline::Cache& pipelineCache,
                      WMT::Reference<WMT::Device> device,
                      const core::BackendLimits& limits,
                      WMT::Reference<WMT::BinaryArchive>* archive,
                      const std::string* archivePath,
                      const core::ColorFillDesc& fill,
                      std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments) {
  auto* surface = pool.findSurface(fill.destination.value);
  if (!surface || !surface->texture) {
    return;
  }
  WMTRenderPassInfo passInfo{};
  passInfo.colors[0].texture = surface->texture.handle;
  passInfo.colors[0].load_action = fill.hasRect ? WMTLoadActionLoad : WMTLoadActionClear;
  passInfo.colors[0].store_action = WMTStoreActionStore;
  if (surface->resolveTexture) {
    passInfo.colors[0].resolve_texture = surface->resolveTexture.handle;
    passInfo.colors[0].store_action = WMTStoreActionMultisampleResolve;
  }
  if (!fill.hasRect) {
    passInfo.colors[0].clear_color = WMTClearColor{fill.color.r, fill.color.g,
                                                   fill.color.b, fill.color.a};
  }
  attachCounterSampleBuffers(passInfo, sampleBufferAttachments);
  auto encoder = commandBuffer.renderCommandEncoder(passInfo);
  if (!encoder) {
    return;
  }
  encoder.setLabel(labels::makeLabelStringFmt(
      "ColorFill[rt=0x%llx]",
      static_cast<unsigned long long>(fill.destination.value)));
  if (fill.hasRect) {
    WMTScissorRect rect{};
    rect.x = static_cast<uint64_t>(std::max(0, fill.rect.left));
    rect.y = static_cast<uint64_t>(std::max(0, fill.rect.top));
    rect.width = static_cast<uint64_t>(std::max(0, fill.rect.right - fill.rect.left));
    rect.height = static_cast<uint64_t>(std::max(0, fill.rect.bottom - fill.rect.top));
    encoder.setScissorRect(rect);
    const auto pixelFormat = static_cast<u32>(convert::toPixelFormat(surface->desc.format, limits));
    auto pipeline = pipelineCache.getOrBuildFillPipeline(device, fill.color, pixelFormat,
                                                          archive, archivePath).get();
    if (pipeline) {
      encoder.setRenderPipelineState(pipeline);
      encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
    }
  }
  encoder.endEncoding();
}

std::optional<WMTScissorRect> clipClearRect(const core::Rect& rect,
                                            u32 width, u32 height) noexcept {
  const auto left = std::max<std::int64_t>(0, rect.left);
  const auto top = std::max<std::int64_t>(0, rect.top);
  const auto right = std::min<std::int64_t>(static_cast<std::int64_t>(width), rect.right);
  const auto bottom = std::min<std::int64_t>(static_cast<std::int64_t>(height), rect.bottom);
  if (right <= left || bottom <= top) {
    return std::nullopt;
  }
  return WMTScissorRect{static_cast<std::uint64_t>(left), static_cast<std::uint64_t>(top),
                        static_cast<std::uint64_t>(right - left),
                        static_cast<std::uint64_t>(bottom - top)};
}

std::optional<WMTScissorRect> clippedClearScissor(const core::Rect& rect,
                                                   const resources::SurfaceRecord& surface) {
  return clipClearRect(rect, surface.desc.width, surface.desc.height);
}

template <typename Clear>
void encodeClippedClearPass(
    WMT::CommandBuffer& commandBuffer, resources::Pool& pool, pipeline::Cache& pipelineCache,
    WMT::Reference<WMT::Device> device, const core::BackendLimits& limits,
    WMT::Reference<WMT::BinaryArchive>* archive, const std::string* archivePath,
    const Clear& clear, std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments) {
  bool sampleAttachmentConsumed = false;
  const auto samplesForEncoder = [&]() {
    if (sampleAttachmentConsumed) return std::span<const WMTSampleBufferAttachmentInfo>{};
    sampleAttachmentConsumed = true;
    return sampleBufferAttachments;
  };
  // Each attachment gets its own pass. This keeps the PSO descriptor's color
  // format and sample count exact for MRT/MSAA surfaces while Load preserves
  // every pixel outside each requested rectangle.
  if (clear.clearColor) {
    for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
      auto* surface = pool.findSurface(clear.colorAttachments[i].handle.value);
      if (!surface || !surface->texture) continue;
      const auto pixelFormat = static_cast<u32>(convert::toPixelFormat(surface->desc.format, limits));
      const auto sampleCount = std::max(1u, core::sampleCount(surface->desc.multiSampleType));
      auto pipeline = pipelineCache
          .getOrBuildFillPipeline(device, clear.color, pixelFormat, archive, archivePath,
                                  sampleCount)
          .get();
      if (!pipeline) continue;
      WMTRenderPassInfo passInfo{};
      passInfo.colors[0].texture = surface->texture.handle;
      passInfo.colors[0].load_action = WMTLoadActionLoad;
      passInfo.colors[0].store_action = clearColorStoreAction(bool(surface->resolveTexture));
      if (surface->resolveTexture) {
        passInfo.colors[0].resolve_texture = surface->resolveTexture.handle;
      }
      attachCounterSampleBuffers(passInfo, samplesForEncoder());
      auto encoder = commandBuffer.renderCommandEncoder(passInfo);
      if (!encoder) continue;
      encoder.setLabel(labels::makeLabelStringFmt(
          "ClearRect[color%zu=0x%llx]", i,
          static_cast<unsigned long long>(clear.colorAttachments[i].handle.value)));
      encoder.setRenderPipelineState(pipeline);
      for (const auto& rect : clear.rects) {
        if (const auto scissor = clippedClearScissor(rect, *surface)) {
          encoder.setScissorRect(*scissor);
          encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
        }
      }
      encoder.endEncoding();
    }
  }

  auto* depthSurface = pool.findSurface(clear.depthStencil.handle.value);
  const bool writeDepth = clear.clearDepth && depthSurface && depthSurface->texture &&
                          depthSurface->desc.depthStencil &&
                          convert::formatHasDepthAspect(depthSurface->desc.format);
  const bool writeStencil = clear.clearStencil && depthSurface && depthSurface->texture &&
                            depthSurface->desc.depthStencil &&
                            convert::formatHasStencilAspect(depthSurface->desc.format);
  if (!writeDepth && !writeStencil) return;

  // Bind every available aspect with Load/Store, even when only one aspect is
  // being changed. The disabled write in the depth-stencil state preserves
  // the other aspect while keeping the pipeline descriptor compatible with a
  // combined D24S8/Depth32S8 texture.
  const bool hasDepthAspect = convert::formatHasDepthAspect(depthSurface->desc.format);
  const bool hasStencilAspect = convert::formatHasStencilAspect(depthSurface->desc.format);
  const auto depthFormat = hasDepthAspect
      ? static_cast<u32>(convert::toPixelFormat(depthSurface->desc.format, limits)) : 0u;
  const auto stencilFormat = hasStencilAspect
      ? static_cast<u32>(convert::toPixelFormat(depthSurface->desc.format, limits)) : 0u;
  const auto sampleCount = std::max(1u, core::sampleCount(depthSurface->desc.multiSampleType));
  auto pipeline = pipelineCache.getOrBuildDepthStencilClearPipeline(
      device, clear.depth, depthFormat, stencilFormat, sampleCount, writeDepth, writeStencil,
      archive, archivePath).get();
  if (!pipeline) return;
  WMTRenderPassInfo passInfo{};
  if (hasDepthAspect) {
    passInfo.depth.texture = depthSurface->texture.handle;
    passInfo.depth.load_action = WMTLoadActionLoad;
    passInfo.depth.store_action = WMTStoreActionStore;
  }
  if (hasStencilAspect) {
    passInfo.stencil.texture = depthSurface->texture.handle;
    passInfo.stencil.load_action = WMTLoadActionLoad;
    passInfo.stencil.store_action = WMTStoreActionStore;
  }
  WMTDepthStencilInfo stateInfo{};
  stateInfo.depth_compare_function = WMTCompareFunctionAlways;
  stateInfo.depth_write_enabled = writeDepth;
  stateInfo.front_stencil.enabled = writeStencil;
  stateInfo.front_stencil.stencil_compare_function = WMTCompareFunctionAlways;
  stateInfo.front_stencil.depth_stencil_pass_op = WMTStencilOperationReplace;
  stateInfo.front_stencil.write_mask = 0xffu;
  stateInfo.front_stencil.read_mask = 0xffu;
  stateInfo.back_stencil = stateInfo.front_stencil;
  auto depthStencilState = device.newDepthStencilState(stateInfo);
  if (!depthStencilState) return;
  attachCounterSampleBuffers(passInfo, samplesForEncoder());
  auto encoder = commandBuffer.renderCommandEncoder(passInfo);
  if (!encoder) return;
  encoder.setDepthStencilState(depthStencilState,
                               static_cast<std::uint8_t>(clear.stencil & 0xffu));
  encoder.setRenderPipelineState(pipeline);
  encoder.setLabel(labels::makeLabelStringFmt(
      "ClearRect[depth=0x%llx]",
      static_cast<unsigned long long>(clear.depthStencil.handle.value)));
  for (const auto& rect : clear.rects) {
    if (const auto scissor = clippedClearScissor(rect, *depthSurface)) {
      encoder.setScissorRect(*scissor);
      encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
    }
  }
  encoder.endEncoding();
}

template <typename Clear>
void encodeClearPassImpl(
    WMT::CommandBuffer& commandBuffer,
    resources::Pool& pool,
    const Clear& clear,
    std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments,
    pipeline::Cache* pipelineCache = nullptr,
    WMT::Reference<WMT::Device> device = {},
    const core::BackendLimits* limits = nullptr,
    WMT::Reference<WMT::BinaryArchive>* archive = nullptr,
    const std::string* archivePath = nullptr) {
  if (!clear.rects.empty()) {
    if (pipelineCache && device && limits) {
      encodeClippedClearPass(commandBuffer, pool, *pipelineCache, device, *limits, archive,
                             archivePath, clear, sampleBufferAttachments);
    } else {
      DXMT_ASSERT(false && "clipped clear requires pipeline/device context");
    }
    return;
  }
  const bool hasDepthStencilTarget =
      (clear.clearDepth || clear.clearStencil) && clear.depthStencil.handle;
  const bool hasColorTarget = clear.clearColor &&
      std::any_of(clear.colorAttachments.begin(), clear.colorAttachments.end(),
                  [](const auto& attachment) { return static_cast<bool>(attachment.handle); });
  if (!hasColorTarget && !hasDepthStencilTarget) {
    return;
  }
  WMTRenderPassInfo passInfo{};
  bool hasAttachment = false;
  if (clear.clearColor) {
    for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
      auto* surface = pool.findSurface(clear.colorAttachments[i].handle.value);
      if (!surface || !surface->texture) {
        continue;
      }
      auto& color = passInfo.colors[i];
      color.texture = surface->texture.handle;
      color.load_action = WMTLoadActionClear;
      color.store_action = clearColorStoreAction(bool(surface->resolveTexture));
      color.clear_color = WMTClearColor{clear.color.r, clear.color.g,
                                        clear.color.b, clear.color.a};
      if (surface->resolveTexture) {
        color.resolve_texture = surface->resolveTexture.handle;
      }
      hasAttachment = true;
    }
  }

  auto* depthSurface = pool.findSurface(clear.depthStencil.handle.value);
  if (depthSurface && depthSurface->texture && depthSurface->desc.depthStencil) {
    if (clear.clearDepth && dxmt9::convert::formatHasDepthAspect(depthSurface->desc.format)) {
      passInfo.depth.texture = depthSurface->texture.handle;
      passInfo.depth.load_action = WMTLoadActionClear;
      passInfo.depth.store_action = WMTStoreActionStore;
      passInfo.depth.clear_depth = clear.depth;
      hasAttachment = true;
    }
    if (clear.clearStencil && dxmt9::convert::formatHasStencilAspect(depthSurface->desc.format)) {
      passInfo.stencil.texture = depthSurface->texture.handle;
      passInfo.stencil.load_action = WMTLoadActionClear;
      passInfo.stencil.store_action = WMTStoreActionStore;
      passInfo.stencil.clear_stencil = clear.stencil;
      hasAttachment = true;
    }
  }

  if (!hasAttachment) {
    return;
  }
  attachCounterSampleBuffers(passInfo, sampleBufferAttachments);
  auto encoder = commandBuffer.renderCommandEncoder(passInfo);
  if (encoder) {
    const auto rt0 = static_cast<unsigned long long>(
        clear.colorAttachments[0].handle.value);
    const auto depth = static_cast<unsigned long long>(
        clear.depthStencil.handle.value);
    encoder.setLabel(labels::makeLabelStringFmt(
        "Clear[rt=0x%llx,depth=0x%llx]", rt0, depth));
    encoder.endEncoding();
  }
}

void encodeClearPass(
    WMT::CommandBuffer& commandBuffer,
    resources::Pool& pool,
    const core::ClearDesc& clear,
    std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments) {
  encodeClearPassImpl(commandBuffer, pool, clear, sampleBufferAttachments);
}

void encodeClearPass(
    WMT::CommandBuffer& commandBuffer,
    resources::Pool& pool,
    const core::ClearCommandView& clear,
    std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments) {
  encodeClearPassImpl(commandBuffer, pool, clear, sampleBufferAttachments);
}

void encodeClearPass(
    WMT::CommandBuffer& commandBuffer, resources::Pool& pool, pipeline::Cache& pipelineCache,
    WMT::Reference<WMT::Device> device, const core::BackendLimits& limits,
    WMT::Reference<WMT::BinaryArchive>* archive, const std::string* archivePath,
    const core::ClearDesc& clear,
    std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments) {
  encodeClearPassImpl(commandBuffer, pool, clear, sampleBufferAttachments, &pipelineCache,
                      device, &limits, archive, archivePath);
}

void encodeClearPass(
    WMT::CommandBuffer& commandBuffer, resources::Pool& pool, pipeline::Cache& pipelineCache,
    WMT::Reference<WMT::Device> device, const core::BackendLimits& limits,
    WMT::Reference<WMT::BinaryArchive>* archive, const std::string* archivePath,
    const core::ClearCommandView& clear,
    std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments) {
  encodeClearPassImpl(commandBuffer, pool, clear, sampleBufferAttachments, &pipelineCache,
                      device, &limits, archive, archivePath);
}

void encodeDepthResolve(WMT::CommandBuffer& commandBuffer,
                        resources::Pool& pool,
                        core::Handle msaaDepthSource,
                        core::Handle intzDestination,
                        std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments) {
  // R-FORMAT-11 — RESZ multisample depth resolve. The DEPTH twin of the
  // color MSAA resolve in encodeStretchRect/encodeColorFill/encodeClearPass:
  // open a render pass whose depth attachment binds the multisampled depth
  // surface as `texture`, the INTZ destination as `resolve_texture`, with
  // store=MultisampleResolve + filter=Sample, then immediately end the
  // encoder. Metal performs the depth resolve at encoder-end exactly as it
  // does for the color resolve store action — no draws are recorded.
  auto* src = pool.findSurface(msaaDepthSource.value);
  auto* dst = pool.findSurface(intzDestination.value);
  if (!src || !dst || !src->texture || !dst->texture) {
    // Benign no-op: the RESZ idiom is fire-and-forget on real hardware too.
    return;
  }
  // The source must be a multisampled depth surface; the destination must
  // carry a depth aspect (INTZ). A mismatched pair is a no-op rather than a
  // mis-encode.
  if (!src->desc.depthStencil ||
      !dxmt9::convert::formatHasDepthAspect(src->desc.format) ||
      !dxmt9::convert::formatHasDepthAspect(dst->desc.format)) {
    return;
  }
  WMTRenderPassInfo passInfo{};
  passInfo.depth.texture = src->texture.handle;
  passInfo.depth.load_action = WMTLoadActionLoad;
  passInfo.depth.store_action = WMTStoreActionMultisampleResolve;
  passInfo.depth.resolve_texture = dst->texture.handle;
  passInfo.depth.resolve_filter = WMTMultisampleDepthResolveFilterSample;
  attachCounterSampleBuffers(passInfo, sampleBufferAttachments);
  auto encoder = commandBuffer.renderCommandEncoder(passInfo);
  if (!encoder) {
    return;
  }
  encoder.setLabel(labels::makeLabelStringFmt(
      "DepthResolve[src=0x%llx,intz=0x%llx]",
      static_cast<unsigned long long>(msaaDepthSource.value),
      static_cast<unsigned long long>(intzDestination.value)));
  encoder.endEncoding();
}

bool readbackSurface(CommandQueue& queue,
                      resources::Pool& pool,
                      WMT::Reference<WMT::Device> device,
                      const core::BackendLimits& limits,
                      const core::ReadbackDesc& desc,
                      core::ReadbackPixels& pixels) {
  using u32 = std::uint32_t;
  using i32 = std::int32_t;

  WMT::Reference<WMT::Texture> sourceTexture;
  core::Format format = core::Format::Unknown;
  core::Rect sourceRect{};
  u32 sourceLevel = desc.sourceLevel;
  {
    std::lock_guard lock(queue.mutex_);
    auto* surface = pool.findSurface(desc.source.value);
    if (!surface || !surface->texture) {
      return false;
    }
    sourceTexture = surface->resolveTexture ? surface->resolveTexture : surface->texture;
    format = surface->desc.format;
    sourceRect = desc.sourceRect;
    if (sourceRect.right <= sourceRect.left || sourceRect.bottom <= sourceRect.top) {
      sourceRect.right = static_cast<i32>(surface->desc.width);
      sourceRect.bottom = static_cast<i32>(surface->desc.height);
    }
  }

  const u32 width = static_cast<u32>(std::max(1, sourceRect.right - sourceRect.left));
  const u32 height = static_cast<u32>(std::max(1, sourceRect.bottom - sourceRect.top));
  const u32 bpp = core::bytesPerPixel(format);
  if (!sourceTexture || bpp == 0) {
    return false;
  }

  if (format == core::Format::A16B16G16R16F)
    return readbackRawColorTexture(queue, pool, device, desc.source,
                                  sourceTexture, WMTPixelFormatRGBA16Float,
                                  sourceRect, sourceLevel, pixels);

  WMTTextureInfo stagingInfo{};
  stagingInfo.type = WMTTextureType2D;
  stagingInfo.pixel_format = dxmt9::convert::toPixelFormat(format, limits);
  stagingInfo.width = width;
  stagingInfo.height = height;
  stagingInfo.depth = 1;
  stagingInfo.mipmap_level_count = 1;
  stagingInfo.sample_count = 1;
  stagingInfo.array_length = 1;
  stagingInfo.options = WMTResourceStorageModeShared;
  stagingInfo.usage = WMTTextureUsageShaderRead;
  auto stagingTexture = device.newTexture(stagingInfo);
  if (!stagingTexture) {
    return false;
  }

  auto commandBuffer = queue.newCommandBuffer();
  if (!commandBuffer) {
    return false;
  }
  auto blit = commandBuffer.blitCommandEncoder();
  if (!blit) {
    return false;
  }
  blit.setLabel(labels::makeLabelStringFmt(
      "Blit[ReadbackSurface stage src=0x%llx]",
      static_cast<unsigned long long>(desc.source.value)));
  {
    // Source is a Pool surface that may alias a heap-backed parent
    // texture; destination is an ephemeral staging texture (not pooled,
    // never heap-backed).
    UsedHeapSet set;
    considerSurface(pool, desc.source, set);
    emitUseHeap(blit, set);
  }
  WMTOrigin srcOrigin{(uint64_t)sourceRect.left, (uint64_t)sourceRect.top, 0};
  WMTSize srcSize{width, height, 1};
  WMTOrigin dstOrigin{0, 0, 0};
  blit.copyFromTextureToTexture(WMT::Texture{sourceTexture.handle}, 0, sourceLevel,
                                srcOrigin, srcSize,
                                WMT::Texture{stagingTexture.handle}, 0, 0, dstOrigin);
  blit.endEncoding();
  commandBuffer.commit();
  const auto copyStarted = std::chrono::steady_clock::now();
  commandBuffer.waitUntilCompleted();
  const auto copyElapsed = std::chrono::steady_clock::now() - copyStarted;
  perf::countSyncWait(
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(copyElapsed).count()));

  pixels.pitch = width * bpp;
  pixels.bytes.resize(static_cast<std::size_t>(pixels.pitch) * height);
  WMTBufferInfo bufInfo{};
  bufInfo.length = static_cast<uint64_t>(pixels.pitch) * height;
  bufInfo.options = WMTResourceStorageModeShared;
  auto readbackBuf = device.newBuffer(bufInfo);
  if (readbackBuf) {
    perf::countMetalBuffer(static_cast<std::size_t>(bufInfo.length));
  }
  if (readbackBuf) {
    auto cmdBuf2 = queue.newCommandBuffer();
    if (cmdBuf2) {
      auto blit2 = cmdBuf2.blitCommandEncoder();
      if (blit2) {
        blit2.setLabel(labels::makeLabelStringFmt(
            "Blit[ReadbackSurface drain src=0x%llx]",
            static_cast<unsigned long long>(desc.source.value)));
        // Both source (staging) and destination (readback buffer) are
        // ephemeral, non-pooled allocations — neither can be
        // heap-backed, so no useHeap is required. The empty walk is
        // kept for symmetry with other blit sites in this file.
        UsedHeapSet set;
        emitUseHeap(blit2, set);
        WMTOrigin origin{0, 0, 0};
        WMTSize size{width, height, 1};
        blit2.copyFromTextureToBuffer(WMT::Texture{stagingTexture.handle}, 0, 0,
                                      origin, size, WMT::Buffer{readbackBuf.handle},
                                      0, pixels.pitch, 0);
        blit2.endEncoding();
      }
      cmdBuf2.commit();
      const auto readStarted = std::chrono::steady_clock::now();
      cmdBuf2.waitUntilCompleted();
      const auto readElapsed = std::chrono::steady_clock::now() - readStarted;
      perf::countSyncWait(
          static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(readElapsed).count()));
    }
    if (bufInfo.memory.ptr) {
      std::memcpy(pixels.bytes.data(), bufInfo.memory.ptr, pixels.bytes.size());
    }
  }
  return true;
}

bool captureRenderTapeColorTexture(CommandQueue& queue, resources::Pool& pool,
                               WMT::Reference<WMT::Device> device,
                               core::SurfaceHandle source,
                               core::ReadbackPixels& pixels) {
  pixels = {};
  // This cold capture, unlike ordinary readbackSurface, establishes both
  // initializer completion and producer exclusion, even for an empty writer.
  // The lease already owns mutex_: lookup and heap traversal must not relock.
  auto lease = queue.beginRenderTapeCheckpoint();
  if (!device || !lease) return false;
  const auto* record = pool.findSurface(source.value);
  if (!record || !record->texture || record->resolveTexture ||
      (record->desc.format != core::Format::A8R8G8B8 &&
       record->desc.format != core::Format::A16B16G16R16F &&
       record->desc.format != core::Format::R32F) ||
      record->desc.pool != core::Pool::Default || !record->desc.renderTarget ||
      record->desc.depthStencil ||
      record->desc.multiSampleType != core::MultiSampleType::None ||
      record->desc.width == 0u || record->desc.height == 0u ||
      record->desc.height > std::numeric_limits<std::int32_t>::max())
    return false;
  const bool rgba16Float = record->desc.format == core::Format::A16B16G16R16F;
  const bool r32Float = record->desc.format == core::Format::R32F;
  const std::uint32_t bytesPerPixel = rgba16Float ? 8u : 4u;
  const auto physicalFormat =
      rgba16Float ? WMTPixelFormatRGBA16Float
          : r32Float ? WMTPixelFormatR32Float : WMTPixelFormatBGRA8Unorm;
  if (record->desc.width >
      std::numeric_limits<std::int32_t>::max() / bytesPerPixel) return false;
  WMT::Reference<WMT::Texture> texture = record->texture;
  if (texture.pixelFormat() != physicalFormat ||
      texture.textureType() != WMTTextureType2D || texture.depth() != 1u ||
      texture.arrayLength() != 1u || texture.mipmapLevelCount() != 1u ||
      texture.width() != record->desc.width || texture.height() != record->desc.height)
    return false;
  const core::Rect rectangle{0, 0, static_cast<std::int32_t>(record->desc.width),
                                  static_cast<std::int32_t>(record->desc.height)};
  // The exact retained backing and exclusive lease survive GPU completion
  // and the host copy. No format conversion or numeric operation is involved.
  return readbackRawColorTexture(queue, pool, device, source, texture,
                                physicalFormat, rectangle, 0u, pixels);
}

namespace {

struct D24X8ConversionPipelines {
  WMT::Reference<WMT::Library> library;
  WMT::Reference<WMT::RenderPipelineState> read;
  WMT::Reference<WMT::RenderPipelineState> write;
  WMT::Reference<WMT::DepthStencilState> depthState;
};

D24X8ConversionPipelines makeD24X8ConversionPipelines(
    WMT::Reference<WMT::Device> device, WMTPixelFormat physicalFormat) {
  static constexpr char kSource[] = R"msl(
#include <metal_stdlib>
using namespace metal;
struct VertexOut { float4 position [[position, invariant]]; };
vertex VertexOut dxmt9_d24x8_snapshot_vs(uint vertexId [[vertex_id]]) {
  const float2 p[3] = {
    float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0),
  };
  return VertexOut{float4(p[vertexId], 0.0, 1.0)};
}
fragment float dxmt9_d24x8_capture_fs(
    VertexOut in [[stage_in]], depth2d<float> source [[texture(0)]]) {
  return source.read(uint2(in.position.xy));
}
struct DepthOut { float depth [[depth(any)]]; };
fragment DepthOut dxmt9_d24x8_seed_fs(
    VertexOut in [[stage_in]], texture2d<float> source [[texture(0)]]) {
  return DepthOut{source.read(uint2(in.position.xy)).r};
}
)msl";
  D24X8ConversionPipelines result;
  WMT::Error error{};
  result.library = device.newLibraryFromSource(kSource, error);
  if (!result.library) return {};
  auto vertex = result.library.newFunction("dxmt9_d24x8_snapshot_vs");
  auto capture = result.library.newFunction("dxmt9_d24x8_capture_fs");
  auto seed = result.library.newFunction("dxmt9_d24x8_seed_fs");
  if (!vertex || !capture || !seed) return {};

  WMTRenderPipelineInfo readInfo{};
  readInfo.vertex_function = vertex.handle;
  readInfo.fragment_function = capture.handle;
  readInfo.colors[0].pixel_format = WMTPixelFormatR32Float;
  readInfo.colors[0].write_mask = WMTColorWriteMaskAll;
  readInfo.rasterization_enabled = true;
  readInfo.raster_sample_count = 1;
  readInfo.max_tessellation_factor = 1;
  result.read = device.newRenderPipelineState(readInfo, error);

  WMTRenderPipelineInfo writeInfo{};
  writeInfo.vertex_function = vertex.handle;
  writeInfo.fragment_function = seed.handle;
  writeInfo.depth_pixel_format = physicalFormat;
  writeInfo.rasterization_enabled = true;
  writeInfo.raster_sample_count = 1;
  writeInfo.max_tessellation_factor = 1;
  result.write = device.newRenderPipelineState(writeInfo, error);

  WMTDepthStencilInfo depthInfo{};
  depthInfo.depth_compare_function = WMTCompareFunctionAlways;
  depthInfo.depth_write_enabled = true;
  result.depthState = device.newDepthStencilState(depthInfo);
  return result;
}

bool checkedD24X8Layout(std::uint32_t width, std::uint32_t height,
                        std::uint32_t& pitch, std::size_t& bytes) {
  if (width == 0u || height == 0u ||
      width > std::numeric_limits<std::uint32_t>::max() / sizeof(float)) {
    return false;
  }
  pitch = width * sizeof(float);
  if (height > std::numeric_limits<std::size_t>::max() / pitch) {
    return false;
  }
  bytes = static_cast<std::size_t>(pitch) * height;
  return bytes != 0u;
}

bool commandBufferCompleted(WMT::Reference<WMT::CommandBuffer>& commandBuffer) {
  if (!commandBuffer) return false;
  commandBuffer.commit();
  const auto started = std::chrono::steady_clock::now();
  commandBuffer.waitUntilCompleted();
  perf::countSyncWait(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - started).count()));
  return commandBuffer.status() == WMTCommandBufferStatusCompleted;
}

}  // namespace

bool captureCanonicalD24X8Depth(CommandQueue& queue,
                                resources::Pool& pool,
                                WMT::Reference<WMT::Device> device,
                                const core::BackendLimits& limits,
                                core::SurfaceHandle source,
                                core::CanonicalD24X8Depth& depth) {
  depth = {};
  WMT::Reference<WMT::Texture> sourceTexture;
  core::SurfaceDesc desc{};
  {
    std::lock_guard lock(queue.mutex_);
    const auto* record = pool.findSurface(source.value);
    if (!record || !record->texture || record->resolveTexture) return false;
    desc = record->desc;
    sourceTexture = record->texture;
  }
  const auto physicalFormat = convert::toPixelFormat(core::Format::D24X8, limits);
  if (!device || desc.format != core::Format::D24X8 || !desc.depthStencil ||
      desc.renderTarget || desc.multiSampleType != core::MultiSampleType::None ||
      sourceTexture.pixelFormat() != physicalFormat ||
      !canonicalD24X8PhysicalFormatSupported(physicalFormat)) {
    return false;
  }
  std::uint32_t pitch = 0u;
  std::size_t byteCount = 0u;
  if (!checkedD24X8Layout(desc.width, desc.height, pitch, byteCount)) return false;
  auto pipelines = makeD24X8ConversionPipelines(device, physicalFormat);
  if (!pipelines.read) return false;

  WMTTextureInfo canonicalInfo{};
  canonicalInfo.type = WMTTextureType2D;
  canonicalInfo.pixel_format = WMTPixelFormatR32Float;
  canonicalInfo.width = desc.width;
  canonicalInfo.height = desc.height;
  canonicalInfo.depth = 1u;
  canonicalInfo.mipmap_level_count = 1u;
  canonicalInfo.sample_count = 1u;
  canonicalInfo.array_length = 1u;
  canonicalInfo.options = WMTResourceStorageModePrivate;
  canonicalInfo.usage = WMTTextureUsageRenderTarget;
  auto canonical = device.newTexture(canonicalInfo);
  WMTBufferInfo bufferInfo{};
  bufferInfo.length = byteCount;
  bufferInfo.options = WMTResourceStorageModeShared;
  auto buffer = device.newBuffer(bufferInfo);
  if (!canonical || !buffer || !bufferInfo.memory.ptr) return false;

  auto commandBuffer = queue.newCommandBuffer();
  if (!commandBuffer) return false;
  WMTRenderPassInfo pass{};
  pass.colors[0].texture = canonical.handle;
  pass.colors[0].load_action = WMTLoadActionDontCare;
  pass.colors[0].store_action = WMTStoreActionStore;
  auto encoder = commandBuffer.renderCommandEncoder(pass);
  if (!encoder) return false;
  encoder.setRenderPipelineState(pipelines.read);
  encoder.setFragmentTexture(sourceTexture, 0u);
  encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0u, 3u);
  encoder.endEncoding();
  auto blit = commandBuffer.blitCommandEncoder();
  if (!blit) return false;
  blit.copyFromTextureToBuffer(
      canonical, 0u, 0u, WMTOrigin{0u, 0u, 0u},
      WMTSize{desc.width, desc.height, 1u}, buffer, 0u, pitch, byteCount);
  blit.endEncoding();
  if (!commandBufferCompleted(commandBuffer)) return false;

  try {
    depth.bytes.resize(byteCount);
  } catch (...) {
    return false;
  }
  std::memcpy(depth.bytes.data(), bufferInfo.memory.ptr, byteCount);
  depth.version = core::kCanonicalD24X8DepthVersion1;
  depth.width = desc.width;
  depth.height = desc.height;
  depth.pitch = pitch;
  depth.physicalFormat = static_cast<std::uint32_t>(physicalFormat);
  return true;
}

bool seedCanonicalD24X8Depth(CommandQueue& queue,
                             resources::Pool& pool,
                             WMT::Reference<WMT::Device> device,
                             const core::BackendLimits& limits,
                             core::SurfaceHandle destination,
                             const core::CanonicalD24X8Depth& depth) {
  WMT::Reference<WMT::Texture> destinationTexture;
  core::SurfaceDesc desc{};
  {
    std::lock_guard lock(queue.mutex_);
    const auto* record = pool.findSurface(destination.value);
    if (!record || !record->texture || record->resolveTexture) return false;
    desc = record->desc;
    destinationTexture = record->texture;
  }
  const auto physicalFormat = convert::toPixelFormat(core::Format::D24X8, limits);
  std::uint32_t pitch = 0u;
  std::size_t byteCount = 0u;
  if (!device || desc.format != core::Format::D24X8 || !desc.depthStencil ||
      desc.renderTarget || desc.multiSampleType != core::MultiSampleType::None ||
      destinationTexture.pixelFormat() != physicalFormat ||
      depth.version != core::kCanonicalD24X8DepthVersion1 ||
      depth.width != desc.width || depth.height != desc.height ||
      !canonicalD24X8PhysicalFormatSupported(physicalFormat) ||
      !checkedD24X8Layout(desc.width, desc.height, pitch, byteCount) ||
      depth.pitch != pitch || depth.bytes.size() != byteCount) {
    return false;
  }
  auto pipelines = makeD24X8ConversionPipelines(device, physicalFormat);
  if (!pipelines.write || !pipelines.depthState) return false;
  WMTTextureInfo uploadInfo{};
  uploadInfo.type = WMTTextureType2D;
  uploadInfo.pixel_format = WMTPixelFormatR32Float;
  uploadInfo.width = desc.width;
  uploadInfo.height = desc.height;
  uploadInfo.depth = 1u;
  uploadInfo.mipmap_level_count = 1u;
  uploadInfo.sample_count = 1u;
  uploadInfo.array_length = 1u;
  uploadInfo.options = WMTResourceStorageModeShared;
  uploadInfo.usage = WMTTextureUsageShaderRead;
  auto upload = device.newTexture(uploadInfo);
  if (!upload) return false;
  upload.replaceRegion(WMTOrigin{0u, 0u, 0u},
                       WMTSize{desc.width, desc.height, 1u}, 0u, 0u,
                       depth.bytes.data(), pitch, byteCount);

  auto commandBuffer = queue.newCommandBuffer();
  if (!commandBuffer) return false;
  WMTRenderPassInfo pass{};
  pass.depth.texture = destinationTexture.handle;
  pass.depth.load_action = WMTLoadActionDontCare;
  pass.depth.store_action = WMTStoreActionStore;
  auto encoder = commandBuffer.renderCommandEncoder(pass);
  if (!encoder) return false;
  encoder.setRenderPipelineState(pipelines.write);
  encoder.setDepthStencilState(pipelines.depthState);
  encoder.setFragmentTexture(upload, 0u);
  encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0u, 3u);
  encoder.endEncoding();
  return commandBufferCompleted(commandBuffer);
}

}  // namespace dxmt9::encoders
