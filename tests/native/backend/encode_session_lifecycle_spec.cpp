// End-to-end native coverage for the deferred EncodeSession lifecycle.
//
// The native nowine test host has no Metal device. This spec therefore uses
// retained Foundation objects strictly as non-null WMT ownership tokens and an
// inert CommandQueue. Targetless deferred clears exercise session payload
// carry, while the EncodeDrawRecorder supplies a fake render encoder and UP
// slices for explicit DrawRun partition integration. Metal calls are suppressed,
// but the production encodeChunk and finalizeEncodeChunkSessionIntoSubmission
// implementations execute in full, covering command-once traversal, absolute
// draw indexing, command-buffer carry, ordered source accumulation, publication
// into the tail submission, and session reset.

#include "../../../src/dxmt9/dxmt9_draw_encoder.hpp"
#include "../../../src/dxmt9/dxmt9_encode_partition.hpp"
#include "../../../src/dxmt9/dxmt9_encode_session.hpp"
#include "../../../src/dxmt9/dxmt9_pipeline_cache.hpp"
#include "../../../src/dxmt9/dxmt9_resource_pool.hpp"
#include "../../../src/dxmt9/dxmt9_ring_arena.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <array>
#include <exception>
#include <iostream>
#include <memory>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

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
void checkEq(const A& actual, const B& expected, std::string_view message) {
  if (!(actual == expected)) {
    std::ostringstream out;
    out << message << " (" << actual << " vs " << expected << ")";
    fail(out.str());
  }
}

template <typename WmtType>
WMT::Reference<WmtType> retainedToken(const char* label) {
  auto owner = WMT::MakeString(label, WMTUTF8StringEncoding);
  return WMT::Reference<WmtType>(WmtType{owner.handle});
}

struct DrawRunCapture {
  std::size_t drawRunBegins = 0;
  std::size_t renderPassBegins = 0;
  std::size_t renderPassEnds = 0;
  std::size_t splitPolicyCalls = 0;
  std::size_t uploadBatchCalls = 0;
  std::size_t unexpectedNonIndexedDraws = 0;
  std::vector<std::pair<std::size_t, std::size_t>> subranges;
  std::vector<std::uint64_t> vertexOffsets;
  std::vector<std::uint64_t> indexOffsets;
  std::vector<std::uint64_t> indexCounts;
};

void recordDrawRunBegin(void* userdata,
                        std::size_t,
                        std::size_t) {
  ++static_cast<DrawRunCapture*>(userdata)->drawRunBegins;
}

void recordDrawSubrange(void* userdata,
                        std::size_t,
                        std::size_t absoluteDrawParamBegin,
                        std::size_t drawCount) {
  static_cast<DrawRunCapture*>(userdata)->subranges.push_back(
      {absoluteDrawParamBegin, drawCount});
}

void recordRenderPassBegin(void* userdata, std::size_t) {
  ++static_cast<DrawRunCapture*>(userdata)->renderPassBegins;
}

void recordRenderPassEnd(void* userdata) {
  ++static_cast<DrawRunCapture*>(userdata)->renderPassEnds;
}

void recordSplitPolicy(void* userdata, bool) {
  ++static_cast<DrawRunCapture*>(userdata)->splitPolicyCalls;
}

std::vector<dxmt9::CommandQueue::TransientBufferSlice> recordUploadBatch(
    void* userdata,
    std::span<const std::span<const std::byte>> payloads) {
  auto* capture = static_cast<DrawRunCapture*>(userdata);
  ++capture->uploadBatchCalls;
  std::vector<dxmt9::CommandQueue::TransientBufferSlice> slices;
  slices.reserve(payloads.size());
  for (std::size_t i = 0; i < payloads.size(); ++i) {
    slices.push_back(dxmt9::CommandQueue::TransientBufferSlice{
        .buffer = WMT::Buffer{
            static_cast<obj_handle_t>(0x710000000000000ull + i)},
        .offset = 1000u + i * 100u,
        .size = payloads[i].size(),
    });
  }
  return slices;
}

void recordVertexBuffer(void* userdata,
                        WMT::Buffer,
                        std::uint64_t offset,
                        std::uint8_t index) {
  if (index == 1u) {
    static_cast<DrawRunCapture*>(userdata)->vertexOffsets.push_back(offset);
  }
}

void recordDrawPrimitives(void* userdata,
                          WMTPrimitiveType,
                          std::uint64_t,
                          std::uint64_t,
                          std::uint32_t,
                          std::uint32_t) {
  ++static_cast<DrawRunCapture*>(userdata)->unexpectedNonIndexedDraws;
}

void recordDrawIndexedPrimitives(void* userdata,
                                 WMTPrimitiveType,
                                 WMTIndexType,
                                 std::uint64_t indexCount,
                                 WMT::Buffer,
                                 std::uint64_t indexBufferOffset,
                                 std::uint32_t,
                                 std::int32_t,
                                 std::uint32_t) {
  auto* capture = static_cast<DrawRunCapture*>(userdata);
  capture->indexOffsets.push_back(indexBufferOffset);
  capture->indexCounts.push_back(indexCount);
}

