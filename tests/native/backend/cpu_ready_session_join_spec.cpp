// Tape-gated session join (DXMT9_CPU_READY_TAPE) — deterministic coverage
// for source-kind-neutral EncodeSession integration:
//   * payload-view carrier predicates classify Legacy and Arena sources
//     identically to the ChunkSlot predicates;
//   * the neutral batch prefix selector admits multi-Arena and mixed
//     Legacy/Arena FIFO prefixes that the H229 legacy-only selector rejects;
//   * the production encodeChunk payload overload carries one EncodeSession
//     and one injected command buffer across consecutive Arena sources with
//     no artificial submission/session boundary at the source edge;
//   * the real queue seam (replayRawChunk publication -> neutral batch
//     dequeue -> whole-prefix retention -> one submitted record -> completion
//     -> ordered reclaim) attributes FIFO completion per source for a mixed
//     Arena/Legacy/Arena prefix through a single submission;
//   * the production runCpuReadySessionEncodeLoop consumes multi-Arena and
//     mixed prefixes, carries one pending session, and releases it at both a
//     deterministic shutdown drain and a real compatibility-writer capacity
//     fence.

#include "device_c_common.hpp"
#include "device_c_cpu_ready_plan.hpp"
#include "device_c_replay_offload.hpp"
#include "dxmt9/com.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9/device_c.h"
#include "dxmt9/dxmt9_command_queue.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "d3d9_pe_wire_handle.hpp"

#include "../../../src/dxmt9/dxmt9_draw_encoder.hpp"
#include "../../../src/dxmt9/dxmt9_encode_session.hpp"
#include "../../../src/dxmt9/dxmt9_pipeline_cache.hpp"
#include "../../../src/dxmt9/dxmt9_resource_pool.hpp"
#include "../../../src/dxmt9/dxmt9_ring_arena.hpp"
#include "../../../src/dxmt9/dxmt9_source_payload.hpp"
#include "../../../src/dxmt9/render/backend_interface.hpp"
#include "../../../src/dxmt9/render/open_cb_carrier.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace dxmt9 {

struct CommandQueueArenaLeaseTestAccess {
  struct BatchResult {
    std::size_t dequeued = 0;
    std::size_t retained = 0;
    bool submitted = false;
    bool completed = false;
    std::size_t finishIterations = 0;
    std::vector<std::uint64_t> seqIds;
    std::vector<bool> arenaKinds;
  };

  static std::size_t readyCount(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.cpuReadyTape_.readyCount();
  }

  static std::size_t residentCount(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.cpuReadyTape_.residentCount();
  }

  static std::uint64_t completedSeqId(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.completedSeqId_;
  }

