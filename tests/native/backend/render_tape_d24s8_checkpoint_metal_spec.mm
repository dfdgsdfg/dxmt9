// Independent capability proof plus the actual Unix/bridge checkpoint path.
// D3D9 D24S8 backed by Metal Depth32Float_Stencil8 is represented as exact
// float32-LE depth followed by uint8 stencil, never a quantized packed D24 word.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "device_c_common.hpp"
#include "device_c_render_tape_descriptors.hpp"
#include "dxmt9/dxmt9_command_queue.hpp"
#include "dxmt9/dxmt9_device.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::uint32_t kWidth = 17u;
constexpr std::uint32_t kHeight = 19u;
constexpr std::size_t kPixels = kWidth * kHeight;
constexpr std::size_t kDepthBytes = kPixels * sizeof(float);
constexpr std::size_t kStencilBytes = kPixels;
constexpr std::size_t kCheckpointBytes = kDepthBytes + kStencilBytes;
static_assert(std::endian::native == std::endian::little);

void check(bool value, std::string_view message) {
  if (!value) throw std::runtime_error(std::string(message));
}

void finish(id<MTLCommandBuffer> command, std::string_view operation) {
  check(command != nil, "Metal command buffer creates");
  [command commit];
  [command waitUntilCompleted];
  if (command.status != MTLCommandBufferStatusCompleted) {
    throw std::runtime_error(std::string(operation) + ": " +
        (command.error ? command.error.localizedDescription.UTF8String : "unknown Metal failure"));
  }
}

struct Pattern {
  std::array<float, kPixels> depth{};
  std::array<std::uint8_t, kPixels> stencil{};
  std::vector<std::uint8_t> checkpoint() const {
    std::vector<std::uint8_t> result(kCheckpointBytes);
    std::memcpy(result.data(), depth.data(), kDepthBytes);
    std::memcpy(result.data() + kDepthBytes, stencil.data(), kStencilBytes);
    return result;
  }
};

Pattern pattern(bool alternate) {
  Pattern result;
  for (std::size_t index = 0u; index < kPixels; ++index) {
    const bool high = ((index + (alternate ? 1u : 0u)) % 3u) != 0u;
    // Variable mantissa bits distinguish backend float precision from a
    // rounded 24-bit D3D depth encoding. Values remain finite in [0.25, 1).
    const auto bits = (high ? 0x3f400000u : 0x3e800000u) |
        ((static_cast<std::uint32_t>(index) * 7919u + (alternate ? 17u : 3u)) & 0x000fffffu);
    result.depth[index] = std::bit_cast<float>(bits);
    result.stencil[index] = static_cast<std::uint8_t>(index * 73u + (alternate ? 97u : 19u));
  }
  return result;
}

id<MTLTexture> texture(id<MTLDevice> device, MTLPixelFormat format, NSString* label) {
  auto* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
      width:kWidth height:kHeight mipmapped:NO];
  descriptor.storageMode = MTLStorageModePrivate;
  descriptor.usage = MTLTextureUsageRenderTarget;
  id<MTLTexture> result = [device newTextureWithDescriptor:descriptor];
  check(result != nil, "Metal test texture creates");
  result.label = label;
  return result;
}

id<MTLLibrary> library(id<MTLDevice> device) {
  static NSString* source = @R"msl(
#include <metal_stdlib>
using namespace metal;
struct V { float4 position [[position, invariant]]; float4 color; };
vertex V fullscreen(uint i [[vertex_id]], constant float4& params [[buffer(0)]]) {
  const float2 p[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)};
  return V{float4(p[i], params.x, 1), float4(params.yzw, 1)};
}
struct DepthOut { float depth [[depth(any)]]; };
fragment DepthOut initialize(V in [[stage_in]],
    constant float* depths [[buffer(0)]], constant uint& width [[buffer(1)]]) {
  uint2 pixel = uint2(in.position.xy);
  return DepthOut{depths[pixel.y * width + pixel.x]};
}
fragment float4 probe(V in [[stage_in]]) { return in.color; }
)msl";
  NSError* error = nil;
  id<MTLLibrary> result = [device newLibraryWithSource:source options:nil error:&error];
  if (!result)
    throw std::runtime_error(std::string("MSL compile: ") + error.localizedDescription.UTF8String);
  result.label = @"D24S8 checkpoint capability shaders";
  return result;
}

