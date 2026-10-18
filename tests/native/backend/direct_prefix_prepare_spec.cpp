// R-BACK-2.107: the real full/suffix predicates and single-use authority.
#include "dxmt9/dxmt9_source_payload.hpp"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <type_traits>
#include <vector>

using namespace dxmt9::core;

namespace dxmt9::core {
struct DirectPrefixPrepareTestAccess {
  using Assembler = TransactionalChunkSlotAssembler;
  static auto mint(Assembler& assembler, ChunkSlot& slot,
                   Assembler::OuterBinding binding) {
    return Assembler::PrefixPrepareCapability(assembler, slot, binding);
  }
  static bool prepare(Assembler& assembler,
                      Assembler::PrefixPrepareCapability&& capability) {
    return assembler.prepareOwnedSuffix(std::move(capability));
  }
};
}

namespace {
using Assembler = TransactionalChunkSlotAssembler;
using Access = DirectPrefixPrepareTestAccess;
using Capability = Assembler::PrefixPrepareCapability;
static_assert(!std::is_default_constructible_v<Capability>);
static_assert(!std::is_copy_constructible_v<Capability>);
static_assert(!std::is_move_assignable_v<Capability>);
static_assert(std::is_nothrow_move_constructible_v<Capability>);
static_assert(!std::is_constructible_v<Capability, Assembler&, ChunkSlot&,
                                     Assembler::OuterBinding>);

void check(bool condition, const char* reason) {
  if (!condition) {
    std::fprintf(stderr, "%s\n", reason);
    std::abort();
  }
}

Assembler::OuterBinding binding() {
  return {.rawOrdinal=1, .sourceOrdinal=2, .seqId=2, .buildGeneration=3,
          .sourceGeneration=4, .storageGeneration=5, .controlIndex=0,
          .firstPage=0, .pageCount=1, .segmentIndex=0, .segmentCount=1,
          .plannedBytes=4096};
}

SourcePayloadCapacity capacity(std::size_t draws = 1) {
  SourcePayloadCapacity c{};
  c.commandHeaders = draws + 1;
  c.clearRecords = 1;
  c.drawHotStates = c.drawShaderLayouts = c.drawDebugSnapshots = draws;
  c.drawPsoSubviews = c.drawRunRecords = c.drawParams = draws;
  c.drawUniformPayloads = c.drawUniformFixedPayloads = draws;
  c.drawUniformVertexConstants = c.drawUniformPixelConstants = draws;
  c.drawUniformVertexConstantBytes = draws * sizeof(VertexShaderConstants);
  c.drawUniformPixelConstantBytes = draws * sizeof(PixelShaderConstants);
  return c;
}

struct Fixture {
  ChunkSlot slot;
  FlatDrawStateRecord hot{};
  DrawShaderLayoutContext layout{};
  DrawUniformPayload uniforms{};
  std::optional<Assembler> assembler;

