#include "../../../src/dxmt9/render/backend_interface.hpp"
#include "../../../src/dxmt9/render/tail_present_batch.hpp"
#include "../../../src/dxmt9/dxmt9_draw_encoder.hpp"
#include "../../../src/dxmt9/dxmt9_draw_encoder_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using dxmt9::core::ChunkSlot;
using dxmt9::core::DrawParam;
using dxmt9::core::DrawParamPayloadView;
using dxmt9::core::DrawUniformPayload;
using dxmt9::core::metalqueue::QueueSubmissionRecord;
using dxmt9::core::metalqueue::ReadySlotSnapshot;
using dxmt9::encoders::RenderPassEntryDecision;
using dxmt9::render::OpenCbPendingTailWaitAction;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw TestFailure(std::move(message));
}

void check(bool condition, std::string_view message) {
  if (!condition) {
    fail(std::string(message));
  }
}

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    fail(std::string(message));
  }
}

void checkBytes(std::span<const dxmt9::core::u8> left,
                std::span<const dxmt9::core::u8> right,
                std::string_view message) {
  if (left.size() != right.size() ||
      !std::equal(left.begin(), left.end(), right.begin())) {
    fail(std::string(message));
  }
}

class CountingBackend final : public dxmt9::render::IRenderBackend {
 public:
  std::optional<QueueSubmissionRecord> onChunkReady(
      dxmt9::encoders::EncodeContext&,
      std::size_t slotIndex,
      const ChunkSlot& slot,
      dxmt9::encoders::EncodeChunkOptions = {}) override {
    ++singleCalls;
    lastSlotIndex = slotIndex;
    lastSeqId = slot.seqId;
    QueueSubmissionRecord record;
    record.slotIndex = slotIndex;
    record.seqId = slot.seqId;
    return record;
  }

  dxmt9::render::BackendMode mode() const override {
    return dxmt9::render::BackendMode::Traditional;
  }

  std::size_t singleCalls = 0;
  std::size_t lastSlotIndex = 0;
  std::uint64_t lastSeqId = 0;
};

struct ContextFixture {
  dxmt9::core::BackendLimits limits{};
  dxmt9::CommandQueue queue{WMT::Device{}, limits};
  dxmt9::encoders::EncodeContext ctx{
      .device = {},
      .limits = limits,
      .pool = queue.pool(),
      .cache = queue.pipelineCache(),
      .allocators = queue.allocators(),
      .shaderArchive = nullptr,
      .shaderArchivePath = nullptr,
      .queue = queue,
  };
};

ReadySlotSnapshot makeReadySource(ChunkSlot& slot,
                                  std::size_t slotIndex,
                                  std::uint64_t seqId) {
  ReadySlotSnapshot source;
  source.slotIndex = slotIndex;
  slot.seqId = seqId;
  source.slot = &slot;
  return source;
}

ReadySlotSnapshot makeClearSource(ChunkSlot& slot,
                                  std::size_t slotIndex,
                                  std::uint64_t seqId) {
  auto source = makeReadySource(slot, slotIndex, seqId);
  slot.appendClear({});
  return source;
}

ReadySlotSnapshot makePresentOnlySource(ChunkSlot& slot,
                                        std::size_t slotIndex,
                                        std::uint64_t seqId) {
  auto source = makeReadySource(slot, slotIndex, seqId);
  slot.appendPresent({}, {});
  return source;
}

DrawUniformPayload makeUniformPayload(std::uint64_t hash) {
  DrawUniformPayload payload{};
  payload.vertexConstantsHash = hash + 1u;
  payload.pixelConstantsHash = hash + 2u;
  payload.fixedPayloadHash = hash + 3u;
  payload.hash = hash;
  return payload;
}

void appendTestDraw(ChunkSlot& slot,
                    std::uint32_t primitiveCount,
                    DrawUniformPayload uniforms,
                    std::span<const dxmt9::core::u8> userVertexData,
                    std::span<const dxmt9::core::u8> bindingOverrideData) {
  std::array<DrawParam, 1> draws{DrawParam{
      .primitiveCount = primitiveCount,
  }};
  std::array<DrawParamPayloadView, 1> payloads{DrawParamPayloadView{
      .userVertexData = userVertexData,
      .bindingOverrideData = bindingOverrideData,
  }};
  slot.appendDrawRun(dxmt9::core::CanonicalDrawState{}, uniforms,
                     std::span<const DrawParam>(draws.data(), draws.size()),
                     std::span<const DrawParamPayloadView>(
                         payloads.data(), payloads.size()));
}

void emptyBatchCompletesInlineByDefault() {
  ContextFixture fixture;
  CountingBackend backend;
  std::array<ReadySlotSnapshot, 0> sources{};

  const auto submission = backend.onChunkBatchReady(fixture.ctx, sources);

  check(!submission.has_value(), "empty backend batch returns no submission");
  checkEq(backend.singleCalls, 0u, "empty batch does not call single-source path");
}

void encodeChunkOptionsDefaultToFreshCommandBufferPath() {
  dxmt9::encoders::EncodeChunkOptions options{};
  check(!options.hasInjectedCommandBuffer(),
        "default encodeChunk options do not inject a command buffer");
  check(!options.disableMidChunkCommits,
        "default encodeChunk options keep current mid-chunk commit policy");
  check(!options.disablePresentAcquireSplit,
        "default encodeChunk options keep current present-acquire split policy");
  check(!options.deferSessionFinalization,
        "default encodeChunk options finalize the session before return");
}

void singleSourceBatchFallsBackToOnChunkReady() {
  ContextFixture fixture;
  CountingBackend backend;
  ChunkSlot slot;
  std::array<ReadySlotSnapshot, 1> sources{
      makeReadySource(slot, /*slotIndex=*/3, /*seqId=*/11),
  };

  const auto submission = backend.onChunkBatchReady(fixture.ctx, sources);

  check(submission.has_value(), "single-source batch returns backend submission");
  checkEq(backend.singleCalls, 1u, "single-source batch calls onChunkReady once");
  checkEq(backend.lastSlotIndex, 3u, "single-source batch forwards slot index");
  checkEq(backend.lastSeqId, 11ull, "single-source batch forwards seqId");
  checkEq(submission->slotIndex, 3u, "submission keeps forwarded slot index");
  checkEq(submission->seqId, 11ull, "submission keeps forwarded seqId");
}

void multiSourceBatchRequiresExplicitBackendImplementation() {
  ContextFixture fixture;
  CountingBackend backend;
  std::array<ChunkSlot, 2> slots{};
  std::array<ReadySlotSnapshot, 2> sources{
      makeReadySource(slots[0], /*slotIndex=*/1, /*seqId=*/5),
      makeReadySource(slots[1], /*slotIndex=*/2, /*seqId=*/6),
  };

  const auto submission = backend.onChunkBatchReady(fixture.ctx, sources);

  check(!submission.has_value(), "multi-source batch has no default submission");
  checkEq(backend.singleCalls, 0u,
          "multi-source batch does not silently encode only the first source");
}

