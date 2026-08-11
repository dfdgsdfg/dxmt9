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
#include "../../../src/dxmt9/dxmt9_encode_session_internal.hpp"
#include "../../../src/dxmt9/dxmt9_perf_counters.hpp"
#include "../../../src/dxmt9/dxmt9_pipeline_cache.hpp"
#include "../../../src/dxmt9/dxmt9_resource_pool.hpp"
#include "../../../src/dxmt9/dxmt9_ring_arena.hpp"
#include "../../../src/dxmt9/render/framegraph_backend.hpp"
#include "../framegraph/arena_payload_fixture.hpp"

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
  struct LateStoreResolution {
    std::uint8_t aspect = 0;
    std::uint8_t colorIndex = 0;
    std::uint32_t action = 0;
    std::uint8_t cause = 0;
  };

  std::size_t drawRunBegins = 0;
  std::size_t renderPassBegins = 0;
  std::size_t renderPassEnds = 0;
  std::size_t splitPolicyCalls = 0;
  std::size_t midChunkSplits = 0;
  std::size_t uploadBatchCalls = 0;
  std::size_t unexpectedNonIndexedDraws = 0;
  std::vector<std::size_t> drawRunCommands;
  std::vector<std::size_t> renderPassBeginCommands;
  std::vector<std::pair<std::size_t, std::size_t>> subranges;
  std::vector<std::uint64_t> vertexOffsets;
  std::vector<std::uint64_t> indexOffsets;
  std::vector<std::uint64_t> indexCounts;
  dxmt9::encoders::EncodeChunkSessionState* observeSessionAtDrawRun = nullptr;
  bool observedPendingClearAtDrawRun = false;
  std::optional<dxmt9::core::metalqueue::PublishedCommandRef>
      pendingClearAtDrawRun;
  dxmt9::encoders::LateRenderPassStoreState lateStoreSeed{};
  std::size_t lateStorePrepareCalls = 0;
  bool prepareLateStoreOnce = false;
  std::vector<LateStoreResolution> lateStoreResolutions;
};

void recordDrawRunBegin(void* userdata,
                        std::size_t commandIndex,
                        std::size_t) {
  auto* capture = static_cast<DrawRunCapture*>(userdata);
  ++capture->drawRunBegins;
  capture->drawRunCommands.push_back(commandIndex);
  if (capture->observeSessionAtDrawRun &&
      !capture->observedPendingClearAtDrawRun) {
    capture->pendingClearAtDrawRun =
        dxmt9::encoders::encodeChunkSessionPendingClearCommand(
            *capture->observeSessionAtDrawRun);
    capture->observedPendingClearAtDrawRun = true;
  }
}

void recordDrawSubrange(void* userdata,
                        std::size_t,
                        std::size_t absoluteDrawParamBegin,
                        std::size_t drawCount) {
  static_cast<DrawRunCapture*>(userdata)->subranges.push_back(
      {absoluteDrawParamBegin, drawCount});
}

void recordRenderPassBegin(void* userdata, std::size_t commandIndex) {
  auto* capture = static_cast<DrawRunCapture*>(userdata);
  ++capture->renderPassBegins;
  capture->renderPassBeginCommands.push_back(commandIndex);
}

void recordRenderPassEnd(void* userdata) {
  ++static_cast<DrawRunCapture*>(userdata)->renderPassEnds;
}

void prepareLateStore(void* userdata,
                      dxmt9::encoders::LateRenderPassStoreState& state) {
  auto* capture = static_cast<DrawRunCapture*>(userdata);
  ++capture->lateStorePrepareCalls;
  if (!capture->prepareLateStoreOnce || capture->lateStorePrepareCalls == 1u) {
    state = capture->lateStoreSeed;
  }
}

void recordLateStoreResolution(void* userdata,
                               std::uint8_t aspect,
                               std::uint8_t colorIndex,
                               std::uint32_t action,
                               std::uint8_t cause) {
  static_cast<DrawRunCapture*>(userdata)->lateStoreResolutions.push_back({
      .aspect = aspect,
      .colorIndex = colorIndex,
      .action = action,
      .cause = cause,
  });
}

void recordSplitPolicy(void* userdata, bool) {
  ++static_cast<DrawRunCapture*>(userdata)->splitPolicyCalls;
}

