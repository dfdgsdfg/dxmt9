#include "dxmt9_blit_encoders.hpp"
#include "dxmt9_command_queue.hpp"
#include "dxmt9_perf_counters.hpp"

#import <Metal/Metal.h>

#include <bit>
#include <chrono>
#include <cstring>
#include <limits>

namespace dxmt9::encoders {
namespace {

struct Layout {
  std::uint32_t depthPitch = 0;
  std::uint32_t stencilPitch = 0;
  std::size_t stencilOffset = 0;
  std::size_t bytes = 0;
};

bool layoutFor(const core::SurfaceDesc& desc, Layout& layout) {
  static_assert(std::endian::native == std::endian::little);
  if (desc.format != core::Format::D24S8 || !desc.depthStencil ||
      desc.renderTarget || desc.multiSampleType != core::MultiSampleType::None ||
      desc.width == 0u || desc.height == 0u ||
      desc.width > std::numeric_limits<std::uint32_t>::max() / sizeof(float))
    return false;
  const auto pixels = static_cast<std::uint64_t>(desc.width) * desc.height;
  if (pixels > std::numeric_limits<std::size_t>::max() / 5u) return false;
  layout.depthPitch = desc.width * sizeof(float);
  layout.stencilPitch = desc.width;
  layout.stencilOffset = static_cast<std::size_t>(pixels) * sizeof(float);
  layout.bytes = static_cast<std::size_t>(pixels) * 5u;
  return true;
}

bool retainBacking(CommandQueue& queue, resources::Pool& pool,
                   core::SurfaceHandle handle,
                   WMT::Reference<WMT::Texture>& texture,
                   core::SurfaceDesc& desc, Layout& layout) {
  // Pool lookup resolves the full generation-bearing handle. Retain this
  // exact backing before unlock; never look up a replacement after the drain.
  std::lock_guard lock(queue.mutex_);
  const auto* record = pool.findSurface(handle.value);
  if (!record || !record->texture || record->resolveTexture) return false;
  desc = record->desc;
  if (!layoutFor(desc, layout)) return false;
  id<MTLTexture> native = (__bridge id<MTLTexture>)(void*)record->texture.handle;
  if (native.pixelFormat != MTLPixelFormatDepth32Float_Stencil8 ||
      native.sampleCount != 1u || native.textureType != MTLTextureType2D ||
      native.width != desc.width || native.height != desc.height) return false;
  texture = record->texture;
  return true;
}

bool complete(WMT::Reference<WMT::CommandBuffer>& command) {
  command.commit();
  const auto started = std::chrono::steady_clock::now();
  command.waitUntilCompleted();
  perf::countSyncWait(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - started).count()));
  return command.status() == WMTCommandBufferStatusCompleted;
}

bool transfer(CommandQueue& queue, WMT::Reference<WMT::Texture> texture,
              WMT::Reference<WMT::Buffer> buffer, const core::SurfaceDesc& desc,
              const Layout& layout, bool capture) {
  auto command = queue.newCommandBuffer();
  if (!command) return false;
  auto blit = command.blitCommandEncoder();
  if (!blit) return false;
  id<MTLBlitCommandEncoder> native = (__bridge id<MTLBlitCommandEncoder>)(void*)blit.handle;
  id<MTLTexture> attachment = (__bridge id<MTLTexture>)(void*)texture.handle;
  id<MTLBuffer> bytes = (__bridge id<MTLBuffer>)(void*)buffer.handle;
  native.label = capture ? @"D24S8 exact dual-plane capture" : @"D24S8 exact dual-plane seed";
  const MTLOrigin origin{0u, 0u, 0u};
  const MTLSize extent{desc.width, desc.height, 1u};
  const auto stencilBytes = layout.bytes - layout.stencilOffset;
  if (capture) {
    [native copyFromTexture:attachment sourceSlice:0 sourceLevel:0 sourceOrigin:origin
        sourceSize:extent toBuffer:bytes destinationOffset:0
        destinationBytesPerRow:layout.depthPitch destinationBytesPerImage:layout.stencilOffset
        options:MTLBlitOptionDepthFromDepthStencil];
    [native copyFromTexture:attachment sourceSlice:0 sourceLevel:0 sourceOrigin:origin
        sourceSize:extent toBuffer:bytes destinationOffset:layout.stencilOffset
        destinationBytesPerRow:layout.stencilPitch destinationBytesPerImage:stencilBytes
        options:MTLBlitOptionStencilFromDepthStencil];
  } else {
    [native copyFromBuffer:bytes sourceOffset:0 sourceBytesPerRow:layout.depthPitch
        sourceBytesPerImage:layout.stencilOffset sourceSize:extent toTexture:attachment
        destinationSlice:0 destinationLevel:0 destinationOrigin:origin
        options:MTLBlitOptionDepthFromDepthStencil];
    [native copyFromBuffer:bytes sourceOffset:layout.stencilOffset sourceBytesPerRow:layout.stencilPitch
        sourceBytesPerImage:stencilBytes sourceSize:extent toTexture:attachment
        destinationSlice:0 destinationLevel:0 destinationOrigin:origin
        options:MTLBlitOptionStencilFromDepthStencil];
  }
  blit.endEncoding();
  // The retained texture/buffer survive both copies and GPU completion.
  return complete(command);
}

} // namespace

