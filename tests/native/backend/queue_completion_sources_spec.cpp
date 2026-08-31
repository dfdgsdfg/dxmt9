#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include "../../../src/dxmt9/dxmt9_draw_encoder.hpp"
#include "../../../src/dxmt9/dxmt9_queue.hpp"
#include "../../../src/dxmt9/dxmt9_scheduling_progress_watchdog.hpp"

namespace {

using dxmt9::core::metalqueue::QueueCompletionSource;
using dxmt9::core::metalqueue::ArenaGroupSettlementLedger;
using dxmt9::core::metalqueue::QueueLifecycleController;
using dxmt9::core::metalqueue::QueueSubmissionRecord;
using dxmt9::core::metalqueue::ReadySlotSnapshot;
using dxmt9::core::metalqueue::ResolvedPublishedSource;
using dxmt9::core::metalqueue::GenerationQualifiedSourceBorrow;
using dxmt9::core::metalqueue::SynchronousSourceBorrowBatch;
using dxmt9::core::metalqueue::SynchronousSourcePayloadBorrow;
using dxmt9::core::metalqueue::WorkerOwnedSourceSnapshot;
using dxmt9::core::metalqueue::EncodeSessionSourceList;
using dxmt9::core::metalqueue::kMaxEncodeSessionSources;
using dxmt9::core::metalqueue::appendCompletionSourcesToQueues;
using dxmt9::core::metalqueue::completionSourceForReadySlot;
using dxmt9::queue::PipelineOwner;
using dxmt9::core::metalqueue::committedSequenceWaitTarget;
using dxmt9::core::metalqueue::foldEncodedSessionFragmentCarrier;
using dxmt9::core::metalqueue::hasExactRedundantFixedCompletionSources;
using dxmt9::core::metalqueue::mergeEncodedPendingTailSubmission;
using dxmt9::core::metalqueue::mergeCommandBufferDiagnostics;
using dxmt9::core::metalqueue::summarizeNoEnqueueFirstPublishSlotShape;
using dxmt9::core::ChunkSlot;
using dxmt9::core::ChunkSlotControl;
using dxmt9::core::ArenaSourcePayloadBuilder;
using dxmt9::core::CpuReadySourceId;
using dxmt9::core::CpuReadyStorageRef;
using dxmt9::core::CpuReadyAdmissionIdentity;
using dxmt9::core::CpuReadyTape;
using dxmt9::core::CpuReadyTapeConfig;
using dxmt9::core::SourcePayloadCapacity;
using dxmt9::core::SourcePayloadLayout;
using dxmt9::core::makeArenaSourcePayloadLayout;
using dxmt9::core::makeSourcePayloadLayout;

constexpr CpuReadyTape::SourceRef testSource(std::size_t slotIndex,
                                             std::uint64_t seqId) {
  const std::uint64_t generation = seqId == 0 ? 1 : seqId;
  return {
      .id = {
          .index = static_cast<std::uint32_t>(slotIndex),
          .generation = generation,
      },
      .storage = {
          .firstPage = static_cast<std::uint32_t>(slotIndex),
          .pageCount = 1,
          .generation = generation,
      },
  };
}

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

void dceChunkLookaheadProgressPolicyIsFailOpen() {
  using dxmt9::DceChunkLookaheadAction;
  using dxmt9::DceChunkLookaheadSourceAction;
  using dxmt9::resolveDceChunkLookaheadAction;
  using dxmt9::resolveDceChunkLookaheadSourceAction;

  checkEq(resolveDceChunkLookaheadAction(/*hasReady=*/true),
          DceChunkLookaheadAction::UseReady,
          "a ready FIFO successor is selected opportunistically");
  checkEq(resolveDceChunkLookaheadAction(/*hasReady=*/false),
          DceChunkLookaheadAction::FailOpen,
          "no ready successor immediately releases the held source");

  checkEq(resolveDceChunkLookaheadSourceAction(
              /*currentValid=*/true, /*currentHasLegacySlot=*/true,
              /*hasNext=*/true, /*nextValid=*/true,
              /*nextHasLegacySlot=*/false),
          DceChunkLookaheadSourceAction::EncodeCurrentHoldNext,
          "legacy current with an arena successor encodes fail-open and "
          "holds the arena as the next current");
  checkEq(resolveDceChunkLookaheadSourceAction(
              /*currentValid=*/true, /*currentHasLegacySlot=*/true,
              /*hasNext=*/true, /*nextValid=*/true,
              /*nextHasLegacySlot=*/true),
          DceChunkLookaheadSourceAction::EncodeCurrentExposeLegacyLookahead,
          "legacy successors remain eligible for the semantic proof view");
  checkEq(resolveDceChunkLookaheadSourceAction(
              /*currentValid=*/true, /*currentHasLegacySlot=*/true,
              /*hasNext=*/true, /*nextValid=*/false,
              /*nextHasLegacySlot=*/false),
          DceChunkLookaheadSourceAction::Poison,
          "an invalid represented successor remains a Tape failure");
}

template <typename T>
std::span<const T> asSpan(const std::vector<T>& values) {
  return std::span<const T>(values.data(), values.size());
}

CpuReadyTapeConfig makeSeparatedPayloadConfig(
    std::size_t pageCount = 8,
    std::size_t sourceCount = 8,
    std::size_t compatibilityCount = 1) {
  const auto config = CpuReadyTapeConfig::create({
      .pageSize = 4096,
      .pageCount = pageCount,
      .sourceSlotCount = sourceCount,
      .readyFifoCount = sourceCount,
      .compatibilityPayloadCount = compatibilityCount,
      .maxPagesPerSource = pageCount,
      .highWaterSources = sourceCount,
      .lowWaterSources = 0,
      .highWaterPages = pageCount,
      .lowWaterPages = 0,
      .highWaterReady = sourceCount,
      .lowWaterReady = 0,
  });
  check(config.has_value(), "separated payload configuration validates");
  return *config;
}

SourcePayloadLayout makeMinimalArenaLayout() {
  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 1;
  capacity.surfaceCopyRecords = 1;
  const auto layout = makeSourcePayloadLayout(capacity, 4096, 8);
  check(layout.has_value(), "minimal arena layout validates");
  return *layout;
}

void publishMinimalArena(CpuReadyTape& tape,
                         const CpuReadyTape::Reservation& reservation,
                         const SourcePayloadLayout& layout) {
  auto memory = tape.writableStorage(reservation.ticket);
  check(reservation.payloadKind == CpuReadyTape::PayloadKind::Arena &&
            reservation.arenaPayload != nullptr &&
            memory.size() >= layout.usedBytes,
        "strict reservation owns writable arena storage");
  ArenaSourcePayloadBuilder builder(
      *reservation.arenaPayload, layout, memory.first(layout.usedBytes));
  check(builder.tryAppendSurfaceCopyCommand({}) && builder.publish(),
        "minimal arena payload publishes before Tape seal");
}

void mapWaitTargetNeverExceedsCommittedWaterline() {
  checkEq(committedSequenceWaitTarget(8, 6), 6ull,
          "future resource mark clamps to committed waterline");
  checkEq(committedSequenceWaitTarget(4, 6), 4ull,
          "committed resource mark keeps its requested sequence");
  checkEq(committedSequenceWaitTarget(0, 6), 0ull,
          "no resource dependency remains no wait");
}

void undrainedSettlementLedgerFailsClosedAtCapacity() {
  ArenaGroupSettlementLedger ledger;
  for (std::uint64_t tail = 1; tail <= ArenaGroupSettlementLedger::kCapacity;
       ++tail) {
    check(ledger.append(CpuReadyTape::ArenaGroupSettlement{
              .rawOrdinal = tail,
              .buildGeneration = tail,
              .firstSourceOrdinal = tail,
              .tailSeqId = tail,
              .sourceCount = 1,
              .hasPresent = false,
          }),
          "an undrained ledger accepts only its fixed capacity");
  }
  check(!ledger.append(CpuReadyTape::ArenaGroupSettlement{
            .rawOrdinal = ArenaGroupSettlementLedger::kCapacity + 1u,
            .buildGeneration = ArenaGroupSettlementLedger::kCapacity + 1u,
            .firstSourceOrdinal = ArenaGroupSettlementLedger::kCapacity + 1u,
            .tailSeqId = ArenaGroupSettlementLedger::kCapacity + 1u,
            .sourceCount = 1,
            .hasPresent = false,
        }),
        "an undrained ledger rejects overflow instead of growing");
}

void appendsSingleLegacySource() {
  std::deque<std::uint64_t> completed;
  std::deque<std::uint64_t> presentCompleted;
  const std::vector<QueueCompletionSource> sources = {{
      .slotIndex = 0,
      .seqId = 1,
      .hasPresent = false,
  }};

  appendCompletionSourcesToQueues(completed, &presentCompleted, 0, asSpan(sources));

  checkEq(completed.size(), 1u, "single source appends one completed seq");
  checkEq(completed.front(), 1ull, "single source appends seq 1");
  check(presentCompleted.empty(), "non-present source skips present queue");
}

void appendsMultiSourceBatchInStrictSeqOrder() {
  std::deque<std::uint64_t> completed;
  std::deque<std::uint64_t> presentCompleted;
  const std::vector<QueueCompletionSource> sources = {
      {
          .slotIndex = 3,
          .seqId = 5,
          .hasPresent = false,
      },
      {
          .slotIndex = 4,
          .seqId = 6,
          .hasPresent = true,
      },
      {
          .slotIndex = 5,
          .seqId = 7,
          .hasPresent = false,
      },
  };

  appendCompletionSourcesToQueues(completed, &presentCompleted, 4, asSpan(sources));

  checkEq(completed.size(), 3u, "multi source appends every seq");
  checkEq(completed[0], 5ull, "multi source seq 5");
  checkEq(completed[1], 6ull, "multi source seq 6");
  checkEq(completed[2], 7ull, "multi source seq 7");
  checkEq(presentCompleted.size(), 1u, "only present source enters present queue");
  checkEq(presentCompleted.front(), 6ull, "present queue records source seq");
}

void respectsAlreadyQueuedCompletions() {
  std::deque<std::uint64_t> completed;
  std::deque<std::uint64_t> presentCompleted;
  completed.push_back(11);
  const std::vector<QueueCompletionSource> sources = {{
      .slotIndex = 2,
      .seqId = 12,
      .hasPresent = true,
  }};

  appendCompletionSourcesToQueues(completed, &presentCompleted, 10, asSpan(sources));

  checkEq(completed.size(), 2u, "existing completed entries are retained");
  checkEq(completed[0], 11ull, "existing seq remains first");
  checkEq(completed[1], 12ull, "new seq follows queued seq");
  checkEq(presentCompleted.size(), 1u, "present seq appends with existing completed queue");
  checkEq(presentCompleted.front(), 12ull, "present seq matches appended source");
}

void presentQueueMayBeAbsent() {
  std::deque<std::uint64_t> completed;
  const std::vector<QueueCompletionSource> sources = {{
      .slotIndex = 1,
      .seqId = 3,
      .hasPresent = true,
  }};

  appendCompletionSourcesToQueues(completed, nullptr, 2, asSpan(sources));

  checkEq(completed.size(), 1u, "completed queue appends without present queue");
  checkEq(completed.front(), 3ull, "completed seq is preserved without present queue");
}

void encodeSessionSourceListStoresConsecutiveSources() {
  EncodeSessionSourceList list;

  check(list.append(QueueCompletionSource{
            .source = testSource(3, 7),
            .slotIndex = 3,
            .seqId = 7,
            .hasPresent = false,
            .commandCount = 11,
        }),
        "first session source appends");
  check(list.append(QueueCompletionSource{
            .source = testSource(4, 8),
            .slotIndex = 4,
            .seqId = 8,
            .hasPresent = true,
            .commandCount = 1,
        }),
        "present tail session source appends");

  checkEq(list.size(), 2u, "session source list tracks source count");
  const auto span = list.span();
  checkEq(span[0].slotIndex, 3u, "session source list preserves head slot");
  checkEq(span[0].seqId, 7ull, "session source list preserves head seq");
  checkEq(span[1].slotIndex, 4u, "session source list preserves tail slot");
  checkEq(span[1].seqId, 8ull, "session source list preserves tail seq");
  check(span[1].hasPresent, "session source list preserves tail present flag");
  checkEq(span[0].commandBegin, 0u,
          "session source list preserves head command begin");
  checkEq(span[1].commandBegin, 0u,
          "session source list preserves tail command begin");
  checkEq(span[0].commandCount, 11u,
          "session source list preserves head command count");
  checkEq(span[1].commandCount, 1u,
          "session source list preserves tail command count");

  list.clear();
  check(list.empty(), "session source list clear resets count");
  checkEq(list.entries[0].seqId, 0ull,
          "session source list clear scrubs stale head seq metadata");
  check(!list.entries[1].hasPresent,
        "session source list clear scrubs stale tail present metadata");
  checkEq(list.entries[0].commandCount, 0u,
          "session source list clear scrubs stale command-count metadata");
  checkEq(list.entries[0].commandBegin, 0u,
          "session source list clear scrubs stale command-begin metadata");
}

void carriedSessionRetainsPerSourceEncodeOwners() {
  EncodeSessionSourceList list;
  auto serial = QueueCompletionSource{
      .source = testSource(1u, 1u), .slotIndex = 1u, .seqId = 1u};
  auto parallel = QueueCompletionSource{
      .source = testSource(2u, 2u), .slotIndex = 2u, .seqId = 2u};
  check(list.append(serial), "mixed session accepts serial source");
  check(list.append(parallel), "mixed session accepts selected source");
  check(list.setOwner(1u, PipelineOwner::SelectedParallel),
        "selected owner joins after serial fragment");
  check(list.setOwner(1u, PipelineOwner::SerialEncode),
        "serial fragment cannot downgrade a selected owner");
  check(list.setOwner(2u, PipelineOwner::SelectedParallel),
        "selected owner is attached to its source");
  check(list.entries[0].pipelineOwner == PipelineOwner::SelectedParallel &&
            list.entries[1].pipelineOwner == PipelineOwner::SelectedParallel,
        "carried session preserves per-source selected owners");

  auto replacement = parallel;
  replacement.source = testSource(3u, 2u);
  replacement.receipt = {};
  check(list.replaceIdentity(parallel, replacement),
        "receipt/source replacement succeeds for selected source");
  check(list.entries[1].pipelineOwner == PipelineOwner::SelectedParallel,
        "identity replacement retains exact source owner");

  EncodeSessionSourceList reverse;
  auto reverseSource = QueueCompletionSource{
      .source = testSource(4u, 4u), .slotIndex = 4u, .seqId = 4u};
  check(reverse.append(reverseSource), "reverse-order source is retained");
  check(reverse.setOwner(4u, PipelineOwner::SelectedParallel),
        "parallel owner is accepted first");
  check(reverse.setOwner(4u, PipelineOwner::SerialEncode),
        "serial owner joins after parallel without downgrade");
  check(reverse.setOwner(4u, PipelineOwner::SelectedParallel),
        "repeated parallel owner update is idempotent");
  check(reverse.entries[0].pipelineOwner == PipelineOwner::SelectedParallel,
        "owner join is commutative and idempotent");
}

void encodeSessionSourceListRejectsInvalidShape() {
  EncodeSessionSourceList list;

  check(!list.canAppend(QueueCompletionSource{
            .source = testSource(1, 0),
            .slotIndex = 1,
            .seqId = 0,
            .hasPresent = false,
        }),
        "session source list preflight rejects zero seqId");
  check(!list.append(QueueCompletionSource{
            .source = testSource(1, 0),
            .slotIndex = 1,
            .seqId = 0,
            .hasPresent = false,
        }),
        "session source list rejects zero seqId");
  check(list.canAppend(QueueCompletionSource{
            .source = testSource(1, 1),
            .slotIndex = 1,
            .seqId = 1,
            .hasPresent = false,
        }),
        "session source list preflight accepts initial valid seqId");
  check(list.append(QueueCompletionSource{
            .source = testSource(1, 1),
            .slotIndex = 1,
            .seqId = 1,
            .hasPresent = false,
        }),
        "session source list accepts initial valid seqId");
  check(!list.canAppend(QueueCompletionSource{
            .source = testSource(2, 3),
            .slotIndex = 3,
            .seqId = 3,
            .hasPresent = false,
        }),
        "session source list preflight rejects seqId gaps");
  check(!list.append(QueueCompletionSource{
            .source = testSource(2, 3),
            .slotIndex = 3,
            .seqId = 3,
            .hasPresent = false,
        }),
        "session source list rejects seqId gaps");

  EncodeSessionSourceList tailList;
  check(tailList.append(QueueCompletionSource{
            .source = testSource(4, 4),
            .slotIndex = 1,
            .seqId = 1,
            .hasPresent = true,
        }),
        "session source list accepts present tail");
  check(!tailList.append(QueueCompletionSource{
            .source = testSource(5, 5),
            .slotIndex = 2,
            .seqId = 2,
            .hasPresent = false,
        }),
        "session source list rejects appending after present tail");

  EncodeSessionSourceList full;
  for (std::size_t i = 0; i < kMaxEncodeSessionSources; ++i) {
    check(full.append(QueueCompletionSource{
              .source = testSource(i, i + 1u),
              .slotIndex = i,
              .seqId = static_cast<std::uint64_t>(i + 1u),
              .hasPresent = false,
          }),
          "session source list fills bounded capacity");
  }
  check(!full.canAppend(QueueCompletionSource{
            .source = testSource(kMaxEncodeSessionSources,
                                 kMaxEncodeSessionSources + 1u),
            .slotIndex = kMaxEncodeSessionSources,
            .seqId = static_cast<std::uint64_t>(kMaxEncodeSessionSources + 1u),
            .hasPresent = false,
        }),
        "session source list preflight rejects overflow");
  check(!full.append(QueueCompletionSource{
            .source = testSource(kMaxEncodeSessionSources,
                                 kMaxEncodeSessionSources + 1u),
            .slotIndex = kMaxEncodeSessionSources,
            .seqId = static_cast<std::uint64_t>(kMaxEncodeSessionSources + 1u),
            .hasPresent = false,
        }),
        "session source list rejects overflow");
}

void encodeSessionSourceListAssignIsTransactional() {
  const std::array<QueueCompletionSource, 2> initial{{
      {
          .source = testSource(1, 1),
          .slotIndex = 1,
          .seqId = 1,
          .hasPresent = false,
          .commandCount = 4,
      },
      {
          .source = testSource(2, 2),
          .slotIndex = 2,
          .seqId = 2,
          .hasPresent = false,
          .commandCount = 5,
      },
  }};
  EncodeSessionSourceList list;
  check(list.assign(std::span<const QueueCompletionSource>(
            initial.data(), initial.size())),
        "session source list assigns valid source span");

  const std::array<QueueCompletionSource, 2> invalid{{
      {
          .source = testSource(8, 8),
          .slotIndex = 8,
          .seqId = 8,
          .hasPresent = false,
      },
      {
          .source = testSource(10, 10),
          .slotIndex = 10,
          .seqId = 10,
          .hasPresent = false,
      },
  }};
  check(!list.assign(std::span<const QueueCompletionSource>(
            invalid.data(), invalid.size())),
        "session source list rejects invalid assign span");
  checkEq(list.size(), 2u, "failed assign preserves previous source count");
  checkEq(list.span()[0].seqId, 1ull, "failed assign preserves previous head");
  checkEq(list.span()[1].seqId, 2ull, "failed assign preserves previous tail");
  checkEq(list.span()[0].commandCount, 4u,
          "failed assign preserves previous source metadata");
}

void diagnosticsMergeKeepsTailIdentityAndAggregatesSourceShape() {
  dxmt9::core::metalqueue::CommandBufferDiagnostics aggregate{
      .seqId = 9,
      .slotIndex = 3,
      .hasDraw = true,
      .compatFlags = 0x01,
      .vertexShaderHash = 0x10,
      .pixelShaderHash = 0x20,
      .shaderVariantHash = 0x30,
  };
  const dxmt9::core::metalqueue::CommandBufferDiagnostics source{
      .seqId = 7,
      .slotIndex = 1,
      .hasPresent = true,
      .hasBlit = true,
      .hasStretchRect = true,
      .frame = 42,
      .compatFlags = 0x04,
      .vertexShaderHash = 0x11,
      .pixelShaderHash = 0x21,
      .shaderVariantHash = 0x31,
  };

  const auto merged = mergeCommandBufferDiagnostics(aggregate, source);

  checkEq(merged.seqId, 9ull, "merged diagnostics keep tail seq identity");
  checkEq(merged.slotIndex, 3u, "merged diagnostics keep tail slot identity");
  check(merged.hasDraw, "merged diagnostics retain draw flag");
  check(merged.hasPresent, "merged diagnostics aggregate present flag");
  check(merged.hasBlit, "merged diagnostics aggregate blit flag");
  check(merged.hasStretchRect, "merged diagnostics aggregate stretch flag");
  checkEq(merged.frame, 42u, "merged diagnostics pick first non-zero frame");
  checkEq(merged.compatFlags, 0x05u, "merged diagnostics OR compat flags");
  checkEq(merged.vertexShaderHash, 0x11ull, "merged diagnostics use latest VS hash");
  checkEq(merged.pixelShaderHash, 0x21ull, "merged diagnostics use latest PS hash");
  checkEq(merged.shaderVariantHash, 0x31ull,
          "merged diagnostics use latest shader variant hash");
}

void encodeChunkSessionFactoryStartsWithoutActiveRender() {
  auto session = dxmt9::encoders::makeEncodeChunkSession();

  check(static_cast<bool>(session), "encode session factory returns a session");
  check(!dxmt9::encoders::encodeChunkSessionHasActiveRender(*session),
        "new encode session has no active render encoder");
  check(!dxmt9::encoders::encodeChunkSessionHasDeferredSubmissionPayload(*session),
        "new encode session has no deferred submission payload");

  dxmt9::encoders::resetEncodeChunkSession(*session);
  check(!dxmt9::encoders::encodeChunkSessionHasActiveRender(*session),
        "reset encode session has no active render encoder");
  check(!dxmt9::encoders::encodeChunkSessionHasDeferredSubmissionPayload(*session),
        "reset encode session has no deferred submission payload");
}

void encodeChunkSessionOwnsOrderedSourceList() {
  auto session = dxmt9::encoders::makeEncodeChunkSession();

  check(dxmt9::encoders::canAppendEncodeChunkSessionSource(
            *session,
            QueueCompletionSource{
                .source = testSource(3, 10),
                .slotIndex = 3,
                .seqId = 10,
                .hasPresent = false,
                .commandCount = 6,
            }),
        "encode session preflight accepts first completion source");
  check(dxmt9::encoders::appendEncodeChunkSessionSource(
            *session,
            QueueCompletionSource{
                .source = testSource(3, 10),
                .slotIndex = 3,
                .seqId = 10,
                .hasPresent = false,
                .commandCount = 6,
            }),
        "encode session accepts first completion source");
  check(dxmt9::encoders::canAppendEncodeChunkSessionSource(
            *session,
            QueueCompletionSource{
                .source = testSource(4, 11),
                .slotIndex = 4,
                .seqId = 11,
                .hasPresent = true,
                .commandCount = 1,
            }),
        "encode session preflight accepts present tail completion source");
  check(dxmt9::encoders::appendEncodeChunkSessionSource(
            *session,
            QueueCompletionSource{
                .source = testSource(4, 11),
                .slotIndex = 4,
                .seqId = 11,
                .hasPresent = true,
                .commandCount = 1,
            }),
        "encode session accepts present tail completion source");

  const auto sources = dxmt9::encoders::encodeChunkSessionSources(*session);
  checkEq(sources.size(), 2u, "encode session owns two completion sources");
  checkEq(sources[0].seqId, 10ull, "encode session keeps head seq");
  checkEq(sources[1].seqId, 11ull, "encode session keeps tail seq");
  check(sources[1].hasPresent, "encode session keeps present flag");
  checkEq(sources[0].commandCount, 6u,
          "encode session keeps head command count");
  checkEq(sources[1].commandCount, 1u,
          "encode session keeps tail command count");
  checkEq(sources[0].commandBegin, 0u,
          "encode session keeps head command begin");
  checkEq(sources[1].commandBegin, 0u,
          "encode session keeps tail command begin");
  check(!dxmt9::encoders::canAppendEncodeChunkSessionSource(
            *session,
            QueueCompletionSource{
                .source = testSource(5, 12),
                .slotIndex = 5,
                .seqId = 12,
                .hasPresent = false,
            }),
        "encode session preflight rejects source after present tail");
  check(!dxmt9::encoders::appendEncodeChunkSessionSource(
            *session,
            QueueCompletionSource{
                .source = testSource(5, 12),
                .slotIndex = 5,
                .seqId = 12,
                .hasPresent = false,
            }),
        "encode session rejects source after present tail");

  dxmt9::encoders::resetEncodeChunkSession(*session);
  check(dxmt9::encoders::encodeChunkSessionSources(*session).empty(),
        "reset clears encode session source list");
}

void encodeChunkSessionBatchAppendIsTransactional() {
  auto session = dxmt9::encoders::makeEncodeChunkSession();
  const QueueCompletionSource head{
      .source = testSource(3, 10),
      .slotIndex = 3,
      .seqId = 10,
      .commandCount = 2,
  };
  check(dxmt9::encoders::appendEncodeChunkSessionSource(*session, head),
        "batch fixture appends its head source");

  const std::array rejectedBatch{
      QueueCompletionSource{
          .source = testSource(4, 11),
          .slotIndex = 4,
          .seqId = 11,
          .commandCount = 3,
      },
      QueueCompletionSource{
          .source = testSource(5, 13),
          .slotIndex = 5,
          .seqId = 13,
          .commandCount = 1,
      },
  };
  check(!dxmt9::encoders::appendEncodeChunkSessionSources(
            *session, rejectedBatch),
        "sequence gap rejects the whole source batch");
  auto sources = dxmt9::encoders::encodeChunkSessionSources(*session);
  checkEq(sources.size(), std::size_t{1},
          "rejected batch does not retain its valid prefix");
  checkEq(sources.front().seqId, 10ull,
          "rollback preserves the original session head");

  const std::array acceptedBatch{
      rejectedBatch[0],
      QueueCompletionSource{
          .source = testSource(5, 12),
          .slotIndex = 5,
          .seqId = 12,
          .commandCount = 1,
      },
  };
  check(dxmt9::encoders::appendEncodeChunkSessionSources(
            *session, acceptedBatch),
        "strict FIFO source batch appends transactionally");
  sources = dxmt9::encoders::encodeChunkSessionSources(*session);
  checkEq(sources.size(), std::size_t{3},
          "successful batch publishes its full source list");
  checkEq(sources[1].seqId, 11ull,
          "successful batch preserves its first source");
  checkEq(sources[2].seqId, 12ull,
          "successful batch preserves its second source");

  const std::array duplicate{acceptedBatch[1]};
  check(!dxmt9::encoders::appendEncodeChunkSessionSources(
            *session, duplicate),
        "duplicate source sequence is rejected");
  checkEq(dxmt9::encoders::encodeChunkSessionSources(*session).size(),
          std::size_t{3},
          "duplicate rejection leaves the session unchanged");
}

void retainEncodeChunkSessionStoresOwnerInSubmissionRecord() {
  auto session = dxmt9::encoders::makeEncodeChunkSession();
  check(static_cast<bool>(session), "test setup creates encode session owner");

  QueueSubmissionRecord record;
  check(dxmt9::encoders::retainEncodeChunkSessionUntilSubmissionComplete(
            std::move(session), record),
        "encode session owner is retained by submission record");
  check(!session, "encode session unique owner is moved into the record");
  checkEq(record.retainedPayloads.size(), 1u,
          "submission record stores retained encode session owner");
}

struct QueueFixture {
  std::optional<std::size_t> writingSlot{};
  std::size_t writeIndex = 0;
  // Atomic to match SubmissionBinding::nextSeqId — the mark ticket is read
  // without the queue mutex (design T2a/T2a'); writes still happen under it.
  std::atomic<std::uint64_t> nextSeqId{1};
  std::deque<std::uint64_t> completedSeqQueue{};
  std::deque<std::uint64_t> completedPresentSeqQueue{};
  std::size_t inflightCount = 0;
  // Atomic only so the map DISCARD fast path can read the GPU watermark
  // without the queue mutex (design T2c); the controller is still the sole
  // writer and still writes under it.
  std::atomic<std::uint64_t> completedSeqId{0};
  std::uint64_t presentCompletedSeqId = 0;
  std::uint64_t lastCommittedSeqId = 0;
  CpuReadyTape cpuReadyTape;
  std::array<ChunkSlotControl, 4> slots{};
  std::mutex mutex{};
  std::condition_variable writeCv{};
  std::condition_variable encodeCv{};
  std::condition_variable finishCv{};
  std::condition_variable presentCompletedCv{};
  bool stop = false;
  dxmt9::queue::PipelineLifecycleObserver pipelineObserver{};
  std::optional<dxmt9::SchedulingProgressWatchdog> pipelineWatchdog{};
  QueueLifecycleController controller{};