  static std::vector<core::metalqueue::QueueCompletionSource>
  snapshotReadyCompletionSources(CommandQueue& queue) {
    using namespace core::metalqueue;
    std::lock_guard lock(queue.mutex_);
    std::array<core::CpuReadyTape::ReadyEntry, kCommandChunkCount> ready{};
    const std::size_t count = queue.cpuReadyTape_.copyReadyPrefix(
        std::span<core::CpuReadyTape::ReadyEntry>(
            ready.data(), queue.cpuReadyTape_.readyCount()));
    std::vector<QueueCompletionSource> sources;
    sources.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      const auto& entry = ready[i];
      if (entry.controlIndex >= queue.slots_.size()) {
        return {};
      }
      const auto& control = queue.slots_[entry.controlIndex];
      const auto payload = queue.cpuReadyTape_.resolveSourcePayload(
          entry.source.id, entry.source.storage,
          core::CpuReadyTape::State::Ready);
      if (!payload.valid() || control.state != core::ChunkSlot::State::Pending ||
          control.sourceId != entry.source.id ||
          control.storage != entry.source.storage ||
          control.seqId != entry.seqId) {
        return {};
      }
      sources.push_back(QueueCompletionSource{
          .source = entry.source,
          .slotIndex = entry.controlIndex,
          .seqId = entry.seqId,
          .hasPresent = payload.presentRecordCount() != 0,
          .commandBegin = 0,
          .commandCount = payload.commandCount(),
      });
    }
    return sources;
  }

  static void installBackend(
      CommandQueue& queue,
      std::unique_ptr<render::IRenderBackend> backend) {
    queue.backend_ = std::move(backend);
  }

  static void stopAndRunCpuReadySessionEncodeLoop(CommandQueue& queue) {
    {
      std::lock_guard lock(queue.mutex_);
      queue.stop_ = true;
    }
    queue.runCpuReadySessionEncodeLoop({});
  }

  static void runCpuReadySessionEncodeLoop(CommandQueue& queue) {
    queue.runCpuReadySessionEncodeLoop({});
  }

  static void requestStop(CommandQueue& queue) {
    {
      std::lock_guard lock(queue.mutex_);
      queue.stop_ = true;
    }
    queue.encodeCv_.notify_all();
    queue.writeCv_.notify_all();
  }

  // Drive the real compatibility writer-acquire path, then discard the empty
  // reservation once capacity becomes available. This is the production wait
  // that a Legacy Present/query source reaches after Direct Arena sources have
  // occupied every control shell.
  static bool ensureAndAbortEmptyWriter(CommandQueue& queue) {
    std::unique_lock lock(queue.mutex_);
    if (!queue.queueLifecycle_.ensureWriterSlot(lock, kMaxQueuedChunks)) {
      return false;
    }
    (void)queue.queueLifecycle_.commitCurrentChunk(lock, kMaxQueuedChunks);
    return !queue.writingSlot_.has_value();
  }

  static bool allSourcesSubmitted(
      CommandQueue& queue,
      std::span<const core::metalqueue::QueueCompletionSource> sources) {
    std::lock_guard lock(queue.mutex_);
    for (const auto& source : sources) {
      if (queue.cpuReadyTape_.state(source.source.id,
                                    source.source.storage) !=
          core::CpuReadyTape::State::GPU) {
        return false;
      }
    }
    return true;
  }

  static std::size_t completeAndFinish(
      CommandQueue& queue,
      std::span<const core::metalqueue::QueueCompletionSource> sources) {
    using namespace core::metalqueue;
    QueueLifecycleController::PendingCompletion pending{};
    pending.slotIndex = sources.back().slotIndex;
    pending.seqId = sources.back().seqId;
    for (const auto& source : sources) {
      if (!pending.fixedCompletionSources.append(source)) {
        return 0;
      }
    }
    queue.queueLifecycle_.enqueuePendingCompletionForTest(std::move(pending));
    bool completionStop = false;
    if (!queue.queueLifecycle_.processOnePendingCompletion(completionStop)) {
      return 0;
    }

    std::size_t iterations = 0;
    std::unique_lock lock(queue.mutex_);
    while (iterations < sources.size() &&
           queue.queueLifecycle_.runFinishIteration(lock)) {
      ++iterations;
    }
    return iterations;
  }

  // Publishes the current legacy writing slot through the compatibility
  // publication path (the lane used for oversize/TriangleFan/Present chunks
  // when the Tape gate is on).
  static bool publishLegacyWritingSlot(CommandQueue& queue) {
    std::unique_lock lock(queue.mutex_);
    return queue.queueLifecycle_.commitCurrentChunk(lock, kMaxQueuedChunks);
  }

  // Dequeues one maximal source-kind-neutral FIFO prefix, retains the whole
  // prefix, submits ONE record covering every source, then completes and
  // reclaims it, recording per-source FIFO identity and payload kind.
  static BatchResult consumeBatch(CommandQueue& queue,
                                  std::size_t maxSources) {
    using namespace core::metalqueue;
    BatchResult result{};
    std::array<ReadySlotSnapshot, kCommandChunkCount> scratch{};
    std::array<QueueCompletionSource, kCommandChunkCount> retained{};
    {
      std::unique_lock lock(queue.mutex_);
      const std::size_t count = queue.queueLifecycle_.dequeueReadySlotBatchPrefix(
          lock,
          std::span<ReadySlotSnapshot>(scratch.data(),
                                       std::min(maxSources, scratch.size())),
          [](std::span<const ResolvedPublishedSource> candidates) noexcept {
            return render::selectCpuReadySessionBatchPrefix(candidates);
          });
      result.dequeued = count;
      if (count == 0) {
        return result;
      }
      for (std::size_t i = 0; i < count; ++i) {
        const auto resolved =
            queue.queueLifecycle_.resolveRepresentedSource(scratch[i]);
        if (!resolved.valid()) {
          return result;
        }
        result.seqIds.push_back(resolved.seqId);
        result.arenaKinds.push_back(resolved.payload.isArena());
      }
      result.retained = queue.queueLifecycle_.retainEncodedSourcesForPendingTail(
          lock,
          std::span<const ReadySlotSnapshot>(scratch.data(), count),
          std::span<QueueCompletionSource>(retained.data(), count));
      if (result.retained != count) {
        return result;
      }

      QueueSubmissionRecord record{};
      record.testOnlyAllowNullCommandBuffer = true;
      record.slotIndex = retained[count - 1].slotIndex;
      record.seqId = retained[count - 1].seqId;
      if (!record.assignFixedCompletionSources(
              std::span<const QueueCompletionSource>(retained.data(), count))) {
        return result;
      }
      result.submitted =
          queue.queueLifecycle_.submitEncodedSubmission(lock, record);
      if (!result.submitted) {
        return result;
      }
    }

    core::metalqueue::QueueLifecycleController::PendingCompletion pending{};
    pending.slotIndex = retained[result.retained - 1].slotIndex;
    pending.seqId = retained[result.retained - 1].seqId;
    for (std::size_t i = 0; i < result.retained; ++i) {
      if (!pending.fixedCompletionSources.append(retained[i])) {
        return result;
      }
    }
    queue.queueLifecycle_.enqueuePendingCompletionForTest(std::move(pending));
    bool completionStop = false;
    result.completed =
        queue.queueLifecycle_.processOnePendingCompletion(completionStop);
    if (!result.completed) {
      return result;
    }
    {
      std::unique_lock lock(queue.mutex_);
      for (std::size_t i = 0; i < result.retained; ++i) {
        if (!queue.queueLifecycle_.runFinishIteration(lock)) {
          break;
        }
        ++result.finishIterations;
        if (queue.cpuReadyTape_.residentCount() == 0) {
          break;
        }
      }
    }
    return result;
  }
};

}  // namespace dxmt9