  Fixture() {
    slot.drawHotStates.resize(1);
    slot.drawShaderLayouts.resize(1);
    slot.drawDebugSnapshots.resize(1);
    slot.drawPsoSubviews.resize(1);
    slot.drawRunRecords.resize(1);
    slot.drawParams.resize(1);
    slot.drawUniformPayloads.resize(1);
    slot.drawUniformPayloads[0].handle = {0, 1, 31};
    slot.drawRunRecords[0].paramCount = 1;
    slot.drawRunRecords[0].uniformHandle = {0, 1, 31};
    slot.drawParams[0].uniformHandle = {0, 1, 31};
    slot.commandHeaders.push_back({MetalCommandKind::DrawRun, {0}});
    check(slot.commandPayloadsInRange(), "prefix reference valid");
    assembler.emplace(slot, capacity());
    check(assembler->good() && assembler->bindOuter(binding()), "reserve and bind");
  }
  void append() {
    check(assembler->tryAppendDirectDraw({.hot=&hot, .shaderLayout=&layout,
                                         .uniforms=&uniforms}), "append actual suffix");
    check(slot.drawRunRecords.size() == 2, "old run is never extended");
  }
  auto token() { return Access::mint(*assembler, slot, binding()); }
};

void gateTruthTable() {
  // Complete 2^4 gate domain; names match DirectPrefixPrepare's decision.
  for (unsigned bits = 0; bits != 16; ++bits) {
    const bool active = bits & 1, present = bits & 2;
    const bool match = bits & 4, preserved = bits & 8;
    const auto actual = decidePrefixPrepare(active, present, match, preserved);
    const auto expected = !active ? PrefixPrepareDecision::Reject
        : !present ? PrefixPrepareDecision::Full
        : match && preserved ? PrefixPrepareDecision::Suffix
                             : PrefixPrepareDecision::Reject;
    check(actual == expected, "complete decision truth table");
  }
}

void suffixDifferential() {
  for (unsigned mask = 0; mask != 64; ++mask) {
    Fixture f;
    f.append();
    if (mask & 1) ++f.slot.drawParams.back().uniformHandle.generation;
    if (mask & 2) f.slot.drawRunRecords.back().stateIndex = 99;
    if (mask & 4) f.slot.drawRunRecords.back().paramCount = 99;
    if (mask & 8) f.slot.drawParams.back().userVertexRange.size = 1;
    if (mask & 16) f.slot.commandHeaders.back().payloadIndex = {99};
    if (mask & 32) f.slot.drawDebugSnapshots.pop_back();
    const bool reference = f.slot.commandPayloadsInRange();
    check(f.slot.commandPayloadSuffixInRange(1) == reference,
          "same shared predicate validates every malformed suffix");
    check(Access::prepare(*f.assembler, f.token()) == reference,
          "owned prepare equals full reference in 64 fault combinations");
    f.assembler->rollback();
    check(f.slot.commandPayloadsInRange(), "rollback preserves old valid prefix");
  }
  Fixture f;
  f.append();
  check(!f.slot.commandPayloadSuffixInRange(f.slot.commandCount() + 1),
        "out-of-range suffix cursor rejects");
  ++f.slot.drawUniformPayloads[0].handle.generation;
  check(!f.assembler->prepare(), "raw-value prepare still detects old-prefix corruption");
}

void identityAndTerminal() {
  for (unsigned field = 0; field != 14; ++field) {
    Fixture f;
    auto stale = binding();
    switch (field) {
    case 0: ++stale.rawOrdinal; break;
    case 1: ++stale.sourceOrdinal; break;
    case 2: ++stale.seqId; break;
    case 3: ++stale.buildGeneration; break;
    case 4: ++stale.sourceGeneration; break;
    case 5: ++stale.storageGeneration; break;
    case 6: ++stale.controlIndex; break;
    case 7: ++stale.firstPage; break;
    case 8: ++stale.pageCount; break;
    case 9: ++stale.segmentIndex; break;
    case 10: ++stale.segmentCount; break;
    case 11: ++stale.rangeRecordCount; break;
    case 12: ++stale.rangeDrawCount; break;
    case 13: ++stale.plannedBytes; break;
    }
    check(!Access::prepare(*f.assembler, Access::mint(*f.assembler, f.slot, stale)),
          "every changed identity/plan field rejects");
  }
  {
    Fixture f, other;
    check(!Access::prepare(*f.assembler, other.token()), "foreign assembler rejects");
  }
  {
    Fixture f, other;
    check(!Access::prepare(*f.assembler, Access::mint(*f.assembler, other.slot, binding())),
          "foreign destination rejects");
  }
  {
    Fixture f;
    auto token = f.token();
    auto moved = std::move(token);
    check(Access::prepare(*f.assembler, std::move(moved)), "moved capability consumes once");
    check(!Access::prepare(*f.assembler, std::move(moved)), "duplicate consume rejects");
    check(!Access::prepare(*f.assembler, std::move(token)), "moved-from rejects");
  }
  for (unsigned terminal = 0; terminal != 3; ++terminal) {
    Fixture f;
    auto token = f.token();
    if (terminal == 0) f.assembler->rollback();
    if (terminal == 1) check(f.assembler->commitValueOnlyForTest(), "test commit");
    if (terminal == 2) f.assembler->abandonFailStop();
    const auto state = f.assembler->state();
    check(!Access::prepare(*f.assembler, std::move(token)), "terminal capability rejects");
    check(f.assembler->state() == state, "rejected token never re-enables terminal rollback");
  }
  {
    Fixture f;
    auto stale = f.token();
    auto* address = &*f.assembler;
    f.assembler->rollback();
    f.assembler.reset();
    f.assembler.emplace(f.slot, capacity());
    auto next = binding();
    ++next.buildGeneration;
    check(f.assembler->bindOuter(next) && address == &*f.assembler, "same-address reuse fixture");
    check(!Access::prepare(*f.assembler, std::move(stale)), "same-address build ABA rejects");
  }
}

void everyExtent() {
  std::size_t tested = 0;
#define DXMT9_CHECK_PREFIX_Vector(plan, storage) { \
    ChunkSlot slot; \
    slot.storage.resize(1); \
    slot.drawHotStates.resize(1); \
    slot.drawShaderLayouts.resize(1); \
    slot.drawDebugSnapshots.resize(1); \
    /* Headers must be valid if this is the dimension under test. */ \
    if (!slot.commandHeaders.empty()) { \
      slot.commandHeaders[0] = {MetalCommandKind::Clear, {0}}; \
      slot.clearRecords.resize(1); \
    } \
    Assembler assembler(slot, SourcePayloadCapacity{}); \
    check(assembler.good() && assembler.bindOuter(binding()), "extent reservation"); \
    auto token = Access::mint(assembler, slot, binding()); \
    slot.storage.clear(); \
    check(!Access::prepare(assembler, std::move(token)), "lost prefix extent rejects"); \
    ++tested; \
  }
#define DXMT9_CHECK_PREFIX_SemanticOnly(plan, storage)
#define DXMT9_CHECK_PREFIX(region, plan, storage, element, physical, provision, allocation, lookup, owner) \
  DXMT9_CHECK_PREFIX_##physical(plan, storage)
  DXMT9_DIRECT_CHUNK_SLOT_DIMENSIONS(DXMT9_CHECK_PREFIX)
#undef DXMT9_CHECK_PREFIX
#undef DXMT9_CHECK_PREFIX_Vector
#undef DXMT9_CHECK_PREFIX_SemanticOnly
  check(tested == 31, "every schema-owned physical dimension tested");
}