  explicit QueueFixture(
      CpuReadyTapeConfig tapeConfig = CpuReadyTapeConfig::compatibility(4),
      bool lifecycleSink = true, bool watchdogEnabled = false)
      : cpuReadyTape(tapeConfig) {
    if (watchdogEnabled) {
      pipelineWatchdog.emplace(/*enabled=*/true, /*thresholdMs=*/1000,
                               /*startSamplerThread=*/false);
    }
    controller.bindTrackedSubmissionState(QueueLifecycleController::SubmissionBinding{
        .writingSlot = &writingSlot,
        .writeIndex = &writeIndex,
        .nextSeqId = &nextSeqId,
        .completedSeqQueue = &completedSeqQueue,
        .completedPresentSeqQueue = &completedPresentSeqQueue,
        .completedArenaGroupSettlements =
            controller.completedArenaGroupSettlementLedger(),
        .inflightCount = &inflightCount,
        .completedSeqId = &completedSeqId,
        .presentCompletedSeqId = &presentCompletedSeqId,
        .lastCommittedSeqId = &lastCommittedSeqId,
        .slots = std::span<ChunkSlotControl>(slots.data(), slots.size()),
        .cpuReadyTape = &cpuReadyTape,
        .mutex = &mutex,
        .writeCv = &writeCv,
        .encodeCv = &encodeCv,
        .finishCv = &finishCv,
        .presentCompletedCv = &presentCompletedCv,
        .stop = &stop,
        .schedulingProgressWatchdog = pipelineWatchdog
            ? &*pipelineWatchdog : nullptr,
        .pipelineLifecycleObserver = lifecycleSink
            ? pipelineObserver.productionSink()
            : dxmt9::queue::PipelineLifecycleObserverSink{},
        .pipelineControlObserver = pipelineObserver.productionControlSink(),
    });
  }

  void addReadySlot(std::size_t slotIndex,
                    std::uint64_t seqId,
                    bool appendClear = false,
                    const std::function<void(ChunkSlot&)>& beforePublish = {},
                    std::size_t usedBytes = 0,
                    std::uint64_t rawOrdinal = 0) {
    const auto reservation = cpuReadyTape.reserve();
    check(reservation.has_value(), "fixture reserves CPU-ready payload");
    slots[slotIndex].sourceId = reservation->id;
    slots[slotIndex].storage = reservation->storage;
    slots[slotIndex].payload = reservation->payload;
    if (appendClear) {
      slots[slotIndex].payload->appendClear({});
    }
    if (beforePublish) {
      beforePublish(*slots[slotIndex].payload);
    }
    check(cpuReadyTape.sealAndPublish(
              reservation->ticket, seqId, seqId, slotIndex,
              usedBytes, rawOrdinal),
          "fixture publishes CPU-ready payload");
    slots[slotIndex].state = ChunkSlot::State::Pending;
    slots[slotIndex].seqId = seqId;
    slots[slotIndex].payload->seqId = seqId;
    lastCommittedSeqId = std::max(lastCommittedSeqId, seqId);
    nextSeqId.store(std::max(nextSeqId.load(std::memory_order_relaxed),
                             seqId + 1u),
                    std::memory_order_relaxed);
    ++inflightCount;
  }

  std::size_t readyControlIndex(std::size_t offset = 0) const {
    std::array<CpuReadyTape::ReadyEntry, 4> ready{};
    const std::size_t count = cpuReadyTape.copyReadyPrefix(ready);
    check(offset < count, "fixture Ready offset must exist");
    return ready[offset].controlIndex;
  }
};

// Actual-owner smoke: use QueueLifecycleController's production Tape and CV
// paths (including publication, dequeue, inline completion, finish wake, and
// reclaim) rather than the logical predicate-only fixture.  The command
// buffer is intentionally absent; this is the deterministic native lane.
void actualOwnerPipelineObserverUsesQueueAndCv() {
  QueueFixture fixture(CpuReadyTapeConfig::compatibility(1));
  {
    std::unique_lock lock(fixture.mutex);
    fixture.controller.presentAndCommit(lock, 1u, dxmt9::core::SwapDesc{},
                                        dxmt9::core::Handle{});
    check(fixture.lastCommittedSeqId == 1u,
          "actual owner publishes one Present-bearing source");
    ReadySlotSnapshot source{};
    check(fixture.controller.dequeueReadySlot(lock, source),
          "actual owner dequeues through the encode CV path");
    check(fixture.controller.completeInlineChunk(lock, source.slotIndex,
                                                 source.seqId),
          "actual owner completes inline through the reclaim transaction");
    check(fixture.controller.runFinishIteration(lock),
          "actual owner drains the finish CV completion and waterline");
  }
  check(fixture.pipelineObserver.state().ownerEventCount != 0u,
        "production sink receives actual QueueLifecycleController owners");
  const auto& ownerState = fixture.pipelineObserver.state();
  check(ownerState.ownerEventCount == 6u,
        "one physical owner emits each lifecycle edge exactly once");
  constexpr std::array expectedStages{
      std::pair{dxmt9::queue::PipelineStage::SourceArrival,
                 dxmt9::queue::PipelineStage::ProducerOwned},
      std::pair{dxmt9::queue::PipelineStage::ProducerOwned,
                 dxmt9::queue::PipelineStage::RawOwned},
      std::pair{dxmt9::queue::PipelineStage::RawOwned,
                 dxmt9::queue::PipelineStage::ReplayBorrowed},
      std::pair{dxmt9::queue::PipelineStage::ReplayBorrowed,
                 dxmt9::queue::PipelineStage::FinalOwned},
      std::pair{dxmt9::queue::PipelineStage::FinalOwned,
                 dxmt9::queue::PipelineStage::Encoding},
      std::pair{dxmt9::queue::PipelineStage::Encoding,
                 dxmt9::queue::PipelineStage::Reclaimed},
  };
  for (std::size_t i = 0; i < expectedStages.size(); ++i) {
    check(ownerState.ownerEvents[i].from == expectedStages[i].first &&
              ownerState.ownerEvents[i].to == expectedStages[i].second,
          "actual owner event order has no fabricated GPU edge");
  }
  check(ownerState.ownerEvents[0].owner == dxmt9::queue::PipelineOwner::PeImport &&
            ownerState.ownerEvents[5].disposition ==
                dxmt9::queue::PipelineDisposition::NoGpuTerminal,
        "actual owners qualify import and zero-GPU terminal evidence");
  const auto& event = ownerState.ownerEvents[0];
  check(event.identity.sourceOrdinal == event.identity.seqId &&
            event.identity.generation != 0u,
        "actual owner evidence preserves source ordinal, seqId, and generation");
}

void disabledPipelineProjectionDoesNotResolveOrEmit() {
  QueueFixture fixture(CpuReadyTapeConfig::compatibility(1),
                       /*lifecycleSink=*/false, /*watchdogEnabled=*/false);
  const auto staleRejectsBefore = fixture.cpuReadyTape.stats().staleRejects;
  {
    std::unique_lock lock(fixture.mutex);
    check(fixture.controller.presentAndCommit(lock, 1u, dxmt9::core::SwapDesc{},
                                              dxmt9::core::Handle{}),
          "disabled projection fixture publishes one source");
    ReadySlotSnapshot source{};
    check(fixture.controller.dequeueReadySlot(lock, source),
          "disabled projection fixture dequeues one source");
    check(fixture.controller.completeInlineChunk(lock, source.slotIndex,
                                                 source.seqId),
          "disabled projection fixture completes one source inline");
    check(fixture.controller.runFinishIteration(lock),
          "disabled projection fixture reclaims one source");
  }
  check(fixture.cpuReadyTape.stats().staleRejects == staleRejectsBefore &&
            fixture.pipelineObserver.state().ownerEventCount == 0u,
        "disabled projection gates before Tape resolution and emits no owner rows");
}

void watchdogOnlyPipelineProjectionRetainsAttribution() {
  QueueFixture fixture(CpuReadyTapeConfig::compatibility(1),
                       /*lifecycleSink=*/false, /*watchdogEnabled=*/true);
  {
    std::unique_lock lock(fixture.mutex);
    check(fixture.controller.presentAndCommit(lock, 1u, dxmt9::core::SwapDesc{},
                                              dxmt9::core::Handle{}),
          "watchdog-only fixture publishes one source");
    ReadySlotSnapshot source{};
    check(fixture.controller.dequeueReadySlot(lock, source),
          "watchdog-only fixture dequeues one source");
    check(fixture.controller.completeInlineChunk(lock, source.slotIndex,
                                                 source.seqId),
          "watchdog-only fixture completes one source inline");
    check(fixture.controller.runFinishIteration(lock),
          "watchdog-only fixture reclaims one source");
  }
  const auto snapshot = fixture.pipelineWatchdog->slotSnapshotForTest(1u);
  check(snapshot.tracked && snapshot.identity == 1u &&
            snapshot.owner == dxmt9::queue::PipelineOwner::Queue &&
            snapshot.disposition == dxmt9::queue::PipelineDisposition::NoGpuTerminal,
        "watchdog-only projection preserves production owner attribution");
  check(fixture.pipelineObserver.state().ownerEventCount == 0u,
        "watchdog-only projection does not require a lifecycle sink");
}

void controlBoundaryObservationIsColdAndNonReclaiming() {
  QueueFixture fixture;
  fixture.controller.observePipelineControl(
      dxmt9::queue::PipelineControl::Reset,
      dxmt9::queue::PipelineDisposition::Reset);
  const auto& state = fixture.pipelineObserver.state();
  check(state.controlEventCount == 1u && state.ownerEventCount == 0u,
        "Reset emits one cold control observation without owner rows");
  check(state.controlEvents[0].control == dxmt9::queue::PipelineControl::Reset &&
            state.controlEvents[0].disposition ==
                dxmt9::queue::PipelineDisposition::Reset &&
            state.controlEvents[0].epoch != 0u &&
            state.controlEvents[0].liveSourceCount == 0u &&
            state.controlEvents[0].drained,
        "Reset observation records epoch and drained source count");
  fixture.controller.observePipelineControl(
      dxmt9::queue::PipelineControl::Teardown,
      dxmt9::queue::PipelineDisposition::Teardown);
  check(fixture.pipelineObserver.state().controlEventCount == 2u &&
            fixture.pipelineObserver.state().controlEvents[1].epoch >
                fixture.pipelineObserver.state().controlEvents[0].epoch,
        "Teardown advances the cold control epoch independently");
}

void arenaSourceArrivalUsesOneGenerationQualifiedOwnerEdge() {
  QueueFixture fixture;
  const auto source = testSource(2u, 7u);
  fixture.controller.recordPipelineSourceArrival(
      CpuReadyTape::PayloadKind::Arena, source,
      CpuReadyAdmissionIdentity{
          .rawOrdinal = 101u,
          .sourceOrdinal = 202u,
          .seqId = 7u,
          .buildGeneration = 404u,
      },
      128u);
  const auto& state = fixture.pipelineObserver.state();
  check(state.ownerEventCount == 1u,
        "Arena admission records exactly one SourceArrival owner edge");
  const auto& event = state.ownerEvents[0];
  check(event.from == dxmt9::queue::PipelineStage::SourceArrival &&
            event.to == dxmt9::queue::PipelineStage::ProducerOwned &&
            event.owner == dxmt9::queue::PipelineOwner::PeImport &&
            event.payloadKind == dxmt9::queue::PipelinePayloadKind::Arena &&
            event.identity.workId == 101u &&
            event.identity.sourceOrdinal == 202u && event.identity.seqId == 7u &&
            event.identity.generation == source.storage.generation,
        "Arena arrival preserves raw/source/seq/storage-generation identity");
}

void finishPathDrainsSettlementLedgerBeyondCapacity() {
  QueueFixture fixture;
  constexpr std::uint64_t eventCount = static_cast<std::uint64_t>(
      ArenaGroupSettlementLedger::kCapacity * 2u + 7u);
  for (std::uint64_t tail = 1; tail <= eventCount; ++tail) {
    check(fixture.controller.completedArenaGroupSettlementLedger()->append(
              CpuReadyTape::ArenaGroupSettlement{
                  .rawOrdinal = tail,
                  .buildGeneration = tail,
                  .firstSourceOrdinal = tail,
                  .tailSeqId = tail,
                  .sourceCount = 1,
                  .hasPresent = false,
              }),
          "production fixture appends the next event settlement");
    fixture.completedSeqQueue.push_back(tail);
    fixture.lastCommittedSeqId = tail;
    std::unique_lock lock(fixture.mutex);
    check(fixture.controller.runFinishIteration(lock),
          "finish path consumes the completed event settlement");
  }
  checkEq(fixture.controller.completedEventSettlementCount(), eventCount,
          "finish path drains more events than the fixed ledger capacity");
  checkEq(fixture.controller.completedEventTailSeqId(), eventCount,
          "finish path advances the monotonic event tail waterline");
  const auto& last = fixture.controller.lastCompletedEventSettlement();
  check(last.has_value() && !last->hasPresent &&
            last->tailSeqId == eventCount,
        "finish path retains the value-owned final non-Present status");
}

void reclaimLeavesReservedCompatibilityWriterAtFifoHead() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1, true);

  ReadySlotSnapshot completedSource{};
  QueueSubmissionRecord record;
  record.testOnlyAllowNullCommandBuffer = true;
  {
    std::unique_lock lock(fixture.mutex);
    check(fixture.controller.dequeueReadySlot(lock, completedSource),
          "frontier fixture represents the older source");
    record.slotIndex = completedSource.slotIndex;
    record.seqId = completedSource.seqId;
    const std::array completionSources{
        completionSourceForReadySlot(completedSource),
    };
    check(record.assignFixedCompletionSources(completionSources),
          "frontier fixture stores the older source locator");
    check(fixture.controller.submitEncodedSubmission(lock, record),
          "frontier fixture submits the older source");
  }

  check(fixture.cpuReadyTape.complete(completedSource.sourceId,
                                     completedSource.storage),
        "frontier fixture completes the older source");
  fixture.completedSeqQueue.push_back(1);

  std::unique_lock lock(fixture.mutex);
  check(fixture.controller.ensureWriterSlot(lock, 4),
        "frontier fixture reserves a younger compatibility writer");
  const std::size_t writerIndex = *fixture.writingSlot;
  auto& writer = fixture.slots[writerIndex];
  check(writer.state == ChunkSlot::State::Writing && writer.seqId == 0,
        "younger compatibility source remains an uncommitted writer");
  writer.payload->appendClear({});
  const CpuReadySourceId writerSourceId = writer.sourceId;
  const CpuReadyStorageRef writerStorage = writer.storage;
  ChunkSlot* const writerPayload = writer.payload;
  const std::size_t writerCommandCount = writer.payload->commandCount();
  const auto writerCommandHeader = writer.payload->commandHeaders.front();
  const std::size_t writerClearCount = writer.payload->clearRecords.size();

  check(fixture.controller.runFinishIteration(lock),
        "reclaiming an older source tolerates the live writer frontier");
  check(!fixture.stop,
        "a reserved compatibility writer does not poison the queue");
  check(fixture.slots[writerIndex].state == ChunkSlot::State::Writing &&
            fixture.slots[writerIndex].seqId == 0 &&
            fixture.slots[writerIndex].sourceId == writerSourceId &&
            fixture.slots[writerIndex].storage == writerStorage &&
            fixture.slots[writerIndex].payload == writerPayload &&
            fixture.slots[writerIndex].payload->commandCount() ==
                writerCommandCount &&
            fixture.slots[writerIndex].payload->commandHeaders.front().kind ==
                writerCommandHeader.kind &&
            fixture.slots[writerIndex].payload->commandHeaders.front()
                    .payloadIndex == writerCommandHeader.payloadIndex &&
            fixture.slots[writerIndex].payload->clearRecords.size() ==
                writerClearCount,
        "frontier reclaim preserves the writer control and payload exactly");
  check(fixture.cpuReadyTape.state(writerSourceId, writerStorage) ==
            CpuReadyTape::State::Writing,
        "frontier reclaim preserves the writer Tape state");
  checkEq(fixture.cpuReadyTape.residentCount(), std::size_t{1},
          "only the reserved writer remains resident after older reclaim");

  check(fixture.controller.commitCurrentChunk(lock, 4),
        "preserved Writing frontier remains committable");
  check(!fixture.stop && !fixture.writingSlot.has_value() &&
            fixture.slots[writerIndex].state == ChunkSlot::State::Pending &&
            fixture.slots[writerIndex].seqId == 2 &&
            fixture.slots[writerIndex].sourceId == writerSourceId &&
            fixture.slots[writerIndex].storage == writerStorage &&
            fixture.slots[writerIndex].payload == writerPayload &&
            fixture.slots[writerIndex].payload->seqId == 2 &&
            fixture.slots[writerIndex].payload->commandCount() ==
                writerCommandCount &&
            fixture.cpuReadyTape.state(writerSourceId, writerStorage) ==
                CpuReadyTape::State::Ready,
        "committing the preserved frontier publishes the exact source");
  checkEq(fixture.cpuReadyTape.readyCount(), std::size_t{1},
          "committing the preserved frontier publishes one Ready source");
  checkEq(fixture.nextSeqId.load(std::memory_order_relaxed), 3ull,
          "committing the preserved frontier assigns the next sequence");
  checkEq(fixture.lastCommittedSeqId, 2ull,
          "committing the preserved frontier advances the committed waterline");
}