namespace {

using namespace dxmt9::core;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

std::size_t alignUp(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

template <typename T>
std::vector<std::byte> bytesOf(const T& value) {
  std::vector<std::byte> bytes(sizeof(T));
  std::memcpy(bytes.data(), &value, sizeof(value));
  return bytes;
}

struct RecordSpec {
  std::uint32_t type = 0;
  std::vector<std::byte> payload;
  std::vector<D9CCommandChunkWireHandleEntryV2> handles;
};

struct WireFixture {
  std::vector<std::byte> bytes;
  dxmt9::d3d9::V2ChunkEnvelope envelope{};
};

WireFixture makeWireFixture(std::span<const RecordSpec> specs) {
  std::vector<D9CCommandChunkWireRecordHeaderV2> records;
  std::vector<D9CCommandChunkWireHandleEntryV2> handles;
  std::vector<std::byte> payload;
  for (const auto& spec : specs) {
    const auto* rule = dxmt9::d3d9::v2RecordRule(spec.type);
    check(rule != nullptr, "session join fixture record must be known");
    payload.resize(alignUp(payload.size(), rule->payloadAlignment));
    records.push_back({
        .type = spec.type,
        .flags = D9C_COMMAND_CHUNK_V2_RECORD_FLAG_NONE,
        .payloadOffset = static_cast<std::uint32_t>(payload.size()),
        .payloadSize = static_cast<std::uint32_t>(spec.payload.size()),
        .firstHandle = static_cast<std::uint32_t>(handles.size()),
        .handleCount = static_cast<std::uint32_t>(spec.handles.size()),
    });
    handles.insert(handles.end(), spec.handles.begin(), spec.handles.end());
    payload.insert(payload.end(), spec.payload.begin(), spec.payload.end());
  }

  D9CCommandChunkWireHeaderV2 header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION_V2,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_V2_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_V2_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_V2_SIZE,
      .recordTableOffset = D9C_COMMAND_CHUNK_WIRE_HEADER_V2_SIZE,
      .recordCount = static_cast<std::uint32_t>(records.size()),
      .handleCount = static_cast<std::uint32_t>(handles.size()),
      .payloadArenaSize = static_cast<std::uint32_t>(payload.size()),
  };
  header.handleTableOffset = static_cast<std::uint32_t>(alignUp(
      header.recordTableOffset + records.size() * sizeof(records[0]),
      alignof(D9CCommandChunkWireHandleEntryV2)));
  header.payloadArenaOffset = static_cast<std::uint32_t>(alignUp(
      header.handleTableOffset + handles.size() * sizeof(handles[0]),
      alignof(std::uint32_t)));

  WireFixture fixture;
  fixture.bytes.resize(header.payloadArenaOffset + payload.size());
  std::memcpy(fixture.bytes.data(), &header, sizeof(header));
  std::memcpy(fixture.bytes.data() + header.recordTableOffset,
              records.data(), records.size() * sizeof(records[0]));
  if (!handles.empty()) {
    std::memcpy(fixture.bytes.data() + header.handleTableOffset,
                handles.data(), handles.size() * sizeof(handles[0]));
  }
  if (!payload.empty()) {
    std::memcpy(fixture.bytes.data() + header.payloadArenaOffset,
                payload.data(), payload.size());
  }
  fixture.envelope = {
      .version = D9C_COMMAND_CHUNK_VERSION_V2,
      .recordCount = header.recordCount,
      .handleCount = header.handleCount,
  };
  return fixture;
}

RecordSpec clearRecord() {
  return {
      .type = D9C_COMMAND_RECORD_CLEAR,
      .payload = bytesOf(D9CCommandChunkWireClearV2{
          .flags = 1u,
          .colorARGB = 0xff654321u,
          .z = 1.0f,
          .stencil = 0,
          .rectCount = 0,
          .rectOffset = sizeof(D9CCommandChunkWireClearV2),
      }),
  };
}

dxmt9::d3d9::RawCommandChunk makeRaw(const WireFixture& fixture,
                                     std::uint64_t rawOrdinal) {
  dxmt9::d3d9::WireObjectRegistry registry;
  dxmt9::d3d9::RawCommandChunk raw;
  const bool prepared = dxmt9::d3d9::prepareV2OffloadChunk(
      fixture.bytes, fixture.envelope, registry,
      [](std::uint32_t, void*) noexcept {}, raw);
  check(prepared, "session join raw chunk must pass owned preflight");
  raw.replaySeq = rawOrdinal;
  raw.cpuReadyTapePlanningEnabled = true;
  return raw;
}

struct SessionJoinDevice final : dxmt9::Device {
  SessionJoinDevice()
      : queue_(dxmt9::CommandQueue::ArenaLeaseTestQueueTag{}, limits_) {}

  WMT::Device wmtDevice() override { return WMT::Device{NULL_OBJECT_HANDLE}; }
  dxmt9::CommandQueue& queue() override { return queue_; }
  const BackendLimits& limits() const override { return limits_; }
  std::shared_ptr<BackendDevice> backend() override { return {}; }
  bool supportsCpuReadyArenaReplay() const noexcept override { return true; }

  BufferHandle createBuffer(const BufferDesc& desc) override {
    return queue_.pool().createBuffer(WMT::Device{NULL_OBJECT_HANDLE}, desc);
  }

  SurfaceHandle createSurface(const SurfaceDesc&) override {
    return SurfaceHandle{nextHandle_++};
  }

  void submitClear(const ClearDesc& clear) override {
    queue_.submitClear(clear);
  }

  void submitDrawRun(CanonicalDrawState state,
                     const DrawUniformPayload& uniforms,
                     std::span<const DrawParam> draws,
                     std::span<const DrawParamPayloadView> payloads) override {
    queue_.submitDrawRun(std::move(state), uniforms, draws, payloads);
  }

  BackendLimits limits_{};
  dxmt9::CommandQueue queue_;
  std::uint64_t nextHandle_ = 1;
};

struct RuntimeFixture {
  RuntimeFixture() {
    auto upper = std::make_unique<SessionJoinDevice>();
    routing = upper.get();
    factory = dxmt9::com::Direct3DCreate9Ex(
        dxmt9::com::D3D_SDK_VERSION, std::move(upper));
    check(factory != nullptr, "session join factory must construct");

    PresentParameters params{};
    params.backBufferWidth = 16;
    params.backBufferHeight = 16;
    params.backBufferFormat = Format::A8R8G8B8;
    params.windowed = true;
    params.deviceWindow = Handle{92};
    params.presentationInterval = PresentInterval::Immediate;
    device = factory->CreateDeviceEx(0, params, nullptr);
    check(device != nullptr, "session join core device must construct");
    device->AddRef();
    cDevice = std::make_unique<D9CDevice>(device);
  }