id<MTLRenderPipelineState> pipeline(id<MTLDevice> device, id<MTLLibrary> shaders,
                                    bool initialize) {
  auto* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.vertexFunction = [shaders newFunctionWithName:@"fullscreen"];
  descriptor.fragmentFunction = [shaders newFunctionWithName:initialize ? @"initialize" : @"probe"];
  descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
  descriptor.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
  descriptor.label = initialize ? @"Independent depth stencil initializer" : @"Depth stencil pixel oracle";
  if (!initialize) descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
  NSError* error = nil;
  id<MTLRenderPipelineState> result = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
  if (!result)
    throw std::runtime_error(std::string("Metal pipeline: ") + error.localizedDescription.UTF8String);
  return result;
}

id<MTLDepthStencilState> testState(id<MTLDevice> device, bool initialize,
                                   bool testDepth, bool testStencil) {
  auto* descriptor = [[MTLDepthStencilDescriptor alloc] init];
  descriptor.depthCompareFunction = testDepth ? MTLCompareFunctionLess : MTLCompareFunctionAlways;
  descriptor.depthWriteEnabled = initialize;
  descriptor.label = initialize ? @"Independent full aspect writes" : @"Read-only aspect test";
  auto* stencil = [[MTLStencilDescriptor alloc] init];
  stencil.stencilCompareFunction = testStencil ? MTLCompareFunctionEqual : MTLCompareFunctionAlways;
  stencil.readMask = 0xffu;
  stencil.writeMask = initialize ? 0xffu : 0u;
  stencil.stencilFailureOperation = MTLStencilOperationKeep;
  stencil.depthFailureOperation = MTLStencilOperationKeep;
  stencil.depthStencilPassOperation = initialize ? MTLStencilOperationReplace : MTLStencilOperationKeep;
  descriptor.frontFaceStencil = stencil;
  descriptor.backFaceStencil = stencil;
  id<MTLDepthStencilState> result = [device newDepthStencilStateWithDescriptor:descriptor];
  check(result != nil, "depth stencil state creates");
  return result;
}

MTLRenderPassDescriptor* attachmentPass(id<MTLTexture> attachment, MTLLoadAction action) {
  auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.depthAttachment.texture = attachment;
  pass.depthAttachment.loadAction = action;
  pass.depthAttachment.storeAction = MTLStoreActionStore;
  pass.depthAttachment.clearDepth = 1.0;
  pass.stencilAttachment.texture = attachment;
  pass.stencilAttachment.loadAction = action;
  pass.stencilAttachment.storeAction = MTLStoreActionStore;
  pass.stencilAttachment.clearStencil = 0u;
  return pass;
}

void initialize(id<MTLDevice> device, id<MTLCommandQueue> queue,
                 id<MTLTexture> destination, id<MTLRenderPipelineState> draw,
                 const Pattern& values) {
  // Independent source production: float fragment depth + hardware stencil
  // Replace, not the buffer-to-texture path being qualified below.
  id<MTLBuffer> depths = [device newBufferWithBytes:values.depth.data()
      length:kDepthBytes options:MTLResourceStorageModeShared];
  check(depths != nil, "initializer depth buffer creates");
  depths.label = @"Independent nonuniform depth values";
  id<MTLCommandBuffer> command = [queue commandBuffer];
  command.label = @"Independent depth stencil pattern production";
  id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:
      attachmentPass(destination, MTLLoadActionClear)];
  check(encoder != nil, "initializer render encoder creates");
  encoder.label = @"Independent per-pixel depth stencil writes";
  [encoder setRenderPipelineState:draw];
  [encoder setDepthStencilState:testState(device, true, false, false)];
  constexpr std::array<float, 4u> params{0.5f, 0.f, 0.f, 0.f};
  [encoder setVertexBytes:params.data() length:sizeof(params) atIndex:0];
  [encoder setFragmentBuffer:depths offset:0 atIndex:0];
  [encoder setFragmentBytes:&kWidth length:sizeof(kWidth) atIndex:1];
  for (std::uint32_t y = 0u; y < kHeight; ++y) {
    for (std::uint32_t x = 0u; x < kWidth; ++x) {
      [encoder setScissorRect:MTLScissorRect{x, y, 1u, 1u}];
      [encoder setStencilReferenceValue:values.stencil[y * kWidth + x]];
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
  }
  [encoder endEncoding];
  finish(command, "independent pattern production");
}

