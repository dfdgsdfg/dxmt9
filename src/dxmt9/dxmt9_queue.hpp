#pragma once

#include "dxmt9_backend_types.hpp"
#include "dxmt9_capture.hpp"
#include "../winemetal/Metal.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>
#include <deque>
#include <condition_variable>
#include <mutex>

namespace dxmt9::core::metalhud {
class SubmissionDiagnosticsController;
}

namespace dxmt9::core::metalqueue {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

enum class RenderEncoderGpuPassType : u32 {
  Unknown = 0,
  Draw,
  Clear,
  SurfaceCopy,
  StretchRect,
  ColorFill,
  DepthResolve,
  Present,
};

struct CommandBufferDiagnostics {
  u64 seqId = 0;
  size_t slotIndex = 0;
  bool hasDraw = false;
  bool hasPresent = false;
  bool hasBlit = false;
  bool hasStretchRect = false;
  u32 frame = 0;
  u32 compatFlags = 0;
  u64 vertexShaderHash = 0;
  u64 pixelShaderHash = 0;
  u64 shaderVariantHash = 0;
};

enum class QueueSlotState {
  Free = 0,
  Writing = 1,
  Pending = 2,
  Encoding = 3,
  GPU = 4,
};

struct ActiveSlotInfo {
  size_t index = 0;
  QueueSlotState state = QueueSlotState::Free;
  u64 seqId = 0;
  size_t commandCount = 0;
};

struct QueueTraceSnapshot {
  std::optional<size_t> slotIndex;
  std::optional<size_t> writingSlot;
  size_t writeIndex = 0;
  size_t readyCount = 0;
  size_t completedQueueCount = 0;
  size_t inflightCount = 0;
  u64 completedSeqId = 0;
  u64 lastCommittedSeqId = 0;
  u64 eventSeqId = 0;
  std::vector<ActiveSlotInfo> activeSlots;
};

struct ChunkSummaryInput {
  u64 seqId = 0;
  size_t slotIndex = 0;
  bool hasDraw = false;
  bool hasPresent = false;
  bool hasBlit = false;
  bool hasStretchRect = false;
  u32 frame = 0;
  u32 compatFlags = 0;
  u64 vertexShaderHash = 0;
  u64 pixelShaderHash = 0;
  u64 shaderVariantHash = 0;
};

enum class ChunkObservationKind {
  Draw,
  Blit,
  StretchRect,
  Present,
};

struct ChunkObservation {
  ChunkObservationKind kind = ChunkObservationKind::Draw;
  u32 compatFlags = 0;
  u64 vertexShaderHash = 0;
  u64 pixelShaderHash = 0;
  u64 shaderVariantHash = 0;
};

struct QueueTraceState {
  std::optional<size_t> slotIndex;
  std::optional<size_t> writingSlot;
  size_t writeIndex = 0;
  size_t readyCount = 0;
  size_t completedQueueCount = 0;
  size_t inflightCount = 0;
  u64 completedSeqId = 0;
  u64 lastCommittedSeqId = 0;
  u64 eventSeqId = 0;
  std::vector<ActiveSlotInfo> activeSlots;
};

struct QueueLifecycleContext {
  std::optional<size_t> writingSlot;
  size_t writeIndex = 0;
  size_t readyCount = 0;
  size_t completedQueueCount = 0;
  size_t inflightCount = 0;
  u64 completedSeqId = 0;
  u64 lastCommittedSeqId = 0;
};

struct QueueControllerState {
  std::optional<size_t> writingSlot;
  size_t writeIndex = 0;
  size_t readyCount = 0;
  size_t completedQueueCount = 0;
  size_t inflightCount = 0;
  u64 completedSeqId = 0;
  u64 lastCommittedSeqId = 0;
  std::span<const ChunkSlot> slots;
};

enum class QueueLifecycleEvent {
  PresentEnqueue,
  WriterWaitBegin,
  WriterWaitEnd,
  WriterAcquire,
  CommitEmpty,
  CommitWaitBegin,
  CommitWaitEnd,
  CommitPublish,
  EncodeDequeue,
  EncodeCommit,
  GpuComplete,
  FinishInline,
  FinishDequeue,
  ReclaimFree,
  WaitSeqBegin,
  WaitSeqEnd,
};

struct QueueTransitionRecord {
  QueueControllerState before{};
  QueueControllerState after{};
  std::optional<size_t> slotIndex;
  u64 eventSeqId = 0;
  size_t inflightLimit = 0;
  const SwapDesc* present = nullptr;
  Handle sourceHandle{};
};

struct QueueSubmissionRecord {
  struct RenderEncoderGpuSample {
    u32 startIndex = 0;
    u32 endIndex = 0;
    RenderEncoderGpuPassType passType = RenderEncoderGpuPassType::Unknown;
    u64 rtHandle = 0;
    u64 depthHandle = 0;
    u64 psoHandle = 0;
  };

