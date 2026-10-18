#include "../../../src/dxmt9/observations/gpu_source.hpp"
#include "../../../src/dxmt9/observations/bounded_records.hpp"
#include "../../../src/dxmt9/dxmt9_trace_gpu.hpp"
#include "../../../src/winemetal/Metal.hpp"
#import <Metal/Metal.h>
#include <array>
#include <atomic>
#include <cassert>
#include <thread>
#include <vector>
#include <iostream>

static std::atomic<unsigned> liveSources{0};

@interface CountingGpuSource : NSObject {
@public
  std::atomic<unsigned> startReads, endReads, resolves;
  bool missing;
}
- (double)GPUStartTime;
- (double)GPUEndTime;
- (NSUInteger)sampleCount;
- (NSData*)resolveCounterRange:(NSRange)range;
@end
@implementation CountingGpuSource
- (id)init { self = [super init]; if (self) ++liveSources; return self; }
- (void)dealloc { --liveSources; [super dealloc]; }
- (double)GPUStartTime { ++startReads; return 1.0; }
- (double)GPUEndTime { ++endReads; return 2.0; }
- (NSUInteger)sampleCount { return 4; }
- (NSData*)resolveCounterRange:(NSRange)range {
  ++resolves;
  assert(range.length == 2 && range.location < 4);
  if (missing) return nil;
  std::uint64_t values[] = {100 + range.location, 200 + range.location};
  return [NSData dataWithBytes:values length:sizeof(values)];
}
@end

void concurrentSources() {
  using namespace dxmt9::observations;
  auto* cb = [[CountingGpuSource alloc] init];
  const auto cbHandle = (obj_handle_t)cb;
  // Legacy-only acquisition has no cache installation or changed result.
  assert(commandBufferEndpoints(cbHandle).valid());
  assert(cb->startReads == 1 && cb->endReads == 1);
  assert(prepareCommandBufferSource(cbHandle));
  auto* counter = [[CountingGpuSource alloc] init];
  const auto counterHandle = (obj_handle_t)counter;
  assert(!hasCounterSource(counterHandle));
  assert(prepareCounterSource(counterHandle));
  std::vector<std::thread> consumers;
  for (int i = 0; i != 8; ++i) consumers.emplace_back([&] {
    @autoreleasepool {
      for (int j = 0; j != 20; ++j) {
        const auto time = commandBufferEndpoints(cbHandle);
        assert(time.begin == 1000000000 && time.end == 2000000000);
        const auto pair = counterPairEndpoints(counterHandle, 0);
        assert(pair.begin == 100 && pair.end == 200);
      }
    }
  });
  for (auto& consumer : consumers) consumer.join();
  assert(cb->startReads == 2 && cb->endReads == 2);
  assert(counter->resolves == 1);
  assert(!counterPairEndpoints(counterHandle, 1).valid());
  assert(!counterPairEndpoints(counterHandle, 4).valid());
  counter->missing = true;
  assert(!counterPairEndpoints(counterHandle, 2).valid());
  counter->missing = false;
  assert(!counterPairEndpoints(counterHandle, 2).valid());
  assert(counter->resolves == 2); // Missing is immutable, never silently retried.
  [cb release];
  [counter release];
  auto* fresh = [[CountingGpuSource alloc] init];
  assert(prepareCounterSource((obj_handle_t)fresh));
  assert(counterPairEndpoints((obj_handle_t)fresh, 2).valid());
  assert(fresh->resolves == 1); // A new object never inherits cached evidence.
  [fresh release];
  assert(liveSources == 0);
}

void boundedPublication() {
  using namespace dxmt9::observations;
  BoundedRecords<std::array<std::uint64_t, 2>, 32> records;
  assert(records.allocate());
  const auto unpublished = records.reserve();
  records.writable(unpublished) = {42, 84};
  assert(!records.read(unpublished));
  std::vector<std::thread> writers;
  for (unsigned i = 0; i != 40; ++i) writers.emplace_back([&, i] {
    const auto index = records.reserve();
    if (index >= 32) return;
    records.writable(index) = {i, i * 2};
    records.publish(index);
  });
  for (auto& writer : writers) writer.join();
  assert(records.size() == 41 && !records.read(32));
  assert(!records.read(unpublished));
  records.publish(unpublished);
  for (unsigned i = 0; i != 32; ++i) {
    const auto* row = records.read(i);
    assert(row && (*row)[1] == (*row)[0] * 2);
  }
}