dxmt9::encoders::EncodeDrawRecorder makeDrawRunRecorder(
    DrawRunCapture& capture,
    WMT::RenderCommandEncoder renderCommandEncoder) {
  return dxmt9::encoders::EncodeDrawRecorder{
      .userdata = &capture,
      .suppressMetalCalls = true,
      .suppressBaseStateLookup = true,
      .renderPipelineState = WMT::RenderPipelineState{0x720000000000001ull},
      .depthStencilState = WMT::DepthStencilState{0x720000000000002ull},
      .renderCommandEncoder = renderCommandEncoder,
      .beginDrawRunCommand = recordDrawRunBegin,
      .beginDrawSubrange = recordDrawSubrange,
      .beginRenderPass = recordRenderPassBegin,
      .endRenderPass = recordRenderPassEnd,
      .applyPerRecordSplitPolicy = recordSplitPolicy,
      .uploadTransientBufferBatch = recordUploadBatch,
      .setVertexBuffer = recordVertexBuffer,
      .drawPrimitives = recordDrawPrimitives,
      .drawIndexedPrimitives = recordDrawIndexedPrimitives,
  };
}

struct Harness {
  dxmt9::core::BackendLimits limits{};
  dxmt9::resources::Pool pool{};
  dxmt9::pipeline::Cache cache{};
  dxmt9::scratch::FrameAllocators allocators{};
  WMT::Reference<WMT::Device> device =
      retainedToken<WMT::Device>("encode-session-device-token");
  dxmt9::CommandQueue queue;

