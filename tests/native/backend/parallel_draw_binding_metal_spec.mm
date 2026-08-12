// R-BACK-2.63 / R-VERIF-2.15 concrete Metal oracle for child-local uniform
// generations. The serial lane binds every A -> B -> A payload. The parallel
// lane uses the same pure transition as production and must produce identical
// BGRA8 bytes through two MTLParallelRenderCommandEncoder children. Additive
// blending makes the intermediate B draw observable, so a stale A binding
// cannot accidentally match merely because the sequence ends at A.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <dispatch/dispatch.h>

#include "../../../src/dxmt9/dxmt9_uniform_dirty.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace {

using dxmt9::uniform::DirectCbufPayloadCounts;
using dxmt9::uniform::DirtyState;
using dxmt9::uniform::DrawBindingAbi;
using dxmt9::uniform::DrawBindingPath;
using dxmt9::uniform::DrawBindingPayloadIdentity;

constexpr NSUInteger kWidth = 64;
constexpr NSUInteger kHeight = 32;

struct alignas(16) VertexUniform {
  float center[2]{};
  float halfExtent[2]{};
};

struct alignas(16) FragmentUniform {
  float color[4]{};
};

struct DrawPayload {
  DrawBindingPayloadIdentity identity{};
  VertexUniform vertex{};
  FragmentUniform fragment{};
};

struct ChildPayloads {
  std::array<DrawPayload, 3> draws{};
};

void fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
  std::exit(1);
}

id<MTLLibrary> makeLibrary(id<MTLDevice> device) {
  static NSString* source = @R"msl(
#include <metal_stdlib>
using namespace metal;

struct VertexUniform {
  float2 center;
  float2 halfExtent;
};

struct FragmentUniform {
  float4 color;
};

struct VertexOut {
  float4 position [[position]];
};

vertex VertexOut binding_vs(
    uint vertexId [[vertex_id]],
    constant VertexUniform& uniform [[buffer(0)]]) {
  const float2 corners[6] = {
    float2(-1.0, -1.0), float2( 1.0, -1.0), float2(-1.0,  1.0),
    float2(-1.0,  1.0), float2( 1.0, -1.0), float2( 1.0,  1.0),
  };
  VertexOut out;
  out.position = float4(
      uniform.center + corners[vertexId] * uniform.halfExtent,
      0.0, 1.0);
  return out;
}

fragment float4 binding_fs(
    constant FragmentUniform& uniform [[buffer(0)]]) {
  return uniform.color;
}
)msl";

  NSError* error = nil;
  id<MTLLibrary> library = [device newLibraryWithSource:source
                                                options:nil
                                                  error:&error];
  if (!library) {
    std::cerr << "Metal library compile failed: "
              << (error ? error.localizedDescription.UTF8String : "unknown")
              << '\n';
  }
  return library;
}