void tailPresentBatchShapeRequiresFinalPresentTail() {
  std::array<ChunkSlot, 2> slots{};
  std::array<ReadySlotSnapshot, 2> valid{
      makeClearSource(slots[0], /*slotIndex=*/1, /*seqId=*/5),
      makePresentOnlySource(slots[1], /*slotIndex=*/2, /*seqId=*/6),
  };
  check(dxmt9::render::canCoalesceTailPresentBatch(valid),
        "non-present head plus present-only tail is coalescable");
  check(dxmt9::render::slotIsPresentOnlyTail(*valid.back().slot),
        "tail source is recognized as present-only");
  check(dxmt9::render::slotHasFinalPresentTail(*valid.back().slot),
        "present-only tail is also a final-present tail");

  std::array<ChunkSlot, 2> headWithPresentSlots{};
  std::array<ReadySlotSnapshot, 2> headWithPresent{
      makeClearSource(headWithPresentSlots[0], /*slotIndex=*/1, /*seqId=*/5),
      makePresentOnlySource(headWithPresentSlots[1], /*slotIndex=*/2, /*seqId=*/6),
  };
  headWithPresentSlots[0].appendPresent({}, {});
  check(!dxmt9::render::canCoalesceTailPresentBatch(headWithPresent),
        "head source with an existing present is rejected");

  std::array<ChunkSlot, 2> tailWithPrePresentWorkSlots{};
  std::array<ReadySlotSnapshot, 2> tailWithPrePresentWork{
      makeClearSource(tailWithPrePresentWorkSlots[0], /*slotIndex=*/1,
                      /*seqId=*/5),
      makeReadySource(tailWithPrePresentWorkSlots[1], /*slotIndex=*/2,
                      /*seqId=*/6),
  };
  tailWithPrePresentWorkSlots[1].appendClear({});
  tailWithPrePresentWorkSlots[1].appendPresent({}, {});
  check(!dxmt9::render::slotIsPresentOnlyTail(
            *tailWithPrePresentWork.back().slot),
        "tail with pre-Present work is not present-only");
  check(dxmt9::render::slotHasFinalPresentTail(
            *tailWithPrePresentWork.back().slot),
        "tail with pre-Present work still has a final Present");
  check(dxmt9::render::canCoalesceTailPresentBatch(tailWithPrePresentWork),
        "tail source with pre-Present work is coalescable");

  std::array<ChunkSlot, 2> tailWithPostPresentWorkSlots{};
  std::array<ReadySlotSnapshot, 2> tailWithPostPresentWork{
      makeClearSource(tailWithPostPresentWorkSlots[0], /*slotIndex=*/1,
                      /*seqId=*/5),
      makeReadySource(tailWithPostPresentWorkSlots[1], /*slotIndex=*/2,
                      /*seqId=*/6),
  };
  tailWithPostPresentWorkSlots[1].appendPresent({}, {});
  tailWithPostPresentWorkSlots[1].appendClear({});
  check(!dxmt9::render::slotHasFinalPresentTail(
            *tailWithPostPresentWork.back().slot),
        "tail with post-Present work is not a final-present tail");
  check(!dxmt9::render::canCoalesceTailPresentBatch(tailWithPostPresentWork),
        "tail source with post-Present work is rejected");
}

void openCbPreencodeHeadRequiresPresentSplitBeforePublish() {
  ChunkSlot head;
  head.publishReason = dxmt9::perf::ChunkPublishReason::PresentSplitBefore;
  head.appendClear({});
  check(dxmt9::render::slotIsOpenCbPreencodeHead(head),
        "PresentSplitBefore non-present work can be pre-encoded");

  auto wrongReason = head;
  wrongReason.publishReason = dxmt9::perf::ChunkPublishReason::DrawLimit;
  check(!dxmt9::render::slotIsOpenCbPreencodeHead(wrongReason),
        "other publish reasons are not held for a future Present tail");

  auto withPresent = head;
  withPresent.appendPresent({}, {});
  check(!dxmt9::render::slotIsOpenCbPreencodeHead(withPresent),
        "present-bearing chunks are final tails, not pre-encoded heads");

  head.clearCommands();
  check(head.publishReason == dxmt9::perf::ChunkPublishReason::Unknown,
        "clearCommands resets publish reason before slot reuse");
}

void openCbPendingAppendPolicyAllowsFinalPresentTail() {
  ChunkSlot head;
  head.publishReason = dxmt9::perf::ChunkPublishReason::PresentSplitBefore;
  head.appendClear({});
  check(dxmt9::render::slotCanStartOpenCbPendingSession(
            head, /*carryRenderSession=*/false,
            /*tailReadyForCurrentHead=*/false),
        "open-CB preencode heads can start pending work");
  check(!dxmt9::render::slotCanStartOpenCbPendingSession(
            head, /*carryRenderSession=*/true,
            /*tailReadyForCurrentHead=*/false),
        "draw-count preencode heads do not start carried sessions without a ready tail");
  check(dxmt9::render::slotCanStartOpenCbPendingSession(
            head, /*carryRenderSession=*/true,
            /*tailReadyForCurrentHead=*/true),
        "draw-count preencode heads may start when the final Present tail is ready");

  ChunkSlot presentOnly;
  presentOnly.appendPresent({}, {});
  check(dxmt9::render::slotHasFinalPresentTail(presentOnly),
        "present-only tail has final Present");
  check(dxmt9::render::slotCanAppendToOpenCbPending(
            presentOnly, /*carryRenderSession=*/false,
            /*hasPendingSession=*/false,
            /*tailReadyForCurrentHead=*/false),
        "present-only tails can finalize pending work");

  ChunkSlot ordinaryWork;
  ordinaryWork.appendClear({});
  check(!dxmt9::render::slotCanStartOpenCbPendingSession(
            ordinaryWork, /*carryRenderSession=*/false,
            /*tailReadyForCurrentHead=*/false),
        "ordinary work cannot start a pending session without carry mode");
  check(!dxmt9::render::slotCanStartOpenCbPendingSession(
            ordinaryWork, /*carryRenderSession=*/true,
            /*tailReadyForCurrentHead=*/false),
        "ordinary work cannot start a carried session without a semantic boundary");
  check(dxmt9::render::slotCanStartOpenCbPendingSession(
            ordinaryWork, /*carryRenderSession=*/true,
            /*tailReadyForCurrentHead=*/true),
        "ordinary non-present work may start when the final Present tail is ready");
  check(!dxmt9::render::slotCanAppendToOpenCbPending(
            ordinaryWork, /*carryRenderSession=*/true,
            /*hasPendingSession=*/false,
            /*tailReadyForCurrentHead=*/false),
        "ordinary work cannot append before a session exists");
  check(dxmt9::render::slotCanAppendToOpenCbPending(
            ordinaryWork, /*carryRenderSession=*/true,
            /*hasPendingSession=*/true,
            /*tailReadyForCurrentHead=*/false),
        "ordinary non-present work can append once a carried session exists");
  check(dxmt9::render::slotCanAppendToOpenCbPending(
            ordinaryWork, /*carryRenderSession=*/true,
            /*hasPendingSession=*/true,
            /*tailReadyForCurrentHead=*/true),
        "ordinary non-present work may append when the final Present tail is ready");

  ChunkSlot semanticWork;
  semanticWork.publishReason = dxmt9::perf::ChunkPublishReason::SemanticBoundary;
  semanticWork.appendClear({});
  check(dxmt9::render::slotCanStartOpenCbPendingSession(
            semanticWork, /*carryRenderSession=*/true,
            /*tailReadyForCurrentHead=*/false),
        "semantic boundaries can start carried sessions before the tail is ready");
  check(dxmt9::render::slotCanAppendToOpenCbPending(
            semanticWork, /*carryRenderSession=*/true,
            /*hasPendingSession=*/true,
            /*tailReadyForCurrentHead=*/false),
        "semantic boundaries can append to carried sessions before the tail is ready");

  ChunkSlot presentWithPreWork;
  presentWithPreWork.appendClear({});
  presentWithPreWork.appendPresent({}, {});
  check(!dxmt9::render::slotIsPresentOnlyTail(presentWithPreWork),
        "pre-Present work makes the tail non-present-only");
  check(dxmt9::render::slotHasFinalPresentTail(presentWithPreWork),
        "pre-Present work can still end in a final Present");
  check(dxmt9::render::slotCanAppendToOpenCbPending(
            presentWithPreWork, /*carryRenderSession=*/true,
            /*hasPendingSession=*/true,
            /*tailReadyForCurrentHead=*/false),
        "final-present tails can finalize an open-CB session");

  ChunkSlot presentWithPostWork;
  presentWithPostWork.appendPresent({}, {});
  presentWithPostWork.appendClear({});
  check(!dxmt9::render::slotHasFinalPresentTail(presentWithPostWork),
        "post-Present work is not a final-present tail");
  check(!dxmt9::render::slotCanAppendToOpenCbPending(
            presentWithPostWork, /*carryRenderSession=*/true,
            /*hasPendingSession=*/true,
            /*tailReadyForCurrentHead=*/false),
        "present-bearing work must not append after the final Present");
}