  Harness()
      : queue(dxmt9::CommandQueue::InertTestQueueTag{},
              retainedToken<WMT::CommandQueue>("encode-session-queue-token"),
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

dxmt9::encoders::EncodeChunkOptions deferredOptions(
    dxmt9::encoders::EncodeChunkSessionState& session,
    WMT::Reference<WMT::CommandBuffer> commandBuffer,
    dxmt9::core::metalqueue::QueueCompletionSource source) {
  if (!source.source.valid()) {
    source.source = dxmt9::core::CpuReadyTape::SourceRef{
        .id = {
            .index = static_cast<std::uint32_t>(source.slotIndex),
            .generation = source.seqId,
        },
        .storage = {
            .firstPage = static_cast<std::uint32_t>(source.slotIndex),
            .pageCount = 1,
            .generation = source.seqId,
        },
    };
  }
  dxmt9::encoders::EncodeChunkOptions options{};
  options.commandBuffer = std::move(commandBuffer);
  options.session = &session;
  options.deferSessionFinalization = true;
  options.sessionSource = source;
  options.partitionSource = source.source;
  return options;
}

constexpr std::size_t kPartitionDrawCount = 5u;
constexpr std::size_t kPartitionCommandIndex = 1u;
constexpr std::uint32_t kPartitionFirstParam = 1u;

dxmt9::core::metalqueue::QueueCompletionSource partitionSource(
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
      .commandBegin = kPartitionCommandIndex,
      .commandCount = 1u,
  };
}

dxmt9::core::CanonicalDrawState makeDrawRunState() {
  dxmt9::core::CanonicalDrawState state{};
  state.hot.streamStrides[0] = 4u;
  state.shaderLayout.vertexDecl.streams[0].stride = 4u;
  state.shaderLayout.vertexShader.kind =
      dxmt9::core::ShaderRef::Kind::Bytecode;
  state.shaderLayout.pixelShader.kind =
      dxmt9::core::ShaderRef::Kind::Bytecode;
  return state;
}

dxmt9::core::ChunkSlot makeDrawRunSlot(std::uint64_t seqId) {
  std::array<dxmt9::core::DrawParam, kPartitionDrawCount> draws{};
  std::array<dxmt9::core::DrawParamPayloadView, kPartitionDrawCount>
      payloads{};
  std::array<std::array<std::uint8_t, 64>, kPartitionDrawCount>
      vertexPayloads{};
  std::array<std::array<std::uint8_t, 32>, kPartitionDrawCount>
      indexPayloads{};
  for (std::size_t i = 0; i < draws.size(); ++i) {
    draws[i].primitiveType = dxmt9::core::PrimitiveType::TriangleList;
    draws[i].primitiveCount = static_cast<std::uint32_t>(i + 1u);
    draws[i].indexed = true;
    draws[i].indexType = dxmt9::core::IndexType::UInt16;
    draws[i].instanceCount = 1u;
    payloads[i].userVertexData = vertexPayloads[i];
    payloads[i].userIndexData = indexPayloads[i];
  }

  dxmt9::core::ChunkSlot slot{};
  slot.seqId = seqId;
  const std::array<dxmt9::core::DrawParam, 1> prefixDraws{
      dxmt9::core::DrawParam{},
  };
  const std::array<dxmt9::core::DrawParamPayloadView, 1> prefixPayloads{};
  slot.appendDrawRun(makeDrawRunState(),
                     dxmt9::core::DrawUniformPayload{}, prefixDraws,
                     prefixPayloads);
  slot.appendDrawRun(makeDrawRunState(),
                     dxmt9::core::DrawUniformPayload{}, draws, payloads);
  checkEq(slot.commandCount(), std::size_t{2},
          "DrawRun fixture appends a prefix and selected command");
  checkEq(slot.commandAt(kPartitionCommandIndex).drawRunRecord->firstParam,
          kPartitionFirstParam,
          "selected DrawRun starts at a nonzero absolute DrawParam index");
  return slot;
}

dxmt9::encoders::EncodePartitionRangeSnapshot makeDrawRange(
    const dxmt9::encoders::EncodePartitionReplayStream& stream,
    std::uint32_t absoluteDrawBegin,
    std::uint32_t drawCount) {
  dxmt9::encoders::EncodePartitionRangeSnapshot range{
      .kind =
          dxmt9::encoders::EncodePartitionRangeKind::DrawRunEntries,
      .replayOrdinalBegin = 0,
      .replayOrdinalCount = 1,
      .drawEntryCount = drawCount,
  };
  check(dxmt9::encoders::buildEncodePartitionEntrySnapshot(
            stream, 0, absoluteDrawBegin, range.entry),
        "explicit DrawRun entry snapshot builds");
  return range;
}

std::vector<dxmt9::encoders::EncodePartitionRangeSnapshot> makeDrawRanges(
    std::size_t slotIndex,
    const dxmt9::core::ChunkSlot& slot,
    std::span<const std::pair<std::uint32_t, std::uint32_t>> subranges) {
  const auto stream = dxmt9::encoders::makeEncodePartitionReplayStream(
      slotIndex, slot, kPartitionCommandIndex, 1u, false, {}, {},
      partitionSource(slotIndex, slot.seqId).source);
  check(stream.valid, "DrawRun fixture replay stream is valid");
  const std::uint32_t firstParam =
      slot.commandAt(kPartitionCommandIndex).drawRunRecord->firstParam;
  std::vector<dxmt9::encoders::EncodePartitionRangeSnapshot> ranges;
  ranges.reserve(subranges.size());
  for (const auto& [begin, count] : subranges) {
    ranges.push_back(makeDrawRange(stream, firstParam + begin, count));
  }
  return ranges;
}

void checkDrawRunCapture(
    const DrawRunCapture& capture,
    std::span<const std::pair<std::uint32_t, std::uint32_t>> subranges,
    std::size_t expectedRenderPassEnds) {
  checkEq(capture.drawRunBegins, std::size_t{1},
          "DrawRun command-level setup begins once");
  checkEq(capture.renderPassBegins, std::size_t{1},
          "DrawRun opens one render pass across subranges");
  checkEq(capture.renderPassEnds, expectedRenderPassEnds,
          "partition edges do not end the render pass");
  checkEq(capture.splitPolicyCalls, std::size_t{1},
          "per-record split policy runs once for the DrawRun command");
  checkEq(capture.uploadBatchCalls, std::size_t{1},
          "UP payload upload is batched once for the complete DrawRun");
  checkEq(capture.subranges.size(), subranges.size(),
          "encoder observes the exact DrawRun subrange count");
  for (std::size_t i = 0; i < subranges.size(); ++i) {
    checkEq(capture.subranges[i].first,
            static_cast<std::size_t>(kPartitionFirstParam +
                                     subranges[i].first),
            "subrange retains its absolute DrawParam begin");
    checkEq(capture.subranges[i].second,
            static_cast<std::size_t>(subranges[i].second),
            "subrange retains its draw count");
  }

  checkEq(capture.unexpectedNonIndexedDraws, std::size_t{0},
          "indexed fixture never changes draw issue shape");
  checkEq(capture.vertexOffsets.size(), kPartitionDrawCount,
          "every DrawRun vertex payload is bound once");
  checkEq(capture.indexOffsets.size(), kPartitionDrawCount,
          "every DrawRun index payload is drawn once");
  checkEq(capture.indexCounts.size(), kPartitionDrawCount,
          "every DrawRun draw count is recorded once");
  for (std::size_t i = 0; i < kPartitionDrawCount; ++i) {
    checkEq(capture.vertexOffsets[i], 1000u + i * 200u,
            "UP vertex slice uses the absolute DrawRun index");
    checkEq(capture.indexOffsets[i], 1100u + i * 200u,
            "UP index slice uses the absolute DrawRun index");
    checkEq(capture.indexCounts[i], (i + 1u) * 3u,
            "every source DrawParam emits exactly once in order");
  }
}

void encodeChunkThenFinalizerPublishesAndResetsOneSession() {
  Harness harness;
  auto ctx = harness.makeContext();
  auto session = dxmt9::encoders::makeEncodeChunkSession();
  check(static_cast<bool>(session), "session owner is created");

  dxmt9::core::ChunkSlot headSlot{};
  headSlot.seqId = 41;
  dxmt9::core::ClearDesc headClear{};
  headClear.clearColor = true;
  headSlot.appendClear(headClear);
  const dxmt9::core::metalqueue::QueueCompletionSource headSource{
      .slotIndex = 2,
      .seqId = headSlot.seqId,
      .hasPresent = false,
      .commandBegin = 0,
      .commandCount = 1,
  };
  auto commandBuffer =
      retainedToken<WMT::CommandBuffer>("encode-session-cb-token");
  const obj_handle_t commandBufferHandle = commandBuffer.handle;

  auto head = dxmt9::encoders::encodeChunk(
      ctx, headSource.slotIndex, headSlot,
      deferredOptions(*session, std::move(commandBuffer), headSource));
  check(head.has_value(), "head encodeChunk returns a deferred submission");
  checkEq(head->commandBuffer.handle, commandBufferHandle,
          "head submission carries the injected command buffer");
  check(head->explicitCompletionSourceSpan().empty(),
        "deferred head does not publish session sources");
  check(dxmt9::encoders::encodeChunkSessionHasDeferredSubmissionPayload(
            *session),
        "head clear remains as deferred session payload");

  const auto sourcesAfterHead =
      dxmt9::encoders::encodeChunkSessionSources(*session);
  checkEq(sourcesAfterHead.size(), std::size_t{1},
          "head source is retained by the session");
  checkEq(sourcesAfterHead[0].seqId, headSource.seqId,
          "session retains the head sequence");

  dxmt9::core::ChunkSlot tailSlot{};
  tailSlot.seqId = 42;
  const dxmt9::core::metalqueue::QueueCompletionSource tailSource{
      .slotIndex = 3,
      .seqId = tailSlot.seqId,
      .hasPresent = false,
      .commandBegin = 0,
      .commandCount = 0,
  };
  auto tail = dxmt9::encoders::encodeChunk(
      ctx, tailSource.slotIndex, tailSlot,
      deferredOptions(*session, std::move(head->commandBuffer), tailSource));
  check(tail.has_value(), "tail encodeChunk returns a deferred submission");
  check(!head->commandBuffer,
        "tail encodeChunk consumes the head command-buffer carrier");
  checkEq(tail->commandBuffer.handle, commandBufferHandle,
          "tail submission carries the same command buffer");
  check(tail->explicitCompletionSourceSpan().empty(),
        "deferred tail still does not publish session sources");
  check(dxmt9::encoders::encodeChunkSessionHasDeferredSubmissionPayload(
            *session),
        "tail preserves the deferred clear until finalization");

  const auto sourcesBeforeFinalize =
      dxmt9::encoders::encodeChunkSessionSources(*session);
  checkEq(sourcesBeforeFinalize.size(), std::size_t{2},
          "both sources remain ordered in the session");
  checkEq(sourcesBeforeFinalize[0].seqId, headSource.seqId,
          "head remains first before finalization");
  checkEq(sourcesBeforeFinalize[1].seqId, tailSource.seqId,
          "tail remains second before finalization");

  check(dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, *tail),
        "finalizer publishes the deferred session");
  checkEq(tail->commandBuffer.handle, commandBufferHandle,
          "finalized submission keeps the command buffer");

