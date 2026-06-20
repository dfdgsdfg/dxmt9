#include "../../../src/dxmt9/render/backend_interface.hpp"
#include "../../../src/dxmt9/render/tail_present_batch.hpp"
#include "../../../src/dxmt9/dxmt9_draw_encoder.hpp"

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

ReadySlotSnapshot makeReadySource(std::size_t slotIndex, std::uint64_t seqId) {
  ReadySlotSnapshot source;
  source.slotIndex = slotIndex;
  source.slot.seqId = seqId;
  return source;
}

ReadySlotSnapshot makeClearSource(std::size_t slotIndex, std::uint64_t seqId) {
  auto source = makeReadySource(slotIndex, seqId);
  source.slot.appendClear({});
  return source;
}

ReadySlotSnapshot makePresentOnlySource(std::size_t slotIndex,
                                        std::uint64_t seqId) {
  auto source = makeReadySource(slotIndex, seqId);
  source.slot.appendPresent({}, {});
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
  std::array<ReadySlotSnapshot, 1> sources{
      makeReadySource(/*slotIndex=*/3, /*seqId=*/11),
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
  std::array<ReadySlotSnapshot, 2> sources{
      makeReadySource(/*slotIndex=*/1, /*seqId=*/5),
      makeReadySource(/*slotIndex=*/2, /*seqId=*/6),
  };

  const auto submission = backend.onChunkBatchReady(fixture.ctx, sources);

  check(!submission.has_value(), "multi-source batch has no default submission");
  checkEq(backend.singleCalls, 0u,
          "multi-source batch does not silently encode only the first source");
}

void tailPresentBatchShapeRequiresPresentOnlyTail() {
  std::array<ReadySlotSnapshot, 2> valid{
      makeClearSource(/*slotIndex=*/1, /*seqId=*/5),
      makePresentOnlySource(/*slotIndex=*/2, /*seqId=*/6),
  };
  check(dxmt9::render::canCoalesceTailPresentBatch(valid),
        "non-present head plus present-only tail is coalescable");
  check(dxmt9::render::slotIsPresentOnlyTail(valid.back().slot),
        "tail source is recognized as present-only");

  auto headWithPresent = valid;
  headWithPresent.front().slot.appendPresent({}, {});
  check(!dxmt9::render::canCoalesceTailPresentBatch(headWithPresent),
        "head source with an existing present is rejected");

  auto tailWithExtraWork = valid;
  tailWithExtraWork.back().slot.clearCommands();
  tailWithExtraWork.back().slot.appendClear({});
  tailWithExtraWork.back().slot.appendPresent({}, {});
  check(!dxmt9::render::slotIsPresentOnlyTail(tailWithExtraWork.back().slot),
        "tail with extra work is not present-only");
  check(!dxmt9::render::canCoalesceTailPresentBatch(tailWithExtraWork),
        "tail source with extra work is rejected");
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

void tailPresentBatchShapeAllowsSeveralHeads() {
  std::array<ReadySlotSnapshot, 3> valid{
      makeClearSource(/*slotIndex=*/1, /*seqId=*/5),
      makeClearSource(/*slotIndex=*/2, /*seqId=*/6),
      makePresentOnlySource(/*slotIndex=*/3, /*seqId=*/7),
  };
  check(dxmt9::render::canCoalesceTailPresentBatch(valid),
        "several non-present heads plus present-only tail are coalescable");

  auto middleWithPresent = valid;
  middleWithPresent[1].slot.appendPresent({}, {});
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
  slots[2].appendPresent({}, {});

  const std::deque<std::size_t> readySlots{0, 1, 2};
  checkEq(dxmt9::render::selectTailPresentBatchPrefix(
              readySlots,
              std::span<const ChunkSlot>(slots.data(), slots.size()),
              /*maxCount=*/3),
          3u,
          "selector accepts complete head/head/present-tail prefix");
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
          0u,
          "selector rejects present metadata before the tail");
}

void openCbTailPresentPrefixRequiresOpenCbHeads() {
  std::array<ChunkSlot, 4> slots{};
  slots[0].seqId = 5;
  slots[0].publishReason = dxmt9::perf::ChunkPublishReason::PresentSplitBefore;
  slots[0].appendClear({});
  slots[1].seqId = 6;
  slots[1].publishReason = dxmt9::perf::ChunkPublishReason::PresentSplitBefore;
  slots[1].appendClear({});
  slots[2].seqId = 7;
  slots[2].appendPresent({}, {});

  const std::deque<std::size_t> readySlots{0, 1, 2};
  checkEq(dxmt9::render::selectOpenCbTailPresentBatchPrefix(
              readySlots,
              std::span<const ChunkSlot>(slots.data(), slots.size()),
              /*maxCount=*/3),
          3u,
          "open-CB selector accepts open heads plus present-only tail");
  checkEq(dxmt9::render::selectOpenCbTailPresentBatchPrefix(
              readySlots,
              std::span<const ChunkSlot>(slots.data(), slots.size()),
              /*maxCount=*/2),
          0u,
          "open-CB selector rejects when tail is outside scratch capacity");

  slots[1].publishReason = dxmt9::perf::ChunkPublishReason::DrawLimit;
  checkEq(dxmt9::render::selectOpenCbTailPresentBatchPrefix(
              readySlots,
              std::span<const ChunkSlot>(slots.data(), slots.size()),
              /*maxCount=*/3),
          0u,
          "open-CB selector rejects non-PresentSplitBefore heads");
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
    tailPresentBatchShapeRequiresPresentOnlyTail();
    openCbPreencodeHeadRequiresPresentSplitBeforePublish();
    tailPresentBatchShapeAllowsSeveralHeads();
    tailPresentPrefixSelectorRequiresCompleteTail();
    openCbTailPresentPrefixRequiresOpenCbHeads();
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