void openCbPresentTailSplitRequiresCurrentPrePresentWork() {
  check(!dxmt9::render::openCbPresentTailNeedsPrePresentSplit(
            /*openCbPreencodeTailPresent=*/false,
            /*hasCurrentPrePresentWork=*/true),
        "open-CB tail split is opt-in");
  check(!dxmt9::render::openCbPresentTailNeedsPrePresentSplit(
            /*openCbPreencodeTailPresent=*/true,
            /*hasCurrentPrePresentWork=*/false),
        "open-CB tail split requires current pre-Present work");
  check(dxmt9::render::openCbPresentTailNeedsPrePresentSplit(
            /*openCbPreencodeTailPresent=*/true,
            /*hasCurrentPrePresentWork=*/true),
        "open-CB tail split requests a final Present-only tail source");
}

void openCbPendingTailWaitActionUsesQueueLocalState() {
  check(dxmt9::render::selectOpenCbPendingTailWaitAction(
            /*hasPendingRecord=*/false, /*readySlotsEmpty=*/true,
            /*stopRequested=*/false, /*writerActive=*/false,
            /*timeoutEnabled=*/false) ==
            OpenCbPendingTailWaitAction::None,
        "no pending record needs no tail wait action");
  check(dxmt9::render::selectOpenCbPendingTailWaitAction(
            /*hasPendingRecord=*/true, /*readySlotsEmpty=*/false,
            /*stopRequested=*/false, /*writerActive=*/true,
            /*timeoutEnabled=*/false) ==
            OpenCbPendingTailWaitAction::None,
        "ready sources continue through normal dequeue");
  check(dxmt9::render::selectOpenCbPendingTailWaitAction(
            /*hasPendingRecord=*/true, /*readySlotsEmpty=*/true,
            /*stopRequested=*/false, /*writerActive=*/true,
            /*timeoutEnabled=*/false) ==
            OpenCbPendingTailWaitAction::WaitForReady,
        "active writer lets a pending head wait for the next ready source");
  check(dxmt9::render::selectOpenCbPendingTailWaitAction(
            /*hasPendingRecord=*/true, /*readySlotsEmpty=*/true,
            /*stopRequested=*/false, /*writerActive=*/false,
            /*timeoutEnabled=*/false) ==
            OpenCbPendingTailWaitAction::SubmitPending,
        "inactive writer releases a pending visible prefix deterministically");
  check(dxmt9::render::selectOpenCbPendingTailWaitAction(
            /*hasPendingRecord=*/true, /*readySlotsEmpty=*/true,
            /*stopRequested=*/true, /*writerActive=*/true,
            /*timeoutEnabled=*/false) ==
            OpenCbPendingTailWaitAction::SubmitPending,
        "stop releases a pending visible prefix");
  check(dxmt9::render::selectOpenCbPendingTailWaitAction(
            /*hasPendingRecord=*/true, /*readySlotsEmpty=*/true,
            /*stopRequested=*/false, /*writerActive=*/false,
            /*timeoutEnabled=*/true) ==
            OpenCbPendingTailWaitAction::SubmitPending,
        "inactive writer releases a pending prefix even in timeout mode");
}

void openCbPendingMidChunkPolicyPreservesSemanticSplits() {
  check(dxmt9::render::openCbPendingAllowsSemanticMidChunkCommits(
            /*appendToPending=*/false),
        "fresh pending heads keep semantic pass-boundary sub-CBs enabled");
  check(dxmt9::render::openCbPendingAllowsSemanticMidChunkCommits(
            /*appendToPending=*/true),
        "appended sources keep semantic pass-boundary sub-CBs enabled");
}

void openCbSemanticBoundaryReleaseRequiresCompletionWait() {
  using Mode = dxmt9::render::OpenCbSemanticBoundaryReleaseMode;

  check(dxmt9::render::openCbPendingCanReleaseAtSemanticBoundary(
            /*sourceIsSemanticBoundary=*/true,
            /*sourceHasFinalPresentTail=*/false,
            Mode::CompletionWait,
            /*completionWaitActive=*/true,
            /*semanticReleaseAlreadyUsedDuringWait=*/false),
        "semantic boundary source may release during a completion wait");
  check(!dxmt9::render::openCbPendingCanReleaseAtSemanticBoundary(
            /*sourceIsSemanticBoundary=*/false,
            /*sourceHasFinalPresentTail=*/false,
            Mode::CompletionWait,
            /*completionWaitActive=*/true,
            /*semanticReleaseAlreadyUsedDuringWait=*/false),
        "ordinary sources do not trigger semantic boundary release");
  check(!dxmt9::render::openCbPendingCanReleaseAtSemanticBoundary(
            /*sourceIsSemanticBoundary=*/true,
            /*sourceHasFinalPresentTail=*/true,
            Mode::CompletionWait,
            /*completionWaitActive=*/true,
            /*semanticReleaseAlreadyUsedDuringWait=*/false),
        "final-present tails use the tail submit path");
  check(!dxmt9::render::openCbPendingCanReleaseAtSemanticBoundary(
            /*sourceIsSemanticBoundary=*/true,
            /*sourceHasFinalPresentTail=*/false,
            Mode::CompletionWait,
            /*completionWaitActive=*/false,
            /*semanticReleaseAlreadyUsedDuringWait=*/false),
        "semantic boundary release is useful only while completion waits");
  check(!dxmt9::render::openCbPendingCanReleaseAtSemanticBoundary(
            /*sourceIsSemanticBoundary=*/true,
            /*sourceHasFinalPresentTail=*/false,
            Mode::CompletionWait,
            /*completionWaitActive=*/true,
            /*semanticReleaseAlreadyUsedDuringWait=*/true),
        "only one semantic boundary prefix is released per completion wait");
  check(dxmt9::render::openCbPendingCanReleaseAtSemanticBoundary(
            /*sourceIsSemanticBoundary=*/true,
            /*sourceHasFinalPresentTail=*/false,
            Mode::Deterministic,
            /*completionWaitActive=*/false,
            /*semanticReleaseAlreadyUsedDuringWait=*/true),
        "deterministic semantic-boundary release does not depend on wait state");
}