WMT::Reference<WMT::CommandBuffer> recordCommandBufferSplit(
    void* userdata, WMT::CommandBuffer) {
  ++static_cast<DrawRunCapture*>(userdata)->midChunkSplits;
  return retainedToken<WMT::CommandBuffer>(
      "encode-session-split-command-buffer-token");
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
      .prepareLateRenderPassStoreState = prepareLateStore,
      .resolveLateRenderPassStoreAction = recordLateStoreResolution,
      .endRenderPass = recordRenderPassEnd,
      .applyPerRecordSplitPolicy = recordSplitPolicy,
      .splitCommandBufferForTest = recordCommandBufferSplit,
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

dxmt9::core::CpuReadyTape::SourceRef sourceRefFor(
    std::size_t slotIndex, std::uint64_t seqId) {
  return dxmt9::core::CpuReadyTape::SourceRef{
      .id = {
          .index = static_cast<std::uint32_t>(slotIndex),
          .generation = seqId,
      },
      .storage = {
          .firstPage = static_cast<std::uint32_t>(slotIndex),
          .pageCount = 1,
          .generation = seqId,
      },
  };
}

dxmt9::encoders::EncodeChunkOptions deferredOptions(
    dxmt9::encoders::EncodeChunkSessionState& session,
    WMT::Reference<WMT::CommandBuffer> commandBuffer,
    dxmt9::core::metalqueue::QueueCompletionSource source) {
  if (!source.source.valid()) {
    source.source = sourceRefFor(source.slotIndex, source.seqId);
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

dxmt9::core::metalqueue::QueueCompletionSource fullSource(
    std::size_t slotIndex, std::uint64_t seqId, std::size_t commandCount) {
  return dxmt9::core::metalqueue::QueueCompletionSource{
      .source = sourceRefFor(slotIndex, seqId),
      .slotIndex = slotIndex,
      .seqId = seqId,
      .hasPresent = false,
      .commandBegin = 0,
      .commandCount = commandCount,
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

dxmt9::core::ChunkSlot makeProductionPlannerDrawRunSlot(
    std::uint64_t seqId, std::size_t drawCount) {
  std::vector<dxmt9::core::DrawParam> draws(drawCount);
  std::vector<dxmt9::core::DrawParamPayloadView> payloads(drawCount);
  std::vector<std::array<std::uint8_t, 64>> vertexPayloads(drawCount);
  std::vector<std::array<std::uint8_t, 32>> indexPayloads(drawCount);
  for (std::size_t i = 0u; i < drawCount; ++i) {
    draws[i].primitiveType = dxmt9::core::PrimitiveType::TriangleList;
    draws[i].primitiveCount = 1u;
    draws[i].indexed = true;
    draws[i].indexType = dxmt9::core::IndexType::UInt16;
    draws[i].instanceCount = 1u;
    payloads[i].userVertexData = vertexPayloads[i];
    payloads[i].userIndexData = indexPayloads[i];
  }
  dxmt9::core::ChunkSlot slot{};
  slot.seqId = seqId;
  slot.appendDrawRun(makeDrawRunState(),
                     dxmt9::core::DrawUniformPayload{}, draws, payloads);
  return slot;
}

dxmt9::core::ChunkSlot makeTargetDrawSlot(
    std::uint64_t seqId, std::span<const std::uint64_t> colorHandles) {
  dxmt9::core::ChunkSlot slot{};
  slot.seqId = seqId;
  for (const std::uint64_t colorHandle : colorHandles) {
    auto state = makeDrawRunState();
    state.hot.colorAttachments[0].handle =
        dxmt9::core::Handle{colorHandle};
    state.hot.colorAttachments[0].sampleCount = 1;
    state.hot.renderTargetMask = 1u;
    dxmt9::core::DrawParam draw{};
    draw.primitiveType = dxmt9::core::PrimitiveType::TriangleList;
    draw.primitiveCount = 1;
    draw.instanceCount = 1;
    const std::array draws{draw};
    const std::array<dxmt9::core::DrawParamPayloadView, 1> payloads{};
    slot.appendDrawRun(state, dxmt9::core::DrawUniformPayload{}, draws,
                       payloads);
  }
  return slot;
}

void appendTargetDraw(dxmt9::core::ChunkSlot& slot,
                      std::uint64_t colorHandle,
                      std::uint64_t sampledHandle = 0) {
  auto state = makeDrawRunState();
  state.hot.colorAttachments[0].handle =
      dxmt9::core::Handle{colorHandle};
  state.hot.colorAttachments[0].sampleCount = 1;
  state.hot.renderTargetMask = 1u;
  if (sampledHandle != 0) {
    state.hot.textures[0] = dxmt9::core::Handle{sampledHandle};
    state.hot.textureMask = 1u;
  }
  dxmt9::core::DrawParam draw{};
  draw.primitiveType = dxmt9::core::PrimitiveType::TriangleList;
  draw.primitiveCount = 1;
  draw.instanceCount = 1;
  const std::array draws{draw};
  const std::array<dxmt9::core::DrawParamPayloadView, 1> payloads{};
  slot.appendDrawRun(state, dxmt9::core::DrawUniformPayload{}, draws,
                     payloads);
}

dxmt9::core::ChunkSlot makeActiveSeedOutcomeSlot(
    std::uint64_t seqId, std::uint64_t targetA, std::uint64_t targetB,
    bool bSamplesA, bool aSamplesB) {
  dxmt9::core::ChunkSlot slot{};
  slot.seqId = seqId;
  dxmt9::core::ClearDesc clear{};
  clear.clearColor = true;
  clear.colorAttachments[0].handle = dxmt9::core::Handle{targetB};
  clear.colorAttachments[0].sampleCount = 1;
  slot.appendClear(clear);
  appendTargetDraw(slot, targetB, bSamplesA ? targetA : 0);
  appendTargetDraw(slot, targetA, aSamplesB ? targetB : 0);
  return slot;
}

dxmt9::core::ChunkSlot makeMovedHeadReturnSlot(
    std::uint64_t seqId, std::uint64_t targetA, std::uint64_t targetB) {
  dxmt9::core::ChunkSlot slot{};
  slot.seqId = seqId;
  appendTargetDraw(slot, targetA);
  appendTargetDraw(slot, targetB);
  // B produces data sampled by the returning A, so passcoalesce must move B
  // before the A-A merge. The complete optimized order is B,A1,A2.
  appendTargetDraw(slot, targetA, targetB);
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
  const auto pendingAfterHead =
      dxmt9::encoders::encodeChunkSessionPendingClearCommand(*session);
  check(pendingAfterHead.has_value() && pendingAfterHead->valid(),
        "deferred clear retains a valid command identity");
  check(pendingAfterHead->source ==
            sourceRefFor(headSource.slotIndex, headSource.seqId),
        "deferred clear retains the head Tape generations");
  checkEq(pendingAfterHead->seqId, headSource.seqId,
          "deferred clear retains the head sequence");
  checkEq(pendingAfterHead->slotIndex,
          static_cast<std::uint32_t>(headSource.slotIndex),
          "deferred clear retains the head diagnostic slot");
  checkEq(pendingAfterHead->commandIndex, std::uint32_t{0},
          "deferred clear retains its logical command index");

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
  const auto pendingAfterTail =
      dxmt9::encoders::encodeChunkSessionPendingClearCommand(*session);
  check(pendingAfterTail == pendingAfterHead,
        "source-B encode cannot rewrite source-A deferred clear identity");

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
  check(!dxmt9::encoders::encodeChunkSessionPendingClearCommand(*session)
             .has_value(),
        "successful finalization clears deferred command identity");
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

  tail->fixedCompletionSources.entries[0] = sessionSources[0];
  check(!dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, *tail),
        "record-list mutation rejects a stale completion shadow");
  checkEq(dxmt9::encoders::encodeChunkSessionSources(*session).size(),
          std::size_t{2},
          "shadow mismatch rejection remains retryable and preserves session");

  tail->clearFixedCompletionSources();
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
  submission->clearFixedCompletionSources();
  check(submission->assignFixedCompletionSources(storageGenerationMismatch),
        "fixture installs storage-generation mismatch");
  check(!dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, *submission),
        "same slot/seq/range with a different storage generation is rejected");
  checkEq(dxmt9::encoders::encodeChunkSessionSources(*session).size(),
          std::size_t{1},
          "storage-generation rejection preserves session ownership");

  submission->clearFixedCompletionSources();
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

void productionPartitionModeSubdividesWithoutChangingMetalShape() {
  auto run = [](std::uint64_t seqId,
                dxmt9::render::PartitionExecutionMode mode) {
    Harness harness;
    DrawRunCapture capture;
    auto renderEncoderOwner = retainedToken<WMT::RenderCommandEncoder>(
        "production-partition-render-encoder");
    auto recorder = makeDrawRunRecorder(
        capture, WMT::RenderCommandEncoder{renderEncoderOwner.handle});
    auto ctx = harness.makeContext();
    ctx.drawRecorder = &recorder;

    constexpr std::size_t slotIndex = 18u;
    auto slot = makeProductionPlannerDrawRunSlot(
        seqId, dxmt9::encoders::kProductionPartitionDrawThreshold);
    dxmt9::encoders::EncodeChunkOptions options{};
    options.commandBuffer = retainedToken<WMT::CommandBuffer>(
        "production-partition-command-buffer");
    options.partitionExecutionMode = mode;
    options.sessionSource = fullSource(
        slotIndex, slot.seqId, slot.commandCount());
    options.partitionSource = options.sessionSource->source;
    const auto submission = dxmt9::encoders::encodeChunk(
        ctx, slotIndex, slot, std::move(options));
    check(submission.has_value(),
          "production partition mode returns a serial submission");
    return capture;
  };

  const auto identity = run(
      181u, dxmt9::render::PartitionExecutionMode::IdentitySerial);
  const auto explicitSerial = run(
      182u, dxmt9::render::PartitionExecutionMode::ExplicitSerial);
  checkEq(identity.subranges.size(), std::size_t{1},
          "identity mode consumes one full DrawRun range");
  checkEq(explicitSerial.subranges.size(), std::size_t{2},
          "production serial mode consumes two deterministic subranges");
  checkEq(explicitSerial.subranges[0].second, std::size_t{32},
          "production first subrange uses the target draw count");
  checkEq(explicitSerial.subranges[1].second, std::size_t{32},
          "production tail subrange covers all remaining draws");
  checkEq(explicitSerial.drawRunBegins, identity.drawRunBegins,
          "partition mode preserves command-once DrawRun setup");
  checkEq(explicitSerial.renderPassBegins, identity.renderPassBegins,
          "partition mode preserves render-pass begin shape");
  checkEq(explicitSerial.renderPassEnds, identity.renderPassEnds,
          "partition mode preserves render-pass end shape");
  checkEq(explicitSerial.splitPolicyCalls, identity.splitPolicyCalls,
          "partition mode preserves command-buffer split decisions");
  checkEq(explicitSerial.uploadBatchCalls, identity.uploadBatchCalls,
          "partition mode preserves one complete DrawRun upload batch");
  checkEq(explicitSerial.indexCounts.size(), identity.indexCounts.size(),
          "partition mode emits every source draw exactly once");
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

void closePassIsIdempotentAndSessionResumesOnSameCommandBuffer() {
  Harness harness;
  DrawRunCapture capture;
  auto renderEncoderOwner =
      retainedToken<WMT::RenderCommandEncoder>("close-pass-render-encoder");
  auto recorder = makeDrawRunRecorder(
      capture, WMT::RenderCommandEncoder{renderEncoderOwner.handle});
  auto ctx = harness.makeContext();
  ctx.drawRecorder = &recorder;
  auto session = dxmt9::encoders::makeEncodeChunkSession();

  constexpr std::size_t firstSlotIndex = 11u;
  auto firstSlot = makeDrawRunSlot(75u);
  const auto firstSource =
      partitionSource(firstSlotIndex, firstSlot.seqId);
  auto firstSubmission = dxmt9::encoders::encodeChunk(
      ctx, firstSlotIndex, firstSlot,
      deferredOptions(
          *session,
          retainedToken<WMT::CommandBuffer>("close-pass-command-buffer"),
          firstSource));
  check(firstSubmission.has_value(),
        "first deferred source returns its command-buffer carrier");
  check(dxmt9::encoders::encodeChunkSessionHasActiveRender(*session),
        "first deferred source leaves a render pass active");
  checkEq(capture.renderPassBegins, std::size_t{1},
          "first source begins one render pass");
  checkEq(capture.renderPassEnds, std::size_t{0},
          "deferred source does not end its render pass");
  const auto commandBufferHandle = firstSubmission->commandBuffer.handle;

  check(dxmt9::encoders::closeEncodeChunkSessionRenderPass(
            ctx, *session, *firstSubmission) ==
            dxmt9::encoders::EncodeChunkSessionPassCloseResult::Closed,
        "ordered-control ClosePass closes an active pass");
  check(!dxmt9::encoders::encodeChunkSessionHasActiveRender(*session),
        "ClosePass clears only the active render-pass state");
  checkEq(capture.renderPassEnds, std::size_t{1},
          "ClosePass executes the production pass-end path once");
  checkEq(firstSubmission->commandBuffer.handle, commandBufferHandle,
          "ClosePass preserves the command-buffer carrier");
  check(firstSubmission->explicitCompletionSourceSpan().empty(),
        "ClosePass does not publish completion ownership");
  checkEq(dxmt9::encoders::encodeChunkSessionSources(*session).size(),
          std::size_t{1},
          "ClosePass preserves retained session sources");

  check(dxmt9::encoders::closeEncodeChunkSessionRenderPass(
            ctx, *session, *firstSubmission) ==
            dxmt9::encoders::EncodeChunkSessionPassCloseResult::NoActivePass,
        "repeated ClosePass is an idempotent no-op");
  checkEq(capture.renderPassEnds, std::size_t{1},
          "idempotent ClosePass does not repeat pass-end side effects");

  constexpr std::size_t secondSlotIndex = 12u;
  auto secondSlot = makeDrawRunSlot(76u);
  const auto secondSource =
      partitionSource(secondSlotIndex, secondSlot.seqId);
  auto secondSubmission = dxmt9::encoders::encodeChunk(
      ctx, secondSlotIndex, secondSlot,
      deferredOptions(*session, std::move(firstSubmission->commandBuffer),
                      secondSource));
  check(secondSubmission.has_value(),
        "session resumes encoding after an ordered-control ClosePass");
  checkEq(secondSubmission->commandBuffer.handle, commandBufferHandle,
          "resumed session keeps the same command-buffer chain");
  check(dxmt9::encoders::encodeChunkSessionHasActiveRender(*session),
        "later source opens a new pass in the retained session");
  checkEq(capture.renderPassBegins, std::size_t{2},
          "later source begins exactly one replacement render pass");
  checkEq(dxmt9::encoders::encodeChunkSessionSources(*session).size(),
          std::size_t{2},
          "resumed session preserves ordered source ownership");

  check(dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, *secondSubmission),
        "resumed session finalizes into its tail carrier");
  checkEq(capture.renderPassEnds, std::size_t{2},
          "finalizer ends only the resumed active pass");
  const auto published = secondSubmission->explicitCompletionSourceSpan();
  checkEq(published.size(), std::size_t{2},
          "finalizer publishes both retained sources");
  checkEq(published[0].seqId, firstSource.seqId,
          "finalizer preserves first source order");
  checkEq(published[1].seqId, secondSource.seqId,
          "finalizer preserves second source order");
}

void closedEncoderFrontierAllowsMovedHeadOnSameCommandBuffer() {
  Harness harness;
  DrawRunCapture capture;
  auto renderEncoderOwner = retainedToken<WMT::RenderCommandEncoder>(
      "closed-frontier-render-encoder");
  auto recorder = makeDrawRunRecorder(
      capture, WMT::RenderCommandEncoder{renderEncoderOwner.handle});
  auto ctx = harness.makeContext();
  ctx.drawRecorder = &recorder;
  auto session = dxmt9::encoders::makeEncodeChunkSession();
  using FrontierState =
      dxmt9::encoders::EncodeSessionReplayFrontierState;

  check(dxmt9::encoders::encodeChunkSessionReplayFrontierState(*session) ==
            FrontierState::CleanClosedEncoderNoPendingClear,
        "fresh session starts at the clean closed-encoder frontier");

  constexpr std::uint64_t kTargetA = 0xA410u;
  constexpr std::uint64_t kTargetB = 0xB410u;
  auto headSlot = makeTargetDrawSlot(191u, std::array{kTargetA});
  const auto headSource =
      fullSource(31u, headSlot.seqId, headSlot.commandCount());
  auto head = dxmt9::encoders::encodeChunk(
      ctx, headSource.slotIndex, headSlot,
      deferredOptions(
          *session,
          retainedToken<WMT::CommandBuffer>("closed-frontier-command-buffer"),
          headSource));
  check(head.has_value(), "prior A source opens the deferred session");
  const obj_handle_t commandBufferHandle = head->commandBuffer.handle;
  check(dxmt9::encoders::encodeChunkSessionReplayFrontierState(*session) ==
            FrontierState::ActiveRenderComplete,
        "prior A exposes a complete active-render frontier");
  check(dxmt9::encoders::closeEncodeChunkSessionRenderPass(
            ctx, *session, *head) ==
            dxmt9::encoders::EncodeChunkSessionPassCloseResult::Closed,
        "ordered close ends prior A without finalizing its carrier");
  check(dxmt9::encoders::encodeChunkSessionReplayFrontierState(*session) ==
            FrontierState::CleanClosedEncoderNoPendingClear,
        "closed prior pass exposes a clean frontier with earlier CB work");

  auto currentSlot = makeMovedHeadReturnSlot(
      192u, kTargetA, kTargetB);
  dxmt9::tests::framegraph::ArenaPayloadFixture current(currentSlot);
  check(current.valid(), "moved-head Arena source publishes");
  const auto currentSource = fullSource(
      32u, currentSlot.seqId, currentSlot.commandCount());
  auto options = deferredOptions(
      *session, std::move(head->commandBuffer), currentSource);
  const auto outcomeBefore =
      dxmt9::perf::test::snapshotFramegraphSourceLocalReplayOutcome();
  dxmt9::render::FrameGraphBackend backend;
  auto tail = backend.onSourceReady(ctx, currentSource.slotIndex,
                                    current.view(), currentSlot.seqId,
                                    std::move(options));
  check(tail.has_value(), "clean closed frontier appends moved-head replay");
  const auto outcomeAfter =
      dxmt9::perf::test::snapshotFramegraphSourceLocalReplayOutcome();
  check(capture.drawRunCommands ==
            std::vector<std::size_t>({0u, 1u, 0u, 2u}),
        "prior A then current B,A1,A2 executes the proved moved-head order");
  checkEq(tail->commandBuffer.handle, commandBufferHandle,
          "moved-head replay remains on the prior command buffer");
  checkEq(capture.renderPassBegins, std::size_t{3},
          "prior A, current B, and merged current A open three passes total");
  checkEq(capture.renderPassEnds, std::size_t{2},
          "ordered close and current B-to-A transition end two passes");
  checkEq(outcomeAfter.finalReorderedActivated.sources -
              outcomeBefore.finalReorderedActivated.sources,
          std::uint64_t{1},
          "clean frontier activates one source-local reordered plan");
  checkEq(outcomeAfter.frontierRollbackMovedHeadUnproved.sources -
              outcomeBefore.frontierRollbackMovedHeadUnproved.sources,
          std::uint64_t{0},
          "clean frontier does not report an unproved moved head");

  check(dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, *tail),
        "closed-frontier session finalizes once");
  checkEq(capture.renderPassEnds, std::size_t{3},
          "finalizer closes the merged current A pass");
  const auto published = tail->explicitCompletionSourceSpan();
  check(published.size() == 2 && published[0].seqId == headSource.seqId &&
            published[1].seqId == currentSource.seqId,
        "moved replay preserves FIFO source completion");
}

void pendingClearSidecarAloneCannotProveCleanFrontier() {
  using dxmt9::encoders::EncodeSessionReplayFrontierState;
  using dxmt9::encoders::encode_session::ReplayFrontierFacts;
  using dxmt9::encoders::encode_session::replayFrontierStateForFacts;

  check(replayFrontierStateForFacts(ReplayFrontierFacts{
            .hasPendingClearCommand = true,
        }) == EncodeSessionReplayFrontierState::PendingClear,
        "a sidecar-only partial pending clear remains conservative");
  check(replayFrontierStateForFacts(ReplayFrontierFacts{}) ==
            EncodeSessionReplayFrontierState::
                CleanClosedEncoderNoPendingClear,
        "only an encoder-free state without either pending-clear half is clean");
}

void freshCleanFrontierAllowsMovedHead() {
  Harness harness;
  DrawRunCapture capture;
  auto renderEncoderOwner = retainedToken<WMT::RenderCommandEncoder>(
      "fresh-clean-frontier-render-encoder");
  auto recorder = makeDrawRunRecorder(
      capture, WMT::RenderCommandEncoder{renderEncoderOwner.handle});
  auto ctx = harness.makeContext();
  ctx.drawRecorder = &recorder;
  auto session = dxmt9::encoders::makeEncodeChunkSession();

  constexpr std::uint64_t kTargetA = 0xA420u;
  constexpr std::uint64_t kTargetB = 0xB420u;
  auto slot = makeMovedHeadReturnSlot(193u, kTargetA, kTargetB);
  dxmt9::tests::framegraph::ArenaPayloadFixture payload(slot);
  check(payload.valid(), "fresh clean moved-head source publishes");
  const auto source = fullSource(33u, slot.seqId, slot.commandCount());
  auto options = deferredOptions(
      *session,
      retainedToken<WMT::CommandBuffer>("fresh-clean-frontier-command-buffer"),
      source);
  dxmt9::render::FrameGraphBackend backend;
  auto tail = backend.onSourceReady(ctx, source.slotIndex, payload.view(),
                                    slot.seqId, std::move(options));
  check(tail.has_value(), "fresh clean session accepts moved-head replay");
  check(capture.drawRunCommands ==
            std::vector<std::size_t>({1u, 0u, 2u}),
        "fresh clean session executes B,A1,A2");
  check(dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, *tail),
        "fresh clean moved-head session finalizes");
  checkEq(tail->explicitCompletionSourceSpan().size(), std::size_t{1},
          "fresh clean finalizer publishes one source");
}

void pendingClearAndInjectedFrontiersKeepNaturalMovedHeadOrder() {
  constexpr std::uint64_t kTargetA = 0xA430u;
  constexpr std::uint64_t kTargetB = 0xB430u;

  {
    Harness harness;
    DrawRunCapture capture;
    auto renderEncoderOwner = retainedToken<WMT::RenderCommandEncoder>(
        "pending-clear-frontier-render-encoder");
    auto recorder = makeDrawRunRecorder(
        capture, WMT::RenderCommandEncoder{renderEncoderOwner.handle});
    auto ctx = harness.makeContext();
    ctx.drawRecorder = &recorder;
    auto session = dxmt9::encoders::makeEncodeChunkSession();

    dxmt9::core::ChunkSlot clearSlot{};
    clearSlot.seqId = 194u;
    dxmt9::core::ClearDesc clear{};
    clear.clearColor = true;
    clearSlot.appendClear(clear);
    const auto clearSource =
        fullSource(34u, clearSlot.seqId, clearSlot.commandCount());
    auto clearCarrier = dxmt9::encoders::encodeChunk(
        ctx, clearSource.slotIndex, clearSlot,
        deferredOptions(
            *session,
            retainedToken<WMT::CommandBuffer>(
                "pending-clear-frontier-command-buffer"),
            clearSource));
    check(clearCarrier.has_value(), "targetless clear creates a carrier");
    check(dxmt9::encoders::encodeChunkSessionReplayFrontierState(*session) ==
              dxmt9::encoders::EncodeSessionReplayFrontierState::PendingClear,
          "targetless clear exposes the pending-clear frontier");
    const auto pending =
        dxmt9::encoders::encodeChunkSessionPendingClearCommand(*session);
    check(pending.has_value() && pending->valid(),
          "pending-clear frontier retains a published command identity");
    capture.observeSessionAtDrawRun = session.get();

    auto currentSlot = makeMovedHeadReturnSlot(
        195u, kTargetA, kTargetB);
    dxmt9::tests::framegraph::ArenaPayloadFixture current(currentSlot);
    check(current.valid(), "pending-clear moved-head source publishes");
    const auto currentSource = fullSource(
        35u, currentSlot.seqId, currentSlot.commandCount());
    auto options = deferredOptions(
        *session, std::move(clearCarrier->commandBuffer), currentSource);
    dxmt9::render::FrameGraphBackend backend;
    auto tail = backend.onSourceReady(ctx, currentSource.slotIndex,
                                      current.view(), currentSlot.seqId,
                                      std::move(options));
    check(tail.has_value(), "pending-clear source falls back and encodes");
    check(capture.drawRunCommands ==
              std::vector<std::size_t>({0u, 1u, 2u}),
          "pending-clear frontier retains natural A1,B,A2 order");
    check(capture.observedPendingClearAtDrawRun &&
              capture.pendingClearAtDrawRun == pending,
          "planning preserves PublishedCommandRef until natural replay consumes clear");
    check(!dxmt9::encoders::encodeChunkSessionPendingClearCommand(*session),
          "natural draw replay consumes the preserved pending clear");
    check(dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
              ctx, *session, *tail),
          "pending-clear fallback session finalizes");
    const auto published = tail->explicitCompletionSourceSpan();
    check(published.size() == 2 &&
              published[0].seqId == clearSource.seqId &&
              published[1].seqId == currentSource.seqId,
          "pending-clear fallback preserves FIFO source completion");
  }

  {
    Harness harness;
    DrawRunCapture capture;
    auto renderEncoderOwner = retainedToken<WMT::RenderCommandEncoder>(
        "injected-frontier-render-encoder");
    auto recorder = makeDrawRunRecorder(
        capture, WMT::RenderCommandEncoder{renderEncoderOwner.handle});
    auto ctx = harness.makeContext();
    ctx.drawRecorder = &recorder;
    auto slot = makeMovedHeadReturnSlot(196u, kTargetA, kTargetB);
    dxmt9::tests::framegraph::ArenaPayloadFixture payload(slot);
    check(payload.valid(), "injected moved-head source publishes");
    dxmt9::encoders::EncodeChunkOptions options{};
    options.commandBuffer = retainedToken<WMT::CommandBuffer>(
        "injected-frontier-command-buffer");
    options.partitionSource = sourceRefFor(36u, slot.seqId);
    dxmt9::render::FrameGraphBackend backend;
    auto submission = backend.onSourceReady(
        ctx, 36u, payload.view(), slot.seqId, std::move(options));
    check(submission.has_value(), "injected moved-head source encodes");
    check(capture.drawRunCommands ==
              std::vector<std::size_t>({0u, 1u, 2u}),
          "injected unknown frontier keeps natural A1,B,A2 order");
  }
}

void activeRenderSeedCarriesAcrossLegacyAndArenaSources() {
  Harness harness;
  DrawRunCapture capture;
  auto renderEncoderOwner = retainedToken<WMT::RenderCommandEncoder>(
      "active-seed-render-encoder");
  auto recorder = makeDrawRunRecorder(
      capture, WMT::RenderCommandEncoder{renderEncoderOwner.handle});
  auto ctx = harness.makeContext();
  ctx.drawRecorder = &recorder;
  auto session = dxmt9::encoders::makeEncodeChunkSession();

  constexpr std::uint64_t kTargetA = 0xA100u;
  constexpr std::uint64_t kTargetB = 0xB100u;
  constexpr std::size_t kLegacySlotIndex = 15u;
  const std::array legacyTargets{kTargetA};
  auto legacy = makeTargetDrawSlot(81u, legacyTargets);
  const auto legacySource = fullSource(
      kLegacySlotIndex, legacy.seqId, legacy.commandCount());
  auto head = dxmt9::encoders::encodeChunk(
      ctx, kLegacySlotIndex, legacy,
      deferredOptions(
          *session,
          retainedToken<WMT::CommandBuffer>("active-seed-command-buffer"),
          legacySource));
  check(head.has_value(), "Legacy A source opens a deferred session");
  check(dxmt9::encoders::encodeChunkSessionHasActiveRender(*session),
        "Legacy A source leaves its pass active");
  const auto active =
      dxmt9::encoders::encodeChunkSessionActiveRenderDependencySnapshot(
          *session);
  check(active.has_value() && active->complete,
        "active Legacy pass exposes a complete immutable dependency seed");
  checkEq(active->colorAttachments[0].value, kTargetA,
          "active seed retains attachment A");
  checkEq(active->dependencyCount, std::uint32_t{1},
          "active seed retains one bounded attachment dependency");

  constexpr std::size_t kArenaSlotIndex = 16u;
  const std::array arenaTargets{kTargetB, kTargetA};
  auto arenaSourceSlot = makeTargetDrawSlot(82u, arenaTargets);
  dxmt9::tests::framegraph::ArenaPayloadFixture arena(arenaSourceSlot);
  check(arena.valid(), "Arena B,A source publishes for mixed lifecycle");
  const auto arenaSource = fullSource(
      kArenaSlotIndex, arenaSourceSlot.seqId,
      arenaSourceSlot.commandCount());
  auto options = deferredOptions(*session, std::move(head->commandBuffer),
                                 arenaSource);
  dxmt9::render::FrameGraphBackend backend;
  auto tail = backend.onSourceReady(ctx, kArenaSlotIndex, arena.view(),
                                    arenaSourceSlot.seqId,
                                    std::move(options));
  check(tail.has_value(), "Arena B,A source appends through FrameGraph");
  checkEq(capture.renderPassBegins, std::size_t{2},
          "active A plus planned A,B uses two total render passes");
  checkEq(capture.renderPassEnds, std::size_t{1},
          "source-edge planning closes only on the real A-to-B transition");
  checkEq(capture.renderPassBeginCommands.size(), std::size_t{2},
          "only Legacy A and Arena B open render passes");
  checkEq(capture.renderPassBeginCommands[0], std::size_t{0},
          "Legacy A opens from its source head");
  checkEq(capture.renderPassBeginCommands[1], std::size_t{0},
          "Arena B opens after the reordered Arena A continuation");
  check(dxmt9::encoders::encodeChunkSessionHasActiveRender(*session),
        "Arena B remains active without a source-created boundary");

  check(dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, *tail),
        "mixed Legacy/Arena session finalizes into one carrier");
  checkEq(capture.renderPassEnds, std::size_t{2},
          "finalizer closes the only remaining Arena B pass");
  const auto published = tail->explicitCompletionSourceSpan();
  checkEq(published.size(), std::size_t{2},
          "mixed session publishes both ordered completion sources");
  checkEq(published[0].seqId, legacySource.seqId,
          "Legacy completion remains first");
  checkEq(published[1].seqId, arenaSource.seqId,
          "Arena completion remains second despite command reordering");
}

