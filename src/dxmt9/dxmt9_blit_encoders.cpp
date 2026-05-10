#include "dxmt9_blit_encoders.hpp"

#include "dxmt9_command_queue.hpp"
#include "dxmt9_format_convert.hpp"
#include "dxmt9_perf_counters.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>

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

// R-BACK-14.3 — issue `useHeap:` once per live heap instance on a freshly
// opened blit encoder. The render-encoder path was tightened to walk only
// the bound resources and useHeap the heaps that actually back them; the
// blit helpers below operate on SurfaceRecord src/dst pairs, which are
// excluded from heap pooling by R-BACK-14.1, so the per-encoder set is
// always empty and this call is a no-op when no heap-backed resource is
// in play. TODO(R-BACK-14.3): plumb a per-encoder bound-resource list
// (Buffer/Texture handles) from the few blit callers that may touch
// heap-backed members and switch to the same dedup walk used in
// beginRenderPass; until then keep the broad walk so heap-resident
// bookkeeping stays correct on every blit path.
void useHeapsOnBlit(WMT::BlitCommandEncoder& blit, resources::Pool& pool) {
  pool.heapManager().forEachHeapInstance([&blit](WMT::Heap heap) {
    blit.useHeap(heap);
    perf::countUseHeap();
  });
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
  useHeapsOnBlit(blit, pool);
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
                        const core::StretchRectDesc& stretch) {
  auto* src = pool.findSurface(stretch.source.value);
  auto* dst = pool.findSurface(stretch.destination.value);
  if (!src || !dst || !src->texture || !dst->texture) {
    return;
  }
  const bool fullscreenStretch = isFullscreenStretch(*dst, stretch);
  if (canCopyStretchRect(*src, *dst, stretch)) {
    auto blit = commandBuffer.blitCommandEncoder();
    if (!blit) return;
    useHeapsOnBlit(blit, pool);
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
  auto encoder = commandBuffer.renderCommandEncoder(passInfo);
  if (!encoder) return;
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
                       const core::SurfaceCopyDesc& copy) {
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
    useHeapsOnBlit(blit, pool);
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
  encodeStretchRect(commandBuffer, pool, pipelineCache, device, limits, archive, archivePath, stretch);
}

void encodeColorFill(WMT::CommandBuffer& commandBuffer,
                      resources::Pool& pool,
                      pipeline::Cache& pipelineCache,
                      WMT::Reference<WMT::Device> device,
                      const core::BackendLimits& limits,
                      WMT::Reference<WMT::BinaryArchive>* archive,
                      const std::string* archivePath,
                      const core::ColorFillDesc& fill) {
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
  auto encoder = commandBuffer.renderCommandEncoder(passInfo);
  if (!encoder) {
    return;
  }
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

void encodeClearPass(WMT::CommandBuffer& commandBuffer,
                      resources::Pool& pool,
                      const core::ClearDesc& clear) {
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
      color.store_action = WMTStoreActionStore;
      color.clear_color = WMTClearColor{clear.color.r, clear.color.g,
                                        clear.color.b, clear.color.a};
      if (surface->resolveTexture) {
        color.resolve_texture = surface->resolveTexture.handle;
        color.store_action = WMTStoreActionMultisampleResolve;
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
  auto encoder = commandBuffer.renderCommandEncoder(passInfo);
  if (encoder) {
    encoder.endEncoding();
  }
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
  useHeapsOnBlit(blit, pool);
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
        useHeapsOnBlit(blit2, pool);
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

}  // namespace dxmt9::encoders