// Validator-dependent fields, not struct padding or mutable lookup links.
auto prefixFields(const ChunkSlot& slot, std::size_t commands,
                  std::size_t runs, std::size_t params, std::size_t uniforms) {
  std::vector<std::uint64_t> result;
  auto handle = [&](DrawUniformHandle h) {
    result.insert(result.end(), {h.index, h.generation, h.hash});
  };
  for (std::size_t i = 0; i != commands; ++i) {
    const auto& h = slot.commandHeaders[i];
    result.insert(result.end(), {static_cast<std::uint64_t>(h.kind), h.payloadIndex.value});
  }
  for (std::size_t i = 0; i != runs; ++i) {
    const auto& r = slot.drawRunRecords[i];
    result.insert(result.end(), {r.stateIndex, r.firstParam, r.paramCount,
                                 r.payloadOffset, r.payloadSize});
    handle(r.uniformHandle);
  }
  for (std::size_t i = 0; i != params; ++i) {
    const auto& p = slot.drawParams[i];
    handle(p.uniformHandle);
    for (auto range : {p.userVertexRange, p.userIndexRange,
                       p.bindingOverrideRange, p.bindingSnapshotRange})
      result.insert(result.end(), {range.offset, range.size});
  }
  for (std::size_t i = 0; i != uniforms; ++i) handle(slot.drawUniformPayloads[i].handle);
  return result;
}

void actualAppendPreservesPrefix() {
  // All 2^4 combinations of three explicit cuts and a coordinator Clear.
  for (unsigned mask = 0; mask != 16; ++mask) {
    ChunkSlot slot;
    FlatDrawStateRecord hot{};
    DrawShaderLayoutContext layout{};
    DrawUniformPayload uniforms{};
    DirectReplayDrawInput input{.hot=&hot, .shaderLayout=&layout, .uniforms=&uniforms};
    {
      Assembler first(slot, capacity(2));
      check(first.tryAppendDirectDraw(input) && first.tryAppendDirectDraw(input),
            "build old prefix using actual intern/append");
      check(first.commitValueOnlyForTest(), "retain prefix");
    }
    const auto commands = slot.commandHeaders.size(), runs = slot.drawRunRecords.size();
    const auto params = slot.drawParams.size(), uniformCount = slot.drawUniformPayloads.size();
    const auto before = prefixFields(slot, commands, runs, params, uniformCount);
    Assembler second(slot, capacity(3));
    check(second.bindOuter(binding()) && !second.directRunOpen(), "new run begins closed");
    for (unsigned draw = 0; draw != 3; ++draw) {
      if (mask & (1u << draw)) second.closeDirectRun();
      if (draw == 1 && (mask & 8)) check(second.tryAppendClear({}), "coordinator cut");
      uniforms.vsConst.float4[0][0] = static_cast<float>(draw);
      check(second.tryAppendDirectDraw(input), "append actual draw");
      check(prefixFields(slot, commands, runs, params, uniformCount) == before,
            "append/intern never modifies validated prefix fields");
    }
    check(slot.commandPayloadsInRange(), "full reference before suffix prepare");
    check(Access::prepare(second, Access::mint(second, slot, binding())),
          "qualified suffix prepare matches full reference");
    second.rollback();
    check(prefixFields(slot, commands, runs, params, uniformCount) == before &&
          slot.commandHeaders.size() == commands && slot.drawRunRecords.size() == runs &&
          slot.drawParams.size() == params && slot.drawUniformPayloads.size() == uniformCount &&
          slot.commandPayloadsInRange(), "rollback restores actual prefix");
  }
}
}

int main() {
  gateTruthTable();
  suffixDifferential();
  identityAndTerminal();
  everyExtent();
  actualAppendPreservesPrefix();
  std::puts("direct-prefix-prepare: gate=16 suffix=64 identity=14 dimensions=31 append=16 move/terminal/ABA=pass");
}