bool exactBacking(id<MTLTexture> attachment) {
  return attachment && attachment.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 &&
      attachment.sampleCount == 1u && attachment.width == kWidth &&
      attachment.height == kHeight;
}

std::vector<std::uint8_t> capture(id<MTLDevice> device, id<MTLCommandQueue> queue,
                                  id<MTLTexture> attachment) {
  check(exactBacking(attachment), "capture rejects every other physical backing");
  id<MTLBuffer> output = [device newBufferWithLength:kCheckpointBytes
      options:MTLResourceStorageModeShared];
  check(output != nil, "dual-plane readback buffer creates");
  output.label = @"Exact float32 LE depth then uint8 stencil";
  id<MTLCommandBuffer> command = [queue commandBuffer];
  command.label = @"D32S8 aspect-complete checkpoint capture";
  id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
  check(blit != nil, "plane capture blit creates");
  blit.label = @"Separate depth stencil plane readback";
  const MTLOrigin origin{0u, 0u, 0u};
  const MTLSize extent{kWidth, kHeight, 1u};
  [blit copyFromTexture:attachment sourceSlice:0 sourceLevel:0 sourceOrigin:origin
      sourceSize:extent toBuffer:output destinationOffset:0
      destinationBytesPerRow:kWidth * sizeof(float) destinationBytesPerImage:kDepthBytes
      options:MTLBlitOptionDepthFromDepthStencil];
  [blit copyFromTexture:attachment sourceSlice:0 sourceLevel:0 sourceOrigin:origin
      sourceSize:extent toBuffer:output destinationOffset:kDepthBytes
      destinationBytesPerRow:kWidth destinationBytesPerImage:kStencilBytes
      options:MTLBlitOptionStencilFromDepthStencil];
  [blit endEncoding];
  finish(command, "dual-plane capture");
  const auto* bytes = static_cast<const std::uint8_t*>(output.contents);
  return {bytes, bytes + kCheckpointBytes};
}

void restore(id<MTLDevice> device, id<MTLCommandQueue> queue,
              id<MTLTexture> attachment, std::span<const std::uint8_t> bytes,
              bool depth = true, bool stencil = true) {
  check(exactBacking(attachment) && bytes.size() == kCheckpointBytes,
        "restore requires exact backing and the exact5N checkpoint extent");
  id<MTLBuffer> input = [device newBufferWithBytes:bytes.data() length:bytes.size()
      options:MTLResourceStorageModeShared];
  check(input != nil, "dual-plane restore buffer creates");
  input.label = @"Immutable exact depth stencil checkpoint";
  id<MTLCommandBuffer> command = [queue commandBuffer];
  command.label = @"D32S8 aspect-complete checkpoint restore";
  id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
  check(blit != nil, "plane restore blit creates");
  blit.label = @"Inverse separate depth stencil plane upload";
  const MTLOrigin origin{0u, 0u, 0u};
  const MTLSize extent{kWidth, kHeight, 1u};
  if (depth) {
    [blit copyFromBuffer:input sourceOffset:0 sourceBytesPerRow:kWidth * sizeof(float)
        sourceBytesPerImage:kDepthBytes sourceSize:extent toTexture:attachment
        destinationSlice:0 destinationLevel:0 destinationOrigin:origin
        options:MTLBlitOptionDepthFromDepthStencil];
  }
  if (stencil) {
    [blit copyFromBuffer:input sourceOffset:kDepthBytes sourceBytesPerRow:kWidth
        sourceBytesPerImage:kStencilBytes sourceSize:extent toTexture:attachment
        destinationSlice:0 destinationLevel:0 destinationOrigin:origin
        options:MTLBlitOptionStencilFromDepthStencil];
  }
  [blit endEncoding];
  finish(command, "dual-plane restore");
}