id<MTLRenderPipelineState> makePipeline(id<MTLDevice> device,
                                        id<MTLLibrary> library) {
  MTLRenderPipelineDescriptor* descriptor =
      [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.vertexFunction = [library newFunctionWithName:@"binding_vs"];
  descriptor.fragmentFunction = [library newFunctionWithName:@"binding_fs"];
  descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  descriptor.colorAttachments[0].blendingEnabled = YES;
  descriptor.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
  descriptor.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
  descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
  descriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
  descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
  descriptor.colorAttachments[0].destinationAlphaBlendFactor =
      MTLBlendFactorOne;

  NSError* error = nil;
  id<MTLRenderPipelineState> pipeline =
      [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
  if (!pipeline) {
    std::cerr << "Metal pipeline compile failed: "
              << (error ? error.localizedDescription.UTF8String : "unknown")
              << '\n';
  }
  return pipeline;
}

id<MTLTexture> makeTarget(id<MTLDevice> device) {
  MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                   width:kWidth
                                  height:kHeight
                               mipmapped:NO];
  descriptor.storageMode = MTLStorageModeShared;
  descriptor.usage = MTLTextureUsageRenderTarget;
  return [device newTextureWithDescriptor:descriptor];
}

MTLRenderPassDescriptor* makePass(id<MTLTexture> target) {
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = target;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
  return pass;
}

ChildPayloads makeChild(float centerX, std::uint64_t childSalt) {
  const auto make = [&](std::uint64_t generation,
                        std::array<float, 4> color) {
    return DrawPayload{
        .identity = DrawBindingPayloadIdentity{
            .vertexConstants = childSalt ^ (generation * 0x101u),
            .pixelConstants = childSalt ^ (generation * 0x10001u),
            .fixedFunction = childSalt ^ (generation * 0x1000001u),
        },
        .vertex = VertexUniform{
            .center = {centerX, 0.0f},
            .halfExtent = {0.5f, 1.0f},
        },
        .fragment = FragmentUniform{
            .color = {color[0], color[1], color[2], color[3]},
        },
    };
  };
  const auto a = make(1u, {0.20f, 0.00f, 0.00f, 0.20f});
  const auto b = make(2u, {0.00f, 0.20f, 0.00f, 0.20f});
  return ChildPayloads{.draws = {a, b, a}};
}

void encodeSerialChild(id<MTLRenderCommandEncoder> encoder,
                       id<MTLRenderPipelineState> pipeline,
                       const ChildPayloads& child) {
  [encoder setRenderPipelineState:pipeline];
  for (const auto& draw : child.draws) {
    [encoder setVertexBytes:&draw.vertex length:sizeof(draw.vertex) atIndex:0];
    [encoder setFragmentBytes:&draw.fragment
                       length:sizeof(draw.fragment)
                      atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:6];
  }
}

void encodeTransitionChild(id<MTLRenderCommandEncoder> encoder,
                           id<MTLRenderPipelineState> pipeline,
                           const ChildPayloads& child,
                           bool omitPayloadDirtyTransition) {
  [encoder setRenderPipelineState:pipeline];
  DirtyState dirty{};
  dxmt9::uniform::markAllDirty(dirty);
  std::optional<DrawBindingPayloadIdentity> previous;
  const DirectCbufPayloadCounts counts{
      .vertexFloat = 1,
      .pixelFloat = 1,
  };

  for (const auto& draw : child.draws) {
    const auto transition = dxmt9::uniform::planDrawBindingTransition(
        previous.has_value(), previous.value_or(DrawBindingPayloadIdentity{}),
        draw.identity, DrawBindingAbi::Stage1Direct,
        DrawBindingPath::Direct);
    if (!omitPayloadDirtyTransition || !previous.has_value()) {
      if (!dxmt9::uniform::applyDrawBindingTransition(
              dirty, transition, counts)) {
        fail("direct child rejected a Stage 1 binding ABI");
      }
    }
    if (dxmt9::uniform::anyDirty(
            dirty, dxmt9::uniform::kVsAny |
                dxmt9::uniform::kFfpVsAny)) {
      [encoder setVertexBytes:&draw.vertex
                       length:sizeof(draw.vertex)
                      atIndex:0];
      dxmt9::uniform::clearBits(
          dirty, dxmt9::uniform::kVsAny |
              dxmt9::uniform::kFfpVsAny);
    }
    if (dxmt9::uniform::anyDirty(
            dirty, dxmt9::uniform::kPsAny |
                dxmt9::uniform::kFfpPsAny)) {
      [encoder setFragmentBytes:&draw.fragment
                         length:sizeof(draw.fragment)
                        atIndex:0];
      dxmt9::uniform::clearBits(
          dirty, dxmt9::uniform::kPsAny |
              dxmt9::uniform::kFfpPsAny);
    }
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:6];
    previous = transition.next;
  }
}

bool finish(id<MTLCommandBuffer> commandBuffer, std::string_view lane) {
  [commandBuffer commit];
  [commandBuffer waitUntilCompleted];
  if (commandBuffer.status == MTLCommandBufferStatusCompleted) {
    return true;
  }
  std::cerr << lane << " command buffer failed: "
            << (commandBuffer.error
                    ? commandBuffer.error.localizedDescription.UTF8String
                    : "unknown")
            << '\n';
  return false;
}