  const auto published = tail->explicitCompletionSourceSpan();
  checkEq(published.size(), std::size_t{2},
          "finalizer publishes both completion sources");
  checkEq(published[0].slotIndex, headSource.slotIndex,
          "published head slot matches");
  checkEq(published[0].seqId, headSource.seqId,
          "published head sequence matches");
  check(!published[0].hasPresent,
        "published head remains non-present");
  checkEq(published[0].commandBegin, headSource.commandBegin,
          "published head command begin matches");
  checkEq(published[0].commandCount, headSource.commandCount,
          "published head command count matches");
  checkEq(published[1].slotIndex, tailSource.slotIndex,
          "published tail slot matches");
  checkEq(published[1].seqId, tailSource.seqId,
          "published tail sequence matches");
  check(!published[1].hasPresent,
        "published tail remains non-present");
  checkEq(published[1].commandBegin, tailSource.commandBegin,
          "published tail command begin matches");
  checkEq(published[1].commandCount, tailSource.commandCount,
          "published tail command count matches");

  check(dxmt9::encoders::encodeChunkSessionSources(*session).empty(),
        "successful finalization resets session sources");
  check(!dxmt9::encoders::encodeChunkSessionHasActiveRender(*session),
        "successful finalization leaves no active render encoder");
  check(!dxmt9::encoders::encodeChunkSessionHasDeferredSubmissionPayload(
            *session),
        "successful finalization clears deferred session payload");
}