  ~RuntimeFixture() {
    cDevice.reset();
    if (device) {
      (void)device->Release();
    }
    if (factory) {
      (void)factory->Release();
    }
  }

  void publishArenaClear(std::uint64_t rawOrdinal) {
    const std::array records{clearRecord()};
    auto raw = makeRaw(makeWireFixture(records), rawOrdinal);
    const auto hr = dxmt9::d3d9::replayRawChunk(cDevice.get(), raw);
    check(hr == D3D_OK, "arena clear source must replay and publish");
  }

  void publishLegacyClear() {
    routing->queue_.submitClear(ClearDesc{});
    check(dxmt9::CommandQueueArenaLeaseTestAccess::publishLegacyWritingSlot(
              routing->queue_),
          "legacy writing slot must publish through the compatibility lane");
  }

  SessionJoinDevice* routing = nullptr;
  dxmt9::com::IDirect3D9Ex* factory = nullptr;
  dxmt9::com::IDirect3DDevice9Ex* device = nullptr;
  std::unique_ptr<D9CDevice> cDevice;
};

struct ProductionLoopBackendCall {
  std::size_t slotIndex = 0;
  std::uint64_t seqId = 0;
  bool arena = false;
  std::uintptr_t session = 0;
  bool deferSessionFinalization = false;
  bool allowInjectedCommandBufferMidChunkCommits = false;
  std::optional<dxmt9::core::metalqueue::QueueCompletionSource>
      sessionSource;
  dxmt9::core::CpuReadyTape::SourceRef partitionSource{};
  std::size_t lookaheadCount = 0;
};

struct ProductionLoopBackendState {
  std::vector<ProductionLoopBackendCall> calls;
  std::atomic<std::size_t> observedBackendCalls{0};
  std::atomic<bool> firstRecordPostCommitRan{false};
  std::atomic<std::size_t> backendCallCountAtFirstRecordSubmit{0};
};

class ProductionLoopBackend final : public dxmt9::render::IRenderBackend {
 public:
  explicit ProductionLoopBackend(
      std::shared_ptr<ProductionLoopBackendState> state)
      : state_(std::move(state)) {}

  std::optional<dxmt9::core::metalqueue::QueueSubmissionRecord> onChunkReady(
      dxmt9::encoders::EncodeContext&,
      std::size_t slotIndex,
      const dxmt9::core::ChunkSlot& slot,
      dxmt9::encoders::EncodeChunkOptions options) override {
    return record(slotIndex, dxmt9::core::SourcePayloadView(slot), slot.seqId,
                  std::move(options));
  }

  std::optional<dxmt9::core::metalqueue::QueueSubmissionRecord> onSourceReady(
      dxmt9::encoders::EncodeContext&,
      std::size_t slotIndex,
      dxmt9::core::SourcePayloadView payload,
      std::uint64_t seqId,
      dxmt9::encoders::EncodeChunkOptions options) override {
    return record(slotIndex, payload, seqId, std::move(options));
  }

  dxmt9::render::BackendMode mode() const override {
    return dxmt9::render::BackendMode::Traditional;
  }

 private:
  std::optional<dxmt9::core::metalqueue::QueueSubmissionRecord> record(
      std::size_t slotIndex,
      dxmt9::core::SourcePayloadView payload,
      std::uint64_t seqId,
      dxmt9::encoders::EncodeChunkOptions options) {
    state_->calls.push_back(ProductionLoopBackendCall{
        .slotIndex = slotIndex,
        .seqId = seqId,
        .arena = payload.isArena(),
        .session = reinterpret_cast<std::uintptr_t>(options.session),
        .deferSessionFinalization = options.deferSessionFinalization,
        .allowInjectedCommandBufferMidChunkCommits =
            options.allowInjectedCommandBufferMidChunkCommits,
        .sessionSource = options.sessionSource,
        .partitionSource = options.partitionSource,
        .lookaheadCount = options.sessionLookaheadSources.size(),
    });
    state_->observedBackendCalls.store(state_->calls.size(),
                                       std::memory_order_release);

    dxmt9::core::metalqueue::QueueSubmissionRecord submission{};
    submission.testOnlyAllowNullCommandBuffer = true;
    submission.slotIndex = slotIndex;
    submission.seqId = seqId;
    if (state_->calls.size() == 1) {
      const auto state = state_;
      submission.postCommitCallbacks.push_back([state] {
        state->backendCallCountAtFirstRecordSubmit.store(
            state->calls.size(), std::memory_order_relaxed);
        state->firstRecordPostCommitRan.store(true, std::memory_order_release);
      });
    }
    return submission;
  }

  std::shared_ptr<ProductionLoopBackendState> state_;
};

bool sameCompletionSource(
    const dxmt9::core::metalqueue::QueueCompletionSource& left,
    const dxmt9::core::metalqueue::QueueCompletionSource& right) {
  return left.source == right.source && left.slotIndex == right.slotIndex &&
      left.seqId == right.seqId && left.hasPresent == right.hasPresent &&
      left.commandBegin == right.commandBegin &&
      left.commandCount == right.commandCount;
}

void runProductionLoopStopDrainCase(std::vector<bool> expectedArenaKinds) {
  RuntimeFixture fixture;
  if (expectedArenaKinds == std::vector<bool>({true, true, true})) {
    fixture.publishArenaClear(1);
    fixture.publishArenaClear(2);
    fixture.publishArenaClear(3);
  } else {
    fixture.publishArenaClear(1);
    fixture.publishLegacyClear();
    fixture.publishArenaClear(2);
  }

  auto& queue = fixture.routing->queue_;
  const auto expectedSources =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);
  check(expectedSources.size() == expectedArenaKinds.size(),
        "production-loop fixture must snapshot every ready source");
  check(expectedSources.size() == 3 && expectedSources[0].seqId == 1 &&
            expectedSources[1].seqId == 2 &&
            expectedSources[2].seqId == 3,
        "production-loop completion snapshot must preserve FIFO seqIds");

