#include "dxmt9/dxmt9_render_tape_emission_observer.hpp"
#include "dxmt9/dxmt9_cpu_ready_tape.hpp"
#include "dxmt9/dxmt9_source_payload.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using namespace dxmt9;
using namespace dxmt9::render;
constexpr query::WriterIdentity writer{1u, 2u, 3u, 4u};
std::uint64_t checks = 0u;

void check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

struct Probe {
  struct Row {
    query::WriterIdentity writer{};
    std::uint32_t physical = 0u;
    core::MetalCommandKind kind{};
    std::uint32_t subdraw = 0u;
    std::vector<TapeEmissionRecordLocator> records{};
  };
  std::vector<Row> rows{};
  std::uint32_t failures = 0u;
  bool reject = false;
  static bool bind(void* context, query::WriterIdentity writer,
      std::uint32_t physical, core::MetalCommandKind kind,
      std::span<const TapeEmissionRecordLocator> records, std::uint32_t subdraw) noexcept {
    auto& probe = *static_cast<Probe*>(context);
    if (probe.reject) return false;
    try {
      probe.rows.push_back({writer, physical, kind, subdraw,
          std::vector<TapeEmissionRecordLocator>(records.begin(), records.end())});
      return true;
    } catch (...) { return false; }
  }
  static void fail(void* context) noexcept { ++static_cast<Probe*>(context)->failures; }
  TapeEmissionObserverSink sink() noexcept { return {this, bind, fail}; }
};

void compatibilityBatchCopiesOriginalLocatorsAcrossCuts() {
  Probe probe;
  TapeEmissionObserver observer(probe.sink());
  observer.beginRaw(19u);
  observer.recordIndex(7u);
  observer.acceptCompatibilityDraw();
  observer.recordIndex(8u); // constant upload: not a draw
  observer.recordIndex(9u);
  observer.acceptCompatibilityDraw();
  observer.recordIndex(10u); // capacity flush is under the incoming record
  check(observer.flushCompatibilityBatch([&] {
    observer.append(writer, 4u, core::MetalCommandKind::DrawRun, 2u, 2u);
    return true;
  }), "diagnostics preserve actual successful capacity-flush result");
  check(probe.rows.size() == 1u && probe.rows[0].subdraw == 2u &&
      probe.rows[0].records == std::vector<TapeEmissionRecordLocator>{{19u, 7u}, {19u, 9u}},
      "prior accepted draws retain original import-global indices across intervening upload");
  observer.acceptCompatibilityDraw();
  observer.recordIndex(12u);
  observer.acceptCompatibilityDraw();
  const query::WriterIdentity successor{1u, 3u, 5u, 6u};
  check(observer.flushCompatibilityBatch([&] {
    observer.append(writer, 5u, core::MetalCommandKind::DrawRun, 0u, 1u);
    observer.append(successor, 0u, core::MetalCommandKind::DrawRun, 0u, 1u);
    return true;
  }), "one synchronous source-split flush retains production result");
  check(probe.rows.size() == 3u && probe.rows[1].records.front() == TapeEmissionRecordLocator{19u, 10u} &&
      probe.rows[2].writer == successor &&
      probe.rows[2].records.front() == TapeEmissionRecordLocator{19u, 12u} && !observer.failed(),
      "flush cursor follows actual physical writer split without span-local renumbering");
}

core::SourcePayloadCapacity drawCapacity(std::size_t count) {
  core::SourcePayloadCapacity capacity{};
  capacity.commandHeaders = count + 2u;
  capacity.clearRecords = 1u;
  capacity.presentRecords = 1u;
  capacity.drawHotStates = capacity.drawShaderLayouts = capacity.drawDebugSnapshots = count;
  capacity.drawPsoSubviews = capacity.drawRunRecords = capacity.drawParams = count;
  capacity.drawUniformFixedPayloads = capacity.drawUniformPayloads = count;
  capacity.drawUniformVertexConstants = capacity.drawUniformPixelConstants = count;
  capacity.drawUniformVertexConstantBytes = count * sizeof(core::VertexShaderConstants);
  capacity.drawUniformPixelConstantBytes = count * sizeof(core::PixelShaderConstants);
  const auto buckets = core::detail::chunkSlotUniformLookupBucketCount(count);
  capacity.drawUniformPayloadLookupHeads = capacity.drawUniformPayloadLookupTails = buckets;
  capacity.drawUniformPayloadLookupNext = count;
  capacity.drawUniformVertexConstantsLookupHeads = capacity.drawUniformVertexConstantsLookupTails = buckets;
  capacity.drawUniformVertexConstantsLookupNext = count;
  capacity.drawUniformPixelConstantsLookupHeads = capacity.drawUniformPixelConstantsLookupTails = buckets;
  capacity.drawUniformPixelConstantsLookupNext = count;
  return capacity;
}

