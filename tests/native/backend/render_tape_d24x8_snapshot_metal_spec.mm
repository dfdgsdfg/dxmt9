// Concrete Metal capability proof for Render Tape's D24X8 snapshot encoding.
// The canonical payload is version 1: tightly packed little-endian float32
// depth values. It is produced and consumed by shaders, never by assuming the
// byte layout of Metal's physical depth/stencil format.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace {

constexpr NSUInteger kWidth = 17;
constexpr NSUInteger kHeight = 11;
constexpr std::uint32_t kEncodingVersion = 1;

enum class LogicalFormat : std::uint8_t {
  D24X8,
  D24S8,
};

enum class SnapshotStatus : std::uint8_t {
  Ready,
  StencilBearing,
  Multisampled,
  UnsupportedPhysicalFormat,
  InvalidLayout,
  CopyFailed,
};

struct CanonicalDepth {
  std::uint32_t version = kEncodingVersion;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t pitch = 0;
  std::vector<std::uint8_t> bytes;
};

[[noreturn]] void fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
  std::exit(1);
}

void check(bool condition, std::string_view message) {
  if (!condition) {
    fail(message);
  }
}

SnapshotStatus classify(LogicalFormat format,
                        NSUInteger sampleCount,
                        MTLPixelFormat physicalFormat,
                        MTLPixelFormat expectedPhysicalFormat,
                        std::uint32_t width,
                        std::uint32_t height,
                        std::uint32_t pitch,
                        std::size_t byteCount,
                        bool copySucceeded) {
  if (format == LogicalFormat::D24S8) {
    return SnapshotStatus::StencilBearing;
  }
  if (sampleCount != 1) {
    return SnapshotStatus::Multisampled;
  }
  if (physicalFormat != expectedPhysicalFormat) {
    return SnapshotStatus::UnsupportedPhysicalFormat;
  }
  if (width == 0 || height == 0 ||
      width > std::numeric_limits<std::uint32_t>::max() / sizeof(float)) {
    return SnapshotStatus::InvalidLayout;
  }
  const std::uint32_t expectedPitch = width * sizeof(float);
  if (pitch != expectedPitch ||
      height > std::numeric_limits<std::size_t>::max() / expectedPitch ||
      byteCount != static_cast<std::size_t>(expectedPitch) * height) {
    return SnapshotStatus::InvalidLayout;
  }
  if (!copySucceeded) {
    return SnapshotStatus::CopyFailed;
  }
  return SnapshotStatus::Ready;
}

id<MTLLibrary> makeLibrary(id<MTLDevice> device) {
  static NSString* source = @R"msl(
#include <metal_stdlib>
using namespace metal;

struct VertexOut {
  float4 position [[position]];
};

vertex VertexOut fullscreen_vs(uint vertexId [[vertex_id]]) {
  const float2 positions[3] = {
    float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0),
  };
  VertexOut out;
  out.position = float4(positions[vertexId], 0.0, 1.0);
  return out;
}

fragment float read_depth_fs(VertexOut in [[stage_in]],
                             depth2d<float> source [[texture(0)]]) {
  return source.read(uint2(in.position.xy));
}

struct DepthOut {
  float depth [[depth(any)]];
};

fragment DepthOut write_depth_fs(VertexOut in [[stage_in]],
                                 texture2d<float> source [[texture(0)]]) {
  return DepthOut{source.read(uint2(in.position.xy)).r};
}