void partialSegmentSerialCompletionDefersReclaimUntilTail() {
  QueueFixture fixture{makeSeparatedPayloadConfig()};
  const auto segment = makeMinimalArenaLayout();
  const std::array segmentLayouts{segment};
  const auto sourceLayout = makeArenaSourcePayloadLayout(
      segmentLayouts, 4096, 8);
  check(sourceLayout.has_value(), "SegmentSerial source layout validates");
  const std::array layouts{*sourceLayout, *sourceLayout};
  auto batch = fixture.cpuReadyTape.reserveArenaBatch(
      layouts, 41, 11, 1, 7);
  check(batch.has_value(), "SegmentSerial group reserves atomically");
  for (std::size_t i = 0; i < batch->count; ++i) {
    publishMinimalArena(fixture.cpuReadyTape, batch->reservations[i], segment);
  }
  const std::array<std::size_t, 2> controls{0u, 1u};
  check(fixture.cpuReadyTape.sealAndPublishArenaBatch(*batch, controls),
        "SegmentSerial group publishes atomically");
  std::array<CpuReadyTape::ReadyEntry, 2> sources{};
  check(fixture.cpuReadyTape.copyReadyPrefix(sources) == sources.size() &&
            fixture.cpuReadyTape.representReadyPrefix(sources) &&
            fixture.cpuReadyTape.transitionAll(
                std::array<CpuReadyTape::SourceRef, 2>{
                    sources[0].source, sources[1].source},
                CpuReadyTape::State::Represented,
                CpuReadyTape::State::Submitted),
        "SegmentSerial group reaches submitted state");
  fixture.lastCommittedSeqId = 2;
  fixture.nextSeqId.store(3, std::memory_order_relaxed);
  fixture.inflightCount = 2;

  const auto staleRejectsBefore = fixture.cpuReadyTape.stats().staleRejects;
  check(fixture.cpuReadyTape.complete(
            sources[0].source.id, sources[0].source.storage),
        "SegmentSerial head completes independently");
  const auto partialStatus = fixture.cpuReadyTape.reclaimStatus(
      sources[0].source.id, sources[0].source.storage);
  check(partialStatus.disposition ==
            CpuReadyTape::ReclaimDisposition::AwaitingGroupCompletion &&
            partialStatus.grouped() && partialStatus.groupSourceCount == 2u &&
            partialStatus.groupSourceIndex == 0u &&
            partialStatus.groupTailSeqId == 2u,
        "reclaim observation distinguishes a valid group hold from corruption");
  fixture.completedSeqQueue.push_back(1);
  std::unique_lock lock(fixture.mutex);
  std::uint64_t finishedSeq = 0;
  check(fixture.controller.runFinishIteration(
            lock, [&](std::uint64_t seqId) { finishedSeq = seqId; }),
        "partial SegmentSerial completion advances without poisoning");
  checkEq(finishedSeq, 1ull,
          "partial SegmentSerial completion advances the source waterline");
  check(!fixture.stop && fixture.cpuReadyTape.residentCount() == 2u &&
            fixture.cpuReadyTape.stats().staleRejects == staleRejectsBefore,
        "partial completion retains the whole group without a stale reject");

  check(fixture.cpuReadyTape.complete(
            sources[1].source.id, sources[1].source.storage),
        "SegmentSerial tail completes independently");
  const auto settlement =
      fixture.cpuReadyTape.takeCompletedArenaGroupSettlement(sources[1].source);
  check(settlement.has_value() &&
            fixture.controller.completedArenaGroupSettlementLedger()->append(
                *settlement),
        "SegmentSerial tail publishes one event settlement");
  fixture.completedSeqQueue.push_back(2);
  check(fixture.controller.runFinishIteration(
            lock, [&](std::uint64_t seqId) { finishedSeq = seqId; }),
        "SegmentSerial tail completion drains the retained FIFO group");
  checkEq(finishedSeq, 2ull,
          "SegmentSerial tail advances the completion waterline");
  check(!fixture.stop && fixture.cpuReadyTape.residentCount() == 0u &&
            fixture.controller.completedEventSettlementCount() == 1u,
        "tail completion reclaims every group owner exactly once");

  lock.unlock();
  const auto eventSerial = fixture.cpuReadyTape.reserve();
  check(eventSerial.has_value(),
        "EventSerial remains reachable after SegmentSerial settlement");
  eventSerial->payload->appendClear({});
  eventSerial->payload->seqId = 3;
  check(fixture.cpuReadyTape.sealAndPublish(
            eventSerial->ticket, 13, 3, 0) &&
            fixture.cpuReadyTape.copyReadyPrefix(
                std::span<CpuReadyTape::ReadyEntry>(sources).first(1)) == 1u &&
            fixture.cpuReadyTape.representReadyPrefix(
                std::span<const CpuReadyTape::ReadyEntry>(sources).first(1)) &&
            fixture.cpuReadyTape.transition(
                sources[0].source.id, sources[0].source.storage,
                CpuReadyTape::State::Represented,
                CpuReadyTape::State::Submitted) &&
            fixture.cpuReadyTape.complete(
                sources[0].source.id, sources[0].source.storage),
        "following EventSerial source reaches completed state");
  fixture.lastCommittedSeqId = 3;
  fixture.nextSeqId.store(4, std::memory_order_relaxed);
  fixture.inflightCount = 1;
  fixture.completedSeqQueue.push_back(3);
  lock.lock();
  check(fixture.controller.runFinishIteration(
            lock, [&](std::uint64_t seqId) { finishedSeq = seqId; }) &&
            finishedSeq == 3u && fixture.cpuReadyTape.residentCount() == 0u &&
            !fixture.stop,
        "following EventSerial source preserves FIFO completion and reclaim");
}

void appendShapeTestDraw(ChunkSlot& slot,
                         std::span<const dxmt9::core::DrawParam> draws,
                         std::span<const dxmt9::core::DrawParamPayloadView> payloads) {
  dxmt9::core::DrawUniformPayload uniforms{};
  slot.appendDrawRun(dxmt9::core::CanonicalDrawState{}, uniforms, draws, payloads);
}

void firstPublishSlotShapeClassifiesTailPresentPrefix() {
  ChunkSlot slot{};
  slot.appendClear(dxmt9::core::ClearDesc{});

  const std::array<dxmt9::core::u8, 4> firstPayload{1, 2, 3, 4};
  const std::array<dxmt9::core::u8, 2> secondPayload{5, 6};
  std::array<dxmt9::core::DrawParam, 2> draws{{
      dxmt9::core::DrawParam{.primitiveCount = 1u},
      dxmt9::core::DrawParam{.primitiveCount = 2u},
  }};
  std::array<dxmt9::core::DrawParamPayloadView, 2> payloads{{
      dxmt9::core::DrawParamPayloadView{
          .userVertexData = std::span<const dxmt9::core::u8>(
              firstPayload.data(), firstPayload.size()),
      },
      dxmt9::core::DrawParamPayloadView{
          .userVertexData = std::span<const dxmt9::core::u8>(
              secondPayload.data(), secondPayload.size()),
      },
  }};
  appendShapeTestDraw(slot,
                      std::span<const dxmt9::core::DrawParam>(
                          draws.data(), draws.size()),
                      std::span<const dxmt9::core::DrawParamPayloadView>(
                          payloads.data(), payloads.size()));
  slot.appendPresent(dxmt9::core::SwapDesc{}, dxmt9::core::Handle{0x55});

  const auto shape = summarizeNoEnqueueFirstPublishSlotShape(slot);

  checkEq(shape.commandCount, 3ull,
          "tail-present shape counts every command");
  checkEq(shape.drawRunCommands, 1ull,
          "tail-present shape counts draw-run commands");
  checkEq(shape.drawItems, 2ull,
          "tail-present shape counts draw items");
  checkEq(shape.nonDrawCommands, 2ull,
          "tail-present shape counts clear and present as non-draw");
  checkEq(shape.payloadBytes, 6ull,
          "tail-present shape counts all slot payload bytes");
  checkEq(shape.presentCommands, 1ull,
          "tail-present shape counts present commands");
  checkEq(shape.prePresentCommands, 2ull,
          "tail-present shape counts commands before first present");
  checkEq(shape.prePresentDrawRunCommands, 1ull,
          "tail-present shape counts pre-present draw-run commands");
  checkEq(shape.prePresentDrawItems, 2ull,
          "tail-present shape counts pre-present draw items");
  checkEq(shape.prePresentNonDrawCommands, 1ull,
          "tail-present shape counts pre-present non-draw commands");
  checkEq(shape.prePresentPayloadBytes, 6ull,
          "tail-present shape counts pre-present draw payload bytes");
  checkEq(shape.postPresentCommands, 0ull,
          "tail-present shape has no commands after present");
  checkEq(shape.presentTailSlots, 1ull,
          "tail-present shape classifies present as tail");
  checkEq(shape.presentNonTailSlots, 0ull,
          "tail-present shape does not classify non-tail present");
}

void firstPublishSlotShapeRejectsPostPresentWorkAsTail() {
  ChunkSlot slot{};
  const std::array<dxmt9::core::DrawParam, 1> draws{{
      dxmt9::core::DrawParam{.primitiveCount = 3u},
  }};
  appendShapeTestDraw(slot,
                      std::span<const dxmt9::core::DrawParam>(
                          draws.data(), draws.size()),
                      std::span<const dxmt9::core::DrawParamPayloadView>{});
  slot.appendPresent(dxmt9::core::SwapDesc{}, dxmt9::core::Handle{0x66});
  slot.appendClear(dxmt9::core::ClearDesc{});

  const auto shape = summarizeNoEnqueueFirstPublishSlotShape(slot);

  checkEq(shape.commandCount, 3ull,
          "non-tail-present shape counts every command");
  checkEq(shape.prePresentCommands, 1ull,
          "non-tail-present shape stops prefix at first present");
  checkEq(shape.prePresentDrawRunCommands, 1ull,
          "non-tail-present shape counts the draw before present");
  checkEq(shape.prePresentDrawItems, 1ull,
          "non-tail-present shape counts pre-present draw item");
  checkEq(shape.postPresentCommands, 1ull,
          "non-tail-present shape counts commands after present");
  checkEq(shape.presentTailSlots, 0ull,
          "non-tail-present shape rejects present tail classification");
  checkEq(shape.presentNonTailSlots, 1ull,
          "non-tail-present shape classifies the slot as non-tail present");
}

void firstPublishSlotShapeKeepsNoPresentSlotUnclassified() {
  ChunkSlot slot{};
  const std::array<dxmt9::core::DrawParam, 1> draws{{
      dxmt9::core::DrawParam{.primitiveCount = 4u},
  }};
  appendShapeTestDraw(slot,
                      std::span<const dxmt9::core::DrawParam>(
                          draws.data(), draws.size()),
                      std::span<const dxmt9::core::DrawParamPayloadView>{});
  slot.appendClear(dxmt9::core::ClearDesc{});

  const auto shape = summarizeNoEnqueueFirstPublishSlotShape(slot);

  checkEq(shape.commandCount, 2ull,
          "no-present shape counts commands");
  checkEq(shape.prePresentCommands, 2ull,
          "no-present shape treats all commands as pre-present");
  checkEq(shape.postPresentCommands, 0ull,
          "no-present shape has no post-present commands");
  checkEq(shape.presentTailSlots, 0ull,
          "no-present shape does not classify present tail");
  checkEq(shape.presentNonTailSlots, 0ull,
          "no-present shape does not classify non-tail present");
}

void runEncodeIterationPassesLiveSlotStorage() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);

  bool observedLegacyPayload = false;
  WorkerOwnedSourceSnapshot escapedWorkerSnapshot{};
  std::vector<std::uint64_t> inlineCompleted;
  std::unique_lock lock(fixture.mutex);
  const bool encoded = fixture.controller.runEncodeIteration(
      lock,
      [&](const dxmt9::core::metalqueue::WorkerOwnedSourceSnapshot& workerSource,
          const dxmt9::core::metalqueue::GenerationQualifiedSourceBorrow& borrow)
          -> std::optional<QueueSubmissionRecord> {
        const auto source = workerSource.copyValue();
        escapedWorkerSnapshot = workerSource;
        check(borrow.valid() && workerSource.valid(),
              "encode callback receives qualified borrow and owned snapshot");
        check(borrow.source().id == source.sourceId &&
                  borrow.source().storage == source.storage &&
                  borrow.metadata().seqId == source.seqId &&
                  borrow.seqId() == source.seqId,
              "borrow carries exact source/storage and sequence generations");
        checkEq(source.slotIndex, 0u,
                "single-source iteration forwards the dequeued slot index");
        check(borrow.visitPayload(
                  [&](const dxmt9::core::metalqueue::
                          SynchronousSourcePayloadBorrow& payload) {
                observedLegacyPayload = payload.isLegacy();
              }),
              "encode callback receives a live payload capability");
        check(observedLegacyPayload,
              "single-source iteration exposes the pinned legacy payload");
        check(fixture.slots[0].state == ChunkSlot::State::Encoding,
              "single-source encode sees the control shell in Encoding state");
        return std::nullopt;
      },
      [&](std::uint64_t seqId) { inlineCompleted.push_back(seqId); });

  check(encoded, "single-source iteration consumes the ready slot");
  check(observedLegacyPayload,
        "single-source iteration visited its legacy payload capability");
  check(escapedWorkerSnapshot.valid() &&
            !fixture.controller.resolveRepresentedSource(
                escapedWorkerSnapshot.copyValue()).valid(),
        "value snapshot retained after visit cannot reacquire reclaimed payload");
  checkEq(inlineCompleted.size(), 1u,
          "inline single-source completion invokes callback once");
  checkEq(inlineCompleted.front(), 1ull,
          "inline single-source completion reports source seqId");
  checkEq(fixture.completedSeqQueue.size(), 1u,
          "inline single-source completion queues source seqId");
  check(fixture.slots[0].state == ChunkSlot::State::Free,
        "inline single-source completion releases the live slot");
  check(fixture.controller.runFinishIteration(lock),
        "finish accepts an already inline-reclaimed completed sequence");
  checkEq(fixture.completedSeqId.load(), 1ull,
          "finish advances the inline completion waterline");
  checkEq(fixture.inflightCount, 0u,
          "finish retires inline Tape residency accounting");
  check(!fixture.stop,
        "finish does not poison after an inline source was already reclaimed");
}

static_assert(!std::is_copy_constructible_v<GenerationQualifiedSourceBorrow>);
static_assert(!std::is_move_constructible_v<GenerationQualifiedSourceBorrow>);
static_assert(!std::is_copy_constructible_v<SynchronousSourcePayloadBorrow>);
static_assert(!std::is_move_constructible_v<SynchronousSourcePayloadBorrow>);
static_assert(!std::is_default_constructible_v<SynchronousSourcePayloadBorrow>);
static_assert(!std::is_constructible_v<
              SynchronousSourcePayloadBorrow,
              dxmt9::core::SourcePayloadView>);
static_assert(std::is_trivially_copyable_v<WorkerOwnedSourceSnapshot>);
static_assert(!std::is_copy_constructible_v<SynchronousSourceBorrowBatch>);
static_assert(!std::is_move_constructible_v<SynchronousSourceBorrowBatch>);
static_assert(!std::is_default_constructible_v<SynchronousSourceBorrowBatch>);
static_assert(!std::is_constructible_v<
              SynchronousSourceBorrowBatch,
              std::span<const ResolvedPublishedSource>>);
template <typename T>
concept HasDirectPayloadAccessor = requires(const T& value) {
  value.payload();
};
static_assert(!HasDirectPayloadAccessor<GenerationQualifiedSourceBorrow>);
template <typename T>
concept HasUncheckedPayloadViewAccessor = requires(const T& value) {
  value.checkedView();
};
static_assert(!HasUncheckedPayloadViewAccessor<SynchronousSourcePayloadBorrow>);

void synchronousSourceBatchRejectsStaleAndReclaimingBorrows() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);
  ReadySlotSnapshot source{};
  std::unique_lock lock(fixture.mutex);
  check(fixture.controller.dequeueReadySlot(lock, source),
        "borrow batch fixture dequeues one source");
  const auto resolved = fixture.controller.resolveRepresentedSource(source);
  check(resolved.valid(), "borrow batch fixture resolves a represented source");

  bool visited = false;
  bool nestedCallbackRan = false;
  bool concurrentCompletionSucceeded = true;
  check(fixture.controller.visitRepresentedSourceBorrows(
            lock,
            std::span<const ResolvedPublishedSource>(&resolved, 1),
            [&](const SynchronousSourceBorrowBatch& batch) noexcept {
              return batch.visit(
                  [&](const GenerationQualifiedSourceBorrow& borrow,
                      std::size_t index) noexcept {
                    try {
                      visited = true;
                      check(index == 0u && borrow.seqId() == source.seqId,
                            "borrow batch visits the exact generation-qualified source");
                      check(!fixture.controller.visitRepresentedSourceBorrows(
                                lock,
                                std::span<const ResolvedPublishedSource>(
                                    &resolved, 1),
                                [&](const SynchronousSourceBorrowBatch&) noexcept {
                                  nestedCallbackRan = true;
                                  return true;
                                }),
                            "active borrow epoch rejects nested reentry");
                      lock.unlock();
                      std::thread contender([&] {
                        std::unique_lock contenderLock(fixture.mutex);
                        concurrentCompletionSucceeded =
                            fixture.controller.completeInlineChunk(
                                contenderLock, source.slotIndex, source.seqId);
                      });
                      contender.join();
                      lock.lock();
                      check(borrow.valid(),
                            "concurrent reclaim rejection preserves the active borrow");
                    } catch (...) {
                      return false;
                    }
                    return true;
                  });
            }),
        "queue issues a live synchronous source batch");
  check(lock.owns_lock(),
        "borrow visitor restores queue mutex ownership before epoch release");
  check(visited, "borrow batch invokes its visitor exactly once");
  check(!nestedCallbackRan, "nested borrow callback is never invoked");
  check(!concurrentCompletionSucceeded,
        "concurrent reclaim cannot pass an already-issued source pin");

  bool resolvedViewVisited = false;
  check(fixture.controller.visitRepresentedSourceBorrows(
            lock,
            std::span<const ResolvedPublishedSource>(&resolved, 1),
            [&](const SynchronousSourceBorrowBatch& batch) noexcept {
              return batch.visitResolved(
                  [&](std::span<const ResolvedPublishedSource> views) noexcept {
                    resolvedViewVisited = views.size() == 1u &&
                        views[0].source == resolved.source &&
                        views[0].payload.valid() && views[0].slot == nullptr;
                    return resolvedViewVisited;
                  });
            }),
        "queue derives call-local resolved views from a live borrow batch");
  check(resolvedViewVisited,
        "resolved lookahead view is generation-qualified and slot-free");

  bool throwingVisitReturned = false;
  check(fixture.controller.visitRepresentedSourceBorrows(
            lock,
            std::span<const ResolvedPublishedSource>(&resolved, 1),
            [&](const SynchronousSourceBorrowBatch& batch) noexcept {
              throwingVisitReturned = !batch.visitResolved(
                  [&](std::span<const ResolvedPublishedSource>) {
                    throw std::runtime_error("borrow callback probe");
                  });
              return true;
            }),
        "throwing borrow callback remains a failed synchronous visit");
  check(throwingVisitReturned && lock.owns_lock(),
        "throwing borrow callback restores queue lock ownership");

  QueueFixture postValidationFixture;
  postValidationFixture.addReadySlot(0, 1);
  ReadySlotSnapshot postValidationSource{};
  std::unique_lock postValidationLock(postValidationFixture.mutex);
  check(postValidationFixture.controller.dequeueReadySlot(
            postValidationLock, postValidationSource),
        "post-validation fixture dequeues one source");
  const auto postValidationResolved =
      postValidationFixture.controller.resolveRepresentedSource(
          postValidationSource);
  bool postValidationCallbackRan = false;
  check(!postValidationFixture.controller.visitRepresentedSourceBorrows(
            postValidationLock,
            std::span<const ResolvedPublishedSource>(
                &postValidationResolved, 1),
            [&](const SynchronousSourceBorrowBatch& batch) noexcept {
              postValidationCallbackRan = batch.live();
              return postValidationFixture.controller
                  .invalidateActiveBorrowGenerationForTest(
                      postValidationLock);
            }),
        "post-callback generation mismatch fails the visit closed");
  check(postValidationCallbackRan,
        "generation mismatch seam runs between pre- and post-validation");
  check(postValidationLock.owns_lock() && postValidationFixture.stop,
        "failed post-validation retains the queue lock and poisons the queue");

  QueueFixture reclaimedFixture;
  reclaimedFixture.addReadySlot(0, 1);
  ReadySlotSnapshot reclaimedSource{};
  std::unique_lock reclaimedLock(reclaimedFixture.mutex);
  check(reclaimedFixture.controller.dequeueReadySlot(reclaimedLock,
                                                       reclaimedSource),
        "stale borrow fixture dequeues one source");
  const auto staleAfterVisit =
      reclaimedFixture.controller.resolveRepresentedSource(reclaimedSource);
  check(staleAfterVisit.valid(), "stale borrow fixture resolves source");
  check(reclaimedFixture.controller.completeInlineChunk(
            reclaimedLock, reclaimedSource.slotIndex, reclaimedSource.seqId),
        "stale borrow fixture reclaims the represented source");
  bool staleCallbackRan = false;
  check(!reclaimedFixture.controller.visitRepresentedSourceBorrows(
            reclaimedLock,
            std::span<const ResolvedPublishedSource>(&staleAfterVisit, 1),
            [&](const SynchronousSourceBorrowBatch&) noexcept {
              staleCallbackRan = true;
              return true;
            }),
        "queue rejects a stale source/storage generation after reclaim");
  check(!staleCallbackRan,
        "stale use after the prior visit never reaches a payload callback");
}