void multiSourceFragmentsCarryActivePassAcrossSeparateSources() {
  Harness harness;
  DrawRunCapture capture;
  auto renderEncoderOwner = retainedToken<WMT::RenderCommandEncoder>(
      "multi-source-fragment-render-encoder");
  auto recorder = makeDrawRunRecorder(
      capture, WMT::RenderCommandEncoder{renderEncoderOwner.handle});
  auto ctx = harness.makeContext();
  ctx.drawRecorder = &recorder;
  auto session = dxmt9::encoders::makeEncodeChunkSession();

  constexpr std::uint64_t kTargetA = 0xA180u;
  constexpr std::uint64_t kTargetB = 0xB180u;
  const std::array headTargets{kTargetA};
  auto headSlot = makeTargetDrawSlot(181u, headTargets);
  const auto headSource =
      fullSource(21u, headSlot.seqId, headSlot.commandCount());
  auto carrier = dxmt9::encoders::encodeChunk(
      ctx, headSource.slotIndex, headSlot,
      deferredOptions(
          *session,
          retainedToken<WMT::CommandBuffer>(
              "multi-source-fragment-command-buffer"),
          headSource));
  check(carrier.has_value() &&
            dxmt9::encoders::encodeChunkSessionHasActiveRender(*session),
        "head A opens the multi-source fragment session");
  const auto active =
      dxmt9::encoders::encodeChunkSessionActiveRenderDependencySnapshot(
          *session);
  check(active.has_value() && active->complete,
        "multi-source fragment planner receives a complete active A seed");

  const std::array bTargets{kTargetB};
  const std::array aTargets{kTargetA};
  auto bSlot = makeTargetDrawSlot(182u, bTargets);
  auto aSlot = makeTargetDrawSlot(183u, aTargets);
  dxmt9::tests::framegraph::ArenaPayloadFixture bPayload(bSlot);
  dxmt9::tests::framegraph::ArenaPayloadFixture aPayload(aSlot);
  check(bPayload.valid() && aPayload.valid(),
        "separate B and A payloads publish for replay planning");
  const std::array completionSources{
      fullSource(22u, bSlot.seqId, bSlot.commandCount()),
      fullSource(23u, aSlot.seqId, aSlot.commandCount()),
  };
  check(dxmt9::encoders::appendEncodeChunkSessionSources(
            *session, completionSources),
        "B then A completion sources pre-register once in FIFO order");

  dxmt9::framegraph::ActiveRenderPlanningSeed seed{};
  seed.targets.color[0] =
      dxmt9::framegraph::TextureHandle{active->colorAttachments[0].value};
  seed.targets.color_count = 1;
  seed.targets.sample_count = active->sampleCount;
  seed.dependency_count = active->dependencyCount;
  seed.complete = active->complete;
  for (std::size_t i = 0; i < active->dependencyCount; ++i) {
    seed.write_dependencies[i] = dxmt9::framegraph::ResourceHandle{
        active->writeDependencies[i].value};
  }
  const std::array planningSources{
      dxmt9::framegraph::MultiSourcePlanningSource{bPayload.view()},
      dxmt9::framegraph::MultiSourcePlanningSource{aPayload.view()},
  };
  const auto plan = dxmt9::framegraph::planMultiSourcePassCoalesceReplay(
      planningSources, &seed);
  const auto runs = dxmt9::framegraph::buildMultiSourceReplayRuns(
      planningSources, plan);
  check(plan.reordered() && runs.valid() && runs.runs.size() == 2 &&
            runs.runs[0].retainedSourceIndex == 1u &&
            runs.runs[1].retainedSourceIndex == 0u,
        "active A plans separate source A before B as whole-source runs");

  const std::array payloads{bPayload.view(), aPayload.view()};
  const std::array slotIndices{std::size_t{22}, std::size_t{23}};
  const std::array seqIds{bSlot.seqId, aSlot.seqId};
  std::vector<std::uint64_t> encodedSeqIds;
  for (const auto& run : runs.runs) {
    const std::size_t sourceIndex = run.retainedSourceIndex;
    dxmt9::encoders::EncodeChunkOptions options{};
    const obj_handle_t injectedCommandBuffer =
        carrier->commandBuffer.handle;
    options.commandBuffer = std::move(carrier->commandBuffer);
    options.allowInjectedCommandBufferMidChunkCommits = true;
    options.session = session.get();
    options.deferSessionFinalization = true;
    options.partitionSource = completionSources[sourceIndex].source;
    options.preRegisteredFragment =
        dxmt9::encoders::PreRegisteredEncodeChunkFragment{
            .commandBegin = run.commandBegin,
            .commandCount = run.commandCount,
        };
    options.skipBackendPlanning = true;
    auto fragment = dxmt9::encoders::encodeChunk(
        ctx, slotIndices[sourceIndex], payloads[sourceIndex],
        seqIds[sourceIndex], std::move(options));
    check(fragment.has_value(),
          "each planned whole-source fragment encodes into the session");
    check(dxmt9::core::metalqueue::foldEncodedSessionFragmentCarrier(
              *fragment, *carrier, injectedCommandBuffer),
          "each fragment folds into one command-buffer carrier");
    carrier = std::move(fragment);
    encodedSeqIds.push_back(seqIds[sourceIndex]);
  }
  check(encodedSeqIds == std::vector<std::uint64_t>({183u, 182u}),
        "fragment replay retains exact A-then-B source attribution");
  checkEq(capture.renderPassBegins, std::size_t{2},
          "active A plus separate A then B uses two passes, not three");
  checkEq(capture.renderPassEnds, std::size_t{1},
          "only the real A-to-B transition closes the carried pass");

  carrier->seqId = completionSources.back().seqId;
  carrier->slotIndex = completionSources.back().slotIndex;
  carrier->diagnostics.seqId = completionSources.back().seqId;
  carrier->diagnostics.slotIndex = completionSources.back().slotIndex;
  check(dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, *carrier),
        "multi-source fragment carrier finalizes once");
  const auto published = carrier->explicitCompletionSourceSpan();
  check(published.size() == 3 && published[0].seqId == 181u &&
            published[1].seqId == 182u && published[2].seqId == 183u,
        "completion stays FIFO although fragment replay order differs");
  check(carrier->seqId == 183u && carrier->slotIndex == 23u,
        "carrier identity names the natural FIFO completion tail");
}