  auto backendState = std::make_shared<ProductionLoopBackendState>();
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::make_unique<ProductionLoopBackend>(backendState));

  // All sources are published before stop is set. The production loop must
  // consume the ready prefix before its shutdown-drain fence submits it.
  dxmt9::CommandQueueArenaLeaseTestAccess::
      stopAndRunCpuReadySessionEncodeLoop(queue);

  check(backendState->calls.size() == expectedSources.size(),
        "production loop must hand every selected source to the backend");
  check(backendState->firstRecordPostCommitRan.load(std::memory_order_acquire),
        "the first record callback must run at physical submit");
  check(backendState->backendCallCountAtFirstRecordSubmit.load(
            std::memory_order_relaxed) ==
            expectedSources.size(),
        "the first record must remain pending until all sources merge into "
        "the shutdown-drain submit");

  const auto session = backendState->calls.front().session;
  check(session != 0, "production loop must create a session for the head");
  for (std::size_t i = 0; i < backendState->calls.size(); ++i) {
    const auto& call = backendState->calls[i];
    check(call.slotIndex == expectedSources[i].slotIndex &&
              call.seqId == expectedSources[i].seqId &&
              call.arena == expectedArenaKinds[i],
          "backend calls must preserve snapshotted FIFO source identity");
    check(call.session == session && call.deferSessionFinalization &&
              call.allowInjectedCommandBufferMidChunkCommits,
          "every compatible source must receive the same deferred session "
          "append options");
    check(call.sessionSource.has_value() &&
              sameCompletionSource(*call.sessionSource,
                                   expectedSources[i]),
          "session append options must carry the exact snapshotted source");
    check(call.partitionSource == expectedSources[i].source,
          "partition identity must follow the represented source");
    const std::size_t remaining = expectedSources.size() - i;
    check(call.lookaheadCount == (remaining > 1 ? remaining : 0),
          "production loop must expose the selected FIFO suffix as "
          "session lookahead");
  }

  check(dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(queue) == 0,
        "production loop must consume the whole ready prefix");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::allSourcesSubmitted(
            queue, expectedSources),
        "one shutdown-drain submit must transition every source to GPU");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::residentCount(queue) == 3,
        "submitted sources must remain resident until completion");

  check(dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
            queue, expectedSources) == expectedSources.size(),
        "test completion must expand and finish every merged source");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completedSeqId(queue) == 3,
        "merged completion must advance the FIFO waterline to the tail");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::residentCount(queue) == 0,
        "finish iterations must reclaim all Arena and Legacy residency");
}

void productionLoopJoinsMultipleArenaSourcesOnStopDrain() {
  runProductionLoopStopDrainCase({true, true, true});
}

void productionLoopJoinsMixedSourcesOnStopDrain() {
  runProductionLoopStopDrainCase({true, false, true});
}