void postEncodeRetiringControlRepresentsUnlockedReclaim() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1, true);

  ReadySlotSnapshot source{};
  std::unique_lock lock(fixture.mutex);
  check(fixture.controller.dequeueReadySlot(lock, source),
        "retiring fixture represents its Ready source");
  auto* payload = fixture.cpuReadyTape.beginPostEncodeLegacyRetire(
      source.sourceId, source.storage);
  check(payload != nullptr,
        "retiring fixture begins the two-phase Tape reclaim");
  fixture.slots[0].state = ChunkSlot::State::Retiring;

  fixture.controller.waitForSequence(lock, 0);
  check(fixture.slots[0].commandsEmpty() &&
            fixture.slots[0].commandCount() == 0,
        "retiring control does not expose payload storage to trace or "
        "transition consumers while destruction may run unlocked");
  payload->clearCommands();
  payload->seqId = 0;
  fixture.controller.waitForSequence(lock, 0);
  check(fixture.slots[0].state == ChunkSlot::State::Retiring &&
            fixture.cpuReadyTape.state(source.sourceId, source.storage) ==
                CpuReadyTape::State::Reclaiming,
        "concurrent lifecycle observations accept the explicit unlocked "
        "retirement state before and after payload destruction");

  check(fixture.cpuReadyTape.finishReclaim(source.sourceId, source.storage),
        "retiring fixture finishes its Tape reclaim");
  fixture.slots[0].state = ChunkSlot::State::Free;
  fixture.slots[0].seqId = 0;
  fixture.slots[0].sourceId = {};
  fixture.slots[0].storage = {};
  fixture.slots[0].payload = nullptr;
  --fixture.inflightCount;
  fixture.controller.waitForSequence(lock, 0);
}

void failedEncodeSubmissionDoesNotRunPostCommitCallbacks() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);

  bool callbackRan = false;
  ReadySlotSnapshot represented{};
  std::unique_lock lock(fixture.mutex);
  const bool encoded = fixture.controller.runEncodeIteration(
      lock,
      [&](const dxmt9::core::metalqueue::WorkerOwnedSourceSnapshot& workerSource,
          const dxmt9::core::metalqueue::GenerationQualifiedSourceBorrow&)
          -> std::optional<QueueSubmissionRecord> {
        const auto source = workerSource.copyValue();
        represented = source;
        QueueSubmissionRecord record;
        record.testOnlyAllowNullCommandBuffer = true;
        record.slotIndex = source.slotIndex;
        record.seqId = source.seqId;
        auto corrupt = completionSourceForReadySlot(source);
        corrupt.commandCount += 1u;
        const std::array completionSources{corrupt};
        check(record.assignFixedCompletionSources(completionSources),
              "corrupt submission fixture retains its locator");
        record.postCommitCallbacks.push_back(
            [&] { callbackRan = true; });
        return record;
      });

  check(!encoded, "submission preflight failure stops encode iteration");
  check(!callbackRan,
        "submission preflight failure runs no post-commit callback");
  check(fixture.stop, "submission preflight failure follows fatal stop policy");
  check(fixture.slots[0].state == ChunkSlot::State::Encoding &&
            fixture.slots[0].sourceId == represented.sourceId &&
            fixture.slots[0].storage == represented.storage,
        "submission preflight failure preserves the temporary control");
  check(fixture.cpuReadyTape.state(represented.sourceId,
                                   represented.storage) ==
            CpuReadyTape::State::Represented,
        "submission preflight failure preserves Tape state");
}

void compatibilityPublicationRetainsLegacyInflightLimit() {
  QueueFixture fixture;
  constexpr std::size_t legacyInflightLimit = 1;
  {
    std::unique_lock lock(fixture.mutex);
    check(fixture.controller.ensureWriterSlot(lock, legacyInflightLimit),
          "compatibility writer acquires below the inflight limit");
  }
  check(fixture.writingSlot.has_value(),
        "writer acquisition binds the compatibility control");
  fixture.slots[*fixture.writingSlot].payload->appendClear({});
  fixture.inflightCount = legacyInflightLimit;

  std::mutex enteredMutex;
  std::condition_variable enteredCv;
  bool entered = false;
  bool published = false;
  std::thread publisher([&] {
    std::unique_lock lock(fixture.mutex);
    {
      std::lock_guard enteredLock(enteredMutex);
      entered = true;
    }
    enteredCv.notify_one();
    published =
        fixture.controller.commitCurrentChunk(lock, legacyInflightLimit);
  });

  {
    std::unique_lock enteredLock(enteredMutex);
    enteredCv.wait(enteredLock, [&] { return entered; });
  }
  bool remainedBlocked = false;
  {
    // Acquiring the queue mutex proves commitCurrentChunk reached its CV wait
    // and released the lock; it cannot publish until this scope lowers inflight.
    std::unique_lock lock(fixture.mutex);
    remainedBlocked = fixture.cpuReadyTape.readyCount() == 0u &&
        fixture.writingSlot.has_value();
    fixture.inflightCount = 0;
  }
  fixture.writeCv.notify_all();
  publisher.join();

  check(remainedBlocked,
        "compatibility publication remains blocked at the legacy inflight limit");
  check(published, "publication resumes after GPU-inflight capacity returns");
  checkEq(fixture.cpuReadyTape.readyCount(), 1u,
          "resumed publication enters the Tape Ready FIFO");
  checkEq(fixture.inflightCount, 1u,
          "resumed compatibility publication consumes one inflight slot");
  check(fixture.slots[0].state == ChunkSlot::State::Pending,
        "published compatibility source retains its Pending control");
}

void commitStaleWritingStorageFailStopsWithoutMutation() {
  QueueFixture fixture;
  std::unique_lock lock(fixture.mutex);
  check(fixture.controller.ensureWriterSlot(lock, 1),
        "stale commit fixture acquires a Writing source");
  auto& control = fixture.slots[*fixture.writingSlot];
  ChunkSlot* const payload = control.payload;
  const CpuReadySourceId sourceId = control.sourceId;
  const CpuReadyStorageRef storage = control.storage;
  payload->appendClear({});
  ++control.storage.generation;

  check(!fixture.controller.commitCurrentChunk(lock, 1),
        "stale Writing storage generation rejects commit in release");
  check(fixture.stop &&
            fixture.cpuReadyTape.probeReserve() ==
                CpuReadyTape::ReserveProbe::Stopped,
        "stale Writing commit fail-stops queue and Tape admission");
  check(fixture.writingSlot == std::optional<std::size_t>{0} &&
            fixture.writeIndex == 0u && fixture.nextSeqId == 1u &&
            fixture.inflightCount == 0u &&
            fixture.cpuReadyTape.readyEmpty(),
        "stale Writing commit changes no publication cursor or occupancy");
  check(control.state == ChunkSlot::State::Writing &&
            control.seqId == 0 && control.payload == payload &&
            payload->seqId == 0 && payload->commandCount() == 1u &&
            fixture.cpuReadyTape.state(sourceId, storage) ==
                CpuReadyTape::State::Writing,
        "stale Writing commit leaves control and Tape payload unmodified");
}

void finishReleasesSlotResourceOwnersOutsideQueueLock() {
  QueueFixture fixture{CpuReadyTapeConfig::compatibility(1)};
  std::unique_lock lock(fixture.mutex, std::defer_lock);
  CpuReadyTape::SourceRef sourceRef{};
  bool released = false;
  bool releasedWhileLocked = false;
  bool observedReclaiming = false;
  bool genericResolveRejectedDuringRelease = false;
  bool genericStorageRejectedDuringRelease = false;
  bool genericMatchRejectedDuringRelease = false;
  bool reclaimAccessorPinnedDuringRelease = false;
  bool admissionClosedDuringRelease = false;
  auto buffer = std::shared_ptr<dxmt9::core::Buffer>(
      reinterpret_cast<dxmt9::core::Buffer*>(0x1),
      [&](dxmt9::core::Buffer*) {
        released = true;
        releasedWhileLocked = lock.owns_lock();
        const auto resident = fixture.cpuReadyTape.oldestResident();
        observedReclaiming =
            resident && resident->source == sourceRef &&
            resident->state == CpuReadyTape::State::Reclaiming;
        genericResolveRejectedDuringRelease =
            fixture.cpuReadyTape.resolve(
                sourceRef.id, sourceRef.storage,
                CpuReadyTape::State::Reclaiming) == nullptr;
        genericStorageRejectedDuringRelease =
            fixture.cpuReadyTape.resolveStorage(
                sourceRef.id, sourceRef.storage,
                CpuReadyTape::State::Reclaiming).empty();
        genericMatchRejectedDuringRelease =
            !fixture.cpuReadyTape.matches(
                sourceRef, 1, CpuReadyTape::State::Reclaiming);
        const auto* pinned = fixture.cpuReadyTape.reclaimingPayload(
            sourceRef.id, sourceRef.storage);
        reclaimAccessorPinnedDuringRelease = pinned && pinned->seqId == 1;
        admissionClosedDuringRelease = !fixture.cpuReadyTape.canReserve();
      });
  fixture.addReadySlot(
      0, 1, false, [&](ChunkSlot& writingPayload) {
        writingPayload.drawShaderLayouts.emplace_back()
            .vertexDecl.streams[0].buffer = buffer;
      });
  buffer.reset();
  lock.lock();
  ReadySlotSnapshot source{};
  check(fixture.controller.dequeueReadySlot(lock, source),
        "fixture represents resource-owning payload");
  sourceRef = CpuReadyTape::SourceRef{
      .id = source.sourceId,
      .storage = source.storage,
  };
  check(fixture.cpuReadyTape.transition(
            source.sourceId, source.storage,
            CpuReadyTape::State::Represented,
            CpuReadyTape::State::Submitted) &&
            fixture.cpuReadyTape.complete(source.sourceId, source.storage),
        "fixture submits and completes resource-owning payload");
  fixture.slots[0] = {};
  fixture.completedSeqQueue.push_back(1);
  lock.unlock();

  lock.lock();
  check(fixture.controller.runFinishIteration(lock),
        "resource-owning GPU slot completion drains");
  check(released, "GPU slot completion releases its final buffer owner");
  check(!releasedWhileLocked,
        "GPU slot resource owners are released outside the queue mutex");
  check(observedReclaiming,
        "owner destruction observes the Tape source pinned in Reclaiming");
  check(genericResolveRejectedDuringRelease &&
            genericStorageRejectedDuringRelease &&
            genericMatchRejectedDuringRelease,
        "generic locator APIs reject Reclaiming during owner destruction");
  check(reclaimAccessorPinnedDuringRelease,
        "explicit reclaim accessor keeps the old locator pinned for detach");
  check(admissionClosedDuringRelease,
        "Reclaiming storage is not reusable during owner destruction");
  check(lock.owns_lock(),
        "finish iteration restores queue mutex ownership after release");
  check(fixture.cpuReadyTape.reclaimingPayload(
            sourceRef.id, sourceRef.storage) == nullptr,
        "finished reclaim makes the old locator stale");
  const std::uint64_t generationAfterFinish =
      fixture.cpuReadyTape.sourceGenerationAt(sourceRef.id.index);
  check(generationAfterFinish != sourceRef.id.generation,
        "finishReclaim advances the source generation before reuse");
  check(fixture.cpuReadyTape.canReserve(),
        "finished reclaim reopens the released bounded capacity");
  const auto reused = fixture.cpuReadyTape.reserve();
  check(reused.has_value(), "released source slot is reusable after finish");
  checkEq(reused->id.index, sourceRef.id.index,
          "single-entry Tape reuses the reclaimed descriptor index");
  checkEq(reused->id.generation, generationAfterFinish,
          "reuse consumes the generation advanced by finishReclaim once");
}

void dequeueReadySlotBatchMovesEveryDequeuedSlotToEncoding() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);
  fixture.addReadySlot(1, 2);

  std::array<ReadySlotSnapshot, 2> snapshots{};
  std::unique_lock lock(fixture.mutex);
  const std::size_t count =
      fixture.controller.dequeueReadySlotBatch(lock, std::span<ReadySlotSnapshot>(snapshots));

  checkEq(count, 2u, "batch dequeue returns every requested ready slot");
  check(fixture.cpuReadyTape.readyEmpty(),
        "batch dequeue drains Ready FIFO up to capacity");
  check(fixture.slots[0].state == ChunkSlot::State::Encoding,
        "first source slot moves to Encoding");
  check(fixture.slots[1].state == ChunkSlot::State::Encoding,
        "second source slot moves to Encoding");
  checkEq(snapshots[0].slotIndex, 0u, "first snapshot records slot index");
  check(fixture.controller.resolveRepresentedSource(snapshots[0]).slot ==
            fixture.slots[0].payload,
        "first locator resolves live storage only under the represented pin");
  checkEq(snapshots[0].seqId, 1ull, "first snapshot records seqId");
  check(!snapshots[0].hasPresent, "first snapshot records present absence");
  checkEq(snapshots[0].commandBegin, 0u,
          "first snapshot records whole-source begin");
  checkEq(snapshots[0].commandCount, fixture.slots[0].commandCount(),
          "first snapshot records command count");
  checkEq(snapshots[1].slotIndex, 1u, "second snapshot records slot index");
  check(fixture.controller.resolveRepresentedSource(snapshots[1]).slot ==
            fixture.slots[1].payload,
        "second locator resolves live storage only under the represented pin");
  checkEq(snapshots[1].seqId, 2ull, "second snapshot records seqId");
  check(!snapshots[1].hasPresent, "second snapshot records present absence");
  checkEq(snapshots[1].commandBegin, 0u,
          "second snapshot records whole-source begin");
  checkEq(snapshots[1].commandCount, fixture.slots[1].commandCount(),
          "second snapshot records command count");
}

void dequeueReadySlotBatchRespectsOutputCapacity() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);
  fixture.addReadySlot(1, 2);
  fixture.addReadySlot(2, 3);

  std::array<ReadySlotSnapshot, 2> snapshots{};
  std::unique_lock lock(fixture.mutex);
  const std::size_t count =
      fixture.controller.dequeueReadySlotBatch(lock, std::span<ReadySlotSnapshot>(snapshots));

  checkEq(count, 2u, "batch dequeue is capped by caller storage");
  checkEq(fixture.cpuReadyTape.readyCount(), 1u,
          "capacity-limited batch leaves remaining ready slot");
  checkEq(fixture.readyControlIndex(), 2u,
          "remaining ready slot keeps FIFO order");
  check(fixture.slots[0].state == ChunkSlot::State::Encoding,
        "first capacity slot moves to Encoding");
  check(fixture.slots[1].state == ChunkSlot::State::Encoding,
        "second capacity slot moves to Encoding");
  check(fixture.slots[2].state == ChunkSlot::State::Pending,
        "overflow ready slot remains Pending");
}

void dequeueReadySlotBatchHonorsAppendPredicate() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);
  fixture.addReadySlot(1, 2);
  fixture.addReadySlot(2, 3);

  std::array<ReadySlotSnapshot, 3> snapshots{};
  std::unique_lock lock(fixture.mutex);
  const std::size_t count =
      fixture.controller.dequeueReadySlotBatch(
          lock,
          std::span<ReadySlotSnapshot>(snapshots),
          [](std::size_t selectedCount,
             const GenerationQualifiedSourceBorrow& candidate) {
            checkEq(selectedCount, 1u,
                    "predicate sees the already-selected source count");
            checkEq(candidate.slotIndex(), 1u,
                    "predicate sees the next FIFO candidate");
            return candidate.visitPayload(
                [](const SynchronousSourcePayloadBorrow& payload) {
                  check(payload.commandCount() == 0u,
                        "predicate sees the candidate payload view");
                  return false;
                });
          });

  checkEq(count, 1u, "batch predicate stops after the first source");
  checkEq(fixture.cpuReadyTape.readyCount(), 2u,
          "rejected candidates remain ready for later encode iterations");
  checkEq(fixture.readyControlIndex(), 1u,
          "first rejected candidate keeps FIFO position");
  check(fixture.slots[0].state == ChunkSlot::State::Encoding,
        "accepted source moves to Encoding");
  check(fixture.slots[1].state == ChunkSlot::State::Pending,
        "rejected candidate remains Pending");
  check(fixture.slots[2].state == ChunkSlot::State::Pending,
        "later candidate remains Pending");
}

void dequeueReadySlotBatchPrefixUsesCompleteSelectorCount() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);
  fixture.addReadySlot(1, 2);
  fixture.addReadySlot(2, 3);

  bool selectorCalled = false;
  std::array<ReadySlotSnapshot, 3> snapshots{};
  std::unique_lock lock(fixture.mutex);
  const std::size_t count =
      fixture.controller.dequeueReadySlotBatchPrefix(
          lock,
          std::span<ReadySlotSnapshot>(snapshots),
          [&](const SynchronousSourceBorrowBatch& candidates) {
            selectorCalled = true;
            checkEq(candidates.size(), 3u,
                    "selector sees the fixed candidate-span capacity");
            check(candidates.visitAt(0, [](const GenerationQualifiedSourceBorrow& candidate) {
                      return candidate.slotIndex() == 0u;
                    }),
                  "selector sees first FIFO source");
            check(candidates.visitAt(1, [](const GenerationQualifiedSourceBorrow& candidate) {
                      return candidate.slotIndex() == 1u;
                    }),
                  "selector sees second FIFO source");
            check(candidates.visitAt(2, [&](const GenerationQualifiedSourceBorrow& candidate) {
                      bool payloadMatches = false;
                      const bool payloadVisited = candidate.visitPayload(
                          [&](const SynchronousSourcePayloadBorrow& payload) {
                            payloadMatches = payload.isLegacy();
                          });
                      return candidate.slotIndex() == 2u &&
                             candidate.seqId() == 3ull && payloadVisited &&
                             payloadMatches;
                    }),
                  "selector sees third candidate metadata and payload");
            return 3u;
          });

  check(selectorCalled, "prefix selector is invoked");
  checkEq(count, 3u, "prefix selector controls dequeue count");
  check(fixture.cpuReadyTape.readyEmpty(),
        "complete prefix drains selected sources");
  check(fixture.slots[0].state == ChunkSlot::State::Encoding,
        "first prefix source moves to Encoding");
  check(fixture.slots[1].state == ChunkSlot::State::Encoding,
        "second prefix source moves to Encoding");
  check(fixture.slots[2].state == ChunkSlot::State::Encoding,
        "third prefix source moves to Encoding");
  checkEq(snapshots[2].slotIndex, 2u, "third snapshot records slot index");
  check(fixture.controller.resolveRepresentedSource(snapshots[2]).slot ==
            fixture.slots[2].payload,
        "third locator resolves live storage only under the represented pin");
  checkEq(snapshots[2].seqId, 3ull, "third snapshot records seqId");
  check(!snapshots[2].hasPresent, "third snapshot records present absence");
  checkEq(snapshots[2].commandBegin, 0u,
          "third snapshot records whole-source begin");
  checkEq(snapshots[2].commandCount, fixture.slots[2].commandCount(),
          "third snapshot records command count");
}

