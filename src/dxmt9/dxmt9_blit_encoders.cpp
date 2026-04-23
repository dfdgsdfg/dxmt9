#include "dxmt9_blit_encoders.hpp"

#include "dxmt9/dxmt9_command_queue.hpp"
#include "dxmt9_format_convert.hpp"

#include <algorithm>
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
  if (clear.colorAttachments[0].handle == core::Handle{} &&
      clear.depthStencil.handle == core::Handle{}) {
    return;
  }
  auto* surface = pool.findSurface(clear.colorAttachments[0].handle.value);
  if (!surface || !surface->texture) {
    return;
  }
  WMTRenderPassInfo passInfo{};
  passInfo.colors[0].texture = surface->texture.handle;
  passInfo.colors[0].load_action = WMTLoadActionClear;
  passInfo.colors[0].store_action = WMTStoreActionStore;
  if (surface->resolveTexture) {
    passInfo.colors[0].resolve_texture = surface->resolveTexture.handle;
    passInfo.colors[0].store_action = WMTStoreActionMultisampleResolve;
  }
  passInfo.colors[0].clear_color = WMTClearColor{clear.color.r, clear.color.g,
                                                 clear.color.b, clear.color.a};
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
  using u8 = std::uint8_t;
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
  WMTOrigin srcOrigin{(uint64_t)sourceRect.left, (uint64_t)sourceRect.top, 0};
  WMTSize srcSize{width, height, 1};
  WMTOrigin dstOrigin{0, 0, 0};
  blit.copyFromTextureToTexture(WMT::Texture{sourceTexture.handle}, 0, sourceLevel,
                                srcOrigin, srcSize,
                                WMT::Texture{stagingTexture.handle}, 0, 0, dstOrigin);
  blit.endEncoding();
  commandBuffer.commit();
  commandBuffer.waitUntilCompleted();

  pixels.pitch = width * bpp;
  pixels.bytes.resize(static_cast<std::size_t>(pixels.pitch) * height);
  WMTBufferInfo bufInfo{};
  bufInfo.length = static_cast<uint64_t>(pixels.pitch) * height;
  bufInfo.options = WMTResourceStorageModeShared;
  auto readbackBuf = device.newBuffer(bufInfo);
  if (readbackBuf) {
    auto cmdBuf2 = queue.newCommandBuffer();
    if (cmdBuf2) {
      auto blit2 = cmdBuf2.blitCommandEncoder();
      if (blit2) {
        WMTOrigin origin{0, 0, 0};
        WMTSize size{width, height, 1};
        blit2.copyFromTextureToBuffer(WMT::Texture{stagingTexture.handle}, 0, 0,
                                      origin, size, WMT::Buffer{readbackBuf.handle},
                                      0, pixels.pitch, 0);
        blit2.endEncoding();
      }
      cmdBuf2.commit();
      cmdBuf2.waitUntilCompleted();
    }
    if (bufInfo.memory.ptr) {
      std::memcpy(pixels.bytes.data(), bufInfo.memory.ptr, pixels.bytes.size());
    }
  }
  return true;
}

}  // namespace dxmt9::encoders