void actualDirectAssemblerSuppliesHeaderAndPriorParamCount() {
  Probe probe;
  TapeEmissionObserver observer(probe.sink());
  core::ChunkSlot slot;
  slot.appendClear({}); // committed Writing prefix outside the new attempt
  core::TransactionalChunkSlotAssembler assembler(slot, drawCapacity(3u));
  core::FlatDrawStateRecord hot{};
  core::DrawShaderLayoutContext layout{};
  core::DrawUniformPayload uniforms{};
  const core::DirectReplayDrawInput input{
      .hot = &hot, .shaderLayout = &layout, .uniforms = &uniforms,
      .draw = {.primitiveCount = 1u}};
  ScopedTapeEmissionObserver scope(observer);
  observer.withWriter(writer, 0u, [&] {
    observer.record({7u, 4u});
    check(assembler.tryAppendDirectDraw(input), "actual Direct new DrawRun succeeds");
    observer.record({7u, 6u});
    check(assembler.tryAppendDirectDraw(input), "actual Direct compatible draw extends physical run");
    observer.record({7u, 8u});
    check(assembler.tryAppendClear(core::ClearDesc{}), "actual nonDraw closes physical Direct run");
    observer.record({7u, 10u});
    check(assembler.tryAppendDirectDraw(input), "actual Direct post-cut run starts another header");
  });
  check(probe.rows.size() == 4u && probe.rows[0].physical == 1u && probe.rows[0].subdraw == 0u &&
      probe.rows[1].physical == 1u && probe.rows[1].subdraw == 1u &&
      probe.rows[2].physical == 2u && probe.rows[2].kind == core::MetalCommandKind::Clear &&
      probe.rows[3].physical == 3u && probe.rows[3].subdraw == 0u && !observer.failed(),
      "assembler callbacks reflect successful real headers and exact prior paramCount");
  assembler.rollback();
  observer.discardTentative();
  check(slot.commandCount() == 1u && slot.drawParams.empty(),
      "actual destination rollback restores its committed prefix before cold discard");
}

void arenaPresentUsesActualEarlierSegmentHeaders() {
  const auto capacity = drawCapacity(2u);
  const auto layout = core::makeSourcePayloadLayout(capacity, 4096u, 16u);
  check(layout.has_value(), "bounded actual Arena layout exists");
  auto firstMemory = core::allocateCpuReadyAlignedBytes(layout->usedBytes);
  auto nextMemory = core::allocateCpuReadyAlignedBytes(layout->usedBytes);
  core::ArenaSourcePayloadBlock first, next;
  core::ArenaSourcePayloadBuilder previous(first, *layout, {firstMemory.get(), layout->usedBytes});
  core::ArenaSourcePayloadBuilder builder(next, *layout, {nextMemory.get(), layout->usedBytes});
  check(previous.tryAppendClearCommand({}) && previous.commandCount() == 1u &&
      first.actualCount(core::SourcePayloadRegion::CommandHeaders) == 0u,
      "prior unsealed segment exposes actual constructed headers, not published count cache");
  core::TransactionalChunkSlotAssembler assembler(builder);
  core::FlatDrawStateRecord hot{};
  core::DrawShaderLayoutContext shader{};
  core::DrawUniformPayload uniforms{};
  const core::DirectReplayDrawInput input{
      .hot = &hot, .shaderLayout = &shader, .uniforms = &uniforms,
      .draw = {.primitiveCount = 1u}};
  Probe probe;
  TapeEmissionObserver observer(probe.sink());
  ScopedTapeEmissionObserver scope(observer);
  observer.withWriter(writer, previous.commandCount(), [&] {
    observer.record({5u, 3u});
    check(assembler.tryAppendDirectDraw(input), "actual Arena new run appends");
    observer.record({5u, 5u});
    check(assembler.tryAppendDirectDraw(input), "actual Arena compatible run extends");
    observer.record({5u, 7u});
    // Present intentionally bypasses the assembler, matching production.
    check(builder.tryAppendPresentCommand({}), "actual Arena Present builder append succeeds");
  });
  check(probe.rows.size() == 3u && probe.rows[0].physical == 1u && probe.rows[1].physical == 1u &&
      probe.rows[1].subdraw == 1u && probe.rows[2].physical == 2u &&
      probe.rows[2].kind == core::MetalCommandKind::Present &&
      probe.rows[2].records.front() == TapeEmissionRecordLocator{5u, 7u} && !observer.failed(),
      "Arena exact source-local Present index includes actual earlier segment header");
}