void dequeueReadySlotBatchPrefixFallsBackToSingleWhenSelectorRejects() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);
  fixture.addReadySlot(1, 2);
  fixture.addReadySlot(2, 3);

  std::array<ReadySlotSnapshot, 3> snapshots{};
  std::unique_lock lock(fixture.mutex);
  const std::size_t count =
      fixture.controller.dequeueReadySlotBatchPrefix(
          lock,
          std::span<ReadySlotSnapshot>(snapshots),
          [](const SynchronousSourceBorrowBatch&) { return 0u; });

  checkEq(count, 1u, "rejected prefix falls back to one source");
  checkEq(fixture.cpuReadyTape.readyCount(), 2u,
          "fallback leaves later ready sources pending");
  checkEq(fixture.readyControlIndex(), 1u,
          "fallback keeps the first rejected source FIFO-visible");
  check(fixture.slots[0].state == ChunkSlot::State::Encoding,
        "fallback source moves to Encoding");
  check(fixture.slots[1].state == ChunkSlot::State::Pending,
        "first rejected source remains Pending");
  check(fixture.slots[2].state == ChunkSlot::State::Pending,
        "later rejected source remains Pending");
  checkEq(snapshots[0].slotIndex, 0u, "fallback snapshot records first source");
  check(fixture.controller.resolveRepresentedSource(snapshots[0]).slot ==
            fixture.slots[0].payload,
        "fallback locator resolves live storage under the represented pin");
  checkEq(snapshots[0].seqId, 1ull, "fallback snapshot records first seq");
  check(!snapshots[0].hasPresent, "fallback snapshot records present absence");
  checkEq(snapshots[0].commandBegin, 0u,
          "fallback snapshot records whole-source begin");
  checkEq(snapshots[0].commandCount, fixture.slots[0].commandCount(),
          "fallback snapshot records command count");
}

void tentativeReadyPrefixKeepsControlsPendingAndSuffixReady() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);
  fixture.addReadySlot(1, 2);
  fixture.addReadySlot(2, 3);

  std::array<ReadySlotSnapshot, 3> snapshots{};
  std::unique_lock lock(fixture.mutex);
  checkEq(fixture.controller.reserveReadySlotBatchPrefix(
              lock, std::span<ReadySlotSnapshot>(snapshots),
              [](const SynchronousSourceBorrowBatch&) { return 0u; }),
          0u,
          "transactional selector rejection leaves the Ready FIFO intact");
  checkEq(fixture.cpuReadyTape.readyCount(), 3u,
          "zero-prefix selection does not consume a fallback source");

  const std::size_t count =
      fixture.controller.reserveReadySlotBatchPrefix(
          lock, std::span<ReadySlotSnapshot>(snapshots),
          [](const SynchronousSourceBorrowBatch& candidates) {
            checkEq(candidates.size(), 3u,
                    "tentative selector sees the bounded Ready prefix");
            return 2u;
          });
  checkEq(count, 2u, "two-source prefix is reserved tentatively");
  check(fixture.cpuReadyTape.state(
            snapshots[0].sourceId, snapshots[0].storage) ==
            CpuReadyTape::State::TentativeRepresented &&
            fixture.cpuReadyTape.state(
                snapshots[1].sourceId, snapshots[1].storage) ==
                CpuReadyTape::State::TentativeRepresented,
        "selected Tape sources enter tentative representation");
  check(fixture.slots[0].state == ChunkSlot::State::Pending &&
            fixture.slots[1].state == ChunkSlot::State::Pending,
        "tentative reservation does not expose Encoding controls");
  checkEq(fixture.cpuReadyTape.readyCount(), 1u,
          "unselected suffix remains Ready-visible");
  checkEq(fixture.readyControlIndex(), 2u,
          "unselected suffix retains FIFO head identity");
  check(fixture.controller.resolveTentativeSource(lock, snapshots[0]).valid() &&
            fixture.controller.resolveTentativeSource(lock, snapshots[1]).valid(),
        "pure preflight resolves exact tentative snapshots under the queue lock");

  check(fixture.controller.restoreReservedReadySlotBatch(
            lock,
            std::span<const ReadySlotSnapshot>(snapshots.data(), count)),
        "fixture restores tentative prefix after inspection");
}

void tentativeRestorePrecedesYoungerReadyPublication() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);
  fixture.addReadySlot(1, 2);
  fixture.addReadySlot(2, 3);

  std::array<ReadySlotSnapshot, 2> snapshots{};
  std::unique_lock lock(fixture.mutex);
  const std::size_t count =
      fixture.controller.reserveReadySlotBatchPrefix(
          lock, std::span<ReadySlotSnapshot>(snapshots),
          [](const SynchronousSourceBorrowBatch&) { return 2u; });
  checkEq(count, 2u, "fixture reserves the older prefix");

  lock.unlock();
  fixture.addReadySlot(3, 4);
  lock.lock();
  check(fixture.controller.restoreReservedReadySlotBatch(
            lock,
            std::span<const ReadySlotSnapshot>(snapshots.data(), count)),
        "queue restores an exact tentative prefix");
  std::array<CpuReadyTape::ReadyEntry, 4> ready{};
  checkEq(fixture.cpuReadyTape.copyReadyPrefix(ready), ready.size(),
          "restored FIFO contains old prefix, suffix, and younger publication");
  checkEq(ready[0].controlIndex, 0u, "first restored source is oldest");
  checkEq(ready[1].controlIndex, 1u, "second restored source follows");
  checkEq(ready[2].controlIndex, 2u, "old suffix remains after restored prefix");
  checkEq(ready[3].controlIndex, 3u, "younger publication remains last");
  check(fixture.slots[0].state == ChunkSlot::State::Pending &&
            fixture.slots[1].state == ChunkSlot::State::Pending &&
            fixture.slots[2].state == ChunkSlot::State::Pending &&
            fixture.slots[3].state == ChunkSlot::State::Pending,
        "restore leaves every Ready control Pending");
}

void tentativeCommitAloneMovesExactPrefixToEncoding() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1, false, {}, 0, 77);
  fixture.addReadySlot(1, 2, false, {}, 0, 78);

  // Mirror the production Arena admission/publication evidence before the
  // queue performs its real two-source reserved commit.  The commit itself
  // must then preserve one strict logical chain per source.
  for (std::size_t i = 0; i < 2; ++i) {
    const auto& slot = fixture.slots[i];
    const auto metadata = fixture.cpuReadyTape.sourceMetadata(
        slot.sourceId, slot.storage, CpuReadyTape::State::Ready);
    check(metadata.has_value(), "batch fixture exposes ready source metadata");
    const CpuReadyAdmissionIdentity identity{
        .rawOrdinal = 77u + i,
        .sourceOrdinal = i + 1u,
        .seqId = i + 1u,
        // Compatibility payloads do not carry the Arena build-generation
        // field, but the storage generation is still the authoritative
        // freshness witness at this observer boundary.
        .buildGeneration = slot.storage.generation,
    };
    fixture.controller.recordPipelineSourceArrival(
        CpuReadyTape::PayloadKind::Legacy,
        CpuReadyTape::SourceRef{.id = slot.sourceId, .storage = slot.storage},
        identity, metadata->usedBytes);
    fixture.controller.recordPipelineSourcePublication(
        CpuReadyTape::PayloadKind::Legacy,
        CpuReadyTape::SourceRef{.id = slot.sourceId, .storage = slot.storage},
        identity, metadata->usedBytes, 77u, static_cast<std::uint32_t>(i), 2u);
  }
  fixture.addReadySlot(2, 3);

  std::array<ReadySlotSnapshot, 2> snapshots{};
  std::unique_lock lock(fixture.mutex);
  const std::size_t count =
      fixture.controller.reserveReadySlotBatchPrefix(
          lock, std::span<ReadySlotSnapshot>(snapshots),
          [](const SynchronousSourceBorrowBatch&) { return 2u; });
  checkEq(count, 2u, "fixture reserves two tentative sources");
  check(fixture.slots[0].state == ChunkSlot::State::Pending &&
            fixture.slots[1].state == ChunkSlot::State::Pending,
        "pre-commit controls remain Pending");
  check(!fixture.controller.commitReservedReadySlotBatch(
            lock,
            std::span<const ReadySlotSnapshot>(snapshots.data(), 1u)),
        "commit rejects a proper subset of the reserved prefix");
  auto reordered = snapshots;
  std::swap(reordered[0], reordered[1]);
  check(!fixture.controller.commitReservedReadySlotBatch(
            lock, std::span<const ReadySlotSnapshot>(reordered)),
        "commit rejects reordered tentative identities");
  check(fixture.slots[0].state == ChunkSlot::State::Pending &&
            fixture.slots[1].state == ChunkSlot::State::Pending,
        "inexact commit attempts do not expose Encoding controls");
  check(fixture.controller.commitReservedReadySlotBatch(
            lock,
            std::span<const ReadySlotSnapshot>(snapshots.data(), count)),
        "exact tentative prefix commits");
  check(fixture.slots[0].state == ChunkSlot::State::Encoding &&
            fixture.slots[1].state == ChunkSlot::State::Encoding,
        "commit is the only operation that exposes Encoding controls");
  check(fixture.slots[2].state == ChunkSlot::State::Pending &&
            fixture.cpuReadyTape.readyCount() == 1u &&
            fixture.readyControlIndex() == 2u,
        "commit leaves the unselected suffix Pending and Ready-visible");
  check(fixture.controller.resolveRepresentedSource(snapshots[0]).valid() &&
            fixture.controller.resolveRepresentedSource(snapshots[1]).valid(),
        "committed snapshots resolve under represented lifetime");
  check(!fixture.controller.resolveTentativeSource(lock, snapshots[0]).valid(),
        "committed snapshot no longer resolves as tentative");
  const auto& ownerState = fixture.pipelineObserver.state();
  bool sawBatchMember[2] = {false, false};
  for (std::size_t i = 0; i < ownerState.ownerEventCount; ++i) {
    const auto& event = ownerState.ownerEvents[i];
    if (event.physicalBatchId != 0 && event.batchCount == 2 &&
        event.batchIndex < 2) {
      sawBatchMember[event.batchIndex] = true;
    }
  }
  check(sawBatchMember[0] && sawBatchMember[1],
        "production reserved commit retains both physical batch members");
  check(fixture.pipelineObserver.valid(),
        "production two-source commit has a valid lifecycle projection");
}

void tentativeQueueApisRejectUnlockedStaleAndTamperedSnapshots() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1, true, {}, 19, 11);

  std::array<ReadySlotSnapshot, 1> snapshots{};
  std::unique_lock lock(fixture.mutex);
  const std::size_t count =
      fixture.controller.reserveReadySlotBatchPrefix(
          lock, std::span<ReadySlotSnapshot>(snapshots),
          [](const SynchronousSourceBorrowBatch&) { return 1u; });
  checkEq(count, 1u, "fixture reserves one tentative source");

  ReadySlotSnapshot tampered = snapshots[0];
  ++tampered.metadata.usedBytes;
  check(!fixture.controller.resolveTentativeSource(lock, tampered).valid() &&
            !fixture.controller.commitReservedReadySlotBatch(
                lock, std::span<const ReadySlotSnapshot>(&tampered, 1)) &&
            !fixture.controller.restoreReservedReadySlotBatch(
                lock, std::span<const ReadySlotSnapshot>(&tampered, 1)),
        "metadata tampering cannot resolve, commit, or restore a tentative source");

  ReadySlotSnapshot stale = snapshots[0];
  ++stale.storage.generation;
  check(!fixture.controller.resolveTentativeSource(lock, stale).valid(),
        "storage generation mismatch is rejected before payload use");
  ReadySlotSnapshot changedRange = snapshots[0];
  changedRange.commandCount = 0;
  check(!fixture.controller.resolveTentativeSource(lock, changedRange).valid(),
        "a valid but non-identical command range is not the reserved snapshot");
  check(fixture.slots[0].state == ChunkSlot::State::Pending &&
            fixture.cpuReadyTape.state(
                snapshots[0].sourceId, snapshots[0].storage) ==
                CpuReadyTape::State::TentativeRepresented,
        "rejected snapshots leave the live tentative owner unchanged");

  lock.unlock();
  check(!fixture.controller.resolveTentativeSource(lock, snapshots[0]).valid() &&
            !fixture.controller.commitReservedReadySlotBatch(
                lock, std::span<const ReadySlotSnapshot>(snapshots)) &&
            !fixture.controller.restoreReservedReadySlotBatch(
                lock, std::span<const ReadySlotSnapshot>(snapshots)),
        "tentative queue operations require ownership of the bound queue lock");
  lock.lock();
  check(fixture.controller.restoreReservedReadySlotBatch(
            lock, std::span<const ReadySlotSnapshot>(snapshots)),
        "exact snapshot remains restorable after rejected attempts");
}

void dequeueCarriesGenerationCheckedAdmissionMetadata() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 7, true, {}, 23, 41);

  std::array<ReadySlotSnapshot, 1> snapshots{};
  std::unique_lock lock(fixture.mutex);
  const std::size_t count = fixture.controller.dequeueReadySlotBatch(
      lock, std::span<ReadySlotSnapshot>(snapshots));
  checkEq(count, 1u, "test setup dequeues one represented source");
  const auto& metadata = snapshots[0].metadata;
  checkEq(metadata.rawOrdinal, 41ull,
          "snapshot preserves raw admission order");
  checkEq(metadata.sourceOrdinal, 7ull,
          "snapshot preserves source admission order");
  checkEq(metadata.seqId, 7ull,
          "snapshot preserves publication sequence identity");
  checkEq(metadata.usedBytes, 23u,
          "snapshot preserves the sealed storage extent");
  checkEq(metadata.pageCount, fixture.slots[0].storage.pageCount,
          "snapshot preserves the admitted page extent");
  check(!metadata.strictAdmission,
        "compatibility publication remains distinguishable from strict admission");
  const auto& semantic = snapshots[0].semantic;
  check(semantic.valid() && semantic.entryStable() &&
            semantic.entryKind ==
                dxmt9::core::SourceEntryEncoderKind::Render &&
            semantic.firstBoundary ==
                dxmt9::core::SourceSemanticBoundaryKind::Clear &&
            semantic.firstBoundaryOrdinal == 0 &&
            semantic.commandCount == 1 && semantic.drawCount == 0 &&
            semantic.byteCount == metadata.usedBytes &&
            semantic.pageCount == metadata.pageCount,
        "snapshot carries the sealed source summary and exact extent");

  const auto resolved =
      fixture.controller.resolveRepresentedSource(snapshots[0]);
  check(resolved.valid() && resolved.metadata == metadata &&
            resolved.semantic == semantic,
        "represented resolution revalidates immutable metadata and semantics");

  ReadySlotSnapshot tampered = snapshots[0];
  ++tampered.metadata.usedBytes;
  check(!fixture.controller.resolveRepresentedSource(tampered).valid(),
        "metadata mutation is rejected against the live Tape generation");
  std::array<QueueCompletionSource, 1> retained{};
  checkEq(fixture.controller.retainEncodedSourcesForPendingTail(
              lock,
              std::span<const ReadySlotSnapshot>(&tampered, 1),
              std::span<QueueCompletionSource>(retained)),
          0u,
          "tampered metadata cannot be retained into completion ownership");
  check(fixture.slots[0].state == ChunkSlot::State::Encoding,
        "metadata rejection preserves the live encode owner");

  tampered = snapshots[0];
  ++tampered.semantic.commandCount;
  check(!fixture.controller.resolveRepresentedSource(tampered).valid() &&
            fixture.controller.retainEncodedSourcesForPendingTail(
                lock,
                std::span<const ReadySlotSnapshot>(&tampered, 1),
                std::span<QueueCompletionSource>(retained)) == 0u,
        "semantic mutation is rejected against the sealed Tape generation");
}

void completionSourceForReadySlotPreservesPresentMetadata() {
  ChunkSlot slot{};
  ReadySlotSnapshot snapshot{};
  snapshot.slotIndex = 3;
  slot.seqId = 7;
  slot.appendPresent(dxmt9::core::SwapDesc{}, dxmt9::core::Handle{0x77});
  snapshot.seqId = slot.seqId;
  snapshot.sourceId = CpuReadySourceId{.index = 2, .generation = 11};
  snapshot.storage = CpuReadyStorageRef{
      .firstPage = 3,
      .pageCount = 1,
      .generation = 22,
  };
  snapshot.hasPresent = true;
  snapshot.commandCount = slot.commandCount();

  const auto source = completionSourceForReadySlot(snapshot);

  checkEq(source.slotIndex, 3u, "completion source preserves slot index");
  checkEq(source.seqId, 7ull, "completion source preserves seqId");
  check(source.source.id == snapshot.sourceId &&
            source.source.storage == snapshot.storage,
        "completion source preserves source and storage generations");
  check(source.hasPresent, "completion source preserves present metadata");
  checkEq(source.commandBegin, 0u,
          "completion source preserves command begin metadata");
  checkEq(source.commandCount, slot.commandCount(),
          "completion source preserves command count metadata");
}

void completionSourceForReadySlotPreservesRangeMetadata() {
  ChunkSlot slot{};
  ReadySlotSnapshot snapshot{};
  snapshot.slotIndex = 4;
  slot.seqId = 8;
  slot.appendClear(dxmt9::core::ClearDesc{});
  slot.appendPresent(dxmt9::core::SwapDesc{}, dxmt9::core::Handle{0x88});
  snapshot.seqId = slot.seqId;
  snapshot.sourceId = CpuReadySourceId{.index = 3, .generation = 12};
  snapshot.storage = CpuReadyStorageRef{
      .firstPage = 4,
      .pageCount = 1,
      .generation = 23,
  };
  snapshot.hasPresent = false;
  snapshot.commandBegin = 0;
  snapshot.commandCount = 1;

  const auto source = completionSourceForReadySlot(snapshot);

  checkEq(source.slotIndex, 4u, "range source preserves slot index");
  checkEq(source.seqId, 8ull, "range source preserves seqId");
  check(!source.hasPresent, "range source preserves non-present metadata");
  checkEq(source.commandBegin, 0u, "range source preserves command begin");
  checkEq(source.commandCount, 1u, "range source preserves command count");
}

void retainEncodedSourcesRejectsPendingSources() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);

  ReadySlotSnapshot snapshot{};
  snapshot.slotIndex = 0;
  snapshot.seqId = fixture.slots[0].seqId;
  snapshot.hasPresent = false;
  snapshot.commandCount = fixture.slots[0].commandCount();
  snapshot.sourceId = fixture.slots[0].sourceId;
  std::array<QueueCompletionSource, 1> retained{};
  std::unique_lock lock(fixture.mutex);
  const std::size_t count = fixture.controller.retainEncodedSourcesForPendingTail(
      lock,
      std::span<const ReadySlotSnapshot>(&snapshot, 1),
      std::span<QueueCompletionSource>(retained));

  checkEq(count, 0u, "pending sources are not retained as encoded heads");
  checkEq(fixture.cpuReadyTape.readyCount(), 1u,
          "rejected pending source remains ready-visible");
  check(fixture.slots[0].state == ChunkSlot::State::Pending,
        "rejected pending source keeps Pending state");
}

void retainEncodedSourcesRejectsStaleSnapshotMetadata() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);
  fixture.slots[0].payload->appendClear({});

  std::array<ReadySlotSnapshot, 1> snapshots{};
  std::unique_lock lock(fixture.mutex);
  const std::size_t count =
      fixture.controller.dequeueReadySlotBatch(lock, std::span<ReadySlotSnapshot>(snapshots));
  checkEq(count, 1u, "test setup dequeues one source");

  ReadySlotSnapshot stale = snapshots[0];
  stale.commandBegin = 1;
  std::array<QueueCompletionSource, 1> retained{};
  const std::size_t retainedCount =
      fixture.controller.retainEncodedSourcesForPendingTail(
          lock,
          std::span<const ReadySlotSnapshot>(&stale, 1),
          std::span<QueueCompletionSource>(retained));

  checkEq(retainedCount, 0u, "stale command-begin metadata is rejected");
  check(fixture.slots[0].state == ChunkSlot::State::Encoding,
        "rejected stale source remains owned by the current encode");
}

void retainEncodedSourcesRejectsStaleSourceAndStorageLocators() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);
  fixture.slots[0].payload->appendClear(dxmt9::core::ClearDesc{});

  std::array<ReadySlotSnapshot, 1> snapshots{};
  std::unique_lock lock(fixture.mutex);
  const std::size_t count = fixture.controller.dequeueReadySlotBatch(
      lock, std::span<ReadySlotSnapshot>(snapshots));
  checkEq(count, 1u, "test setup dequeues one source");

  std::array<QueueCompletionSource, 1> retained{};
  ReadySlotSnapshot staleSource = snapshots[0];
  ++staleSource.sourceId.generation;
  checkEq(fixture.controller.retainEncodedSourcesForPendingTail(
              lock,
              std::span<const ReadySlotSnapshot>(&staleSource, 1),
              std::span<QueueCompletionSource>(retained)),
          0u,
          "source-generation mismatch rejects before payload use");

  ReadySlotSnapshot staleStorage = snapshots[0];
  ++staleStorage.storage.generation;
  checkEq(fixture.controller.retainEncodedSourcesForPendingTail(
              lock,
              std::span<const ReadySlotSnapshot>(&staleStorage, 1),
              std::span<QueueCompletionSource>(retained)),
          0u,
          "page-generation mismatch rejects before payload use");
  check(fixture.slots[0].state == ChunkSlot::State::Encoding,
        "stale locator rejection preserves live source ownership");
}

void retainEncodedSourcesAcceptsPartialRangeMetadata() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);
  fixture.slots[0].payload->appendClear(dxmt9::core::ClearDesc{});
  fixture.slots[0].payload->appendPresent(dxmt9::core::SwapDesc{},
                                          dxmt9::core::Handle{0x99});

  std::array<ReadySlotSnapshot, 1> snapshots{};
  std::unique_lock lock(fixture.mutex);
  const std::size_t count =
      fixture.controller.dequeueReadySlotBatch(lock, std::span<ReadySlotSnapshot>(snapshots));
  checkEq(count, 1u, "test setup dequeues one source");

  ReadySlotSnapshot head = snapshots[0];
  head.hasPresent = false;
  head.commandBegin = 0;
  head.commandCount = 1;
  std::array<QueueCompletionSource, 1> retained{};
  const std::size_t retainedCount =
      fixture.controller.retainEncodedSourcesForPendingTail(
          lock,
          std::span<const ReadySlotSnapshot>(&head, 1),
          std::span<QueueCompletionSource>(retained));

  checkEq(retainedCount, 1u, "partial non-present range is retained");
  checkEq(retained[0].commandBegin, 0u,
          "retained partial range preserves command begin");
  checkEq(retained[0].commandCount, 1u,
          "retained partial range preserves command count");
  check(!retained[0].hasPresent,
        "retained partial range preserves hasPresent=false");
}