template <typename Predicate>
bool waitUntil(Predicate&& predicate,
               std::chrono::milliseconds timeout =
                   std::chrono::milliseconds(2000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

void productionLoopReleasesForActualWriterCapacityWait() {
  RuntimeFixture fixture;
  for (std::uint64_t rawOrdinal = 1;
       rawOrdinal <= dxmt9::kCommandChunkCount; ++rawOrdinal) {
    fixture.publishArenaClear(rawOrdinal);
  }

  auto& queue = fixture.routing->queue_;
  const auto expectedSources =
      dxmt9::CommandQueueArenaLeaseTestAccess::
          snapshotReadyCompletionSources(queue);
  check(expectedSources.size() == dxmt9::kCommandChunkCount,
        "writer-pressure fixture must occupy every queue control shell");

  auto backendState = std::make_shared<ProductionLoopBackendState>();
  dxmt9::CommandQueueArenaLeaseTestAccess::installBackend(
      queue, std::make_unique<ProductionLoopBackend>(backendState));

  std::thread encodeThread([&] {
    dxmt9::CommandQueueArenaLeaseTestAccess::
        runCpuReadySessionEncodeLoop(queue);
  });

  const bool pendingParked = waitUntil([&] {
    return backendState->observedBackendCalls.load(std::memory_order_acquire) ==
           expectedSources.size();
  });
  if (!pendingParked) {
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    encodeThread.join();
    check(false,
          "production loop must encode the full source prefix before parking");
  }

  std::atomic<bool> writerFinished{false};
  std::atomic<bool> writerAcquired{false};
  std::thread writerThread([&] {
    writerAcquired.store(
        dxmt9::CommandQueueArenaLeaseTestAccess::ensureAndAbortEmptyWriter(
            queue),
        std::memory_order_release);
    writerFinished.store(true, std::memory_order_release);
  });

  // No stop or synthetic helper flag is involved: entering the real
  // ensureWriterSlot wait must register writer pressure, wake the parked
  // encode loop, and physically submit its pending record.
  const bool submittedForWriterPressure = waitUntil([&] {
    return backendState->firstRecordPostCommitRan.load(
        std::memory_order_acquire);
  });
  if (!submittedForWriterPressure) {
    dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
    writerThread.join();
    encodeThread.join();
    check(false,
          "actual compatibility-writer pressure must submit the parked "
          "session before shutdown");
  }

  const std::size_t finishedSources =
      dxmt9::CommandQueueArenaLeaseTestAccess::completeAndFinish(
          queue, expectedSources);
  const bool writerProceeded = waitUntil([&] {
    return writerFinished.load(std::memory_order_acquire);
  });

  dxmt9::CommandQueueArenaLeaseTestAccess::requestStop(queue);
  writerThread.join();
  encodeThread.join();

  check(backendState->backendCallCountAtFirstRecordSubmit.load(
            std::memory_order_relaxed) == expectedSources.size(),
        "writer-pressure release must retain the whole encoded FIFO prefix");
  check(finishedSources == expectedSources.size(),
        "writer-pressure submission must complete and reclaim every source");
  check(writerProceeded && writerAcquired.load(std::memory_order_acquire),
        "completion of the pressure-released prefix must unblock the actual "
        "compatibility writer");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::residentCount(queue) == 0,
        "writer-pressure completion must leave no Arena residency");
}

// ---------------------------------------------------------------------------
// Arena payload block fixtures for the pure predicate and encoder-seam tests.

struct ArenaClearBlockFixture {
  std::vector<std::max_align_t> backing;
  SourcePayloadLayout layout{};
  ArenaSourcePayloadBlock block;

  explicit ArenaClearBlockFixture(bool presentTail = false) {
    SourcePayloadCapacity capacity{};
    capacity.commandHeaders = 1;
    if (presentTail) {
      capacity.presentRecords = 1;
    } else {
      capacity.clearRecords = 1;
    }
    const auto built = makeSourcePayloadLayout(capacity, 4096, 64);
    check(built.has_value(), "arena fixture layout must build");
    layout = *built;
    backing.resize((layout.usedBytes + sizeof(std::max_align_t) - 1) /
                   sizeof(std::max_align_t));
    std::span<std::byte> memory{
        reinterpret_cast<std::byte*>(backing.data()),
        backing.size() * sizeof(std::max_align_t)};
    ArenaSourcePayloadBuilder builder(block, layout,
                                      memory.first(layout.usedBytes));
    check(builder.good(), "arena fixture builder must bind");
    if (presentTail) {
      check(builder.tryAppendPresentCommand(PresentCommandRecord{}),
            "arena fixture present record must append");
    } else {
      ClearDesc clear{};
      clear.clearColor = true;
      check(builder.tryAppendClearCommand(clear),
            "arena fixture clear record must append");
    }
    check(builder.publish(), "arena fixture block must publish");
  }

  SourcePayloadView view() const { return SourcePayloadView(block); }
};

void payloadPredicatesAreSourceKindNeutral() {
  // Legacy shapes: the payload-view predicates must agree with the ChunkSlot
  // predicates the H229 carrier uses.
  ChunkSlot head{};
  head.appendClear({});
  ChunkSlot presentTail{};
  presentTail.appendPresent({}, Handle{0x21});
  ChunkSlot midPresent{};
  midPresent.appendPresent({}, Handle{0x22});
  midPresent.appendClear({});

  const auto agree = [](const ChunkSlot& slot, std::string_view what) {
    const SourcePayloadView view(slot);
    check(dxmt9::render::openCbCarrierSourceHasFinalPresentTail(view) ==
              dxmt9::render::openCbCarrierSlotHasFinalPresentTail(slot),
          std::string("present-tail predicate must agree: ") + std::string(what));
    check(dxmt9::render::openCbCarrierSourceCanBeSessionHead(view) ==
              dxmt9::render::openCbCarrierSlotCanBeSessionHead(slot),
          std::string("session-head predicate must agree: ") + std::string(what));
    for (const bool pending : {false, true}) {
      check(dxmt9::render::openCbCarrierSourceCanAppendToPending(view,
                                                                pending) ==
                dxmt9::render::openCbCarrierSlotCanAppendToPending(slot,
                                                                  pending),
            std::string("append predicate must agree: ") + std::string(what));
    }
  };
  agree(head, "clear head");
  agree(presentTail, "present tail");
  agree(midPresent, "mid present");
  agree(ChunkSlot{}, "empty");

  // Arena shapes: a published clear block is a session head; a present-only
  // block is a final present tail.
  ArenaClearBlockFixture arenaClear;
  const auto arenaClearView = arenaClear.view();
  check(arenaClearView.isArena() && arenaClearView.commandCount() == 1,
        "arena clear fixture must expose one arena command");
  check(dxmt9::render::openCbCarrierSourceCanBeSessionHead(arenaClearView),
        "arena clear source must be a session head");
  check(!dxmt9::render::openCbCarrierSourceHasFinalPresentTail(arenaClearView),
        "arena clear source is not a present tail");
  check(dxmt9::render::openCbCarrierSourceCanAppendToPending(arenaClearView,
                                                             true),
        "arena clear source must append to an active session");
  check(!dxmt9::render::openCbCarrierSourceCanAppendToPending(arenaClearView,
                                                              false),
        "arena clear source must not append without a session");

  ArenaClearBlockFixture arenaPresent(/*presentTail=*/true);
  const auto arenaPresentView = arenaPresent.view();
  check(arenaPresentView.presentRecordCount() == 1,
        "arena present fixture must expose one present record");
  check(dxmt9::render::openCbCarrierSourceHasFinalPresentTail(arenaPresentView),
        "arena present-only source is a final present tail");
  check(!dxmt9::render::openCbCarrierSourceCanBeSessionHead(arenaPresentView),
        "arena present tail cannot open a session");
}

void neutralPrefixSelectorAdmitsMixedCandidates() {
  ChunkSlot legacyHead{};
  legacyHead.appendClear({});
  ChunkSlot legacyEmpty{};
  ChunkSlot legacyNonHead{};
  legacyNonHead.appendPresent({}, Handle{0x30});
  legacyNonHead.appendClear({});
  ChunkSlot legacyPresentTail{};
  legacyPresentTail.appendPresent({}, Handle{0x31});
  ArenaClearBlockFixture arenaClear;

  auto candidate = [](SourcePayloadView payload, std::uint64_t seqId) {
    dxmt9::core::metalqueue::ResolvedPublishedSource source{};
    source.payload = payload;
    source.seqId = seqId;
    source.slot = payload.legacyPayload();
    return source;
  };

  const std::array mixed{
      candidate(arenaClear.view(), 1),
      candidate(SourcePayloadView(legacyHead), 2),
      candidate(SourcePayloadView(legacyPresentTail), 3),
  };
  check(dxmt9::render::selectCpuReadySessionBatchPrefix(mixed) == 3,
        "neutral selector must admit arena+legacy heads through the tail");
  check(dxmt9::render::selectOpenCbCarrierBatchPrefix(mixed) == 0,
        "legacy-only H229 selector must still reject an arena-first prefix");

  const std::array arenaOnly{
      candidate(arenaClear.view(), 1),
      candidate(arenaClear.view(), 2),
  };
  check(dxmt9::render::selectCpuReadySessionBatchPrefix(arenaOnly) == 2,
        "neutral selector must admit a multi-arena head run");

  const std::array tailFirst{
      candidate(SourcePayloadView(legacyPresentTail), 1),
      candidate(arenaClear.view(), 2),
  };
  check(dxmt9::render::selectCpuReadySessionBatchPrefix(tailFirst) == 0,
        "a leading present tail still falls back to single-source dequeue");

  const std::array invalidAfterHeads{
      candidate(arenaClear.view(), 1),
      candidate(SourcePayloadView(legacyHead), 2),
      candidate(SourcePayloadView{}, 3),
  };
  check(dxmt9::render::selectCpuReadySessionBatchPrefix(invalidAfterHeads) ==
            2,
        "a later invalid source must preserve the maximal valid head prefix");

  const std::array emptyAfterHeads{
      candidate(arenaClear.view(), 1),
      candidate(SourcePayloadView(legacyHead), 2),
      candidate(SourcePayloadView(legacyEmpty), 3),
  };
  check(dxmt9::render::selectCpuReadySessionBatchPrefix(emptyAfterHeads) == 2,
        "a later empty source must preserve the maximal valid head prefix");

  const std::array nonHeadAfterHead{
      candidate(arenaClear.view(), 1),
      candidate(SourcePayloadView(legacyNonHead), 2),
  };
  check(dxmt9::render::selectCpuReadySessionBatchPrefix(nonHeadAfterHead) == 1,
        "a later non-head source must preserve the maximal valid head prefix");

  const std::array leadingInvalid{
      candidate(SourcePayloadView{}, 1),
      candidate(arenaClear.view(), 2),
  };
  check(dxmt9::render::selectCpuReadySessionBatchPrefix(leadingInvalid) == 0,
        "a leading invalid source must still reject the batch prefix");

  const std::array leadingEmpty{
      candidate(SourcePayloadView(legacyEmpty), 1),
      candidate(arenaClear.view(), 2),
  };
  check(dxmt9::render::selectCpuReadySessionBatchPrefix(leadingEmpty) == 0,
        "a leading empty source must still reject the batch prefix");

  const std::array leadingNonHead{
      candidate(SourcePayloadView(legacyNonHead), 1),
      candidate(arenaClear.view(), 2),
  };
  check(dxmt9::render::selectCpuReadySessionBatchPrefix(leadingNonHead) == 0,
        "a leading non-head source must still reject the batch prefix");
}

// ---------------------------------------------------------------------------
// Encoder seam: two Arena sources share one session and one command buffer.

template <typename WmtType>
WMT::Reference<WmtType> retainedToken(const char* label) {
  auto owner = WMT::MakeString(label, WMTUTF8StringEncoding);
  return WMT::Reference<WmtType>(WmtType{owner.handle});
}

struct EncoderHarness {
  dxmt9::core::BackendLimits limits{};
  dxmt9::resources::Pool pool{};
  dxmt9::pipeline::Cache cache{};
  dxmt9::scratch::FrameAllocators allocators{};
  WMT::Reference<WMT::Device> device =
      retainedToken<WMT::Device>("session-join-device-token");
  dxmt9::CommandQueue queue;

  EncoderHarness()
      : queue(dxmt9::CommandQueue::InertTestQueueTag{},
              retainedToken<WMT::CommandQueue>("session-join-queue-token"),
              limits) {}

  dxmt9::encoders::EncodeContext makeContext() {
    return dxmt9::encoders::EncodeContext{
        device,
        limits,
        pool,
        cache,
        allocators,
        nullptr,
        nullptr,
        queue,
    };
  }
};

dxmt9::core::metalqueue::QueueCompletionSource arenaSessionSource(
    std::size_t slotIndex,
    std::uint64_t seqId) {
  return dxmt9::core::metalqueue::QueueCompletionSource{
      .source = {
          .id = {
              .index = static_cast<std::uint32_t>(slotIndex),
              .generation = seqId,
          },
          .storage = {
              .firstPage = static_cast<std::uint32_t>(slotIndex),
              .pageCount = 1,
              .generation = seqId,
          },
      },
      .slotIndex = slotIndex,
      .seqId = seqId,
      .hasPresent = false,
      .commandBegin = 0,
      .commandCount = 1,
  };
}

void arenaSessionCarryAcrossSourcesSharesOneCommandBuffer() {
  EncoderHarness harness;
  auto ctx = harness.makeContext();
  auto session = dxmt9::encoders::makeEncodeChunkSession();
  check(static_cast<bool>(session), "session owner is created");

  ArenaClearBlockFixture headBlock;
  ArenaClearBlockFixture tailBlock;
  const auto headSource = arenaSessionSource(2, 41);
  const auto tailSource = arenaSessionSource(3, 42);

  auto commandBuffer =
      retainedToken<WMT::CommandBuffer>("session-join-cb-token");
  const obj_handle_t commandBufferHandle = commandBuffer.handle;

  auto makeOptions = [&session](
                         WMT::Reference<WMT::CommandBuffer> cb,
                         const dxmt9::core::metalqueue::QueueCompletionSource&
                             source) {
    dxmt9::encoders::EncodeChunkOptions options{};
    options.commandBuffer = std::move(cb);
    options.allowInjectedCommandBufferMidChunkCommits = true;
    options.session = session.get();
    options.deferSessionFinalization = true;
    options.sessionSource = source;
    options.partitionSource = source.source;
    return options;
  };

  auto head = dxmt9::encoders::encodeChunk(
      ctx, headSource.slotIndex, headBlock.view(), headSource.seqId,
      makeOptions(std::move(commandBuffer), headSource));
  check(head.has_value(),
        "arena head encodeChunk must return a deferred submission");
  check(head->commandBuffer.handle == commandBufferHandle,
        "arena head submission carries the injected command buffer");
  check(head->explicitCompletionSourceSpan().empty(),
        "deferred arena head does not publish session sources");
  check(dxmt9::encoders::encodeChunkSessionHasDeferredSubmissionPayload(
            *session),
        "arena head clear remains deferred session payload");

  auto tail = dxmt9::encoders::encodeChunk(
      ctx, tailSource.slotIndex, tailBlock.view(), tailSource.seqId,
      makeOptions(std::move(head->commandBuffer), tailSource));
  check(tail.has_value(),
        "arena tail encodeChunk must return a deferred submission");
  check(tail->commandBuffer.handle == commandBufferHandle,
        "arena source boundary must not open a new command buffer");

  const auto sources = dxmt9::encoders::encodeChunkSessionSources(*session);
  check(sources.size() == 2 && sources[0].seqId == headSource.seqId &&
            sources[1].seqId == tailSource.seqId,
        "session must retain both arena sources in FIFO order");

  check(dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, *tail),
        "finalizer publishes the deferred arena session");
  const auto published = tail->explicitCompletionSourceSpan();
  check(published.size() == 2 && published[0].seqId == headSource.seqId &&
            published[1].seqId == tailSource.seqId &&
            tail->commandBuffer.handle == commandBufferHandle,
        "one submission covers both arena sources with FIFO attribution");
}

