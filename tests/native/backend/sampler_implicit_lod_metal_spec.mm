// Concrete Metal oracle for implicit texture LOD selection. Each mip carries a
// distinct red value and a full-screen draw supplies analytically controlled
// screen-space derivatives. This fixture deliberately proves Metal's native
// convention only; D3D9-to-Metal policy remains covered by the pure sampler
// state and shader translation specs.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

constexpr NSUInteger kExtent = 64;
constexpr NSUInteger kMipCount = 6;

struct alignas(16) SampleParams {
  float inverseTargetExtent[2]{};
  float coordinateScale{};
  float lodBias{};
};

void fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
  std::exit(1);
}

id<MTLLibrary> makeLibrary(id<MTLDevice> device) {
  static NSString* source = @R"msl(
#include <metal_stdlib>
using namespace metal;

struct SampleParams {
  float2 inverseTargetExtent;
  float coordinateScale;
  float lodBias;
};

struct VertexOut {
  float4 position [[position]];
};

struct PerspectiveVertexOut {
  float4 position [[position]];
  float2 uv;
};

vertex VertexOut implicit_lod_vs(uint vertexId [[vertex_id]]) {
  const float2 positions[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0),
  };
  VertexOut out;
  out.position = float4(positions[vertexId], 0.0, 1.0);
  return out;
}

vertex PerspectiveVertexOut perspective_lod_vs(uint vertexId [[vertex_id]]) {
  const float2 ndc[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0),
  };
  const float clipW[3] = {1.0, 8.0, 8.0};
  const float2 uv[3] = {
    float2( 0.0,  0.0),
    float2(28.0,  0.0),
    float2( 0.0, 28.0),
  };
  PerspectiveVertexOut out;
  out.position = float4(ndc[vertexId] * clipW[vertexId], 0.0,
                        clipW[vertexId]);
  out.uv = uv[vertexId];
  return out;
}

fragment float4 implicit_lod_plain_fs(
    VertexOut in [[stage_in]],
    texture2d<float> source [[texture(0)]],
    sampler sourceSampler [[sampler(0)]],
    constant SampleParams& params [[buffer(0)]]) {
  const float2 uv = in.position.xy * params.inverseTargetExtent *
                    params.coordinateScale;
  return source.sample(sourceSampler, uv);
}

fragment float4 implicit_lod_bias_fs(
    VertexOut in [[stage_in]],
    texture2d<float> source [[texture(0)]],
    sampler sourceSampler [[sampler(0)]],
    constant SampleParams& params [[buffer(0)]]) {
  const float2 uv = in.position.xy * params.inverseTargetExtent *
                    params.coordinateScale;
  return source.sample(sourceSampler, uv, bias(params.lodBias));
}

fragment float4 implicit_lod_level_fs(
    VertexOut in [[stage_in]],
    texture2d<float> source [[texture(0)]],
    sampler sourceSampler [[sampler(0)]],
    constant SampleParams& params [[buffer(0)]]) {
  const float2 uv = in.position.xy * params.inverseTargetExtent *
                    params.coordinateScale;
  return source.sample(sourceSampler, uv, level(params.lodBias));
}