void pixelOracle(id<MTLDevice> device, id<MTLCommandQueue> queue,
                   id<MTLTexture> attachment, id<MTLRenderPipelineState> draw,
                   const Pattern& values, bool depthTest, bool stencilTest) {
  auto color = texture(device, MTLPixelFormatRGBA8Unorm, @"Aspect-tested pixel oracle target");
  auto* pass = attachmentPass(attachment, MTLLoadActionLoad);
  pass.colorAttachments[0].texture = color;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
  id<MTLCommandBuffer> command = [queue commandBuffer];
  command.label = @"Independent depth stencil tested pixels";
  id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
  check(encoder != nil, "pixel oracle render encoder creates");
  encoder.label = @"Read-only depth stencil comparison oracle";
  [encoder setRenderPipelineState:draw];
  [encoder setDepthStencilState:testState(device, false, depthTest, stencilTest)];
  const std::uint32_t references = stencilTest ? 256u : 1u;
  for (std::uint32_t reference = 0u; reference < references; ++reference) {
    const std::array<float, 4u> params{
        0.5f, static_cast<float>(stencilTest ? reference : 17u) / 255.f,
        101.f / 255.f, 203.f / 255.f};
    [encoder setVertexBytes:params.data() length:sizeof(params) atIndex:0];
    [encoder setStencilReferenceValue:reference];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  }
  [encoder endEncoding];
  id<MTLBuffer> output = [device newBufferWithLength:kPixels * 4u
      options:MTLResourceStorageModeShared];
  check(output != nil, "pixel readback buffer creates");
  output.label = @"Actual depth stencil tested RGBA bytes";
  id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
  check(blit != nil, "pixel readback encoder creates");
  blit.label = @"Aspect-tested pixel oracle readback";
  [blit copyFromTexture:color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOrigin{0,0,0}
      sourceSize:MTLSize{kWidth,kHeight,1} toBuffer:output destinationOffset:0
      destinationBytesPerRow:kWidth * 4u destinationBytesPerImage:kPixels * 4u];
  [blit endEncoding];
  finish(command, "depth stencil pixel oracle");
  const auto* actual = static_cast<const std::uint8_t*>(output.contents);
  for (std::size_t index = 0u; index < kPixels; ++index) {
    const bool passDepth = !depthTest || 0.5f < values.depth[index];
    const std::array<std::uint8_t, 4u> expected = passDepth
        ? std::array<std::uint8_t, 4u>{stencilTest ? values.stencil[index] : std::uint8_t{17u}, 101u, 203u, 255u}
        : std::array<std::uint8_t, 4u>{0u, 0u, 0u, 255u};
    check(std::memcmp(actual + index * expected.size(), expected.data(), expected.size()) == 0,
          "every actual aspect-tested RGBA pixel matches the independent oracle");
  }
}