void finalizerValidationFailureIsNoMutationAndRetryable() {
  Harness harness;
  auto ctx = harness.makeContext();
  auto session = dxmt9::encoders::makeEncodeChunkSession();

  dxmt9::core::ChunkSlot headSlot{};
  headSlot.seqId = 51;
  dxmt9::core::ClearDesc headClear{};
  headClear.clearColor = true;
  headSlot.appendClear(headClear);
  const dxmt9::core::metalqueue::QueueCompletionSource headSource{
      .slotIndex = 4,
      .seqId = headSlot.seqId,
      .hasPresent = false,
      .commandBegin = 0,
      .commandCount = 1,
  };
  auto commandBuffer =
      retainedToken<WMT::CommandBuffer>("encode-session-retry-cb-token");
  const obj_handle_t commandBufferHandle = commandBuffer.handle;
  auto head = dxmt9::encoders::encodeChunk(
      ctx, headSource.slotIndex, headSlot,
      deferredOptions(*session, std::move(commandBuffer), headSource));
  check(head.has_value(), "retry fixture head encode succeeds");

  dxmt9::core::ChunkSlot tailSlot{};
  tailSlot.seqId = 52;
  const dxmt9::core::metalqueue::QueueCompletionSource tailSource{
      .slotIndex = 5,
      .seqId = tailSlot.seqId,
      .hasPresent = false,
      .commandBegin = 0,
      .commandCount = 0,
  };
  auto tail = dxmt9::encoders::encodeChunk(
      ctx, tailSource.slotIndex, tailSlot,
      deferredOptions(*session, std::move(head->commandBuffer), tailSource));
  check(tail.has_value(), "retry fixture tail encode succeeds");

  const std::array<dxmt9::core::metalqueue::QueueCompletionSource, 1>
      mismatchedSources{
          dxmt9::core::metalqueue::QueueCompletionSource{
              .source = {
                  .id = {.index = 9, .generation = 99},
                  .storage = {
                      .firstPage = 9,
                      .pageCount = 1,
                      .generation = 99,
                  },
              },
              .slotIndex = 99,
              .seqId = 99,
              .hasPresent = false,
              .commandBegin = 0,
              .commandCount = 0,
          },
      };
  check(tail->assignFixedCompletionSources(mismatchedSources),
        "retry fixture installs a valid but conflicting source list");
  tail->postCommitCallbacks.emplace_back([] {});
  tail->completionCallbacks.emplace_back([] {});
  tail->retainedPayloads.emplace_back(std::make_shared<int>(7));

  const auto postCommitCount = tail->postCommitCallbacks.size();
  const auto completionCount = tail->completionCallbacks.size();
  const auto retainedPayloadCount = tail->retainedPayloads.size();
  check(!dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, *tail),
        "mismatched completion sources reject finalization");

  checkEq(tail->commandBuffer.handle, commandBufferHandle,
          "rejected finalization preserves record command-buffer ownership");
  checkEq(tail->postCommitCallbacks.size(), postCommitCount,
          "rejected finalization preserves record post-commit callbacks");
  checkEq(tail->completionCallbacks.size(), completionCount,
          "rejected finalization preserves record completion callbacks");
  checkEq(tail->retainedPayloads.size(), retainedPayloadCount,
          "rejected finalization preserves record retained payloads");
  const auto rejectedSources = tail->explicitCompletionSourceSpan();
  checkEq(rejectedSources.size(), std::size_t{1},
          "rejected finalization preserves conflicting record sources");
  checkEq(rejectedSources[0].seqId, std::uint64_t{99},
          "rejected finalization does not rewrite record source identity");

  const auto sessionSources =
      dxmt9::encoders::encodeChunkSessionSources(*session);
  checkEq(sessionSources.size(), std::size_t{2},
          "rejected finalization preserves all session sources");
  checkEq(sessionSources[0].seqId, headSource.seqId,
          "rejected finalization preserves the head source");
  checkEq(sessionSources[1].seqId, tailSource.seqId,
          "rejected finalization preserves the tail source");
  check(dxmt9::encoders::encodeChunkSessionHasDeferredSubmissionPayload(
            *session),
        "rejected finalization leaves the pending clear in session storage");

  tail->fixedCompletionSources.clear();
  check(dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, *tail),
        "corrected record retries finalization successfully");
  checkEq(tail->commandBuffer.handle, commandBufferHandle,
          "successful retry keeps the command buffer");
  const auto published = tail->explicitCompletionSourceSpan();
  checkEq(published.size(), std::size_t{2},
          "successful retry publishes both session sources");
  checkEq(published[0].seqId, headSource.seqId,
          "successful retry publishes the head source first");
  checkEq(published[1].seqId, tailSource.seqId,
          "successful retry publishes the tail source second");
  check(dxmt9::encoders::encodeChunkSessionSources(*session).empty(),
        "successful retry resets session sources");
  check(!dxmt9::encoders::encodeChunkSessionHasDeferredSubmissionPayload(
            *session),
        "successful retry clears deferred session payload");
}