struct ActiveSeedOutcomeRun {
  dxmt9::perf::test::FramegraphActiveRenderSeedSnapshot before{};
  dxmt9::perf::test::FramegraphActiveRenderSeedSnapshot after{};
  std::vector<std::size_t> drawRunCommands;
  std::size_t renderPassBegins = 0;
  std::size_t renderPassEndsBeforeFinalize = 0;
};

void seedUnknownStore(DrawRunCapture& capture,
                      dxmt9::encoders::LateRenderPassStoreAspect aspect,
                      std::uint64_t handle,
                      std::uint8_t colorIndex = 0u) {
  check(capture.lateStoreSeed.append({
            .handle = dxmt9::core::Handle{handle},
            .pixelBytes = 64u,
            .action = WMTStoreActionUnknown,
            .aspect = aspect,
            .colorIndex = colorIndex,
        }),
        "late Store fixture fits the copied session ledger");
}

void lateStoreResolutionCarriesAcrossSessionSources() {
  using Aspect = dxmt9::encoders::LateRenderPassStoreAspect;
  using Cause = dxmt9::perf::RenderPassLateStoreResolutionCause;

  Harness harness;
  DrawRunCapture capture;
  capture.prepareLateStoreOnce = true;
  constexpr std::uint64_t colorA = 0xA400u;
  constexpr std::uint64_t colorWrongSlot = 0xC400u;
  constexpr std::uint64_t depth = 0xD400u;
  seedUnknownStore(capture, Aspect::Color, colorA, 0u);
  seedUnknownStore(capture, Aspect::Color, colorWrongSlot, 1u);
  seedUnknownStore(capture, Aspect::Depth, depth);
  seedUnknownStore(capture, Aspect::Stencil, depth);
  auto encoderOwner = retainedToken<WMT::RenderCommandEncoder>(
      "late-store-carried-render-encoder");
  auto recorder = makeDrawRunRecorder(
      capture, WMT::RenderCommandEncoder{encoderOwner.handle});
  auto ctx = harness.makeContext();
  ctx.drawRecorder = &recorder;
  auto session = dxmt9::encoders::makeEncodeChunkSession();
  const auto accountingBefore =
      dxmt9::perf::test::snapshotRenderPassStoreAccounting();

  auto headSlot = makeTargetDrawSlot(300u, std::array{colorA});
  auto headSource = fullSource(30u, headSlot.seqId, headSlot.commandCount());
  auto carrier = dxmt9::encoders::encodeChunk(
      ctx, headSource.slotIndex, headSlot,
      deferredOptions(
          *session,
          retainedToken<WMT::CommandBuffer>("late-store-carried-cb"),
          headSource));
  check(carrier.has_value(), "late Store head source opens a carried pass");

  // Carry through more than eight separately injected session sources.
  // This pins copied-state lifetime and ContinueActive behavior; the pure
  // render-pass-actions selector test separately pins StorageTruncated
  // production eligibility.
  for (std::size_t i = 1; i <= 8u; ++i) {
    auto slot = makeTargetDrawSlot(300u + i, std::array{colorA});
    const auto source = fullSource(30u + i, slot.seqId, slot.commandCount());
    auto next = dxmt9::encoders::encodeChunk(
        ctx, source.slotIndex, slot,
        deferredOptions(*session, std::move(carrier->commandBuffer), source));
    check(next.has_value(), "compatible source continues the carried pass");
    carrier = std::move(next);
    check(capture.lateStoreResolutions.empty(),
          "compatible same-pass draw leaves Unknown unresolved");
  }
  checkEq(capture.renderPassBegins, std::size_t{1},
          "nine compatible sources retain one Metal render encoder");

  dxmt9::core::ChunkSlot clearSlot{};
  clearSlot.seqId = 309u;
  dxmt9::core::ClearDesc clear{};
  clear.clearColor = true;
  clear.clearDepth = true;
  clear.clearStencil = false;
  clear.colorAttachments[0].handle = dxmt9::core::Handle{colorA};
  // Same handle in the wrong MRT slot must not authorize slot-1 discard.
  clear.colorAttachments[2].handle =
      dxmt9::core::Handle{colorWrongSlot};
  clear.depthStencil.handle = dxmt9::core::Handle{depth};
  clearSlot.appendClear(clear);
  const auto clearSource =
      fullSource(39u, clearSlot.seqId, clearSlot.commandCount());
  auto tail = dxmt9::encoders::encodeChunk(
      ctx, clearSource.slotIndex, clearSlot,
      deferredOptions(*session, std::move(carrier->commandBuffer), clearSource));
  check(tail.has_value(), "matching full clear closes the carried pass");
  checkEq(capture.renderPassEnds, std::size_t{1},
          "clear performs the one physical carried-pass close");
  checkEq(capture.lateStoreResolutions.size(), std::size_t{4},
          "each copied attachment resolves exactly once");
  check(capture.lateStoreResolutions[0].action == WMTStoreActionDontCare &&
            capture.lateStoreResolutions[0].cause ==
                static_cast<std::uint8_t>(Cause::Clear),
        "matching full color clear resolves DontCare");
  check(capture.lateStoreResolutions[1].action == WMTStoreActionStore &&
            capture.lateStoreResolutions[1].cause ==
                static_cast<std::uint8_t>(Cause::ClearMismatch),
        "wrong MRT slot resolves Store");
  check(capture.lateStoreResolutions[2].action == WMTStoreActionDontCare &&
            capture.lateStoreResolutions[2].cause ==
                static_cast<std::uint8_t>(Cause::Clear),
        "matching full depth clear resolves DontCare");
  check(capture.lateStoreResolutions[3].action == WMTStoreActionStore &&
            capture.lateStoreResolutions[3].cause ==
                static_cast<std::uint8_t>(Cause::ClearMismatch),
        "depth-only clear cannot discard stencil");
  const auto accountingAfterResolution =
      dxmt9::perf::test::snapshotRenderPassStoreAccounting();
  check(accountingAfterResolution.colorStore - accountingBefore.colorStore ==
            1u &&
            accountingAfterResolution.colorDontCare -
                    accountingBefore.colorDontCare ==
                1u &&
            accountingAfterResolution.depthStore - accountingBefore.depthStore ==
                0u &&
            accountingAfterResolution.depthDontCare -
                    accountingBefore.depthDontCare ==
                1u &&
            accountingAfterResolution.stencilStore -
                    accountingBefore.stencilStore ==
                1u &&
            accountingAfterResolution.stencilDontCare -
                    accountingBefore.stencilDontCare ==
                0u,
        "resolved Store histograms conserve one action per included aspect");
  checkEq(accountingAfterResolution.tilePreservationBytes -
              accountingBefore.tilePreservationBytes,
          std::uint64_t{128u},
          "only the two Store attachments contribute tile Store bytes");

  check(dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, *tail, dxmt9::encoders::SessionFinalizeCause::Drain),
        "late Store multi-source session finalizes");
  const auto published = tail->explicitCompletionSourceSpan();
  checkEq(published.size(), std::size_t{10},
          "all sources remain FIFO-owned through late resolution");
  for (std::size_t i = 0; i < published.size(); ++i) {
    checkEq(published[i].seqId, std::uint64_t{300u + i},
            "late Store resolution preserves FIFO completion order");
  }
  const auto accountingAfterFinalize =
      dxmt9::perf::test::snapshotRenderPassStoreAccounting();
  check(accountingAfterFinalize.colorStore ==
            accountingAfterResolution.colorStore &&
            accountingAfterFinalize.colorDontCare ==
                accountingAfterResolution.colorDontCare &&
            accountingAfterFinalize.depthStore ==
                accountingAfterResolution.depthStore &&
            accountingAfterFinalize.depthDontCare ==
                accountingAfterResolution.depthDontCare &&
            accountingAfterFinalize.stencilStore ==
                accountingAfterResolution.stencilStore &&
            accountingAfterFinalize.stencilDontCare ==
                accountingAfterResolution.stencilDontCare &&
            accountingAfterFinalize.tilePreservationBytes ==
                accountingAfterResolution.tilePreservationBytes,
        "post-close finalization cannot double-account actions or tile bytes");
}

