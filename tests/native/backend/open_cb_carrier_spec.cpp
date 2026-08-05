// H229 open-CB overlap carrier (DXMT9_OPEN_CB_CARRIER) — deterministic
// coverage for the pure carrier decision helpers: slot classification
// (open/append/close), producer publication predicates, pending release
// predicates (semantic wait / producer wait / drain / fail-open shape),
// wait-transition rechecks, and the encode-side batch prefix selector.
// The queue-side retain/carry/completion machinery is covered by
// queue_completion_sources_spec.cpp; the env-off default path is pinned
// by the rest of the suite running without DXMT9_OPEN_CB_CARRIER.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "../../../src/dxmt9/render/open_cb_carrier.hpp"

namespace {

using dxmt9::core::ChunkSlot;
using dxmt9::core::FlatDrawStateRecord;
using dxmt9::core::Handle;
using dxmt9::core::metalqueue::ResolvedPublishedSource;
using dxmt9::render::OpenCbCarrierPendingWaitAction;

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

void slotClassificationCoversOpenAppendAndCloseShapes() {
  ChunkSlot empty{};
  check(!dxmt9::render::openCbCarrierSlotHasFinalPresentTail(empty),
        "empty slot is not a present tail");
  check(!dxmt9::render::openCbCarrierSlotCanBeSessionHead(empty),
        "empty slot cannot open a carrier session");
  check(!dxmt9::render::openCbCarrierSlotCanAppendToPending(empty, true),
        "empty slot cannot append to a pending carrier");

  ChunkSlot head{};
  head.appendClear({});
  check(!dxmt9::render::openCbCarrierSlotHasFinalPresentTail(head),
        "present-free slot is not a present tail");
  check(dxmt9::render::openCbCarrierSlotCanBeSessionHead(head),
        "non-empty present-free slot can open a carrier session");
  check(dxmt9::render::openCbCarrierSlotCanAppendToPending(head, true),
        "session head appends into an active pending session");
  check(!dxmt9::render::openCbCarrierSlotCanAppendToPending(head, false),
        "session head cannot append without an active pending session");

  ChunkSlot presentTail{};
  presentTail.appendPresent({}, Handle{0x11});
  check(dxmt9::render::openCbCarrierSlotHasFinalPresentTail(presentTail),
        "present-only slot is a final present tail");
  check(!dxmt9::render::openCbCarrierSlotCanBeSessionHead(presentTail),
        "present tail cannot open a carrier session");
  check(dxmt9::render::openCbCarrierSlotCanAppendToPending(presentTail, true),
        "present tail closes an active pending carrier");
  check(dxmt9::render::openCbCarrierSlotCanAppendToPending(presentTail, false),
        "present tail appends even without an active session");

  ChunkSlot midPresent{};
  midPresent.appendPresent({}, Handle{0x12});
  midPresent.appendClear({});
  check(!dxmt9::render::openCbCarrierSlotHasFinalPresentTail(midPresent),
        "present followed by more work is not a final present tail");
  check(!dxmt9::render::openCbCarrierSlotCanBeSessionHead(midPresent),
        "present-bearing slot cannot open a carrier session");
}

void presentTailSplitRequiresCarrierAndPrePresentWork() {
  check(dxmt9::render::openCbCarrierPresentTailNeedsPrePresentSplit(true, true),
        "carrier with pre-present work splits the present tail");
  check(!dxmt9::render::openCbCarrierPresentTailNeedsPrePresentSplit(true, false),
        "carrier without pre-present work keeps the fused present chunk");
  check(!dxmt9::render::openCbCarrierPresentTailNeedsPrePresentSplit(false, true),
        "env-off default never splits the present tail");
  check(!dxmt9::render::openCbCarrierPresentTailNeedsPrePresentSplit(false, false),
        "env-off default with empty slot never splits");
}

void drawAttachmentKeysCompareHandlesAndSampleCounts() {
  FlatDrawStateRecord a{};
  FlatDrawStateRecord b{};
  a.colorAttachments[0].handle = Handle{0x10};
  b.colorAttachments[0].handle = Handle{0x10};
  a.depthStencil.handle = Handle{0x20};
  b.depthStencil.handle = Handle{0x20};
  check(dxmt9::render::openCbCarrierDrawAttachmentKeysMatch(a, b),
        "identical attachment keys match");

  b.colorAttachments[0].handle = Handle{0x11};
  check(!dxmt9::render::openCbCarrierDrawAttachmentKeysMatch(a, b),
        "changed color attachment breaks the key");
  b.colorAttachments[0].handle = Handle{0x10};

  b.depthStencil.handle = Handle{0x21};
  check(!dxmt9::render::openCbCarrierDrawAttachmentKeysMatch(a, b),
        "changed depth attachment breaks the key");
  b.depthStencil.handle = Handle{0x20};

  b.colorAttachments[1].sampleCount = 4;
  check(!dxmt9::render::openCbCarrierDrawAttachmentKeysMatch(a, b),
        "changed max sample count breaks the key");
}

void waitStartPublishRequiresIdleCarrierDuringCompletionWait() {
  check(dxmt9::render::openCbCarrierShouldPublishWaitStartSlot(
            true, false, true, false, true, false, false),
        "idle carrier during completion wait publishes the writing slot");
  check(!dxmt9::render::openCbCarrierShouldPublishWaitStartSlot(
            false, false, true, false, true, false, false),
        "visible ready source suppresses wait-start publish");
  check(!dxmt9::render::openCbCarrierShouldPublishWaitStartSlot(
            true, true, true, false, true, false, false),
        "pending open CB suppresses wait-start publish");
  check(!dxmt9::render::openCbCarrierShouldPublishWaitStartSlot(
            true, false, false, false, true, false, false),
        "no completion wait suppresses wait-start publish");
  check(!dxmt9::render::openCbCarrierShouldPublishWaitStartSlot(
            true, false, true, true, true, false, false),
        "stop request suppresses wait-start publish");
  check(!dxmt9::render::openCbCarrierShouldPublishWaitStartSlot(
            true, false, true, false, false, false, false),
        "inactive writer suppresses wait-start publish");
  check(!dxmt9::render::openCbCarrierShouldPublishWaitStartSlot(
            true, false, true, false, true, true, false),
        "empty writing slot has nothing to publish");
  check(!dxmt9::render::openCbCarrierShouldPublishWaitStartSlot(
            true, false, true, false, true, false, true),
        "present-bearing writing slot is the tail, not a wait-start publish");
}

void activeWaitPublishRequiresReleasablePendingCarrier() {
  check(dxmt9::render::openCbCarrierShouldPublishActiveWaitSlot(
            true, true, true, false, true, false, false),
        "releasable pending carrier during wait publishes the writing slot");
  check(!dxmt9::render::openCbCarrierShouldPublishActiveWaitSlot(
            false, true, true, false, true, false, false),
        "ready source suppresses active-wait publish");
  check(!dxmt9::render::openCbCarrierShouldPublishActiveWaitSlot(
            true, false, true, false, true, false, false),
        "non-releasable pending suppresses active-wait publish");
  check(!dxmt9::render::openCbCarrierShouldPublishActiveWaitSlot(
            true, true, false, false, true, false, false),
        "inactive completion wait suppresses active-wait publish");
  check(!dxmt9::render::openCbCarrierShouldPublishActiveWaitSlot(
            true, true, true, true, true, false, false),
        "used semantic release latch suppresses active-wait publish");
  check(!dxmt9::render::openCbCarrierShouldPublishActiveWaitSlot(
            true, true, true, false, true, true, false),
        "empty writing slot suppresses active-wait publish");
  check(!dxmt9::render::openCbCarrierShouldPublishActiveWaitSlot(
            true, true, true, false, true, false, true),
        "present-bearing writing slot suppresses active-wait publish");
}

void semanticReleaseFiresOncePerCompletionWait() {
  check(dxmt9::render::openCbCarrierCanReleasePendingAtSemanticBoundary(
            true, true, false),
        "releasable pending releases during an active completion wait");
  check(!dxmt9::render::openCbCarrierCanReleasePendingAtSemanticBoundary(
            true, true, true),
        "second release in the same wait is latched off");
  check(!dxmt9::render::openCbCarrierCanReleasePendingAtSemanticBoundary(
            true, false, false),
        "no completion wait means no semantic release");
  check(!dxmt9::render::openCbCarrierCanReleasePendingAtSemanticBoundary(
            false, true, false),
        "non-semantic pending head never releases at semantic boundary");
}

void appendablePendingPrefersAppendOverRelease() {
  check(dxmt9::render::openCbCarrierShouldAppendReadyBeforeRelease(
            true, true, false, true),
        "appendable ready source is appended instead of releasing pending");
  check(!dxmt9::render::openCbCarrierShouldAppendReadyBeforeRelease(
            true, true, false, false),
        "non-appendable ready source does not block the release");
  check(!dxmt9::render::openCbCarrierShouldAppendReadyBeforeRelease(
            true, true, true, true),
        "used release latch removes the append-before-release preference");
  check(!dxmt9::render::openCbCarrierShouldAppendReadyBeforeRelease(
            true, false, false, true),
        "no completion wait removes the append-before-release preference");
}

void producerWaitAlwaysReleasesPendingWork() {
  check(dxmt9::render::openCbCarrierShouldSubmitForProducerWait(true, true),
        "pending work releases when the producer blocks on a sequence wait");
  check(!dxmt9::render::openCbCarrierShouldSubmitForProducerWait(true, false),
        "no producer wait keeps the pending carrier open");
  check(!dxmt9::render::openCbCarrierShouldSubmitForProducerWait(false, true),
        "producer wait with no pending work is a no-op");
}

void initializerWaitReleasesActiveRenderSessions() {
  check(dxmt9::render::openCbCarrierShouldSubmitBeforeInitializerWait(
            true, true, true),
        "active render session releases before the initializer wait");
  check(!dxmt9::render::openCbCarrierShouldSubmitBeforeInitializerWait(
            true, false, true),
        "no active render in the session keeps the carrier open");
  check(!dxmt9::render::openCbCarrierShouldSubmitBeforeInitializerWait(
            true, true, false),
        "no pending initializer uploads keeps the carrier open");
  check(!dxmt9::render::openCbCarrierShouldSubmitBeforeInitializerWait(
            false, true, true),
        "non-appendable source path handles its own release");
}

void waitTransitionRecheckCoversLatchAndWaitEnd() {
  check(dxmt9::render::openCbCarrierWaitTransitionNeedsRecheck(
            true, false, true, false),
        "wait became active with unreleased releasable pending: recheck");
  check(dxmt9::render::openCbCarrierWaitTransitionNeedsRecheck(
            false, true, false, false),
        "observed wait ended: recheck to reset the release latch");
  check(!dxmt9::render::openCbCarrierWaitTransitionNeedsRecheck(
            true, true, true, true),
        "latched release during the same wait does not spin the recheck");
  check(!dxmt9::render::openCbCarrierWaitTransitionNeedsRecheck(
            false, false, true, false),
        "no wait transition and no wait: no recheck");
}

void pendingWaitActionSubmitsOnStopOrInactiveWriter() {
  checkEq(dxmt9::render::selectOpenCbCarrierPendingWaitAction(
              true, true, false, true),
          OpenCbCarrierPendingWaitAction::WaitForReady,
          "pending carrier with active writer waits for more work");
  checkEq(dxmt9::render::selectOpenCbCarrierPendingWaitAction(
              true, true, true, true),
          OpenCbCarrierPendingWaitAction::SubmitPending,
          "stop request drains the pending carrier (fail-open)");
  checkEq(dxmt9::render::selectOpenCbCarrierPendingWaitAction(
              true, true, false, false),
          OpenCbCarrierPendingWaitAction::SubmitPending,
          "inactive writer drains the pending carrier (fail-open)");
  checkEq(dxmt9::render::selectOpenCbCarrierPendingWaitAction(
              true, false, false, true),
          OpenCbCarrierPendingWaitAction::None,
          "visible ready source proceeds to dequeue instead of waiting");
  checkEq(dxmt9::render::selectOpenCbCarrierPendingWaitAction(
              false, true, true, true),
          OpenCbCarrierPendingWaitAction::None,
          "no pending carrier never selects a pending wait action");
}

void arenaStandaloneSubmissionRetainsTapeCompletionIdentity() {
  using namespace dxmt9::core;
  using namespace dxmt9::core::metalqueue;

  const ReadySlotSnapshot source{
      .slotIndex = 7,
      .seqId = 19,
      .hasPresent = false,
      .commandBegin = 0,
      .commandCount = 2,
      .sourceId = CpuReadySourceId{.index = 3, .generation = 11},
      .storage = CpuReadyStorageRef{
          .firstPage = 5,
          .pageCount = 2,
          .generation = 13,
      },
  };
  QueueSubmissionRecord record{};
  check(assignOrValidateSingleCompletionSource(
            record, source),
        "standalone arena submission must retain its Tape locator");
  checkEq(record.fixedCompletionSources.size(), std::size_t{1},
          "standalone arena submission retains exactly one source");
  const auto& retained = record.fixedCompletionSources.entries[0];
  check(retained.source.id == source.sourceId &&
            retained.source.storage == source.storage &&
            retained.slotIndex == source.slotIndex &&
            retained.seqId == source.seqId &&
            retained.commandCount == source.commandCount,
        "retained completion source preserves arena identity and range");

  check(assignOrValidateSingleCompletionSource(
            record, source),
        "already-retained standalone identity remains valid");
  checkEq(record.fixedCompletionSources.size(), std::size_t{1},
          "idempotent retention must not duplicate completion sources");

  QueueSubmissionRecord wrongIdentity{};
  auto wrong = completionSourceForReadySlot(source);
  ++wrong.seqId;
  const std::array wrongSources{wrong};
  check(wrongIdentity.assignFixedCompletionSources(wrongSources) &&
            !assignOrValidateSingleCompletionSource(wrongIdentity, source),
        "nonempty standalone record must reject a mismatched locator");

  QueueSubmissionRecord multiple{};
  auto following = completionSourceForReadySlot(source);
  ++following.seqId;
  following.source.id.index += 1u;
  following.source.storage.firstPage += following.source.storage.pageCount;
  const std::array multipleSources{
      completionSourceForReadySlot(source), following};
  check(multiple.assignFixedCompletionSources(multipleSources) &&
            !assignOrValidateSingleCompletionSource(multiple, source),
        "standalone record must reject multiple completion sources");
}

void batchPrefixSelectsHeadsUpToPresentTail() {
  auto resolve = [](ChunkSlot& slot) {
    return ResolvedPublishedSource{
        .payload = dxmt9::core::SourcePayloadView(slot),
        .slot = &slot,
        .commandCount = slot.commandCount(),
    };
  };
  std::array<ChunkSlot, 4> slots{};
  slots[0].appendClear({});
  slots[1].appendClear({});
  slots[2].appendPresent({}, Handle{0x31});
  slots[3].appendClear({});

  // head, head, tail, head -> select head..tail (3 sources).
  std::array<ResolvedPublishedSource, 4> ready{{
      resolve(slots[0]),
      resolve(slots[1]),
      resolve(slots[2]),
      resolve(slots[3]),
  }};
  checkEq(dxmt9::render::selectOpenCbCarrierBatchPrefix(
              ready),
          std::size_t{3},
          "prefix runs through appendable heads and closes at the tail");

  // Tail first -> 0 (fall back to single-source dequeue of the tail).
  std::array<ResolvedPublishedSource, 2> tailFirst{{
      resolve(slots[2]),
      resolve(slots[0]),
  }};
  checkEq(dxmt9::render::selectOpenCbCarrierBatchPrefix(
              tailFirst),
          std::size_t{0},
          "leading present tail falls back to single-source dequeue");

  // Heads only, no tail visible -> select every head.
  std::array<ResolvedPublishedSource, 3> headsOnly{{
      resolve(slots[0]),
      resolve(slots[1]),
      resolve(slots[3]),
  }};
  checkEq(dxmt9::render::selectOpenCbCarrierBatchPrefix(
              headsOnly),
          std::size_t{3},
          "session heads without a visible tail are all selected");

  // Capacity clamps before the tail is reached.
  checkEq(dxmt9::render::selectOpenCbCarrierBatchPrefix(
              std::span<const ResolvedPublishedSource>(ready.data(), 2)),
          std::size_t{2},
          "caller capacity clamps the selected prefix");

  // Non-head (empty) source in the prefix rejects the batch.
  std::array<ChunkSlot, 2> withEmpty{};
  withEmpty[0].appendClear({});
  std::array<ResolvedPublishedSource, 2> emptySecond{{
      resolve(withEmpty[0]),
      resolve(withEmpty[1]),
  }};
  checkEq(dxmt9::render::selectOpenCbCarrierBatchPrefix(
              emptySecond),
          std::size_t{0},
          "non-appendable source in the prefix falls back to single dequeue");

  // Candidate spans carry already-resolved payload views; null rejects before
  // the selector can inspect any external slot table.
  std::array<ResolvedPublishedSource, 1> unresolved{};
  checkEq(dxmt9::render::selectOpenCbCarrierBatchPrefix(
              unresolved),
          std::size_t{0},
          "unresolved candidate falls back to single dequeue");

  checkEq(dxmt9::render::selectOpenCbCarrierBatchPrefix(
              std::span<const ResolvedPublishedSource>{}),
          std::size_t{0}, "empty ready queue selects nothing");
}

}  // namespace

int main() {
  try {
    slotClassificationCoversOpenAppendAndCloseShapes();
    presentTailSplitRequiresCarrierAndPrePresentWork();
    drawAttachmentKeysCompareHandlesAndSampleCounts();
    waitStartPublishRequiresIdleCarrierDuringCompletionWait();
    activeWaitPublishRequiresReleasablePendingCarrier();
    semanticReleaseFiresOncePerCompletionWait();
    appendablePendingPrefersAppendOverRelease();
    producerWaitAlwaysReleasesPendingWork();
    initializerWaitReleasesActiveRenderSessions();
    waitTransitionRecheckCoversLatchAndWaitEnd();
    pendingWaitActionSubmitsOnStopOrInactiveWriter();
    arenaStandaloneSubmissionRetainsTapeCompletionIdentity();
    batchPrefixSelectsHeadsUpToPresentTail();
  } catch (const TestFailure& error) {
    std::cerr << "open_cb_carrier_spec failed: " << error.what() << '\n';
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "open_cb_carrier_spec unexpected error: " << error.what()
              << '\n';
    return 1;
  }
  std::cout << "open_cb_carrier_spec passed\n";
  return 0;
}