void retainEncodedSourcesAcceptsSelectedPrefixMetadata() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);
  fixture.addReadySlot(1, 2);
  fixture.addReadySlot(2, 3);
  fixture.slots[0].payload->appendClear(dxmt9::core::ClearDesc{});
  fixture.slots[1].payload->appendClear(dxmt9::core::ClearDesc{});
  fixture.slots[2].payload->appendPresent(dxmt9::core::SwapDesc{},
                                          dxmt9::core::Handle{0xA3});

  std::array<ReadySlotSnapshot, 3> snapshots{};
  std::unique_lock lock(fixture.mutex);
  const std::size_t sourceCount =
      fixture.controller.dequeueReadySlotBatchPrefix(
          lock,
          std::span<ReadySlotSnapshot>(snapshots),
          [](const SynchronousSourceBorrowBatch&) { return 3u; });
  checkEq(sourceCount, 3u, "test setup selects the whole source prefix");

  std::array<QueueCompletionSource, 3> retained{};
  const std::size_t retainedCount =
      fixture.controller.retainEncodedSourcesForPendingTail(
          lock,
          std::span<const ReadySlotSnapshot>(snapshots.data(), sourceCount),
          std::span<QueueCompletionSource>(retained));

  checkEq(retainedCount, sourceCount,
          "selected source prefix is retained as compact metadata");
  checkEq(retained[0].seqId, 1ull, "first prefix source keeps seqId");
  checkEq(retained[1].seqId, 2ull, "middle prefix source keeps seqId");
  checkEq(retained[2].seqId, 3ull, "tail prefix source keeps seqId");
  check(!retained[0].hasPresent, "first prefix source is non-present");
  check(!retained[1].hasPresent, "middle prefix source is non-present");
  check(retained[2].hasPresent, "tail prefix source preserves present flag");
  check(fixture.slots[0].state == ChunkSlot::State::Encoding,
        "first retained prefix source remains encode-owned");
  check(fixture.slots[1].state == ChunkSlot::State::Encoding,
        "middle retained prefix source remains encode-owned");
  check(fixture.slots[2].state == ChunkSlot::State::Encoding,
        "tail retained prefix source remains encode-owned");
}

void retainedEncodedHeadCompletesOnlyWithTailCarrier() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);

  std::array<ReadySlotSnapshot, 1> head{};
  std::unique_lock lock(fixture.mutex);
  const std::size_t headCount =
      fixture.controller.dequeueReadySlotBatch(lock, std::span<ReadySlotSnapshot>(head));
  checkEq(headCount, 1u, "test setup dequeues one head source");
  check(fixture.cpuReadyTape.readyEmpty(),
        "encoded head is no longer ready-visible");
  check(fixture.slots[0].state == ChunkSlot::State::Encoding,
        "head source enters Encoding before retention");

  std::array<QueueCompletionSource, 1> retainedHeads{};
  const std::size_t retainedCount = fixture.controller.retainEncodedSourcesForPendingTail(
      lock,
      std::span<const ReadySlotSnapshot>(head.data(), headCount),
      std::span<QueueCompletionSource>(retainedHeads));
  checkEq(retainedCount, 1u, "encoded head is retained for the pending tail");
  checkEq(retainedHeads[0].slotIndex, 0u, "retained head keeps slot index");
  checkEq(retainedHeads[0].seqId, 1ull, "retained head keeps seqId");
  check(!retainedHeads[0].hasPresent, "retained head is not a present source");
  checkEq(retainedHeads[0].commandBegin, head[0].commandBegin,
          "retained head keeps command begin metadata");
  checkEq(retainedHeads[0].commandCount, head[0].commandCount,
          "retained head keeps command count metadata");
  check(fixture.slots[0].state == ChunkSlot::State::Encoding,
        "retaining head does not make it GPU-visible");

  fixture.addReadySlot(1, 2);
  fixture.slots[1].payload->appendPresent(dxmt9::core::SwapDesc{},
                                          dxmt9::core::Handle{0xA1});
  std::array<ReadySlotSnapshot, 1> tail{};
  const std::size_t tailCount =
      fixture.controller.dequeueReadySlotBatch(lock, std::span<ReadySlotSnapshot>(tail));
  checkEq(tailCount, 1u, "test setup dequeues the present tail");
  check(tail[0].hasPresent,
        "tail locator carries present metadata without a payload pointer");

  QueueSubmissionRecord record;
  record.testOnlyAllowNullCommandBuffer = true;
  record.slotIndex = tail[0].slotIndex;
  record.seqId = tail[0].seqId;
  const std::array<QueueCompletionSource, 2> recordSources{{
      retainedHeads[0],
      completionSourceForReadySlot(tail[0]),
  }};
  check(record.assignFixedCompletionSources(
            std::span<const QueueCompletionSource>(
                recordSources.data(), recordSources.size())),
        "tail carrier stores retained sources in fixed completion metadata");

  check(fixture.controller.submitEncodedSubmission(lock, record),
        "tail-carrier submission succeeds");
  check(fixture.slots[0].state == ChunkSlot::State::Free &&
            fixture.slots[1].state == ChunkSlot::State::Free,
        "tail-carrier submit releases every temporary control shell");
  check(fixture.cpuReadyTape.state(recordSources[0].source.id,
                                   recordSources[0].source.storage) ==
            CpuReadyTape::State::Submitted &&
            fixture.cpuReadyTape.state(recordSources[1].source.id,
                                       recordSources[1].source.storage) ==
                CpuReadyTape::State::Submitted,
        "tail-carrier sources remain Tape-owned while submitted");
  check(fixture.completedSeqQueue.empty(),
        "carrier submission alone does not mark sources completed");

  check(fixture.cpuReadyTape.complete(recordSources[0].source.id,
                                     recordSources[0].source.storage) &&
            fixture.cpuReadyTape.complete(recordSources[1].source.id,
                                          recordSources[1].source.storage),
        "tail carrier completion marks every represented source completed");
  appendCompletionSourcesToQueues(
      fixture.completedSeqQueue,
      &fixture.completedPresentSeqQueue,
      fixture.completedSeqId.load(),
      record.explicitCompletionSourceSpan());

  std::uint64_t finishedSeq = 0;
  const bool finishedHead = fixture.controller.runFinishIteration(
      lock, [&](std::uint64_t seqId) { finishedSeq = seqId; });
  check(finishedHead, "head completion drains first");
  checkEq(finishedSeq, 1ull, "head seq completes first");
  check(fixture.slots[0].state == ChunkSlot::State::Free,
        "head is freed only after tail-carrier completion");
  check(fixture.cpuReadyTape.state(recordSources[1].source.id,
                                   recordSources[1].source.storage) ==
            CpuReadyTape::State::Completed,
        "tail remains Tape-resident until its own completion drains");
  checkEq(fixture.presentCompletedSeqId, 0ull,
          "present completion waits for the present tail seq");

  const bool finishedTail = fixture.controller.runFinishIteration(
      lock, [&](std::uint64_t seqId) { finishedSeq = seqId; });
  check(finishedTail, "tail completion drains second");
  checkEq(finishedSeq, 2ull, "tail seq completes second");
  check(fixture.slots[1].state == ChunkSlot::State::Free,
        "tail is freed after its completion drains");
  checkEq(fixture.completedSeqId.load(), 2ull, "completed seq advances through tail");
  checkEq(fixture.presentCompletedSeqId, 2ull,
          "present completion advances at the tail seq");
}

void pendingCompletionWatcherExpandsSessionSourcesInOrder() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);
  fixture.addReadySlot(1, 2);
  fixture.addReadySlot(2, 3);
  fixture.slots[2].payload->appendPresent(dxmt9::core::SwapDesc{},
                                          dxmt9::core::Handle{0xA2});

  std::array<ReadySlotSnapshot, 3> sources{};
  QueueSubmissionRecord record;
  {
    std::unique_lock lock(fixture.mutex);
    const std::size_t sourceCount =
        fixture.controller.dequeueReadySlotBatch(
            lock, std::span<ReadySlotSnapshot>(sources));
    checkEq(sourceCount, 3u, "test setup dequeues every source");
    record.slotIndex = sources[2].slotIndex;
    record.testOnlyAllowNullCommandBuffer = true;
    check(sources[2].hasPresent,
          "tail source keeps locator-only present metadata");
    record.seqId = sources[2].seqId;
    EncodeSessionSourceList recordSources;
    for (const auto& source : sources) {
      check(recordSources.append(completionSourceForReadySlot(source)),
            "test setup builds fixed completion source metadata");
    }
    check(record.assignFixedCompletionSources(recordSources.span()),
          "session submission stores fixed completion source metadata");

    check(fixture.controller.submitEncodedSubmission(lock, record),
          "session submission succeeds");
    check(fixture.completedSeqQueue.empty(),
          "GPU submission alone does not complete any source");
    for (std::size_t i = 0; i < sources.size(); ++i) {
      check(fixture.slots[i].state == ChunkSlot::State::Free,
            "every session submit releases its temporary control");
      check(fixture.cpuReadyTape.state(
                recordSources.entries[i].source.id,
                recordSources.entries[i].source.storage) ==
                CpuReadyTape::State::Submitted,
            "every session source remains submitted in Tape storage");
    }
  }

  bool completionCallbackRan = false;
  QueueLifecycleController::PendingCompletion pending;
  pending.slotIndex = record.slotIndex;
  pending.seqId = record.seqId;
  pending.diagnostics.hasDraw = true;
  pending.diagnostics.hasPresent = true;
  pending.contextValue = "queue-completion-sources-spec";
  pending.fixedCompletionSources = record.fixedCompletionSources;
  pending.completionSpanShadow = record.completionSpanShadow;
  pending.completionCallbacks.push_back(
      [&completionCallbackRan] { completionCallbackRan = true; });
  fixture.controller.enqueuePendingCompletionForTest(std::move(pending));

  const bool processed = fixture.controller.processOnePendingCompletion();
  check(processed, "pending completion watcher processes injected record");
  check(completionCallbackRan,
        "pending completion watcher runs completion callbacks before queueing");

  std::unique_lock lock(fixture.mutex);
  checkEq(fixture.completedSeqQueue.size(), 3u,
          "watcher expands every session source into completed queue");
  checkEq(fixture.completedSeqQueue[0], 1ull,
          "watcher queues first source completion first");
  checkEq(fixture.completedSeqQueue[1], 2ull,
          "watcher queues second source completion second");
  checkEq(fixture.completedSeqQueue[2], 3ull,
          "watcher queues tail source completion last");
  checkEq(fixture.completedPresentSeqQueue.size(), 1u,
          "watcher queues only the present-bearing tail for present waiters");
  checkEq(fixture.completedPresentSeqQueue.front(), 3ull,
          "present completion is tied to tail seqId");

  std::uint64_t finishedSeq = 0;
  check(fixture.controller.runFinishIteration(
            lock, [&](std::uint64_t seqId) { finishedSeq = seqId; }),
        "first watcher-produced completion drains");
  checkEq(finishedSeq, 1ull, "first watcher completion finishes source 1");
  checkEq(fixture.presentCompletedSeqId, 0ull,
          "present completion does not advance on source 1");
  check(fixture.slots[0].state == ChunkSlot::State::Free,
        "source 1 is reclaimed after its finish");
  checkEq(fixture.cpuReadyTape.residentCount(), 2u,
          "later completed sources wait for ordered Tape reclaim");

  check(fixture.controller.runFinishIteration(
            lock, [&](std::uint64_t seqId) { finishedSeq = seqId; }),
        "second watcher-produced completion drains");
  checkEq(finishedSeq, 2ull, "second watcher completion finishes source 2");
  checkEq(fixture.presentCompletedSeqId, 0ull,
          "present completion still waits for tail source");
  check(fixture.slots[1].state == ChunkSlot::State::Free,
        "source 2 is reclaimed after its finish");
  checkEq(fixture.cpuReadyTape.residentCount(), 1u,
          "tail source remains Tape-resident before tail finish");

  check(fixture.controller.runFinishIteration(
            lock, [&](std::uint64_t seqId) { finishedSeq = seqId; }),
        "tail watcher-produced completion drains");
  checkEq(finishedSeq, 3ull, "tail watcher completion finishes source 3");
  checkEq(fixture.completedSeqId.load(), 3ull,
          "completed seq advances through the full session");
  checkEq(fixture.presentCompletedSeqId, 3ull,
          "present completion advances only at the session tail");
  check(fixture.slots[2].state == ChunkSlot::State::Free,
        "tail source is reclaimed after tail finish");
}

void multiSourceSubmitRejectsStaleTailAtomically() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);
  fixture.addReadySlot(1, 2);

  std::array<ReadySlotSnapshot, 2> sources{};
  std::unique_lock lock(fixture.mutex);
  checkEq(fixture.controller.dequeueReadySlotBatch(lock, sources), 2u,
          "submit atomicity fixture dequeues both sources");
  QueueSubmissionRecord record;
  record.testOnlyAllowNullCommandBuffer = true;
  record.slotIndex = sources[1].slotIndex;
  record.seqId = sources[1].seqId;
  const std::array completionSources{
      completionSourceForReadySlot(sources[0]),
      completionSourceForReadySlot(sources[1]),
  };
  check(record.assignFixedCompletionSources(completionSources),
        "submit atomicity fixture stores both completion sources");

  const CpuReadyStorageRef secondLiveStorage = fixture.slots[1].storage;
  ++fixture.slots[1].storage.generation;
  check(!fixture.controller.submitEncodedSubmission(lock, record),
        "stale tail submission reports failure");

  check(fixture.stop, "stale tail submission fail-stops the queue");
  check(fixture.slots[0].state == ChunkSlot::State::Encoding &&
            fixture.slots[1].state == ChunkSlot::State::Encoding,
        "stale tail leaves every control source unsubmitted");
  check(fixture.cpuReadyTape.state(sources[0].sourceId, sources[0].storage) ==
            CpuReadyTape::State::Encoding &&
            fixture.cpuReadyTape.state(sources[1].sourceId,
                                       secondLiveStorage) ==
                CpuReadyTape::State::Encoding,
        "stale tail leaves every tape source in the preflight state");
  check(fixture.completedSeqQueue.empty(),
        "failed multi-source submit publishes no completion prefix");
}

void submitRejectsWrongRangeAndPresentMetadataAtomically() {
  auto run = [](std::string_view label,
                const std::function<void(QueueCompletionSource&)>& corrupt) {
    QueueFixture fixture;
    fixture.addReadySlot(0, 1, true);
    fixture.addReadySlot(1, 2, true);

    std::array<ReadySlotSnapshot, 2> sources{};
    std::unique_lock lock(fixture.mutex);
    checkEq(fixture.controller.dequeueReadySlotBatch(lock, sources), 2u,
            "metadata preflight fixture represents both sources");
    std::array completionSources{
        completionSourceForReadySlot(sources[0]),
        completionSourceForReadySlot(sources[1]),
    };
    corrupt(completionSources[1]);
    QueueSubmissionRecord record;
    record.testOnlyAllowNullCommandBuffer = true;
    record.slotIndex = sources[1].slotIndex;
    record.seqId = sources[1].seqId;
    check(record.assignFixedCompletionSources(completionSources),
          "corrupt range metadata remains structurally assignable");

    check(!fixture.controller.submitEncodedSubmission(lock, record),
          "corrupt range metadata reports submission failure");
    check(fixture.stop, label);
    check(fixture.slots[0].state == ChunkSlot::State::Encoding &&
              fixture.slots[1].state == ChunkSlot::State::Encoding,
          "metadata rejection frees no temporary controls");
    check(fixture.cpuReadyTape.state(sources[0].sourceId,
                                     sources[0].storage) ==
              CpuReadyTape::State::Represented &&
              fixture.cpuReadyTape.state(sources[1].sourceId,
                                         sources[1].storage) ==
                  CpuReadyTape::State::Represented,
          "metadata rejection submits no Tape prefix");
  };

  run("out-of-range command begin fail-stops before submit", [](auto& source) {
    source.commandBegin = 2;
    source.commandCount = 0;
  });
  run("out-of-range command count fail-stops before submit", [](auto& source) {
    source.commandCount = 2;
  });
  run("incorrect present metadata fail-stops before submit", [](auto& source) {
    source.hasPresent = true;
  });
}

void appendPresentRejectsStaleWriterBeforeDereference() {
  QueueFixture fixture;
  const auto reservation = fixture.cpuReadyTape.reserve();
  check(reservation.has_value(), "present stale fixture reserves writer");
  fixture.writingSlot = 0;
  fixture.slots[0].state = ChunkSlot::State::Writing;
  fixture.slots[0].sourceId = reservation->id;
  fixture.slots[0].storage = reservation->storage;
  fixture.slots[0].payload = reservation->payload;
  ++fixture.slots[0].storage.generation;

  check(!fixture.controller.appendPresentCommand(
            dxmt9::core::SwapDesc{}, dxmt9::core::Handle{0xA3}),
        "stale writer locator rejects present append");
  check(fixture.stop,
        "stale writer locator fail-stops the queue before dereference");
  check(reservation->payload->presentRecords.empty(),
        "stale present append leaves payload untouched");
  check(fixture.cpuReadyTape.probeReserve() ==
            CpuReadyTape::ReserveProbe::Stopped,
        "queue poison consistently stops tape admission");
}

void multiSourceCompletionRejectsStaleTailAtomically() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);
  fixture.addReadySlot(1, 2);

  std::array<ReadySlotSnapshot, 2> sources{};
  QueueSubmissionRecord record;
  record.testOnlyAllowNullCommandBuffer = true;
  {
    std::unique_lock lock(fixture.mutex);
    checkEq(fixture.controller.dequeueReadySlotBatch(lock, sources), 2u,
            "completion atomicity fixture dequeues both sources");
    record.slotIndex = sources[1].slotIndex;
    record.seqId = sources[1].seqId;
    const std::array completionSources{
        completionSourceForReadySlot(sources[0]),
        completionSourceForReadySlot(sources[1]),
    };
    check(record.assignFixedCompletionSources(completionSources),
          "completion atomicity fixture stores both sources");
    check(fixture.controller.submitEncodedSubmission(lock, record),
          "completion atomicity fixture submits valid sources");
    check(fixture.slots[0].state == ChunkSlot::State::Free &&
              fixture.slots[1].state == ChunkSlot::State::Free,
          "completion atomicity fixture frees both controls at submit");
  }

  QueueLifecycleController::PendingCompletion pending;
  pending.slotIndex = record.slotIndex;
  pending.seqId = record.seqId;
  pending.fixedCompletionSources = record.fixedCompletionSources;
  pending.completionSpanShadow = record.completionSpanShadow;
  ++pending.fixedCompletionSources.entries[1].source.storage.generation;
  fixture.controller.enqueuePendingCompletionForTest(std::move(pending));

  check(!fixture.controller.processOnePendingCompletion(),
        "stale tail completion is rejected");
  check(fixture.stop, "stale tail completion fail-stops the queue");
  check(fixture.completedSeqQueue.empty() &&
            fixture.completedPresentSeqQueue.empty(),
        "stale tail completion appends no partial queue prefix");
  check(fixture.slots[0].state == ChunkSlot::State::Free &&
            fixture.slots[1].state == ChunkSlot::State::Free,
        "stale tail does not recreate or mutate submitted controls");
  check(fixture.cpuReadyTape.state(sources[0].sourceId, sources[0].storage) ==
            CpuReadyTape::State::Submitted &&
            fixture.cpuReadyTape.state(sources[1].sourceId,
                                       sources[1].storage) ==
                CpuReadyTape::State::Submitted,
        "stale tail leaves every tape source uncompleted");
}

void recycledControlIsNeverCompletionOrReclaimIdentity() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);

  ReadySlotSnapshot sourceA{};
  QueueSubmissionRecord recordA;
  recordA.testOnlyAllowNullCommandBuffer = true;
  {
    std::unique_lock lock(fixture.mutex);
    check(fixture.controller.dequeueReadySlot(lock, sourceA),
          "source A represents before submit");
    recordA.slotIndex = sourceA.slotIndex;
    recordA.seqId = sourceA.seqId;
    const std::array completionSources{
        completionSourceForReadySlot(sourceA),
    };
    check(recordA.assignFixedCompletionSources(completionSources),
          "source A stores stable completion identity");
    fixture.controller.submitEncodedSubmission(lock, recordA);
    check(fixture.slots[0].state == ChunkSlot::State::Free &&
              fixture.cpuReadyTape.state(sourceA.sourceId, sourceA.storage) ==
                  CpuReadyTape::State::Submitted,
          "source A submit frees its control but keeps Tape residency");
  }

  fixture.addReadySlot(0, 2);
  const CpuReadySourceId sourceBId = fixture.slots[0].sourceId;
  const CpuReadyStorageRef sourceBStorage = fixture.slots[0].storage;
  ChunkSlot* const sourceBPayload = fixture.slots[0].payload;
  check(sourceBId != sourceA.sourceId &&
            fixture.slots[0].state == ChunkSlot::State::Pending,
        "freed control is reused by source B before A completes");

  QueueLifecycleController::PendingCompletion completionA;
  completionA.slotIndex = recordA.slotIndex;
  completionA.seqId = recordA.seqId;
  completionA.fixedCompletionSources = recordA.fixedCompletionSources;
  completionA.completionSpanShadow = recordA.completionSpanShadow;
  fixture.controller.enqueuePendingCompletionForTest(std::move(completionA));
  check(fixture.controller.processOnePendingCompletion(),
        "source A completion resolves its recorded Tape locator");
  check(fixture.slots[0].sourceId == sourceBId &&
            fixture.slots[0].storage == sourceBStorage &&
            fixture.slots[0].payload == sourceBPayload &&
            fixture.slots[0].state == ChunkSlot::State::Pending,
        "source A completion does not touch reused source B control");

  {
    std::unique_lock lock(fixture.mutex);
    check(fixture.controller.runFinishIteration(lock),
          "source A reclaims from the completed Tape FIFO head");
  }
  check(fixture.slots[0].sourceId == sourceBId &&
            fixture.slots[0].storage == sourceBStorage &&
            fixture.slots[0].payload == sourceBPayload &&
            fixture.cpuReadyTape.state(sourceBId, sourceBStorage) ==
                CpuReadyTape::State::Ready,
        "source A reclaim does not scan or mutate source B control");

  QueueLifecycleController::PendingCompletion duplicateA;
  duplicateA.slotIndex = recordA.slotIndex;
  duplicateA.seqId = recordA.seqId;
  duplicateA.fixedCompletionSources = recordA.fixedCompletionSources;
  duplicateA.completionSpanShadow = recordA.completionSpanShadow;
  fixture.controller.enqueuePendingCompletionForTest(std::move(duplicateA));
  check(!fixture.controller.processOnePendingCompletion(),
        "duplicate source A callback is stale after ordered reclaim");
  check(fixture.slots[0].sourceId == sourceBId &&
            fixture.slots[0].storage == sourceBStorage &&
            fixture.slots[0].payload == sourceBPayload &&
            fixture.slots[0].state == ChunkSlot::State::Pending,
        "duplicate callback cannot mutate the reused source");
}