fragment DepthOut constant_depth_fs(constant float& value [[buffer(0)]]) {
  return DepthOut{value};
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
                                        MTLPixelFormat colorFormat,
                                        MTLPixelFormat depthFormat) {
  MTLRenderPipelineDescriptor* descriptor =
      [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.vertexFunction = [library newFunctionWithName:@"fullscreen_vs"];
  descriptor.fragmentFunction = [library newFunctionWithName:fragmentName];
  descriptor.colorAttachments[0].pixelFormat = colorFormat;
  descriptor.depthAttachmentPixelFormat = depthFormat;
  NSError* error = nil;
  id<MTLRenderPipelineState> pipeline =
      [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
  if (!pipeline) {
    std::cerr << "Metal pipeline compile failed for " << fragmentName.UTF8String
              << ": "
              << (error ? error.localizedDescription.UTF8String : "unknown")
              << '\n';
  }
  return pipeline;
}

id<MTLTexture> makeTexture(id<MTLDevice> device,
                           MTLPixelFormat format,
                           MTLTextureUsage usage,
                           MTLStorageMode storageMode) {
  MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:format
                                   width:kWidth
                                  height:kHeight
                               mipmapped:NO];
  descriptor.storageMode = storageMode;
  descriptor.usage = usage;
  return [device newTextureWithDescriptor:descriptor];
}

bool finish(id<MTLCommandBuffer> commandBuffer, std::string_view label) {
  [commandBuffer commit];
  [commandBuffer waitUntilCompleted];
  if (commandBuffer.status == MTLCommandBufferStatusCompleted) {
    return true;
  }
  std::cerr << label << " command buffer failed: "
            << (commandBuffer.error
                    ? commandBuffer.error.localizedDescription.UTF8String
                    : "unknown")
            << '\n';
  return false;
}

MTLRenderPassDescriptor* depthPass(id<MTLTexture> texture,
                                   MTLLoadAction loadAction,
                                   double clearDepth) {
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.depthAttachment.texture = texture;
  pass.depthAttachment.loadAction = loadAction;
  pass.depthAttachment.storeAction = MTLStoreActionStore;
  pass.depthAttachment.clearDepth = clearDepth;
  return pass;
}

bool initializeSource(id<MTLCommandQueue> queue,
                      id<MTLTexture> source,
                      id<MTLRenderPipelineState> constantPipeline,
                      id<MTLDepthStencilState> depthState) {
  id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
  id<MTLRenderCommandEncoder> encoder = [commandBuffer
      renderCommandEncoderWithDescriptor:depthPass(
          source, MTLLoadActionClear, 0.2718281828)];
  [encoder setRenderPipelineState:constantPipeline];
  [encoder setDepthStencilState:depthState];
  [encoder setScissorRect:MTLScissorRect{3, 2, 9, 6}];
  const float patchDepth = 0.812345f;
  [encoder setFragmentBytes:&patchDepth length:sizeof(patchDepth) atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle
              vertexStart:0
              vertexCount:3];
  [encoder endEncoding];
  return finish(commandBuffer, "source clear/draw drain");
}

CanonicalDepth captureDepth(id<MTLCommandQueue> queue,
                            id<MTLDevice> device,
                            id<MTLTexture> depth,
                            id<MTLRenderPipelineState> readPipeline) {
  CanonicalDepth result{
      .width = static_cast<std::uint32_t>(kWidth),
      .height = static_cast<std::uint32_t>(kHeight),
      .pitch = static_cast<std::uint32_t>(kWidth * sizeof(float)),
  };
  id<MTLTexture> canonical = makeTexture(
      device, MTLPixelFormatR32Float,
      MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead,
      MTLStorageModeShared);
  if (!canonical) {
    return {};
  }
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = canonical;
  pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
  id<MTLRenderCommandEncoder> encoder =
      [commandBuffer renderCommandEncoderWithDescriptor:pass];
  [encoder setRenderPipelineState:readPipeline];
  [encoder setFragmentTexture:depth atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle
              vertexStart:0
              vertexCount:3];
  [encoder endEncoding];
  if (!finish(commandBuffer, "depth capture")) {
    return {};
  }
  result.bytes.resize(static_cast<std::size_t>(result.pitch) * result.height);
  [canonical getBytes:result.bytes.data()
            bytesPerRow:result.pitch
             fromRegion:MTLRegionMake2D(0, 0, kWidth, kHeight)
            mipmapLevel:0];
  return result;
}

bool seedDepth(id<MTLCommandQueue> queue,
               id<MTLDevice> device,
               id<MTLTexture> destination,
               id<MTLRenderPipelineState> writePipeline,
               id<MTLDepthStencilState> depthState,
               const CanonicalDepth& canonical) {
  id<MTLTexture> upload = makeTexture(
      device, MTLPixelFormatR32Float, MTLTextureUsageShaderRead,
      MTLStorageModeShared);
  if (!upload) {
    return false;
  }
  [upload replaceRegion:MTLRegionMake2D(0, 0, kWidth, kHeight)
             mipmapLevel:0
               withBytes:canonical.bytes.data()
             bytesPerRow:canonical.pitch];
  id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
  id<MTLRenderCommandEncoder> encoder = [commandBuffer
      renderCommandEncoderWithDescriptor:depthPass(
          destination, MTLLoadActionDontCare, 1.0)];
  [encoder setRenderPipelineState:writePipeline];
  [encoder setDepthStencilState:depthState];
  [encoder setFragmentTexture:upload atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle
              vertexStart:0
              vertexCount:3];
  [encoder endEncoding];
  return finish(commandBuffer, "depth seed");
}

std::uint64_t hash(std::span<const std::uint8_t> bytes) {
  std::uint64_t value = 1469598103934665603ull;
  for (const auto byte : bytes) {
    value ^= byte;
    value *= 1099511628211ull;
  }
  return value;
}

void testRejectionMatrix(MTLPixelFormat physicalFormat) {
  constexpr std::uint32_t width = 17;
  constexpr std::uint32_t height = 11;
  constexpr std::uint32_t pitch = width * sizeof(float);
  constexpr std::size_t bytes = static_cast<std::size_t>(pitch) * height;
  check(classify(LogicalFormat::D24X8, 1, physicalFormat, physicalFormat,
                 width, height, pitch, bytes, true) == SnapshotStatus::Ready,
        "exact D24X8 layout is accepted");
  check(classify(LogicalFormat::D24S8, 1, physicalFormat, physicalFormat,
                 width, height, pitch, bytes, true) ==
            SnapshotStatus::StencilBearing,
        "D24S8 is explicitly rejected as stencil-bearing");
  check(classify(LogicalFormat::D24X8, 4, physicalFormat, physicalFormat,
                 width, height, pitch, bytes, true) ==
            SnapshotStatus::Multisampled,
        "MSAA D24X8 is rejected");
  check(classify(LogicalFormat::D24X8, 1, MTLPixelFormatDepth16Unorm,
                 physicalFormat, width, height, pitch, bytes, true) ==
            SnapshotStatus::UnsupportedPhysicalFormat,
        "unexpected physical depth format is rejected");
  check(classify(LogicalFormat::D24X8, 1, physicalFormat, physicalFormat,
                 width, height, pitch + 4, bytes, true) ==
            SnapshotStatus::InvalidLayout,
        "non-tight pitch is rejected");
  check(classify(LogicalFormat::D24X8, 1, physicalFormat, physicalFormat,
                 width, height, pitch, bytes - 1, true) ==
            SnapshotStatus::InvalidLayout,
        "partial canonical bytes are rejected");
  check(classify(LogicalFormat::D24X8, 1, physicalFormat, physicalFormat,
                 width, height, pitch, bytes, false) ==
            SnapshotStatus::CopyFailed,
        "copy failure is rejected");
}

}  // namespace