  // RAII-owned command buffer for the tail of this chunk's Metal command
  // buffer chain. encodeChunk may commit earlier sub-CBs internally; the
  // finish/completion pipeline commits and waits only this tail CB, relying
  // on Metal same-queue in-order execution to make tail completion imply all
  // earlier sub-CBs in this chunk have completed.
  WMT::Reference<WMT::CommandBuffer> commandBuffer{};
  // Total command buffers in the chunk chain, including the tail above.
  // 1 means the public record is the whole chunk; >1 means earlier sub-CBs
  // were already committed during encodeChunk.
  u64 commandBufferChainLength = 1;
  WMT::Device metalCaptureDevice{};
  std::optional<metalcapture::MetalCaptureRequest> metalCapture{};
  // True when MTLCaptureManager.startCapture was already issued at
  // chunk-begin (in encodeChunk) so every encoder/draw call is in scope.
  // commitCommandBuffer skips the legacy start in that case and only
  // issues stopCapture after commit.
  bool metalCaptureAlreadyStarted = false;
  size_t slotIndex = 0;
  u64 seqId = 0;
  CommandBufferDiagnostics diagnostics{};
  const char* context = "queue";
  WMT::Reference<WMT::CounterSampleBuffer> renderEncoderGpuSampleBuffer{};
  std::vector<RenderEncoderGpuSample> renderEncoderGpuSamples{};
  std::vector<std::function<void()>> postCommitCallbacks;
};

CommandBufferDiagnostics summarizeChunk(u64 seqId,
                                        size_t slotIndex,
                                        std::span<const ChunkObservation> observations);
CommandBufferDiagnostics summarizeCommands(u64 seqId,
                                          size_t slotIndex,
                                          const ChunkSlot& slot,
                                          const std::function<u32(Handle)>& resolveSurfaceFlags);
Handle selectPresentSourceHandle(const SwapDesc& desc, Handle currentBackBuffer) noexcept;
QueueTraceSnapshot makeQueueTraceSnapshot(const QueueTraceState& state);

template <typename CommandContainer, typename ObservationMapper>
CommandBufferDiagnostics summarizeChunk(u64 seqId,
                                        size_t slotIndex,
                                        const CommandContainer& commands,
                                        ObservationMapper&& mapObservation) {
  std::vector<ChunkObservation> observations;
  observations.reserve(commands.size());
  for (const auto& command : commands) {
    observations.push_back(std::invoke(std::forward<ObservationMapper>(mapObservation), command));
  }
  return summarizeChunk(seqId, slotIndex, std::span<const ChunkObservation>(observations.data(), observations.size()));
}

template <typename SlotContainer, typename SlotMapper>
QueueTraceSnapshot makeQueueTraceSnapshot(std::optional<size_t> slotIndex,
                                          std::optional<size_t> writingSlot,
                                          size_t writeIndex,
                                          size_t readyCount,
                                          size_t completedQueueCount,
                                          size_t inflightCount,
                                          u64 completedSeqId,
                                          u64 lastCommittedSeqId,
                                          u64 eventSeqId,
                                          const SlotContainer& slots,
                                          SlotMapper&& mapSlot) {
  QueueTraceState state;
  state.slotIndex = slotIndex;
  state.writingSlot = writingSlot;
  state.writeIndex = writeIndex;
  state.readyCount = readyCount;
  state.completedQueueCount = completedQueueCount;
  state.inflightCount = inflightCount;
  state.completedSeqId = completedSeqId;
  state.lastCommittedSeqId = lastCommittedSeqId;
  state.eventSeqId = eventSeqId;
  state.activeSlots.reserve(slots.size());
  for (size_t i = 0; i < slots.size(); ++i) {
    auto mapped = std::invoke(std::forward<SlotMapper>(mapSlot), i, slots[i]);
    if (mapped.has_value()) {
      state.activeSlots.push_back(*mapped);
    }
  }
  return makeQueueTraceSnapshot(state);
}

template <typename SlotContainer>
QueueTraceSnapshot makeQueueTraceSnapshot(std::optional<size_t> slotIndex,
                                          std::optional<size_t> writingSlot,
                                          size_t writeIndex,
                                          size_t readyCount,
                                          size_t completedQueueCount,
                                          size_t inflightCount,
                                          u64 completedSeqId,
                                          u64 lastCommittedSeqId,
                                          u64 eventSeqId,
                                          const SlotContainer& slots) {
  QueueTraceState state;
  state.slotIndex = slotIndex;
  state.writingSlot = writingSlot;
  state.writeIndex = writeIndex;
  state.readyCount = readyCount;
  state.completedQueueCount = completedQueueCount;
  state.inflightCount = inflightCount;
  state.completedSeqId = completedSeqId;
  state.lastCommittedSeqId = lastCommittedSeqId;
  state.eventSeqId = eventSeqId;
  state.activeSlots.reserve(slots.size());
  for (size_t i = 0; i < slots.size(); ++i) {
    const auto& slot = slots[i];
    if (slot.state == ChunkSlot::State::Free) {
      continue;
    }
    state.activeSlots.push_back(ActiveSlotInfo{
        .index = i,
        .state = static_cast<QueueSlotState>(static_cast<int>(slot.state)),
        .seqId = slot.seqId,
        .commandCount = slot.commandCount(),
    });
  }
  return makeQueueTraceSnapshot(state);
}

template <typename SlotContainer, typename SlotMapper>
void traceQueueEvent(const char* event,
                     std::optional<size_t> slotIndex,
                     std::optional<size_t> writingSlot,
                     size_t writeIndex,
                     size_t readyCount,
                     size_t completedQueueCount,
                     size_t inflightCount,
                     u64 completedSeqId,
                     u64 lastCommittedSeqId,
                     u64 eventSeqId,
                     const SlotContainer& slots,
                     SlotMapper&& mapSlot,
                     const char* extra = nullptr) {
  traceQueueEvent(
      event,
      makeQueueTraceSnapshot(slotIndex, writingSlot, writeIndex, readyCount, completedQueueCount,
                             inflightCount, completedSeqId, lastCommittedSeqId, eventSeqId, slots,
                             std::forward<SlotMapper>(mapSlot)),
      extra);
}

template <typename SlotContainer>
void traceQueueEvent(const char* event,
                     std::optional<size_t> slotIndex,
                     std::optional<size_t> writingSlot,
                     size_t writeIndex,
                     size_t readyCount,
                     size_t completedQueueCount,
                     size_t inflightCount,
                     u64 completedSeqId,
                     u64 lastCommittedSeqId,
                     u64 eventSeqId,
                     const SlotContainer& slots,
                     const char* extra = nullptr) {
  traceQueueEvent(
      event,
      makeQueueTraceSnapshot(slotIndex, writingSlot, writeIndex, readyCount, completedQueueCount,
                             inflightCount, completedSeqId, lastCommittedSeqId, eventSeqId, slots),
      extra);
}

bool queueTraceEnabled();
const char* queueTraceFilePath();
u64 queueTraceFromSeq();

void emitQueueTraceLine(const std::string& line);
void emitTextureTraceLine(const std::string& line);

CommandBufferDiagnostics summarizeChunk(const ChunkSummaryInput& input);
bool shouldTraceQueue(const QueueTraceSnapshot& snapshot);
std::string formatActiveSlots(const QueueTraceSnapshot& snapshot);
void traceQueueEvent(const char* event, const QueueTraceSnapshot& snapshot, const char* extra = nullptr);
void traceLifecycleEvent(QueueLifecycleEvent event,
                         std::optional<size_t> slotIndex,
                         u64 eventSeqId,
                         std::optional<size_t> writingSlot,
                         size_t writeIndex,
                         size_t readyCount,
                         size_t completedQueueCount,
                         size_t inflightCount,
                         u64 completedSeqId,
                         u64 lastCommittedSeqId,
                         std::span<const ChunkSlot> slots,
                         const char* extra = nullptr);
void traceQueueSlotsEvent(const char* event,
                          std::optional<size_t> slotIndex,
                          u64 eventSeqId,
                          std::optional<size_t> writingSlot,
                          size_t writeIndex,
                          size_t readyCount,
                          size_t completedQueueCount,
                          size_t inflightCount,
                          u64 completedSeqId,
                          u64 lastCommittedSeqId,
                          std::span<const ChunkSlot> slots,
                          const char* extra = nullptr);

/*
 * TLA+: QueueLifecycleRefinement
 *
 * Variable mapping:
 *   writingSlot               -> *SubmissionBinding::writingSlot
 *   writeIndex                -> *SubmissionBinding::writeIndex
 *   nextSeqId                 -> *SubmissionBinding::nextSeqId
 *   readySlots                -> *SubmissionBinding::readySlots
 *   completedSeqQueue         -> *SubmissionBinding::completedSeqQueue
 *   pendingCompletion         -> pendingCompletion_
 *   inflightCount             -> *SubmissionBinding::inflightCount
 *   completedSeqId            -> *SubmissionBinding::completedSeqId
 *   lastCommittedSeqId        -> *SubmissionBinding::lastCommittedSeqId
 *   stop                      -> *SubmissionBinding::stop
 *   slotState[s]              -> SubmissionBinding::slots[s].state
 *   slotSeqId[s]              -> SubmissionBinding::slots[s].seqId
 *   slotHasCommands[s]        -> !SubmissionBinding::slots[s].commandsEmpty()
 *
 * TLA+: PresentFrameLatency
 *
 * Variable mapping:
 *   presentCompletedSeqId     -> *SubmissionBinding::presentCompletedSeqId
 *   present completion queue  -> *SubmissionBinding::completedPresentSeqQueue
 *
 * Debug assertions in assertQueueLifecycleInvariants() and
 * assertPendingCompletionInvariantsLocked() are the executable binding for
 * the safety invariants in both modules.
 */
class QueueLifecycleController {
 public:
  struct SubmissionBinding {
    std::optional<size_t>* writingSlot = nullptr;
    size_t* writeIndex = nullptr;
    u64* nextSeqId = nullptr;
    std::deque<size_t>* readySlots = nullptr;
    std::deque<u64>* completedSeqQueue = nullptr;
    std::deque<u64>* completedPresentSeqQueue = nullptr;
    size_t* inflightCount = nullptr;
    u64* completedSeqId = nullptr;
    u64* presentCompletedSeqId = nullptr;
    u64* lastCommittedSeqId = nullptr;
    std::span<ChunkSlot> slots;
    std::mutex* mutex = nullptr;
    std::condition_variable* writeCv = nullptr;
    std::condition_variable* encodeCv = nullptr;
    std::condition_variable* finishCv = nullptr;
    std::condition_variable* presentCompletedCv = nullptr;
    bool* stop = nullptr;
    metalhud::SubmissionDiagnosticsController* submissionDiagnostics = nullptr;
    std::function<u32(Handle)> resolveSurfaceFlags;
  };