fragment float4 perspective_lod_fs(
    PerspectiveVertexOut in [[stage_in]],
    texture2d<float> source [[texture(0)]],
    sampler sourceSampler [[sampler(0)]]) {
  return source.sample(sourceSampler, in.uv);
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
                                        id<MTLLibrary> library,
                                        NSString* fragmentName,
                                        NSString* vertexName = @"implicit_lod_vs") {
  MTLRenderPipelineDescriptor* descriptor =
      [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.vertexFunction = [library newFunctionWithName:vertexName];
  descriptor.fragmentFunction = [library newFunctionWithName:fragmentName];
  descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

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

id<MTLTexture> makeMipTexture(id<MTLDevice> device,
                              MTLPixelFormat pixelFormat) {
  MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:pixelFormat
                                   width:kExtent
                                  height:kExtent
                               mipmapped:YES];
  descriptor.storageMode = MTLStorageModeShared;
  descriptor.usage = MTLTextureUsageShaderRead;
  id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
  if (!texture || texture.mipmapLevelCount < kMipCount) {
    return nil;
  }

  constexpr std::array<std::uint8_t, kMipCount> kUnormRed{
      32u, 64u, 96u, 128u, 160u, 192u};
  constexpr std::array<std::int8_t, kMipCount> kSnormRed{
      16, 32, 48, 64, 80, 96};
  for (NSUInteger level = 0; level < kMipCount; ++level) {
    const NSUInteger extent = std::max<NSUInteger>(1u, kExtent >> level);
    std::array<std::uint8_t, kExtent * kExtent * 4u> bytes{};
    for (NSUInteger pixel = 0; pixel < extent * extent; ++pixel) {
      if (pixelFormat == MTLPixelFormatRGBA8Snorm) {
        bytes[pixel * 4u] = static_cast<std::uint8_t>(kSnormRed[level]);
        bytes[pixel * 4u + 3u] = static_cast<std::uint8_t>(127);
      } else {
        bytes[pixel * 4u] = kUnormRed[level];
        bytes[pixel * 4u + 3u] = 255u;
      }
    }
    [texture replaceRegion:MTLRegionMake2D(0, 0, extent, extent)
               mipmapLevel:level
                 withBytes:bytes.data()
               bytesPerRow:extent * 4u];
  }
  return texture;
}

id<MTLTexture> makeTarget(id<MTLDevice> device) {
  MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                   width:kExtent
                                  height:kExtent
                               mipmapped:NO];
  descriptor.storageMode = MTLStorageModeShared;
  descriptor.usage = MTLTextureUsageRenderTarget;
  return [device newTextureWithDescriptor:descriptor];
}

id<MTLSamplerState> makeSampler(id<MTLDevice> device) {
  MTLSamplerDescriptor* descriptor = [[MTLSamplerDescriptor alloc] init];
  descriptor.minFilter = MTLSamplerMinMagFilterNearest;
  descriptor.magFilter = MTLSamplerMinMagFilterNearest;
  descriptor.mipFilter = MTLSamplerMipFilterNearest;
  descriptor.sAddressMode = MTLSamplerAddressModeRepeat;
  descriptor.tAddressMode = MTLSamplerAddressModeRepeat;
  descriptor.lodMinClamp = 0.0f;
  descriptor.lodMaxClamp = static_cast<float>(kMipCount - 1u);
  return [device newSamplerStateWithDescriptor:descriptor];
}

std::uint8_t renderAndReadRed(id<MTLCommandQueue> queue,
                              id<MTLRenderPipelineState> pipeline,
                              id<MTLTexture> source,
                              id<MTLSamplerState> sampler,
                              id<MTLTexture> target,
                              float coordinateScale,
                              float lodBias) {
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = target;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

  SampleParams params{
      .inverseTargetExtent = {1.0f / static_cast<float>(kExtent),
                              1.0f / static_cast<float>(kExtent)},
      .coordinateScale = coordinateScale,
      .lodBias = lodBias,
  };
  id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
  id<MTLRenderCommandEncoder> encoder =
      [commandBuffer renderCommandEncoderWithDescriptor:pass];
  [encoder setRenderPipelineState:pipeline];
  [encoder setFragmentTexture:source atIndex:0];
  [encoder setFragmentSamplerState:sampler atIndex:0];
  [encoder setFragmentBytes:&params length:sizeof(params) atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle
              vertexStart:0
              vertexCount:3];
  [encoder endEncoding];
  [commandBuffer commit];
  [commandBuffer waitUntilCompleted];
  if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
    std::cerr << "Metal command buffer failed: "
              << (commandBuffer.error
                      ? commandBuffer.error.localizedDescription.UTF8String
                      : "unknown")
              << '\n';
    std::exit(1);
  }

  std::array<std::uint8_t, 4> pixel{};
  [target getBytes:pixel.data()
          bytesPerRow:4u
           fromRegion:MTLRegionMake2D(kExtent / 2u, kExtent / 2u, 1u, 1u)
          mipmapLevel:0];
  return pixel[2];
}

