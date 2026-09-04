// Queue-local ordinary DrawRunBatchEntry folding.
//
// The production queue owns resource stamping and slot-split decisions; the
// final-storage primitive tested here owns the invariant that compatible
// adjacent entries share one canonical state/DrawRun while every entry keeps
// its own interned uniform and every draw keeps its own payload bytes.

#include "../../../src/dxmt9/dxmt9_backend_types.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace dxmt9::core;

int failures = 0;

void check(bool condition, const std::string& message) {
  if (condition) return;
  std::fprintf(stderr, "FAIL: %s\n", message.c_str());
  ++failures;
}

CanonicalDrawState state(unsigned identity) {
  CanonicalDrawState value{};
  if (identity != 0u) {
    value.hot.viewport.viewport.width = identity;
    value.hot.key.viewportHash = identity;
  }
  return value;
}

DrawUniformPayload uniform(unsigned identity) {
  DrawUniformPayload value{};
  value.clipPlaneMask = identity;
  value.fixedPayloadHash = 100u + identity;
  value.hash = 200u + identity;
  return value;
}

DrawParam draw(unsigned identity) {
  DrawParam value{};
  value.primitiveCount = identity + 1u;
  value.startVertex = identity * 3u;
  return value;
}

struct FoldCounts {
  std::size_t inputRuns = 0;
  std::size_t emittedRuns = 0;
  std::size_t baseResourceMarks = 0;
  std::size_t overrideMarks = 0;
  std::size_t snapshotMarks = 0;
};

bool appendBatch(ChunkSlot& slot,
                 std::span<CanonicalDrawState> states,
                 std::span<const DrawUniformPayload> uniforms,
                 std::span<const DrawParam> draws,
                 std::span<const DrawParamPayloadView> payloads,
                 FoldCounts& counts) {
  bool runOpen = false;
  for (std::size_t i = 0; i < states.size(); ++i) {
    ++counts.inputRuns;
    const bool extend = runOpen && slot.lastDrawRunCompatible(states[i]);
    if (!extend) {
      ++counts.emittedRuns;
      ++counts.baseResourceMarks;
    }
    counts.overrideMarks += !payloads[i].bindingOverrideData.empty();
    counts.snapshotMarks += !payloads[i].bindingSnapshotData.empty();
    const std::span<const DrawParam> oneDraw(&draws[i], 1u);
    const std::span<const DrawParamPayloadView> onePayload(&payloads[i], 1u);
    const bool appended = extend
        ? slot.appendCompatibleDrawRunEntry(
              states[i], uniforms[i], oneDraw, onePayload)
        : slot.appendDrawRun(states[i], uniforms[i], oneDraw, onePayload,
                             {}, /*stampUniformHandle=*/true);
    if (!appended) return false;
    runOpen = true;
  }
  return true;
}

struct DrawSemantic {
  std::uint32_t primitiveCount = 0;
  std::uint32_t uniformClipPlaneMask = 0;
  std::vector<u8> bindingOverride{};
  std::vector<u8> bindingSnapshot{};

  friend bool operator==(const DrawSemantic&, const DrawSemantic&) = default;
};

std::vector<DrawSemantic> semantics(const ChunkSlot& slot) {
  std::vector<DrawSemantic> result;
  for (std::size_t commandIndex = 0; commandIndex < slot.commandCount();
       ++commandIndex) {
    const auto command = slot.commandAt(commandIndex);
    for (const auto& param : command.drawParams) {
      DrawSemantic semantic{};
      semantic.primitiveCount = param.primitiveCount;
      const auto* uniformRecord = slot.drawUniformPayloadRecord(
          param.uniformHandle);
      if (uniformRecord) {
        const auto* fixedRecord =
            slot.drawUniformFixedPayloadRecord(uniformRecord->fixedHandle);
        if (fixedRecord) {
          semantic.uniformClipPlaneMask = fixedRecord->payload.clipPlaneMask;
        }
      }
      const auto overrideBytes = drawPayloadRangeBytes(
          param.bindingOverrideRange, command.drawPayloadBytes);
      semantic.bindingOverride.assign(overrideBytes.begin(),
                                      overrideBytes.end());
      const auto snapshotBytes = drawPayloadRangeBytes(
          param.bindingSnapshotRange, command.drawPayloadBytes);
      semantic.bindingSnapshot.assign(snapshotBytes.begin(),
                                      snapshotBytes.end());
      result.push_back(std::move(semantic));
    }
  }
  return result;
}