  void bindTrackedSubmissionState(SubmissionBinding binding);
  // TLA+: WriterAcquire, WriterWaitBegin, WriterWaitEnd.
  bool ensureWriterSlot(std::unique_lock<std::mutex>& lock, size_t inflightLimit);
  // TLA+: Present enqueue + CommitPublish for a present-bearing chunk.
  void presentAndCommit(std::unique_lock<std::mutex>& lock,
                        size_t inflightLimit,
                        const SwapDesc& present,
                        Handle sourceHandle,
                        const std::function<void(ChunkSlot&)>& onBeforePublish = {});
  // TLA+: CommitPublish followed by waitForSequence(lastCommittedSeqId).
  void flushAndWait(std::unique_lock<std::mutex>& lock,
                    size_t inflightLimit,
                    const std::function<void(ChunkSlot&)>& onBeforePublish = {});
  // TLA+: CommitEmpty or CommitPublish.
  bool commitCurrentChunk(std::unique_lock<std::mutex>& lock,
                          size_t inflightLimit,
                          const std::function<void(ChunkSlot&)>& onBeforePublish = {});
  // TLA+: EncodeDequeue.
  bool dequeueReadySlot(std::unique_lock<std::mutex>& lock, size_t& slotIndex, ChunkSlot& slotCopy);
  // TLA+: EncodeDequeue followed by EncodeSubmitToGpu or EncodeCompleteInline.
  bool runEncodeIteration(
      std::unique_lock<std::mutex>& lock,
      const std::function<std::optional<QueueSubmissionRecord>(size_t, const ChunkSlot&)>& encodeFn,
      const std::function<void(u64)>& onInlineComplete = {});
  // TLA+: present-bearing metadata append before CommitPublish.
  void appendPresentCommand(const SwapDesc& present, Handle sourceHandle);
  // TLA+: EncodeSubmitToGpu.
  void submitEncodedChunk(WMT::Reference<WMT::CommandBuffer> commandBuffer,
                          size_t slotIndex,
                          u64 seqId,
                          const char* context = "queue");
  // TLA+: EncodeCompleteInline.
  void completeInlineChunk(size_t slotIndex, u64 seqId);
  // TLA+: FinishDequeue, and PresentComplete for eligible present seq IDs.
  bool drainCompletedSequence(std::unique_lock<std::mutex>& lock, u64& seqId);
  // TLA+: FinishDequeue followed by ReclaimFree.
  bool runFinishIteration(std::unique_lock<std::mutex>& lock,
                          const std::function<void(u64)>& onAfterFinish = {});
  // TLA+: ReclaimFree.
  void reclaimCompletedGpuSlots(u64 seqId);
  // TLA+: BeginWaitForSequence / EndWaitForSequence.
  void waitForSequence(std::unique_lock<std::mutex>& lock, u64 targetSeqId);