void lateStoreDrawAndSampleControls() {
  using Aspect = dxmt9::encoders::LateRenderPassStoreAspect;
  using Cause = dxmt9::perf::RenderPassLateStoreResolutionCause;
  constexpr std::uint64_t colorA = 0xA410u;
  constexpr std::uint64_t colorB = 0xB410u;

  for (const bool sampleA : {false, true}) {
    Harness harness;
    DrawRunCapture capture;
    capture.prepareLateStoreOnce = true;
    seedUnknownStore(capture, Aspect::Color, colorA);
    auto encoderOwner = retainedToken<WMT::RenderCommandEncoder>(
        sampleA ? "late-store-sample-encoder" : "late-store-draw-encoder");
    auto recorder = makeDrawRunRecorder(
        capture, WMT::RenderCommandEncoder{encoderOwner.handle});
    auto ctx = harness.makeContext();
    ctx.drawRecorder = &recorder;
    auto session = dxmt9::encoders::makeEncodeChunkSession();
    auto headSlot = makeTargetDrawSlot(320u, std::array{colorA});
    const auto headSource =
        fullSource(50u, headSlot.seqId, headSlot.commandCount());
    auto carrier = dxmt9::encoders::encodeChunk(
        ctx, headSource.slotIndex, headSlot,
        deferredOptions(
            *session,
            retainedToken<WMT::CommandBuffer>("late-store-control-cb"),
            headSource));
    check(carrier.has_value(), "draw/sample control opens its A pass");

    dxmt9::core::ChunkSlot tailSlot{};
    tailSlot.seqId = 321u;
    appendTargetDraw(tailSlot, colorB, sampleA ? colorA : 0u);
    const auto tailSource =
        fullSource(51u, tailSlot.seqId, tailSlot.commandCount());
    auto tail = dxmt9::encoders::encodeChunk(
        ctx, tailSource.slotIndex, tailSlot,
        deferredOptions(*session, std::move(carrier->commandBuffer),
                        tailSource));
    check(tail.has_value(), "incompatible control source encodes");
    checkEq(capture.lateStoreResolutions.size(), std::size_t{1},
            "incompatible draw resolves the old pass once");
    check(capture.lateStoreResolutions[0].action == WMTStoreActionStore,
          "incompatible draw preserves the old attachment");
    const auto expectedCause = sampleA ? Cause::Sample : Cause::Draw;
    check(capture.lateStoreResolutions[0].cause ==
              static_cast<std::uint8_t>(expectedCause),
          "draw/sample cause attribution is exact");
    check(dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
              ctx, *session, *tail),
          "draw/sample control finalizes");
  }
}

void lateStoreBarrierCommandControls() {
  using Aspect = dxmt9::encoders::LateRenderPassStoreAspect;
  using Cause = dxmt9::perf::RenderPassLateStoreResolutionCause;
  enum class Barrier { PartialClear, Readback, Copy, Resolve, Present };
  const std::array cases{
      std::pair{Barrier::PartialClear, Cause::ClearMismatch},
      std::pair{Barrier::Readback, Cause::Readback},
      std::pair{Barrier::Copy, Cause::Copy},
      std::pair{Barrier::Resolve, Cause::Resolve},
      std::pair{Barrier::Present, Cause::Present},
  };
  constexpr std::uint64_t colorA = 0xA418u;

  for (std::size_t i = 0; i < cases.size(); ++i) {
    Harness harness;
    DrawRunCapture capture;
    capture.prepareLateStoreOnce = true;
    seedUnknownStore(capture, Aspect::Color, colorA);
    auto encoderOwner = retainedToken<WMT::RenderCommandEncoder>(
        "late-store-barrier-encoder");
    auto recorder = makeDrawRunRecorder(
        capture, WMT::RenderCommandEncoder{encoderOwner.handle});
    auto ctx = harness.makeContext();
    ctx.drawRecorder = &recorder;
    auto session = dxmt9::encoders::makeEncodeChunkSession();
    auto headSlot = makeTargetDrawSlot(325u + i, std::array{colorA});
    const auto headSource =
        fullSource(55u + i, headSlot.seqId, headSlot.commandCount());
    auto carrier = dxmt9::encoders::encodeChunk(
        ctx, headSource.slotIndex, headSlot,
        deferredOptions(
            *session,
            retainedToken<WMT::CommandBuffer>("late-store-barrier-cb"),
            headSource));
    check(carrier.has_value(), "barrier control opens its A pass");

    dxmt9::core::ChunkSlot tailSlot{};
    tailSlot.seqId = 326u + i;
    switch (cases[i].first) {
    case Barrier::PartialClear: {
      dxmt9::core::ClearDesc clear{};
      clear.clearColor = true;
      clear.colorAttachments[0].handle = dxmt9::core::Handle{colorA};
      clear.rects.push_back({.left = 0, .top = 0, .right = 4, .bottom = 4});
      tailSlot.appendClear(clear);
      break;
    }
    case Barrier::Readback:
      tailSlot.appendReadback({
          .source = dxmt9::core::Handle{colorA},
          .destination = dxmt9::core::Handle{0xB418u},
      });
      break;
    case Barrier::Copy:
      tailSlot.appendSurfaceCopy({
          .source = dxmt9::core::Handle{colorA},
          .destination = dxmt9::core::Handle{0xB418u},
      });
      break;
    case Barrier::Resolve:
      tailSlot.appendDepthResolve({
          .msaaDepth = dxmt9::core::Handle{0xD418u},
          .intzDest = dxmt9::core::Handle{0xD419u},
      });
      break;
    case Barrier::Present:
      tailSlot.appendPresent({}, dxmt9::core::Handle{colorA});
      break;
    }
    auto tailSource =
        fullSource(56u + i, tailSlot.seqId, tailSlot.commandCount());
    tailSource.hasPresent = cases[i].first == Barrier::Present;
    auto tail = dxmt9::encoders::encodeChunk(
        ctx, tailSource.slotIndex, tailSlot,
        deferredOptions(*session, std::move(carrier->commandBuffer),
                        tailSource));
    check(tail.has_value(), "barrier control source encodes");
    checkEq(capture.lateStoreResolutions.size(), std::size_t{1},
            "barrier resolves the old attachment exactly once");
    check(capture.lateStoreResolutions[0].action == WMTStoreActionStore &&
              capture.lateStoreResolutions[0].cause ==
                  static_cast<std::uint8_t>(cases[i].second),
          "barrier uses its cause-specific conservative Store bucket");
    check(dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
              ctx, *session, *tail),
          "barrier control finalizes");
  }
}

