// Standalone diagnostic: adding a preserving render-pass/command-buffer
// boundary may expose native attachment/blending precision. Split equality is
// an observation, not an assertion. This does not exercise dxmt9 publication,
// resource recycling, mutable uploads, depth/stencil, MSAA, or shader replay.
// Build with Objective-C ARC and Foundation + Metal; no dxmt9 runtime link.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr NSUInteger kExtent = 64u;
constexpr std::size_t kPixels = kExtent * kExtent;
constexpr std::uint32_t kDrawCount = 12u;
constexpr std::uint32_t kSplitAfter = 3u;
constexpr NSUInteger kParameterStride = 256u;
static_assert(std::endian::native == std::endian::little);

enum class Boundary { None, SameCommandBuffer, SeparateCommandBuffer };

struct alignas(16) DrawParameters {
  std::uint32_t ordinal = 0u;
  std::uint32_t workload = 0u;
  std::uint32_t reserved[2]{};
};
static_assert(sizeof(DrawParameters) == 16u);

struct RunResult {
  std::vector<std::uint8_t> source;
  std::vector<std::uint8_t> output;
  NSMutableArray* errors = [NSMutableArray array];
  std::uint32_t commandBuffers = 0u;
  std::uint32_t gpuErrors = 0u;
  bool complete = false;
};

void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

std::string metalError(NSError* error) {
  return error ? error.localizedDescription.UTF8String : "unknown Metal failure";
}

id<MTLLibrary> makeLibrary(id<MTLDevice> device) {
  static NSString* source = @R"msl(
#include <metal_stdlib>
using namespace metal;
struct V { float4 position [[position, invariant]]; };
struct DrawParameters { uint ordinal; uint workload; uint2 reserved; };
vertex V fullscreen(uint i [[vertex_id]]) {
  const float2 positions[3] = {
    float2(-1, -1), float2(3, -1), float2(-1, 3)
  };
  return V{float4(positions[i], 0, 1)};
}
fragment float4 blend_pattern(V in [[stage_in]],
    constant DrawParameters& p [[buffer(0)]]) {
  const uint2 pixel = uint2(in.position.xy);
  const uint token = (pixel.x * 17u + pixel.y * 31u + p.ordinal * 13u) % 257u;
  const float t = float(token) / 256.0f;
  if (p.workload == 0u) {
    return float4(float3(.0123f, .0091f, .0047f) +
                  t * float3(.0207f, .0149f, .0193f), 1.0f);
  }
  return float4(float3(.1234f, .4567f, .6789f) +
                t * float3(.51f, .17f, .09f), .11f + .17f * t);
}
fragment float4 convert_rgba8(V in [[stage_in]],
    texture2d<float> source [[texture(0)]], sampler nearest [[sampler(0)]]) {
  return source.sample(nearest, in.position.xy / 64.0f, level(0.0f));
}
)msl";
  MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
  options.preserveInvariance = YES;
  if (@available(macOS 15.0, *)) {
    options.mathMode = MTLMathModeSafe;
  } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    options.fastMathEnabled = NO;
#pragma clang diagnostic pop
  }
  NSError* error = nil;
  id<MTLLibrary> result = [device newLibraryWithSource:source
                                             options:options error:&error];
  if (!result) throw std::runtime_error("MSL compile: " + metalError(error));
  return result;
}

