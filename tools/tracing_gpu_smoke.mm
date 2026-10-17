#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <mach/mach_time.h>
#import <CommonCrypto/CommonDigest.h>

#include "../src/dxmt9/dxmt9_trace.hpp"
#include "../src/dxmt9/dxmt9_trace_gpu_bridge.hpp"
#include "../src/winemetal/Metal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace gpu = dxmt9::trace::gpu;

static std::uint64_t toNanoseconds(std::uint64_t ticks) {
  mach_timebase_info_data_t timebase{};
  if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.denom == 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(
      static_cast<long double>(ticks) * timebase.numer / timebase.denom);
}

static std::uint64_t threadCpuNanoseconds() {
  timespec value{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) return 0;
  return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ull +
      static_cast<std::uint64_t>(value.tv_nsec);
}

static std::uint64_t hashPixel(id<MTLTexture> texture) {
  std::array<std::uint8_t, 64 * 64 * 4> pixels{};
  [texture getBytes:pixels.data() bytesPerRow:64 * 4
         fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
  std::uint64_t hash = 1469598103934665603ull;
  for (std::uint8_t byte : pixels) hash = (hash ^ byte) * 1099511628211ull;
  return hash;
}

static id<MTLRenderPipelineState> makeSmokePipeline(id<MTLDevice> device) {
  static NSString *source = @R"msl(
    #include <metal_stdlib>
    using namespace metal;
    vertex float4 smoke_vs(uint vertexId [[vertex_id]]) {
      constexpr float2 positions[3] = {
        float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)
      };
      return float4(positions[vertexId], 0.0, 1.0);
    }
    fragment float4 smoke_fs() {
      return float4(0.4, 0.5, 0.6, 1.0);
    }
  )msl";
  NSError *error = nil;
  id<MTLLibrary> library = [device newLibraryWithSource:source
                                                options:nil error:&error];
  if (!library) return nil;
  MTLRenderPipelineDescriptor *descriptor =
      [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.vertexFunction = [library newFunctionWithName:@"smoke_vs"];
  descriptor.fragmentFunction = [library newFunctionWithName:@"smoke_fs"];
  descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  id<MTLRenderPipelineState> pipeline =
      [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
  [descriptor release];
  [library release];
  return pipeline;
}

static std::string sha256(const std::string& text) {
  unsigned char digest[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256(text.data(), static_cast<CC_LONG>(text.size()), digest);
  static constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(sizeof(digest) * 2);
  for (unsigned char byte : digest) {
    result.push_back(hex[byte >> 4]);
    result.push_back(hex[byte & 15]);
  }
  return result;
}

static std::string jsonString(const std::string& value) {
  std::string result{"\""};
  for (const char byte : value) {
    if (byte == '"' || byte == '\\') result.push_back('\\');
    result.push_back(byte);
  }
  result.push_back('"');
  return result;
}

static std::string jsonObjectStringField(const std::string& object,
                                         const std::string& field) {
  const std::string marker = "\"" + field + "\"";
  const auto key = object.find(marker);
  if (key == std::string::npos) return {};
  const auto colon = object.find(':', key + marker.size());
  if (colon == std::string::npos) return {};
  const auto valueBegin = object.find_first_not_of(" \t\r\n", colon + 1);
  if (valueBegin == std::string::npos || object[valueBegin] != '"') return {};
  const auto valueEnd = object.find('"', valueBegin + 1);
  if (valueEnd == std::string::npos) return {};
  return object.substr(valueBegin + 1, valueEnd - valueBegin - 1);
}

static bool jsonObjectFieldIsTrue(const std::string& object,
                                  const std::string& field) {
  const std::string marker = "\"" + field + "\"";
  const auto key = object.find(marker);
  if (key == std::string::npos) return false;
  const auto colon = object.find(':', key + marker.size());
  if (colon == std::string::npos) return false;
  const auto valueBegin = object.find_first_not_of(" \t\r\n", colon + 1);
  if (valueBegin == std::string::npos ||
      object.compare(valueBegin, 4, "true") != 0) return false;
  const auto valueEnd = valueBegin + 4;
  return valueEnd == object.size() || object[valueEnd] == ',' ||
      object[valueEnd] == '}' || object[valueEnd] == ' ' ||
      object[valueEnd] == '\t' || object[valueEnd] == '\r' ||
      object[valueEnd] == '\n';
}

static bool hasSubmittedGpuRelation(const std::string& raw,
                                    const std::string& submitRecord,
                                    const std::string& gpuRecord) {
  if (submitRecord.empty() || gpuRecord.empty()) return false;
  const auto relationsKey = raw.find("\"relations\"");
  if (relationsKey == std::string::npos) return false;
  const auto arrayBegin = raw.find('[', relationsKey);
  const auto arrayEnd = raw.find(']', arrayBegin);
  if (arrayBegin == std::string::npos || arrayEnd == std::string::npos)
    return false;
  for (std::size_t position = arrayBegin + 1; position < arrayEnd;) {
    const auto objectBegin = raw.find('{', position);
    if (objectBegin == std::string::npos || objectBegin >= arrayEnd) break;
    const auto objectEnd = raw.find('}', objectBegin + 1);
    if (objectEnd == std::string::npos || objectEnd > arrayEnd) break;
    const std::string object = raw.substr(
        objectBegin, objectEnd - objectBegin + 1);
    if (jsonObjectStringField(object, "source_event") == submitRecord &&
        jsonObjectStringField(object, "target_event") == gpuRecord &&
        jsonObjectStringField(object, "type") == "submitted_in" &&
        jsonObjectStringField(object, "evidence") == "observed_binding" &&
        jsonObjectFieldIsTrue(object, "flow")) {
      return true;
    }
    position = objectEnd + 1;
  }
  return false;
}

static bool runIntegratedCapture(id<MTLDevice> device,
                                 id<MTLCommandQueue> queue,
                                 id<MTLRenderPipelineState> pipeline,
                                 id<MTLTexture> color) {
  namespace tracing = dxmt9::tracing;
  namespace fs = std::filesystem;
  const fs::path root = fs::current_path() /
      "experiments/output/tracing-acceptance" /
      ("integrated-metal-smoke-" + std::to_string(getpid()));
  fs::create_directories(root);
  const fs::path requestPath = root / "request.json";
  const fs::path receiptPath = root / "receipt.json";
  const fs::path rawPath = root / "trace.pftrace.raw.jsonl";
  const fs::path pftracePath = root / "trace.pftrace";
  const std::string runId = "integrated-metal-smoke-" +
      std::to_string(getpid());

  // Canonical request body with the qualification policy frozen at 1 us.
  const std::string canonical =
      "{\"attempt_id\":" + jsonString(runId) +
      ",\"categories\":[\"submit\",\"gpu\"]"
      ",\"gpu_timing\":{\"calibration\":true,\"capacity\":8,"
      "\"level\":\"command-buffer\",\"required\":true}"
      ",\"limits\":{\"finalize_timeout_ms\":10000,"
      "\"max_bytes\":1048576,\"max_events\":4096}"
      ",\"optional_collectors\":[]"
      ",\"output\":{\"pftrace\":" + jsonString(pftracePath.string()) +
      ",\"receipt\":" + jsonString(receiptPath.string()) + "}"
      ",\"profile_id\":\"native-gpu-smoke\""
      ",\"qualification_policy\":{\"max_clock_error_ns\":1000}"
      ",\"question\":\"integrated-native-gpu-capture\""
      ",\"required_categories\":[\"submit\",\"gpu\"]"
      ",\"required_collectors\":[\"engine-perfetto\"]"
      ",\"run_id\":" + jsonString(runId) +
      ",\"schema\":\"dxmt9.trace.request.v1\""
      ",\"window\":{\"delay_ms\":0,\"duration_ms\":30000,"
      "\"mode\":\"duration\"}}";
  const std::string request = canonical.substr(0, canonical.size() - 1) +
      ",\"config_sha256\":" + jsonString(sha256(canonical)) + "}";
  {
    std::ofstream output(requestPath, std::ios::binary);
    output << request;
    if (!output.good()) {
      std::puts("integrated_capture=invalid reason=request_write_failed");
      return false;
    }
  }
  unsetenv("DXMT9_TRACE_CONFIG");
  if (setenv("DXMT9_TRACE_CONFIG", requestPath.c_str(), 1) != 0 ||
      !tracing::initialize_from_environment()) {
    std::puts("integrated_capture=unsupported reason=trace_facade_unavailable");
    return false;
  }
  bool becameEnabled = false;
  for (unsigned attempt = 0; attempt < 5000; ++attempt) {
    if (tracing::enabled()) {
      becameEnabled = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!becameEnabled || !tracing::attach_metal_device(
          reinterpret_cast<std::uintptr_t>(device))) {
    std::puts("integrated_capture=invalid reason=enable_or_gpu_attach_failed");
    (void)tracing::stop_and_finalize();
    return false;
  }

  id<MTLCommandQueue> secondQueue = [device newCommandQueue];
  if (!secondQueue || secondQueue == queue) {
    if (secondQueue) [secondQueue release];
    std::puts("integrated_capture=unsupported reason=second_metal_queue_unavailable");
    (void)tracing::stop_and_finalize();
    return false;
  }
  std::array<std::uint64_t, 2> commandBufferIds{};
  std::array<std::uintptr_t, 2> queueIdentities{};
  bool submitObserved = true;
  bool commandBuffersSucceeded = true;
  bool completionsPublished = true;
  bool probeValid = true;
  const std::array<id<MTLCommandQueue>, 2> queues{queue, secondQueue};
  for (std::size_t index = 0; index < queues.size(); ++index) {
    id<MTLCommandQueue> probeQueue = queues[index];
    id<MTLCommandBuffer> commandBuffer = [probeQueue commandBuffer];
    if (!commandBuffer) {
      std::puts("integrated_capture=invalid reason=no_command_buffer");
      probeValid = false;
      break;
    }
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor
        renderPassDescriptor];
    pass.colorAttachments[0].texture = color;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor =
        MTLClearColorMake(0.1, 0.2, 0.3, 1.0);
    id<MTLRenderCommandEncoder> encoder =
        [commandBuffer renderCommandEncoderWithDescriptor:pass];
    if (!encoder) {
      std::puts("integrated_capture=invalid reason=no_render_encoder");
      probeValid = false;
      break;
    }
    [encoder setRenderPipelineState:pipeline];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0
                 vertexCount:3];
    [encoder endEncoding];

    const std::uintptr_t queueIdentity =
        reinterpret_cast<std::uintptr_t>(probeQueue);
    const std::uint64_t sequenceId = index + 1;
    const std::uintptr_t commandBufferHandle =
        reinterpret_cast<std::uintptr_t>(commandBuffer);
    const std::uint64_t commandBufferId = gpu::processCommandBufferId(
        commandBufferHandle, queueIdentity, sequenceId);
    const std::uint64_t deviceGeneration =
        gpu::processCommandBufferDeviceGeneration(commandBufferHandle);
    if (!commandBufferId || !deviceGeneration ||
        !gpu::setProcessCommandBufferDeviceGeneration(
            commandBufferHandle, queueIdentity, sequenceId, deviceGeneration)) {
      std::puts("integrated_capture=invalid reason=work_identity_failed");
      probeValid = false;
      break;
    }
    const gpu::WorkIdentity work{
        .deviceGeneration = deviceGeneration,
        .queueGeneration = queueIdentity,
        .commandBufferId = commandBufferId,
        .sequenceId = sequenceId,
    };
    if (gpu::observeProcessGpuSubmission(work, commandBufferHandle) !=
        commandBufferId) {
      std::puts("integrated_capture=invalid reason=production_bridge_admission_failed");
      probeValid = false;
      break;
    }
    [commandBuffer commit];
    const auto emitSubmit = [&](tracing::EventId event) {
      tracing::TraceEvent value{};
      value.timestamp_ns = tracing::now_ns();
      value.work_id = commandBufferId;
      value.arg0 = queueIdentity;
      value.related_owner_id = queueIdentity;
      value.device_generation = deviceGeneration;
      value.event_id = static_cast<std::uint16_t>(event);
      value.category = static_cast<std::uint8_t>(tracing::Category::Submit);
      value.phase = static_cast<std::uint8_t>(tracing::Phase::Instant);
      return tracing::emit(value);
    };
    const bool submitBeginEmitted = emitSubmit(tracing::EventId::SubmitBegin);
    const bool submitEmitted = emitSubmit(tracing::EventId::Submit);
    submitObserved = submitBeginEmitted && submitEmitted && submitObserved;
    [commandBuffer waitUntilCompleted];
    const bool commandBufferSucceeded =
        commandBuffer.status != MTLCommandBufferStatusError;
    commandBuffersSucceeded = commandBufferSucceeded &&
        commandBuffersSucceeded;
    completionsPublished = gpu::publishCompletedProcessGpuCommandBuffer(
        commandBufferHandle, queueIdentity, sequenceId,
        commandBufferSucceeded ? gpu::CommandBufferStatus::Completed
                               : gpu::CommandBufferStatus::Failed) &&
        completionsPublished;
    gpu::retireProcessCommandBufferIdsThrough(queueIdentity, sequenceId);
    commandBufferIds[index] = commandBufferId;
    queueIdentities[index] = queueIdentity;
  }
  [secondQueue release];
  const bool finalized = tracing::stop_and_finalize();

  std::ifstream rawFile(rawPath, std::ios::binary);
  std::ifstream receiptFile(receiptPath, std::ios::binary);
  std::ifstream traceFile(pftracePath, std::ios::binary | std::ios::ate);
  const std::string raw{std::istreambuf_iterator<char>(rawFile), {}};
  const std::string receipt{std::istreambuf_iterator<char>(receiptFile), {}};
  const auto stringField = [](const std::string& line,
                              const std::string& field) {
    const std::string marker = "\"" + field + "\":\"";
    const auto start = line.find(marker);
    if (start == std::string::npos) return std::string{};
    const auto valueStart = start + marker.size();
    const auto end = line.find('"', valueStart);
    return end == std::string::npos
        ? std::string{} : line.substr(valueStart, end - valueStart);
  };
  std::vector<std::string> gpuLines;
  std::vector<std::string> submitLines;
  for (std::size_t position = 0; position < raw.size();) {
    const std::size_t end = raw.find('\n', position);
    const std::string line = raw.substr(position, end == std::string::npos
        ? std::string::npos : end - position);
    if (line.find("\"name\":\"gpu_command_buffer_begin\"") !=
        std::string::npos) gpuLines.push_back(line);
    if (line.find("\"name\":\"submit_begin\"") != std::string::npos)
      submitLines.push_back(line);
    if (end == std::string::npos) break;
    position = end + 1;
  }
  std::array<std::string, 2> gpuTrackIds{};
  bool gpuSamplesBound = true;
  bool gpuClockCalibrated = true;
  bool submitBindingsObserved = true;
  bool submitGpuFlowsObserved = true;
  for (std::size_t index = 0; index < commandBufferIds.size(); ++index) {
    const std::string cbId = std::to_string(commandBufferIds[index]);
    const std::string queueId = std::to_string(queueIdentities[index]);
    const std::string* gpuLine = nullptr;
    const std::string* submitLine = nullptr;
    for (const std::string& line : gpuLines) {
      if (line.find("\"arg0\":\"" + cbId + "\"") !=
              std::string::npos &&
          line.find("\"relation_queue_identity\":\"" + queueId + "\"") !=
              std::string::npos) gpuLine = &line;
    }
    for (const std::string& line : submitLines) {
      if (line.find("\"work_ids\":[\"" + cbId + "\"]") !=
              std::string::npos &&
          line.find("\"relation_queue_identity\":\"" + queueId + "\"") !=
              std::string::npos) submitLine = &line;
    }
    gpuSamplesBound = gpuSamplesBound && gpuLine && submitLine &&
        gpuLine->find("\"calibration_id\":\"0\"") ==
            std::string::npos;
    gpuClockCalibrated = gpuClockCalibrated && gpuLine &&
        gpuLine->find("\"source_clock_id\":\"" +
                      std::to_string(gpu::kMachAbsoluteTimeClockId) + "\"") !=
            std::string::npos &&
        gpuLine->find("\"calibration_id\":\"0\"") ==
            std::string::npos;
    submitBindingsObserved = submitBindingsObserved && submitLine;
    if (gpuLine && submitLine) {
      const std::string submitRecord = stringField(*submitLine, "event_id");
      const std::string gpuRecord = stringField(*gpuLine, "event_id");
      submitGpuFlowsObserved = submitGpuFlowsObserved &&
          !submitRecord.empty() && !gpuRecord.empty() &&
          hasSubmittedGpuRelation(raw, submitRecord, gpuRecord);
      gpuTrackIds[index] = stringField(*gpuLine, "track_id");
    } else {
      submitGpuFlowsObserved = false;
    }
  }
  const bool separateGpuTracks = !gpuTrackIds[0].empty() &&
      !gpuTrackIds[1].empty() && gpuTrackIds[0] != gpuTrackIds[1];
  const bool distinctQueueIdentities = queueIdentities[0] != 0 &&
      queueIdentities[1] != 0 && queueIdentities[0] != queueIdentities[1];
  const bool distinctCommandBufferIds = commandBufferIds[0] != 0 &&
      commandBufferIds[1] != 0 && commandBufferIds[0] != commandBufferIds[1];
  const bool hasGpuTracks = gpuLines.size() >= 2 &&
      raw.find("\"track_kind\":\"gpu\"") != std::string::npos;
  const bool artifactsPreserved = fs::exists(requestPath) &&
      fs::exists(receiptPath) && fs::exists(rawPath) &&
      fs::exists(pftracePath) && traceFile && traceFile.tellg() > 0;
  const bool receiptComplete =
      receipt.find("\"status\":\"complete\"") != std::string::npos;
  const bool passed = finalized && probeValid && submitObserved &&
      completionsPublished && commandBuffersSucceeded && gpuSamplesBound &&
      submitBindingsObserved && submitGpuFlowsObserved && separateGpuTracks &&
      distinctQueueIdentities && distinctCommandBufferIds &&
      gpuClockCalibrated && hasGpuTracks &&
      artifactsPreserved && receiptComplete;
  std::printf("integrated_capture=%s submit_begin=%s gpu_cb_bound=%s gpu_clock_calibration=%s queues_distinct=%s command_buffers_distinct=%s gpu_tracks_separate=%s submit_gpu_flows=%s artifacts_preserved=%s\n",
              passed ? "passed" : "failed",
              submitBindingsObserved ? "yes" : "no",
              gpuSamplesBound ? "yes" : "no",
              gpuClockCalibrated ? "yes" : "no",
              distinctQueueIdentities ? "yes" : "no",
              distinctCommandBufferIds ? "yes" : "no",
              separateGpuTracks ? "yes" : "no",
              submitGpuFlowsObserved ? "yes" : "no",
              artifactsPreserved ? "yes" : "no");
  std::printf("integrated_artifacts request=%s receipt=%s raw=%s pftrace=%s\n",
              requestPath.c_str(), receiptPath.c_str(), rawPath.c_str(),
              pftracePath.c_str());
  unsetenv("DXMT9_TRACE_CONFIG");
  return passed;
}

static bool runIntegratedStageCapture(id<MTLDevice> device,
                                      id<MTLCommandQueue> queue,
                                      id<MTLRenderPipelineState> pipeline,
                                      id<MTLTexture> color) {
  namespace tracing = dxmt9::tracing;
  namespace fs = std::filesystem;
  const fs::path root = fs::current_path() /
      "experiments/output/tracing-acceptance" /
      ("integrated-metal-stage-smoke-" + std::to_string(getpid()));
  fs::create_directories(root);
  const fs::path requestPath = root / "request.json";
  const fs::path receiptPath = root / "receipt.json";
  const fs::path rawPath = root / "trace.pftrace.raw.jsonl";
  const fs::path pftracePath = root / "trace.pftrace";
  const std::string runId = "integrated-metal-stage-smoke-" +
      std::to_string(getpid());
  const std::string canonical =
      "{\"attempt_id\":" + jsonString(runId) +
      ",\"categories\":[\"submit\",\"gpu\"]"
      ",\"gpu_timing\":{\"calibration\":true,\"capacity\":8,"
      "\"level\":\"encoder-stage\",\"required\":true}"
      ",\"limits\":{\"finalize_timeout_ms\":10000,"
      "\"max_bytes\":1048576,\"max_events\":4096}"
      ",\"optional_collectors\":[]"
      ",\"output\":{\"pftrace\":" + jsonString(pftracePath.string()) +
      ",\"receipt\":" + jsonString(receiptPath.string()) + "}"
      ",\"profile_id\":\"native-gpu-stage-smoke\""
      ",\"qualification_policy\":{\"max_clock_error_ns\":1000}"
      ",\"question\":\"integrated-native-gpu-stage-capture\""
      ",\"required_categories\":[\"submit\",\"gpu\"]"
      ",\"required_collectors\":[\"engine-perfetto\"]"
      ",\"run_id\":" + jsonString(runId) +
      ",\"schema\":\"dxmt9.trace.request.v1\""
      ",\"window\":{\"delay_ms\":0,\"duration_ms\":30000,"
      "\"mode\":\"duration\"}}";
  const std::string request = canonical.substr(0, canonical.size() - 1) +
      ",\"config_sha256\":" + jsonString(sha256(canonical)) + "}";
  {
    std::ofstream output(requestPath, std::ios::binary);
    output << request;
    if (!output.good()) {
      std::puts("integrated_stage_capture=invalid reason=request_write_failed");
      return false;
    }
  }
  unsetenv("DXMT9_TRACE_CONFIG");
  if (setenv("DXMT9_TRACE_CONFIG", requestPath.c_str(), 1) != 0 ||
      !tracing::initialize_from_environment()) {
    std::puts("integrated_stage_capture=unsupported reason=trace_facade_unavailable");
    return false;
  }
  bool becameEnabled = false;
  for (unsigned attempt = 0; attempt < 5000; ++attempt) {
    if (tracing::enabled()) {
      becameEnabled = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const std::uintptr_t deviceHandle =
      reinterpret_cast<std::uintptr_t>(device);
  if (!becameEnabled || !tracing::attach_metal_device(deviceHandle)) {
    std::puts("integrated_stage_capture=invalid reason=enable_or_gpu_attach_failed");
    (void)tracing::stop_and_finalize();
    return false;
  }

  id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
  if (!commandBuffer) {
    std::puts("integrated_stage_capture=invalid reason=no_command_buffer");
    (void)tracing::stop_and_finalize();
    return false;
  }
  const std::uintptr_t queueIdentity = reinterpret_cast<std::uintptr_t>(queue);
  constexpr std::uint64_t sequenceId = 1;
  const std::uintptr_t commandBufferHandle =
      reinterpret_cast<std::uintptr_t>(commandBuffer);
  const std::uint64_t commandBufferId = gpu::processCommandBufferId(
      commandBufferHandle, queueIdentity, sequenceId);
  const std::uint64_t deviceGeneration =
      gpu::processCommandBufferDeviceGeneration(commandBufferHandle);
  if (!commandBufferId || !deviceGeneration ||
      !gpu::setProcessCommandBufferDeviceGeneration(
          commandBufferHandle, queueIdentity, sequenceId, deviceGeneration)) {
    std::puts("integrated_stage_capture=invalid reason=work_identity_failed");
    (void)tracing::stop_and_finalize();
    return false;
  }
  const gpu::WorkIdentity work{
      .deviceGeneration = deviceGeneration,
      .queueGeneration = queueIdentity,
      .commandBufferId = commandBufferId,
      .sequenceId = sequenceId,
  };
  gpu::StagePairReservation stageReservation{};
  {
    gpu::StageWorkScope stageWork(work);
    stageReservation = gpu::reserveCurrentStageAttachment(
        deviceHandle, commandBufferHandle);
  }
  if (!stageReservation) {
    std::puts("integrated_stage_capture=unsupported reason=stage_reservation_unavailable");
    (void)tracing::stop_and_finalize();
    return false;
  }
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor
      renderPassDescriptor];
  pass.colorAttachments[0].texture = color;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.2, 0.3, 1.0);
  pass.sampleBufferAttachments[0].sampleBuffer =
      (id<MTLCounterSampleBuffer>)static_cast<obj_handle_t>(
          stageReservation.attachment.sample_buffer);
  pass.sampleBufferAttachments[0].startOfVertexSampleIndex =
      stageReservation.attachment.start_of_encoder_sample_index;
  pass.sampleBufferAttachments[0].endOfVertexSampleIndex = MTLCounterDontSample;
  pass.sampleBufferAttachments[0].startOfFragmentSampleIndex =
      MTLCounterDontSample;
  pass.sampleBufferAttachments[0].endOfFragmentSampleIndex =
      stageReservation.attachment.end_of_encoder_sample_index;
  id<MTLRenderCommandEncoder> encoder =
      [commandBuffer renderCommandEncoderWithDescriptor:pass];
  if (!encoder) {
    std::puts("integrated_stage_capture=invalid reason=no_render_encoder");
    (void)tracing::stop_and_finalize();
    return false;
  }
  [encoder setRenderPipelineState:pipeline];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [encoder endEncoding];
  if (gpu::observeProcessGpuSubmission(work, commandBufferHandle) !=
      commandBufferId) {
    std::puts("integrated_stage_capture=invalid reason=production_bridge_admission_failed");
    (void)tracing::stop_and_finalize();
    return false;
  }
  [commandBuffer commit];
  const auto emitSubmit = [&](tracing::EventId event) {
    tracing::TraceEvent value{};
    value.timestamp_ns = tracing::now_ns();
    value.work_id = commandBufferId;
    value.arg0 = queueIdentity;
    value.related_owner_id = queueIdentity;
    value.device_generation = deviceGeneration;
    value.event_id = static_cast<std::uint16_t>(event);
    value.category = static_cast<std::uint8_t>(tracing::Category::Submit);
    value.phase = static_cast<std::uint8_t>(tracing::Phase::Instant);
    return tracing::emit(value);
  };
  const bool submitBeginEmitted = emitSubmit(tracing::EventId::SubmitBegin);
  const bool submitEmitted = emitSubmit(tracing::EventId::Submit);
  [commandBuffer waitUntilCompleted];
  const bool commandBufferSucceeded =
      commandBuffer.status != MTLCommandBufferStatusError;
  const bool completionPublished = gpu::publishCompletedProcessGpuCommandBuffer(
      commandBufferHandle, queueIdentity, sequenceId,
      commandBufferSucceeded ? gpu::CommandBufferStatus::Completed
                             : gpu::CommandBufferStatus::Failed);
  gpu::retireProcessCommandBufferIdsThrough(queueIdentity, sequenceId);
  const bool finalized = tracing::stop_and_finalize();

  std::ifstream rawFile(rawPath, std::ios::binary);
  std::ifstream receiptFile(receiptPath, std::ios::binary);
  std::ifstream traceFile(pftracePath, std::ios::binary | std::ios::ate);
  const std::string raw{std::istreambuf_iterator<char>(rawFile), {}};
  const std::string receipt{std::istreambuf_iterator<char>(receiptFile), {}};
  const std::string cbId = std::to_string(commandBufferId);
  const std::string queueId = std::to_string(queueIdentity);
  const auto stringField = [](const std::string& line,
                              const std::string& field) {
    const std::string marker = "\"" + field + "\":\"";
    const auto start = line.find(marker);
    if (start == std::string::npos) return std::string{};
    const auto valueStart = start + marker.size();
    const auto end = line.find('"', valueStart);
    return end == std::string::npos
        ? std::string{} : line.substr(valueStart, end - valueStart);
  };
  std::string stageBeginLine;
  std::string stageEndLine;
  std::string cbBeginLine;
  std::string submitBeginLine;
  for (std::size_t position = 0; position < raw.size();) {
    const std::size_t end = raw.find('\n', position);
    const std::string line = raw.substr(position, end == std::string::npos
        ? std::string::npos : end - position);
    const bool bound =
        line.find("\"arg0\":\"" + cbId + "\"") != std::string::npos &&
        line.find("\"relation_queue_identity\":\"" + queueId + "\"") !=
            std::string::npos;
    if (bound && line.find("\"name\":\"gpu_stage_begin\"") !=
            std::string::npos) stageBeginLine = line;
    if (bound && line.find("\"name\":\"gpu_stage_end\"") !=
            std::string::npos) stageEndLine = line;
    if (bound && line.find("\"name\":\"gpu_command_buffer_begin\"") !=
            std::string::npos) cbBeginLine = line;
    if (line.find("\"name\":\"submit_begin\"") != std::string::npos &&
        line.find("\"work_ids\":[\"" + cbId + "\"]") !=
            std::string::npos &&
        line.find("\"relation_queue_identity\":\"" + queueId + "\"") !=
            std::string::npos) submitBeginLine = line;
    if (end == std::string::npos) break;
    position = end + 1;
  }
  const std::string calibrationId = stringField(stageBeginLine, "calibration_id");
  const std::string sourceTimestamp =
      stringField(stageBeginLine, "source_timestamp");
  const std::string endSourceTimestamp =
      stringField(stageEndLine, "source_timestamp");
  const std::string mappingUncertainty =
      stringField(stageBeginLine, "uncertainty_ns");
  const std::string calibrationScale =
      stringField(stageBeginLine, "calibration_scale");
  const std::string firstSource =
      stringField(stageBeginLine, "calibration_source_first");
  const std::string secondSource =
      stringField(stageBeginLine, "calibration_source_second");
  const std::string firstHost =
      stringField(stageBeginLine, "calibration_host_first_ns");
  const std::string secondHost =
      stringField(stageBeginLine, "calibration_host_second_ns");
  const std::string validBegin =
      stringField(stageBeginLine, "calibration_valid_begin");
  const std::string validEnd =
      stringField(stageBeginLine, "calibration_valid_end");
  const bool mappingQualified = !calibrationId.empty() && calibrationId != "0" &&
      !sourceTimestamp.empty() && sourceTimestamp != "0" &&
      !calibrationScale.empty() &&
      std::strtod(calibrationScale.c_str(), nullptr) > 0.0 &&
      !mappingUncertainty.empty() &&
      std::strtoull(mappingUncertainty.c_str(), nullptr, 10) <= 1000 &&
      !firstSource.empty() && firstSource != "0" &&
      !secondSource.empty() && secondSource != "0" &&
      !firstHost.empty() && firstHost != "0" &&
      !secondHost.empty() && secondHost != "0" &&
      !validBegin.empty() && validBegin != "0" &&
      !validEnd.empty() && validEnd != "0" &&
      std::strtoull(secondSource.c_str(), nullptr, 10) >
          std::strtoull(firstSource.c_str(), nullptr, 10) &&
      std::strtoull(secondHost.c_str(), nullptr, 10) >
          std::strtoull(firstHost.c_str(), nullptr, 10) &&
      std::strtoull(validEnd.c_str(), nullptr, 10) >
          std::strtoull(validBegin.c_str(), nullptr, 10) &&
      std::strtoull(sourceTimestamp.c_str(), nullptr, 10) >=
          std::strtoull(validBegin.c_str(), nullptr, 10) &&
      std::strtoull(sourceTimestamp.c_str(), nullptr, 10) <=
          std::strtoull(validEnd.c_str(), nullptr, 10) &&
      !endSourceTimestamp.empty() && endSourceTimestamp != "0" &&
      std::strtoull(endSourceTimestamp.c_str(), nullptr, 10) >
          std::strtoull(sourceTimestamp.c_str(), nullptr, 10) &&
      std::strtoull(endSourceTimestamp.c_str(), nullptr, 10) <=
          std::strtoull(validEnd.c_str(), nullptr, 10) &&
      stringField(stageEndLine, "calibration_id") == calibrationId;
  const std::string submitRecord = stringField(submitBeginLine, "event_id");
  const std::string cbRecord = stringField(cbBeginLine, "event_id");
  const bool submitGpuFlow =
      hasSubmittedGpuRelation(raw, submitRecord, cbRecord);
  const bool distinctStageTrack = !stageBeginLine.empty() &&
      !stageEndLine.empty() && !cbBeginLine.empty() &&
      stringField(stageBeginLine, "track_id") !=
          stringField(cbBeginLine, "track_id") &&
      stringField(stageBeginLine, "track_id") ==
          stringField(stageEndLine, "track_id");
  const bool artifactsPreserved = fs::exists(requestPath) &&
      fs::exists(receiptPath) && fs::exists(rawPath) &&
      fs::exists(pftracePath) && traceFile && traceFile.tellg() > 0;
  const bool receiptComplete =
      receipt.find("\"status\":\"complete\"") != std::string::npos;
  const bool stageSampleBound = !stageBeginLine.empty() &&
      !stageEndLine.empty() &&
      stageBeginLine.find("\"source_clock_id\":\"" +
          std::to_string(gpu::kMetalGpuCounterClockId) + "\"") !=
          std::string::npos &&
      stageBeginLine.find("\"source_timestamp_unit\":\"ticks\"") !=
          std::string::npos &&
      stageBeginLine.find("\"source_api\":\"MTLCounterSampleBuffer::timestamp startOfVertex\"") !=
          std::string::npos &&
      stageEndLine.find("\"source_api\":\"MTLCounterSampleBuffer::timestamp endOfFragment\"") !=
          std::string::npos;
  const bool passed = finalized && submitBeginEmitted && submitEmitted &&
      commandBufferSucceeded && completionPublished && mappingQualified &&
      stageSampleBound && distinctStageTrack && submitGpuFlow &&
      artifactsPreserved && receiptComplete;
  std::printf("integrated_stage_capture=%s stage_sample_bound=%s clock_mapping_qualified=%s stage_cb_tracks_separate=%s submit_gpu_flow=%s artifacts_preserved=%s\n",
              passed ? "passed" : "failed",
              stageSampleBound ? "yes" : "no",
              mappingQualified ? "yes" : "no",
              distinctStageTrack ? "yes" : "no",
              submitGpuFlow ? "yes" : "no",
              artifactsPreserved ? "yes" : "no");
  std::printf("integrated_stage_artifacts request=%s receipt=%s raw=%s pftrace=%s\n",
              requestPath.c_str(), receiptPath.c_str(), rawPath.c_str(),
              pftracePath.c_str());
  unsetenv("DXMT9_TRACE_CONFIG");
  return passed;
}

struct Samples {
  std::uint32_t commandBuffer = 0;
  std::uint32_t stage = 0;
  gpu::Sample command{};
  gpu::StageSample stageSample{};
};

static void consumeCommand(void* context, const gpu::Sample& sample) noexcept {
  auto& samples = *static_cast<Samples*>(context);
  samples.command = sample;
  ++samples.commandBuffer;
}

static void consumeStage(void* context,
                         const gpu::StageSample& sample) noexcept {
  auto& samples = *static_cast<Samples*>(context);
  samples.stageSample = sample;
  ++samples.stage;
}

int main() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      std::puts("gpu_timing=unsupported reason=no_metal_device");
      return 0;
    }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!queue) {
      std::puts("gpu_timing=invalid reason=no_command_queue");
      return 1;
    }
    WMT::Device wmtDevice{static_cast<obj_handle_t>(
        reinterpret_cast<std::uintptr_t>(device))};
    const auto capabilities = gpu::queryCapabilities(wmtDevice);
    const char* deviceName = [[device name] UTF8String];
    std::printf("metal_device_name=%s registry_id=%llu\n",
                deviceName ? deviceName : "unknown",
                static_cast<unsigned long long>(device.registryID));
    std::printf("gpu_capabilities=command_buffer_time:%s stage_boundary:%s\n",
                capabilities.commandBufferTime == gpu::Support::Supported
                    ? "supported" : "unsupported",
                capabilities.stageBoundary == gpu::Support::Supported
                    ? "supported" : "unsupported");
    std::puts("gpu_sample_sources=command_buffer:MTLCommandBuffer.GPUStartTime-GPUEndTime stage:MTLCommonCounterSetTimestamp startOfVertex-endOfFragment host:mach_absolute_time");
    gpu::ClockAnchor firstAnchor{}, secondAnchor{};
    const bool firstAnchorValid = gpu::sampleClockAnchor(wmtDevice, firstAnchor);

    MTLTextureDescriptor *textureDesc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:64 height:64 mipmapped:NO];
    textureDesc.storageMode = MTLStorageModeShared;
    textureDesc.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> color = [device newTextureWithDescriptor:textureDesc];
    if (!color) {
      std::puts("gpu_timing=invalid reason=no_shared_render_target");
      [queue release];
      return 1;
    }
    id<MTLRenderPipelineState> pipeline = makeSmokePipeline(device);
    if (!pipeline) {
      std::puts("gpu_timing=invalid reason=no_smoke_pipeline");
      [color release]; [queue release];
      return 1;
    }

    auto encodeClearAndDraw = [&](id<MTLCommandBuffer> commandBuffer,
                                  const WMTSampleBufferAttachmentInfo* attachment) {
      MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor
          renderPassDescriptor];
      pass.colorAttachments[0].texture = color;
      pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      pass.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.2, 0.3, 1.0);
      if (attachment && attachment->sample_buffer) {
        pass.sampleBufferAttachments[0].sampleBuffer =
            (id<MTLCounterSampleBuffer>)static_cast<obj_handle_t>(
                attachment->sample_buffer);
        pass.sampleBufferAttachments[0].startOfVertexSampleIndex =
            attachment->start_of_encoder_sample_index;
        pass.sampleBufferAttachments[0].endOfVertexSampleIndex = MTLCounterDontSample;
        pass.sampleBufferAttachments[0].startOfFragmentSampleIndex = MTLCounterDontSample;
        pass.sampleBufferAttachments[0].endOfFragmentSampleIndex =
            attachment->end_of_encoder_sample_index;
      }
      id<MTLRenderCommandEncoder> encoder =
          [commandBuffer renderCommandEncoderWithDescriptor:pass];
      if (!encoder) return false;
      [encoder setRenderPipelineState:pipeline];
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0
                       vertexCount:3];
      [encoder endEncoding];
      return true;
    };

    constexpr std::size_t kRepeats = 3;
    constexpr std::uint64_t kMaxObserverAddedEncodeMedianThreadCpuNs =
        100'000;
    const bool stageSupported =
        capabilities.stageBoundary == gpu::Support::Supported;
    gpu::StageSampleBuffer stageBuffer;
    const bool stageStarted = stageSupported &&
        stageBuffer.start(wmtDevice, 1, static_cast<std::uint32_t>(kRepeats));
    if (!stageStarted) {
      std::puts("stage_sampling=unsupported reason=production_reservation_unavailable");
    }

    const gpu::SessionConfig config{
        .sessionGeneration = 1,
        .capacity = static_cast<std::uint32_t>(kRepeats),
        .level = gpu::Level::CommandBuffer,
        .calibration = {},
    };
    gpu::SubmissionBridge bridge;
    if (!bridge.start(config, capabilities)) {
      std::puts("gpu_timing=unsupported reason=production_bridge_unavailable");
      [color release]; [queue release];
      return 0;
    }
    gpu::Collector collector;
    if (collector.start(config, capabilities) != gpu::StartResult::Started) {
      std::puts("gpu_timing=invalid reason=production_collector_start_failed");
      [color release]; [queue release];
      return 1;
    }
    std::array<std::uint64_t, kRepeats> baselineHashes{};
    std::array<std::uint64_t, kRepeats> sampledHashes{};
    std::array<std::uint64_t, kRepeats> baselineEncodeNs{};
    std::array<std::uint64_t, kRepeats> sampledEncodeNs{};
    std::array<std::uint64_t, kRepeats> baselineEncodeCpuNs{};
    std::array<std::uint64_t, kRepeats> sampledEncodeCpuNs{};
    std::array<std::uint64_t, kRepeats> gpuIntervalNs{};
    std::array<std::uint64_t, kRepeats> stageIntervalTicks{};
    std::uint64_t commandSamplesDrained = 0;
    std::uint64_t stageSamplesResolved = 0;
    const auto measuredRunStart = mach_absolute_time();
    const auto measuredRunCpuStart = threadCpuNanoseconds();
    for (std::size_t i = 0; i < kRepeats; ++i) {
      id<MTLCommandBuffer> baselineBuffer = [queue commandBuffer];
      if (!baselineBuffer) {
        std::puts("gpu_timing=invalid reason=no_baseline_command_buffer");
        [color release]; [queue release];
        return 1;
      }
      auto encodeBegin = mach_absolute_time();
      auto encodeCpuBegin = threadCpuNanoseconds();
      const bool baselineEncoded = encodeClearAndDraw(baselineBuffer, nullptr);
      baselineEncodeNs[i] = toNanoseconds(mach_absolute_time() - encodeBegin);
      baselineEncodeCpuNs[i] = threadCpuNanoseconds() - encodeCpuBegin;
      if (!baselineEncoded) {
        std::puts("gpu_timing=invalid reason=no_baseline_encoder");
        [color release]; [queue release];
        return 1;
      }
      [baselineBuffer commit];
      [baselineBuffer waitUntilCompleted];
      baselineHashes[i] = hashPixel(color);

      const gpu::WorkIdentity work{
          .deviceGeneration = 1,
          .queueGeneration = 1,
          .commandBufferId = i + 1u,
          .sequenceId = i + 1u,
      };
      const auto stageReservation = stageStarted
          ? stageBuffer.reserve(work)
          : gpu::StagePairReservation{};
      const auto collectorTicket = collector.reserve(work);
      id<MTLCommandBuffer> measuredBuffer = [queue commandBuffer];
      if (!measuredBuffer) {
        std::puts("gpu_timing=invalid reason=no_measured_command_buffer");
        [color release]; [queue release];
        return 1;
      }
      encodeBegin = mach_absolute_time();
      encodeCpuBegin = threadCpuNanoseconds();
      const bool measuredEncoded =
          encodeClearAndDraw(measuredBuffer,
                             stageReservation ? &stageReservation.attachment
                                              : nullptr);
      sampledEncodeNs[i] = toNanoseconds(mach_absolute_time() - encodeBegin);
      sampledEncodeCpuNs[i] = threadCpuNanoseconds() - encodeCpuBegin;
      if (!measuredEncoded) {
        std::puts("gpu_timing=invalid reason=no_measured_encoder");
        [color release]; [queue release];
        return 1;
      }
      WMT::CommandBuffer wmtCommandBuffer{
          static_cast<obj_handle_t>(
              reinterpret_cast<std::uintptr_t>(measuredBuffer))};
      if (!bridge.observeSubmission(
              work, static_cast<std::uintptr_t>(wmtCommandBuffer.handle))) {
        std::puts("gpu_timing=unsupported reason=production_submission_capacity");
        [color release]; [queue release];
        return 0;
      }
      [measuredBuffer commit];
      [measuredBuffer waitUntilCompleted];
      const gpu::Completion completion{
          .status = measuredBuffer.status == MTLCommandBufferStatusError
                        ? gpu::CommandBufferStatus::Failed
                        : gpu::CommandBufferStatus::Completed,
          .gpuStartNs = measuredBuffer.GPUStartTime > 0.0
              ? static_cast<std::uint64_t>(measuredBuffer.GPUStartTime * 1.0e9)
              : 0,
          .gpuEndNs = measuredBuffer.GPUEndTime > 0.0
              ? static_cast<std::uint64_t>(measuredBuffer.GPUEndTime * 1.0e9)
              : 0,
      };
      if (collectorTicket) (void)collector.complete(collectorTicket, completion);
      if (stageReservation) {
        gpu::StagePairResult stageResult{};
        const bool resolved = stageBuffer.resolve(
            stageReservation.ticket,
            gpu::CompletionEvidence{work, completion.status, true}, stageResult);
        stageSamplesResolved += resolved ? 1u : 0u;
        if (resolved && stageResult.validity == gpu::ResultValidity::Valid) {
          stageIntervalTicks[i] = stageResult.endTimestamp -
              stageResult.startTimestamp;
        } else {
          std::printf("stage_sampling=invalid repeat=%zu validity=%u\n", i,
                      static_cast<unsigned>(stageResult.validity));
        }
      }
      (void)bridge.pollCompleted();
      Samples samples{};
      commandSamplesDrained += bridge.drain(consumeCommand, &samples);
      if (samples.commandBuffer &&
          samples.command.validity == gpu::ResultValidity::Valid) {
        gpuIntervalNs[i] = samples.command.durationNs;
      }
      gpu::Sample collectorSample{};
      (void)collector.drainOne(collectorSample);
      sampledHashes[i] = hashPixel(color);
      if (sampledHashes[i] != baselineHashes[i]) {
        std::puts("image_compare=failed reason=observer_changed_output");
        [color release]; [queue release];
        return 1;
      }
    }
    const auto measuredRunElapsedNs =
        toNanoseconds(mach_absolute_time() - measuredRunStart);
    const auto measuredRunThreadCpuNs =
        threadCpuNanoseconds() - measuredRunCpuStart;
    const bool sameOutputs = std::all_of(
        baselineHashes.begin(), baselineHashes.end(),
        [&](std::uint64_t value) { return value == baselineHashes[0]; }) &&
        std::equal(baselineHashes.begin(), baselineHashes.end(),
                   sampledHashes.begin());
    if (!sameOutputs) {
      std::puts("image_compare=failed reason=unmatched_output_hashes");
      [color release]; [queue release];
      return 1;
    }
    std::printf("image_compare=passed repeats=%zu full_surface_hash=%016llx\n",
                kRepeats, static_cast<unsigned long long>(baselineHashes[0]));
    auto printDistribution = [](const char* name, const auto& values) {
      auto sorted = values;
      std::sort(sorted.begin(), sorted.end());
      std::printf("%s_ns_min=%llu median=%llu max=%llu\n", name,
                  static_cast<unsigned long long>(sorted.front()),
                  static_cast<unsigned long long>(sorted[sorted.size() / 2]),
                  static_cast<unsigned long long>(sorted.back()));
    };
    printDistribution("cpu_encode_off", baselineEncodeNs);
    printDistribution("cpu_encode_on", sampledEncodeNs);
    printDistribution("cpu_encode_off_thread", baselineEncodeCpuNs);
    printDistribution("cpu_encode_on_thread", sampledEncodeCpuNs);
    auto medianValue = [](const auto& values) {
      auto sorted = values;
      std::sort(sorted.begin(), sorted.end());
      return sorted[sorted.size() / 2];
    };
    const auto threadCpuMedianDelta =
        static_cast<std::int64_t>(medianValue(sampledEncodeCpuNs)) -
        static_cast<std::int64_t>(medianValue(baselineEncodeCpuNs));
    const auto wallMedianDelta =
        static_cast<std::int64_t>(medianValue(sampledEncodeNs)) -
        static_cast<std::int64_t>(medianValue(baselineEncodeNs));
    std::printf("observer_encode_median_delta_thread_cpu_ns=%lld wall_ns=%lld budget_thread_cpu_ns=%llu budget_result=%s\n",
                static_cast<long long>(threadCpuMedianDelta),
                static_cast<long long>(wallMedianDelta),
                static_cast<unsigned long long>(
                    kMaxObserverAddedEncodeMedianThreadCpuNs),
                threadCpuMedianDelta <= static_cast<std::int64_t>(
                    kMaxObserverAddedEncodeMedianThreadCpuNs)
                    ? "within_informational_budget" : "over_informational_budget");
    std::printf("measured_run_elapsed_ns=%llu measured_run_thread_cpu_ns=%llu repeats=%zu\n",
                static_cast<unsigned long long>(measuredRunElapsedNs),
                static_cast<unsigned long long>(measuredRunThreadCpuNs),
                kRepeats);
    const auto validGpu = static_cast<std::size_t>(std::count_if(
        gpuIntervalNs.begin(), gpuIntervalNs.end(),
        [](std::uint64_t value) { return value != 0; }));
    if (validGpu == kRepeats) printDistribution("gpu_interval", gpuIntervalNs);
    else std::printf("gpu_interval=partial valid_repeats=%zu total_repeats=%zu\n",
                     validGpu, kRepeats);
    if (stageStarted) {
      const auto validStage = static_cast<std::size_t>(std::count_if(
          stageIntervalTicks.begin(), stageIntervalTicks.end(),
          [](std::uint64_t value) { return value != 0; }));
      if (validStage == kRepeats) {
        printDistribution("stage_interval_ticks", stageIntervalTicks);
      } else {
        std::printf("stage_interval=partial valid_repeats=%zu total_repeats=%zu\n",
                    validStage, kRepeats);
      }
    }
    const auto bridgeCounters = bridge.counters();
    const auto collectorCounters = collector.counters();
    const auto stageCounters = stageBuffer.counters();
    const auto lossCount = bridgeCounters.lostCapacity +
        collectorCounters.lostCapacity + stageCounters.lostCapacity;
    std::printf("trace_events=command_buffer_samples:%llu stage_samples_resolved:%llu\n",
                static_cast<unsigned long long>(commandSamplesDrained),
                static_cast<unsigned long long>(stageSamplesResolved));
    std::printf("trace_counters=command_selected:%llu command_completed:%llu command_invalid:%llu command_lost:%llu stage_selected:%llu stage_resolved:%llu stage_invalid:%llu stage_lost:%llu total_loss_count:%llu\n",
                static_cast<unsigned long long>(bridgeCounters.selected),
                static_cast<unsigned long long>(bridgeCounters.completed),
                static_cast<unsigned long long>(bridgeCounters.invalid),
                static_cast<unsigned long long>(bridgeCounters.lostCapacity +
                                               collectorCounters.lostCapacity),
                static_cast<unsigned long long>(stageCounters.selected),
                static_cast<unsigned long long>(stageCounters.resolved),
                static_cast<unsigned long long>(stageCounters.invalid),
                static_cast<unsigned long long>(stageCounters.lostCapacity),
                static_cast<unsigned long long>(lossCount));
    std::printf("qualification=limited workload=64x64_triangle_clear repeats=%zu informational_budget=observer_added_encode_median_thread_cpu_ns_le_%llu hashes_match=%s zero_losses=%s\n",
                kRepeats,
                static_cast<unsigned long long>(
                    kMaxObserverAddedEncodeMedianThreadCpuNs),
                sameOutputs ? "yes" : "no", lossCount == 0 ? "yes" : "no");
    const bool smokeAccepted = sameOutputs && lossCount == 0;

    const bool secondAnchorValid = gpu::sampleClockAnchor(wmtDevice, secondAnchor);
    gpu::Calibration calibration{};
    if (firstAnchorValid && secondAnchorValid &&
        gpu::calibrateFromAnchors(1, gpu::kMetalGpuCounterClockId,
                                  gpu::kMachAbsoluteTimeClockId,
                                  firstAnchor, secondAnchor, calibration)) {
      std::printf("gpu_clock_calibration=valid observations=%u scale=%.12g offset=%.3f uncertainty_ns=%llu\n",
                  calibration.observationCount, calibration.scale,
                  calibration.offset,
                  static_cast<unsigned long long>(calibration.uncertaintyNs));
    } else {
      std::puts("gpu_clock_calibration=unsupported reason=invalid_paired_anchors");
    }
    stageBuffer.close();
    bridge.close();
    collector.close();
    const bool integratedAccepted =
        runIntegratedCapture(device, queue, pipeline, color);
    const bool integratedStageAccepted =
        runIntegratedStageCapture(device, queue, pipeline, color);
    [pipeline release];
    [color release]; [queue release];
    return smokeAccepted && integratedAccepted && integratedStageAccepted ? 0 : 1;
  }
}