int main() {
  @autoreleasepool {
    check(std::endian::native == std::endian::little,
          "version 1 canonical encoding requires little endian");
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    check(device != nil, "Metal device is available");
    const MTLPixelFormat physicalFormat =
        device.depth24Stencil8PixelFormatSupported
            ? MTLPixelFormatDepth24Unorm_Stencil8
            : MTLPixelFormatDepth32Float_Stencil8;
    testRejectionMatrix(physicalFormat);

    id<MTLLibrary> library = makeLibrary(device);
    check(library != nil, "shader library compiled");
    id<MTLRenderPipelineState> readPipeline = makePipeline(
        device, library, @"read_depth_fs", MTLPixelFormatR32Float,
        MTLPixelFormatInvalid);
    id<MTLRenderPipelineState> writePipeline = makePipeline(
        device, library, @"write_depth_fs", MTLPixelFormatInvalid,
        physicalFormat);
    id<MTLRenderPipelineState> constantPipeline = makePipeline(
        device, library, @"constant_depth_fs", MTLPixelFormatInvalid,
        physicalFormat);
    check(readPipeline && writePipeline && constantPipeline,
          "depth conversion pipelines compiled");

    MTLDepthStencilDescriptor* depthDescriptor =
        [[MTLDepthStencilDescriptor alloc] init];
    depthDescriptor.depthCompareFunction = MTLCompareFunctionAlways;
    depthDescriptor.depthWriteEnabled = YES;
    id<MTLDepthStencilState> depthState =
        [device newDepthStencilStateWithDescriptor:depthDescriptor];
    check(depthState != nil, "depth-write state created");

    id<MTLTexture> source = makeTexture(
        device, physicalFormat,
        MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead,
        MTLStorageModePrivate);
    id<MTLTexture> destination = makeTexture(
        device, physicalFormat,
        MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead,
        MTLStorageModePrivate);
    check(source && destination, "physical D24X8-equivalent surfaces created");
    id<MTLCommandQueue> queue = [device newCommandQueue];
    check(queue != nil, "command queue created");

    check(initializeSource(queue, source, constantPipeline, depthState),
          "source clear/draw completed and drained");
    const auto captured = captureDepth(queue, device, source, readPipeline);
    check(captured.version == kEncodingVersion &&
              captured.pitch == kWidth * sizeof(float) &&
              captured.bytes.size() == captured.pitch * captured.height,
          "owned canonical v1 depth bytes have exact tight layout");
    check(classify(LogicalFormat::D24X8, source.sampleCount,
                   source.pixelFormat, physicalFormat, captured.width,
                   captured.height, captured.pitch, captured.bytes.size(),
                   true) == SnapshotStatus::Ready,
          "captured runtime surface passes the exact capability contract");
    check(seedDepth(queue, device, destination, writePipeline, depthState,
                    captured),
          "canonical bytes seed a second physical depth surface");
    const auto replayed = captureDepth(queue, device, destination, readPipeline);
    check(replayed.version == captured.version &&
              replayed.width == captured.width &&
              replayed.height == captured.height &&
              replayed.pitch == captured.pitch,
          "replayed canonical metadata matches");
    check(replayed.bytes == captured.bytes,
          "depth capture/seed/capture roundtrip is byte-exact");

    std::cout << "render_tape_d24x8_snapshot_metal_spec passed"
              << " physical=" << static_cast<unsigned>(physicalFormat)
              << " encoding=" << kEncodingVersion
              << " digest=0x" << std::hex << hash(captured.bytes) << std::dec
              << " bytes=" << captured.bytes.size() << '\n';
  }
  return 0;
}
