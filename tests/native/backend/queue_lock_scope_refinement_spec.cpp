#include "../../../src/dxmt9/dxmt9_command_queue.hpp"
#include "../../../src/dxmt9/dxmt9_draw_payload_arithmetic.hpp"
#include "../../../src/dxmt9/dxmt9_lock_scope_policy.hpp"
#include "../../../src/dxmt9/dxmt9_upload_arena_lock_diag.hpp"
#include "../../../src/winemetal/winemetal.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dxmt9 {

// This fixture deliberately reads only the queue's current Writing payload.
// The ArenaLeaseTestQueueTag constructor starts no workers and owns no Metal
// objects, so the pointer remains stable for the duration of each assertion.
struct CommandQueueArenaLeaseTestAccess {
  static core::ChunkSlot copyWritingSlot(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    if (!queue.writingSlot_ || *queue.writingSlot_ >= queue.slots_.size()) {
      return {};
    }
    const auto& control = queue.slots_[*queue.writingSlot_];
    auto* payload = queue.cpuReadyTape_.resolveForWrite(
        core::CpuReadyPublicationTicket{
            .id = control.sourceId,
            .storage = control.storage,
        });
    return payload ? *payload : core::ChunkSlot{};
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

using dxmt9::core::CanonicalDrawState;
using dxmt9::core::ChunkSlot;
using dxmt9::core::DrawBindingOverride;
using dxmt9::core::DrawBindingSnapshot;
using dxmt9::core::DrawParam;
using dxmt9::core::DrawParamPayloadView;
using dxmt9::core::DrawRunBatchEntry;
using dxmt9::core::DrawUniformPayload;
using dxmt9::core::u8;

bool sameDrawParam(const DrawParam& lhs, const DrawParam& rhs) {
  return lhs.primitiveType == rhs.primitiveType &&
         lhs.primitiveCount == rhs.primitiveCount &&
         lhs.startVertex == rhs.startVertex &&
         lhs.baseVertexIndex == rhs.baseVertexIndex &&
         lhs.startIndex == rhs.startIndex && lhs.indexType == rhs.indexType &&
         lhs.indexed == rhs.indexed && lhs.instanceCount == rhs.instanceCount &&
         lhs.userVertexRange.offset == rhs.userVertexRange.offset &&
         lhs.userVertexRange.size == rhs.userVertexRange.size &&
         lhs.userIndexRange.offset == rhs.userIndexRange.offset &&
         lhs.userIndexRange.size == rhs.userIndexRange.size &&
         lhs.bindingOverrideRange.offset == rhs.bindingOverrideRange.offset &&
         lhs.bindingOverrideRange.size == rhs.bindingOverrideRange.size &&
         lhs.bindingSnapshotRange.offset == rhs.bindingSnapshotRange.offset &&
         lhs.bindingSnapshotRange.size == rhs.bindingSnapshotRange.size &&
         lhs.uniformHandle == rhs.uniformHandle;
}

bool sameDrawRunRecord(const dxmt9::core::DrawRunCommandRecord& lhs,
                       const dxmt9::core::DrawRunCommandRecord& rhs) {
  return lhs.stateIndex == rhs.stateIndex &&
         lhs.firstParam == rhs.firstParam &&
         lhs.paramCount == rhs.paramCount &&
         lhs.payloadOffset == rhs.payloadOffset &&
         lhs.payloadSize == rhs.payloadSize &&
         lhs.uniformHandle == rhs.uniformHandle &&
         lhs.renderPsoHandle == rhs.renderPsoHandle &&
         lhs.tilePsoHandle == rhs.tilePsoHandle &&
         lhs.depthStencilHandle == rhs.depthStencilHandle &&
         lhs.invariant.viewportScissorHash == rhs.invariant.viewportScissorHash &&
         lhs.invariant.runStableBindingHash == rhs.invariant.runStableBindingHash &&
         lhs.invariant.streamMask == rhs.invariant.streamMask &&
         lhs.invariant.textureMask == rhs.invariant.textureMask &&
         lhs.invariant.samplerStateMask == rhs.invariant.samplerStateMask;
}

bool sameUniformRecord(const dxmt9::core::DrawUniformPayloadRecord& lhs,
                       const dxmt9::core::DrawUniformPayloadRecord& rhs) {
  return lhs.handle == rhs.handle && lhs.fixedHandle == rhs.fixedHandle &&
         lhs.vertexConstantsHandle == rhs.vertexConstantsHandle &&
         lhs.pixelConstantsHandle == rhs.pixelConstantsHandle &&
         lhs.vertexConstantsHash == rhs.vertexConstantsHash &&
         lhs.pixelConstantsHash == rhs.pixelConstantsHash &&
         lhs.fixedPayloadHash == rhs.fixedPayloadHash && lhs.hash == rhs.hash;
}

bool sameSlotSemantic(const ChunkSlot& lhs, const ChunkSlot& rhs) {
  if (lhs.commandHeaders.size() != rhs.commandHeaders.size() ||
      lhs.drawHotStates != rhs.drawHotStates ||
      lhs.drawShaderLayouts != rhs.drawShaderLayouts ||
      lhs.drawDebugSnapshots != rhs.drawDebugSnapshots ||
      lhs.drawPsoSubviews != rhs.drawPsoSubviews ||
      lhs.drawUniformFixedPayloads.size() != rhs.drawUniformFixedPayloads.size() ||
      lhs.drawUniformVertexConstants.size() !=
          rhs.drawUniformVertexConstants.size() ||
      lhs.drawUniformPixelConstants.size() != rhs.drawUniformPixelConstants.size() ||
      lhs.drawUniformPayloads.size() != rhs.drawUniformPayloads.size() ||
      lhs.drawParams.size() != rhs.drawParams.size() ||
      lhs.drawRunRecords.size() != rhs.drawRunRecords.size() ||
      lhs.drawPayloadArena != rhs.drawPayloadArena ||
      lhs.drawUniformVertexConstantBytes != rhs.drawUniformVertexConstantBytes ||
      lhs.drawUniformPixelConstantBytes != rhs.drawUniformPixelConstantBytes ||
      lhs.drawUniformPayloadLookupHeads != rhs.drawUniformPayloadLookupHeads ||
      lhs.drawUniformPayloadLookupTails != rhs.drawUniformPayloadLookupTails ||
      lhs.drawUniformPayloadLookupNext != rhs.drawUniformPayloadLookupNext ||
      lhs.drawUniformVertexConstantsLookupHeads !=
          rhs.drawUniformVertexConstantsLookupHeads ||
      lhs.drawUniformVertexConstantsLookupTails !=
          rhs.drawUniformVertexConstantsLookupTails ||
      lhs.drawUniformVertexConstantsLookupNext !=
          rhs.drawUniformVertexConstantsLookupNext ||
      lhs.drawUniformPixelConstantsLookupHeads !=
          rhs.drawUniformPixelConstantsLookupHeads ||
      lhs.drawUniformPixelConstantsLookupTails !=
          rhs.drawUniformPixelConstantsLookupTails ||
      lhs.drawUniformPixelConstantsLookupNext !=
          rhs.drawUniformPixelConstantsLookupNext) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.commandHeaders.size(); ++i) {
    if (lhs.commandHeaders[i].kind != rhs.commandHeaders[i].kind ||
        lhs.commandHeaders[i].payloadIndex.value !=
            rhs.commandHeaders[i].payloadIndex.value) {
      return false;
    }
  }
  for (std::size_t i = 0; i < lhs.drawUniformFixedPayloads.size(); ++i) {
    if (lhs.drawUniformFixedPayloads[i].handle !=
            rhs.drawUniformFixedPayloads[i].handle ||
        lhs.drawUniformFixedPayloads[i].payload !=
            rhs.drawUniformFixedPayloads[i].payload) {
      return false;
    }
  }
  for (std::size_t i = 0; i < lhs.drawUniformVertexConstants.size(); ++i) {
    if (lhs.drawUniformVertexConstants[i].handle !=
            rhs.drawUniformVertexConstants[i].handle ||
        lhs.drawUniformVertexConstants[i].constants !=
            rhs.drawUniformVertexConstants[i].constants) {
      return false;
    }
  }
  for (std::size_t i = 0; i < lhs.drawUniformPixelConstants.size(); ++i) {
    if (lhs.drawUniformPixelConstants[i].handle !=
            rhs.drawUniformPixelConstants[i].handle ||
        lhs.drawUniformPixelConstants[i].constants !=
            rhs.drawUniformPixelConstants[i].constants) {
      return false;
    }
  }
  for (std::size_t i = 0; i < lhs.drawUniformPayloads.size(); ++i) {
    if (!sameUniformRecord(lhs.drawUniformPayloads[i],
                           rhs.drawUniformPayloads[i])) {
      return false;
    }
  }
  for (std::size_t i = 0; i < lhs.drawParams.size(); ++i) {
    if (!sameDrawParam(lhs.drawParams[i], rhs.drawParams[i])) {
      return false;
    }
  }
  for (std::size_t i = 0; i < lhs.drawRunRecords.size(); ++i) {
    if (!sameDrawRunRecord(lhs.drawRunRecords[i], rhs.drawRunRecords[i])) {
      return false;
    }
  }
  return true;
}

CanonicalDrawState makeState(bool incompatible, std::uint32_t debugPrimitive) {
  CanonicalDrawState state{};
  state.debug.primitiveCount = debugPrimitive;
  if (incompatible) {
    state.hot.renderStates.entries[0] = {.state = 1u, .value = 1u};
    state.hot.renderStates.count = 1u;
    state.hot.renderStates.hash = 0xfeedu;
  }
  return state;
}

DrawUniformPayload makeUniform(std::uint64_t delta) {
  DrawUniformPayload payload{};
  payload.hash = delta;
  payload.fixedPayloadHash = delta ^ 0x1000u;
  payload.vertexConstantsHash = delta ^ 0x2000u;
  payload.pixelConstantsHash = delta ^ 0x3000u;
  return payload;
}

DrawParam makeDraw(std::uint32_t primitiveCount, std::uint32_t startVertex) {
  DrawParam draw{};
  draw.primitiveCount = primitiveCount;
  draw.startVertex = startVertex;
  draw.instanceCount = 1u;
  return draw;
}

void appendReference(ChunkSlot& expected, const DrawRunBatchEntry& entry,
                     bool extend) {
  const bool appended = extend
      ? expected.appendCompatibleDrawRunEntry(
            *entry.state, *entry.uniforms, entry.draws, entry.payloads)
      : expected.appendDrawRun(
            *entry.state, *entry.uniforms, entry.draws, entry.payloads,
            {}, /*stampUniformHandle=*/true);
  check(appended, "scalar ChunkSlot reference append must succeed");
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

std::uint64_t bufferArenaOperationCount(
    dxmt9::UploadLockOperation operation) {
  dxmt9::UploadLockSnapshot snapshot;
  dxmt9::collectUploadArenaLockSnapshot(snapshot);
  std::uint64_t count = 0;
  for (const auto& row : snapshot.buckets) {
    if (row.valid && row.nativeThreadId == dxmt9::uploadArenaNativeThreadId() &&
        row.site == dxmt9::UploadLockSite::BufferArena &&
        row.operation == operation) {
      count += row.count;
    }
  }
  return count;
}

std::vector<DrawRunBatchEntry> makeEntries(
    std::array<CanonicalDrawState, 4>& states,
    std::array<DrawUniformPayload, 4>& uniforms,
    std::array<DrawParam, 4>& draws,
    std::array<DrawParamPayloadView, 4>& payloads) {
  return {
      DrawRunBatchEntry{&states[0], &uniforms[0], {&draws[0], 1},
                        {&payloads[0], 1}},
      DrawRunBatchEntry{&states[1], &uniforms[1], {&draws[1], 1},
                        {&payloads[1], 1}},
      DrawRunBatchEntry{&states[2], &uniforms[2], {&draws[2], 1},
                        {&payloads[2], 1}},
      DrawRunBatchEntry{&states[3], &uniforms[3], {&draws[3], 1},
                        {&payloads[3], 1}},
  };
}

void testDestinationPredicateTable() {
  for (int stopped = 0; stopped != 2; ++stopped) {
    for (int arena = 0; arena != 2; ++arena) {
      for (int direct = 0; direct != 2; ++direct) {
        const bool expected = !stopped && !arena && !direct;
        check(dxmt9::preparedDrawBatchDestinationAvailable(
                  stopped != 0, arena != 0, direct != 0) == expected,
              "destination predicate must reject every stopped/active row");
      }
    }
  }
}

void testBindingSnapshotPayloadByteDelta() {
  using dxmt9::detail::drawRunPayloadBytesAfterBindingSnapshotCapture;
  constexpr std::size_t snapshotBytes = sizeof(DrawBindingSnapshot);
  constexpr std::size_t max = std::numeric_limits<std::size_t>::max();

  check(drawRunPayloadBytesAfterBindingSnapshotCapture(23u, 4u, 4u) == 23u,
        "unchanged capture must retain the prepared payload extent");
  check(drawRunPayloadBytesAfterBindingSnapshotCapture(
            23u, 4u, 7u) == 23u + 3u * snapshotBytes,
        "mixed capture must add only newly synthesized snapshots");
  check(drawRunPayloadBytesAfterBindingSnapshotCapture(23u, 7u, 4u) == max,
        "an invalid shrinking snapshot delta must fail closed");
  check(drawRunPayloadBytesAfterBindingSnapshotCapture(max, 4u, 4u) == max,
        "the initial overflow sentinel must not be discarded");
  check(drawRunPayloadBytesAfterBindingSnapshotCapture(max, 4u, 5u) == max,
        "capture must preserve an overflow sentinel with added snapshots");
  check(drawRunPayloadBytesAfterBindingSnapshotCapture(
            max - snapshotBytes, 0u, 1u) == max,
        "an exact added-byte boundary must saturate at the sentinel");
  check(drawRunPayloadBytesAfterBindingSnapshotCapture(
            max - snapshotBytes + 1u, 0u, 1u) == max,
        "added-byte overflow must saturate instead of wrapping");
  check(drawRunPayloadBytesAfterBindingSnapshotCapture(0u, 0u, max) == max,
        "snapshot-count multiplication overflow must fail closed");
  // Independent bounded repeated-add oracle for the constant-time reducer.
  for (std::size_t base = 0; base != 32u; ++base) {
    for (std::size_t before = 0; before != 8u; ++before) {
      for (std::size_t added = 0; added != 8u; ++added) {
        std::size_t expected = base;
        for (std::size_t i = 0; i != added; ++i) expected += snapshotBytes;
        check(drawRunPayloadBytesAfterBindingSnapshotCapture(
                  base, before, before + added) == expected,
              "delta reducer must equal independent per-snapshot accumulation");
      }
    }
  }
}

void testBatchMatchesScalarReferenceAndOwnsInputs() {
  using namespace dxmt9;
  using namespace dxmt9::core;

  std::array<CanonicalDrawState, 4> states{
      makeState(false, 10u), makeState(false, 20u), makeState(true, 30u),
      makeState(false, 40u)};
  // A, A, B, A: A entries are run-compatible, but each uniform is a distinct
  // immutable delta. B has a different render-state set and must split.
  std::array<DrawUniformPayload, 4> uniforms{
      makeUniform(0xa1u), makeUniform(0xa2u), makeUniform(0xb1u),
      makeUniform(0xa3u)};
  std::array<DrawParam, 4> draws{
      makeDraw(2u, 1u), makeDraw(3u, 4u), makeDraw(4u, 7u), makeDraw(5u, 9u)};

  const std::array<u8, 2> vertex0{1u, 2u};
  const std::array<u8, 3> index1{3u, 4u, 5u};
  const std::array<u8, 1> vertex2{6u};
  std::array<u8, 2> vertex3{7u, 8u};
  DrawBindingSnapshot snapshot{};
  snapshot.streamMask = 1u;
  snapshot.streams[0].buffer = Handle{0x55u};
  snapshot.streams[0].stride = 16u;
  snapshot.streams[0].snapshot = DrawBufferBindingSnapshot{
      .metalHandle = 0x1234u,
      .contentsAddress = 0x5678u,
      .byteSize = 64u,
      .contentRevision = 9u,
  };
  DrawBindingOverride override{};
  override.streamMask = 1u;
  override.streams[0].buffer = Handle{0x55u};
  override.streams[0].stride = 16u;
  std::array<DrawParamPayloadView, 4> payloads{
      DrawParamPayloadView{.userVertexData = vertex0},
      DrawParamPayloadView{.userIndexData = index1,
                           .bindingSnapshotData = drawBindingSnapshotBytes(snapshot)},
      DrawParamPayloadView{.userVertexData = vertex2},
      DrawParamPayloadView{.userVertexData = vertex3,
                           .bindingOverrideData = drawBindingOverrideBytes(override)},
  };
  auto entries = makeEntries(states, uniforms, draws, payloads);

  ChunkSlot expected;
  appendReference(expected, entries[0], false);
  appendReference(expected, entries[1], true);
  appendReference(expected, entries[2], false);
  appendReference(expected, entries[3], false);

  CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  check(queue.submitDrawRunBatch(entries),
        "native null-Metal queue must accept the valid batch");

  // Mutating every borrowed source after return must not mutate queue-owned
  // records or payload bytes. This also covers the binding-snapshot byte copy.
  states[0].hot.renderStates.entries[0].value = 99u;
  uniforms[0].hash = 0xffffu;
  vertex3[0] = 0xeeu;
  snapshot.streams[0].snapshot.contentRevision = 77u;
  override.streams[0].stride = 99u;

  const auto actual = CommandQueueArenaLeaseTestAccess::copyWritingSlot(queue);
  check(actual.commandHeaders.size() == 3u && actual.drawRunRecords.size() == 3u,
        "A A B A must produce three ordered DrawRun records");
  check(actual.drawRunRecords[0].paramCount == 2u &&
            actual.drawRunRecords[1].paramCount == 1u &&
            actual.drawRunRecords[2].paramCount == 1u,
        "compatible A entries must fold while B and trailing A split");
  check(sameSlotSemantic(actual, expected),
        "queue output must equal the scalar ChunkSlot reference exactly");

  const auto snapshotBytes = drawPayloadRangeBytes(
      actual.drawParams[1].bindingSnapshotRange, actual.drawPayloadArena);
  check(snapshotBytes.size() == sizeof(DrawBindingSnapshot),
        "binding snapshot payload must be owned in the final slot");
  DrawBindingSnapshot copied{};
  std::memcpy(&copied, snapshotBytes.data(), sizeof(copied));
  check(copied.streams[0].snapshot.contentRevision == 9u,
        "copied binding snapshot must retain the pre-submit revision");

  const auto beforeInvalid = actual;
  const std::array<DrawRunBatchEntry, 2> invalidEntries{
      entries[0], DrawRunBatchEntry{}};
  check(!queue.submitDrawRunBatch(invalidEntries),
        "an invalid entry must reject the whole batch");
  const auto afterInvalid = CommandQueueArenaLeaseTestAccess::copyWritingSlot(queue);
  check(sameSlotSemantic(afterInvalid, beforeInvalid),
        "invalid preflight must not partially append a valid prefix");
}

void testLiveSnapshotCaptureAndWatermarks() {
  using namespace dxmt9;
  using namespace dxmt9::core;

  CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  BufferDesc desc{};
  desc.size = 64u;
  desc.pool = Pool::Default;
  desc.usage = UsageDynamic;
  const auto buffer = queue.pool().createBuffer(
      WMT::Device{NULL_OBJECT_HANDLE}, desc);
  auto* record = queue.pool().findBuffer(buffer.value);
  check(record != nullptr && record->isDynamicRename,
        "null-Metal dynamic buffer must create a versioned record");
  record->renameRing.emplace_back();
  constexpr obj_handle_t activeBacking = 0xa0u;
  constexpr obj_handle_t staleBacking = 0xb0u;
  record->buffer.handle = activeBacking;
  record->renameRing[0].buffer.handle = activeBacking;
  record->renameRing[1].buffer.handle = staleBacking;
  record->renameActiveIndex = 0u;
  SyntheticBufferHandleGuard handleGuard{record};

  CanonicalDrawState state{};
  state.hot.streamBuffers[0] = buffer;
  state.hot.streamMask = 1u;
  state.hot.streamStrides[0] = 16u;
  DrawUniformPayload uniforms = makeUniform(0xc1u);
  const std::array<DrawParam, 3> draws{
      makeDraw(1u, 3u), makeDraw(1u, 6u), makeDraw(1u, 9u)};
  const DrawRunBatchEntry entry{
      .state = &state,
      .uniforms = &uniforms,
      .draws = draws,
      .payloads = {},
  };

  const auto updateManyBefore = bufferArenaOperationCount(
      UploadLockOperation::UpdateMany);
  const auto updateBefore = bufferArenaOperationCount(UploadLockOperation::Update);
  check(queue.submitDrawRunBatch(std::span<const DrawRunBatchEntry>(&entry, 1u)),
        "live dynamic binding batch must submit");

  const auto actual = CommandQueueArenaLeaseTestAccess::copyWritingSlot(queue);
  check(actual.drawParams.size() == draws.size() && actual.drawRunRecords.size() == 1u,
        "live binding fixture emits three draws in one run");
  const auto& param = actual.drawParams[0];
  check(param.bindingSnapshotRange.size == sizeof(DrawBindingSnapshot) &&
            actual.drawRunRecords[0].payloadSize == draws.size() * sizeof(DrawBindingSnapshot) &&
            actual.drawPayloadArena.size() == draws.size() * sizeof(DrawBindingSnapshot) &&
            actual.drawPayloadArena.capacity() >= draws.size() * sizeof(DrawBindingSnapshot),
        "synthesized binding snapshot reserves and owns its exact payload size");
  DrawBindingSnapshot copied{};
  std::memcpy(&copied,
              actual.drawPayloadArena.data() + param.bindingSnapshotRange.offset,
              sizeof(copied));
  check(copied.streamMask == 1u && copied.streams[0].buffer == buffer &&
            copied.streams[0].snapshot.metalHandle == activeBacking &&
            copied.streams[0].snapshot.byteSize == desc.size &&
            copied.streams[0].snapshot.contentRevision == record->contentRevision,
        "synthesized snapshot captures the active backing and logical metadata");

  const auto* after = queue.pool().findBuffer(buffer.value);
  check(after != nullptr && after->lastUsedSeqId == 1u &&
            after->renameRing[0].lastUsedSeqId == 1u &&
            after->renameRing[1].lastUsedSeqId == 0u,
        "live logical and active-backing watermarks advance independently");
  if (uploadArenaLockSplitEnabled()) {
    const auto updateManyDelta = bufferArenaOperationCount(
                                     UploadLockOperation::UpdateMany) -
                                 updateManyBefore;
    check(updateManyDelta == (lockScopeRefinementEnabled() ? 2u : 1u),
          "queue marks captured and logical bindings through update_many");
    check(bufferArenaOperationCount(UploadLockOperation::Update) - updateBefore ==
              (lockScopeRefinementEnabled() ? 0u : draws.size()),
          "three captured scalar acquisitions become one bounded batch acquisition");
  }
}

void testMixedCapturedLiveAndImplicitSnapshotPayloads() {
  using namespace dxmt9;
  using namespace dxmt9::core;

  CommandQueue queue(CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  BufferDesc desc{};
  desc.size = 64u;
  desc.pool = Pool::Default;
  desc.usage = UsageDynamic;
  const auto buffer = queue.pool().createBuffer(
      WMT::Device{NULL_OBJECT_HANDLE}, desc);
  auto* record = queue.pool().findBuffer(buffer.value);
  check(record != nullptr && record->isDynamicRename,
        "mixed snapshot fixture must create a versioned buffer");
  record->renameRing.emplace_back();
  constexpr obj_handle_t activeBacking = 0xc0u;
  record->buffer.handle = activeBacking;
  record->renameRing[0].buffer.handle = activeBacking;
  record->renameActiveIndex = 0u;
  SyntheticBufferHandleGuard handleGuard{record};

  CanonicalDrawState state{};
  state.hot.streamBuffers[0] = buffer;
  state.hot.streamMask = 1u;
  state.hot.streamStrides[0] = 16u;
  DrawUniformPayload uniforms = makeUniform(0xd1u);

  DrawBindingSnapshot captured{};
  captured.streamMask = 1u;
  captured.streams[0].buffer = buffer;
  captured.streams[0].stride = 16u;
  captured.streams[0].snapshot = DrawBufferBindingSnapshot{
      .metalHandle = activeBacking,
      .contentsAddress = 0x7788u,
      .byteSize = desc.size,
      .contentRevision = record->contentRevision,
  };
  const auto capturedBytes = drawBindingSnapshotBytes(captured);
  const std::array<u8, 3> userVertices{0x11u, 0x22u, 0x33u};
  const std::array<u8, 5> userIndices{0x44u, 0x55u, 0x66u, 0x77u, 0x88u};

  const std::array<DrawParam, 2> firstDraws{
      makeDraw(1u, 1u), makeDraw(1u, 2u)};
  const std::array<DrawParamPayloadView, 2> firstPayloads{
      DrawParamPayloadView{.userVertexData = userVertices,
                           .userIndexData = userIndices,
                           .bindingSnapshotData = capturedBytes},
      DrawParamPayloadView{},
  };
  const std::array<DrawParam, 2> secondDraws{
      makeDraw(1u, 3u), makeDraw(1u, 4u)};
  // The second draw has an implicit empty payload because this span is short.
  const std::array<DrawParamPayloadView, 1> secondPayloads{
      DrawParamPayloadView{.bindingSnapshotData = capturedBytes},
  };
  const std::array<DrawRunBatchEntry, 2> entries{
      DrawRunBatchEntry{.state = &state,
                        .uniforms = &uniforms,
                        .draws = firstDraws,
                        .payloads = firstPayloads},
      DrawRunBatchEntry{.state = &state,
                        .uniforms = &uniforms,
                        .draws = secondDraws,
                        .payloads = secondPayloads},
  };

  check(queue.submitDrawRunBatch(entries),
        "mixed captured/live/implicit payload batch must submit");
  const auto actual = CommandQueueArenaLeaseTestAccess::copyWritingSlot(queue);
  constexpr std::size_t expectedBytes = 4u * sizeof(DrawBindingSnapshot) + 8u;
  check(actual.drawParams.size() == 4u && actual.drawRunRecords.size() == 1u,
        "mixed entries must remain one compatible draw run");
  check(actual.drawRunRecords[0].payloadSize == expectedBytes &&
            actual.drawPayloadArena.size() == expectedBytes,
        "final payload bytes must equal the exact captured/live extent");
  const auto copiedVertices = drawPayloadRangeBytes(
      actual.drawParams[0].userVertexRange, actual.drawPayloadArena);
  const auto copiedIndices = drawPayloadRangeBytes(
      actual.drawParams[0].userIndexRange, actual.drawPayloadArena);
  check(copiedVertices.size() == userVertices.size() &&
            copiedIndices.size() == userIndices.size() &&
            std::memcmp(copiedVertices.data(), userVertices.data(), userVertices.size()) == 0 &&
            std::memcmp(copiedIndices.data(), userIndices.data(), userIndices.size()) == 0,
        "live snapshot settlement must preserve other payload roles byte-for-byte");
  for (const auto& param : actual.drawParams) {
    check(param.bindingSnapshotRange.size == sizeof(DrawBindingSnapshot),
          "every mixed draw must own exactly one binding snapshot");
    const auto bytes = drawPayloadRangeBytes(param.bindingSnapshotRange,
                                             actual.drawPayloadArena);
    DrawBindingSnapshot copied{};
    std::memcpy(&copied, bytes.data(), sizeof(copied));
    check(copied.streamMask == 1u && copied.streams[0].buffer == buffer &&
              copied.streams[0].snapshot.metalHandle == activeBacking,
          "captured and synthesized snapshots must preserve the active backing");
  }
}

}  // namespace

int main() {
  try {
    testDestinationPredicateTable();
    testBindingSnapshotPayloadByteDelta();
    testBatchMatchesScalarReferenceAndOwnsInputs();
    testLiveSnapshotCaptureAndWatermarks();
    testMixedCapturedLiveAndImplicitSnapshotPayloads();
    std::cout << "queue lock-scope refinement spec: ok\n";
    return 0;
  } catch (const TestFailure& failure) {
    std::cerr << "queue lock-scope refinement spec: " << failure.what() << '\n';
    return 1;
  }
}