 private:
  QueueControllerState currentState() const;
  QueueLifecycleEvent classifyTransition(const QueueTransitionRecord& record) const;
#ifndef NDEBUG
  void assertQueueLifecycleInvariants(size_t inflightLimit = 0) const;
  void assertPendingCompletionInvariantsLocked() const;
#endif
  CommandBufferDiagnostics summarizeSubmission(
      u64 seqId,
      size_t slotIndex) const;
  void observeTransition(const QueueTransitionRecord& record) const;
  void enqueuePresent(size_t slotIndex,
                      u64 eventSeqId,
                      const SwapDesc& present,
                      Handle sourceHandle,
                      const std::function<void()>& mutate = {});
  void observeWriterWait(size_t slotIndex, u64 eventSeqId, size_t inflightLimit);
  void acquireWriterSlot(size_t slotIndex,
                         u64 eventSeqId,
                         size_t inflightLimit,
                         const std::function<void()>& mutate = {});
  void commitEmpty(size_t slotIndex, u64 eventSeqId, const std::function<void()>& mutate = {});
  void observeCommitWait(size_t slotIndex, u64 eventSeqId, size_t inflightLimit);
  void commitPublish(size_t slotIndex,
                     u64 eventSeqId,
                     size_t inflightLimit,
                     const std::function<void()>& mutate = {});
  void encodeDequeue(size_t slotIndex, u64 eventSeqId, const std::function<void()>& mutate = {});
  void finishInline(size_t slotIndex, u64 eventSeqId, const std::function<void()>& mutate = {});
  void finishDequeue(u64 eventSeqId, const std::function<void()>& mutate = {});
  void reclaimFree(size_t slotIndex, u64 eventSeqId, const std::function<void()>& mutate = {});
  void observeWaitForSequence(u64 targetSeqId);
  void notePresentEnqueue(const QueueControllerState& state,
                          size_t slotIndex,
                          u64 eventSeqId,
                          const SwapDesc& present,
                          Handle sourceHandle) const;
  void noteWriterWaitBeginIfNeeded(const QueueControllerState& state,
                                   size_t slotIndex,
                                   u64 eventSeqId,
                                   size_t inflightLimit) const;
  void noteWriterWaitEnd(const QueueControllerState& state,
                         size_t slotIndex,
                         u64 eventSeqId) const;
  void noteWriterAcquire(const QueueControllerState& state,
                         size_t slotIndex,
                         u64 eventSeqId) const;
  void noteCommitEmpty(const QueueControllerState& state,
                       size_t slotIndex,
                       u64 eventSeqId) const;
  void noteCommitWaitBeginIfNeeded(const QueueControllerState& state,
                                   size_t slotIndex,
                                   u64 eventSeqId,
                                   size_t inflightLimit) const;
  void noteCommitWaitEnd(const QueueControllerState& state,
                         size_t slotIndex,
                         u64 eventSeqId) const;
  void noteCommitPublish(const QueueControllerState& state,
                         size_t slotIndex,
                         u64 eventSeqId) const;
  void noteEncodeDequeue(const QueueControllerState& state,
                         size_t slotIndex,
                         u64 eventSeqId) const;
  void noteEncodeCommit(const QueueControllerState& state,
                        size_t slotIndex,
                        u64 eventSeqId) const;
  void noteGpuComplete(const QueueControllerState& state,
                       size_t slotIndex,
                       u64 eventSeqId) const;
  void noteFinishInline(const QueueControllerState& state,
                        size_t slotIndex,
                        u64 eventSeqId) const;
  void noteFinishDequeue(const QueueControllerState& state,
                         u64 eventSeqId) const;
  void noteReclaimFree(const QueueControllerState& state,
                       size_t slotIndex,
                       u64 eventSeqId) const;
  void noteWaitSeqBeginIfNeeded(const QueueControllerState& state,
                                u64 targetSeqId) const;
  void noteWaitSeqEnd(const QueueControllerState& state,
                      u64 targetSeqId) const;
  void transition(QueueTransitionRecord record, const std::function<void()>& mutate = {});
  void enqueueSubmission(QueueSubmissionRecord record);
  void submit(QueueSubmissionRecord& record);