// ---------------------------------------------------------------------------
// Queue seam: mixed Arena/Legacy prefix through one submission.

void multipleArenaSourcesCompleteFifoThroughOneSubmission() {
  RuntimeFixture fixture;
  fixture.publishArenaClear(1);
  fixture.publishArenaClear(2);
  fixture.publishArenaClear(3);
  check(dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
            fixture.routing->queue_) == 3,
        "three arena sources must be ready");

  const auto batch = dxmt9::CommandQueueArenaLeaseTestAccess::consumeBatch(
      fixture.routing->queue_, 8);
  check(batch.dequeued == 3 && batch.retained == 3,
        "the neutral prefix must represent all three arena sources at once");
  check(batch.arenaKinds == std::vector<bool>({true, true, true}),
        "every represented source must resolve as an arena payload");
  check(batch.seqIds == std::vector<std::uint64_t>({1, 2, 3}),
        "represented arena sources must retain FIFO seqId order");
  check(batch.submitted && batch.completed,
        "one submitted record must cover the whole arena prefix");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completedSeqId(
            fixture.routing->queue_) == 3,
        "completion must expand all three sources in FIFO order");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::residentCount(
            fixture.routing->queue_) == 0,
        "ordered reclaim must release every arena source");
}

void mixedLegacyAndArenaSourcesShareOneSubmission() {
  RuntimeFixture fixture;
  fixture.publishArenaClear(1);
  fixture.publishLegacyClear();
  fixture.publishArenaClear(2);
  check(dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
            fixture.routing->queue_) == 3,
        "arena/legacy/arena sources must all be ready");

  const auto batch = dxmt9::CommandQueueArenaLeaseTestAccess::consumeBatch(
      fixture.routing->queue_, 8);
  check(batch.dequeued == 3 && batch.retained == 3,
        "the neutral prefix must admit the mixed arena/legacy run");
  check(batch.arenaKinds == std::vector<bool>({true, false, true}),
        "represented kinds must interleave arena and legacy sources");
  check(batch.seqIds == std::vector<std::uint64_t>({1, 2, 3}),
        "mixed sources must retain FIFO seqId order");
  check(batch.submitted && batch.completed,
        "one submitted record must cover the mixed prefix without an "
        "artificial submission boundary");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::completedSeqId(
            fixture.routing->queue_) == 3,
        "mixed completion must expand per source in FIFO order");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::residentCount(
            fixture.routing->queue_) == 0,
        "mixed ordered reclaim must release every source");
}

}  // namespace

int main() {
  try {
    payloadPredicatesAreSourceKindNeutral();
    neutralPrefixSelectorAdmitsMixedCandidates();
    arenaSessionCarryAcrossSourcesSharesOneCommandBuffer();
    multipleArenaSourcesCompleteFifoThroughOneSubmission();
    mixedLegacyAndArenaSourcesShareOneSubmission();
    productionLoopJoinsMultipleArenaSourcesOnStopDrain();
    productionLoopJoinsMixedSourcesOnStopDrain();
    productionLoopReleasesForActualWriterCapacityWait();
  } catch (const TestFailure& error) {
    std::cerr << "cpu_ready_session_join_spec failed: " << error.what()
              << '\n';
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "cpu_ready_session_join_spec unexpected error: "
              << error.what() << '\n';
    return 1;
  }
  std::cout << "cpu_ready_session_join_spec passed\n";
  return 0;
}
