#include "../../../src/dxmt9/dxmt9_command_queue.hpp"
#include "../../../src/dxmt9/dxmt9_source_payload.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

namespace dxmt9 {

struct CommandQueueArenaLeaseTestAccess {
  struct LifecycleResult {
    bool dequeued = false;
    bool resolvedArena = false;
    bool sawClear = false;
    bool submitted = false;
    bool completed = false;
    bool reclaimed = false;
    bool staleResolveRejected = false;
    std::size_t commandCount = 0;
    std::size_t residentSources = 0;
    std::uint64_t generationBefore = 0;
    std::uint64_t generationAfter = 0;
  };

  static void ensureEmptyLegacyWriter(CommandQueue& queue) {
    std::unique_lock lock(queue.mutex_);
    (void)queue.queueLifecycle_.ensureWriterSlot(lock, kMaxQueuedChunks);
  }

  static void setWriteIndex(CommandQueue& queue, std::size_t index) {
    std::lock_guard lock(queue.mutex_);
    queue.writeIndex_ = index;
  }

  static void setCurrentBackBuffer(CommandQueue& queue, core::Handle handle) {
    std::lock_guard lock(queue.mutex_);
    queue.currentBackBuffer_ = handle;
  }

  static void setStopped(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    queue.stop_ = true;
  }

  static void stopCpuReadyTape(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    queue.cpuReadyTape_.stopAdmission();
  }

  static void stopAfterCompatibilityFlush(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    queue.testOnlyStopCpuReadyArenaAfterCompatibilityFlush_ = true;
  }

  static bool injectCompletedSettlement(
      CommandQueue& queue, core::CpuReadyTape::ArenaGroupSettlement settlement) {
    std::lock_guard lock(queue.mutex_);
    auto* ledger = queue.queueLifecycle_.completedArenaGroupSettlementLedger();
    if (!ledger->append(settlement) ||
        !queue.queueLifecycle_.drainCompletedArenaGroupSettlementsLocked(
            settlement.tailSeqId)) {
      return false;
    }
    queue.finishCv_.notify_all();
    return true;
  }

  static void forceNextBatchRollbackFailure(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    queue.testOnlyForceNextCpuReadyArenaRollbackFailure_ = true;
  }

  static void armBatchPlannerObservation(CommandQueue& queue,
                                         bool forceInvalid) {
    std::lock_guard lock(queue.mutex_);
    queue.testOnlyObserveNextCpuReadyArenaPlanner_ = true;
    queue.testOnlyCpuReadyArenaPlannerInvocationCount_ = 0;
    queue.testOnlyCpuReadyArenaPlannerValid_ = false;
    queue.testOnlyForceNextCpuReadyArenaPlannerInvalid_ = forceInvalid;
  }

  static std::uint32_t batchPlannerInvocationCount(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.testOnlyCpuReadyArenaPlannerInvocationCount_;
  }

  static bool batchPlannerValid(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.testOnlyCpuReadyArenaPlannerValid_;
  }

  static std::size_t readyCount(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.cpuReadyTape_.readyCount();
  }

  static CommandQueue::CpuReadyArenaFailureSnapshot activeFailure(
      CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.arenaBuildContext_
        ? queue.arenaBuildContext_->firstFailure()
        : CommandQueue::CpuReadyArenaFailureSnapshot{};
  }

  static bool activeFlushDeferred(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.arenaBuildContext_ &&
           queue.arenaBuildContext_->flushAfterPublication;
  }

  static core::PresentId registerFakePresenter(CommandQueue& queue) {
    return queue.registerPresenter(
        reinterpret_cast<Presenter*>(static_cast<std::uintptr_t>(1)));
  }

  static void failNextPresenterRegistration(CommandQueue& queue) {
    std::lock_guard lock(queue.presenterRegistryMutex_);
    queue.testOnlyFailNextPresenterRegistration_ = true;
  }

  static bool appendStashedPresent(CommandQueue& queue,
                                   core::PresentId id) {
    core::SwapDesc desc{};
    desc.presentId = id;
    desc.pacedByPresentOrdinal = true;
    return queue.appendActiveArenaPresent(
               std::move(desc), BoundaryPolicy::Default,
               /*tokenStashed=*/true) ==
           CommandQueue::ActiveArenaAppendResult::Appended;
  }

  static core::Handle currentBackBuffer(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.currentBackBuffer_;
  }

  static std::uint64_t nextSeqId(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.nextSeqId_;
  }

  static std::size_t writeIndex(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.writeIndex_;
  }

  static core::ChunkSlotControl control(CommandQueue& queue,
                                        std::size_t index) {
    std::lock_guard lock(queue.mutex_);
    return queue.slots_[index];
  }

  static void enableArenaAdmissionWaitObservation(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    queue.testOnlySchedulingWaitObservationEnabled_ = true;
    queue.testOnlyArenaAdmissionWaitEntries_ = 0;
    queue.testOnlyArenaAdmissionPredicateEvaluations_ = 0;
  }

  static bool waitForArenaAdmissionPredicateEvaluations(
      CommandQueue& queue, std::uint64_t expected) {
    std::unique_lock lock(queue.mutex_);
    return queue.sessionReleaseCv_.wait_for(
        lock, std::chrono::seconds(2), [&] {
          return queue.testOnlyArenaAdmissionPredicateEvaluations_ >= expected;
        });
  }

  static std::array<std::uint64_t, 2> arenaAdmissionWaitObservations(
      CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return {queue.testOnlyArenaAdmissionWaitEntries_,
            queue.testOnlyArenaAdmissionPredicateEvaluations_};
  }

  static void setControlStateAndWake(
      CommandQueue& queue, std::size_t index,
      core::ChunkSlot::State state) {
    {
      std::lock_guard lock(queue.mutex_);
      queue.slots_[index] = {};
      queue.slots_[index].state = state;
    }
    queue.writeCv_.notify_all();
  }

  static bool waitForArenaAdmission(
      CommandQueue& queue,
      std::span<const core::ArenaSourcePayloadLayout> layouts) {
    return queue.waitForCpuReadyArenaAdmission(layouts);
  }

  static bool admissionIsFullyVisible(
      CommandQueue& queue,
      core::CpuReadyPublicationTicket ticket,
      std::size_t controlIndex) {
    std::lock_guard lock(queue.mutex_);
    if (!queue.arenaAdmissionActive_.load(std::memory_order_relaxed) ||
        queue.activeArenaBuild_.load(std::memory_order_relaxed) !=
            (queue.arenaBuildContext_ ? &*queue.arenaBuildContext_ : nullptr) ||
        !queue.arenaBuildContext_ ||
        queue.arenaBuildContext_->controlIndex != controlIndex ||
        queue.arenaBuildContext_->reservation.ticket != ticket) {
      return false;
    }
    const auto memory = queue.cpuReadyTape_.writableArenaSegment(ticket, 0);
    return queue.arenaBuildContext_->layout.segmentCount == 1 &&
           memory.size() ==
               queue.arenaBuildContext_->layout.segments[0].layout.usedBytes &&
           queue.arenaBuildContext_->reservation.arenaPayload->boundTo(
               memory);
  }

  static const core::ArenaSourcePayloadBlock* publishedArena(
      CommandQueue& queue,
      core::CpuReadyPublicationTicket ticket) {
    std::lock_guard lock(queue.mutex_);
    return queue.cpuReadyTape_.resolveArena(
        ticket.id, ticket.storage, core::CpuReadyTape::State::Ready);
  }

  static std::size_t residentSources(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.cpuReadyTape_.stats().residentSources;
  }