void completionProjectionCorruptionRejectsBeforeCallbacks() {
  const auto run = [](std::string_view label, auto corrupt) {
    QueueFixture fixture;
    fixture.addReadySlot(0, 1);
    ReadySlotSnapshot source{};
    QueueSubmissionRecord record;
    record.testOnlyAllowNullCommandBuffer = true;
    {
      std::unique_lock lock(fixture.mutex);
      check(fixture.controller.dequeueReadySlot(lock, source),
            "projection-corruption fixture represents source");
      record.slotIndex = source.slotIndex;
      record.seqId = source.seqId;
      const std::array completionSources{
          completionSourceForReadySlot(source),
      };
      check(record.assignFixedCompletionSources(completionSources),
            "projection-corruption fixture stores source locator");
      check(fixture.controller.submitEncodedSubmission(lock, record),
            "projection-corruption fixture submits its valid source");
    }

    QueueLifecycleController::PendingCompletion pending;
    pending.slotIndex = record.slotIndex;
    pending.seqId = record.seqId;
    pending.fixedCompletionSources = record.fixedCompletionSources;
    pending.completionSpanShadow = record.completionSpanShadow;
    corrupt(pending);
    bool completionCallbackRan = false;
    pending.completionCallbacks.push_back(
        [&completionCallbackRan] { completionCallbackRan = true; });
    fixture.controller.enqueuePendingCompletionForTest(std::move(pending));

    check(!fixture.controller.processOnePendingCompletion(),
          label);
    check(!completionCallbackRan,
          "projection mismatch rejects before completion callbacks run");
    check(fixture.stop &&
              fixture.cpuReadyTape.state(source.sourceId, source.storage) ==
              CpuReadyTape::State::Submitted &&
              fixture.completedSeqQueue.empty(),
          "projection mismatch fail-stops before Tape or queue mutation");
  };

  run("seq projection corruption is rejected", [](auto& pending) {
    ++pending.fixedCompletionSources.entries[0].seqId;
  });
  run("count projection corruption is rejected", [](auto& pending) {
    pending.fixedCompletionSources.count = 0;
  });
  run("Present projection corruption is rejected", [](auto& pending) {
    pending.fixedCompletionSources.entries[0].hasPresent = true;
  });
}

void invalidCompletionLocatorRetainsLegacyCallbackBeforeTapeFailure() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);
  ReadySlotSnapshot source{};
  QueueSubmissionRecord record;
  record.testOnlyAllowNullCommandBuffer = true;
  {
    std::unique_lock lock(fixture.mutex);
    check(fixture.controller.dequeueReadySlot(lock, source),
          "invalid-locator fixture represents source");
    record.slotIndex = source.slotIndex;
    record.seqId = source.seqId;
    const std::array completionSources{
        completionSourceForReadySlot(source),
    };
    check(record.assignFixedCompletionSources(completionSources),
          "invalid-locator fixture seals a valid old list");
    check(fixture.controller.submitEncodedSubmission(lock, record),
          "invalid-locator fixture submits its valid source");
  }

  QueueLifecycleController::PendingCompletion pending;
  pending.slotIndex = record.slotIndex;
  pending.seqId = record.seqId;
  pending.fixedCompletionSources = record.fixedCompletionSources;
  pending.completionSpanShadow = record.completionSpanShadow;
  pending.fixedCompletionSources.entries[0].source = {};
  check(pending.completionSpanShadowMatchesSources(),
        "syntactically invalid locator is not a projection mismatch");
  bool completionCallbackRan = false;
  pending.completionCallbacks.push_back(
      [&completionCallbackRan] { completionCallbackRan = true; });
  fixture.controller.enqueuePendingCompletionForTest(std::move(pending));

  check(!fixture.controller.processOnePendingCompletion(),
        "old-list validation rejects the invalid locator");
  check(completionCallbackRan,
        "legacy completion callback runs before old-list locator validation");
  check(fixture.stop &&
            fixture.cpuReadyTape.state(source.sourceId, source.storage) ==
                CpuReadyTape::State::Submitted &&
            fixture.completedSeqQueue.empty(),
        "old-list locator failure poisons without completing Tape or queue");
}

void staleReceiptRejectsBeforeCallbacksAndWaterlines() {
  QueueFixture fixture;
  const QueueCompletionSource staleReceiptSource{
      .receipt = {
          .seqId = 1u,
          .generation = 1u,
          .slot = 0u,
      },
      .slotIndex = 0u,
      .seqId = 1u,
      .hasPresent = false,
  };
  QueueLifecycleController::PendingCompletion pending;
  pending.slotIndex = 0u;
  pending.seqId = 1u;
  check(pending.assignFixedCompletionSources(
            std::span<const QueueCompletionSource>(&staleReceiptSource, 1u)),
        "stale-receipt fixture seals a structurally valid receipt identity");
  bool completionCallbackRan = false;
  pending.completionCallbacks.push_back(
      [&completionCallbackRan] { completionCallbackRan = true; });
  fixture.controller.enqueuePendingCompletionForTest(std::move(pending));

  check(!fixture.controller.processOnePendingCompletion(),
        "unowned receipt generation is rejected as stale");
  check(!completionCallbackRan && fixture.stop &&
            fixture.completedSeqQueue.empty() &&
            fixture.completedPresentSeqQueue.empty(),
        "stale receipt fails before callbacks and completion waterlines");
}

void emptyCompletionSourcesAndNullCommandBufferFailBeforeMutation() {
  {
    QueueFixture fixture;
    fixture.addReadySlot(0, 1);
    ReadySlotSnapshot source{};
    std::unique_lock lock(fixture.mutex);
    check(fixture.controller.dequeueReadySlot(lock, source),
          "empty-list fixture represents source");
    QueueSubmissionRecord record;
    record.testOnlyAllowNullCommandBuffer = true;
    record.slotIndex = source.slotIndex;
    record.seqId = source.seqId;
    check(!fixture.controller.submitEncodedSubmission(lock, record),
          "empty completion list reports submission failure");
    check(fixture.stop &&
              fixture.slots[0].state == ChunkSlot::State::Encoding &&
              fixture.cpuReadyTape.state(source.sourceId, source.storage) ==
                  CpuReadyTape::State::Represented,
          "empty completion list fails before Tape or control transition");
  }

  {
    QueueFixture fixture;
    fixture.addReadySlot(0, 1);
    ReadySlotSnapshot source{};
    std::unique_lock lock(fixture.mutex);
    check(fixture.controller.dequeueReadySlot(lock, source),
          "null-command-buffer fixture represents source");
    QueueSubmissionRecord record;
    record.slotIndex = source.slotIndex;
    record.seqId = source.seqId;
    const std::array completionSources{
        completionSourceForReadySlot(source),
    };
    check(record.assignFixedCompletionSources(completionSources),
          "null-command-buffer fixture stores valid sources");
    check(!fixture.controller.submitEncodedSubmission(lock, record),
          "null command buffer reports submission failure");
    check(fixture.stop &&
              fixture.slots[0].state == ChunkSlot::State::Encoding &&
              fixture.cpuReadyTape.state(source.sourceId, source.storage) ==
                  CpuReadyTape::State::Represented,
          "null command buffer fails before Tape or control transition");
  }
}

QueueSubmissionRecord::RenderEncoderGpuSample makeGpuSample(
    std::uint32_t commandIndex,
    std::uint64_t seqId) {
  return QueueSubmissionRecord::RenderEncoderGpuSample{
      .startIndex = commandIndex * 2u,
      .endIndex = commandIndex * 2u + 1u,
      .seqId = seqId,
      .slotIndex = commandIndex,
      .commandIndex = commandIndex,
      .sourceCommand = {
          .seqId = seqId,
          .commandIndex = commandIndex,
      },
  };
}

void redundantFixedCompletionSourcePredicateIsExact() {
  const std::array<QueueCompletionSource, 3> sources{{
      QueueCompletionSource{
          .source = testSource(3, 31),
          .slotIndex = 3,
          .seqId = 31,
          .hasPresent = false,
          .commandBegin = 0,
          .commandCount = 2,
      },
      QueueCompletionSource{
          .source = testSource(4, 32),
          .slotIndex = 4,
          .seqId = 32,
          .hasPresent = false,
          .commandBegin = 1,
          .commandCount = 3,
      },
      QueueCompletionSource{
          .source = testSource(5, 33),
          .slotIndex = 5,
          .seqId = 33,
          .hasPresent = false,
          .commandBegin = 0,
          .commandCount = 1,
      },
  }};
  const auto makeList = [&](std::size_t count) {
    EncodeSessionSourceList list;
    check(list.assign(std::span<const QueueCompletionSource>(
              sources.data(), count)),
          "exact-list fixture builds a valid FIFO list");
    return list;
  };
  QueueSubmissionRecord exactRecord;
  check(exactRecord.assignFixedCompletionSources(
            std::span<const QueueCompletionSource>(sources.data(), 2u)),
        "exact-list fixture publishes two record sources");
  const EncodeSessionSourceList exactPending = makeList(2u);
  const EncodeSessionSourceList exactSession = makeList(2u);
  check(hasExactRedundantFixedCompletionSources(
            exactRecord, exactPending.span(), exactSession.span()),
        "three complete exact lists qualify as redundant");

  enum class Mismatch {
    Prefix,
    Superset,
    Reordered,
    SourceGeneration,
    StorageGeneration,
    Slot,
    Sequence,
    Present,
    CommandBegin,
    CommandCount,
    SessionOwner,
  };
  constexpr std::array mismatches{
      Mismatch::Prefix,
      Mismatch::Superset,
      Mismatch::Reordered,
      Mismatch::SourceGeneration,
      Mismatch::StorageGeneration,
      Mismatch::Slot,
      Mismatch::Sequence,
      Mismatch::Present,
      Mismatch::CommandBegin,
      Mismatch::CommandCount,
      Mismatch::SessionOwner,
  };
  for (const auto mismatch : mismatches) {
    QueueSubmissionRecord record = exactRecord;
    EncodeSessionSourceList pending = exactPending;
    EncodeSessionSourceList session = exactSession;
    switch (mismatch) {
    case Mismatch::Prefix:
      record.fixedCompletionSources.count = 1u;
      break;
    case Mismatch::Superset:
      check(record.fixedCompletionSources.append(sources[2]),
            "superset fixture appends a valid third source");
      break;
    case Mismatch::Reordered:
      std::swap(record.fixedCompletionSources.entries[0],
                record.fixedCompletionSources.entries[1]);
      break;
    case Mismatch::SourceGeneration:
      ++record.fixedCompletionSources.entries[0].source.id.generation;
      break;
    case Mismatch::StorageGeneration:
      ++record.fixedCompletionSources.entries[0].source.storage.generation;
      break;
    case Mismatch::Slot:
      ++record.fixedCompletionSources.entries[0].slotIndex;
      break;
    case Mismatch::Sequence:
      ++record.fixedCompletionSources.entries[0].seqId;
      break;
    case Mismatch::Present:
      record.fixedCompletionSources.entries[0].hasPresent = true;
      break;
    case Mismatch::CommandBegin:
      ++record.fixedCompletionSources.entries[0].commandBegin;
      break;
    case Mismatch::CommandCount:
      ++record.fixedCompletionSources.entries[0].commandCount;
      break;
    case Mismatch::SessionOwner:
      ++session.entries[1].commandCount;
      break;
    }
    check(!hasExactRedundantFixedCompletionSources(
              record, pending.span(), session.span()),
          "every prefix/superset/order/identity/range/owner mismatch stays "
          "non-canonical");
  }
}

void mergeEncodedPendingTailSubmissionPreservesHeadThenTailOrder() {
  QueueSubmissionRecord head;
  head.slotIndex = 0;
  head.seqId = 1;
  head.commandBufferChainLength = 3;
  head.diagnostics = dxmt9::core::metalqueue::CommandBufferDiagnostics{
      .seqId = 1,
      .slotIndex = 0,
      .hasDraw = true,
      .vertexShaderHash = 0x10,
      .pixelShaderHash = 0x20,
  };
  head.renderEncoderGpuSamples.push_back(makeGpuSample(1, 1));
  head.postCommitCallbacks.push_back([] {});
  head.completionCallbacks.push_back([] {});
  auto headRetained = std::make_shared<int>(11);
  head.retainedPayloads.push_back(headRetained);

  QueueSubmissionRecord tail;
  tail.slotIndex = 1;
  tail.seqId = 2;
  tail.commandBufferChainLength = 2;
  tail.diagnostics = dxmt9::core::metalqueue::CommandBufferDiagnostics{
      .seqId = 2,
      .slotIndex = 1,
      .hasPresent = true,
      .hasBlit = true,
      .vertexShaderHash = 0x30,
      .pixelShaderHash = 0x40,
  };
  tail.renderEncoderGpuSamples.push_back(makeGpuSample(2, 2));
  tail.postCommitCallbacks.push_back([] {});
  tail.completionCallbacks.push_back([] {});
  auto tailRetained = std::make_shared<int>(22);
  tail.retainedPayloads.push_back(tailRetained);

  const std::array<QueueCompletionSource, 1> headSources{QueueCompletionSource{
      .source = testSource(0, 1),
      .slotIndex = 0,
      .seqId = 1,
      .hasPresent = false,
      .commandCount = 4,
  }};
  const QueueCompletionSource tailSource{
      .source = testSource(1, 2),
      .slotIndex = 1,
      .seqId = 2,
      .hasPresent = true,
      .commandCount = 1,
  };

  const bool merged = mergeEncodedPendingTailSubmission(
      tail,
      head,
      std::span<const QueueCompletionSource>(
          headSources.data(), headSources.size()),
      tailSource);

  check(merged, "encoded head submission merges into the tail record");
  checkEq(tail.slotIndex, 1u, "merged record keeps tail slot identity");
  checkEq(tail.seqId, 2ull, "merged record keeps tail seq identity");
  checkEq(tail.commandBufferChainLength, 4ull,
          "chain length counts head sub-CBs plus one final tail commit");
  const auto tailSources = tail.explicitCompletionSourceSpan();
  checkEq(tailSources.size(), 2u,
          "merged record carries head and tail completion sources");
  checkEq(tailSources[0].seqId, 1ull,
          "head completion source stays first");
  checkEq(tailSources[1].seqId, 2ull,
          "tail completion source stays second");
  checkEq(tailSources[0].commandBegin, 0u,
          "head completion source keeps command-begin metadata");
  checkEq(tailSources[1].commandBegin, 0u,
          "tail completion source keeps command-begin metadata");
  checkEq(tailSources[0].commandCount, 4u,
          "head completion source keeps command-count metadata");
  checkEq(tailSources[1].commandCount, 1u,
          "tail completion source keeps command-count metadata");
  check(tailSources[1].hasPresent,
        "tail completion source carries present metadata");
  check(tail.diagnostics.hasDraw, "merged diagnostics include head draw work");
  check(tail.diagnostics.hasPresent,
        "merged diagnostics include tail present work");
  check(tail.diagnostics.hasBlit, "merged diagnostics include tail blit work");
  checkEq(tail.diagnostics.seqId, 2ull,
          "merged diagnostics keep tail seq identity");
  checkEq(tail.diagnostics.vertexShaderHash, 0x30ull,
          "tail shader hash wins as the latest source");
  checkEq(tail.renderEncoderGpuSamples.size(), 2u,
          "render encoder samples are merged");
  checkEq(tail.renderEncoderGpuSamples[0].seqId, 1ull,
          "head render sample stays before tail sample");
  checkEq(tail.renderEncoderGpuSamples[1].seqId, 2ull,
          "tail render sample stays after head sample");
  check(tail.renderEncoderGpuSamples[0].sourceCommand.valid(),
        "head render sample keeps locator-free encoded identity");
  check(tail.renderEncoderGpuSamples[1].sourceCommand.valid(),
        "tail render sample keeps locator-free encoded identity");
  checkEq(tail.renderEncoderGpuSamples[0].sourceCommand.seqId,
          tail.renderEncoderGpuSamples[0].seqId,
          "head sample scalar and qualified sequence stay consistent");
  checkEq(tail.renderEncoderGpuSamples[1].sourceCommand.commandIndex,
          tail.renderEncoderGpuSamples[1].commandIndex,
          "tail sample scalar and qualified command stay consistent");
  checkEq(tail.postCommitCallbacks.size(), 2u,
          "post-commit callbacks are merged");
  checkEq(tail.completionCallbacks.size(), 2u,
          "completion callbacks are merged");
  checkEq(tail.retainedPayloads.size(), 2u,
          "retained payload owners are merged");
  check(tail.retainedPayloads[0] == headRetained,
        "head retained payload stays before tail payload");
  check(tail.retainedPayloads[1] == tailRetained,
        "tail retained payload remains after head payload");
}

void fragmentCarrierFoldIgnoresEncodeSequenceAndPreservesCompletionOrder() {
  QueueSubmissionRecord encodedSeq2;
  encodedSeq2.slotIndex = 2;
  encodedSeq2.seqId = 2;
  encodedSeq2.commandBuffer = WMT::Reference<WMT::CommandBuffer>(
      static_cast<obj_handle_t>(0x120));
  encodedSeq2.diagnostics.hasDraw = true;
  encodedSeq2.completionCallbacks.push_back([] {});

  QueueSubmissionRecord encodedSeq1Tail;
  encodedSeq1Tail.slotIndex = 1;
  encodedSeq1Tail.seqId = 1;
  encodedSeq1Tail.commandBuffer = WMT::Reference<WMT::CommandBuffer>(
      static_cast<obj_handle_t>(0x120));
  encodedSeq1Tail.diagnostics.hasPresent = true;
  encodedSeq1Tail.completionCallbacks.push_back([] {});
  const std::array completionSources{
      QueueCompletionSource{
          .source = testSource(1, 1),
          .slotIndex = 1,
          .seqId = 1,
          .commandCount = 1,
      },
      QueueCompletionSource{
          .source = testSource(2, 2),
          .slotIndex = 2,
          .seqId = 2,
          .commandCount = 1,
      },
  };
  check(encodedSeq1Tail.assignFixedCompletionSources(completionSources),
        "fragment fold fixture stores FIFO completion sources");

  check(foldEncodedSessionFragmentCarrier(encodedSeq1Tail, encodedSeq2),
        "seq2 carrier folds into a later encoded seq1 tail");
  checkEq(encodedSeq1Tail.seqId, 1ull,
          "fold keeps the newest carrier record identity");
  checkEq(encodedSeq1Tail.slotIndex, std::size_t{1},
          "fold keeps the newest carrier slot identity");
  check(encodedSeq1Tail.diagnostics.hasDraw &&
            encodedSeq1Tail.diagnostics.hasPresent,
        "fold aggregates fragment diagnostics");
  checkEq(encodedSeq1Tail.completionCallbacks.size(), std::size_t{2},
          "fold carries both fragment callback sets");
  const auto published = encodedSeq1Tail.explicitCompletionSourceSpan();
  checkEq(published.size(), std::size_t{2},
          "fold does not replace pre-arranged completion metadata");
  checkEq(published[0].seqId, 1ull,
          "completion metadata stays in source FIFO order");
  checkEq(published[1].seqId, 2ull,
          "completion metadata remains independent of encode order");

  encodedSeq1Tail.commandBuffer.handle = NULL_OBJECT_HANDLE;
  encodedSeq2.commandBuffer.handle = NULL_OBJECT_HANDLE;
}

void fragmentCarrierFoldPreflightRejectsWithoutMutation() {
  QueueSubmissionRecord oldCarrier;
  oldCarrier.seqId = 1;
  oldCarrier.commandBuffer = WMT::Reference<WMT::CommandBuffer>(
      static_cast<obj_handle_t>(0x121));
  oldCarrier.retainedPayloads.push_back(std::make_shared<int>(5));

  QueueSubmissionRecord newTail;
  newTail.seqId = 2;
  newTail.commandBuffer = WMT::Reference<WMT::CommandBuffer>(
      static_cast<obj_handle_t>(0x122));
  newTail.commandBufferChainLength = 1;
  check(!foldEncodedSessionFragmentCarrier(newTail, oldCarrier),
        "unproved different live command buffers reject the fold");
  checkEq(newTail.commandBuffer.handle, static_cast<obj_handle_t>(0x122),
          "rejected fold keeps the new tail command buffer");
  checkEq(newTail.commandBufferChainLength, 1ull,
          "rejected fold keeps the new tail chain length");
  checkEq(oldCarrier.retainedPayloads.size(), std::size_t{1},
          "rejected fold does not move old retained ownership");

  oldCarrier.commandBuffer.handle = NULL_OBJECT_HANDLE;
  newTail.commandBuffer.handle = NULL_OBJECT_HANDLE;
}

