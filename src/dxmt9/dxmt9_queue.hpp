#pragma once

#include "dxmt9_backend_types.hpp"
#include "dxmt9_capture.hpp"
#include "../winemetal/Metal.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
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

struct NoEnqueueCommitChunkRecordShape {
  u64 recordCount = 0;
  u64 drawRecords = 0;
  u64 constRecords = 0;
  u64 applyStateRecords = 0;
  u64 clearRecords = 0;
  u64 presentRecords = 0;
  u64 surfaceRecords = 0;
  u64 queryRecords = 0;
  u64 otherRecords = 0;
};

struct NoEnqueueFirstPublishSlotShape {
  u64 commandCount = 0;
  u64 drawRunCommands = 0;
  u64 drawItems = 0;
  u64 nonDrawCommands = 0;
  u64 payloadBytes = 0;
  u64 presentCommands = 0;
  u64 prePresentCommands = 0;
  u64 prePresentDrawRunCommands = 0;
  u64 prePresentDrawItems = 0;
  u64 prePresentNonDrawCommands = 0;
  u64 prePresentPayloadBytes = 0;
  u64 postPresentCommands = 0;
  u64 presentTailSlots = 0;
  u64 presentNonTailSlots = 0;
};

NoEnqueueFirstPublishSlotShape summarizeNoEnqueueFirstPublishSlotShape(
    const ChunkSlot& slot) noexcept;

struct QueueCompletionSource {
  size_t slotIndex = 0;
  u64 seqId = 0;
  bool hasPresent = false;
  size_t commandBegin = 0;
  size_t commandCount = 0;
};

inline constexpr size_t kMaxEncodeSessionSources = 32;

struct EncodeSessionSourceList {
  std::array<QueueCompletionSource, kMaxEncodeSessionSources> entries{};
  size_t count = 0;

  bool canAppend(QueueCompletionSource source) const noexcept {
    if (source.seqId == 0 || count >= entries.size()) {
      return false;
    }
    if (count > 0) {
      const auto& previous = entries[count - 1u];
      if (previous.hasPresent || source.seqId != previous.seqId + 1u) {
        return false;
      }
    }
    return true;
  }

  bool append(QueueCompletionSource source) noexcept {
    if (!canAppend(source)) {
      return false;
    }
    entries[count++] = source;
    return true;
  }

  bool assign(std::span<const QueueCompletionSource> sources) noexcept {
    EncodeSessionSourceList next{};
    for (const auto& source : sources) {
      if (!next.append(source)) {
        return false;
      }
    }
    *this = next;
    return true;
  }

  void clear() noexcept {
    entries = {};
    count = 0;
  }
  bool empty() const noexcept { return count == 0; }
  size_t size() const noexcept { return count; }

  std::span<const QueueCompletionSource> span() const noexcept {
    return std::span<const QueueCompletionSource>(entries.data(), count);
  }

  const QueueCompletionSource* begin() const noexcept { return entries.data(); }
  const QueueCompletionSource* end() const noexcept {
    return entries.data() + count;
  }
};

struct ReadySlotSnapshot {
  size_t slotIndex = 0;
  u64 seqId = 0;
  bool hasPresent = false;
  size_t commandBegin = 0;
  size_t commandCount = 0;
  // Non-owning ref into QueueLifecycleController::SubmissionBinding::slots.
  // A dequeued slot is in Encoding state and cannot be recycled until its
  // completion source drains. The scalar fields above are compact selected
  // source metadata; the pointer is only the replay view used during this
  // encode call, not retained session storage.
  ChunkSlot* slot = nullptr;
};

QueueCompletionSource completionSourceForReadySlot(
    const ReadySlotSnapshot& snapshot) noexcept;

using ReadySlotBatchAppendPredicate =
    std::function<bool(std::span<const ReadySlotSnapshot> selected,
                       size_t candidateSlotIndex,
                       const ChunkSlot& candidateSlot)>;
using ReadySlotBatchPrefixSelector =
    std::function<size_t(const std::deque<size_t>& readySlots,
                         std::span<const ChunkSlot> slots,
                         size_t maxCount)>;