void openCbSemanticBoundaryReleaseCanPreemptReadySourceDuringWait() {
  using Mode = dxmt9::render::OpenCbSemanticBoundaryReleaseMode;

  check(dxmt9::render::openCbPendingShouldReleaseBeforeReadySource(
            /*readySlotsEmpty=*/false,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::CompletionWait,
            /*completionWaitActive=*/true,
            /*semanticReleaseAlreadyUsedDuringWait=*/false),
        "active wait releases a semantic-boundary pending prefix before appending ready work");
  check(!dxmt9::render::openCbPendingShouldReleaseBeforeReadySource(
            /*readySlotsEmpty=*/true,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::CompletionWait,
            /*completionWaitActive=*/true,
            /*semanticReleaseAlreadyUsedDuringWait=*/false),
        "empty ready queue stays on the existing wait/release path");
  check(!dxmt9::render::openCbPendingShouldReleaseBeforeReadySource(
            /*readySlotsEmpty=*/false,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::CompletionWait,
            /*completionWaitActive=*/false,
            /*semanticReleaseAlreadyUsedDuringWait=*/false),
        "inactive wait preserves ready-source append locality");
  check(!dxmt9::render::openCbPendingShouldReleaseBeforeReadySource(
            /*readySlotsEmpty=*/false,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::CompletionWait,
            /*completionWaitActive=*/true,
            /*semanticReleaseAlreadyUsedDuringWait=*/true),
        "one semantic-boundary preemptive release is allowed per completion wait");
  check(!dxmt9::render::openCbPendingShouldReleaseBeforeReadySource(
            /*readySlotsEmpty=*/false,
            /*canReleaseAtSemanticBoundary=*/false,
            Mode::CompletionWait,
            /*completionWaitActive=*/true,
            /*semanticReleaseAlreadyUsedDuringWait=*/false),
        "ordinary pending prefixes do not preempt ready-source append");
  check(dxmt9::render::openCbPendingShouldReleaseBeforeReadySource(
            /*readySlotsEmpty=*/false,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::Deterministic,
            /*completionWaitActive=*/false,
            /*semanticReleaseAlreadyUsedDuringWait=*/true),
        "deterministic release preempts ready-source append for diagnostics");

  check(dxmt9::render::openCbPendingReadySourceBlocksSemanticReleaseNoCompletionWait(
            /*readySlotsEmpty=*/false,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::CompletionWait,
            /*completionWaitActive=*/false),
        "ready-source append blocks semantic release outside completion wait");
  check(!dxmt9::render::openCbPendingReadySourceBlocksSemanticReleaseNoCompletionWait(
            /*readySlotsEmpty=*/false,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::CompletionWait,
            /*completionWaitActive=*/true),
        "active completion wait releases instead of counting ready-source block");
  check(!dxmt9::render::openCbPendingReadySourceBlocksSemanticReleaseNoCompletionWait(
            /*readySlotsEmpty=*/true,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::CompletionWait,
            /*completionWaitActive=*/false),
        "empty ready queue uses the existing no-completion-wait blocker");
  check(!dxmt9::render::openCbPendingReadySourceBlocksSemanticReleaseNoCompletionWait(
            /*readySlotsEmpty=*/false,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::Deterministic,
            /*completionWaitActive=*/false),
        "deterministic mode is not a completion-wait miss");
}

void openCbSemanticBoundaryNoWaitBlockClassifiesWriterState() {
  using Block = dxmt9::render::OpenCbSemanticReleaseNoCompletionWaitBlock;
  using SlotState =
      dxmt9::render::OpenCbSemanticReleaseWriterActiveSlotState;
  using Mode = dxmt9::render::OpenCbSemanticBoundaryReleaseMode;

  check(dxmt9::render::classifyOpenCbPendingSemanticReleaseNoCompletionWaitBlock(
            /*readySlotsEmpty=*/true,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::CompletionWait,
            /*completionWaitActive=*/false,
            /*writerActive=*/true) == Block::WriterActive,
        "empty-ready semantic release miss records an active writer");
  check(dxmt9::render::classifyOpenCbPendingSemanticReleaseNoCompletionWaitBlock(
            /*readySlotsEmpty=*/true,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::CompletionWait,
            /*completionWaitActive=*/false,
            /*writerActive=*/false) == Block::WriterInactive,
        "empty-ready semantic release miss records an inactive writer");
  check(dxmt9::render::classifyOpenCbPendingSemanticReleaseNoCompletionWaitBlock(
            /*readySlotsEmpty=*/true,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::CompletionWait,
            /*completionWaitActive=*/true,
            /*writerActive=*/true) == Block::None,
        "active completion wait is a release opportunity, not a no-wait miss");
  check(dxmt9::render::classifyOpenCbPendingSemanticReleaseNoCompletionWaitBlock(
            /*readySlotsEmpty=*/false,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::CompletionWait,
            /*completionWaitActive=*/false,
            /*writerActive=*/true) == Block::None,
        "ready-source no-wait misses use the ready-source counter");
  check(dxmt9::render::classifyOpenCbPendingSemanticReleaseNoCompletionWaitBlock(
            /*readySlotsEmpty=*/true,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::Deterministic,
            /*completionWaitActive=*/false,
            /*writerActive=*/true) == Block::None,
        "deterministic diagnostics are not completion-wait misses");

  check(dxmt9::render::classifyOpenCbPendingSemanticReleaseWriterActiveSlotState(
            Block::WriterActive,
            /*writingSlotEmpty=*/true,
            /*writingSlotHasPresent=*/false) == SlotState::Empty,
        "writer-active miss records an empty writing slot");
  check(dxmt9::render::classifyOpenCbPendingSemanticReleaseWriterActiveSlotState(
            Block::WriterActive,
            /*writingSlotEmpty=*/false,
            /*writingSlotHasPresent=*/false) == SlotState::NonPresentWork,
        "writer-active miss records non-present work in the writing slot");
  check(dxmt9::render::classifyOpenCbPendingSemanticReleaseWriterActiveSlotState(
            Block::WriterActive,
            /*writingSlotEmpty=*/false,
            /*writingSlotHasPresent=*/true) == SlotState::PresentBearing,
        "writer-active miss records present-bearing writing slot work");
  check(dxmt9::render::classifyOpenCbPendingSemanticReleaseWriterActiveSlotState(
            Block::WriterInactive,
            /*writingSlotEmpty=*/false,
            /*writingSlotHasPresent=*/false) == SlotState::None,
        "inactive-writer misses do not claim a writer-active slot state");

  check(dxmt9::render::openCbPendingShouldCpuReadyPublishWriterActiveSlot(
            /*readySlotsEmpty=*/true,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::CompletionWait,
            /*completionWaitActive=*/false,
            /*writerActive=*/true,
            /*writingSlotEmpty=*/false,
            /*writingSlotHasPresent=*/false),
        "writer-active no-wait miss with non-present work may be cut as CPU-ready");
  check(!dxmt9::render::openCbPendingShouldCpuReadyPublishWriterActiveSlot(
            /*readySlotsEmpty=*/true,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::CompletionWait,
            /*completionWaitActive=*/false,
            /*writerActive=*/true,
            /*writingSlotEmpty=*/true,
            /*writingSlotHasPresent=*/false),
        "empty writer slot is not a CPU-ready source");
  check(!dxmt9::render::openCbPendingShouldCpuReadyPublishWriterActiveSlot(
            /*readySlotsEmpty=*/true,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::CompletionWait,
            /*completionWaitActive=*/false,
            /*writerActive=*/true,
            /*writingSlotEmpty=*/false,
            /*writingSlotHasPresent=*/true),
        "present-bearing writer slot is not a semantic CPU-ready source");
  check(!dxmt9::render::openCbPendingShouldCpuReadyPublishWriterActiveSlot(
            /*readySlotsEmpty=*/true,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::CompletionWait,
            /*completionWaitActive=*/true,
            /*writerActive=*/true,
            /*writingSlotEmpty=*/false,
            /*writingSlotHasPresent=*/false),
        "active completion wait uses release, not writer-slot publication");
  check(!dxmt9::render::openCbPendingShouldCpuReadyPublishWriterActiveSlot(
            /*readySlotsEmpty=*/true,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::Deterministic,
            /*completionWaitActive=*/false,
            /*writerActive=*/true,
            /*writingSlotEmpty=*/false,
            /*writingSlotHasPresent=*/false),
        "deterministic release mode does not need writer-slot CPU-ready publication");
}