  static LifecycleResult publishToReclaim(
      CommandQueue& queue,
      core::CpuReadyPublicationTicket ticket) {
    using namespace core;
    using namespace core::metalqueue;
    LifecycleResult result{};
    ReadySlotSnapshot locator{};
    QueueCompletionSource completionSource{};
    {
      std::unique_lock lock(queue.mutex_);
      result.generationBefore =
          queue.cpuReadyTape_.sourceGenerationAt(ticket.id.index);
      result.dequeued = queue.queueLifecycle_.dequeueReadySlot(lock, locator);
      if (!result.dequeued) {
        return result;
      }
      {
        const auto resolved =
            queue.queueLifecycle_.resolveRepresentedSource(locator);
        result.resolvedArena = resolved.valid() && resolved.payload.isArena() &&
                               resolved.slot == nullptr;
        result.commandCount = resolved.payload.commandCount();
        result.sawClear = result.commandCount == 1u &&
                          resolved.payload.commandAt(0).kind() ==
                              MetalCommandKind::Clear;
      }

      QueueSubmissionRecord record{};
      record.testOnlyAllowNullCommandBuffer = true;
      record.slotIndex = locator.slotIndex;
      record.seqId = locator.seqId;
      completionSource = completionSourceForReadySlot(locator);
      const std::array sources{completionSource};
      if (!record.assignFixedCompletionSources(sources)) {
        return result;
      }
      result.submitted =
          queue.queueLifecycle_.submitEncodedSubmission(lock, record);
    }
    if (!result.submitted) {
      return result;
    }

    QueueLifecycleController::PendingCompletion pending{};
    pending.slotIndex = completionSource.slotIndex;
    pending.seqId = completionSource.seqId;
    const std::array pendingSources{completionSource};
    if (!pending.assignFixedCompletionSources(pendingSources)) {
      return result;
    }
    queue.queueLifecycle_.enqueuePendingCompletionForTest(std::move(pending));
    result.completed = queue.queueLifecycle_.processOnePendingCompletion();

    {
      std::unique_lock lock(queue.mutex_);
      result.reclaimed = result.completed &&
          queue.queueLifecycle_.runFinishIteration(lock);
      result.residentSources = queue.cpuReadyTape_.residentCount();
      result.generationAfter =
          queue.cpuReadyTape_.sourceGenerationAt(ticket.id.index);
      result.staleResolveRejected =
          !queue.cpuReadyTape_.resolveSourcePayload(
              ticket.id, ticket.storage, CpuReadyTape::State::Ready).valid();
    }
    return result;
  }
};

}  // namespace dxmt9

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

dxmt9::core::SourcePayloadCapacity groupedDrawCapacity() {
  using namespace dxmt9::core;
  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 2;
  capacity.drawHotStates = 2;
  capacity.drawShaderLayouts = 2;
  capacity.drawDebugSnapshots = 2;
  capacity.drawPsoSubviews = 2;
  capacity.drawUniformFixedPayloads = 3;
  capacity.drawUniformVertexConstants = 3;
  capacity.drawUniformVertexConstantBytes =
      3 * sizeof(VertexShaderConstants);
  capacity.drawUniformPixelConstants = 3;
  capacity.drawUniformPixelConstantBytes = 3 * sizeof(PixelShaderConstants);
  capacity.drawUniformPayloads = 3;
  capacity.drawParams = 3;
  capacity.drawPayloadBytes =
      4 * (sizeof(DrawBindingOverride) + sizeof(DrawBindingSnapshot));
  capacity.drawRunRecords = 2;
  return capacity;
}

dxmt9::core::SourcePayloadCapacity singleDrawCapacity(
    std::size_t commandCount = 1) {
  using namespace dxmt9::core;
  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = commandCount;
  capacity.drawHotStates = commandCount;
  capacity.drawShaderLayouts = commandCount;
  capacity.drawDebugSnapshots = commandCount;
  capacity.drawPsoSubviews = commandCount;
  capacity.drawUniformFixedPayloads = commandCount;
  capacity.drawUniformVertexConstants = commandCount;
  capacity.drawUniformVertexConstantBytes =
      commandCount * sizeof(VertexShaderConstants);
  capacity.drawUniformPixelConstants = commandCount;
  capacity.drawUniformPixelConstantBytes =
      commandCount * sizeof(PixelShaderConstants);
  capacity.drawUniformPayloads = commandCount;
  capacity.drawParams = commandCount;
  capacity.drawPayloadBytes = 4096;
  capacity.drawRunRecords = commandCount;
  return capacity;
}

dxmt9::core::ArenaSourcePayloadLayout makeLayout(
    const dxmt9::core::SourcePayloadCapacity& capacity) {
  const auto segment =
      dxmt9::core::makeSourcePayloadLayout(capacity, 4096, 64);
  check(segment.has_value(), "arena lease fixture segment must build");
  const std::array segments{*segment};
  const auto layout = dxmt9::core::makeArenaSourcePayloadLayout(
      segments, 4096, 64);
  check(layout.has_value(), "arena lease fixture layout must build");
  return *layout;
}

struct SyntheticBufferHandleGuard {
  dxmt9::resources::BufferRecord* record = nullptr;

  ~SyntheticBufferHandleGuard() {
    if (!record) {
      return;
    }
    record->buffer.handle = NULL_OBJECT_HANDLE;
    for (auto& entry : record->renameRing) {
      entry.buffer.handle = NULL_OBJECT_HANDLE;
    }
  }
};

dxmt9::core::DrawRunSubmission materializedSubmission(
    dxmt9::core::Handle attachment,
    std::uint64_t stateGeneration,
    std::uint64_t uniformGeneration) {
  using namespace dxmt9::core;
  DrawRunSubmission submission{
      .state = CanonicalDrawState{},
      .uniforms = DrawUniformPayload{},
      .draw = DrawParam{.primitiveCount = 1},
      .stateGeneration = stateGeneration,
      .uniformGeneration = uniformGeneration,
      .stateLane = DrawRunSubmissionStateLane::FullNoIndex,
  };
  submission.materializedState().hot.colorAttachments[0].handle = attachment;
  return submission;
}