void lateStoreCloseAndFinalizeCauses() {
  using Aspect = dxmt9::encoders::LateRenderPassStoreAspect;
  using Cause = dxmt9::perf::RenderPassLateStoreResolutionCause;
  using Finalize = dxmt9::encoders::SessionFinalizeCause;
  constexpr std::uint64_t colorA = 0xA420u;
  const std::array cases{
      std::pair{Finalize::Drain, Cause::Drain},
      std::pair{Finalize::SessionCap, Cause::Finalize},
      std::pair{Finalize::Independent, Cause::Finalize},
      std::pair{Finalize::Initializer, Cause::Finalize},
      std::pair{Finalize::ProducerWait, Cause::Finalize},
      std::pair{Finalize::FailOrOther, Cause::Error},
  };
  for (std::size_t i = 0; i < cases.size(); ++i) {
    Harness harness;
    DrawRunCapture capture;
    capture.prepareLateStoreOnce = true;
    seedUnknownStore(capture, Aspect::Color, colorA);
    auto encoderOwner = retainedToken<WMT::RenderCommandEncoder>(
        "late-store-finalize-encoder");
    auto recorder = makeDrawRunRecorder(
        capture, WMT::RenderCommandEncoder{encoderOwner.handle});
    auto ctx = harness.makeContext();
    ctx.drawRecorder = &recorder;
    auto session = dxmt9::encoders::makeEncodeChunkSession();
    auto slot = makeTargetDrawSlot(340u + i, std::array{colorA});
    const auto source = fullSource(60u + i, slot.seqId, slot.commandCount());
    auto carrier = dxmt9::encoders::encodeChunk(
        ctx, source.slotIndex, slot,
        deferredOptions(
            *session,
            retainedToken<WMT::CommandBuffer>("late-store-finalize-cb"),
            source));
    check(carrier.has_value(), "finalize-cause fixture opens a pass");
    check(dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
              ctx, *session, *carrier, cases[i].first),
          "finalize-cause fixture finalizes");
    checkEq(capture.lateStoreResolutions.size(), std::size_t{1},
            "finalizer resolves Unknown exactly once");
    check(capture.lateStoreResolutions[0].action == WMTStoreActionStore &&
              capture.lateStoreResolutions[0].cause ==
                  static_cast<std::uint8_t>(cases[i].second),
          "finalizer uses its cause-specific conservative Store bucket");
  }

  Harness harness;
  DrawRunCapture capture;
  capture.prepareLateStoreOnce = true;
  seedUnknownStore(capture, Aspect::Color, colorA);
  auto encoderOwner = retainedToken<WMT::RenderCommandEncoder>(
      "late-store-close-pass-encoder");
  auto recorder = makeDrawRunRecorder(
      capture, WMT::RenderCommandEncoder{encoderOwner.handle});
  auto ctx = harness.makeContext();
  ctx.drawRecorder = &recorder;
  auto session = dxmt9::encoders::makeEncodeChunkSession();
  auto slot = makeTargetDrawSlot(350u, std::array{colorA});
  const auto source = fullSource(70u, slot.seqId, slot.commandCount());
  auto carrier = dxmt9::encoders::encodeChunk(
      ctx, source.slotIndex, slot,
      deferredOptions(
          *session,
          retainedToken<WMT::CommandBuffer>("late-store-close-pass-cb"),
          source));
  check(carrier.has_value(), "ClosePass fixture opens a pass");
  check(dxmt9::encoders::closeEncodeChunkSessionRenderPass(
            ctx, *session, *carrier) ==
            dxmt9::encoders::EncodeChunkSessionPassCloseResult::Closed,
        "ordered ClosePass closes the active encoder");
  checkEq(capture.lateStoreResolutions.size(), std::size_t{1},
          "ordered ClosePass resolves Unknown once");
  check(capture.lateStoreResolutions[0].cause ==
            static_cast<std::uint8_t>(Cause::IncompatibleClose),
        "ordered ClosePass uses incompatible-close attribution");
  check(dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, *carrier),
        "closed session finalizes without a second resolution");
  checkEq(capture.lateStoreResolutions.size(), std::size_t{1},
          "post-close finalization cannot double-resolve or double-account");
}

void lateStoreEncoderOpenFailureDropsPendingState() {
  using Aspect = dxmt9::encoders::LateRenderPassStoreAspect;
  Harness harness;
  DrawRunCapture capture;
  seedUnknownStore(capture, Aspect::Color, 0xA430u);
  auto recorder = makeDrawRunRecorder(capture, WMT::RenderCommandEncoder{});
  auto ctx = harness.makeContext();
  ctx.drawRecorder = &recorder;
  auto session = dxmt9::encoders::makeEncodeChunkSession();

  auto slot = makeTargetDrawSlot(360u, std::array{std::uint64_t{0xA430u}});
  dxmt9::core::ClearDesc clear{};
  clear.clearColor = true;
  clear.colorAttachments[0].handle = dxmt9::core::Handle{0xA430u};
  slot.appendClear(clear);
  const auto source = fullSource(80u, slot.seqId, slot.commandCount());
  auto carrier = dxmt9::encoders::encodeChunk(
      ctx, source.slotIndex, slot,
      deferredOptions(
          *session,
          retainedToken<WMT::CommandBuffer>("late-store-null-encoder-cb"),
          source));
  check(carrier.has_value(),
        "null encoder fixture still returns its deferred carrier");
  check(capture.lateStorePrepareCalls > 0u,
        "null encoder fixture attempted to seed Unknown state");
  check(capture.lateStoreResolutions.empty(),
        "later clear cannot resolve stale state from a failed encoder open");
  checkEq(capture.renderPassEnds, std::size_t{0},
          "failed encoder open emits no pass-end effect");
  check(dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, *carrier),
        "null encoder fixture finalizes without a resolution leak");
  check(capture.lateStoreResolutions.empty(),
        "finalization cannot account discarded null-encoder state");
}

ActiveSeedOutcomeRun runActiveSeedOutcomeCase(bool bSamplesA,
                                               bool aSamplesB,
                                               std::uint64_t seqBase) {
  Harness harness;
  DrawRunCapture capture;
  auto renderEncoderOwner = retainedToken<WMT::RenderCommandEncoder>(
      bSamplesA || aSamplesB ? "active-seed-blocked-render-encoder"
                             : "active-seed-control-render-encoder");
  auto recorder = makeDrawRunRecorder(
      capture, WMT::RenderCommandEncoder{renderEncoderOwner.handle});
  auto ctx = harness.makeContext();
  ctx.drawRecorder = &recorder;
  auto session = dxmt9::encoders::makeEncodeChunkSession();

  constexpr std::uint64_t kTargetA = 0xA200u;
  constexpr std::uint64_t kTargetB = 0xB200u;
  const std::array headTargets{kTargetA};
  auto headSlot = makeTargetDrawSlot(seqBase, headTargets);
  const std::size_t headSlotIndex = static_cast<std::size_t>(seqBase);
  const auto headSource =
      fullSource(headSlotIndex, headSlot.seqId, headSlot.commandCount());
  auto head = dxmt9::encoders::encodeChunk(
      ctx, headSlotIndex, headSlot,
      deferredOptions(
          *session,
          retainedToken<WMT::CommandBuffer>(
              bSamplesA || aSamplesB
                  ? "active-seed-blocked-command-buffer"
                  : "active-seed-control-command-buffer"),
          headSource));
  check(head.has_value(), "active A source opens the outcome-test session");
  check(dxmt9::encoders::encodeChunkSessionHasActiveRender(*session),
        "outcome-test head leaves active A open");

  auto currentSlot = makeActiveSeedOutcomeSlot(
      seqBase + 1u, kTargetA, kTargetB, bSamplesA, aSamplesB);
  dxmt9::tests::framegraph::ArenaPayloadFixture current(currentSlot);
  check(current.valid(), "active-seed outcome Arena source publishes");
  const std::size_t currentSlotIndex = headSlotIndex + 1u;
  const auto currentSource = fullSource(
      currentSlotIndex, currentSlot.seqId, currentSlot.commandCount());
  auto options = deferredOptions(*session, std::move(head->commandBuffer),
                                 currentSource);
  const auto before =
      dxmt9::perf::test::snapshotFramegraphActiveRenderSeed();
  dxmt9::render::FrameGraphBackend backend;
  auto tail = backend.onSourceReady(ctx, currentSlotIndex, current.view(),
                                    currentSlot.seqId, std::move(options));
  check(tail.has_value(), "active-seed outcome source encodes");
  const auto after =
      dxmt9::perf::test::snapshotFramegraphActiveRenderSeed();
  const std::size_t endsBeforeFinalize = capture.renderPassEnds;
  check(dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, *tail),
        "active-seed outcome session finalizes");
  checkEq(tail->explicitCompletionSourceSpan().size(), std::size_t{2},
          "outcome session retains source completion order");

  return ActiveSeedOutcomeRun{
      .before = before,
      .after = after,
      .drawRunCommands = std::move(capture.drawRunCommands),
      .renderPassBegins = capture.renderPassBegins,
      .renderPassEndsBeforeFinalize = endsBeforeFinalize,
  };
}