void openCbActiveWaitAppendKeepsReadySourceInSession() {
  using Mode = dxmt9::render::OpenCbSemanticBoundaryReleaseMode;

  check(dxmt9::render::openCbPendingShouldAppendReadySourceBeforeSemanticRelease(
            /*readySlotsEmpty=*/false,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::CompletionWait,
            /*completionWaitActive=*/true,
            /*semanticReleaseAlreadyUsedDuringWait=*/false,
            /*firstReadySourceCanAppendToPending=*/true),
        "active wait appends compatible ready source before semantic release");
  check(!dxmt9::render::openCbPendingShouldAppendReadySourceBeforeSemanticRelease(
            /*readySlotsEmpty=*/false,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::CompletionWait,
            /*completionWaitActive=*/true,
            /*semanticReleaseAlreadyUsedDuringWait=*/false,
            /*firstReadySourceCanAppendToPending=*/false),
        "nonappendable ready source keeps release-before-ready policy");
  check(!dxmt9::render::openCbPendingShouldAppendReadySourceBeforeSemanticRelease(
            /*readySlotsEmpty=*/false,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::CompletionWait,
            /*completionWaitActive=*/false,
            /*semanticReleaseAlreadyUsedDuringWait=*/false,
            /*firstReadySourceCanAppendToPending=*/true),
        "no-wait ready source must not bypass completion-wait release policy");
  check(!dxmt9::render::openCbPendingShouldAppendReadySourceBeforeSemanticRelease(
            /*readySlotsEmpty=*/false,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::CompletionWait,
            /*completionWaitActive=*/true,
            /*semanticReleaseAlreadyUsedDuringWait=*/true,
            /*firstReadySourceCanAppendToPending=*/true),
        "one semantic release per completion-wait window is preserved");
  check(!dxmt9::render::openCbPendingShouldAppendReadySourceBeforeSemanticRelease(
            /*readySlotsEmpty=*/false,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::Deterministic,
            /*completionWaitActive=*/false,
            /*semanticReleaseAlreadyUsedDuringWait=*/false,
            /*firstReadySourceCanAppendToPending=*/true),
        "active-wait append does not alter deterministic release mode");

  check(dxmt9::render::openCbPendingShouldCpuReadyPublishActiveWaitSlot(
            /*readySlotsEmpty=*/true,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::CompletionWait,
            /*completionWaitActive=*/true,
            /*semanticReleaseAlreadyUsedDuringWait=*/false,
            /*writerActive=*/true,
            /*writingSlotEmpty=*/false,
            /*writingSlotHasPresent=*/false),
        "active wait may cut current writer work as a semantic CPU-ready source");
  check(!dxmt9::render::openCbPendingShouldCpuReadyPublishActiveWaitSlot(
            /*readySlotsEmpty=*/false,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::CompletionWait,
            /*completionWaitActive=*/true,
            /*semanticReleaseAlreadyUsedDuringWait=*/false,
            /*writerActive=*/true,
            /*writingSlotEmpty=*/false,
            /*writingSlotHasPresent=*/false),
        "existing ready source is appended directly instead of cutting writer slot");
  check(!dxmt9::render::openCbPendingShouldCpuReadyPublishActiveWaitSlot(
            /*readySlotsEmpty=*/true,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::CompletionWait,
            /*completionWaitActive=*/false,
            /*semanticReleaseAlreadyUsedDuringWait=*/false,
            /*writerActive=*/true,
            /*writingSlotEmpty=*/false,
            /*writingSlotHasPresent=*/false),
        "inactive completion wait uses the older no-wait diagnostic path");
  check(!dxmt9::render::openCbPendingShouldCpuReadyPublishActiveWaitSlot(
            /*readySlotsEmpty=*/true,
            /*canReleaseAtSemanticBoundary=*/true,
            Mode::CompletionWait,
            /*completionWaitActive=*/true,
            /*semanticReleaseAlreadyUsedDuringWait=*/false,
            /*writerActive=*/true,
            /*writingSlotEmpty=*/false,
            /*writingSlotHasPresent=*/true),
        "present-bearing writer slot is not an active-wait semantic source");
}

