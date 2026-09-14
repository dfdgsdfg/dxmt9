#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "../../../src/winemetal/winemetal.h"
#include "../../../src/winemetal/unix/winemetal_sampling_lineage.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(value) do { if (!(value)) { \
  std::fprintf(stderr, "direct buffer check failed at %d: %s\n", __LINE__, #value); \
  std::abort(); } } while (false)

template <typename T>
void issue(id<MTLRenderCommandEncoder> encoder, const T& command) {
  MTLRenderCommandEncoder_encodeCommands((obj_handle_t)encoder,
      reinterpret_cast<const wmtcmd_base*>(&command));
}

int main() {
  @autoreleasepool {
    using namespace dxmt9::diagnostics;
    const bool observed = metalSamplingLineageEnabled();
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    CHECK(device);
    std::array<float, 4> red{1, 0, 0, 1}, green{0, 1, 0, 1};
    id<MTLBuffer> a = [device newBufferWithLength:512 options:MTLResourceStorageModeShared];
    id<MTLBuffer> b = [device newBufferWithLength:512 options:MTLResourceStorageModeShared];
    CHECK(a && b);
    std::memcpy(a.contents, red.data(), sizeof(red));
    std::memcpy(static_cast<char*>(a.contents) + 256, green.data(), sizeof(green));
    std::memcpy(b.contents, red.data(), sizeof(red));
    std::memcpy(static_cast<char*>(b.contents) + 256, green.data(), sizeof(green));
    auto* textureDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
        width:4 height:4 mipmapped:NO];
    textureDesc.storageMode = MTLStorageModeShared;
    textureDesc.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [device newTextureWithDescriptor:textureDesc];
    NSString* source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 vs(uint i [[vertex_id]]) {float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[i],0,1);}\n"
        "fragment float4 fs(constant float4& color [[buffer(0)]]) {return color;}\n";
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    CHECK(library && !error);
    id<MTLFunction> vs = [library newFunctionWithName:@"vs"];
    id<MTLFunction> fs = [library newFunctionWithName:@"fs"];
    auto* psoDesc = [[MTLRenderPipelineDescriptor alloc] init];
    psoDesc.vertexFunction = vs;
    psoDesc.fragmentFunction = fs;
    psoDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:psoDesc error:&error];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    CHECK(target && pso && !error && queue);
    for (unsigned mode = 0; mode < 6; ++mode) {
      auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = target;
      pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      id<MTLCommandBuffer> cb = [queue commandBuffer];
      id<MTLRenderCommandEncoder> encoder = [cb renderCommandEncoderWithDescriptor:pass];
      const auto check = [&](bool vertex, unsigned index, id<MTLBuffer> expected,
                              unsigned offset, const char* status) {
        CHECK(std::strcmp(metalDirectBufferMatch((obj_handle_t)encoder, vertex, index,
                    (obj_handle_t)expected, offset), observed ? status : "unknown") == 0);
      };
      const auto bind = [&](bool vertex, unsigned index, id<MTLBuffer> value, unsigned offset) {
        wmtcmd_render_setbuffer command{};
        command.type = vertex ? WMTRenderCommandSetVertexBuffer : WMTRenderCommandSetFragmentBuffer;
        command.buffer = (obj_handle_t)value;
        command.offset = offset;
        command.index = index;
        issue(encoder, command);
      };
      const auto offset = [&](bool vertex, unsigned index, unsigned value) {
        wmtcmd_render_setbufferoffset command{};
        command.type = vertex ? WMTRenderCommandSetVertexBufferOffset : WMTRenderCommandSetFragmentBufferOffset;
        command.index = index;
        command.offset = value;
        issue(encoder, command);
      };
      check(false, 0, a, 0, "unobserved");
      // Missing offset origin is a synthetic observer negative, not an illegal
      // offset-only Metal call before any buffer has been bound.
      noteMetalDirectBuffer((obj_handle_t)encoder, false, 0, 0, 256, true);
      check(false, 0, a, 256, "unknown");
      bind(false, 0, nil, 0);
      check(false, 0, a, 0, "explicit-null");
      for (bool vertex : {true, false}) {
        for (unsigned index : {0u, 3u}) {
          bind(vertex, index, a, 0);
          check(vertex, index, a, 0, "match");
          check(vertex, index, b, 0, "mismatch");  // Equal bytes are not object identity.
          offset(vertex, index, 256);
          check(vertex, index, a, 0, "mismatch");
          check(vertex, index, a, 256, "match");
          bind(vertex, index, b, 0);
          check(vertex, index, b, 0, "match");
          bind(vertex, index, a, 0);
          check(vertex, index, a, 0, "match");
          wmtcmd_render_setbytes bytes{};
          bytes.type = vertex ? WMTRenderCommandSetVertexBytes : WMTRenderCommandSetFragmentBytes;
          bytes.bytes.ptr = green.data();
          bytes.length = sizeof(green);
          bytes.index = index;
          issue(encoder, bytes);
          check(vertex, index, a, 0, "inline-bytes");
          bind(vertex, index, a, 0);
        }
      }
      bool greenPixel = false;
      if (mode == 1) {
        // Model a high-level cache claiming B/256 without issuing its bind.
        check(false, 0, b, 256, "mismatch");  // GPU must still draw stale red.
      } else if (mode == 2) {
        offset(false, 0, 256);
        check(false, 0, a, 256, "match");
        greenPixel = true;
      } else if (mode == 3) {
        bind(false, 0, b, 256);
        check(false, 0, b, 256, "match");
        greenPixel = true;
      } else if (mode == 4) {
        bind(false, 0, b, 256);
        bind(false, 0, a, 0);
        check(false, 0, a, 0, "match");
      } else if (mode == 5) {
        wmtcmd_render_setbytes bytes{};
        bytes.type = WMTRenderCommandSetFragmentBytes;
        bytes.bytes.ptr = green.data();
        bytes.length = sizeof(green);
        issue(encoder, bytes);
        check(false, 0, a, 0, "inline-bytes");
        greenPixel = true;
      }
      [encoder setRenderPipelineState:pso];
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
      [encoder endEncoding];
      [cb commit];
      [cb waitUntilCompleted];
      CHECK(cb.status == MTLCommandBufferStatusCompleted && !cb.error);
      std::array<unsigned char, 64> pixels{};
      [target getBytes:pixels.data() bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
      for (unsigned i = 0; i < 16; ++i) {
        CHECK(pixels[4*i] == (greenPixel ? 0 : 255));
        CHECK(pixels[4*i+1] == (greenPixel ? 255 : 0));
        CHECK(pixels[4*i+2] == 0 && pixels[4*i+3] == 255);
      }
    }
    [queue release]; [pso release]; [psoDesc release]; [vs release]; [fs release];
    [library release]; [target release]; [a release]; [b release]; [device release];
    std::puts("actual direct-buffer calls, stale rebind negative and six exact pixel cuts passed");
  }
}