void testExplicitControlIndexGroupingAndExactSnapshotMark() {
  using namespace dxmt9;
  using namespace dxmt9::core;

  static_assert(!std::is_copy_constructible_v<
                CommandQueue::CpuReadyArenaBuildLease>);
  static_assert(std::is_move_constructible_v<
                CommandQueue::CpuReadyArenaBuildLease>);

  CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  CommandQueueArenaLeaseTestAccess::setWriteIndex(queue, 5);
  const Handle oldBackBuffer{70};
  CommandQueueArenaLeaseTestAccess::setCurrentBackBuffer(queue, oldBackBuffer);

  BufferDesc bufferDesc{};
  bufferDesc.size = 64;
  bufferDesc.pool = Pool::Default;
  bufferDesc.usage = UsageDynamic;
  const auto buffer = queue.pool().createBuffer(
      WMT::Device{NULL_OBJECT_HANDLE}, bufferDesc);
  auto* bufferRecord = queue.pool().findBuffer(buffer.value);
  check(bufferRecord && bufferRecord->renameRing.size() == 1,
        "dynamic buffer fixture must seed a rename ring");
  bufferRecord->renameRing.emplace_back();
  SyntheticBufferHandleGuard handleGuard{bufferRecord};
  constexpr obj_handle_t activeBacking = 0x1234u;
  constexpr obj_handle_t capturedBacking = 0x5678u;
  bufferRecord->buffer.handle = activeBacking;
  bufferRecord->renameRing[0].buffer.handle = activeBacking;
  bufferRecord->renameRing[1].buffer.handle = capturedBacking;

  const auto layout = makeLayout(groupedDrawCapacity());
  auto lease = queue.beginCpuReadyArenaSource(101, layout);
  check(lease.has_value(), "direct arena admission must succeed");
  const auto ticket = lease->ticket();
  check(ticket.id.index == 0,
        "fixture must allocate source index zero");
  check(CommandQueueArenaLeaseTestAccess::admissionIsFullyVisible(
            queue, ticket, 5),
        "reserve, Tape storage bind, context emplace, and active publication "
        "must all be visible under the scheduling mutex");
  check(CommandQueueArenaLeaseTestAccess::nextSeqId(queue) ==
            ticket.seqId + 1,
        "strict admission must consume nextSeqId immediately");

  std::array<DrawRunSubmission, 3> submissions{
      materializedSubmission(Handle{11}, 7, 9),
      DrawRunSubmission{},
      materializedSubmission(Handle{22}, 8, 10),
  };
  submissions[1].state.reset();
  submissions[1].uniforms.reset();
  submissions[1].stateMaterialized = false;
  submissions[1].stateGeneration = 7;
  submissions[1].uniformGeneration = 9;
  submissions[1].stateLane = DrawRunSubmissionStateLane::FullNoIndex;
  submissions[1].draw.primitiveCount = 1;
  submissions[1].bindingOverride.streamMask = 1;
  submissions[1].bindingOverride.streams[0].buffer = buffer;
  submissions[1].bindingOverride.streams[0].stride = 16;
  DrawBindingSnapshot captured{};
  captured.streamMask = 1;
  captured.streams[0].buffer = buffer;
  captured.streams[0].stride = 16;
  captured.streams[0].snapshot.metalHandle = capturedBacking;
  submissions[1].payload.bindingSnapshotData =
      drawBindingSnapshotBytes(captured);

  queue.submitDrawRunBatch(submissions);
  check(CommandQueueArenaLeaseTestAccess::currentBackBuffer(queue) ==
            oldBackBuffer,
        "active appends must not expose pending backbuffer semantics");

  const std::array<ChunkHandleEntry, 1> resources{{
      ChunkHandleEntry{.kind = ChunkHandleKind::Buffer, .handle = buffer},
  }};
  check(lease->publish(resources), "valid grouped arena source must publish");
  check(CommandQueueArenaLeaseTestAccess::currentBackBuffer(queue) ==
            Handle{22},
        "publish lock must commit the final draw-group backbuffer semantic");
  check(CommandQueueArenaLeaseTestAccess::writeIndex(queue) == 6,
        "publish must advance from the selected control index");
  const auto selectedControl =
      CommandQueueArenaLeaseTestAccess::control(queue, 5);
  const auto unrelatedControl =
      CommandQueueArenaLeaseTestAccess::control(queue, ticket.id.index);
  check(selectedControl.state == ChunkSlot::State::Pending &&
            selectedControl.sourceId == ticket.id &&
            unrelatedControl.state == ChunkSlot::State::Free,
        "source index and explicit control index must remain independent");

  const auto* arena =
      CommandQueueArenaLeaseTestAccess::publishedArena(queue, ticket);
  check(arena != nullptr, "published arena owner must resolve from Tape");
  const SourcePayloadView view(*arena);
  check(view.valid() && view.commandCount() == 2 &&
            view.commandAt(0).kind() == MetalCommandKind::DrawRun &&
            view.commandAt(1).kind() == MetalCommandKind::DrawRun,
        "incompatible attachment states must remain two ordered DrawRuns");
  const auto firstRun = view.commandAt(0).command;
  const auto secondRun = view.commandAt(1).command;
  check(firstRun.drawParams.size() == 2 &&
            secondRun.drawParams.size() == 1,
        "compatible elided draw must stay in its accepted group");
  check(firstRun.drawParams[1].bindingOverrideRange.size ==
            sizeof(DrawBindingOverride) &&
            firstRun.drawParams[1].bindingSnapshotRange.size ==
                sizeof(DrawBindingSnapshot),
        "elided draw must retain prepared override and captured snapshot");
  check(bufferRecord->lastUsedSeqId == ticket.seqId &&
            bufferRecord->renameRing[0].lastUsedSeqId == ticket.seqId &&
            bufferRecord->renameRing[1].lastUsedSeqId == ticket.seqId,
        "logical, active, and captured backing uses must be marked with the "
        "exact strict ticket seqId");
}

void testActiveMismatchFailStopsWithoutFallback() {
  using namespace dxmt9;
  using namespace dxmt9::core;

  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 1;
  capacity.clearRecords = 1;
  const auto layout = makeLayout(capacity);
  CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  auto lease = queue.beginCpuReadyArenaSource(1, layout);
  check(lease.has_value(), "mismatch fixture admission must succeed");
  const auto ticket = lease->ticket();
  queue.submitReadback(ReadbackDesc{});
  check(!lease->publish(),
        "unexpected direct-lane command must make publish fail");
  check(queue.cpuReadyArenaPoisoned(),
        "post-admission mismatch must poison the direct queue lane");
  check(CommandQueueArenaLeaseTestAccess::residentSources(queue) == 0 &&
            CommandQueueArenaLeaseTestAccess::nextSeqId(queue) ==
                ticket.seqId + 1,
        "two-phase abort must reclaim ownership without reusing identity");
  check(CommandQueueArenaLeaseTestAccess::control(queue, 0).state ==
            ChunkSlot::State::Free &&
            CommandQueueArenaLeaseTestAccess::publishedArena(queue, ticket) ==
                nullptr,
        "failed direct source must leave no legacy or arena publication");
}

void testLegacyWriterBoundaryBeforeStrictAdmission() {
  using namespace dxmt9;
  using namespace dxmt9::core;

  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 1;
  capacity.clearRecords = 1;
  const auto layout = makeLayout(capacity);

  {
    CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
    CommandQueueArenaLeaseTestAccess::ensureEmptyLegacyWriter(queue);
    auto lease = queue.beginCpuReadyArenaSource(1, layout);
    check(lease.has_value() && lease->seqId() == 1,
          "empty legacy writer must abort without consuming a seq identity");
    ClearDesc clear{};
    clear.colorAttachments[0].handle = Handle{31};
    queue.submitClear(clear);
    check(lease->publish(), "arena source after empty writer must publish");
  }

  {
    CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
    ClearDesc legacyClear{};
    legacyClear.colorAttachments[0].handle = Handle{41};
    queue.submitClear(legacyClear);
    auto lease = queue.beginCpuReadyArenaSource(2, layout);
    check(lease.has_value() && lease->seqId() == 2,
          "non-empty legacy writer must publish before strict admission");
    const auto legacyControl =
        CommandQueueArenaLeaseTestAccess::control(queue, 0);
    check(legacyControl.state == ChunkSlot::State::Pending &&
              legacyControl.seqId == 1,
          "legacy publication must keep its original control and seq");
    ClearDesc arenaClear{};
    arenaClear.colorAttachments[0].handle = Handle{42};
    queue.submitClear(arenaClear);
    check(lease->publish(),
          "strict source after non-empty legacy publication must publish");
    check(CommandQueueArenaLeaseTestAccess::currentBackBuffer(queue) ==
              Handle{42},
          "strict publish must commit semantics after the legacy boundary");
  }
}