  SubmissionBinding submissionBinding_{};

 public:
  // Records that have been committed to Metal and are awaiting GPU completion.
  // The owning MetalBackendDevice runs a dedicated completion-watcher thread
  // that pops from this queue, calls waitUntilCompleted() on each
  // commandBuffer, and then drives the downstream completion pipeline
  // (diagnostics + completedSeqQueue + transitions).
  struct PendingCompletion {
    WMT::Reference<WMT::CommandBuffer> commandBuffer;
    CommandBufferDiagnostics diagnostics{};
    std::string contextValue{};
    size_t slotIndex = 0;
    u64 seqId = 0;
    u64 commandBufferChainLength = 1;
    WMT::Reference<WMT::CounterSampleBuffer> renderEncoderGpuSampleBuffer{};
    std::vector<QueueSubmissionRecord::RenderEncoderGpuSample> renderEncoderGpuSamples{};
  };

  // Drain one pending completion — blocks on waitUntilCompleted() and then
  // runs the diagnostics / completedSeqQueue / transition work. Called from
  // the dedicated completion-watcher thread. Returns true if a record was
  // processed, false on stop. `stop` is read under pendingCompletionMutex_.
  bool processOnePendingCompletion(bool& stop);

  void notifyPendingCompletionStop() {
    pendingCompletionCv_.notify_all();
  }

 private:
  std::mutex pendingCompletionMutex_{};
  std::condition_variable pendingCompletionCv_{};
  std::deque<PendingCompletion> pendingCompletion_{};
};

class CompletionTracker {
 public:
  bool inspect(obj_handle_t commandBuffer, const CommandBufferDiagnostics& diagnostics, const char* context);
  const std::string& lastErrorSummary() const noexcept { return lastErrorSummary_; }

 private:
  std::string commandBufferStatusName(WMTCommandBufferStatus status) const;

  std::string lastErrorSummary_;
};

}  // namespace dxmt9::core::metalqueue