id<MTLRenderPipelineState> makePipeline(id<MTLDevice> device,
    id<MTLLibrary> library, MTLPixelFormat format, bool blending,
    std::uint32_t workload) {
  MTLRenderPipelineDescriptor* descriptor =
      [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.vertexFunction = [library newFunctionWithName:@"fullscreen"];
  descriptor.fragmentFunction = [library newFunctionWithName:
      (blending ? @"blend_pattern" : @"convert_rgba8")];
  auto* color = descriptor.colorAttachments[0];
  color.pixelFormat = format;
  color.blendingEnabled = blending;
  if (blending) {
    color.writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen |
                      MTLColorWriteMaskBlue;
    color.rgbBlendOperation = MTLBlendOperationAdd;
    color.sourceRGBBlendFactor = workload == 0u
        ? MTLBlendFactorOne : MTLBlendFactorSourceAlpha;
    color.destinationRGBBlendFactor = workload == 0u
        ? MTLBlendFactorOne : MTLBlendFactorOneMinusSourceAlpha;
  }
  NSError* error = nil;
  id<MTLRenderPipelineState> result =
      [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
  if (!result) throw std::runtime_error("pipeline compile: " + metalError(error));
  return result;
}

id<MTLTexture> makeTexture(id<MTLDevice> device, MTLPixelFormat format) {
  auto* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
      width:kExtent height:kExtent mipmapped:NO];
  descriptor.storageMode = MTLStorageModePrivate;
  descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  id<MTLTexture> result = [device newTextureWithDescriptor:descriptor];
  check(result != nil, "texture creates");
  return result;
}

id<MTLRenderCommandEncoder> beginPass(id<MTLCommandBuffer> command,
    id<MTLTexture> target, id<MTLRenderPipelineState> pipeline, bool clear) {
  auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = target;
  pass.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(.041, .073, .109, 1.0);
  id<MTLRenderCommandEncoder> encoder =
      [command renderCommandEncoderWithDescriptor:pass];
  check(encoder != nil, "render encoder creates");
  [encoder setRenderPipelineState:pipeline];
  [encoder setViewport:MTLViewport{0, 0, double(kExtent), double(kExtent), 0, 1}];
  [encoder setScissorRect:MTLScissorRect{0, 0, kExtent, kExtent}];
  [encoder setCullMode:MTLCullModeNone];
  return encoder;
}

void observeCompletion(id<MTLCommandBuffer> command, RunResult& result) {
  [command waitUntilCompleted];
  if (command.status != MTLCommandBufferStatusCompleted || command.error) {
    ++result.gpuErrors;
    [result.errors addObject:@{
      @"status": @(command.status),
      @"message": command.error ? command.error.localizedDescription
                                  : @"command buffer did not complete"
    }];
  }
}

void encodeReadback(id<MTLBlitCommandEncoder> encoder, id<MTLTexture> texture,
    id<MTLBuffer> buffer, NSUInteger bytesPerPixel) {
  [encoder copyFromTexture:texture sourceSlice:0 sourceLevel:0
      sourceOrigin:MTLOriginMake(0, 0, 0)
      sourceSize:MTLSizeMake(kExtent, kExtent, 1)
      toBuffer:buffer destinationOffset:0
      destinationBytesPerRow:kExtent * bytesPerPixel
      destinationBytesPerImage:kPixels * bytesPerPixel];
}

RunResult render(id<MTLDevice> device, id<MTLCommandQueue> queue,
    id<MTLRenderPipelineState> blend, id<MTLRenderPipelineState> conversion,
    id<MTLSamplerState> sampler, id<MTLBuffer> immutableParameters,
    MTLPixelFormat format, Boundary boundary) {
  RunResult result;
  const NSUInteger sourcePixelBytes = format == MTLPixelFormatRGBA16Float ? 8u : 4u;
  id<MTLTexture> source = makeTexture(device, format);
  id<MTLTexture> output = makeTexture(device, MTLPixelFormatRGBA8Unorm);
  id<MTLBuffer> sourceReadback = [device newBufferWithLength:kPixels * sourcePixelBytes
      options:MTLResourceStorageModeShared];
  id<MTLBuffer> outputReadback = [device newBufferWithLength:kPixels * 4u
      options:MTLResourceStorageModeShared];
  check(sourceReadback && outputReadback, "readback buffers create");
  id<MTLCommandBuffer> prefix = nil;
  id<MTLCommandBuffer> tail = [queue commandBuffer];
  check(tail != nil, "command buffer creates");
  result.commandBuffers = 1u;
  id<MTLRenderCommandEncoder> encoder = beginPass(tail, source, blend, true);
  for (std::uint32_t draw = 0u; draw < kDrawCount; ++draw) {
    if (draw == kSplitAfter && boundary != Boundary::None) {
      [encoder endEncoding];
      if (boundary == Boundary::SeparateCommandBuffer) {
        prefix = tail;
        [prefix commit];
        tail = [queue commandBuffer];
        check(tail != nil, "successor command buffer creates");
        ++result.commandBuffers;
      }
      // Same queue, tracked resources, and no CPU wait before the suffix.
      // Both source texture and the immutable parameter buffer remain strongly
      // owned until every command buffer has completed; nothing is recycled.
      encoder = beginPass(tail, source, blend, false);
    }
    [encoder setFragmentBuffer:immutableParameters
        offset:draw * kParameterStride atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  }
  [encoder endEncoding];
  encoder = beginPass(tail, output, conversion, true);
  [encoder setFragmentTexture:source atIndex:0];
  [encoder setFragmentSamplerState:sampler atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [encoder endEncoding];
  id<MTLBlitCommandEncoder> readback = [tail blitCommandEncoder];
  check(readback != nil, "readback encoder creates");
  encodeReadback(readback, source, sourceReadback, sourcePixelBytes);
  encodeReadback(readback, output, outputReadback, 4u);
  [readback endEncoding];
  [tail commit];
  if (prefix) observeCompletion(prefix, result);
  observeCompletion(tail, result);
  result.complete = result.gpuErrors == 0u;
  if (result.complete) {
    result.source.resize(kPixels * sourcePixelBytes);
    result.output.resize(kPixels * 4u);
    std::memcpy(result.source.data(), sourceReadback.contents, result.source.size());
    std::memcpy(result.output.data(), outputReadback.contents, result.output.size());
  }
  return result;
}

float halfValue(std::uint16_t bits) {
  const float sign = (bits & 0x8000u) ? -1.0f : 1.0f;
  const unsigned exponent = (bits >> 10u) & 31u;
  const unsigned mantissa = bits & 1023u;
  if (exponent == 31u) {
    return mantissa ? std::numeric_limits<float>::quiet_NaN()
                    : sign * std::numeric_limits<float>::infinity();
  }
  return sign * (exponent == 0u
      ? std::ldexp(static_cast<float>(mantissa), -24)
      : std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f,
                   static_cast<int>(exponent) - 15));
}

double sourceComponent(const std::vector<std::uint8_t>& bytes,
    std::size_t component, bool half) {
  if (!half) return bytes[component];
  std::uint16_t bits = 0u;
  std::memcpy(&bits, bytes.data() + component * 2u, sizeof(bits));
  return halfValue(bits);
}

NSDictionary* difference(const std::vector<std::uint8_t>& reference,
    const std::vector<std::uint8_t>& candidate, bool half) {
  if (reference.empty() || reference.size() != candidate.size()) {
    return @{@"available": @NO, @"byte_equal": @NO};
  }
  std::size_t changedBytes = 0u, changedPixels = 0u, changedComponents = 0u;
  unsigned maxByteDelta = 0u;
  double maxComponentDelta = 0.0;
  const std::size_t componentBytes = half ? 2u : 1u;
  for (std::size_t i = 0u; i < reference.size(); ++i) {
    if (reference[i] != candidate[i]) ++changedBytes;
    maxByteDelta = std::max(maxByteDelta,
        static_cast<unsigned>(std::abs(int(reference[i]) - int(candidate[i]))));
  }
  for (std::size_t pixel = 0u; pixel < kPixels; ++pixel) {
    bool changed = false;
    for (std::size_t channel = 0u; channel < 4u; ++channel) {
      const std::size_t component = pixel * 4u + channel;
      if (std::memcmp(reference.data() + component * componentBytes,
                      candidate.data() + component * componentBytes,
                      componentBytes) != 0) {
        ++changedComponents;
        changed = true;
      }
      maxComponentDelta = std::max(maxComponentDelta, std::abs(
          sourceComponent(reference, component, half) -
          sourceComponent(candidate, component, half)));
    }
    if (changed) ++changedPixels;
  }
  return @{
    @"available": @YES, @"byte_equal": @(changedBytes == 0u),
    @"component_encoding": half ? @"float16-le" : @"uint8",
    @"changed_bytes": @(changedBytes), @"changed_pixels": @(changedPixels),
    @"changed_components": @(changedComponents),
    @"half_component_equal": half ? (id)@(changedComponents == 0u) : (id)[NSNull null],
    @"max_byte_delta": @(maxByteDelta), @"max_component_delta": @(maxComponentDelta)
  };
}

bool nonDegenerate(const RunResult& result, bool half) {
  if (!result.complete || result.source.empty() || result.output.empty()) return false;
  bool varied = false, interior = false;
  for (std::size_t pixel = 0u; pixel < kPixels; ++pixel) {
    if (result.output[pixel * 4u + 3u] != 255u) return false;
    for (std::size_t channel = 0u; channel < 3u; ++channel) {
      const auto value = result.output[pixel * 4u + channel];
      varied = varied || value != result.output[channel];
      interior = interior || (value > 0u && value < 255u);
      const auto source = sourceComponent(result.source, pixel * 4u + channel, half);
      if (!std::isfinite(source) || source <= 0.0 || source >= (half ? 1.0 : 255.0))
        return false;
    }
  }
  return varied && interior;
}

NSDictionary* runReport(const RunResult& reference, const RunResult& candidate,
    bool half, NSString* boundary) {
  return @{
    @"boundary": boundary, @"completed": @(candidate.complete),
    @"command_buffers": @(candidate.commandBuffers),
    @"gpu_errors": @(candidate.gpuErrors), @"errors": candidate.errors,
    @"non_degenerate": @(nonDegenerate(candidate, half)),
    @"source": difference(reference.source, candidate.source, half),
    @"final_rgba8": difference(reference.output, candidate.output, false)
  };
}

void printJson(NSDictionary* result) {
  NSError* error = nil;
  NSData* bytes = [NSJSONSerialization dataWithJSONObject:result options:0 error:&error];
  check(bytes != nil, "JSON serialization succeeds");
  std::fwrite(bytes.bytes, 1u, bytes.length, stdout);
  std::fputc('\n', stdout);
}

}  // namespace

int main(int argc, char**) {
  @autoreleasepool {
    if (argc != 1) {
      std::fprintf(stderr, "This standalone diagnostic takes no arguments.\n");
      return 2;
    }
    try {
      id<MTLDevice> device = MTLCreateSystemDefaultDevice();
      if (!device) {
        printJson(@{@"schema": @"dxmt9.native.render_pass_roundtrip.v1",
                    @"status": @"skipped", @"reason": @"no-metal-device"});
        return 77;
      }
      id<MTLLibrary> library = makeLibrary(device);
      id<MTLCommandQueue> queue = [device newCommandQueue];
      check(queue != nil, "command queue creates");
      MTLSamplerDescriptor* samplerDescriptor = [[MTLSamplerDescriptor alloc] init];
      samplerDescriptor.minFilter = MTLSamplerMinMagFilterNearest;
      samplerDescriptor.magFilter = MTLSamplerMinMagFilterNearest;
      samplerDescriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
      samplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
      samplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
      samplerDescriptor.normalizedCoordinates = YES;
      id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:samplerDescriptor];
      check(sampler != nil, "sampler creates");
      auto conversion = makePipeline(device, library, MTLPixelFormatRGBA8Unorm, false, 0u);
      NSMutableArray* cases = [NSMutableArray array];
      bool valid = true, allSplitEqual = true;
      std::uint32_t gpuErrors = 0u;
      for (const auto format : {MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA16Float}) {
        const bool half = format == MTLPixelFormatRGBA16Float;
        for (std::uint32_t workload = 0u; workload < 2u; ++workload) {
          auto blend = makePipeline(device, library, format, true, workload);
          std::array<std::uint8_t, kDrawCount * kParameterStride> parameterBytes{};
          for (std::uint32_t draw = 0u; draw < kDrawCount; ++draw) {
            const DrawParameters parameters{draw, workload, {0u, 0u}};
            std::memcpy(parameterBytes.data() + draw * kParameterStride,
                        &parameters, sizeof(parameters));
          }
          // Initialized exactly once before any submission; no later CPU write.
          id<MTLBuffer> immutable = [device newBufferWithBytes:parameterBytes.data()
              length:parameterBytes.size() options:MTLResourceStorageModeShared];
          check(immutable != nil, "immutable parameter buffer creates");
          const auto reference = render(device, queue, blend, conversion, sampler,
              immutable, format, Boundary::None);
          const auto repeat = render(device, queue, blend, conversion, sampler,
              immutable, format, Boundary::None);
          const auto same = render(device, queue, blend, conversion, sampler,
              immutable, format, Boundary::SameCommandBuffer);
          const auto separate = render(device, queue, blend, conversion, sampler,
              immutable, format, Boundary::SeparateCommandBuffer);
          const bool referenceRepeatEqual = reference.complete && repeat.complete &&
              reference.source == repeat.source && reference.output == repeat.output;
          const bool caseValid = referenceRepeatEqual && nonDegenerate(reference, half) &&
              nonDegenerate(repeat, half) && nonDegenerate(same, half) &&
              nonDegenerate(separate, half);
          const bool splitEqual = caseValid && reference.source == same.source &&
              reference.output == same.output && reference.source == separate.source &&
              reference.output == separate.output;
          valid = valid && caseValid;
          allSplitEqual = allSplitEqual && splitEqual;
          gpuErrors += reference.gpuErrors + repeat.gpuErrors + same.gpuErrors +
                       separate.gpuErrors;
          [cases addObject:@{
            @"format": half ? @"RGBA16Float" : @"RGBA8Unorm",
            @"workload": workload == 0u ? @"additive" : @"source-over",
            @"valid": @(caseValid), @"reference_repeat_equal": @(referenceRepeatEqual),
            @"split_source_and_final_equal": @(splitEqual),
            @"reference": runReport(reference, reference, half, @"one-pass"),
            @"reference_repeat": runReport(reference, repeat, half, @"one-pass"),
            @"same_cb_split": runReport(reference, same, half, @"pass-reopen-after-command-3"),
            @"separate_cb_split": runReport(reference, separate, half, @"cb-split-after-command-3"),
            @"same_cb_vs_separate_cb": @{
              @"source": difference(same.source, separate.source, half),
              @"final_rgba8": difference(same.output, separate.output, false)
            }
          }];
        }
      }
      printJson(@{
        @"schema": @"dxmt9.native.render_pass_roundtrip.v1",
        @"status": valid ? @"complete" : @"failed", @"diagnostic": @YES,
        @"timing_eligible": @NO, @"production_policy_qualified": @NO,
        @"device": device.name, @"width": @(kExtent), @"height": @(kExtent),
        @"draw_commands": @(kDrawCount), @"split_after_draw_command": @(kSplitAfter),
        @"shader_math_policy": @"safe-with-position-invariance",
        @"split_equality_is_required": @NO, @"all_split_equal": @(allSplitEqual),
        @"gpu_errors": @(gpuErrors), @"cases": cases,
        @"scope": @"native-immutable-color-attachment-roundtrip-only",
        @"exclusions": @[@"dxmt9-provider-replay", @"publication-sequence-lifetime",
            @"mutable-buffer-uploads", @"depth-stencil", @"MSAA", @"sRGB",
            @"framegraph-optimizations", @"production-scene-equivalence"]
      });
      // Healthy deterministic differences are the diagnostic's result, not a
      // failed equality assertion. GPU errors/degeneracy/repeat drift do fail.
      return valid ? 0 : 1;
    } catch (const std::exception& error) {
      printJson(@{@"schema": @"dxmt9.native.render_pass_roundtrip.v1",
                  @"status": @"failed", @"diagnostic": @YES,
                  @"reason": [NSString stringWithUTF8String:error.what()]});
      return 1;
    }
  }
}