void testActiveSingleDrawPreservesUpAndStateBlockBypass() {
  using namespace dxmt9;
  using namespace dxmt9::core;

  const auto layout = makeLayout(singleDrawCapacity(2));
  CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  auto begin = queue.beginCpuReadyArenaSource(7, layout);
  check(begin.status == CommandQueue::CpuReadyArenaBeginStatus::Ready,
        "single-draw fixture must receive typed Ready admission");
  const auto ticket = begin->ticket();

  std::array<u8, 12> upVertices{};
  std::array<u8, 6> upIndices{};
  CanonicalDrawState upState{};
  upState.hot.colorAttachments[0].handle = Handle{51};
  const DrawParam upDraw{
      .primitiveCount = 1,
      .indexed = true,
  };
  const DrawParamPayloadView upPayload{
      .userVertexData = upVertices,
      .userIndexData = upIndices,
  };
  queue.submitDrawRun(std::move(upState), DrawUniformPayload{},
                      std::span<const DrawParam>(&upDraw, 1),
                      std::span<const DrawParamPayloadView>(&upPayload, 1));

  // State-block recording disables replay batching and reaches the same
  // single-run queue ingress with ordinary (non-UP) draw parameters.
  CanonicalDrawState stateBlockState{};
  stateBlockState.hot.colorAttachments[0].handle = Handle{52};
  const DrawParam stateBlockDraw{.primitiveCount = 2};
  queue.submitDrawRun(std::move(stateBlockState), DrawUniformPayload{},
                      std::span<const DrawParam>(&stateBlockDraw, 1));
  check(begin->publish(),
        "active single-run ingress must publish without legacy fallback");

  const auto* arena =
      CommandQueueArenaLeaseTestAccess::publishedArena(queue, ticket);
  check(arena != nullptr, "single-run source must resolve from Tape");
  const SourcePayloadView view(*arena);
  check(view.commandCount() == 2 &&
            view.commandAt(0).command.drawParams.size() == 1 &&
            view.commandAt(1).command.drawParams.size() == 1,
        "UP and state-block bypass draws must remain separate DrawRuns");
  const auto& upParam = view.commandAt(0).command.drawParams[0];
  check(upParam.userVertexRange.size == upVertices.size() &&
            upParam.userIndexRange.size == upIndices.size(),
        "single-run arena assembly must preserve UP vertex/index payloads");
  check(CommandQueueArenaLeaseTestAccess::currentBackBuffer(queue) ==
            Handle{52},
        "single-run publish must commit final state-block draw semantics");
}

void testTypedBeginStatusAndMissingAdmissionSnapshot() {
  using namespace dxmt9;
  using namespace dxmt9::core;

  const auto layout = makeLayout(singleDrawCapacity());
  {
    CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
    auto invalid = queue.beginCpuReadyArenaSource(0, layout);
    check(invalid.status == CommandQueue::CpuReadyArenaBeginStatus::Invalid &&
              !invalid,
          "typed begin must distinguish invalid admission input");
    auto active = queue.beginCpuReadyArenaSource(1, layout);
    auto pressure = queue.beginCpuReadyArenaSource(2, layout);
    check(active &&
              pressure.status ==
                  CommandQueue::CpuReadyArenaBeginStatus::TemporaryPressure &&
              pressure.stopReason ==
                  CommandQueue::CpuReadyArenaBeginStopReason::None &&
              !pressure,
          "typed begin must expose an overlapping transaction as pressure");
  }
  {
    CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
    CommandQueueArenaLeaseTestAccess::setStopped(queue);
    const auto stopped = queue.beginCpuReadyArenaSource(1, layout);
    check(stopped.status == CommandQueue::CpuReadyArenaBeginStatus::Stopped,
          "typed begin must distinguish a stopped queue");
    check(stopped.stopReason ==
              CommandQueue::CpuReadyArenaBeginStopReason::QueueAlreadyStopped,
          "typed begin must retain the queue-already-stopped reason");
  }
  {
    CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
    CommandQueueArenaLeaseTestAccess::stopCpuReadyTape(queue);
    const auto stopped = queue.beginCpuReadyArenaSource(1, layout);
    check(stopped.status == CommandQueue::CpuReadyArenaBeginStatus::Stopped &&
              stopped.stopReason ==
                  CommandQueue::CpuReadyArenaBeginStopReason::
                      CpuReadyTapeAlreadyStopped,
          "typed begin must distinguish a stopped CpuReadyTape");
  }
  {
    CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
    CommandQueueArenaLeaseTestAccess::ensureEmptyLegacyWriter(queue);
    CommandQueueArenaLeaseTestAccess::stopAfterCompatibilityFlush(queue);
    const auto stopped = queue.beginCpuReadyArenaSource(1, layout);
    check(stopped.status == CommandQueue::CpuReadyArenaBeginStatus::Stopped &&
              stopped.stopReason ==
                  CommandQueue::CpuReadyArenaBeginStopReason::
                      CompatibilityFlushStopped,
          "typed begin must distinguish a stop during compatibility flush");
  }
  {
    CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
    const std::array layouts{layout};
    CommandQueueArenaLeaseTestAccess::setStopped(queue);
    const auto stopped = queue.beginCpuReadyArenaSources(1, layouts);
    check(stopped.status == CommandQueue::CpuReadyArenaBeginStatus::Stopped &&
              stopped.stopReason ==
                  CommandQueue::CpuReadyArenaBeginStopReason::
                      QueueAlreadyStopped,
          "batch begin must associate queue-already-stopped with its return");
  }
  {
    CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
    const std::array layouts{layout};
    CommandQueueArenaLeaseTestAccess::stopCpuReadyTape(queue);
    const auto stopped = queue.beginCpuReadyArenaSources(1, layouts);
    check(stopped.status == CommandQueue::CpuReadyArenaBeginStatus::Stopped &&
              stopped.stopReason ==
                  CommandQueue::CpuReadyArenaBeginStopReason::
                      CpuReadyTapeAlreadyStopped,
          "batch begin must associate tape-stopped with its return");
  }
  {
    CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
    const std::array layouts{layout};
    CommandQueueArenaLeaseTestAccess::ensureEmptyLegacyWriter(queue);
    CommandQueueArenaLeaseTestAccess::stopAfterCompatibilityFlush(queue);
    const auto stopped = queue.beginCpuReadyArenaSources(1, layouts);
    check(stopped.status == CommandQueue::CpuReadyArenaBeginStatus::Stopped &&
              stopped.stopReason ==
                  CommandQueue::CpuReadyArenaBeginStopReason::
                      CompatibilityFlushStopped,
          "batch begin must associate compatibility stop with its return");
  }
  check(sizeof(CommandQueue::CpuReadyArenaBeginResult) ==
            sizeof(CommandQueue::CpuReadyArenaBeginResultLayoutBaseline),
        "typed begin stop reason must fit the existing result padding");
  {
    CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
    const auto invalid = queue.beginCpuReadyArenaSource(0, layout);
    check(invalid.stopReason ==
              CommandQueue::CpuReadyArenaBeginStopReason::None,
          "non-stopped begin results must carry no stop reason");
  }
  {
    CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
    BufferDesc desc{};
    desc.size = 64;
    desc.pool = Pool::Default;
    desc.usage = UsageDynamic;
    const auto buffer = queue.pool().createBuffer(
        WMT::Device{NULL_OBJECT_HANDLE}, desc);
    auto* record = queue.pool().findBuffer(buffer.value);
    check(record && record->renameRing.size() == 1,
          "missing-snapshot fixture must create a versioned buffer");
    SyntheticBufferHandleGuard handleGuard{record};
    constexpr obj_handle_t backing = 0x3344u;
    record->buffer.handle = backing;
    record->renameRing[0].buffer.handle = backing;

    auto begin = queue.beginCpuReadyArenaSource(1, layout);
    check(begin.status == CommandQueue::CpuReadyArenaBeginStatus::Ready &&
              begin.lease.has_value(),
          "missing-snapshot fixture admission must succeed first");
    CanonicalDrawState state{};
    state.hot.streamMask = 1;
    state.hot.streamBuffers[0] = buffer;
    state.hot.streamStrides[0] = 16;
    const DrawParam draw{.primitiveCount = 1};
    queue.submitDrawRun(std::move(state), DrawUniformPayload{},
                        std::span<const DrawParam>(&draw, 1));
    check(!begin->publish(),
          "missing app-admission snapshot must fail-stop after admission");
    check(queue.cpuReadyArenaPoisoned() && record->lastUsedSeqId == 0,
          "failed snapshot validation must happen before resource marking");
    const auto corrupt = queue.beginCpuReadyArenaSource(2, layout);
    check(corrupt.status == CommandQueue::CpuReadyArenaBeginStatus::Corrupt,
          "typed begin must expose the sticky fail-stop lane as corrupt");
  }
}