void metalSharedStage() {
  using namespace dxmt9::trace::gpu;
  using namespace dxmt9::observations;
  id<MTLDevice> metal = MTLCreateSystemDefaultDevice();
  assert(metal);
  WMT::Device device{(obj_handle_t)metal};
  if (!device.supportsCounterSampling(WMTCounterSamplingPointAtStageBoundary)) {
    std::cout << "stage sampling unsupported\n";
    [metal release];
    return;
  }
  // Trace-only admission still owns an independent buffer.
  StageSampleBuffer traceOnly;
  assert(traceOnly.start(device, 100, 2));
  const WorkIdentity work{1, 2, 3, 4};
  auto own = traceOnly.reserve(work);
  assert(own);
  traceOnly.close();
  StagePairResult failed{};
  assert(traceOnly.resolve(own.ticket, {work, CommandBufferStatus::Failed, true}, failed));
  assert(failed.validity == ResultValidity::CommandBufferFailed && traceOnly.canRetire());

  // Reference ownership is checked with a destructible fake, independently of
  // Metal's own command-buffer retention of resources after completion.
  StageSampleBuffer retained;
  assert(retained.start(device, 99, 1));
  auto* owner = [[CountingGpuSource alloc] init];
  WMTSampleBufferAttachmentInfo fakeAttachment{(obj_handle_t)owner, 0, 1};
  const auto retainedTicket = retained.reserve(work, &fakeAttachment);
  assert(retainedTicket);
  [owner release];
  retained.close();
  assert(liveSources == 1 && !retained.canRetire());
  auto wrongWork = work;
  ++wrongWork.deviceGeneration;
  assert(!retained.resolve(retainedTicket.ticket, {wrongWork, CommandBufferStatus::Failed, true}, failed));
  assert(liveSources == 1);
  assert(retained.resolve(retainedTicket.ticket, {work, CommandBufferStatus::Failed, true}, failed));
  assert(liveSources == 0 && retained.canRetire());

  StageSampleBuffer trace;
  assert(trace.start(device, 101, 2));
  auto legacy = device.newCounterSampleBuffer(4, true);
  assert(legacy);
  WMTSampleBufferAttachmentInfo outOfRange{legacy.handle, 4, 5};
  assert(!trace.reserve(work, &outOfRange));
  assert(trace.counters().lostCapacity == 1);
  WMTSampleBufferAttachmentInfo attachment{legacy.handle, 0, 1};
  auto shared = trace.reserve(work, &attachment);
  assert(shared && shared.attachment.sample_buffer == legacy.handle);
  assert(shared.attachment.start_of_encoder_sample_index == 0);
  assert(shared.attachment.end_of_encoder_sample_index == 1);
  id<MTLCommandQueue> queue = [metal newCommandQueue];
  id<MTLCommandBuffer> cb = [queue commandBuffer];
  auto* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
      width:16 height:16 mipmapped:NO];
  td.usage = MTLTextureUsageRenderTarget;
  id<MTLTexture> texture = [metal newTextureWithDescriptor:td];
  auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = texture;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.sampleBufferAttachments[0].sampleBuffer = (id<MTLCounterSampleBuffer>)legacy.handle;
  pass.sampleBufferAttachments[0].startOfVertexSampleIndex = 0;
  pass.sampleBufferAttachments[0].endOfVertexSampleIndex = MTLCounterDontSample;
  pass.sampleBufferAttachments[0].startOfFragmentSampleIndex = MTLCounterDontSample;
  pass.sampleBufferAttachments[0].endOfFragmentSampleIndex = 1;
  id<MTLRenderCommandEncoder> encoder = [cb renderCommandEncoderWithDescriptor:pass];
  assert(encoder);
  NSError* error = nil;
  id<MTLLibrary> library = [metal newLibraryWithSource:
      @"#include <metal_stdlib>\nusing namespace metal;\n"
       "vertex float4 v(uint i [[vertex_id]]) { float2 p[3] = {float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],0,1); }\n"
       "fragment float4 f() { return float4(1,0,0,1); }"
      options:nil error:&error];
  assert(library && !error);
  auto* pipelineDesc = [[MTLRenderPipelineDescriptor alloc] init];
  pipelineDesc.vertexFunction = [library newFunctionWithName:@"v"];
  pipelineDesc.fragmentFunction = [library newFunctionWithName:@"f"];
  pipelineDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
  id<MTLRenderPipelineState> pipeline = [metal newRenderPipelineStateWithDescriptor:pipelineDesc error:&error];
  assert(pipeline && !error);
  [encoder setRenderPipelineState:pipeline];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [encoder endEncoding];
  [pipelineDesc.vertexFunction release];
  [pipelineDesc.fragmentFunction release];
  [pipelineDesc release];
  [pipeline release];
  [library release];
  trace.close();
  assert(!trace.canRetire() && !trace.reserve(work));
  StagePairResult result{};
  assert(!trace.resolve(shared.ticket, {work, CommandBufferStatus::Completed, false}, result));
  assert(prepareCommandBufferSource((obj_handle_t)cb));
  [cb commit];
  [cb waitUntilCompleted]; // Test driver only; production never waits for profiling.
  assert(cb.status == MTLCommandBufferStatusCompleted);
  const auto legacyResult = counterPairEndpoints(legacy.handle, 0);
  assert(legacyResult.valid());
  // Drop the legacy owner's reference; the trace ticket must retain the source.
  pass.sampleBufferAttachments[0].sampleBuffer = nil;
  legacy = nullptr;
  assert(trace.resolve(shared.ticket, {work, CommandBufferStatus::Completed, true}, result));
  assert(result.validity == ResultValidity::Valid);
  assert(result.startTimestamp == legacyResult.begin && result.endTimestamp == legacyResult.end);
  assert(trace.canRetire());
  assert(!trace.resolve(shared.ticket, {work, CommandBufferStatus::Completed, true}, result));
  assert(trace.start(device, 102, 2));
  assert(!trace.resolve(shared.ticket, {work, CommandBufferStatus::Completed, true}, result));
  trace.close();
  assert(commandBufferEndpoints((obj_handle_t)cb).valid());
  [texture release];
  [queue release];
  [metal release];
}

int main() {
  @autoreleasepool {
    concurrentSources();
    boundedPublication();
    metalSharedStage();
  }
  std::cout << "shared observation sources passed\n";
}