void run() {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  check(device != nil, "Metal device exists");
  id<MTLCommandQueue> queue = [device newCommandQueue];
  check(queue != nil, "Metal queue creates");
  queue.label = @"D24S8 checkpoint capability queue";
  const auto shaders = library(device);
  const auto producer = pipeline(device, shaders, true);
  const auto oracle = pipeline(device, shaders, false);
  auto source = texture(device, MTLPixelFormatDepth32Float_Stencil8, @"Independently produced source depth stencil");
  auto destination = texture(device, MTLPixelFormatDepth32Float_Stencil8, @"Restored depth stencil checkpoint");
  const auto a = pattern(false);
  const auto b = pattern(true);
  const auto bytesA = a.checkpoint();
  const auto bytesB = b.checkpoint();
  check(bytesA != bytesB && std::memcmp(a.depth.data(), b.depth.data(), kDepthBytes) != 0 &&
            a.stencil != b.stencil, "A and B differ in both complete nonuniform aspects");
  for (const auto* values : {&a, &b, &a}) {
    initialize(device, queue, source, producer, *values);
    const auto captured = capture(device, queue, source);
    check(captured == values->checkpoint(),
          "independent rendered depth/stencil capture preserves every exact float bit and stencil byte");
    restore(device, queue, destination, captured);
    check(capture(device, queue, destination) == captured,
          "inverse depth and stencil plane blits restore all5N checkpoint bytes");
    pixelOracle(device, queue, destination, oracle, *values, true, false);
    pixelOracle(device, queue, destination, oracle, *values, false, true);
    pixelOracle(device, queue, destination, oracle, *values, true, true);
    check(capture(device, queue, destination) == captured,
          "read-only oracle draw tests preserve both restored aspects");
  }
  restore(device, queue, destination, bytesB);
  restore(device, queue, destination, bytesA, true, false);
  auto mixed = bytesA;
  std::memcpy(mixed.data() + kDepthBytes, bytesB.data() + kDepthBytes, kStencilBytes);
  check(capture(device, queue, destination) == mixed,
        "depth-only inverse plane copy preserves every preexisting stencil byte");
  restore(device, queue, destination, bytesA, false, true);
  check(capture(device, queue, destination) == bytesA,
        "stencil-only inverse plane copy preserves every preexisting depth bit");
  auto noStencil = texture(device, MTLPixelFormatDepth32Float, @"Rejected stencil-less backing");
  check(!exactBacking(noStencil) && !exactBacking(nil),
        "capability excludes stencil-less and absent backing instead of fabricating stencil");
  std::cout << "D24S8 checkpoint capability: Depth32Float_Stencil8 only; "
            << kWidth << 'x' << kHeight << "; exact5N=" << kCheckpointBytes
            << "; A-B-A byte roundtrips=3; aspect pixel oracles=9; "
            << "inverse depth/stencil preservation=2; all256 stencil values verified\n";
}