void testArenaClearMarksCommonViewResourcesAtExactSeq() {
  using namespace dxmt9;
  using namespace dxmt9::core;

  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 1;
  capacity.clearRecords = 1;
  const auto layout = makeLayout(capacity);
  CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  SurfaceDesc desc{};
  desc.width = 1;
  desc.height = 1;
  desc.format = Format::NullRt;
  desc.pool = Pool::Default;
  desc.renderTarget = true;
  const auto surface = queue.pool().createSurface(
      WMT::Device{NULL_OBJECT_HANDLE}, BackendLimits{}, desc);
  const auto untouchedDepth = queue.pool().createSurface(
      WMT::Device{NULL_OBJECT_HANDLE}, BackendLimits{}, desc);
  auto* record = queue.pool().findSurface(surface.value);
  auto* untouchedDepthRecord = queue.pool().findSurface(untouchedDepth.value);
  check(record != nullptr && untouchedDepthRecord != nullptr,
        "clear resource fixture surfaces must exist");

  auto begin = queue.beginCpuReadyArenaSource(3, layout);
  check(begin.status == CommandQueue::CpuReadyArenaBeginStatus::Ready &&
            begin.lease.has_value(),
        "clear resource fixture admission must succeed");
  const auto seqId = begin->seqId();
  ClearDesc clear{};
  clear.colorAttachments[0].handle = surface;
  clear.depthStencil.handle = untouchedDepth;
  clear.clearColor = true;
  queue.submitClear(clear);
  check(begin->publish(), "clear-only arena source must publish");
  check(record->lastUsedSeqId == seqId,
        "SourceCommandView clear attachments must mark the exact ticket seq "
        "without help from the raw handle table");
  check(untouchedDepthRecord->lastUsedSeqId == 0,
        "clear marking must not stamp an attachment whose depth/stencil flags "
        "are both disabled");
}

void testArenaPublishToReclaimLifecycle() {
  using namespace dxmt9;
  using namespace dxmt9::core;

  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 1;
  capacity.clearRecords = 1;
  const auto layout = makeLayout(capacity);
  CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  auto begin = queue.beginCpuReadyArenaSource(9, layout);
  check(begin.status == CommandQueue::CpuReadyArenaBeginStatus::Ready &&
            begin.lease.has_value(),
        "lifecycle fixture strict admission must succeed");
  const auto ticket = begin->ticket();
  ClearDesc clear{};
  clear.clearColor = true;
  queue.submitClear(clear);
  check(begin->publish(), "lifecycle fixture arena source must publish");

  const auto result =
      CommandQueueArenaLeaseTestAccess::publishToReclaim(queue, ticket);
  check(result.dequeued && result.resolvedArena && result.sawClear &&
            result.commandCount == 1u,
        "Ready arena source must become Represented and resolve through the "
        "common call-local SourcePayloadView");
  check(result.submitted && result.completed && result.reclaimed,
        "arena source must traverse Submitted, Completed, and FIFO reclaim");
  check(result.residentSources == 0u && result.staleResolveRejected,
        "reclaimed arena owner must no longer resolve");
  check(result.generationAfter == result.generationBefore + 1u,
        "arena reclaim must advance the source generation");
}

void testActivePresentPublishesFinalSourceAndDefersFlush() {
  using namespace dxmt9;
  using namespace dxmt9::core;

  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 2;
  capacity.clearRecords = 1;
  capacity.presentRecords = 1;
  const auto layout = makeLayout(capacity);
  CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  const Handle initialBackBuffer{0x91};
  const Handle pendingBackBuffer{0x92};
  CommandQueueArenaLeaseTestAccess::setCurrentBackBuffer(
      queue, initialBackBuffer);
  auto begin = queue.beginCpuReadyArenaSource(10, layout);
  check(begin && begin->seqId() == 1,
        "Present fixture must reserve one strict source identity");

  ClearDesc clear{};
  clear.colorAttachments[0].handle = pendingBackBuffer;
  clear.clearColor = true;
  queue.submitClear(clear);
  SwapDesc present{};
  present.pacedByPresentOrdinal = true;
  check(queue.submitPresent(present) == begin->seqId(),
        "active Arena Present returns the reserved source seq without "
        "publishing early");
  check(CommandQueueArenaLeaseTestAccess::readyCount(queue) == 0,
        "paced Present remains unpublished until the lease seals the source");
  check(resolvePresentBoundaryAction(
            /*pacedByPresentOrdinal=*/true, BoundaryPolicy::Default) ==
            PresentBoundaryAction::SkipPacedByOffloadOrdinal,
        "paced Arena Present selects the post-publication boundary skip");

  queue.submitFlush();
  check(CommandQueueArenaLeaseTestAccess::activeFlushDeferred(queue) &&
            CommandQueueArenaLeaseTestAccess::readyCount(queue) == 0,
        "synchronous Present flush is recorded without waiting before "
        "publication");
  // The inert queue has no encode worker. Stop its wait predicate so publish
  // can exercise the deferred post-publication flush without blocking.
  CommandQueueArenaLeaseTestAccess::setStopped(queue);
  const auto ticket = begin->ticket();
  check(begin->publish(),
        "Present-bearing Arena source publishes before deferred flush runs");
  const auto* arena =
      CommandQueueArenaLeaseTestAccess::publishedArena(queue, ticket);
  check(arena != nullptr, "published Present source resolves from Tape");
  const SourcePayloadView source(*arena);
  check(source.commandCount() == 2 &&
            source.commandAt(0).kind() == MetalCommandKind::Clear &&
            source.commandAt(1).kind() == MetalCommandKind::Present &&
            source.commandAt(1).command.present &&
            source.commandAt(1).command.present->presentSource ==
                pendingBackBuffer,
        "Present is the final command and uses the active build's pending "
        "backbuffer");
}

void testCaptureIdentityUsesExactRawRangesAndPreReorderPasses() {
  using namespace dxmt9;
  using namespace dxmt9::core;

  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 2;
  capacity.clearRecords = 1;
  capacity.presentRecords = 1;
  const auto layout = makeLayout(capacity);
  CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  auto begin = queue.beginCpuReadyArenaSource(12, layout);
  check(begin.has_value(), "capture identity fixture admission succeeds");
  check(begin->beginCaptureIdentity(5u),
        "capture identity fixes the complete raw-record extent");

  check(begin->captureNextCommandRecord(1u),
        "leading state is followed by the exact Clear anchor");
  ClearDesc clear{};
  clear.clearColor = true;
  queue.submitClear(clear);

  check(begin->captureNextCommandRecord(3u),
        "interstitial state is followed by the exact Present anchor");
  SwapDesc present{};
  present.pacedByPresentOrdinal = true;
  check(queue.submitPresent(present) == begin->seqId(),
        "capture identity fixture appends Present to the same source");

  CommandQueue::CpuReadyCaptureIdentity identity{};
  const auto ticket = begin->ticket();
  check(begin->publish({}, &identity),
        "capture identity is copied before Ready visibility");
  check(identity.valid() && identity.sourceOrdinal == ticket.sourceOrdinal &&
            identity.seqId == ticket.seqId && identity.recordCount == 5u,
        "capture identity preserves exact publication metadata");
  check(identity.ranges.size() == 2u &&
            identity.ranges[0].firstRecord == 0u &&
            identity.ranges[0].recordCount == 2u &&
            identity.ranges[1].firstRecord == 2u &&
            identity.ranges[1].recordCount == 3u,
        "leading/interstitial/trailing state follows the structural raw-range "
        "ownership policy without gaps");
  check(identity.ranges[0].dagPassIndex !=
            identity.ranges[1].dagPassIndex &&
            identity.ranges[0].passKind != identity.ranges[1].passKind,
        "Clear and Present retain distinct pre-reorder DAG pass identities");
}