bool captureCanonicalD24S8DepthStencil(
    CommandQueue& queue, resources::Pool& pool,
    WMT::Reference<WMT::Device> device, core::SurfaceHandle source,
    core::CanonicalD24S8DepthStencil& checkpoint) {
  @autoreleasepool {
    checkpoint = {};
    WMT::Reference<WMT::Texture> texture;
    core::SurfaceDesc desc{};
    Layout layout{};
    if (!device || !retainBacking(queue, pool, source, texture, desc, layout)) return false;
    WMTBufferInfo info{};
    info.length = layout.bytes;
    info.options = WMTResourceStorageModeShared;
    auto buffer = device.newBuffer(info);
    if (!buffer || !info.memory.ptr) return false;
    auto lease = queue.beginRenderTapeCheckpoint();
    if (!lease) return false;
    if (!transfer(queue, texture, buffer, desc, layout, true)) return false;
    try {
      checkpoint.bytes.resize(layout.bytes);
    } catch (...) {
      return false;
    }
    std::memcpy(checkpoint.bytes.data(), info.memory.ptr, layout.bytes);
    checkpoint.version = core::kCanonicalD24S8DepthStencilVersion1;
    checkpoint.width = desc.width;
    checkpoint.height = desc.height;
    checkpoint.depthRowPitchBytes = layout.depthPitch;
    checkpoint.stencilRowPitchBytes = layout.stencilPitch;
    checkpoint.stencilOffsetBytes = layout.stencilOffset;
    checkpoint.physicalFormat = WMTPixelFormatDepth32Float_Stencil8;
    return true;
  }
}

bool seedCanonicalD24S8DepthStencil(
    CommandQueue& queue, resources::Pool& pool,
    WMT::Reference<WMT::Device> device, core::SurfaceHandle destination,
    const core::CanonicalD24S8DepthStencil& checkpoint) {
  @autoreleasepool {
    WMT::Reference<WMT::Texture> texture;
    core::SurfaceDesc desc{};
    Layout layout{};
    if (!device || !retainBacking(queue, pool, destination, texture, desc, layout) ||
        checkpoint.version != core::kCanonicalD24S8DepthStencilVersion1 ||
        checkpoint.width != desc.width || checkpoint.height != desc.height ||
        checkpoint.depthRowPitchBytes != layout.depthPitch ||
        checkpoint.stencilRowPitchBytes != layout.stencilPitch ||
        checkpoint.stencilOffsetBytes != layout.stencilOffset ||
        checkpoint.physicalFormat != WMTPixelFormatDepth32Float_Stencil8 ||
        checkpoint.bytes.size() != layout.bytes) return false;
    WMTBufferInfo info{};
    info.length = layout.bytes;
    info.options = WMTResourceStorageModeShared;
    auto buffer = device.newBuffer(info);
    if (!buffer || !info.memory.ptr) return false;
    std::memcpy(info.memory.ptr, checkpoint.bytes.data(), layout.bytes);
    auto lease = queue.beginRenderTapeCheckpoint();
    if (!lease) return false;
    return transfer(queue, texture, buffer, desc, layout, false);
  }
}

} // namespace dxmt9::encoders