struct QueueSubmissionRecord {
  struct RenderEncoderGpuSample {
    u32 startIndex = 0;
    u32 endIndex = 0;
    RenderEncoderGpuPassType passType = RenderEncoderGpuPassType::Unknown;
    u64 seqId = 0;
    u32 slotIndex = 0;
    u32 commandIndex = 0;
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
  // Empty means the legacy one-slot submission: (slotIndex, seqId,
  // diagnostics.hasPresent). EncodeSession/pass-streaming and multi-source
  // paths fill this with every source slot completed by the same tail command
  // buffer, in strict seqId order, without heap allocation.
  EncodeSessionSourceList fixedCompletionSources{};
  std::span<const QueueCompletionSource> explicitCompletionSourceSpan()
      const noexcept {
    return fixedCompletionSources.span();
  }
  bool assignFixedCompletionSources(
      std::span<const QueueCompletionSource> sources) {
    if (!fixedCompletionSources.assign(sources)) {
      return false;
    }
    return true;
  }
  CommandBufferDiagnostics diagnostics{};
  const char* context = "queue";
  WMT::Reference<WMT::CounterSampleBuffer> renderEncoderGpuSampleBuffer{};
  std::vector<RenderEncoderGpuSample> renderEncoderGpuSamples{};
  std::vector<std::function<void()>> postCommitCallbacks;
  std::vector<std::function<void()>> completionCallbacks;
  std::vector<std::shared_ptr<void>> retainedPayloads;
};

CommandBufferDiagnostics summarizeChunk(u64 seqId,
                                        size_t slotIndex,
                                        std::span<const ChunkObservation> observations);
CommandBufferDiagnostics summarizeCommands(u64 seqId,
                                          size_t slotIndex,
                                          const ChunkSlot& slot,
                                          const std::function<u32(Handle)>& resolveSurfaceFlags);
CommandBufferDiagnostics mergeCommandBufferDiagnostics(
    CommandBufferDiagnostics aggregate,
    const CommandBufferDiagnostics& source) noexcept;
bool mergeEncodedPendingTailSubmission(
    QueueSubmissionRecord& tail,
    QueueSubmissionRecord& encodedHead,
    std::span<const QueueCompletionSource> encodedHeadSources,
    QueueCompletionSource tailSource,
    bool encodedHeadTailAlreadyCommitted = false,
    EncodeSessionSourceList* mergedSourcesOut = nullptr);
Handle selectPresentSourceHandle(const SwapDesc& desc, Handle currentBackBuffer) noexcept;
QueueTraceSnapshot makeQueueTraceSnapshot(const QueueTraceState& state);
void appendCompletionSourcesToQueues(
    std::deque<u64>& completedSeqQueue,
    std::deque<u64>* completedPresentSeqQueue,
    u64 completedSeqId,
    std::span<const QueueCompletionSource> sources);

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
  bool dequeueReadySlot(std::unique_lock<std::mutex>& lock,
                        ReadySlotSnapshot& out);
  // TLA+: EncodeDequeue for one or more consecutive ready slots. The caller
  // owns `out`; no heap allocation is performed inside the queue primitive.
  size_t dequeueReadySlotBatch(std::unique_lock<std::mutex>& lock,
                               std::span<ReadySlotSnapshot> out,
                               const ReadySlotBatchAppendPredicate& canAppend = {});
  // TLA+: EncodeDequeue for a caller-selected ready-slot prefix. The selector
  // must only return a FIFO prefix length. Returning zero falls back to the
  // legacy single-source dequeue so incomplete multi-source patterns cannot
  // be consumed accidentally.
  size_t dequeueReadySlotBatchPrefix(std::unique_lock<std::mutex>& lock,
                                     std::span<ReadySlotSnapshot> out,
                                     const ReadySlotBatchPrefixSelector& selectPrefix);
  // Visibility-only staging for tail-Present overlap experiments. The slot
  // remains Pending/in-flight, but is removed from encode-visible readySlots
  // until a later Present-only tail releases it back before the tail source.
  bool stageLastReadySlot(std::unique_lock<std::mutex>& lock,
                          std::deque<size_t>& stagedSlots,
                          size_t expectedSlotIndex);
  size_t releaseStagedSlotsBeforeReadyTail(std::unique_lock<std::mutex>& lock,
                                           std::deque<size_t>& stagedSlots,
                                           size_t tailSlotIndex);
  // Encoded-head carrier for open-CB / pending-tail experiments. Sources must
  // already be dequeued into Encoding state; this records their completion
  // identity without making them ready-visible or GPU-complete.
  size_t retainEncodedSourcesForPendingTail(std::unique_lock<std::mutex>& lock,
                                            std::span<const ReadySlotSnapshot> sources,
                                            std::span<QueueCompletionSource> out);
  // TLA+: EncodeDequeue followed by EncodeSubmitToGpu or EncodeCompleteInline.
  bool runEncodeIteration(
      std::unique_lock<std::mutex>& lock,
      const std::function<std::optional<QueueSubmissionRecord>(size_t, ChunkSlot&)>& encodeFn,
      const std::function<void(u64)>& onInlineComplete = {});
  // TLA+: EncodeDequeue for a caller-provided batch, followed by
  // EncodeSubmitToGpu for every source slot carried by the returned tail
  // submission, or EncodeCompleteInline for every dequeued source slot.
  // The caller owns `scratch`; no heap allocation is performed by dequeue.
  bool runEncodeBatchIteration(
      std::unique_lock<std::mutex>& lock,
      std::span<ReadySlotSnapshot> scratch,
      const std::function<std::optional<QueueSubmissionRecord>(
          std::span<ReadySlotSnapshot>)>& encodeFn,
      const std::function<void(u64)>& onInlineComplete = {},
      const ReadySlotBatchAppendPredicate& canAppend = {},
      const ReadySlotBatchPrefixSelector& selectPrefix = {});
  // TLA+: present-bearing metadata append before CommitPublish.
  void appendPresentCommand(const SwapDesc& present, Handle sourceHandle);
  // TLA+: EncodeSubmitToGpu.
  void submitEncodedChunk(WMT::Reference<WMT::CommandBuffer> commandBuffer,
                          size_t slotIndex,
                          u64 seqId,
                          const char* context = "queue");
  // TLA+: EncodeSubmitToGpu for an externally prepared tail submission.
  // Used by split encode carriers once they have assembled the final
  // fixed completion-source chain.
  void submitEncodedSubmission(std::unique_lock<std::mutex>& lock,
                               QueueSubmissionRecord& record);
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
  // Queue-local observation used by experimental open-CB carriers to decide
  // whether a prefix submit would actually overlap the completion thread's
  // wait. This does not participate in ordering or lifetime decisions.
  bool completionWaitActive();
  // Diagnostic stage probes from the last no-enqueue completion wait end to
  // producer-side commit_chunk milestones.
  void recordCompletionWaitCommitChunkEntry();
  void recordCompletionWaitCommitChunkReplayStart();
  void recordCompletionWaitCommitChunkReplayEnd(std::uint64_t replayNanoseconds);
  void recordNoEnqueueWaitGapToCommitChunkEntry();
  void recordNoEnqueueWaitGapToCommitChunkReplayStart();
  void recordNoEnqueueWaitGapToCommitChunkReplayEnd();
  void recordNoEnqueueCommitChunkReplayCpuBeforePublish(
      std::uint64_t nanoseconds);
  void recordNoEnqueueCommitChunkActiveReplayCpuBeforePublish(
      std::uint64_t nanoseconds);
  void recordNoEnqueueCommitChunkRecordShapeBeforePublish(
      const NoEnqueueCommitChunkRecordShape& shape);
  void recordNoEnqueueFirstPublishSlotShapeBeforePublish(
      const NoEnqueueFirstPublishSlotShape& shape);

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
  CommandBufferDiagnostics summarizeSubmissionSources(
      const QueueSubmissionRecord& record,
      std::span<const QueueCompletionSource> sources) const;
  void recordNoEnqueueWaitGapToCommitPublish();
  void recordNoEnqueueCommitPublishWaitBeforePublish(
      std::uint64_t nanoseconds);
  void recordNoEnqueueCommitPublishOnBeforePublishCpu(
      std::uint64_t nanoseconds);
  void recordNoEnqueueWaitGapToEncodeDequeue();
  void recordNoEnqueueWaitGapToCommandBufferCommit();
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
    EncodeSessionSourceList fixedCompletionSources{};
    std::span<const QueueCompletionSource> explicitCompletionSourceSpan()
        const noexcept {
      return fixedCompletionSources.span();
    }
    u64 commandBufferChainLength = 1;
    std::chrono::steady_clock::time_point enqueueTime{};
    WMT::Reference<WMT::CounterSampleBuffer> renderEncoderGpuSampleBuffer{};
    std::vector<QueueSubmissionRecord::RenderEncoderGpuSample> renderEncoderGpuSamples{};
    std::vector<std::function<void()>> completionCallbacks;
    std::vector<std::shared_ptr<void>> retainedPayloads;
  };

  // Drain one pending completion — blocks on waitUntilCompleted() and then
  // runs the diagnostics / completedSeqQueue / transition work. Called from
  // the dedicated completion-watcher thread. Returns true if a record was
  // processed, false on stop. `stop` is read under pendingCompletionMutex_.
  bool processOnePendingCompletion(bool& stop);
  // CPU-only specs use this to exercise the completion-watcher expansion
  // path without manufacturing a fake Objective-C command-buffer handle.
  // Production submissions enter the same pending queue through submit().
  void enqueuePendingCompletionForTest(PendingCompletion pending);

  void notifyPendingCompletionStop() {
    pendingCompletionCv_.notify_all();
  }

 private:
  void resetNoEnqueueGapProgressLocked();

  std::mutex pendingCompletionMutex_{};
  std::condition_variable pendingCompletionCv_{};
  std::deque<PendingCompletion> pendingCompletion_{};
  bool completionWaitActive_ = false;
  std::uint64_t completionWaitEnqueues_ = 0;
  std::chrono::steady_clock::time_point completionWaitCommitPublishTime_{};
  std::chrono::steady_clock::time_point completionWaitEncodeDequeueTime_{};
  std::chrono::steady_clock::time_point lastNoEnqueueCompletionWaitEnd_{};
  bool noEnqueueGapCommitPublishRecorded_ = false;
  bool noEnqueueGapEncodeDequeueRecorded_ = false;
  bool noEnqueueGapCommandBufferCommitRecorded_ = false;
  bool noEnqueueGapCommitChunkEntryRecorded_ = false;
  bool noEnqueueGapCommitChunkReplayStartRecorded_ = false;
  bool noEnqueueGapCommitChunkReplayEndRecorded_ = false;
  bool noEnqueueGapCommitPublishOnBeforePublishRecorded_ = false;
  std::chrono::steady_clock::time_point noEnqueueGapCommitChunkEntryTime_{};
  std::chrono::steady_clock::time_point noEnqueueGapCommitPublishTime_{};
  std::chrono::steady_clock::time_point noEnqueueGapEncodeDequeueTime_{};
  std::chrono::steady_clock::time_point noEnqueueGapLastCommitChunkReplayEndTime_{};
  std::uint64_t noEnqueueGapCommitChunkEntriesBeforePublish_ = 0;
  std::uint64_t noEnqueueGapCommitChunkReplayStartsBeforePublish_ = 0;
  std::uint64_t noEnqueueGapCommitChunkReplayEndsBeforePublish_ = 0;
  std::uint64_t noEnqueueGapCommitChunkCompletedReplayCpuBeforePublishNs_ = 0;
  std::uint64_t noEnqueueGapCommitChunkActiveReplayCpuBeforePublishNs_ = 0;
  std::uint64_t noEnqueueGapCommitChunkInterReplayGapBeforePublishNs_ = 0;
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