void finalizerRejectsSourceAndStorageGenerationMismatches() {
  Harness harness;
  auto ctx = harness.makeContext();
  auto session = dxmt9::encoders::makeEncodeChunkSession();

  dxmt9::core::ChunkSlot slot{};
  slot.seqId = 57;
  slot.appendClear({});
  const dxmt9::core::metalqueue::QueueCompletionSource source{
      .source = {
          .id = {.index = 2, .generation = 7},
          .storage = {.firstPage = 3, .pageCount = 1, .generation = 11},
      },
      .slotIndex = 4,
      .seqId = slot.seqId,
      .hasPresent = false,
      .commandBegin = 0,
      .commandCount = 1,
  };
  auto submission = dxmt9::encoders::encodeChunk(
      ctx, source.slotIndex, slot,
      deferredOptions(
          *session,
          retainedToken<WMT::CommandBuffer>("encode-session-locator-cb-token"),
          source));
  check(submission.has_value(), "locator mismatch fixture encode succeeds");

  auto mismatched = source;
  ++mismatched.source.id.generation;
  const std::array sourceGenerationMismatch{mismatched};
  check(submission->assignFixedCompletionSources(sourceGenerationMismatch),
        "fixture installs source-generation mismatch");
  check(!dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, *submission),
        "same slot/seq/range with a different source generation is rejected");
  checkEq(dxmt9::encoders::encodeChunkSessionSources(*session).size(),
          std::size_t{1},
          "source-generation rejection preserves session ownership");

  mismatched = source;
  ++mismatched.source.storage.generation;
  const std::array storageGenerationMismatch{mismatched};
  submission->fixedCompletionSources.clear();
  check(submission->assignFixedCompletionSources(storageGenerationMismatch),
        "fixture installs storage-generation mismatch");
  check(!dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, *submission),
        "same slot/seq/range with a different storage generation is rejected");
  checkEq(dxmt9::encoders::encodeChunkSessionSources(*session).size(),
          std::size_t{1},
          "storage-generation rejection preserves session ownership");

  submission->fixedCompletionSources.clear();
  check(dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, *submission),
        "clearing the mismatched record permits locator-correct publication");
  check(submission->explicitCompletionSourceSpan().front().source ==
            source.source,
        "successful finalization publishes the full source locator");
}

void encodeChunkRejectsPartitionAndSessionLocatorMismatchBeforeEffects() {
  Harness harness;
  auto ctx = harness.makeContext();
  auto session = dxmt9::encoders::makeEncodeChunkSession();
  dxmt9::core::ChunkSlot slot{};
  slot.seqId = 58;
  slot.appendClear({});
  const auto source = partitionSource(4, slot.seqId);
  auto options = deferredOptions(
      *session,
      retainedToken<WMT::CommandBuffer>("partition-source-mismatch-cb-token"),
      source);
  ++options.partitionSource.storage.generation;

  const auto submission = dxmt9::encoders::encodeChunk(
      ctx, source.slotIndex, slot, std::move(options));
  check(!submission.has_value(),
        "valid but different partition/session storage generation rejects encode");
  check(dxmt9::encoders::encodeChunkSessionSources(*session).empty(),
        "locator mismatch appends no session completion source");
  check(!dxmt9::encoders::encodeChunkSessionHasActiveRender(*session) &&
            !dxmt9::encoders::encodeChunkSessionHasDeferredSubmissionPayload(
                *session),
        "locator mismatch is rejected before EncodeSession or Metal effects");
}

void arenaSourcePayloadExecutesThroughIdentitySerialEncodeChunk() {
  Harness harness;
  DrawRunCapture capture;
  auto recorder = makeDrawRunRecorder(
      capture, WMT::RenderCommandEncoder{
                   retainedToken<WMT::RenderCommandEncoder>(
                       "arena-source-render-encoder").handle});
  auto ctx = harness.makeContext();
  ctx.drawRecorder = &recorder;

  dxmt9::core::SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 1;
  capacity.clearRecords = 1;
  capacity.clearRects = 2;
  const auto layout = dxmt9::core::makeSourcePayloadLayout(
      capacity, 4096, 2);
  check(layout.has_value(), "arena clear layout is representable");
  std::vector<std::byte> memory(layout->usedBytes);
  dxmt9::core::ArenaSourcePayloadBlock block;
  dxmt9::core::ArenaSourcePayloadBuilder builder(
      block, *layout, std::span<std::byte>(memory));
  check(builder.good(), "arena clear builder binds final storage");

  dxmt9::core::ClearDesc clear{};
  clear.clearColor = true;
  clear.rects = {
      {.left = 1, .top = 2, .right = 11, .bottom = 12},
      {.left = 21, .top = 22, .right = 31, .bottom = 32},
  };
  check(builder.tryAppendClearCommand(clear) && builder.publish(),
        "arena multi-rect clear publishes");
  const dxmt9::core::SourcePayloadView payload(block);
  const auto sourceCommand = payload.commandAt(0);
  check(sourceCommand.clear.has_value() &&
            sourceCommand.clear->rects.size() == 2,
        "arena clear exposes its borrowed rect span");
  const auto* rectStorage = sourceCommand.clear->rects.data();

  dxmt9::encoders::EncodeChunkOptions options{};
  options.commandBuffer = retainedToken<WMT::CommandBuffer>(
      "arena-source-command-buffer");
  options.partitionSource = dxmt9::core::CpuReadyTape::SourceRef{
      .id = {.index = 3, .generation = 7},
      .storage = {.firstPage = 5, .pageCount = 1, .generation = 9},
  };
  auto submission = dxmt9::encoders::encodeChunk(
      ctx, 6, payload, 71, std::move(options));
  check(submission.has_value(),
        "arena SourcePayloadView produces a serial submission");
  checkEq(submission->seqId, std::uint64_t{71},
          "arena serial submission retains source seq");
  checkEq(capture.splitPolicyCalls, std::size_t{1},
          "arena clear command executes exactly once");
  check(payload.commandAt(0).clear->rects.data() == rectStorage,
        "arena multi-rect clear remains backed by source storage");
}

