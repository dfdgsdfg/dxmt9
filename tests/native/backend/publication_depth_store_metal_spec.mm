// R-BACK-15.7/15.9: production pass actions must preserve depth/stencil
// across an arbitrary publication cut. Real Metal readback, not Tape exact
// mode or query preservation. This isolates attachment policy; it does not
// emulate resource retirement or the queue scheduler.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "../../../src/dxmt9/dxmt9_device.hpp"
#include "../../../src/dxmt9/dxmt9_render_pass_internal.hpp"
#include "../../../src/dxmt9/dxmt9_query_policy.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace dxmt9 {
struct CommandQueueArenaLeaseTestAccess {
  static encoders::EncodeContext context(CommandQueue& queue) {
    return queue.makeEncodeContext();
  }
};
}

namespace {
using namespace dxmt9;
constexpr unsigned extent = 16;
using Pixels = std::array<std::uint8_t, extent * extent * 4>;

void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

Pixels renderCase(bool split, bool stencil, bool referenceStore) {
  auto devices = WMT::CopyAllDevices();
  check(devices && devices.count(), "Metal device");
  auto device = devices.object(0u);
  auto upper = CreateDXMT9Device(DEVICE_DESC{.device = device});
  check(bool(upper), "provider device");
  auto ctx = CommandQueueArenaLeaseTestAccess::context(upper->queue());
  check(!query::metalOcclusionEnabled() &&
        !ctx.queue.renderTapeExactAttachmentPreservation(),
        "ordinary production actions, no query/Tape preservation");
  id<MTLDevice> metal = (__bridge id<MTLDevice>)(void*)device.handle;
  id<MTLCommandQueue> queue = [metal newCommandQueue];
  NSError* error = nil;
  id<MTLLibrary> library = [metal newLibraryWithSource:@R"msl(
    #include <metal_stdlib>
    using namespace metal;
    vertex float4 vs(uint i [[vertex_id]], constant float& z [[buffer(0)]]) {
      const float2 p[3] = {float2(-1,-1),float2(3,-1),float2(-1,3)};
      return float4(p[i], z, 1);
    }
    fragment float4 fs(constant float4& color [[buffer(0)]]) { return color; }
  )msl" options:nil error:&error];
  check(bool(library), "MSL compilation");
  MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
  descriptor.vertexFunction = [library newFunctionWithName:@"vs"];
  descriptor.fragmentFunction = [library newFunctionWithName:@"fs"];
  descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  // Pool D24S8 may use the native D24 or D32 implementation: derive the
  // concrete pipeline format from the actual provider-owned texture below.
  core::FlatDrawStateRecord hot{};
  hot.colorAttachments[0].handle = ctx.pool.createSurface(ctx.device, ctx.limits,
      {.width = extent, .height = extent, .format = core::Format::A8R8G8B8,
       .renderTarget = true});
  hot.depthStencil.handle = ctx.pool.createSurface(ctx.device, ctx.limits,
      {.width = extent, .height = extent, .format = core::Format::D24S8,
       .depthStencil = true});
  auto* color = ctx.pool.findSurface(hot.colorAttachments[0].handle.value);
  auto* depth = ctx.pool.findSurface(hot.depthStencil.handle.value);
  check(color && depth && color->texture && depth->texture, "attachments");
  id<MTLTexture> ds = (__bridge id<MTLTexture>)(void*)depth->texture.handle;
  descriptor.depthAttachmentPixelFormat = ds.pixelFormat;
  descriptor.stencilAttachmentPixelFormat = ds.pixelFormat;
  id<MTLRenderPipelineState> pipeline =
      [metal newRenderPipelineStateWithDescriptor:descriptor error:&error];
  check(bool(pipeline), "pipeline");

  core::ClearDesc clear{};
  clear.colorAttachments = hot.colorAttachments;
  clear.depthStencil = hot.depthStencil;
  clear.clearColor = clear.clearDepth = clear.clearStencil = true;
  clear.color = {0, 0, 0, 1};
  clear.depth = 1;
  clear.stencil = 0;
  core::ChunkSlot endedSource;
  const encoders::RenderPassStoreProofLookaheadSource suffix{
      .slot = &endedSource, .firstCommandIndex = 0, .commandEndIndex = 0};
  encoders::PreparedRenderPass prepared{};
  check(encoders::prepareRenderPassWithStoreProofLookahead(
      ctx, {.hot = &hot}, clear, std::span(&suffix, 1), {}, {}, {}, prepared),
      "production preparation");
  if (referenceStore) {
    prepared.info.depth.store_action = WMTStoreActionStore;
    prepared.info.stencil.store_action = WMTStoreActionStore;
  } else {
    check(prepared.info.depth.store_action == WMTStoreActionStore &&
          prepared.info.stencil.store_action == WMTStoreActionStore,
          "publication cut must preserve both aspects in production descriptor");
  }

  id<MTLCommandBuffer> cb = [queue commandBuffer];
  id<MTLCommandBuffer> first = cb;
  WMT::CommandBuffer wcb{reinterpret_cast<obj_handle_t>((__bridge void*)cb)};
  auto wenc = wcb.renderCommandEncoder(prepared.info);
  check(bool(wenc), "first render encoder");
  id<MTLRenderCommandEncoder> encoder =
      (__bridge id<MTLRenderCommandEncoder>)(void*)wenc.handle;
  const auto draw = [&](bool second) {
    MTLDepthStencilDescriptor* state = [MTLDepthStencilDescriptor new];
    state.depthCompareFunction = stencil ? MTLCompareFunctionAlways : MTLCompareFunctionLess;
    state.depthWriteEnabled = !stencil;
    if (stencil) {
      MTLStencilDescriptor* face = [MTLStencilDescriptor new];
      face.stencilCompareFunction = second ? MTLCompareFunctionEqual : MTLCompareFunctionAlways;
      face.depthStencilPassOperation = second ? MTLStencilOperationKeep : MTLStencilOperationReplace;
      face.readMask = face.writeMask = 0xff;
      state.frontFaceStencil = state.backFaceStencil = face;
    }
    [encoder setDepthStencilState:[metal newDepthStencilStateWithDescriptor:state]];
    [encoder setStencilReferenceValue:7];
    [encoder setRenderPipelineState:pipeline];
    [encoder setViewport:MTLViewport{0, 0, extent, extent, 0, 1}];
    [encoder setScissorRect:MTLScissorRect{0, 0, second ? extent : extent / 2, extent}];
    const float z = second ? 0.75f : 0.25f;
    const std::array<float, 4> rgba = second ? std::array<float, 4>{0, 1, 0, 1}
                                            : std::array<float, 4>{1, 0, 0, 1};
    [encoder setVertexBytes:&z length:sizeof(z) atIndex:0];
    [encoder setFragmentBytes:rgba.data() length:sizeof(rgba) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  };
  draw(false);
  if (split) {
    [encoder endEncoding];
    [cb commit];
    // Same queue provides GPU ordering, without inserting a CPU wait.
    cb = [queue commandBuffer];
    wcb = WMT::CommandBuffer{reinterpret_cast<obj_handle_t>((__bridge void*)cb)};
    ctx.queue.markColorHandleTouched(hot.colorAttachments[0].handle);
    check(encoders::prepareRenderPassWithStoreProofLookahead(
        ctx, {.hot = &hot}, std::nullopt, std::span(&suffix, 1), {}, {}, {}, prepared),
        "continuation preparation");
    wenc = wcb.renderCommandEncoder(prepared.info);
    check(bool(wenc), "continuation encoder");
    encoder = (__bridge id<MTLRenderCommandEncoder>)(void*)wenc.handle;
  }
  draw(true);
  [encoder endEncoding];
  id<MTLBuffer> output = [metal newBufferWithLength:extent * extent * 4
                                         options:MTLResourceStorageModeShared];
  id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
  id<MTLTexture> texture = (__bridge id<MTLTexture>)(void*)color->texture.handle;
  [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOrigin{0,0,0}
      sourceSize:MTLSize{extent,extent,1} toBuffer:output destinationOffset:0
      destinationBytesPerRow:extent * 4 destinationBytesPerImage:extent * extent * 4];
  [blit endEncoding];
  [cb commit];
  [cb waitUntilCompleted];
  check(cb.status == MTLCommandBufferStatusCompleted && !cb.error &&
        first.status == MTLCommandBufferStatusCompleted && !first.error,
        "all GPU submissions completed without errors");
  Pixels pixels{};
  std::memcpy(pixels.data(), output.contents, pixels.size());
  return pixels;
}
}

int main() {
  @autoreleasepool {
    try {
      for (bool stencil : {false, true}) {
        const auto serial = renderCase(false, stencil, true);
        const auto split = renderCase(true, stencil, false);
        check(serial == split, "split/serial exact BGRA8 mismatch");
        for (unsigned y = 0; y < extent; ++y) {
          for (unsigned x = 0; x < extent; ++x) {
            const bool left = x < extent / 2;
            const bool red = !stencil && left;
            const bool green = stencil ? left : !left;
            const auto i = (y * extent + x) * 4;
            check(split[i] == 0 && split[i+1] == (green ? 255 : 0) &&
                  split[i+2] == (red ? 255 : 0) && split[i+3] == 255,
                  "independent expected depth/stencil coverage");
          }
        }
      }
      std::cout << "production publication depth/stencil exact readback passed\n";
    } catch (const std::exception& e) {
      std::cerr << e.what() << '\n';
      return 1;
    }
  }
}