void openCbWaitStartPublishCreatesFirstPendingSource() {
  check(dxmt9::render::openCbShouldCpuReadyPublishWaitStartSlot(
            /*readySlotsEmpty=*/true,
            /*hasPendingRecord=*/false,
            /*completionWaitActive=*/true,
            /*stopRequested=*/false,
            /*writerActive=*/true,
            /*writingSlotEmpty=*/false,
            /*writingSlotHasPresent=*/false),
        "wait-start may cut current writer work before a pending session exists");
  check(!dxmt9::render::openCbShouldCpuReadyPublishWaitStartSlot(
            /*readySlotsEmpty=*/false,
            /*hasPendingRecord=*/false,
            /*completionWaitActive=*/true,
            /*stopRequested=*/false,
            /*writerActive=*/true,
            /*writingSlotEmpty=*/false,
            /*writingSlotHasPresent=*/false),
        "existing ready sources should be dequeued instead of cutting writer work");
  check(!dxmt9::render::openCbShouldCpuReadyPublishWaitStartSlot(
            /*readySlotsEmpty=*/true,
            /*hasPendingRecord=*/true,
            /*completionWaitActive=*/true,
            /*stopRequested=*/false,
            /*writerActive=*/true,
            /*writingSlotEmpty=*/false,
            /*writingSlotHasPresent=*/false),
        "pending sessions use active-wait append/release policy");
  check(!dxmt9::render::openCbShouldCpuReadyPublishWaitStartSlot(
            /*readySlotsEmpty=*/true,
            /*hasPendingRecord=*/false,
            /*completionWaitActive=*/false,
            /*stopRequested=*/false,
            /*writerActive=*/true,
            /*writingSlotEmpty=*/false,
            /*writingSlotHasPresent=*/false),
        "inactive completion wait keeps the older writer-active diagnostic path");
  check(!dxmt9::render::openCbShouldCpuReadyPublishWaitStartSlot(
            /*readySlotsEmpty=*/true,
            /*hasPendingRecord=*/false,
            /*completionWaitActive=*/true,
            /*stopRequested=*/true,
            /*writerActive=*/true,
            /*writingSlotEmpty=*/false,
            /*writingSlotHasPresent=*/false),
        "stop/drain does not create a new wait-start source");
  check(!dxmt9::render::openCbShouldCpuReadyPublishWaitStartSlot(
            /*readySlotsEmpty=*/true,
            /*hasPendingRecord=*/false,
            /*completionWaitActive=*/true,
            /*stopRequested=*/false,
            /*writerActive=*/true,
            /*writingSlotEmpty=*/true,
            /*writingSlotHasPresent=*/false),
        "empty writer slot is not a wait-start semantic source");
  check(!dxmt9::render::openCbShouldCpuReadyPublishWaitStartSlot(
            /*readySlotsEmpty=*/true,
            /*hasPendingRecord=*/false,
            /*completionWaitActive=*/true,
            /*stopRequested=*/false,
            /*writerActive=*/true,
            /*writingSlotEmpty=*/false,
            /*writingSlotHasPresent=*/true),
        "present-bearing writer slot is not a wait-start semantic source");
}

void openCbInitializerWaitBoundarySubmitsPendingBeforeAppend() {
  check(dxmt9::render::openCbPendingShouldSubmitBeforeInitializerWait(
            /*canAppendToPending=*/true,
            /*pendingSessionHasActiveRender=*/true,
            /*initializerHasPendingUploads=*/true),
        "initializer uploads split before appending to an active render session");
  check(!dxmt9::render::openCbPendingShouldSubmitBeforeInitializerWait(
            /*canAppendToPending=*/false,
            /*pendingSessionHasActiveRender=*/true,
            /*initializerHasPendingUploads=*/true),
        "non-appendable sources use the existing non-appendable path");
  check(!dxmt9::render::openCbPendingShouldSubmitBeforeInitializerWait(
            /*canAppendToPending=*/true,
            /*pendingSessionHasActiveRender=*/false,
            /*initializerHasPendingUploads=*/true),
        "pending initializer uploads do not split an inactive render session");
  check(!dxmt9::render::openCbPendingShouldSubmitBeforeInitializerWait(
            /*canAppendToPending=*/true,
            /*pendingSessionHasActiveRender=*/true,
            /*initializerHasPendingUploads=*/false),
        "active render sessions can append when no initializer wait is pending");
}

void openCbPendingWakeRecheckTracksCompletionWaitTransitions() {
  using Mode = dxmt9::render::OpenCbSemanticBoundaryReleaseMode;

  check(dxmt9::render::openCbPendingCompletionWaitTransitionNeedsRecheck(
            /*completionWaitActive=*/true,
            /*waitObservedCompletionWaitActive=*/false,
            Mode::CompletionWait,
            /*canReleaseAtSemanticBoundary=*/true,
            /*semanticReleaseAlreadyUsedDuringWait=*/false),
        "wait-start wakes a releasable semantic-boundary prefix");
  check(!dxmt9::render::openCbPendingCompletionWaitTransitionNeedsRecheck(
            /*completionWaitActive=*/true,
            /*waitObservedCompletionWaitActive=*/false,
            Mode::CompletionWait,
            /*canReleaseAtSemanticBoundary=*/true,
            /*semanticReleaseAlreadyUsedDuringWait=*/true),
        "already-used semantic release does not spin while wait stays active");
  check(!dxmt9::render::openCbPendingCompletionWaitTransitionNeedsRecheck(
            /*completionWaitActive=*/true,
            /*waitObservedCompletionWaitActive=*/false,
            Mode::CompletionWait,
            /*canReleaseAtSemanticBoundary=*/false,
            /*semanticReleaseAlreadyUsedDuringWait=*/false),
        "ordinary pending prefixes do not wake on wait-start");
  check(dxmt9::render::openCbPendingCompletionWaitTransitionNeedsRecheck(
            /*completionWaitActive=*/false,
            /*waitObservedCompletionWaitActive=*/true,
            Mode::CompletionWait,
            /*canReleaseAtSemanticBoundary=*/true,
            /*semanticReleaseAlreadyUsedDuringWait=*/true),
        "wait-end wakes to reset the once-per-wait release gate");
  check(!dxmt9::render::openCbPendingCompletionWaitTransitionNeedsRecheck(
            /*completionWaitActive=*/false,
            /*waitObservedCompletionWaitActive=*/false,
            Mode::CompletionWait,
            /*canReleaseAtSemanticBoundary=*/true,
            /*semanticReleaseAlreadyUsedDuringWait=*/false),
        "idle non-wait state does not wake by itself");
  check(!dxmt9::render::openCbPendingCompletionWaitTransitionNeedsRecheck(
            /*completionWaitActive=*/true,
            /*waitObservedCompletionWaitActive=*/false,
            Mode::Deterministic,
            /*canReleaseAtSemanticBoundary=*/true,
            /*semanticReleaseAlreadyUsedDuringWait=*/false),
        "deterministic release does not need completion-wait transition wakeups");
}

void tailPresentBatchShapeAllowsSeveralHeads() {
  std::array<ChunkSlot, 3> slots{};
  std::array<ReadySlotSnapshot, 3> valid{
      makeClearSource(slots[0], /*slotIndex=*/1, /*seqId=*/5),
      makeClearSource(slots[1], /*slotIndex=*/2, /*seqId=*/6),
      makePresentOnlySource(slots[2], /*slotIndex=*/3, /*seqId=*/7),
  };
  check(dxmt9::render::canCoalesceTailPresentBatch(valid),
        "several non-present heads plus present-only tail are coalescable");

  std::array<ChunkSlot, 3> middleWithPresentSlots{};
  std::array<ReadySlotSnapshot, 3> middleWithPresent{
      makeClearSource(middleWithPresentSlots[0], /*slotIndex=*/1, /*seqId=*/5),
      makeClearSource(middleWithPresentSlots[1], /*slotIndex=*/2, /*seqId=*/6),
      makePresentOnlySource(middleWithPresentSlots[2], /*slotIndex=*/3, /*seqId=*/7),
  };
  middleWithPresentSlots[1].appendPresent({}, {});
  check(!dxmt9::render::canCoalesceTailPresentBatch(middleWithPresent),
        "any pre-tail source with present metadata is rejected");
}