void expectNear(std::uint8_t actual,
                std::uint8_t expected,
                std::string_view lane) {
  if (std::abs(static_cast<int>(actual) - static_cast<int>(expected)) <= 2) {
    return;
  }
  std::cerr << "FAIL: " << lane << " expected red "
            << static_cast<unsigned>(expected) << " but got "
            << static_cast<unsigned>(actual) << '\n';
  std::exit(1);
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
    id<MTLRenderPipelineState> plainPipeline =
        library ? makePipeline(device, library, @"implicit_lod_plain_fs") : nil;
    id<MTLRenderPipelineState> biasPipeline =
        library ? makePipeline(device, library, @"implicit_lod_bias_fs") : nil;
    id<MTLRenderPipelineState> levelPipeline =
        library ? makePipeline(device, library, @"implicit_lod_level_fs") : nil;
    id<MTLRenderPipelineState> perspectivePipeline =
        library ? makePipeline(device, library, @"perspective_lod_fs",
                               @"perspective_lod_vs") : nil;
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLSamplerState> sampler = makeSampler(device);
    id<MTLTexture> target = makeTarget(device);
    if (!plainPipeline || !biasPipeline || !levelPipeline ||
        !perspectivePipeline || !queue ||
        !sampler || !target) {
      fail("failed to create the Metal implicit-LOD fixture");
    }

    // Keep the analytic lambda away from an integer rounding boundary. The
    // 1.1 multiplier adds only ~0.14 LOD, so nearest-mip selection remains
    // level N while minor derivative rounding cannot move it to N-1.
    constexpr std::array<float, 4> kScales{1.1f, 2.2f, 4.4f, 8.8f};
    constexpr std::array<std::uint8_t, 4> kUnormExpected{
        32u, 64u, 96u, 128u};
    constexpr std::array<std::uint8_t, 4> kSnormExpected{
        32u, 64u, 96u, 129u};
    for (const auto format :
         {MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA8Snorm}) {
      id<MTLTexture> source = makeMipTexture(device, format);
      if (!source) {
        fail("failed to create the mipmapped source texture");
      }
      const auto& expected = format == MTLPixelFormatRGBA8Snorm
                                 ? kSnormExpected
                                 : kUnormExpected;
      for (std::size_t level = 0; level < kScales.size(); ++level) {
        const auto explicitLevel = renderAndReadRed(
            queue, levelPipeline, source, sampler, target, 1.0f,
            static_cast<float>(level));
        expectNear(explicitLevel, expected[level], "explicit mip level");
        const auto plain = renderAndReadRed(queue, plainPipeline, source,
                                            sampler, target, kScales[level], 0.0f);
        expectNear(plain, expected[level], "plain implicit LOD");
        const auto zeroBias = renderAndReadRed(queue, biasPipeline, source,
                                               sampler, target, kScales[level], 0.0f);
        expectNear(zeroBias, expected[level], "explicit zero bias");
      }

      const auto minusOne = renderAndReadRed(queue, biasPipeline, source,
                                             sampler, target, 4.4f, -1.0f);
      expectNear(minusOne, expected[1], "negative-one bias");

      // The clip-space triangle covers the same screen-space area as the
      // affine control but uses W=(1,8,8). Its perspective-correct center
      // derivatives select mip 2; affine interpolation of the same UV span
      // would select mip 4. This pins the production-relevant non-unit-W
      // stage-in derivative path independently from explicit level upload.
      const auto perspective = renderAndReadRed(
          queue, perspectivePipeline, source, sampler, target, 1.0f, 0.0f);
      expectNear(perspective, expected[2], "perspective implicit LOD");
    }
  }
  return 0;
}