void activeRenderSeedOutcomeTracksCycleAndActivation() {
  auto checkCommandOrder = [](std::span<const std::size_t> actual,
                              std::initializer_list<std::size_t> expected,
                              std::string_view message) {
    if (std::equal(actual.begin(), actual.end(), expected.begin(),
                   expected.end())) {
      return;
    }
    std::ostringstream out;
    out << message << " (actual:";
    for (const std::size_t command : actual) {
      out << ' ' << command;
    }
    out << ')';
    fail(out.str());
  };
  const auto cyclic = runActiveSeedOutcomeCase(true, true, 90u);
  checkCommandOrder(cyclic.drawRunCommands, {0u, 1u, 2u},
                    "A then cyclic B,A stays in source draw order");
  checkEq(cyclic.renderPassBegins, std::size_t{3},
          "cyclic B,A opens both source-local passes after active A");
  checkEq(cyclic.renderPassEndsBeforeFinalize, std::size_t{2},
          "cyclic source closes A then B only at semantic transitions");
  checkEq(cyclic.after.applyApplied - cyclic.before.applyApplied,
          std::uint64_t{1}, "cyclic source records an applied seed");
  checkEq(cyclic.after.passCoalesceBlockedCycle -
              cyclic.before.passCoalesceBlockedCycle,
          std::uint64_t{1}, "cyclic source records the dependency wedge");
  checkEq(cyclic.after.appliedButUnmerged -
              cyclic.before.appliedButUnmerged,
          std::uint64_t{1}, "cyclic source records applied-but-unmerged");
  checkEq(cyclic.after.replayActivated - cyclic.before.replayActivated,
          std::uint64_t{0}, "cyclic source does not activate replay");

  const auto warOnly = runActiveSeedOutcomeCase(true, false, 100u);
  checkCommandOrder(warOnly.drawRunCommands, {0u, 1u, 2u},
                    "B reading A before returning A stays source ordered");
  checkEq(warOnly.after.passCoalesceBlockedCycle -
              warOnly.before.passCoalesceBlockedCycle,
          std::uint64_t{1},
          "B read followed by A attachment write records the WAR wedge");
  checkEq(warOnly.after.appliedButUnmerged -
              warOnly.before.appliedButUnmerged,
          std::uint64_t{1}, "WAR-only source leaves the seed unmerged");
  checkEq(warOnly.after.replayActivated - warOnly.before.replayActivated,
          std::uint64_t{0}, "WAR-only source does not activate replay");

  const auto independent = runActiveSeedOutcomeCase(false, false, 110u);
  checkCommandOrder(independent.drawRunCommands, {0u, 2u, 1u},
                    "independent B,A replays current A before B");
  checkEq(independent.renderPassBegins, std::size_t{2},
          "independent source continues active A before opening B");
  checkEq(independent.renderPassEndsBeforeFinalize, std::size_t{1},
          "independent replay closes only the real A-to-B transition");
  checkEq(independent.after.applyApplied - independent.before.applyApplied,
          std::uint64_t{1}, "independent source records an applied seed");
  checkEq(independent.after.passCoalesceBlockedCycle -
              independent.before.passCoalesceBlockedCycle,
          std::uint64_t{0}, "independent source has no dependency wedge");
  checkEq(independent.after.appliedButUnmerged -
              independent.before.appliedButUnmerged,
          std::uint64_t{0}, "independent source merges its applied seed");
  checkEq(independent.after.movedHeadProved -
              independent.before.movedHeadProved,
          std::uint64_t{1}, "independent source proves its moved head");
  checkEq(independent.after.replayActivated -
              independent.before.replayActivated,
          std::uint64_t{1}, "independent source activates replay");

  Harness harness;
  DrawRunCapture capture;
  auto renderEncoderOwner = retainedToken<WMT::RenderCommandEncoder>(
      "active-seed-absent-render-encoder");
  auto recorder = makeDrawRunRecorder(
      capture, WMT::RenderCommandEncoder{renderEncoderOwner.handle});
  auto ctx = harness.makeContext();
  ctx.drawRecorder = &recorder;
  auto session = dxmt9::encoders::makeEncodeChunkSession();
  constexpr std::uint64_t kTargetA = 0xA300u;
  constexpr std::uint64_t kTargetB = 0xB300u;
  const std::array targets{kTargetA, kTargetB, kTargetA};
  auto sourceSlot = makeTargetDrawSlot(121u, targets);
  dxmt9::tests::framegraph::ArenaPayloadFixture source(sourceSlot);
  check(source.valid(), "snapshot-absent Arena source publishes");
  const auto completion =
      fullSource(110u, sourceSlot.seqId, sourceSlot.commandCount());
  auto options = deferredOptions(
      *session,
      retainedToken<WMT::CommandBuffer>(
          "active-seed-absent-command-buffer"),
      completion);
  const auto absentBefore =
      dxmt9::perf::test::snapshotFramegraphActiveRenderSeed();
  dxmt9::render::FrameGraphBackend backend;
  auto tail = backend.onSourceReady(ctx, completion.slotIndex, source.view(),
                                    sourceSlot.seqId, std::move(options));
  check(tail.has_value(), "snapshot-absent source encodes");
  const auto absentAfter =
      dxmt9::perf::test::snapshotFramegraphActiveRenderSeed();
  checkCommandOrder(capture.drawRunCommands, {0u, 2u, 1u},
                    "snapshot-absent source still activates ordinary pass "
                    "coalescing");
  checkEq(absentAfter.snapshotAbsent - absentBefore.snapshotAbsent,
          std::uint64_t{1}, "empty session records snapshot absent");
  checkEq(absentAfter.applyApplied - absentBefore.applyApplied,
          std::uint64_t{0}, "snapshot-absent replay is not seed-applied");
  checkEq(absentAfter.movedHeadProved - absentBefore.movedHeadProved,
          std::uint64_t{0}, "ordinary replay is not seed head proof");
  checkEq(absentAfter.replayActivated - absentBefore.replayActivated,
          std::uint64_t{0}, "ordinary replay is not seed activation");
  checkEq(absentAfter.passCoalesceBlockedCycle -
              absentBefore.passCoalesceBlockedCycle,
          std::uint64_t{0}, "snapshot-absent planning emits no seed cycle");
  checkEq(absentAfter.passCoalesceSecondNonDraw -
              absentBefore.passCoalesceSecondNonDraw,
          std::uint64_t{0},
          "snapshot-absent planning emits no seed non-draw rejection");
  checkEq(absentAfter.fallbackMovedHeadUnproved -
              absentBefore.fallbackMovedHeadUnproved,
          std::uint64_t{0}, "snapshot-absent planning emits no seed fallback");
  check(dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, *tail),
        "snapshot-absent scope session finalizes");
}

void closePassRejectsMissingCarrierWithoutMutation() {
  Harness harness;
  DrawRunCapture capture;
  auto renderEncoderOwner = retainedToken<WMT::RenderCommandEncoder>(
      "close-pass-rejection-render-encoder");
  auto recorder = makeDrawRunRecorder(
      capture, WMT::RenderCommandEncoder{renderEncoderOwner.handle});
  auto ctx = harness.makeContext();
  ctx.drawRecorder = &recorder;
  auto session = dxmt9::encoders::makeEncodeChunkSession();

  constexpr std::size_t slotIndex = 13u;
  auto slot = makeDrawRunSlot(77u);
  const auto source = partitionSource(slotIndex, slot.seqId);
  auto submission = dxmt9::encoders::encodeChunk(
      ctx, slotIndex, slot,
      deferredOptions(
          *session,
          retainedToken<WMT::CommandBuffer>("close-pass-rejection-cb"),
          source));
  check(submission.has_value(),
        "rejection fixture returns a command-buffer carrier");

  dxmt9::core::metalqueue::QueueSubmissionRecord wrongCarrier{};
  wrongCarrier.commandBuffer =
      retainedToken<WMT::CommandBuffer>("close-pass-wrong-live-cb");
  wrongCarrier.slotIndex = submission->slotIndex;
  wrongCarrier.seqId = submission->seqId;
  check(dxmt9::encoders::closeEncodeChunkSessionRenderPass(
            ctx, *session, wrongCarrier) ==
            dxmt9::encoders::EncodeChunkSessionPassCloseResult::
                InvalidCommandBufferCarrier,
        "ClosePass rejects a different live command-buffer carrier");
  check(!dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, wrongCarrier),
        "session finalizer rejects a different live command-buffer tail");
  check(dxmt9::encoders::encodeChunkSessionHasActiveRender(*session) &&
            capture.renderPassEnds == 0,
        "wrong live carrier rejection is side-effect free");
  checkEq(dxmt9::encoders::encodeChunkSessionSources(*session).size(),
          std::size_t{1},
          "wrong live carrier preserves source ownership");

  constexpr std::size_t continuationSlotIndex = 14u;
  auto continuationSlot = makeDrawRunSlot(78u);
  const auto continuationSource =
      partitionSource(continuationSlotIndex, continuationSlot.seqId);
  auto rejectedContinuation = dxmt9::encoders::encodeChunk(
      ctx, continuationSlotIndex, continuationSlot,
      deferredOptions(
          *session,
          retainedToken<WMT::CommandBuffer>(
              "continuation-wrong-live-command-buffer"),
          continuationSource));
  check(!rejectedContinuation.has_value(),
        "injected continuation rejects a different live command buffer");
  check(dxmt9::encoders::encodeChunkSessionHasActiveRender(*session) &&
            capture.renderPassEnds == 0,
        "continuation identity rejection emits no pass-end side effects");
  checkEq(dxmt9::encoders::encodeChunkSessionSources(*session).size(),
          std::size_t{1},
          "continuation identity rejection does not retain its source");

  auto savedCommandBuffer = std::move(submission->commandBuffer);

  check(dxmt9::encoders::closeEncodeChunkSessionRenderPass(
            ctx, *session, *submission) ==
            dxmt9::encoders::EncodeChunkSessionPassCloseResult::
                InvalidCommandBufferCarrier,
        "active ClosePass rejects a missing command-buffer carrier");
  check(dxmt9::encoders::encodeChunkSessionHasActiveRender(*session),
        "carrier rejection preserves the active pass");
  checkEq(capture.renderPassEnds, std::size_t{0},
          "carrier rejection has no pass-end side effects");
  checkEq(dxmt9::encoders::encodeChunkSessionSources(*session).size(),
          std::size_t{1},
          "carrier rejection preserves retained source ownership");

  submission->commandBuffer = std::move(savedCommandBuffer);
  check(dxmt9::encoders::closeEncodeChunkSessionRenderPass(
            ctx, *session, *submission) ==
            dxmt9::encoders::EncodeChunkSessionPassCloseResult::Closed,
        "ClosePass succeeds after restoring the carrier");
  check(dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, *submission),
        "closed rejection fixture finalizes normally");
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