void testIncompleteCaptureIdentityDoesNotRejectArenaPublication() {
  using namespace dxmt9;
  using namespace dxmt9::core;

  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 1;
  capacity.clearRecords = 1;
  const auto layout = makeLayout(capacity);
  CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  auto begin = queue.beginCpuReadyArenaSource(13, layout);
  check(begin.has_value() && begin->beginCaptureIdentity(5u),
        "observer-failure fixture starts bounded identity capture");
  check(begin->captureNextCommandRecord(1u),
        "the first raw command anchor is accepted");
  ClearDesc clear{};
  clear.clearColor = true;
  queue.submitClear(clear);
  check(begin->captureNextCommandRecord(4u),
        "an unmatched trailing anchor remains pending at publication");

  CommandQueue::CpuReadyCaptureIdentity identity{};
  const auto ticket = begin->ticket();
  check(begin->publish({}, &identity),
        "incomplete diagnostic identity must not reject renderer publication");
  check(!identity.valid(),
        "incomplete diagnostic identity is returned fail-closed");
  const auto* arena =
      CommandQueueArenaLeaseTestAccess::publishedArena(queue, ticket);
  check(arena != nullptr && SourcePayloadView(*arena).commandCount() == 1u,
        "the observed Clear source remains published exactly once");
}

void testPresentAppendAbortRemovesStashedTokenOnce() {
  using namespace dxmt9;
  using namespace dxmt9::core;

  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 1;
  // Intentionally omit Present storage so append fails after the token has
  // been stashed and cleanup responsibility recorded by the build context.
  const auto layout = makeLayout(capacity);
  CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  auto begin = queue.beginCpuReadyArenaSource(11, layout);
  check(begin.has_value(), "token-abort fixture admission must succeed");
  const auto presentId =
      CommandQueueArenaLeaseTestAccess::registerFakePresenter(queue);
  auto token = std::make_shared<PresentDrawableToken>();
  std::weak_ptr<PresentDrawableToken> weakToken = token;
  queue.stashDrawableToken(presentId, token);
  token.reset();
  check(!CommandQueueArenaLeaseTestAccess::appendStashedPresent(
            queue, presentId),
        "capacity failure must reject the active Present append");
  check(!begin->publish(),
        "failed Present append must fail-stop the strict publication");
  check(weakToken.expired() && !queue.takeDrawableToken(presentId),
        "abort removes the sole stashed token and a second take is empty");
  queue.unregisterPresenter(presentId);
}

void testBatchLeaseUsesSourceLocalSegmentCoordinates() {
  using namespace dxmt9;
  using namespace dxmt9::core;
  const auto layout = makeLayout(singleDrawCapacity());
  const std::array layouts{layout, layout};
  {
    CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
    auto begin = queue.beginCpuReadyArenaSources(123, layouts);
    check(begin.has_value(), "batch lease admission must succeed");
    check(!begin->selectSourceSegment(1, 0),
          "selecting a later source before its predecessor is selected fails");
  }
  {
    CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
    auto begin = queue.beginCpuReadyArenaSources(124, layouts);
    check(begin.has_value(), "second batch lease admission must succeed");
    check(begin->selectSourceSegment(0, 0),
          "first source accepts its local segment zero");
    check(begin->selectSourceSegment(1, 0),
          "second source converts local segment zero to the next global edge");
    check(!begin->selectSourceSegment(1, 2),
          "out-of-range source-local segment fails closed");
  }
}

void testBatchAdmissionWaitRequiresEveryContiguousControlSlot() {
  using namespace dxmt9;
  using namespace dxmt9::core;

  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 1;
  capacity.clearRecords = 1;
  const auto layout = makeLayout(capacity);
  const std::array layouts{layout, layout, layout};
  CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});

  CommandQueueArenaLeaseTestAccess::setControlStateAndWake(
      queue, 1u, ChunkSlot::State::Writing);
  CommandQueueArenaLeaseTestAccess::setControlStateAndWake(
      queue, 2u, ChunkSlot::State::Writing);
  const auto pressured = queue.beginCpuReadyArenaSources(130u, layouts);
  check(pressured.status ==
            CommandQueue::CpuReadyArenaBeginStatus::TemporaryPressure,
        "batch begin rejects when the first control is free but a later "
        "required control is occupied");

  CommandQueueArenaLeaseTestAccess::enableArenaAdmissionWaitObservation(queue);
  std::atomic<bool> returned{false};
  std::atomic<bool> admitted{false};
  std::thread waiter([&] {
    admitted.store(
        CommandQueueArenaLeaseTestAccess::waitForArenaAdmission(queue, layouts),
        std::memory_order_release);
    returned.store(true, std::memory_order_release);
  });
  check(CommandQueueArenaLeaseTestAccess::
            waitForArenaAdmissionPredicateEvaluations(queue, 1u) &&
            CommandQueueArenaLeaseTestAccess::arenaAdmissionWaitObservations(
                queue) == std::array<std::uint64_t, 2>{1u, 1u} &&
            !returned.load(std::memory_order_acquire),
        "batch waiter enters once and parks on the occupied contiguous suffix");

  CommandQueueArenaLeaseTestAccess::setControlStateAndWake(
      queue, 1u, ChunkSlot::State::Free);
  check(CommandQueueArenaLeaseTestAccess::
            waitForArenaAdmissionPredicateEvaluations(queue, 2u) &&
            CommandQueueArenaLeaseTestAccess::arenaAdmissionWaitObservations(
                queue) == std::array<std::uint64_t, 2>{1u, 2u} &&
            !returned.load(std::memory_order_acquire),
        "freeing only one later control rechecks once and parks without a "
        "begin/wait retry spin");

  CommandQueueArenaLeaseTestAccess::setControlStateAndWake(
      queue, 2u, ChunkSlot::State::Free);
  waiter.join();
  check(returned.load(std::memory_order_acquire) &&
            admitted.load(std::memory_order_acquire) &&
            CommandQueueArenaLeaseTestAccess::arenaAdmissionWaitObservations(
                queue) == std::array<std::uint64_t, 2>{1u, 3u},
        "batch waiter returns only after every required contiguous control is "
        "free, with one wait entry and no retry loop");

  auto begin = queue.beginCpuReadyArenaSources(130u, layouts);
  check(begin.has_value(),
        "the exact control predicate that released the waiter admits the batch");
}