void explicitDrawRunSubrangesExecuteThroughEncodeChunk() {
  const std::array<std::pair<std::uint32_t, std::uint32_t>, 2> twoSubranges{
      std::pair{0u, 2u},
      std::pair{2u, 3u},
  };
  const std::array<std::pair<std::uint32_t, std::uint32_t>, 3> threeSubranges{
      std::pair{0u, 1u},
      std::pair{1u, 2u},
      std::pair{3u, 2u},
  };

  auto run = [](std::uint64_t seqId,
                std::span<const std::pair<std::uint32_t, std::uint32_t>>
                    subranges) {
    Harness harness;
    DrawRunCapture capture;
    auto renderEncoderOwner =
        retainedToken<WMT::RenderCommandEncoder>("partition-render-encoder");
    auto recorder = makeDrawRunRecorder(
        capture, WMT::RenderCommandEncoder{renderEncoderOwner.handle});
    auto ctx = harness.makeContext();
    ctx.drawRecorder = &recorder;

    constexpr std::size_t slotIndex = 8u;
    auto slot = makeDrawRunSlot(seqId);
    auto ranges = makeDrawRanges(slotIndex, slot, subranges);
    dxmt9::encoders::EncodeChunkOptions options{};
    options.commandBuffer =
        retainedToken<WMT::CommandBuffer>("partition-command-buffer");
    options.partitionRanges = ranges;
    options.sessionSource = partitionSource(slotIndex, slot.seqId);
    options.partitionSource = options.sessionSource->source;
    auto submission = dxmt9::encoders::encodeChunk(
        ctx, slotIndex, slot, std::move(options));
    check(submission.has_value(),
          "explicit DrawRun encodeChunk returns a submission");
    checkDrawRunCapture(capture, subranges, 1u);
  };

  run(71u, twoSubranges);
  run(72u, threeSubranges);
}

void invalidDrawRunPlanFallsBackBeforeEncodeChunkSideEffects() {
  Harness harness;
  DrawRunCapture capture;
  auto renderEncoderOwner =
      retainedToken<WMT::RenderCommandEncoder>("fallback-render-encoder");
  auto recorder = makeDrawRunRecorder(
      capture, WMT::RenderCommandEncoder{renderEncoderOwner.handle});
  auto ctx = harness.makeContext();
  ctx.drawRecorder = &recorder;

  constexpr std::size_t slotIndex = 9u;
  auto slot = makeDrawRunSlot(73u);
  const std::array<std::pair<std::uint32_t, std::uint32_t>, 2> gapPlan{
      std::pair{0u, 1u},
      std::pair{2u, 3u},
  };
  auto ranges = makeDrawRanges(slotIndex, slot, gapPlan);
  dxmt9::encoders::EncodeChunkOptions options{};
  options.commandBuffer =
      retainedToken<WMT::CommandBuffer>("fallback-command-buffer");
  options.partitionRanges = ranges;
  options.sessionSource = partitionSource(slotIndex, slot.seqId);
  options.partitionSource = options.sessionSource->source;
  auto submission = dxmt9::encoders::encodeChunk(
      ctx, slotIndex, slot, std::move(options));
  check(submission.has_value(),
        "invalid explicit DrawRun plan falls back to identity");

  const std::array<std::pair<std::uint32_t, std::uint32_t>, 1> identityRange{
      std::pair{0u, static_cast<std::uint32_t>(kPartitionDrawCount)},
  };
  checkDrawRunCapture(capture, identityRange, 1u);
}