void checkpointCompletionExhaustive() {
  using namespace dxmt9;
  std::size_t checked = 0u;
  for (unsigned builds = 0u; builds < 32u; ++builds)
    check(renderTapeBackbufferSeedBuildQuiescent(
              builds & 1u, builds & 2u, builds & 4u, builds & 8u, builds & 16u) ==
              (builds == 0u), "every borrowed producer blocks payload inspection");
  for (unsigned flags = 0u; flags < 64u; ++flags) {
    for (unsigned next = 0u; next < 3u; ++next)
      for (unsigned published = 0u; published < 3u; ++published)
        for (unsigned completed = 0u; completed < 3u; ++completed)
          for (unsigned initPublished = 0u; initPublished < 3u; ++initPublished)
            for (unsigned initCompleted = 0u; initCompleted < 3u; ++initCompleted)
              for (unsigned inflight = 0u; inflight < 2u; ++inflight) {
                const RenderTapeCheckpointCompletionState state{
                    .nextSequence = next, .publishedSequence = published,
                    .completedSequence = completed,
                    .initializerPublished = initPublished,
                    .initializerCompleted = initCompleted,
                    .inflightCount = inflight,
                    .buildQuiescent = bool(flags & 1u),
                    .writingEmpty = bool(flags & 2u),
                    .initializerPending = bool(flags & 4u),
                    .initializerTrackingAvailable = bool(flags & 8u),
                    .initializerFailed = bool(flags & 16u),
                    .terminal = bool(flags & 32u),
                };
                const bool expected = (flags & 3u) == 3u && (flags & 52u) == 0u &&
                    next > 0u && published + 1u == next && completed >= published &&
                    inflight == 0u && initCompleted >= initPublished &&
                    (initPublished == 0u || (flags & 8u));
                check(renderTapeCheckpointCompletionAllowed(state) == expected,
                      "checkpoint completion rejects early transfer, reopened writer, and signal-only receipts");
                ++checked;
              }
  }
  std::cout << "D24S8 checkpoint completion: shared exhaustive states=" << checked
            << "; build flags=32\n";
  // Bounded distinguishing traces: admission -> flush unlock/relock ->
  // initializer publish/settle -> exclusive transfer. These bind admission to
  // the same production predicates; concrete aspect bytes remain GPU oracles.
  std::size_t traces = 0u;
  std::size_t omittedRecheckCounterexamples = 0u;
  const auto buildsIdle = [](unsigned flags) {
    return renderTapeBackbufferSeedBuildQuiescent(
        flags & 1u, flags & 2u, flags & 4u, flags & 8u, flags & 16u);
  };
  for (unsigned beforeDrain = 0u; beforeDrain < 32u; ++beforeDrain)
    for (unsigned duringDrain = 0u; duringDrain < 32u; ++duringDrain)
      for (unsigned ordinaryDone = 0u; ordinaryDone < 2u; ++ordinaryDone)
        for (unsigned uploadOutcome = 0u; uploadOutcome < 5u; ++uploadOutcome)
          for (unsigned terminal = 0u; terminal < 2u; ++terminal) {
            const bool initialGuard = buildsIdle(beforeDrain);
            const bool postDrainGuard = buildsIdle(duringDrain);
            // Upload outcomes: none, pending, completed, failed (even if its
            // signal fired), and completed signal-only without success owner.
            RenderTapeCheckpointCompletionState settled{
                .nextSequence = 2u, .publishedSequence = 1u,
                .completedSequence = ordinaryDone,
                .initializerPublished = uploadOutcome == 0u ? 0u : 1u,
                .initializerCompleted = uploadOutcome >= 2u ? 1u : 0u,
                .inflightCount = ordinaryDone ? 0u : 1u,
                .buildQuiescent = postDrainGuard,
                .writingEmpty = true,
                .initializerPending = uploadOutcome == 1u,
                .initializerTrackingAvailable = uploadOutcome != 4u,
                .initializerFailed = uploadOutcome == 3u,
                .terminal = bool(terminal),
            };
            const bool leased = initialGuard && postDrainGuard &&
                renderTapeCheckpointCompletionAllowed(settled);
            const bool expected = beforeDrain == 0u && duringDrain == 0u &&
                ordinaryDone && (uploadOutcome == 0u || uploadOutcome == 2u) && !terminal;
            check(leased == expected,
                  "ordered checkpoint trace rejects builder reopening and pending/failed/signal-only initializers");
            // A mutant that omits the post-wait borrowed-builder observation
            // admits a real distinguishing reentry trace; keep that witness.
            settled.buildQuiescent = true;
            if (initialGuard && !postDrainGuard && renderTapeCheckpointCompletionAllowed(settled))
              ++omittedRecheckCounterexamples;
            ++traces;
          }
  check(omittedRecheckCounterexamples != 0u,
        "bounded transition checker distinguishes a missing post-drain producer guard");
  std::cout << "D24S8 checkpoint ordering: bounded traces=" << traces
            << "; missing recheck counterexamples=" << omittedRecheckCounterexamples
            << "; initializer pending/failure/signal-only and early transfer rejected\n";
}