void testCountOneSettlementRequiresExactIdentityTuple() {
  using namespace dxmt9;
  using namespace dxmt9::core;
  using Settlement = core::CpuReadyTape::ArenaGroupSettlement;
  constexpr Settlement expected{
      .rawOrdinal = 71u,
      .buildGeneration = 9u,
      .firstSourceOrdinal = 13u,
      .tailSeqId = 47u,
      .sourceCount = 1u,
      .hasPresent = false,
  };
  const auto wait = [&](Settlement query, bool stopAfter) {
    CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
    check(CommandQueueArenaLeaseTestAccess::injectCompletedSettlement(
              queue, expected),
          "count-one settlement must be recorded and drained");
    if (stopAfter) {
      CommandQueueArenaLeaseTestAccess::setStopped(queue);
    }
    return queue.waitForCpuReadyEventSettlement(
        query.rawOrdinal, query.buildGeneration, query.firstSourceOrdinal,
        query.tailSeqId, query.sourceCount);
  };
  check(wait(expected, true),
        "count-one wait must require the complete settlement tuple");
  check(!wait(Settlement{.rawOrdinal = expected.rawOrdinal + 1u,
                         .buildGeneration = expected.buildGeneration,
                         .firstSourceOrdinal = expected.firstSourceOrdinal,
                         .tailSeqId = expected.tailSeqId,
                         .sourceCount = expected.sourceCount}, true),
        "raw ordinal mutation must not pass a tail-only wait");
  check(!wait(Settlement{.rawOrdinal = expected.rawOrdinal,
                         .buildGeneration = expected.buildGeneration + 1u,
                         .firstSourceOrdinal = expected.firstSourceOrdinal,
                         .tailSeqId = expected.tailSeqId,
                         .sourceCount = expected.sourceCount}, true),
        "build generation mutation must not pass settlement");
  check(!wait(Settlement{.rawOrdinal = expected.rawOrdinal,
                         .buildGeneration = expected.buildGeneration,
                         .firstSourceOrdinal = expected.firstSourceOrdinal + 1u,
                         .tailSeqId = expected.tailSeqId,
                         .sourceCount = expected.sourceCount}, true),
        "first source mutation must not pass settlement");
  check(!wait(Settlement{.rawOrdinal = expected.rawOrdinal,
                         .buildGeneration = expected.buildGeneration,
                         .firstSourceOrdinal = expected.firstSourceOrdinal,
                         .tailSeqId = expected.tailSeqId + 1u,
                         .sourceCount = expected.sourceCount}, true),
        "tail mutation must not pass settlement");
  check(!wait(Settlement{.rawOrdinal = expected.rawOrdinal,
                         .buildGeneration = expected.buildGeneration,
                         .firstSourceOrdinal = expected.firstSourceOrdinal,
                         .tailSeqId = expected.tailSeqId,
                         .sourceCount = 2u}, true),
        "source count mutation must not pass settlement");
}

void testBatchPublishBuildsOneAuthenticatedCrossSourcePass() {
  using namespace dxmt9;
  using namespace dxmt9::core;

  const auto layout = makeLayout(singleDrawCapacity());
  const std::array layouts{layout, layout};
  CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  auto begin = queue.beginCpuReadyArenaSources(126, layouts);
  check(begin.has_value(), "cross-source capture admission must succeed");
  check(begin->beginCaptureIdentity(2u),
        "batch capture identity must reserve the complete event range");

  const auto first = materializedSubmission(Handle{11}, 7, 9);
  const auto second = materializedSubmission(Handle{11}, 7, 9);
  std::array submissions{first};
  check(begin->selectSourceSegment(0, 0),
        "first source local segment must be selected");
  check(begin->captureNextCommandRecord(0),
        "first source anchor must be recorded before append");
  queue.submitDrawRunBatch(submissions);
  submissions[0] = second;
  check(begin->selectSourceSegment(1, 0),
        "second source local segment must be selected in order");
  check(begin->captureNextCommandRecord(1),
        "second source anchor must be recorded before append");
  queue.submitDrawRunBatch(submissions);
  constexpr std::array firstRecords{0u, 1u};
  constexpr std::array recordCounts{1u, 1u};
  check(begin->setCaptureSourceRanges(firstRecords, recordCounts),
        "capture ranges must cover both source rows contiguously");
  check(!begin->setCaptureSourceRanges(firstRecords, recordCounts) &&
            !begin->beginCaptureIdentity(2u),
        "batch capture range and identity setup must each be exactly once");

  CommandQueue::CpuReadyCaptureIdentityBatch identity{};
  check(begin->publishBatch({}, &identity),
        "production batch publish must seal and atomically publish");
  check(identity.valid() && identity.segments.size() == 2u,
        "batch publication must return both authenticated segments");
  check(identity.segments[0].ranges.size() == 1u &&
            identity.segments[1].ranges.size() == 1u &&
            identity.segments[0].ranges[0].logicalPassId != 0u &&
            identity.segments[0].ranges[0].logicalPassId ==
                identity.segments[1].ranges[0].logicalPassId,
        "same-pass cross-source rows must carry one nonzero logical pass");
  check(CommandQueueArenaLeaseTestAccess::readyCount(queue) == 2u,
        "cross-source publication must expose both Ready entries together");
}

void testBatchPublishRunsEventProofWithoutCaptureSidecar() {
  using namespace dxmt9;
  using namespace dxmt9::core;

  const auto layout = makeLayout(singleDrawCapacity());
  const std::array layouts{layout, layout};
  CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  auto begin = queue.beginCpuReadyArenaSources(128, layouts);
  check(begin.has_value(), "no-capture batch admission must succeed");
  const auto submission = materializedSubmission(Handle{11}, 7, 9);
  std::array submissions{submission};
  check(begin->selectSourceSegment(0, 0),
        "no-capture first source selection must succeed");
  queue.submitDrawRunBatch(submissions);
  check(begin->selectSourceSegment(1, 0),
        "no-capture second source selection must succeed");
  queue.submitDrawRunBatch(submissions);
  CommandQueueArenaLeaseTestAccess::armBatchPlannerObservation(queue, false);
  check(begin->publishBatchWithStatus({}, nullptr) ==
            CommandQueue::CpuReadyArenaPublishStatus::Published,
        "no-capture SegmentSerial batch must run the event-wide proof");
  check(CommandQueueArenaLeaseTestAccess::batchPlannerInvocationCount(queue) ==
            1u &&
            CommandQueueArenaLeaseTestAccess::batchPlannerValid(queue),
        "no-capture planner must be invoked exactly once and validate the "
        "complete source window");
  check(CommandQueueArenaLeaseTestAccess::readyCount(queue) == 2u,
        "successful no-capture proof must publish both Ready rows atomically");
}

void testBatchPlannerRejectionRollsBackBeforeEffects() {
  using namespace dxmt9;
  using namespace dxmt9::core;

  const auto layout = makeLayout(singleDrawCapacity());
  const std::array layouts{layout, layout};
  CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  auto begin = queue.beginCpuReadyArenaSources(129, layouts);
  check(begin.has_value(), "planner rejection admission must succeed");
  const auto submission = materializedSubmission(Handle{11}, 7, 9);
  std::array submissions{submission};
  check(begin->selectSourceSegment(0, 0),
        "planner rejection first source selection must succeed");
  queue.submitDrawRunBatch(submissions);
  check(begin->selectSourceSegment(1, 0),
        "planner rejection second source selection must succeed");
  queue.submitDrawRunBatch(submissions);
  CommandQueueArenaLeaseTestAccess::armBatchPlannerObservation(queue, true);
  check(begin->publishBatchWithStatus({}, nullptr) ==
            CommandQueue::CpuReadyArenaPublishStatus::RecoverableFailure,
        "planner rejection must select pre-effect EventSerial fallback");
  check(CommandQueueArenaLeaseTestAccess::batchPlannerInvocationCount(queue) ==
            1u &&
            !CommandQueueArenaLeaseTestAccess::batchPlannerValid(queue),
        "forced invalid planner result must be observed exactly once");
  check(CommandQueueArenaLeaseTestAccess::readyCount(queue) == 0u &&
            CommandQueueArenaLeaseTestAccess::residentSources(queue) == 0u &&
            CommandQueueArenaLeaseTestAccess::nextSeqId(queue) == 1u &&
            CommandQueueArenaLeaseTestAccess::writeIndex(queue) == 0u &&
            !queue.cpuReadyArenaPoisoned(),
        "planner rejection must leave zero Ready/resident entries, restored "
        "cursors, and no poison");
}