void tailPresentPrefixSelectorRequiresCompleteTail() {
  std::array<ChunkSlot, 4> slots{};
  slots[0].seqId = 5;
  slots[0].appendClear({});
  slots[1].seqId = 6;
  slots[1].appendClear({});
  slots[2].seqId = 7;
  slots[2].appendClear({});
  slots[2].appendPresent({}, {});

  const std::deque<std::size_t> readySlots{0, 1, 2};
  checkEq(dxmt9::render::selectTailPresentBatchPrefix(
              readySlots,
              std::span<const ChunkSlot>(slots.data(), slots.size()),
              /*maxCount=*/3),
          3u,
          "selector accepts complete head/head/final-present-tail prefix");
  checkEq(dxmt9::render::selectTailPresentBatchPrefix(
              readySlots,
              std::span<const ChunkSlot>(slots.data(), slots.size()),
              /*maxCount=*/2),
          0u,
          "selector rejects when tail is outside scratch capacity");

  const std::deque<std::size_t> headOnly{0, 1};
  checkEq(dxmt9::render::selectTailPresentBatchPrefix(
              headOnly,
              std::span<const ChunkSlot>(slots.data(), slots.size()),
              /*maxCount=*/2),
          0u,
          "selector rejects head-only prefix");

  slots[1].appendPresent({}, {});
  checkEq(dxmt9::render::selectTailPresentBatchPrefix(
              readySlots,
              std::span<const ChunkSlot>(slots.data(), slots.size()),
              /*maxCount=*/3),
          2u,
          "selector ends the prefix at the first final Present");

  std::array<ChunkSlot, 3> postPresentTailSlots{};
  postPresentTailSlots[0].appendClear({});
  postPresentTailSlots[1].appendClear({});
  postPresentTailSlots[2].appendPresent({}, {});
  postPresentTailSlots[2].appendClear({});
  checkEq(dxmt9::render::selectTailPresentBatchPrefix(
              readySlots,
              std::span<const ChunkSlot>(
                  postPresentTailSlots.data(), postPresentTailSlots.size()),
              /*maxCount=*/3),
          0u,
          "selector rejects work after the final Present");
}

void openCbTailPresentPrefixAllowsSessionHeads() {
  std::array<ChunkSlot, 4> slots{};
  slots[0].seqId = 5;
  slots[0].publishReason = dxmt9::perf::ChunkPublishReason::PresentSplitBefore;
  slots[0].appendClear({});
  slots[1].seqId = 6;
  slots[1].publishReason = dxmt9::perf::ChunkPublishReason::PresentSplitBefore;
  slots[1].appendClear({});
  slots[2].seqId = 7;
  slots[2].appendClear({});
  slots[2].appendPresent({}, {});

  const std::deque<std::size_t> readySlots{0, 1, 2};
  checkEq(dxmt9::render::selectOpenCbTailPresentBatchPrefix(
              readySlots,
              std::span<const ChunkSlot>(slots.data(), slots.size()),
              /*maxCount=*/3),
          3u,
          "open-CB selector accepts open heads plus final-present tail");
  checkEq(dxmt9::render::selectOpenCbTailPresentBatchPrefix(
              readySlots,
              std::span<const ChunkSlot>(slots.data(), slots.size()),
              /*maxCount=*/2),
          0u,
          "open-CB selector rejects when tail is outside scratch capacity");

  const std::deque<std::size_t> headOnly{0, 1};
  checkEq(dxmt9::render::selectOpenCbTailPresentBatchPrefix(
              headOnly,
              std::span<const ChunkSlot>(slots.data(), slots.size()),
              /*maxCount=*/2),
          0u,
          "open-CB selector rejects a real head-only ready queue");

  std::array<ChunkSlot, 3> postPresentTailSlots{};
  postPresentTailSlots[0].publishReason =
      dxmt9::perf::ChunkPublishReason::PresentSplitBefore;
  postPresentTailSlots[0].appendClear({});
  postPresentTailSlots[1].publishReason =
      dxmt9::perf::ChunkPublishReason::PresentSplitBefore;
  postPresentTailSlots[1].appendClear({});
  postPresentTailSlots[2].appendPresent({}, {});
  postPresentTailSlots[2].appendClear({});
  checkEq(dxmt9::render::selectOpenCbTailPresentBatchPrefix(
              readySlots,
              std::span<const ChunkSlot>(
                  postPresentTailSlots.data(), postPresentTailSlots.size()),
              /*maxCount=*/3),
          0u,
          "open-CB selector rejects post-Present work in the tail");

  slots[0].publishReason = dxmt9::perf::ChunkPublishReason::DrawLimit;
  slots[1].publishReason = dxmt9::perf::ChunkPublishReason::DrawLimit;
  checkEq(dxmt9::render::selectOpenCbTailPresentBatchPrefix(
              readySlots,
              std::span<const ChunkSlot>(slots.data(), slots.size()),
              /*maxCount=*/3),
          3u,
          "carry-session selector accepts ordinary non-present heads");
}

void renderPassEntryDecisionContinuesOnlyOnSemanticCleanMatch() {
  check(dxmt9::encoders::classifyRenderPassEntry(
            /*hasActiveRender=*/true,
            /*attachmentKeyMatches=*/true,
            /*exactHazard=*/false,
            /*tileMidPassIneligible=*/false) ==
            RenderPassEntryDecision::ContinueActive,
        "same attachment with clean hazards continues active render encoder");
  check(dxmt9::encoders::classifyRenderPassEntry(
            /*hasActiveRender=*/false,
            /*attachmentKeyMatches=*/true,
            /*exactHazard=*/false,
            /*tileMidPassIneligible=*/false) ==
            RenderPassEntryDecision::BeginPass,
        "no active render encoder begins a pass");
  check(dxmt9::encoders::classifyRenderPassEntry(
            /*hasActiveRender=*/true,
            /*attachmentKeyMatches=*/false,
            /*exactHazard=*/false,
            /*tileMidPassIneligible=*/false) ==
            RenderPassEntryDecision::SplitRenderTargetChange,
        "attachment changes are semantic render-pass boundaries");
  check(dxmt9::encoders::classifyRenderPassEntry(
            /*hasActiveRender=*/true,
            /*attachmentKeyMatches=*/true,
            /*exactHazard=*/true,
            /*tileMidPassIneligible=*/false) ==
            RenderPassEntryDecision::SplitHazard,
        "exact active-render-target hazards split the render pass");
  check(dxmt9::encoders::classifyRenderPassEntry(
            /*hasActiveRender=*/true,
            /*attachmentKeyMatches=*/true,
            /*exactHazard=*/false,
            /*tileMidPassIneligible=*/true) ==
            RenderPassEntryDecision::SplitTileMidPassIneligible,
        "tile-FFP mid-pass ineligibility is a semantic encoder boundary");
}