void compatibleFoldMatchesSerialAndOwnsInput() {
  std::array states{state(0), state(0), state(0)};
  const std::array uniforms{uniform(1), uniform(2), uniform(3)};
  const std::array draws{draw(0), draw(1), draw(2)};
  std::array<std::array<u8, 2>, 3> overrides{{{{1, 11}}, {{2, 12}}, {{3, 13}}}};
  std::array<std::array<u8, 1>, 3> snapshots{{{{21}}, {{22}}, {{23}}}};
  std::array<DrawParamPayloadView, 3> payloads{};
  for (std::size_t i = 0; i < payloads.size(); ++i) {
    payloads[i].bindingOverrideData = overrides[i];
    payloads[i].bindingSnapshotData = snapshots[i];
  }

  ChunkSlot folded;
  FoldCounts counts{};
  check(appendBatch(folded, states, uniforms, draws, payloads, counts),
        "three compatible entries append into final storage");

  ChunkSlot serial;
  for (std::size_t i = 0; i < states.size(); ++i) {
    check(serial.appendDrawRun(states[i], uniforms[i],
                               std::span<const DrawParam>(&draws[i], 1u),
                               std::span<const DrawParamPayloadView>(
                                   &payloads[i], 1u),
                               {}, /*stampUniformHandle=*/true),
          "serial reference entry appends");
  }

  check(folded.commandHeaders.size() == 1u &&
            folded.drawRunRecords.size() == 1u &&
            folded.drawHotStates.size() == 1u &&
            folded.drawShaderLayouts.size() == 1u &&
            folded.drawDebugSnapshots.size() == 1u &&
            folded.drawParams.size() == 3u,
        "compatible entries store one canonical state and DrawRun");
  check(folded.drawRunRecords[0].paramCount == 3u &&
            folded.drawUniformPayloads.size() == 3u &&
            folded.drawParams[0].uniformHandle.valid() &&
            folded.drawParams[1].uniformHandle.valid() &&
            folded.drawParams[2].uniformHandle.valid() &&
            folded.drawParams[0].uniformHandle !=
                folded.drawParams[1].uniformHandle &&
            folded.drawParams[1].uniformHandle !=
                folded.drawParams[2].uniformHandle,
        "each entry uniform is interned and stamped into its copied draws");
  check(semantics(folded) == semantics(serial),
        "folded and serial final slots expose the same effective draw stream");
  check(counts.inputRuns == 3u && counts.emittedRuns == 1u &&
            counts.baseResourceMarks == 1u && counts.overrideMarks == 3u &&
            counts.snapshotMarks == 3u,
        "one shared base mark accompanies one emitted run while every concrete binding remains exact");

  for (auto& bytes : overrides) bytes.fill(0xffu);
  for (auto& bytes : snapshots) bytes.fill(0xeeu);
  check(semantics(folded) == semantics(serial),
        "final-slot payload bytes outlive and do not alias caller storage");
}

void abaOnlyFoldsAdjacentCompatibleEntries() {
  std::array states{state(0), state(1), state(0)};
  const std::array uniforms{uniform(1), uniform(2), uniform(3)};
  const std::array draws{draw(0), draw(1), draw(2)};
  const std::array payloads{DrawParamPayloadView{}, DrawParamPayloadView{},
                            DrawParamPayloadView{}};
  ChunkSlot slot;
  FoldCounts counts{};
  check(appendBatch(slot, states, uniforms, draws, payloads, counts),
        "A-B-A entries append");
  check(slot.commandHeaders.size() == 3u &&
            slot.drawRunRecords.size() == 3u &&
            slot.drawHotStates.size() == 3u && counts.emittedRuns == 3u &&
            counts.baseResourceMarks == 3u,
        "A-B-A remains three ordered runs and never folds through history");
}

void capacityFaultAndTopologyFailureDoNotAppendAPrefix() {
  const std::array draws{draw(0)};
  const std::array payloads{DrawParamPayloadView{}};
  auto base = state(0);
  const auto uniforms = uniform(1);

  ChunkSlot capacity;
  check(capacity.appendDrawRun(base, uniforms, draws, payloads, {}, true),
        "capacity fixture base run appends");
  capacity.drawRunRecords[0].paramCount =
      std::numeric_limits<std::uint32_t>::max();
  const auto paramsBefore = capacity.drawParams.size();
  const auto uniformsBefore = capacity.drawUniformPayloads.size();
  check(!capacity.appendCompatibleDrawRunEntry(
            base, uniform(2), draws, payloads) &&
            capacity.drawParams.size() == paramsBefore &&
            capacity.drawUniformPayloads.size() == uniformsBefore,
        "param-capacity rejection occurs before uniform or draw mutation");

  ChunkSlot topology;
  check(topology.appendDrawRun(base, uniforms, draws, payloads, {}, true),
        "topology fixture base run appends");
  topology.appendClear(ClearDesc{});
  const auto commandsBefore = topology.commandHeaders.size();
  const auto topologyParamsBefore = topology.drawParams.size();
  check(!topology.appendCompatibleDrawRunEntry(
            base, uniform(2), draws, payloads) &&
            topology.commandHeaders.size() == commandsBefore &&
            topology.drawParams.size() == topologyParamsBefore,
        "an intervening command rejects stale continuation without mutation");

  ChunkSlot storageFault;
  check(storageFault.appendDrawRun(base, uniforms, draws, payloads, {}, true),
        "storage-fault fixture base run appends");
  storageFault.drawShaderLayouts.pop_back();
  const auto faultParamsBefore = storageFault.drawParams.size();
  const auto faultUniformsBefore = storageFault.drawUniformPayloads.size();
  check(!storageFault.appendCompatibleDrawRunEntry(
            base, uniform(2), draws, payloads) &&
            storageFault.drawParams.size() == faultParamsBefore &&
            storageFault.drawUniformPayloads.size() == faultUniformsBefore,
        "inconsistent state storage rejects continuation before mutation");
}

}  // namespace

int main() {
  compatibleFoldMatchesSerialAndOwnsInput();
  abaOnlyFoldsAdjacentCompatibleEntries();
  capacityFaultAndTopologyFailureDoNotAppendAPrefix();
  if (failures != 0) {
    std::fprintf(stderr, "%d draw-run batch fold checks failed\n", failures);
    return 1;
  }
  std::puts("draw_run_batch_fold_spec passed");
  return 0;
}