void stagedPresentAndRollbackNeverBorrowCurrentRecord() {
  Probe probe;
  TapeEmissionObserver observer(probe.sink());
  observer.beginRaw(23u);
  observer.recordIndex(11u);
  observer.stagePresent(writer);
  observer.recordIndex(13u);
  check(observer.withStagedPresent(writer, [&] {
    observer.appendLocal(8u, core::MetalCommandKind::Present);
    return true;
  }), "actual deferred Direct Present append return stays unchanged");
  check(probe.rows.size() == 1u && probe.rows[0].records.front() == TapeEmissionRecordLocator{23u, 11u},
      "deferred Present binds its staged singleton, not later record or publication cut");
  observer.recordIndex(14u);
  observer.stagePresent(writer);
  observer.acceptCompatibilityDraw();
  observer.discardTentative(); // only after real destination restore
  observer.recordIndex(11u);
  observer.stagePresent(writer);
  observer.withStagedPresent(writer, [&] { observer.appendLocal(8u, core::MetalCommandKind::Present); });
  check(probe.rows.size() == 2u && probe.rows.back().records.front() == TapeEmissionRecordLocator{23u, 11u} &&
      !observer.failed(), "successful rollback discards actual tentative cold Present and draw locators");
}

void observerFailuresDoNotChangeProductionResults() {
  for (std::uint32_t failure = 0u; failure != 4u; ++failure) {
    Probe probe;
    TapeEmissionObserver observer(probe.sink(), 1u);
    observer.record({2u, 4u});
    observer.acceptCompatibilityDraw();
    if (failure == 0u) observer.acceptCompatibilityDraw(); // budget
    probe.reject = failure == 1u;
    const bool result = observer.flushCompatibilityBatch([&] {
      if (failure != 2u)
        observer.append(failure == 3u ? query::WriterIdentity{} : writer,
            0u, core::MetalCommandKind::DrawRun, 0u, 1u);
      return true;
    });
    check(result && observer.failed() && probe.failures == 1u,
        "budget, bind rejection, missing physical append and foreign writer fail capture only");
  }
  Probe probe;
  probe.reject = true;
  TapeEmissionObserver observer(probe.sink());
  core::ChunkSlot slot;
  core::TransactionalChunkSlotAssembler assembler(slot, drawCapacity(1u));
  core::FlatDrawStateRecord hot{};
  core::DrawShaderLayoutContext shader{};
  core::DrawUniformPayload uniforms{};
  const core::DirectReplayDrawInput input{.hot = &hot, .shaderLayout = &shader, .uniforms = &uniforms};
  ScopedTapeEmissionObserver scope(observer);
  observer.record({2u, 4u});
  check(observer.withWriter(writer, 0u, [&] { return assembler.tryAppendDirectDraw(input); }) &&
      slot.commandCount() == 1u && observer.failed(),
      "real assembler append succeeds unchanged even when diagnostic bind callback rejects");
}
} // namespace

int main() {
  try {
    compatibilityBatchCopiesOriginalLocatorsAcrossCuts();
    actualDirectAssemblerSuppliesHeaderAndPriorParamCount();
    arenaPresentUsesActualEarlierSegmentHeaders();
    stagedPresentAndRollbackNeverBorrowCurrentRecord();
    observerFailuresDoNotChangeProductionResults();
    check(activeTapeEmissionObserver == nullptr, "all synchronous scopes restore disabled TLS");
    std::cout << "render tape emission observer: " << checks << " checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