void fragmentCarrierFoldRejectsShadowMismatchWithoutMutation() {
  QueueSubmissionRecord oldCarrier;
  oldCarrier.retainedPayloads.push_back(std::make_shared<int>(9));

  QueueSubmissionRecord newTail;
  const std::array completionSources{QueueCompletionSource{
      .source = testSource(2, 2),
      .slotIndex = 2,
      .seqId = 2,
      .commandCount = 1,
  }};
  check(newTail.assignFixedCompletionSources(completionSources),
        "shadow-mismatch fold fixture stores a sealed source");
  newTail.fixedCompletionSources.entries[0].seqId = 3;

  check(!foldEncodedSessionFragmentCarrier(newTail, oldCarrier),
        "fragment fold rejects a list that disagrees with its shadow");
  checkEq(newTail.fixedCompletionSources.entries[0].seqId, 3ull,
          "shadow-mismatch fold rejection preserves the new record");
  checkEq(oldCarrier.retainedPayloads.size(), std::size_t{1},
          "shadow-mismatch fold rejection preserves old ownership");
}

void fragmentCarrierFoldValidatesMovedProductionIdentity() {
  constexpr obj_handle_t injectedHandle =
      static_cast<obj_handle_t>(0x131);

  {
    QueueSubmissionRecord oldCarrier;
    oldCarrier.commandBuffer =
        WMT::Reference<WMT::CommandBuffer>(injectedHandle);
    oldCarrier.retainedPayloads.push_back(std::make_shared<int>(7));
    auto injectedOwner = std::move(oldCarrier.commandBuffer);
    check(!oldCarrier.commandBuffer,
          "production move shape empties the old carrier CB field");

    QueueSubmissionRecord unrelatedTail;
    unrelatedTail.commandBuffer = WMT::Reference<WMT::CommandBuffer>(
        static_cast<obj_handle_t>(0x132));
    unrelatedTail.commandBufferChainLength = 1;
    check(!foldEncodedSessionFragmentCarrier(
              unrelatedTail, oldCarrier, injectedHandle),
          "moved carrier rejects an unrelated uncommitted return tail");
    checkEq(oldCarrier.retainedPayloads.size(), std::size_t{1},
            "moved-identity rejection preserves carrier metadata");
    injectedOwner.handle = NULL_OBJECT_HANDLE;
    unrelatedTail.commandBuffer.handle = NULL_OBJECT_HANDLE;
  }

  {
    QueueSubmissionRecord oldCarrier;
    oldCarrier.commandBuffer =
        WMT::Reference<WMT::CommandBuffer>(injectedHandle);
    auto injectedOwner = std::move(oldCarrier.commandBuffer);
    QueueSubmissionRecord nullTail;
    check(!foldEncodedSessionFragmentCarrier(
              nullTail, oldCarrier, injectedHandle),
          "moved carrier rejects a null returned tail");
    injectedOwner.handle = NULL_OBJECT_HANDLE;
  }

  {
    QueueSubmissionRecord oldCarrier;
    oldCarrier.commandBuffer =
        WMT::Reference<WMT::CommandBuffer>(injectedHandle);
    QueueSubmissionRecord sameTail;
    sameTail.commandBuffer = std::move(oldCarrier.commandBuffer);
    check(foldEncodedSessionFragmentCarrier(
              sameTail, oldCarrier, injectedHandle),
          "moved carrier accepts the same returned command buffer");
    sameTail.commandBuffer.handle = NULL_OBJECT_HANDLE;
  }

  {
    QueueSubmissionRecord oldCarrier;
    oldCarrier.commandBuffer =
        WMT::Reference<WMT::CommandBuffer>(injectedHandle);
    auto injectedOwner = std::move(oldCarrier.commandBuffer);
    QueueSubmissionRecord committedSuccessor;
    committedSuccessor.commandBuffer = WMT::Reference<WMT::CommandBuffer>(
        static_cast<obj_handle_t>(0x133));
    committedSuccessor.commandBufferChainLength = 2;
    check(foldEncodedSessionFragmentCarrier(
              committedSuccessor, oldCarrier, injectedHandle),
          "moved carrier accepts a proven committed successor tail");
    checkEq(committedSuccessor.commandBufferChainLength, 2ull,
            "committed successor retains the encoded chain proof");
    injectedOwner.handle = NULL_OBJECT_HANDLE;
    committedSuccessor.commandBuffer.handle = NULL_OBJECT_HANDLE;
  }
}

void mergeEncodedPendingTailSubmissionAcceptsSessionOwnedSources() {
  QueueSubmissionRecord head;
  head.slotIndex = 0;
  head.seqId = 1;
  head.commandBufferChainLength = 1;

  QueueSubmissionRecord tail;
  tail.slotIndex = 1;
  tail.seqId = 2;
  tail.commandBufferChainLength = 1;

  const std::array<QueueCompletionSource, 1> headSources{QueueCompletionSource{
      .source = testSource(0, 1),
      .slotIndex = 0,
      .seqId = 1,
      .hasPresent = false,
      .commandCount = 5,
  }};
  const QueueCompletionSource tailSource{
      .source = testSource(1, 2),
      .slotIndex = 1,
      .seqId = 2,
      .hasPresent = true,
      .commandCount = 1,
  };
  const std::array<QueueCompletionSource, 2> tailSourcesBeforeMerge{{
      headSources[0],
      tailSource,
  }};
  check(tail.assignFixedCompletionSources(std::span<const QueueCompletionSource>(
            tailSourcesBeforeMerge.data(), tailSourcesBeforeMerge.size())),
        "test setup stores session-owned tail sources in fixed metadata");

  EncodeSessionSourceList mergedSources;
  const bool merged = mergeEncodedPendingTailSubmission(
      tail,
      head,
      std::span<const QueueCompletionSource>(
          headSources.data(), headSources.size()),
      tailSource,
      /*encodedHeadTailAlreadyCommitted=*/false,
      &mergedSources);

  check(merged, "session-owned completion source prefix is accepted");
  const auto tailSources = tail.explicitCompletionSourceSpan();
  checkEq(tailSources.size(), 2u,
          "session-owned completion sources are not duplicated");
  checkEq(tailSources[0].seqId, 1ull,
          "session-owned head source stays first");
  checkEq(tailSources[1].seqId, 2ull,
          "session-owned tail source stays second");
  checkEq(mergedSources.size(), 2u,
          "merged source list mirrors the queue completion sources");
  checkEq(mergedSources.span()[0].seqId, 1ull,
          "merged source list preserves head seq");
  checkEq(mergedSources.span()[1].seqId, 2ull,
          "merged source list preserves tail seq");
  check(mergedSources.span()[1].hasPresent,
        "merged source list preserves tail present metadata");
  checkEq(mergedSources.span()[0].commandBegin, 0u,
          "merged source list preserves head command begin");
  checkEq(mergedSources.span()[0].commandCount, 5u,
          "merged source list preserves head command count");
}

void mergeEncodedPendingTailSubmissionRejectsSourceMetadataMismatch() {
  QueueSubmissionRecord head;
  head.slotIndex = 0;
  head.seqId = 1;

  QueueSubmissionRecord tail;
  tail.slotIndex = 1;
  tail.seqId = 2;

  const std::array<QueueCompletionSource, 1> headSources{QueueCompletionSource{
      .source = testSource(0, 1),
      .slotIndex = 0,
      .seqId = 1,
      .hasPresent = false,
      .commandCount = 7,
  }};
  const QueueCompletionSource tailSource{
      .source = testSource(1, 2),
      .slotIndex = 1,
      .seqId = 2,
      .hasPresent = true,
      .commandCount = 1,
  };
  const std::array<QueueCompletionSource, 2> tailSourcesBeforeMerge{{
      QueueCompletionSource{
          .source = testSource(0, 1),
          .slotIndex = 0,
          .seqId = 1,
          .hasPresent = false,
          .commandBegin = 1,
          .commandCount = 7,
      },
      tailSource,
  }};
  check(tail.assignFixedCompletionSources(std::span<const QueueCompletionSource>(
            tailSourcesBeforeMerge.data(), tailSourcesBeforeMerge.size())),
        "test setup stores mismatched session source metadata");

  EncodeSessionSourceList mergedSources;
  const bool merged = mergeEncodedPendingTailSubmission(
      tail,
      head,
      std::span<const QueueCompletionSource>(
          headSources.data(), headSources.size()),
      tailSource,
      /*encodedHeadTailAlreadyCommitted=*/false,
      &mergedSources);

  check(!merged, "source metadata mismatch rejects tail-source reuse");
  check(mergedSources.empty(),
        "metadata mismatch leaves merged source output empty");
  checkEq(tail.explicitCompletionSourceSpan()[0].commandBegin, 1u,
          "metadata mismatch leaves existing fixed sources untouched");
}

void mergeEncodedPendingTailSubmissionRejectsShadowMismatch() {
  QueueSubmissionRecord head;
  head.slotIndex = 0;
  head.seqId = 1;
  head.retainedPayloads.push_back(std::make_shared<int>(23));

  QueueSubmissionRecord tail;
  tail.slotIndex = 1;
  tail.seqId = 2;
  const std::array headSources{QueueCompletionSource{
      .source = testSource(0, 1),
      .slotIndex = 0,
      .seqId = 1,
      .commandCount = 1,
  }};
  const QueueCompletionSource tailSource{
      .source = testSource(1, 2),
      .slotIndex = 1,
      .seqId = 2,
      .hasPresent = true,
      .commandCount = 1,
  };
  const std::array allSources{headSources[0], tailSource};
  check(tail.assignFixedCompletionSources(allSources),
        "shadow-mismatch merge fixture stores a sealed FIFO list");
  tail.fixedCompletionSources.entries[1].hasPresent = false;

  EncodeSessionSourceList mergedSources;
  check(!mergeEncodedPendingTailSubmission(
            tail, head, headSources, tailSource,
            /*encodedHeadTailAlreadyCommitted=*/false, &mergedSources),
        "pending-tail merge rejects a tail projection mismatch");
  check(!tail.fixedCompletionSources.entries[1].hasPresent &&
            mergedSources.empty(),
        "shadow-mismatch merge rejection leaves completion metadata intact");
  checkEq(head.retainedPayloads.size(), std::size_t{1},
          "shadow-mismatch merge rejection preserves head ownership");
}

void mergeEncodedPendingTailSubmissionAcceptsCommittedHeadTailMismatch() {
  QueueSubmissionRecord head;
  head.slotIndex = 0;
  head.seqId = 1;
  head.commandBuffer =
      WMT::Reference<WMT::CommandBuffer>(static_cast<obj_handle_t>(0x100));
  head.commandBufferChainLength = 2;
  auto headRetained = std::make_shared<int>(17);
  head.retainedPayloads.push_back(headRetained);

  QueueSubmissionRecord tail;
  tail.slotIndex = 1;
  tail.seqId = 2;
  tail.commandBuffer =
      WMT::Reference<WMT::CommandBuffer>(static_cast<obj_handle_t>(0x200));
  tail.commandBufferChainLength = 2;

  const std::array<QueueCompletionSource, 1> headSources{QueueCompletionSource{
      .source = testSource(0, 1),
      .slotIndex = 0,
      .seqId = 1,
      .hasPresent = false,
  }};
  const QueueCompletionSource tailSource{
      .source = testSource(1, 2),
      .slotIndex = 1,
      .seqId = 2,
      .hasPresent = true,
  };

  const bool merged = mergeEncodedPendingTailSubmission(
      tail,
      head,
      std::span<const QueueCompletionSource>(
          headSources.data(), headSources.size()),
      tailSource,
      /*encodedHeadTailAlreadyCommitted=*/true);

  check(merged,
        "committed pending-tail prefix may merge into a new tail CB record");
  checkEq(tail.commandBuffer.handle, static_cast<obj_handle_t>(0x200),
          "merged record keeps the current append tail command buffer");
  checkEq(tail.commandBufferChainLength, 3ull,
          "merged chain counts committed shared tail only once");
  checkEq(tail.retainedPayloads.size(), 1u,
          "head retained payload moves to the final tail record");
  check(tail.retainedPayloads[0] == headRetained,
        "head retained payload is preserved until final tail completion");

  tail.commandBuffer.handle = NULL_OBJECT_HANDLE;
  head.commandBuffer.handle = NULL_OBJECT_HANDLE;
}

void mergeEncodedPendingTailSubmissionRejectsUnprovenHeadTailMismatch() {
  QueueSubmissionRecord head;
  head.slotIndex = 0;
  head.seqId = 1;
  head.commandBuffer =
      WMT::Reference<WMT::CommandBuffer>(static_cast<obj_handle_t>(0x101));
  head.commandBufferChainLength = 2;
  auto headRetained = std::make_shared<int>(19);
  head.retainedPayloads.push_back(headRetained);

  QueueSubmissionRecord tail;
  tail.slotIndex = 1;
  tail.seqId = 2;
  tail.commandBuffer =
      WMT::Reference<WMT::CommandBuffer>(static_cast<obj_handle_t>(0x202));
  tail.commandBufferChainLength = 2;

  const std::array<QueueCompletionSource, 1> headSources{QueueCompletionSource{
      .source = testSource(0, 1),
      .slotIndex = 0,
      .seqId = 1,
      .hasPresent = false,
  }};
  const QueueCompletionSource tailSource{
      .source = testSource(1, 2),
      .slotIndex = 1,
      .seqId = 2,
      .hasPresent = true,
  };

  const bool merged = mergeEncodedPendingTailSubmission(
      tail,
      head,
      std::span<const QueueCompletionSource>(
          headSources.data(), headSources.size()),
      tailSource,
      /*encodedHeadTailAlreadyCommitted=*/false);

  check(!merged,
        "different tail CB handles require proof that the head tail committed");
  check(tail.fixedCompletionSources.empty(),
        "rejected mismatch leaves fixed completion sources untouched");
  checkEq(head.retainedPayloads.size(), 1u,
          "rejected mismatch does not move head retained payloads");

  tail.commandBuffer.handle = NULL_OBJECT_HANDLE;
  head.commandBuffer.handle = NULL_OBJECT_HANDLE;
}

void mergeEncodedPendingTailSubmissionRejectsSequenceGaps() {
  QueueSubmissionRecord head;
  head.slotIndex = 0;
  head.seqId = 1;

  QueueSubmissionRecord tail;
  tail.slotIndex = 2;
  tail.seqId = 3;
  tail.commandBufferChainLength = 9;

  const std::array<QueueCompletionSource, 1> headSources{QueueCompletionSource{
      .source = testSource(0, 1),
      .slotIndex = 0,
      .seqId = 1,
      .hasPresent = false,
  }};
  const QueueCompletionSource tailSource{
      .source = testSource(2, 3),
      .slotIndex = 2,
      .seqId = 3,
      .hasPresent = true,
  };

  const bool merged = mergeEncodedPendingTailSubmission(
      tail,
      head,
      std::span<const QueueCompletionSource>(
          headSources.data(), headSources.size()),
      tailSource);

  check(!merged, "sequence gaps are rejected");
  check(tail.fixedCompletionSources.empty(),
        "failed merge leaves fixed completion sources untouched");
  checkEq(tail.commandBufferChainLength, 9ull,
          "failed merge leaves tail chain length untouched");
}

void mergeEncodedPendingTailSubmissionRejectsSourceListOverflow() {
  QueueSubmissionRecord head;
  head.slotIndex = 0;
  head.seqId = 1;

  QueueSubmissionRecord tail;
  tail.slotIndex = kMaxEncodeSessionSources;
  tail.seqId = static_cast<std::uint64_t>(kMaxEncodeSessionSources + 1u);

  std::array<QueueCompletionSource, kMaxEncodeSessionSources> headSources{};
  for (std::size_t i = 0; i < headSources.size(); ++i) {
    headSources[i] = QueueCompletionSource{
        .source = testSource(i, i + 1u),
        .slotIndex = i,
        .seqId = static_cast<std::uint64_t>(i + 1u),
        .hasPresent = false,
    };
  }
  const QueueCompletionSource tailSource{
      .source = testSource(kMaxEncodeSessionSources,
                           kMaxEncodeSessionSources + 1u),
      .slotIndex = kMaxEncodeSessionSources,
      .seqId = static_cast<std::uint64_t>(kMaxEncodeSessionSources + 1u),
      .hasPresent = true,
  };

  EncodeSessionSourceList mergedSources;
  const bool merged = mergeEncodedPendingTailSubmission(
      tail,
      head,
      std::span<const QueueCompletionSource>(
          headSources.data(), headSources.size()),
      tailSource,
      /*encodedHeadTailAlreadyCommitted=*/false,
      &mergedSources);

  check(!merged, "session source overflow is rejected");
  check(tail.fixedCompletionSources.empty(),
        "overflow rejection leaves fixed completion sources untouched");
  check(mergedSources.empty(),
        "overflow rejection leaves merged source output empty");
}

}  // namespace

int main() {
  try {
    dceChunkLookaheadProgressPolicyIsFailOpen();
    actualOwnerPipelineObserverUsesQueueAndCv();
    disabledPipelineProjectionDoesNotResolveOrEmit();
    watchdogOnlyPipelineProjectionRetainsAttribution();
    controlBoundaryObservationIsColdAndNonReclaiming();
    arenaSourceArrivalUsesOneGenerationQualifiedOwnerEdge();
    mapWaitTargetNeverExceedsCommittedWaterline();
    undrainedSettlementLedgerFailsClosedAtCapacity();
    finishPathDrainsSettlementLedgerBeyondCapacity();
    reclaimLeavesReservedCompatibilityWriterAtFifoHead();
    partialSegmentSerialCompletionDefersReclaimUntilTail();
    appendsSingleLegacySource();
    appendsMultiSourceBatchInStrictSeqOrder();
    respectsAlreadyQueuedCompletions();
    presentQueueMayBeAbsent();
    encodeSessionSourceListStoresConsecutiveSources();
    carriedSessionRetainsPerSourceEncodeOwners();
    encodeSessionSourceListRejectsInvalidShape();
    encodeSessionSourceListAssignIsTransactional();
    diagnosticsMergeKeepsTailIdentityAndAggregatesSourceShape();
    encodeChunkSessionFactoryStartsWithoutActiveRender();
    encodeChunkSessionOwnsOrderedSourceList();
    encodeChunkSessionBatchAppendIsTransactional();
    retainEncodeChunkSessionStoresOwnerInSubmissionRecord();
    firstPublishSlotShapeClassifiesTailPresentPrefix();
    firstPublishSlotShapeRejectsPostPresentWorkAsTail();
    firstPublishSlotShapeKeepsNoPresentSlotUnclassified();
    runEncodeIterationPassesLiveSlotStorage();
    postEncodeRetiringControlRepresentsUnlockedReclaim();
    synchronousSourceBatchRejectsStaleAndReclaimingBorrows();
    failedEncodeSubmissionDoesNotRunPostCommitCallbacks();
    compatibilityPublicationRetainsLegacyInflightLimit();
    commitStaleWritingStorageFailStopsWithoutMutation();
    finishReleasesSlotResourceOwnersOutsideQueueLock();
    dequeueReadySlotBatchMovesEveryDequeuedSlotToEncoding();
    dequeueReadySlotBatchRespectsOutputCapacity();
    dequeueReadySlotBatchHonorsAppendPredicate();
    dequeueReadySlotBatchPrefixUsesCompleteSelectorCount();
    dequeueReadySlotBatchPrefixFallsBackToSingleWhenSelectorRejects();
    tentativeReadyPrefixKeepsControlsPendingAndSuffixReady();
    tentativeRestorePrecedesYoungerReadyPublication();
    tentativeCommitAloneMovesExactPrefixToEncoding();
    tentativeQueueApisRejectUnlockedStaleAndTamperedSnapshots();
    dequeueCarriesGenerationCheckedAdmissionMetadata();
    completionSourceForReadySlotPreservesPresentMetadata();
    completionSourceForReadySlotPreservesRangeMetadata();
    retainEncodedSourcesRejectsPendingSources();
    retainEncodedSourcesRejectsStaleSnapshotMetadata();
    retainEncodedSourcesRejectsStaleSourceAndStorageLocators();
    retainEncodedSourcesAcceptsPartialRangeMetadata();
    retainEncodedSourcesAcceptsSelectedPrefixMetadata();
    retainedEncodedHeadCompletesOnlyWithTailCarrier();
    pendingCompletionWatcherExpandsSessionSourcesInOrder();
    multiSourceSubmitRejectsStaleTailAtomically();
    submitRejectsWrongRangeAndPresentMetadataAtomically();
    appendPresentRejectsStaleWriterBeforeDereference();
    multiSourceCompletionRejectsStaleTailAtomically();
    recycledControlIsNeverCompletionOrReclaimIdentity();
    completionProjectionCorruptionRejectsBeforeCallbacks();
    invalidCompletionLocatorRetainsLegacyCallbackBeforeTapeFailure();
    staleReceiptRejectsBeforeCallbacksAndWaterlines();
    emptyCompletionSourcesAndNullCommandBufferFailBeforeMutation();
    fragmentCarrierFoldIgnoresEncodeSequenceAndPreservesCompletionOrder();
    fragmentCarrierFoldPreflightRejectsWithoutMutation();
    fragmentCarrierFoldRejectsShadowMismatchWithoutMutation();
    fragmentCarrierFoldValidatesMovedProductionIdentity();
    redundantFixedCompletionSourcePredicateIsExact();
    mergeEncodedPendingTailSubmissionPreservesHeadThenTailOrder();
    mergeEncodedPendingTailSubmissionAcceptsSessionOwnedSources();
    mergeEncodedPendingTailSubmissionRejectsSourceMetadataMismatch();
    mergeEncodedPendingTailSubmissionRejectsShadowMismatch();
    mergeEncodedPendingTailSubmissionAcceptsCommittedHeadTailMismatch();
    mergeEncodedPendingTailSubmissionRejectsUnprovenHeadTailMismatch();
    mergeEncodedPendingTailSubmissionRejectsSequenceGaps();
    mergeEncodedPendingTailSubmissionRejectsSourceListOverflow();
  } catch (const TestFailure& error) {
    std::cerr << "queue_completion_sources_spec failed: " << error.what() << '\n';
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "queue_completion_sources_spec unexpected exception: " << error.what()
              << '\n';
    return 1;
  }
  return 0;
}