void explicitDrawRunDeferredSessionRetainsSourceUntilFinalizer() {
  Harness harness;
  DrawRunCapture capture;
  auto renderEncoderOwner =
      retainedToken<WMT::RenderCommandEncoder>("session-render-encoder");
  auto recorder = makeDrawRunRecorder(
      capture, WMT::RenderCommandEncoder{renderEncoderOwner.handle});
  auto ctx = harness.makeContext();
  ctx.drawRecorder = &recorder;
  auto session = dxmt9::encoders::makeEncodeChunkSession();

  constexpr std::size_t slotIndex = 10u;
  auto slot = makeDrawRunSlot(74u);
  const std::array<std::pair<std::uint32_t, std::uint32_t>, 2> subranges{
      std::pair{0u, 2u},
      std::pair{2u, 3u},
  };
  auto ranges = makeDrawRanges(slotIndex, slot, subranges);
  const auto source = partitionSource(slotIndex, slot.seqId);
  auto options = deferredOptions(
      *session,
      retainedToken<WMT::CommandBuffer>("session-partition-command-buffer"),
      source);
  options.partitionRanges = ranges;
  auto submission = dxmt9::encoders::encodeChunk(
      ctx, slotIndex, slot, std::move(options));
  check(submission.has_value(),
        "deferred explicit DrawRun returns a submission carrier");
  checkDrawRunCapture(capture, subranges, 0u);
  check(dxmt9::encoders::encodeChunkSessionHasActiveRender(*session),
        "partition edges preserve the deferred active render pass");
  const auto retainedSources =
      dxmt9::encoders::encodeChunkSessionSources(*session);
  checkEq(retainedSources.size(), std::size_t{1},
          "deferred explicit DrawRun retains one source");
  checkEq(retainedSources.front().slotIndex, source.slotIndex,
          "deferred explicit DrawRun retains source slot identity");
  checkEq(retainedSources.front().seqId, source.seqId,
          "deferred explicit DrawRun retains source sequence");

  check(dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, *submission),
        "explicit DrawRun session finalizes into its submission");
  checkEq(capture.renderPassEnds, std::size_t{1},
          "session finalizer ends the DrawRun render pass once");
  const auto published = submission->explicitCompletionSourceSpan();
  checkEq(published.size(), std::size_t{1},
          "DrawRun finalizer publishes its retained source");
  checkEq(published.front().slotIndex, source.slotIndex,
          "published DrawRun source keeps its slot identity");
  checkEq(published.front().seqId, source.seqId,
          "published DrawRun source keeps its sequence");
  check(!dxmt9::encoders::encodeChunkSessionHasActiveRender(*session),
        "DrawRun finalization clears the active render pass");
}

void partialCommandSegmentSessionFinalizesWithoutTargets() {
  Harness harness;
  auto ctx = harness.makeContext();
  auto session = dxmt9::encoders::makeEncodeChunkSession();

  dxmt9::core::ChunkSlot slot{};
  slot.seqId = 61;
  dxmt9::core::ClearDesc clear{};
  clear.clearColor = true;
  slot.appendClear(clear);
  slot.appendClear(clear);
  slot.appendClear(clear);
  const dxmt9::core::metalqueue::QueueCompletionSource source{
      .slotIndex = 6,
      .seqId = slot.seqId,
      .hasPresent = false,
      .commandBegin = 1,
      .commandCount = 1,
  };
  const std::array<dxmt9::encoders::EncodePartitionRangeSnapshot, 1>
      partitionRanges{
          dxmt9::encoders::EncodePartitionRangeSnapshot{
              .kind =
                  dxmt9::encoders::EncodePartitionRangeKind::CommandSegment,
              .replayOrdinalBegin = 0,
              .replayOrdinalCount = 1,
              .drawEntryCount = 0,
          },
      };
  auto options = deferredOptions(
      *session,
      retainedToken<WMT::CommandBuffer>("partial-command-segment-cb-token"),
      source);
  options.partitionRanges = partitionRanges;
  auto submission = dxmt9::encoders::encodeChunk(
      ctx, source.slotIndex, slot, std::move(options));
  check(submission.has_value(),
        "partial targetless CommandSegment returns a deferred submission");
  check(dxmt9::encoders::encodeChunkSessionHasDeferredSubmissionPayload(
            *session),
        "partial targetless CommandSegment retains its pending clear");
  const auto sources = dxmt9::encoders::encodeChunkSessionSources(*session);
  checkEq(sources.size(), std::size_t{1},
          "partial session retains exactly one completion source");
  checkEq(sources.front().commandBegin, std::size_t{1},
          "partial session retains the absolute selected command begin");
  checkEq(sources.front().commandCount, std::size_t{1},
          "partial session retains only the selected command count");

  check(dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, *submission),
        "partial deferred CommandSegment session finalizes");
  const auto published = submission->explicitCompletionSourceSpan();
  checkEq(published.size(), std::size_t{1},
          "partial finalization publishes one completion source");
  checkEq(published.front().commandBegin, std::size_t{1},
          "partial finalization preserves the selected command begin");
  check(!dxmt9::encoders::encodeChunkSessionHasDeferredSubmissionPayload(
            *session),
        "partial finalization clears targetless deferred payload");
}

}  // namespace

int main() {
  setenv("DXMT9_PERF_ENCODER_GPU_TIME", "0", 1);
  setenv("DXMT_METAL_CAPTURE_FRAME", "0", 1);
  try {
    encodeChunkThenFinalizerPublishesAndResetsOneSession();
    finalizerValidationFailureIsNoMutationAndRetryable();
    finalizerRejectsSourceAndStorageGenerationMismatches();
    encodeChunkRejectsPartitionAndSessionLocatorMismatchBeforeEffects();
    arenaSourcePayloadExecutesThroughIdentitySerialEncodeChunk();
    explicitDrawRunSubrangesExecuteThroughEncodeChunk();
    invalidDrawRunPlanFallsBackBeforeEncodeChunkSideEffects();
    explicitDrawRunDeferredSessionRetainsSourceUntilFinalizer();
    partialCommandSegmentSessionFinalizesWithoutTargets();
  } catch (const TestFailure& error) {
    std::cerr << "encode_session_lifecycle_spec failed: " << error.what()
              << '\n';
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "encode_session_lifecycle_spec unexpected exception: "
              << error.what() << '\n';
    return 1;
  }
  return 0;
}