bool renderSerial(id<MTLCommandQueue> queue,
                  id<MTLRenderPipelineState> pipeline,
                  id<MTLTexture> target,
                  const std::array<ChildPayloads, 2>& children) {
  id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
  id<MTLRenderCommandEncoder> encoder =
      [commandBuffer renderCommandEncoderWithDescriptor:makePass(target)];
  for (const auto& child : children) {
    encodeSerialChild(encoder, pipeline, child);
  }
  [encoder endEncoding];
  return finish(commandBuffer, "serial");
}

bool renderParallel(id<MTLCommandQueue> queue,
                    id<MTLRenderPipelineState> pipeline,
                    id<MTLTexture> target,
                    const std::array<ChildPayloads, 2>& children,
                    bool omitPayloadDirtyTransition) {
  id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
  id<MTLParallelRenderCommandEncoder> parent =
      [commandBuffer parallelRenderCommandEncoderWithDescriptor:makePass(target)];
  std::array<id<MTLRenderCommandEncoder>, 2> childEncoders{
      [parent renderCommandEncoder],
      [parent renderCommandEncoder],
  };
  dispatch_apply(childEncoders.size(),
                 dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                 ^(std::size_t i) {
    encodeTransitionChild(childEncoders[i], pipeline, children[i],
                          omitPayloadDirtyTransition);
    [childEncoders[i] endEncoding];
  });
  [parent endEncoding];
  return finish(commandBuffer, "parallel");
}

std::vector<std::uint8_t> readback(id<MTLTexture> texture) {
  std::vector<std::uint8_t> bytes(kWidth * kHeight * 4u);
  [texture getBytes:bytes.data()
         bytesPerRow:kWidth * 4u
          fromRegion:MTLRegionMake2D(0, 0, kWidth, kHeight)
         mipmapLevel:0];
  return bytes;
}

std::uint64_t hash(std::span<const std::uint8_t> bytes) {
  std::uint64_t value = 1469598103934665603ull;
  for (const auto byte : bytes) {
    value ^= byte;
    value *= 1099511628211ull;
  }
  return value;
}

}  // namespace

int main() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      std::cerr << "SKIP: no Metal device\n";
      return 77;
    }
    id<MTLLibrary> library = makeLibrary(device);
    id<MTLRenderPipelineState> pipeline =
        library ? makePipeline(device, library) : nil;
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!library || !pipeline || !queue) {
      return 1;
    }

    const std::array<ChildPayloads, 2> children{
        makeChild(-0.5f, 0x10000000u),
        makeChild(0.5f, 0x20000000u),
    };
    id<MTLTexture> serialTarget = makeTarget(device);
    id<MTLTexture> parallelTarget = makeTarget(device);
    id<MTLTexture> staleTarget = makeTarget(device);
    if (!serialTarget || !parallelTarget || !staleTarget) {
      fail("failed to allocate shared offscreen targets");
    }

    if (!renderSerial(queue, pipeline, serialTarget, children) ||
        !renderParallel(queue, pipeline, staleTarget, children,
                        /*omitPayloadDirtyTransition=*/true)) {
      return 1;
    }
    const auto serialBytes = readback(serialTarget);
    const auto staleBytes = readback(staleTarget);
    if (serialBytes == staleBytes) {
      fail("pixel oracle does not distinguish an omitted A-B-A dirty edge");
    }

    constexpr std::uint32_t kRepeatCount = 100u;
    for (std::uint32_t repeat = 0; repeat < kRepeatCount; ++repeat) {
      if (!renderParallel(queue, pipeline, parallelTarget, children,
                          /*omitPayloadDirtyTransition=*/false)) {
        return 1;
      }
      const auto parallelBytes = readback(parallelTarget);
      if (parallelBytes != serialBytes) {
        std::cerr << "FAIL: serial/parallel pixel mismatch at repeat "
                  << repeat << " serial=0x" << std::hex << hash(serialBytes)
                  << " parallel=0x" << hash(parallelBytes) << std::dec
                  << '\n';
        return 1;
      }
    }
  }
  return 0;
}