void testBatchBuilderFailureRollsBackForEventSerialFallback() {
  using namespace dxmt9;
  using namespace dxmt9::core;

  const auto layout = makeLayout(singleDrawCapacity());
  const std::array layouts{layout, layout};
  CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  auto begin = queue.beginCpuReadyArenaSources(127, layouts);
  check(begin.has_value(), "builder failure admission must succeed");
  const auto submission = materializedSubmission(Handle{12}, 8, 10);
  std::array submissions{submission};
  check(begin->selectSourceSegment(0, 0),
        "builder failure first source selection must succeed");
  queue.submitDrawRunBatch(submissions);
  check(begin->selectSourceSegment(1, 0),
        "builder failure second source selection must succeed");
  queue.submitDrawRunBatch(submissions);
  // The one-command source capacity makes this append reject the active
  // builder before publication; the batch must still have zero Ready rows.
  queue.submitDrawRunBatch(submissions);
  const auto publishStatus = begin->publishBatchWithStatus({}, nullptr);
  check(publishStatus ==
            CommandQueue::CpuReadyArenaPublishStatus::RecoverableFailure,
        "pre-effect builder rejection must rollback for EventSerial fallback");
  check(CommandQueueArenaLeaseTestAccess::readyCount(queue) == 0u &&
            CommandQueueArenaLeaseTestAccess::residentSources(queue) == 0u &&
            CommandQueueArenaLeaseTestAccess::nextSeqId(queue) == 1u &&
            CommandQueueArenaLeaseTestAccess::writeIndex(queue) == 0u &&
            !queue.cpuReadyArenaPoisoned(),
        "builder rollback must restore cursors, residency, zero Ready, and "
        "leave the queue unpoisoned");
}

void testFirstArenaFailureRetainsCoordinatesAcrossLaterFailures() {
  using namespace dxmt9;
  using namespace dxmt9::core;

  const auto layout = makeLayout(singleDrawCapacity());
  const std::array layouts{layout, layout};
  CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  auto begin = queue.beginCpuReadyArenaSources(129, layouts);
  check(begin.has_value(), "failure-retention admission must succeed");
  const auto submission = materializedSubmission(Handle{17}, 8, 10);
  std::array submissions{submission};
  check(begin->selectSourceSegment(0, 0),
        "failure-retention first source selection must succeed");
  queue.submitDrawRunBatch(submissions);
  check(begin->selectSourceSegment(1, 0),
        "failure-retention second source selection must succeed");
  queue.submitDrawRunBatch(submissions);
  queue.submitDrawRunBatch(submissions);
  const auto first = CommandQueueArenaLeaseTestAccess::activeFailure(queue);
  check(first.failureClass ==
            CommandQueue::CpuReadyArenaFailureClass::Append &&
            first.source == 1u && first.segment == 0u &&
            first.plannedPages == layout.segments[0].layout.pageCount &&
            first.actualCommands == 1u,
        "organic append failure coordinates: class=" +
            std::to_string(static_cast<unsigned>(first.failureClass)) +
            " source=" + std::to_string(first.source) +
            " segment=" + std::to_string(first.segment) +
            " planned=" + std::to_string(first.plannedPages) +
            " actual=" + std::to_string(first.actualCommands));
  queue.submitDrawRunBatch(submissions);
  const auto retained = CommandQueueArenaLeaseTestAccess::activeFailure(queue);
  check(retained.failureClass == first.failureClass &&
            retained.source == first.source && retained.segment == first.segment &&
            retained.plannedPages == first.plannedPages &&
            retained.actualCommands == first.actualCommands,
        "a later failure must not overwrite the first typed failure record");
  check(begin->publishBatchWithStatus({}, nullptr) ==
            CommandQueue::CpuReadyArenaPublishStatus::RecoverableFailure,
        "retained organic append failure must take the pre-effect rollback");
}

void testBatchRollbackFailureDoesNotReportRecoverableFallback() {
  using namespace dxmt9;
  using namespace dxmt9::core;

  const auto layout = makeLayout(singleDrawCapacity());
  const std::array layouts{layout, layout};
  CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  CommandQueueArenaLeaseTestAccess::forceNextBatchRollbackFailure(queue);
  const auto begin = queue.beginCpuReadyArenaSources(125, layouts);
  check(begin.status == CommandQueue::CpuReadyArenaBeginStatus::Corrupt &&
            !begin && queue.cpuReadyArenaPoisoned(),
        "guarded rollback failure must fail-stop instead of reporting a "
        "recoverable EventSerial fallback");
}

void testWsiQuiescenceAndRegistryFailureDisposition() {
  using namespace dxmt9;
  using namespace dxmt9::core;
  using dxmt9::wsi::QuiescenceDisposition;

  CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  const auto currentId =
      CommandQueueArenaLeaseTestAccess::registerFakePresenter(queue);
  check(currentId && queue.lookupPresenter(currentId) ==
            reinterpret_cast<Presenter*>(static_cast<std::uintptr_t>(1)),
        "current Presenter registry binding must exist");
  CommandQueueArenaLeaseTestAccess::failNextPresenterRegistration(queue);
  const auto failedCandidate = queue.registerPresenter(
      reinterpret_cast<Presenter*>(static_cast<std::uintptr_t>(2)));
  check(!failedCandidate && queue.lookupPresenter(currentId) ==
            reinterpret_cast<Presenter*>(static_cast<std::uintptr_t>(1)),
        "injected candidate registry failure preserves the current PresentId");

  const auto layout = makeLayout(singleDrawCapacity());
  CommandQueue activeQueue(
      CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  auto active = activeQueue.beginCpuReadyArenaSource(131, layout);
  check(active.has_value(), "active-arena WSI fixture admission must succeed");
  check(activeQueue.beginWsiQuiescence() ==
            QuiescenceDisposition::ActiveArena,
        "WSI quiescence must fail closed instead of deferring active arena flush");
  (void)active->abortForFallback();

  check(queue.beginWsiQuiescence() == QuiescenceDisposition::Complete,
        "idle queue must establish actual WSI quiescence");
  check(queue.beginWsiQuiescence() == QuiescenceDisposition::AlreadyActive,
        "a second cold replacement cannot overlap the armed WSI gate");
  check(queue.submitPresent({}) == 0u,
        "armed WSI gate rejects new Presenter users before registry swap");
  check(queue.beginCpuReadyArenaSource(133, layout).status ==
            CommandQueue::CpuReadyArenaBeginStatus::TemporaryPressure,
        "armed WSI gate rejects a new CPU-ready arena");
  queue.endWsiQuiescence();
  queue.unregisterPresenter(currentId);

  CommandQueue stopped(
      CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  CommandQueueArenaLeaseTestAccess::setStopped(stopped);
  check(stopped.beginWsiQuiescence() ==
            QuiescenceDisposition::QueueStopped,
        "stopped queue has a distinct non-quiescent disposition");
}

}  // namespace

int main() {
  try {
    testExplicitControlIndexGroupingAndExactSnapshotMark();
    testActiveMismatchFailStopsWithoutFallback();
    testLegacyWriterBoundaryBeforeStrictAdmission();
    testActiveSingleDrawPreservesUpAndStateBlockBypass();
    testTypedBeginStatusAndMissingAdmissionSnapshot();
    testArenaClearMarksCommonViewResourcesAtExactSeq();
    testArenaPublishToReclaimLifecycle();
    testActivePresentPublishesFinalSourceAndDefersFlush();
    testCaptureIdentityUsesExactRawRangesAndPreReorderPasses();
    testIncompleteCaptureIdentityDoesNotRejectArenaPublication();
    testPresentAppendAbortRemovesStashedTokenOnce();
    testBatchLeaseUsesSourceLocalSegmentCoordinates();
    testBatchAdmissionWaitRequiresEveryContiguousControlSlot();
    testCountOneSettlementRequiresExactIdentityTuple();
    testBatchPublishBuildsOneAuthenticatedCrossSourcePass();
    testBatchPublishRunsEventProofWithoutCaptureSidecar();
    testBatchPlannerRejectionRollsBackBeforeEffects();
    testBatchBuilderFailureRollsBackForEventSerialFallback();
    testFirstArenaFailureRetainsCoordinatesAcrossLaterFailures();
    testBatchRollbackFailureDoesNotReportRecoverableFallback();
    testWsiQuiescenceAndRegistryFailureDisposition();
  } catch (const std::exception& error) {
    std::cerr << "cpu_ready_arena_lease_spec: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
