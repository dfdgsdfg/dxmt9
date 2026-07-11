#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "../../../src/dxmt9/dxmt9_draw_encoder.hpp"
#include "../../../src/dxmt9/dxmt9_queue.hpp"

namespace {

using dxmt9::core::metalqueue::QueueCompletionSource;
using dxmt9::core::metalqueue::QueueLifecycleController;
using dxmt9::core::metalqueue::QueueSubmissionRecord;
using dxmt9::core::metalqueue::ReadySlotSnapshot;
using dxmt9::core::metalqueue::EncodeSessionSourceList;
using dxmt9::core::metalqueue::kMaxEncodeSessionSources;
using dxmt9::core::metalqueue::appendCompletionSourcesToQueues;
using dxmt9::core::metalqueue::completionSourceForReadySlot;
using dxmt9::core::metalqueue::mergeEncodedPendingTailSubmission;
using dxmt9::core::metalqueue::mergeCommandBufferDiagnostics;
using dxmt9::core::metalqueue::summarizeNoEnqueueFirstPublishSlotShape;
using dxmt9::core::ChunkSlot;

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

template <typename T>
std::span<const T> asSpan(const std::vector<T>& values) {
  return std::span<const T>(values.data(), values.size());
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
            .slotIndex = 3,
            .seqId = 7,
            .hasPresent = false,
            .commandCount = 11,
        }),
        "first session source appends");
  check(list.append(QueueCompletionSource{
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

void encodeSessionSourceListRejectsInvalidShape() {
  EncodeSessionSourceList list;

  check(!list.canAppend(QueueCompletionSource{
            .slotIndex = 1,
            .seqId = 0,
            .hasPresent = false,
        }),
        "session source list preflight rejects zero seqId");
  check(!list.append(QueueCompletionSource{
            .slotIndex = 1,
            .seqId = 0,
            .hasPresent = false,
        }),
        "session source list rejects zero seqId");
  check(list.canAppend(QueueCompletionSource{
            .slotIndex = 1,
            .seqId = 1,
            .hasPresent = false,
        }),
        "session source list preflight accepts initial valid seqId");
  check(list.append(QueueCompletionSource{
            .slotIndex = 1,
            .seqId = 1,
            .hasPresent = false,
        }),
        "session source list accepts initial valid seqId");
  check(!list.canAppend(QueueCompletionSource{
            .slotIndex = 3,
            .seqId = 3,
            .hasPresent = false,
        }),
        "session source list preflight rejects seqId gaps");
  check(!list.append(QueueCompletionSource{
            .slotIndex = 3,
            .seqId = 3,
            .hasPresent = false,
        }),
        "session source list rejects seqId gaps");

  EncodeSessionSourceList tailList;
  check(tailList.append(QueueCompletionSource{
            .slotIndex = 1,
            .seqId = 1,
            .hasPresent = true,
        }),
        "session source list accepts present tail");
  check(!tailList.append(QueueCompletionSource{
            .slotIndex = 2,
            .seqId = 2,
            .hasPresent = false,
        }),
        "session source list rejects appending after present tail");

  EncodeSessionSourceList full;
  for (std::size_t i = 0; i < kMaxEncodeSessionSources; ++i) {
    check(full.append(QueueCompletionSource{
              .slotIndex = i,
              .seqId = static_cast<std::uint64_t>(i + 1u),
              .hasPresent = false,
          }),
          "session source list fills bounded capacity");
  }
  check(!full.canAppend(QueueCompletionSource{
            .slotIndex = kMaxEncodeSessionSources,
            .seqId = static_cast<std::uint64_t>(kMaxEncodeSessionSources + 1u),
            .hasPresent = false,
        }),
        "session source list preflight rejects overflow");
  check(!full.append(QueueCompletionSource{
            .slotIndex = kMaxEncodeSessionSources,
            .seqId = static_cast<std::uint64_t>(kMaxEncodeSessionSources + 1u),
            .hasPresent = false,
        }),
        "session source list rejects overflow");
}

void encodeSessionSourceListAssignIsTransactional() {
  const std::array<QueueCompletionSource, 2> initial{{
      {
          .slotIndex = 1,
          .seqId = 1,
          .hasPresent = false,
          .commandCount = 4,
      },
      {
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
          .slotIndex = 8,
          .seqId = 8,
          .hasPresent = false,
      },
      {
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
                .slotIndex = 3,
                .seqId = 10,
                .hasPresent = false,
                .commandCount = 6,
            }),
        "encode session preflight accepts first completion source");
  check(dxmt9::encoders::appendEncodeChunkSessionSource(
            *session,
            QueueCompletionSource{
                .slotIndex = 3,
                .seqId = 10,
                .hasPresent = false,
                .commandCount = 6,
            }),
        "encode session accepts first completion source");
  check(dxmt9::encoders::canAppendEncodeChunkSessionSource(
            *session,
            QueueCompletionSource{
                .slotIndex = 4,
                .seqId = 11,
                .hasPresent = true,
                .commandCount = 1,
            }),
        "encode session preflight accepts present tail completion source");
  check(dxmt9::encoders::appendEncodeChunkSessionSource(
            *session,
            QueueCompletionSource{
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
                .slotIndex = 5,
                .seqId = 12,
                .hasPresent = false,
            }),
        "encode session preflight rejects source after present tail");
  check(!dxmt9::encoders::appendEncodeChunkSessionSource(
            *session,
            QueueCompletionSource{
                .slotIndex = 5,
                .seqId = 12,
                .hasPresent = false,
            }),
        "encode session rejects source after present tail");

  dxmt9::encoders::resetEncodeChunkSession(*session);
  check(dxmt9::encoders::encodeChunkSessionSources(*session).empty(),
        "reset clears encode session source list");
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
  std::uint64_t nextSeqId = 1;
  std::deque<std::size_t> readySlots{};
  std::deque<std::uint64_t> completedSeqQueue{};
  std::deque<std::uint64_t> completedPresentSeqQueue{};
  std::size_t inflightCount = 0;
  std::uint64_t completedSeqId = 0;
  std::uint64_t presentCompletedSeqId = 0;
  std::uint64_t lastCommittedSeqId = 0;
  std::array<ChunkSlot, 4> slots{};
  std::mutex mutex{};
  std::condition_variable writeCv{};
  std::condition_variable encodeCv{};
  std::condition_variable finishCv{};
  std::condition_variable presentCompletedCv{};
  bool stop = false;
  QueueLifecycleController controller{};

  QueueFixture() {
    controller.bindTrackedSubmissionState(QueueLifecycleController::SubmissionBinding{
        .writingSlot = &writingSlot,
        .writeIndex = &writeIndex,
        .nextSeqId = &nextSeqId,
        .readySlots = &readySlots,
        .completedSeqQueue = &completedSeqQueue,
        .completedPresentSeqQueue = &completedPresentSeqQueue,
        .inflightCount = &inflightCount,
        .completedSeqId = &completedSeqId,
        .presentCompletedSeqId = &presentCompletedSeqId,
        .lastCommittedSeqId = &lastCommittedSeqId,
        .slots = std::span<ChunkSlot>(slots.data(), slots.size()),
        .mutex = &mutex,
        .writeCv = &writeCv,
        .encodeCv = &encodeCv,
        .finishCv = &finishCv,
        .presentCompletedCv = &presentCompletedCv,
        .stop = &stop,
    });
  }

  void addReadySlot(std::size_t slotIndex, std::uint64_t seqId) {
    slots[slotIndex].state = ChunkSlot::State::Pending;
    slots[slotIndex].seqId = seqId;
    readySlots.push_back(slotIndex);
    lastCommittedSeqId = std::max(lastCommittedSeqId, seqId);
    nextSeqId = std::max(nextSeqId, seqId + 1u);
    ++inflightCount;
  }
};

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

  ChunkSlot* observedSlot = nullptr;
  std::vector<std::uint64_t> inlineCompleted;
  std::unique_lock lock(fixture.mutex);
  const bool encoded = fixture.controller.runEncodeIteration(
      lock,
      [&](std::size_t slotIndex, ChunkSlot& slot)
          -> std::optional<QueueSubmissionRecord> {
        checkEq(slotIndex, 0u,
                "single-source iteration forwards the dequeued slot index");
        observedSlot = &slot;
        check(observedSlot == &fixture.slots[0],
              "single-source iteration passes live slot storage by reference");
        check(slot.state == ChunkSlot::State::Encoding,
              "single-source encode sees the live slot in Encoding state");
        return std::nullopt;
      },
      [&](std::uint64_t seqId) { inlineCompleted.push_back(seqId); });

  check(encoded, "single-source iteration consumes the ready slot");
  check(observedSlot == &fixture.slots[0],
        "single-source iteration did not encode a ChunkSlot copy");
  checkEq(inlineCompleted.size(), 1u,
          "inline single-source completion invokes callback once");
  checkEq(inlineCompleted.front(), 1ull,
          "inline single-source completion reports source seqId");
  checkEq(fixture.completedSeqQueue.size(), 1u,
          "inline single-source completion queues source seqId");
  check(fixture.slots[0].state == ChunkSlot::State::Free,
        "inline single-source completion releases the live slot");
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
  check(fixture.readySlots.empty(), "batch dequeue drains ready queue up to capacity");
  check(fixture.slots[0].state == ChunkSlot::State::Encoding,
        "first source slot moves to Encoding");
  check(fixture.slots[1].state == ChunkSlot::State::Encoding,
        "second source slot moves to Encoding");
  checkEq(snapshots[0].slotIndex, 0u, "first snapshot records slot index");
  check(snapshots[0].slot == &fixture.slots[0],
        "first snapshot references live slot storage");
  checkEq(snapshots[0].seqId, 1ull, "first snapshot records seqId");
  check(!snapshots[0].hasPresent, "first snapshot records present absence");
  checkEq(snapshots[0].commandBegin, 0u,
          "first snapshot records whole-source begin");
  checkEq(snapshots[0].commandCount, fixture.slots[0].commandCount(),
          "first snapshot records command count");
  checkEq(snapshots[1].slotIndex, 1u, "second snapshot records slot index");
  check(snapshots[1].slot == &fixture.slots[1],
        "second snapshot references live slot storage");
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
  checkEq(fixture.readySlots.size(), 1u, "capacity-limited batch leaves remaining ready slot");
  checkEq(fixture.readySlots.front(), 2u, "remaining ready slot keeps FIFO order");
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
          [](std::span<const ReadySlotSnapshot> selected,
             std::size_t candidateSlotIndex,
             const ChunkSlot& candidateSlot) {
            checkEq(selected.size(), 1u,
                    "predicate sees the already-selected source");
            checkEq(candidateSlotIndex, 1u,
                    "predicate sees the next FIFO candidate");
            checkEq(candidateSlot.seqId, 2ull,
                    "predicate sees the candidate slot payload");
            return false;
          });

  checkEq(count, 1u, "batch predicate stops after the first source");
  checkEq(fixture.readySlots.size(), 2u,
          "rejected candidates remain ready for later encode iterations");
  checkEq(fixture.readySlots.front(), 1u,
          "first rejected candidate keeps FIFO position");
  check(fixture.slots[0].state == ChunkSlot::State::Encoding,
        "accepted source moves to Encoding");
  check(fixture.slots[1].state == ChunkSlot::State::Pending,
        "rejected candidate remains Pending");
  check(fixture.slots[2].state == ChunkSlot::State::Pending,
        "later candidate remains Pending");
}

void completionSourceForReadySlotPreservesPresentMetadata() {
  ChunkSlot slot{};
  ReadySlotSnapshot snapshot{};
  snapshot.slotIndex = 3;
  slot.seqId = 7;
  slot.appendPresent(dxmt9::core::SwapDesc{}, dxmt9::core::Handle{0x77});
  snapshot.seqId = slot.seqId;
  snapshot.hasPresent = true;
  snapshot.commandCount = slot.commandCount();
  snapshot.slot = &slot;

  const auto source = completionSourceForReadySlot(snapshot);

  checkEq(source.slotIndex, 3u, "completion source preserves slot index");
  checkEq(source.seqId, 7ull, "completion source preserves seqId");
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
  snapshot.hasPresent = false;
  snapshot.commandBegin = 0;
  snapshot.commandCount = 1;
  snapshot.slot = &slot;

  const auto source = completionSourceForReadySlot(snapshot);

  checkEq(source.slotIndex, 4u, "range source preserves slot index");
  checkEq(source.seqId, 8ull, "range source preserves seqId");
  check(!source.hasPresent, "range source preserves non-present metadata");
  checkEq(source.commandBegin, 0u, "range source preserves command begin");
  checkEq(source.commandCount, 1u, "range source preserves command count");
}

void pendingCompletionWatcherExpandsSessionSourcesInOrder() {
  QueueFixture fixture;
  fixture.addReadySlot(0, 1);
  fixture.addReadySlot(1, 2);
  fixture.addReadySlot(2, 3);
  fixture.slots[2].appendPresent(dxmt9::core::SwapDesc{},
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
    check(sources[2].slot != nullptr, "tail source keeps live slot view");
    record.seqId = sources[2].seqId;
    EncodeSessionSourceList recordSources;
    for (const auto& source : sources) {
      check(recordSources.append(completionSourceForReadySlot(source)),
            "test setup builds fixed completion source metadata");
    }
    check(record.assignFixedCompletionSources(recordSources.span()),
          "session submission stores fixed completion source metadata");

    fixture.controller.submitEncodedSubmission(lock, record);
    check(fixture.completedSeqQueue.empty(),
          "GPU submission alone does not complete any source");
    for (std::size_t i = 0; i < sources.size(); ++i) {
      check(fixture.slots[i].state == ChunkSlot::State::GPU,
            "every session source moves to GPU before pending completion");
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
  pending.completionCallbacks.push_back(
      [&completionCallbackRan] { completionCallbackRan = true; });
  fixture.controller.enqueuePendingCompletionForTest(std::move(pending));

  bool stop = false;
  const bool processed = fixture.controller.processOnePendingCompletion(stop);
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
  check(fixture.slots[1].state == ChunkSlot::State::GPU,
        "source 2 waits for its own finish");
  check(fixture.slots[2].state == ChunkSlot::State::GPU,
        "tail source waits for its own finish");

  check(fixture.controller.runFinishIteration(
            lock, [&](std::uint64_t seqId) { finishedSeq = seqId; }),
        "second watcher-produced completion drains");
  checkEq(finishedSeq, 2ull, "second watcher completion finishes source 2");
  checkEq(fixture.presentCompletedSeqId, 0ull,
          "present completion still waits for tail source");
  check(fixture.slots[1].state == ChunkSlot::State::Free,
        "source 2 is reclaimed after its finish");
  check(fixture.slots[2].state == ChunkSlot::State::GPU,
        "tail source remains GPU-visible before tail finish");

  check(fixture.controller.runFinishIteration(
            lock, [&](std::uint64_t seqId) { finishedSeq = seqId; }),
        "tail watcher-produced completion drains");
  checkEq(finishedSeq, 3ull, "tail watcher completion finishes source 3");
  checkEq(fixture.completedSeqId, 3ull,
          "completed seq advances through the full session");
  checkEq(fixture.presentCompletedSeqId, 3ull,
          "present completion advances only at the session tail");
  check(fixture.slots[2].state == ChunkSlot::State::Free,
        "tail source is reclaimed after tail finish");
}

QueueSubmissionRecord::RenderEncoderGpuSample makeGpuSample(
    std::uint32_t commandIndex,
    std::uint64_t seqId) {
  return QueueSubmissionRecord::RenderEncoderGpuSample{
      .startIndex = commandIndex * 2u,
      .endIndex = commandIndex * 2u + 1u,
      .seqId = seqId,
      .commandIndex = commandIndex,
  };
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
      .slotIndex = 0,
      .seqId = 1,
      .hasPresent = false,
      .commandCount = 4,
  }};
  const QueueCompletionSource tailSource{
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
      .slotIndex = 0,
      .seqId = 1,
      .hasPresent = false,
      .commandCount = 5,
  }};
  const QueueCompletionSource tailSource{
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
      .slotIndex = 0,
      .seqId = 1,
      .hasPresent = false,
      .commandCount = 7,
  }};
  const QueueCompletionSource tailSource{
      .slotIndex = 1,
      .seqId = 2,
      .hasPresent = true,
      .commandCount = 1,
  };
  const std::array<QueueCompletionSource, 2> tailSourcesBeforeMerge{{
      QueueCompletionSource{
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
      .slotIndex = 0,
      .seqId = 1,
      .hasPresent = false,
  }};
  const QueueCompletionSource tailSource{
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
      .slotIndex = 0,
      .seqId = 1,
      .hasPresent = false,
  }};
  const QueueCompletionSource tailSource{
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
      .slotIndex = 0,
      .seqId = 1,
      .hasPresent = false,
  }};
  const QueueCompletionSource tailSource{
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
        .slotIndex = i,
        .seqId = static_cast<std::uint64_t>(i + 1u),
        .hasPresent = false,
    };
  }
  const QueueCompletionSource tailSource{
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
    appendsSingleLegacySource();
    appendsMultiSourceBatchInStrictSeqOrder();
    respectsAlreadyQueuedCompletions();
    presentQueueMayBeAbsent();
    encodeSessionSourceListStoresConsecutiveSources();
    encodeSessionSourceListRejectsInvalidShape();
    encodeSessionSourceListAssignIsTransactional();
    diagnosticsMergeKeepsTailIdentityAndAggregatesSourceShape();
    encodeChunkSessionFactoryStartsWithoutActiveRender();
    encodeChunkSessionOwnsOrderedSourceList();
    retainEncodeChunkSessionStoresOwnerInSubmissionRecord();
    firstPublishSlotShapeClassifiesTailPresentPrefix();
    firstPublishSlotShapeRejectsPostPresentWorkAsTail();
    firstPublishSlotShapeKeepsNoPresentSlotUnclassified();
    runEncodeIterationPassesLiveSlotStorage();
    dequeueReadySlotBatchMovesEveryDequeuedSlotToEncoding();
    dequeueReadySlotBatchRespectsOutputCapacity();
    dequeueReadySlotBatchHonorsAppendPredicate();
    completionSourceForReadySlotPreservesPresentMetadata();
    completionSourceForReadySlotPreservesRangeMetadata();
    pendingCompletionWatcherExpandsSessionSourcesInOrder();
    mergeEncodedPendingTailSubmissionPreservesHeadThenTailOrder();
    mergeEncodedPendingTailSubmissionAcceptsSessionOwnedSources();
    mergeEncodedPendingTailSubmissionRejectsSourceMetadataMismatch();
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