void productionCaptureAndRestore() {
  using namespace dxmt9;
  using namespace dxmt9::d3d9;
  const D9CPresentParams params{.backBufferWidth = kWidth, .backBufferHeight = kHeight,
      .backBufferFormat = 21u, .backBufferCount = 1u, .swapEffect = 1u,
      .windowed = 1u, .presentationInterval = 0x80000000u};
  auto* factory = dxmt9c_factory_create();
  check(factory != nullptr, "production factory creates");
  auto* device = dxmt9c_factory_create_device(factory, 0u, &params, 0u, nullptr);
  check(device != nullptr, "production checkpoint device creates");
  auto* source = dxmt9c_device_create_depth_stencil(device, kWidth, kHeight,
      75u, 0u, 0u, 0u, nullptr);
  auto* destination = dxmt9c_device_create_depth_stencil(device, kWidth, kHeight,
      75u, 0u, 0u, 0u, nullptr);
  check(source && destination, "production standalone D24S8 wrappers create");
  auto upper = device->dev().upperDevice();
  check(upper && upper->pool(), "production checkpoint has real queue and pool");
  auto& queue = upper->queue();
  const auto backing = [&](D9CSurface* wrapped) {
    std::lock_guard lock(queue.mutex_);
    auto* record = upper->pool()->findSurface(wrapped->obj->handle().value);
    check(record && record->texture, "generation-resolved production backing exists");
    return record->texture;
  };
  const auto sourceTexture = backing(source);
  const auto destinationTexture = backing(destination);
  id<MTLTexture> nativeSource = (__bridge id<MTLTexture>)(void*)sourceTexture.handle;
  id<MTLTexture> nativeDestination = (__bridge id<MTLTexture>)(void*)destinationTexture.handle;
  id<MTLCommandQueue> nativeQueue = (__bridge id<MTLCommandQueue>)(void*)queue.raw().handle;
  id<MTLDevice> nativeDevice = nativeSource.device;
  auto shaders = library(nativeDevice);
  auto producer = pipeline(nativeDevice, shaders, true);
  auto oracle = pipeline(nativeDevice, shaders, false);
  D9CRenderTapeD24S8SnapshotRequest request{};
  check(dxmt9c_surface_get_wire_identity(source, &request.identity) == core::D3D_OK &&
        dxmt9c_surface_get_desc(source, &request.surface) == core::D3D_OK,
        "production snapshot identity and descriptor come from live wrapper");
  request.encodingVersion = D9C_RENDER_TAPE_D24S8_ENCODING_FLOAT32_LE_STENCIL_UINT8_V1;
  std::vector<std::uint8_t> bytes(kCheckpointBytes, 0xccu);
  D9CRenderTapeD24S8SnapshotResult result{};
  // No manual flush: the checkpoint must publish and finish this queued clear.
  check(dxmt9c_device_set_depth_stencil(device, source) == core::D3D_OK &&
        dxmt9c_device_clear(device, 0u, nullptr, 6u, 0u, 0.375f, 0x5au) == core::D3D_OK,
        "production depth+stencil work is queued before checkpoint");
  check(dxmt9c_device_capture_render_tape_d24s8_snapshot(device, &request, &result,
            bytes.data(), bytes.size()) == core::D3D_OK &&
        renderTapeD24S8SnapshotResultMatches(request, result, bytes.size()),
        "actual bridge returns lawful complete5N receipt after queue drain");
  Pattern clear{};
  clear.depth.fill(0.375f);
  clear.stencil.fill(0x5au);
  check(bytes == clear.checkpoint(), "queued production clear completes in both captured aspects");
  for (const auto& values : {pattern(false), pattern(true), pattern(false)}) {
    initialize(nativeDevice, nativeQueue, nativeSource, producer, values);
    check(dxmt9c_device_capture_render_tape_d24s8_snapshot(device, &request, &result,
              bytes.data(), bytes.size()) == core::D3D_OK &&
          renderTapeD24S8SnapshotResultMatches(request, result, bytes.size()) &&
          bytes == values.checkpoint(), "production bridge preserves every independently produced5N byte");
    core::CanonicalD24S8DepthStencil checkpoint;
    check(upper->captureCanonicalD24S8DepthStencil(source->obj->handle(), checkpoint) &&
          checkpoint.bytes == bytes &&
          upper->seedCanonicalD24S8DepthStencil(destination->obj->handle(), checkpoint),
          "production inverse helper restores exact A-B-A checkpoints");
    pixelOracle(nativeDevice, nativeQueue, nativeDestination, oracle, values, true, false);
    pixelOracle(nativeDevice, nativeQueue, nativeDestination, oracle, values, false, true);
    pixelOracle(nativeDevice, nativeQueue, nativeDestination, oracle, values, true, true);
    core::CanonicalD24S8DepthStencil recaptured;
    check(upper->captureCanonicalD24S8DepthStencil(destination->obj->handle(), recaptured) &&
          recaptured.bytes == checkpoint.bytes, "production seed retains both exact aspects after pixel tests");
    auto malformed = checkpoint;
    malformed.stencilOffsetBytes += 1u;
    check(!upper->seedCanonicalD24S8DepthStencil(destination->obj->handle(), malformed) &&
          capture(nativeDevice, nativeQueue, nativeDestination) == bytes,
          "malformed plane layout rejects before changing either aspect");
  }
  auto stale = request;
  ++stale.identity.generation;
  bytes.assign(kCheckpointBytes, 0xccu);
  const auto sentinel = bytes;
  check(dxmt9c_device_capture_render_tape_d24s8_snapshot(device, &stale, &result,
            bytes.data(), bytes.size()) != core::D3D_OK &&
        result.status == D9C_RENDER_TAPE_D24S8_SNAPSHOT_STALE_GENERATION && bytes == sentinel,
        "stale generation rejects without overwriting caller memory");
  auto lease = queue.beginRenderTapeCheckpoint();
  check(lease && lease->owns_lock(), "drained production checkpoint owns an exclusive producer lease");
  bool reopened = false;
  std::thread probe([&] {
    reopened = queue.mutex_.try_lock();
    if (reopened) queue.mutex_.unlock();
  });
  probe.join();
  check(!reopened, "producer cannot reopen between drain receipt and completed transfer");
  lease.reset();
  auto noStencil = texture(nativeDevice, MTLPixelFormatDepth32Float, @"Rejected production stencil-less backing");
  {
    std::lock_guard lock(queue.mutex_);
    auto* record = upper->pool()->findSurface(source->obj->handle().value);
    record->texture = WMT::Texture{reinterpret_cast<obj_handle_t>((__bridge void*)noStencil)};
  }
  core::CanonicalD24S8DepthStencil rejected;
  check(!upper->captureCanonicalD24S8DepthStencil(source->obj->handle(), rejected) && rejected.bytes.empty(),
        "actual production helper rejects D32Float-without-stencil instead of fabricating aspect bytes");
  {
    std::lock_guard lock(queue.mutex_);
    auto* record = upper->pool()->findSurface(source->obj->handle().value);
    record->texture = sourceTexture;
    record->desc.multiSampleType = core::MultiSampleType::Two;
  }
  check(!upper->captureCanonicalD24S8DepthStencil(source->obj->handle(), rejected),
        "production helper rejects logical multisampling before either aspect transfer");
  {
    std::lock_guard lock(queue.mutex_);
    upper->pool()->findSurface(source->obj->handle().value)->desc.multiSampleType = core::MultiSampleType::None;
  }
  check(dxmt9c_device_set_depth_stencil(device, nullptr) == core::D3D_OK,
        "release production checkpoint depth binding");
  dxmt9c_surface_release(destination);
  dxmt9c_surface_release(source);
  dxmt9c_device_release(device);
  dxmt9c_factory_release(factory);
  std::cout << "D24S8 production checkpoint: bridge A-B-A=3; aspect pixel oracles=9; "
            << "queued two-aspect drain=1; stale generation/layout/exclusive lease verified\n";
}

} // namespace

int main() {
  @autoreleasepool {
    try {
      run();
      checkpointCompletionExhaustive();
      productionCaptureAndRestore();
    } catch (const std::exception& error) {
      std::cerr << "D24S8 checkpoint capability failed: " << error.what() << '\n';
      return 1;
    }
  }
  return 0;
}