void preRegisteredFragmentsKeepReplayAndCompletionOrderIndependent() {
  Harness harness;
  DrawRunCapture capture;
  auto recorder = makeDrawRunRecorder(capture, {});
  auto ctx = harness.makeContext();
  ctx.drawRecorder = &recorder;
  auto session = dxmt9::encoders::makeEncodeChunkSession();

  dxmt9::core::ChunkSlot first{};
  first.seqId = 1;
  dxmt9::core::ClearDesc firstClear{};
  firstClear.clearColor = true;
  first.appendClear(firstClear);
  dxmt9::core::ChunkSlot second{};
  second.seqId = 2;
  dxmt9::core::ClearDesc secondClear{};
  secondClear.clearColor = true;
  second.appendClear(secondClear);
  const std::array sources{
      fullSource(1, first.seqId, first.commandCount()),
      fullSource(2, second.seqId, second.commandCount()),
  };
  check(dxmt9::encoders::appendEncodeChunkSessionSources(*session, sources),
        "fragment fixture pre-registers FIFO completion sources");

  auto fragmentOptions = [&](const auto& source,
                             WMT::Reference<WMT::CommandBuffer> carrier) {
    dxmt9::encoders::EncodeChunkOptions options{};
    options.commandBuffer = std::move(carrier);
    options.session = session.get();
    options.deferSessionFinalization = true;
    options.partitionSource = source.source;
    options.preRegisteredFragment =
        dxmt9::encoders::PreRegisteredEncodeChunkFragment{
            .commandBegin = 0,
            .commandCount = 1,
        };
    return options;
  };

  auto encodedSecond = dxmt9::encoders::encodeChunk(
      ctx, sources[1].slotIndex, second,
      fragmentOptions(
          sources[1],
          retainedToken<WMT::CommandBuffer>("fragment-order-cb-token")));
  check(encodedSecond.has_value(),
        "source seq2 encodes first as a pre-registered fragment");
  checkEq(dxmt9::encoders::encodeChunkSessionSources(*session).size(),
          std::size_t{2},
          "fragment replay does not append completion metadata again");

  const obj_handle_t injectedCommandBuffer =
      encodedSecond->commandBuffer.handle;
  auto encodedFirst = dxmt9::encoders::encodeChunk(
      ctx, sources[0].slotIndex, first,
      fragmentOptions(sources[0], std::move(encodedSecond->commandBuffer)));
  check(encodedFirst.has_value(),
        "source seq1 encodes second into the same session carrier");
  check(dxmt9::core::metalqueue::foldEncodedSessionFragmentCarrier(
            *encodedFirst, *encodedSecond,
            injectedCommandBuffer),
        "out-of-sequence fragment metadata folds into the final carrier");
  check(dxmt9::encoders::finalizeEncodeChunkSessionIntoSubmission(
            ctx, *session, *encodedFirst),
        "fragment session finalizes after carrier folding");
  const auto published = encodedFirst->explicitCompletionSourceSpan();
  checkEq(published.size(), std::size_t{2},
          "finalization publishes each pre-registered source once");
  checkEq(published[0].seqId, 1ull,
          "completion remains FIFO despite seq2 encoding first");
  checkEq(published[1].seqId, 2ull,
          "completion order is independent from fragment replay order");
  checkEq(capture.splitPolicyCalls, std::size_t{2},
          "each selected command fragment executes exactly once");
}

void malformedPreRegisteredFragmentsRejectBeforeRecorderEffects() {
  Harness harness;
  DrawRunCapture capture;
  auto recorder = makeDrawRunRecorder(capture, {});
  auto ctx = harness.makeContext();
  ctx.drawRecorder = &recorder;
  auto session = dxmt9::encoders::makeEncodeChunkSession();

  dxmt9::core::ChunkSlot slot{};
  slot.seqId = 7;
  dxmt9::core::ClearDesc clear{};
  clear.clearColor = true;
  slot.appendClear(clear);
  const auto source = fullSource(7, slot.seqId, slot.commandCount());

  dxmt9::encoders::EncodeChunkOptions unregistered{};
  unregistered.commandBuffer =
      retainedToken<WMT::CommandBuffer>("unregistered-fragment-token");
  unregistered.session = session.get();
  unregistered.deferSessionFinalization = true;
  unregistered.partitionSource = source.source;
  unregistered.preRegisteredFragment =
      dxmt9::encoders::PreRegisteredEncodeChunkFragment{0, 1};
  check(!dxmt9::encoders::encodeChunk(
             ctx, source.slotIndex, slot, std::move(unregistered))
             .has_value(),
        "unregistered fragment source is rejected");
  checkEq(capture.splitPolicyCalls, std::size_t{0},
          "unregistered rejection precedes recorder effects");

  check(dxmt9::encoders::appendEncodeChunkSessionSource(*session, source),
        "malformed fixture registers its exact source");
  auto conflicting = deferredOptions(
      *session, retainedToken<WMT::CommandBuffer>("conflict-fragment-token"),
      source);
  conflicting.preRegisteredFragment =
      dxmt9::encoders::PreRegisteredEncodeChunkFragment{0, 1};
  check(!dxmt9::encoders::encodeChunk(
             ctx, source.slotIndex, slot, std::move(conflicting))
             .has_value(),
        "sessionSource plus pre-registered fragment is rejected");
  checkEq(capture.splitPolicyCalls, std::size_t{0},
          "ambiguous range mode rejects before recorder effects");

  dxmt9::encoders::EncodeChunkOptions finalizingFragment{};
  finalizingFragment.commandBuffer =
      retainedToken<WMT::CommandBuffer>("finalizing-fragment-token");
  finalizingFragment.session = session.get();
  finalizingFragment.partitionSource = source.source;
  finalizingFragment.preRegisteredFragment =
      dxmt9::encoders::PreRegisteredEncodeChunkFragment{0, 1};
  check(!dxmt9::encoders::encodeChunk(
             ctx, source.slotIndex, slot, std::move(finalizingFragment))
             .has_value(),
        "pre-registered fragment cannot finalize the session early");
  checkEq(capture.splitPolicyCalls, std::size_t{0},
          "early-finalization rejection precedes recorder effects");

  dxmt9::encoders::EncodeChunkOptions outsideRange{};
  outsideRange.commandBuffer =
      retainedToken<WMT::CommandBuffer>("outside-fragment-token");
  outsideRange.session = session.get();
  outsideRange.deferSessionFinalization = true;
  outsideRange.partitionSource = source.source;
  outsideRange.preRegisteredFragment =
      dxmt9::encoders::PreRegisteredEncodeChunkFragment{1, 1};
  check(!dxmt9::encoders::encodeChunk(
             ctx, source.slotIndex, slot, std::move(outsideRange))
             .has_value(),
        "fragment outside registered command coverage is rejected");
  checkEq(capture.splitPolicyCalls, std::size_t{0},
          "range rejection precedes recorder effects");
}

void ticketBeforeEncodeAdmissionFailureRemainsUnissued() {
  Harness harness;
  auto invalidContext = harness.makeContext();
  invalidContext.device = {};
  dxmt9::core::ChunkSlot emptySlot{};
  const std::array targets{
      dxmt9::encoders::ActiveSeedMergeTargetWitness{
          .retainedSourceIndex = 0u,
          .commandIndex = 0u,
          .mergeOrdinal = 0u,
          .mergeDistance = 1u,
      }};
  dxmt9::encoders::EncodeChunkOptions options{};
  options.activeSeedMergeTicket =
      dxmt9::encoders::ActiveSeedMergeTicketContext{
          .seed = {.seqId = 9u, .encoderIndex = 3u},
          .windowId = 4u,
          .sourceCount = 2u,
      };
  options.activeSeedMergeTargets = targets;
  const auto before = dxmt9::perf::test::
      snapshotRenderPassNaturalFallbackAttribution();
  check(!dxmt9::encoders::encodeChunk(
             invalidContext, 0u, emptySlot, std::move(options))
             .has_value(),
        "invalid device/queue rejects before encode admission");
  const auto after = dxmt9::perf::test::
      snapshotRenderPassNaturalFallbackAttribution();
  check(after.seedTicketsIssued == before.seedTicketsIssued &&
            after.seedTicketsMatched == before.seedTicketsMatched &&
            after.seedTicketsContinued == before.seedTicketsContinued &&
            after.seedTicketsMismatch == before.seedTicketsMismatch &&
            after.seedTicketsUnconsumed == before.seedTicketsUnconsumed,
        "pre-admission failure owns neither issued nor terminal ticket state");
}

}  // namespace

int main() {
  setenv("DXMT_PERF_COUNTERS", "1", 1);
  setenv("DXMT9_PERF_ENCODER_GPU_TIME", "0", 1);
  setenv("DXMT_METAL_CAPTURE_FRAME", "0", 1);
  setenv("DXMT9_RENDERER_COMPAT_PROFILE", "progressive", 1);
  setenv("DXMT9_RENDERER_FEATURES", "passcoalesce", 1);
  try {
    encodeChunkThenFinalizerPublishesAndResetsOneSession();
    finalizerValidationFailureIsNoMutationAndRetryable();
    finalizerRejectsSourceAndStorageGenerationMismatches();
    encodeChunkRejectsPartitionAndSessionLocatorMismatchBeforeEffects();
    arenaSourcePayloadExecutesThroughIdentitySerialEncodeChunk();
    explicitDrawRunSubrangesExecuteThroughEncodeChunk();
    productionPartitionModeSubdividesWithoutChangingMetalShape();
    invalidDrawRunPlanFallsBackBeforeEncodeChunkSideEffects();
    explicitDrawRunDeferredSessionRetainsSourceUntilFinalizer();
    closePassIsIdempotentAndSessionResumesOnSameCommandBuffer();
    closedEncoderFrontierAllowsMovedHeadOnSameCommandBuffer();
    pendingClearSidecarAloneCannotProveCleanFrontier();
    freshCleanFrontierAllowsMovedHead();
    pendingClearAndInjectedFrontiersKeepNaturalMovedHeadOrder();
    activeRenderSeedCarriesAcrossLegacyAndArenaSources();
    multiSourceFragmentsCarryActivePassAcrossSeparateSources();
    activeRenderSeedOutcomeTracksCycleAndActivation();
    lateStoreResolutionCarriesAcrossSessionSources();
    lateStoreDrawAndSampleControls();
    lateStoreBarrierCommandControls();
    lateStoreCloseAndFinalizeCauses();
    lateStoreEncoderOpenFailureDropsPendingState();
    closePassRejectsMissingCarrierWithoutMutation();
    partialCommandSegmentSessionFinalizesWithoutTargets();
    preRegisteredFragmentsKeepReplayAndCompletionOrderIndependent();
    malformedPreRegisteredFragmentsRejectBeforeRecorderEffects();
    ticketBeforeEncodeAdmissionFailureRemainsUnissued();
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