void storeProofLookaheadIsSourceLocalOnlyOutsideEncodeSession() {
  check(dxmt9::encoders::useSourceLocalStoreProofLookahead(
            /*externalEncodeSession=*/false,
            /*sessionMayContinue=*/false),
        "single-source encode path may use source-local store proof lookahead");
  check(!dxmt9::encoders::useSourceLocalStoreProofLookahead(
            /*externalEncodeSession=*/true,
            /*sessionMayContinue=*/true),
        "carried EncodeSession path must not use source-local store proof lookahead");
  check(dxmt9::encoders::useSourceLocalStoreProofLookahead(
            /*externalEncodeSession=*/true,
            /*sessionMayContinue=*/false),
        "finalizing EncodeSession path may use current-source suffix lookahead");
}

void chunkSlotAppendCommandsFromRemapsPayloadsAndCommandIndices() {
  ChunkSlot destination;
  const std::array<dxmt9::core::u8, 1> existingVertex{{0x90}};
  appendTestDraw(destination, /*primitiveCount=*/1, makeUniformPayload(0x100),
                 existingVertex, {});
  const std::size_t baseCommandCount = destination.commandHeaders.size();
  const std::size_t basePayloadBytes = destination.drawPayloadArena.size();
  const std::size_t baseUniformCount = destination.drawUniformPayloads.size();

  ChunkSlot source;
  const std::array<dxmt9::core::u8, 3> firstVertex{{0x01, 0x02, 0x03}};
  const std::array<dxmt9::core::u8, 2> firstOverride{{0x0a, 0x0b}};
  appendTestDraw(source, /*primitiveCount=*/2, makeUniformPayload(0x200),
                 firstVertex, firstOverride);

  dxmt9::core::ClearDesc clear{};
  clear.clearColor = true;
  clear.color.r = 0.25f;
  source.appendClear(clear);

  const std::array<dxmt9::core::u8, 2> secondVertex{{0x04, 0x05}};
  appendTestDraw(source, /*primitiveCount=*/3, makeUniformPayload(0x300),
                 secondVertex, {});

  dxmt9::core::SwapDesc present{};
  present.width = 640;
  present.height = 480;
  source.appendPresent(present, dxmt9::core::Handle{0x55});

  check(destination.canAppendCommandsFrom(source),
        "destination accepts valid source command stream");
  check(destination.appendCommandsFrom(source),
        "appendCommandsFrom merges source command stream");
  checkEq(destination.commandHeaders.size(), baseCommandCount + 4u,
          "merged command stream preserves source command count");

  const auto firstDraw = destination.commandAt(baseCommandCount);
  check(firstDraw.drawRunRecord != nullptr, "first merged command is draw-run");
  checkEq(firstDraw.drawRunRecord->payloadOffset,
          static_cast<std::uint32_t>(basePayloadBytes),
          "first merged draw payload offset is rebased into destination arena");
  checkEq(firstDraw.drawRunRecord->uniformHandle.index,
          static_cast<std::uint32_t>(baseUniformCount),
          "first merged draw uniform handle is rebased");
  checkEq(firstDraw.drawParams.size(), 1u, "first merged draw has one param");
  checkEq(firstDraw.drawParams[0].primitiveCount, 2u,
          "first merged draw keeps primitive count");
  checkBytes(dxmt9::core::drawRunPayloadBytes(
                 firstDraw.drawParams[0].userVertexRange,
                 firstDraw.drawPayloadBytes),
             firstVertex,
             "first merged draw keeps user vertex payload bytes");
  checkBytes(dxmt9::core::drawRunPayloadBytes(
                 firstDraw.drawParams[0].bindingOverrideRange,
                 firstDraw.drawPayloadBytes),
             firstOverride,
             "first merged draw keeps binding override payload bytes");

  const auto mergedClear = destination.commandAt(baseCommandCount + 1u);
  check(mergedClear.clear != nullptr, "second merged command is clear");
  check(mergedClear.clear->clearColor, "merged clear keeps clearColor flag");
  checkEq(mergedClear.clear->color.r, 0.25f, "merged clear keeps color payload");

  const auto secondDraw = destination.commandAt(baseCommandCount + 2u);
  check(secondDraw.drawRunRecord != nullptr, "third merged command is draw-run");
  checkEq(secondDraw.drawParams[0].primitiveCount, 3u,
          "second merged draw keeps primitive count");
  checkBytes(dxmt9::core::drawRunPayloadBytes(
                 secondDraw.drawParams[0].userVertexRange,
                 secondDraw.drawPayloadBytes),
             secondVertex,
             "second merged draw keeps user vertex payload bytes");

  const auto mergedPresent = destination.commandAt(baseCommandCount + 3u);
  check(mergedPresent.present != nullptr, "fourth merged command is present");
  checkEq(mergedPresent.present->present.width, 640u,
          "merged present keeps width");
  checkEq(mergedPresent.present->present.height, 480u,
          "merged present keeps height");
  checkEq(mergedPresent.present->presentSource.value, 0x55ull,
          "merged present keeps source handle");
}

}  // namespace

int main() {
  try {
    emptyBatchCompletesInlineByDefault();
    encodeChunkOptionsDefaultToFreshCommandBufferPath();
    singleSourceBatchFallsBackToOnChunkReady();
    multiSourceBatchRequiresExplicitBackendImplementation();
    tailPresentBatchShapeRequiresFinalPresentTail();
    openCbPreencodeHeadRequiresPresentSplitBeforePublish();
    openCbPendingAppendPolicyAllowsFinalPresentTail();
    openCbPresentTailSplitRequiresCurrentPrePresentWork();
    openCbPendingTailWaitActionUsesQueueLocalState();
    openCbPendingMidChunkPolicyPreservesSemanticSplits();
    openCbSemanticBoundaryReleaseRequiresCompletionWait();
    openCbSemanticBoundaryReleaseCanPreemptReadySourceDuringWait();
    openCbSemanticBoundaryNoWaitBlockClassifiesWriterState();
    openCbActiveWaitAppendKeepsReadySourceInSession();
    openCbWaitStartPublishCreatesFirstPendingSource();
    openCbInitializerWaitBoundarySubmitsPendingBeforeAppend();
    openCbPendingWakeRecheckTracksCompletionWaitTransitions();
    tailPresentBatchShapeAllowsSeveralHeads();
    tailPresentPrefixSelectorRequiresCompleteTail();
    openCbTailPresentPrefixAllowsSessionHeads();
    renderPassEntryDecisionContinuesOnlyOnSemanticCleanMatch();
    storeProofLookaheadIsSourceLocalOnlyOutsideEncodeSession();
    chunkSlotAppendCommandsFromRemapsPayloadsAndCommandIndices();
  } catch (const TestFailure& error) {
    std::cerr << "render_backend_batch_contract_spec failed: "
              << error.what() << '\n';
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "render_backend_batch_contract_spec unexpected exception: "
              << error.what() << '\n';
    return 1;
  }
  return 0;
}
