#include "../../../src/dxmt9/dxmt9_source_payload.hpp"

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace dxmt9::core {

struct ArenaSourcePayloadBlockTestAccess {
  static void makeClearRectGap(ArenaSourcePayloadBlock& block) {
    block.clearRects_.try_emplace_back(Rect{});
    block.clearRecords_[0].firstRect = 1;
  }
};

}  // namespace dxmt9::core

namespace {

using dxmt9::core::ArenaByteBuffer;
using dxmt9::core::ArenaSourcePayloadBlock;
using dxmt9::core::ArenaSourcePayloadBuilder;
using dxmt9::core::ArenaSourcePayloadAssembler;
using dxmt9::core::ArenaSoA;
using dxmt9::core::ChunkSlot;
using dxmt9::core::ClearDesc;
using dxmt9::core::DrawDebugSnapshot;
using dxmt9::core::DrawParam;
using dxmt9::core::DrawPsoSubview;
using dxmt9::core::DrawRunCommandRecord;
using dxmt9::core::DrawRunSubmission;
using dxmt9::core::DrawShaderLayoutContext;
using dxmt9::core::DrawUniformFixedPayloadRecord;
using dxmt9::core::DrawUniformPayloadRecord;
using dxmt9::core::DrawUniformPixelConstantsRecord;
using dxmt9::core::DrawUniformVertexConstantsRecord;
using dxmt9::core::FlatDrawStateRecord;
using dxmt9::core::MetalCommandHeader;
using dxmt9::core::MetalCommandKind;
using dxmt9::core::Rect;
using dxmt9::core::SourcePayloadView;
using dxmt9::core::SourcePayloadCapacity;
using dxmt9::core::SourcePayloadRegion;
using dxmt9::core::kSourcePayloadRegionCount;
using dxmt9::core::makeSourcePayloadLayout;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

struct alignas(16) AlignedValue {
  std::uint64_t first = 0;
  std::uint64_t second = 0;
};

struct DestructionProbe {
  int value = 0;
  std::array<int, 4>* order = nullptr;
  std::size_t* count = nullptr;

  DestructionProbe(int value,
                   std::array<int, 4>& order,
                   std::size_t& count) noexcept
      : value(value), order(&order), count(&count) {}

  ~DestructionProbe() {
    (*order)[(*count)++] = value;
  }
};

void testArenaSoABindAndExactCapacity() {
  static_assert(!std::is_copy_constructible_v<ArenaSoA<AlignedValue>>);
  static_assert(!std::is_move_constructible_v<ArenaSoA<AlignedValue>>);

  alignas(16) std::array<std::byte, sizeof(AlignedValue) * 2 + 1> storage{};
  ArenaSoA<AlignedValue> misaligned;
  check(!misaligned.bind(
            std::span<std::byte>(storage).subspan(1, sizeof(AlignedValue) * 2),
            2),
        "ArenaSoA must reject a misaligned external span");
  check(!misaligned.bound() && misaligned.size() == 0,
        "failed bind must not mutate ArenaSoA");

  ArenaSoA<AlignedValue> values;
  ArenaSoA<AlignedValue> wrongSize;
  check(!wrongSize.bind(
            std::span<std::byte>(storage).first(sizeof(AlignedValue)), 2),
        "ArenaSoA must reject storage that is smaller than exact capacity");
  check(values.bind(
            std::span<std::byte>(storage).first(sizeof(AlignedValue) * 2), 2),
        "ArenaSoA must bind an exactly sized aligned span");
  check(values.data() == reinterpret_cast<AlignedValue*>(storage.data()),
        "ArenaSoA must use the external span without fallback storage");
  check(values.try_emplace_back(AlignedValue{1, 2}),
        "first exact-capacity append must succeed");
  const std::array tooMany{AlignedValue{3, 4}, AlignedValue{5, 6}};
  check(!values.try_append(tooMany),
        "multi-element append past remaining capacity must fail");
  check(values.size() == 1 && values[0].first == 1 && values[0].second == 2,
        "failed ArenaSoA try_append must not mutate existing state");
  const std::array appended{tooMany[0]};
  check(values.try_append(appended), "second exact-capacity append must succeed");
  check(values.size() == 2 && values[1].second == 4,
        "ArenaSoA must publish appended elements");

  const auto beforeSize = values.size();
  const auto beforeFirst = values[0];
  check(!values.try_emplace_back(AlignedValue{5, 6}),
        "append past exact capacity must fail");
  check(values.size() == beforeSize && values[0].first == beforeFirst.first &&
            values[0].second == beforeFirst.second,
        "failed ArenaSoA append must not mutate existing state");
}

void testArenaSoAReverseDestruction() {
  alignas(DestructionProbe)
      std::array<std::byte, sizeof(DestructionProbe) * 3> storage{};
  std::array<int, 4> order{};
  std::size_t count = 0;
  ArenaSoA<DestructionProbe> values;
  check(values.bind(storage, 3), "destruction probe bind must succeed");
  check(values.try_emplace_back(1, order, count) &&
            values.try_emplace_back(2, order, count) &&
            values.try_emplace_back(3, order, count),
        "destruction probes must construct in the arena");
  values.destroyConstructed();
  check(count == 3 && order[0] == 3 && order[1] == 2 && order[2] == 1,
        "ArenaSoA must destroy constructed owners in reverse order");
  check(values.size() == 0,
        "explicit destruction must clear the constructed count");
}

void testArenaByteBufferAlignmentAndFailureAtomicity() {
  alignas(8) std::array<std::byte, 8> storage{};
  ArenaByteBuffer bytes;
  check(bytes.bind(storage, 8), "ArenaByteBuffer bind must succeed");
  const std::array first{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
  std::size_t offset = 99;
  check(bytes.try_append(first, 1, offset) && offset == 0,
        "first byte append must start at offset zero");
  const std::array second{std::byte{0x44}, std::byte{0x55}};
  check(bytes.try_append(second, 4, offset) && offset == 4 && bytes.size() == 6,
        "aligned byte append must publish the checked aligned offset");
  check(reinterpret_cast<std::uintptr_t>(bytes.data() + offset) % 4 == 0,
        "relative byte offset must produce an aligned absolute address");
  check(storage[3] == std::byte{0},
        "successful aligned append must initialize padding");

  const auto before = storage;
  const auto beforeSize = bytes.size();
  offset = 77;
  const std::array tooLarge{std::byte{1}, std::byte{2}, std::byte{3}};
  check(!bytes.try_append(tooLarge, 4, offset),
        "byte append past exact capacity must fail");
  check(bytes.size() == beforeSize && storage == before && offset == 77,
        "failed byte append must not mutate bytes, size, or output offset");
  check(!bytes.try_append({}, 3, offset),
        "non-power-of-two append alignment must fail");
  check(bytes.size() == beforeSize && offset == 77,
        "invalid alignment failure must not mutate state");
}

SourcePayloadCapacity everyRegionCapacity() {
  SourcePayloadCapacity c{};
  c.commandHeaders = 1;
  c.drawHotStates = 1;
  c.drawShaderLayouts = 1;
  c.drawDebugSnapshots = 1;
  c.drawPsoSubviews = 1;
  c.drawUniformFixedPayloads = 1;
  c.drawUniformVertexConstants = 1;
  c.drawUniformVertexConstantBytes = 1;
  c.drawUniformPixelConstants = 1;
  c.drawUniformPixelConstantBytes = 1;
  c.drawUniformPayloads = 1;
  c.drawUniformPayloadLookupHeads = 1;
  c.drawUniformPayloadLookupTails = 1;
  c.drawUniformPayloadLookupNext = 1;
  c.drawUniformVertexConstantsLookupHeads = 1;
  c.drawUniformVertexConstantsLookupTails = 1;
  c.drawUniformVertexConstantsLookupNext = 1;
  c.drawUniformPixelConstantsLookupHeads = 1;
  c.drawUniformPixelConstantsLookupTails = 1;
  c.drawUniformPixelConstantsLookupNext = 1;
  c.drawParams = 1;
  c.drawPayloadBytes = 1;
  c.drawRunRecords = 1;
  c.clearRecords = 1;
  c.clearRects = 1;
  c.surfaceCopyRecords = 1;
  c.stretchRectRecords = 1;
  c.readbackRecords = 1;
  c.colorFillRecords = 1;
  c.depthResolveRecords = 1;
  c.presentRecords = 1;
  return c;
}

void testTypedLayoutAlignmentNonOverlapAndPages() {
  static_assert(kSourcePayloadRegionCount == 31);
  const auto layout = makeSourcePayloadLayout(everyRegionCapacity(), 4096, 64);
  check(layout.has_value(), "31-region payload layout must build");
  std::size_t priorEnd = 0;
  for (std::size_t i = 0; i < kSourcePayloadRegionCount; ++i) {
    const auto id = static_cast<SourcePayloadRegion>(i);
    const auto& region = layout->region(id);
    check(static_cast<std::size_t>(region.id) == i,
          "layout region identity must match its typed index");
    check(region.elementCount == 1 && region.byteCount == region.elementSize,
          "every requested source payload region must be represented");
    check(region.alignment != 0 && region.offset % region.alignment == 0,
          "every payload region must be naturally aligned");
    check(region.offset >= priorEnd,
          "payload regions must not overlap");
    priorEnd = region.offset + region.byteCount;
  }
  check(layout->usedBytes == priorEnd,
        "layout used bytes must end at the final region");
  check(layout->pageCount == (layout->usedBytes + 4095) / 4096,
        "layout page count must be the checked total page count");
  check(layout->maximumAlignment >= alignof(std::uint64_t),
        "layout must publish its maximum alignment");
}

void testLayoutOverflowAndPageLimit() {
  auto capacity = SourcePayloadCapacity{};
  capacity.commandHeaders = std::numeric_limits<std::size_t>::max();
  check(!makeSourcePayloadLayout(capacity, 4096, 64),
        "element byte-count overflow must reject the complete layout");
  check(!makeSourcePayloadLayout(everyRegionCapacity(), 1, 1),
        "page-limit overflow must reject the complete layout");
  check(!makeSourcePayloadLayout(everyRegionCapacity(), 0, 64),
        "zero page size must reject the complete layout");
  capacity = {};
  capacity.drawPayloadBytes =
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1;
  check(!makeSourcePayloadLayout(capacity, 4096, 64),
        "UINT32_MAX+1 draw bytes must reject the complete layout");
  capacity = {};
  capacity.drawParams =
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1;
  check(!makeSourcePayloadLayout(capacity, 4096, 64),
        "UINT32_MAX+1 indexed elements must reject the complete layout");
  capacity = {};
  capacity.drawHotStates = std::numeric_limits<std::uint32_t>::max();
  check(!makeSourcePayloadLayout(
            capacity, dxmt9::core::kSourcePayloadByteAlignment),
        "UINT32_MAX+1 total pages must reject even without a caller limit");
  check(!makeSourcePayloadLayout(everyRegionCapacity(),
                                 dxmt9::core::kSourcePayloadByteAlignment + 1,
                                 64),
        "page size must preserve every first-page base alignment");
}

std::span<std::byte> alignedBacking(
    std::vector<std::max_align_t>& backing,
    std::size_t byteCount) {
  backing.resize((byteCount + sizeof(std::max_align_t) - 1) /
                 sizeof(std::max_align_t));
  return {reinterpret_cast<std::byte*>(backing.data()),
          backing.size() * sizeof(std::max_align_t)};
}

SourcePayloadCapacity parityCapacity() {
  SourcePayloadCapacity c{};
  c.commandHeaders = 2;
  c.drawHotStates = 1;
  c.drawShaderLayouts = 1;
  c.drawDebugSnapshots = 1;
  c.drawPsoSubviews = 1;
  c.drawUniformFixedPayloads = 1;
  c.drawUniformVertexConstants = 1;
  c.drawUniformVertexConstantBytes = 4;
  c.drawUniformPixelConstants = 1;
  c.drawUniformPixelConstantBytes = 4;
  c.drawUniformPayloads = 1;
  c.drawParams = 1;
  c.drawPayloadBytes = 8;
  c.drawRunRecords = 1;
  c.clearRecords = 1;
  c.clearRects = 2;
  return c;
}

DrawShaderLayoutContext shaderLayoutWithOwner() {
  DrawShaderLayoutContext layout{};
  layout.vertexShader.kind = dxmt9::core::ShaderRef::Kind::Bytecode;
  layout.vertexShader.bytecode.bytes = std::vector<dxmt9::core::u8>{
      1, 2, 3, 4,
  };
  layout.vertexShader.bytecode.hash = 0x1234;
  return layout;
}

void testArenaBlockAlignmentAndStickyFailure() {
  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 1;
  capacity.drawShaderLayouts = 1;
  capacity.surfaceCopyRecords = 2;
  const auto layout = makeSourcePayloadLayout(capacity, 4096, 4);
  check(layout.has_value(), "sticky-failure block layout must build");

  std::vector<std::max_align_t> backing;
  const auto memory = alignedBacking(
      backing, std::max(layout->usedBytes + 1, std::size_t{4 * 4096}));
  check(reinterpret_cast<std::uintptr_t>(memory.data()) %
                layout->requiredBaseAlignment ==
            0,
        "test backing must meet the explicit block base contract");
  check(reinterpret_cast<std::uintptr_t>(memory.data() + 3 * 4096) %
                layout->requiredBaseAlignment ==
            0,
        "an arbitrary firstPage address must preserve layout alignment");

  ArenaSourcePayloadBlock misalignedBlock;
  ArenaSourcePayloadBuilder misalignedBuilder(
      misalignedBlock, *layout, memory.subspan(1));
  check(misalignedBuilder.failed() && !misalignedBlock.bound(),
        "block bind must reject a misaligned absolute backing address");

  ArenaSourcePayloadBlock block;
  ArenaSourcePayloadBuilder builder(block, *layout, memory);
  check(builder.good() && block.bound(), "aligned block bind must succeed");
  auto owner = shaderLayoutWithOwner();
  const auto* ownedBytes = owner.vertexShader.bytecode.bytes.data();
  check(builder.tryAppendDrawShaderLayout(std::move(owner)),
        "DrawShaderLayoutContext must placement-move into its final region");
  check(owner.vertexShader.bytecode.bytes.empty(),
        "DrawShaderLayoutContext append must move rather than copy its owner");
  check(builder.tryAppendSurfaceCopyCommand({}),
        "first command record/header append must succeed");
  check(!builder.tryAppendSurfaceCopyCommand({}),
        "header exhaustion must fail before a second command publication");
  check(builder.failed(), "first builder failure must remain sticky");
  check(!builder.tryAppendDrawShaderLayout(shaderLayoutWithOwner()),
        "sticky failure must reject all later appends");
  check(!builder.publish() && !block.published(),
        "a failed builder must never publish a partial command stream");
  check(!SourcePayloadView(block).valid(),
        "SourcePayloadView must reject an unpublished arena block");
  check(ownedBytes != nullptr,
        "the placement-moved shader owner must exist until explicit abort");
  block.destroyConstructed();
}

void testBlockBindPreflightIsFailureAtomic() {
  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 1;
  capacity.presentRecords = 1;
  const auto layout = makeSourcePayloadLayout(capacity, 4096, 2);
  check(layout.has_value(), "preflight fixture layout must build");
  std::vector<std::max_align_t> backing;
  const auto memory = alignedBacking(backing, layout->usedBytes);

  auto malformed = *layout;
  malformed.regions.back().elementSize += 1;
  ArenaSourcePayloadBlock block;
  ArenaSourcePayloadBuilder rejected(block, malformed, memory);
  check(rejected.failed() && !block.bound(),
        "a malformed final region must fail before any block mutation");

  ArenaSourcePayloadBuilder accepted(block, *layout, memory);
  dxmt9::core::PresentCommandRecord present{};
  check(accepted.good() &&
            accepted.tryAppendPresentCommand(std::move(present)) &&
            accepted.publish(),
        "the same pristine block must bind after late-region preflight failure");
  block.destroyConstructed();
}

void testEmptyAndNonBijectiveStreamsRejectPublish() {
  {
    const auto layout = makeSourcePayloadLayout({}, 4096, 1);
    check(layout.has_value(), "zero-capacity layout must build");
    ArenaSourcePayloadBlock block;
    ArenaSourcePayloadBuilder builder(block, *layout, {});
    check(builder.good() && !builder.publish() && builder.failed() &&
              !block.published() && !SourcePayloadView(block).valid(),
          "an empty arena stream must not publish or expose a view");
    block.destroyConstructed();
  }

  {
    SourcePayloadCapacity capacity{};
    capacity.commandHeaders = 2;
    capacity.presentRecords = 1;
    const auto layout = makeSourcePayloadLayout(capacity, 4096, 1);
    check(layout.has_value(), "duplicate-header layout must build");
    std::vector<std::max_align_t> backing;
    const auto memory = alignedBacking(backing, layout->usedBytes);
    ArenaSourcePayloadBlock block;
    ArenaSourcePayloadBuilder builder(block, *layout, memory);
    dxmt9::core::PresentCommandRecord present{};
    check(builder.tryAppendPresentCommand(std::move(present)) &&
              builder.tryAppendCommand(MetalCommandKind::Present, 0) &&
              !builder.publish(),
          "duplicate headers for one typed record must not publish");
    block.destroyConstructed();
  }

  {
    SourcePayloadCapacity capacity{};
    capacity.commandHeaders = 1;
    capacity.surfaceCopyRecords = 1;
    capacity.drawRunRecords = 1;
    const auto layout = makeSourcePayloadLayout(capacity, 4096, 1);
    check(layout.has_value(), "orphan-record layout must build");
    std::vector<std::max_align_t> backing;
    const auto memory = alignedBacking(backing, layout->usedBytes);
    ArenaSourcePayloadBlock block;
    ArenaSourcePayloadBuilder builder(block, *layout, memory);
    check(builder.tryAppendSurfaceCopyCommand({}) &&
              builder.tryAppendDrawRun({}) && !builder.publish(),
          "a typed record without a matching command must not publish");
    block.destroyConstructed();
  }

  {
    SourcePayloadCapacity capacity{};
    capacity.commandHeaders = 1;
    capacity.clearRecords = 1;
    capacity.clearRects = 2;
    const auto layout = makeSourcePayloadLayout(capacity, 4096, 1);
    check(layout.has_value(), "clear-gap layout must build");
    std::vector<std::max_align_t> backing;
    const auto memory = alignedBacking(backing, layout->usedBytes);
    ArenaSourcePayloadBlock block;
    ArenaSourcePayloadBuilder builder(block, *layout, memory);
    ClearDesc clear{};
    clear.rects.push_back({});
    check(builder.tryAppendClearCommand(clear),
          "clear-gap fixture command must append");
    dxmt9::core::ArenaSourcePayloadBlockTestAccess::makeClearRectGap(block);
    check(!builder.publish(),
          "clear records must consume the flattened rect SoA contiguously");
    block.destroyConstructed();
  }
}

void testLegacyArenaSourcePayloadViewParity() {
  const auto capacity = parityCapacity();
  const auto layout = makeSourcePayloadLayout(capacity, 4096, 8);
  check(layout.has_value(), "source view parity layout must build");
  std::vector<std::max_align_t> backing;
  const auto memory = alignedBacking(backing, layout->usedBytes);

  FlatDrawStateRecord hot{};
  hot.streamMask = 3;
  DrawDebugSnapshot debug{};
  debug.primitiveCount = 7;
  DrawPsoSubview pso{};
  pso.renderStateHash = 0x55;
  auto legacyShaderLayout = shaderLayoutWithOwner();
  auto arenaShaderLayout = legacyShaderLayout;
  const auto* shaderOwnerBytes = arenaShaderLayout.vertexShader.bytecode.bytes.data();
  DrawUniformFixedPayloadRecord fixed{};
  DrawUniformVertexConstantsRecord vertexConstants{};
  DrawUniformPixelConstantsRecord pixelConstants{};
  DrawUniformPayloadRecord uniform{};
  DrawParam param{};
  param.primitiveCount = 7;
  param.userVertexRange = {.offset = 0, .size = 2};
  const std::array<dxmt9::core::u8, 4> payload{10, 20, 30, 40};
  const std::array<dxmt9::core::u8, 2> vertexBytes{50, 60};
  const std::array<dxmt9::core::u8, 1> pixelBytes{70};
  DrawRunCommandRecord run{
      .stateIndex = 0,
      .firstParam = 0,
      .paramCount = 1,
      .payloadOffset = 0,
      .payloadSize = static_cast<std::uint32_t>(payload.size()),
  };
  ClearDesc clear{};
  clear.clearColor = true;
  clear.clearDepth = true;
  clear.color = {.r = 0.25f, .g = 0.5f, .b = 0.75f, .a = 1.0f};
  clear.depth = 0.5f;
  clear.rects = {
      Rect{.left = 1, .top = 2, .right = 3, .bottom = 4},
      Rect{.left = 5, .top = 6, .right = 7, .bottom = 8},
  };

  ChunkSlot legacy;
  legacy.commandHeaders.push_back(MetalCommandHeader{
      .kind = MetalCommandKind::DrawRun,
      .payloadIndex = dxmt9::core::CommandPayloadIndex::fromU32(0),
  });
  legacy.drawHotStates.push_back(hot);
  legacy.drawShaderLayouts.push_back(legacyShaderLayout);
  legacy.drawDebugSnapshots.push_back(debug);
  legacy.drawPsoSubviews.push_back(pso);
  legacy.drawUniformFixedPayloads.push_back(fixed);
  legacy.drawUniformVertexConstants.push_back(vertexConstants);
  legacy.drawUniformVertexConstantBytes.assign(vertexBytes.begin(),
                                                vertexBytes.end());
  legacy.drawUniformPixelConstants.push_back(pixelConstants);
  legacy.drawUniformPixelConstantBytes.assign(pixelBytes.begin(),
                                               pixelBytes.end());
  legacy.drawUniformPayloads.push_back(uniform);
  legacy.drawParams.push_back(param);
  legacy.drawPayloadArena.assign(payload.begin(), payload.end());
  legacy.drawRunRecords.push_back(run);
  legacy.appendClear(clear);

  ArenaSourcePayloadBlock arena;
  ArenaSourcePayloadBuilder builder(arena, *layout, memory);
  std::size_t vertexOffset = 99;
  std::size_t pixelOffset = 99;
  std::size_t payloadOffset = 99;
  check(builder.good() &&
            builder.tryAppendDrawHotState(hot) &&
            builder.tryAppendDrawShaderLayout(std::move(arenaShaderLayout)) &&
            builder.tryAppendDrawDebugSnapshot(debug) &&
            builder.tryAppendDrawPsoSubview(pso) &&
            builder.tryAppendDrawUniformFixedPayload(fixed) &&
            builder.tryAppendDrawUniformVertexConstants(vertexConstants) &&
            builder.tryAppendVertexConstantBytes(vertexBytes, 4,
                                                 vertexOffset) &&
            builder.tryAppendDrawUniformPixelConstants(pixelConstants) &&
            builder.tryAppendPixelConstantBytes(pixelBytes, 4,
                                                pixelOffset) &&
            builder.tryAppendDrawUniformPayload(uniform) &&
            builder.tryAppendDrawParam(param) &&
            builder.tryAppendDrawPayloadBytes(payload, 8, payloadOffset) &&
            builder.tryAppendDrawRun(run) &&
            builder.tryAppendCommand(MetalCommandKind::DrawRun, 0) &&
            builder.tryAppendClearCommand(clear),
        "arena draw/uniform/payload/clear construction must succeed");
  check(vertexOffset == 0 && pixelOffset == 0 && payloadOffset == 0,
        "first byte payloads must use their aligned region bases");
  check(builder.publish() && arena.published(),
        "validated arena command stream must publish");
  check(arena.actualCount(SourcePayloadRegion::CommandHeaders) == 2 &&
            arena.actualCount(SourcePayloadRegion::ClearRects) == 2,
        "publish must freeze immutable actual counts");

  const SourcePayloadView legacyView(legacy);
  const SourcePayloadView arenaView(arena);
  check(legacyView.valid() && arenaView.valid() &&
            legacyView.commandCount() == arenaView.commandCount(),
        "legacy and arena views must expose the same command count");
  const auto legacyDraw = legacyView.commandAt(0);
  const auto arenaDraw = arenaView.commandAt(0);
  check(legacyDraw.kind() == arenaDraw.kind() &&
            legacyDraw.command.drawRunRecord->paramCount ==
                arenaDraw.command.drawRunRecord->paramCount &&
            legacyDraw.command.drawState.hot->streamMask ==
                arenaDraw.command.drawState.hot->streamMask &&
            legacyDraw.command.drawState.debug->primitiveCount ==
                arenaDraw.command.drawState.debug->primitiveCount &&
            legacyDraw.command.drawParams.size() ==
                arenaDraw.command.drawParams.size() &&
            legacyDraw.command.drawParams[0].primitiveCount ==
                arenaDraw.command.drawParams[0].primitiveCount,
        "draw commandAt must preserve run/state/param access");
  check(legacyView.drawUniformPayloads().size() ==
                arenaView.drawUniformPayloads().size() &&
            std::equal(legacyView.drawUniformVertexConstantBytes().begin(),
                       legacyView.drawUniformVertexConstantBytes().end(),
                       arenaView.drawUniformVertexConstantBytes().begin(),
                       arenaView.drawUniformVertexConstantBytes().end()) &&
            std::equal(legacyView.drawUniformPixelConstantBytes().begin(),
                       legacyView.drawUniformPixelConstantBytes().end(),
                       arenaView.drawUniformPixelConstantBytes().begin(),
                       arenaView.drawUniformPixelConstantBytes().end()) &&
            std::equal(legacyView.drawPayloadBytes().begin(),
                       legacyView.drawPayloadBytes().end(),
                       arenaView.drawPayloadBytes().begin(),
                       arenaView.drawPayloadBytes().end()) &&
            std::equal(legacyDraw.command.drawPayloadBytes.begin(),
                       legacyDraw.command.drawPayloadBytes.end(),
                       arenaDraw.command.drawPayloadBytes.begin(),
                       arenaDraw.command.drawPayloadBytes.end()),
        "uniform and draw payload spans must match across source views");
  check(reinterpret_cast<std::uintptr_t>(arenaView.drawPayloadBytes().data()) %
                dxmt9::core::kSourcePayloadByteAlignment ==
            0,
        "block-bound byte region base must be absolutely aligned");
  check(arenaDraw.command.drawState.shaderLayout->vertexShader.bytecode.bytes.data() ==
            shaderOwnerBytes,
        "DrawShaderLayoutContext must retain its placement-moved owner");

  const auto legacyClear = legacyView.commandAt(1);
  const auto arenaClear = arenaView.commandAt(1);
  check(legacyClear.kind() == MetalCommandKind::Clear &&
            arenaClear.kind() == MetalCommandKind::Clear &&
            legacyClear.clear.has_value() && arenaClear.clear.has_value(),
        "clear commandAt must expose span-based clear views");
  check(legacyClear.clear->colorAttachments ==
                arenaClear.clear->colorAttachments &&
            legacyClear.clear->depthStencil == arenaClear.clear->depthStencil &&
            legacyClear.clear->clearColor == arenaClear.clear->clearColor &&
            legacyClear.clear->clearDepth == arenaClear.clear->clearDepth &&
            legacyClear.clear->color == arenaClear.clear->color &&
            legacyClear.clear->depth == arenaClear.clear->depth &&
            legacyClear.clear->rects.size() == arenaClear.clear->rects.size() &&
            legacyClear.clear->rects[0] == arenaClear.clear->rects[0] &&
            legacyClear.clear->rects[1] == arenaClear.clear->rects[1],
        "flattened clear records and rect spans must match legacy ClearDesc");

  arena.destroyConstructed();
  check(arena.actualCount(SourcePayloadRegion::DrawShaderLayouts) == 1,
        "explicit out-of-lock destruction must not rewrite published counts");
  check(!arena.published() && !SourcePayloadView(arena).valid(),
        "destroyed arena storage must no longer produce a borrowed view");
}

void testConsolidatedNondrawCommandParity() {
  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 6;
  capacity.surfaceCopyRecords = 1;
  capacity.stretchRectRecords = 1;
  capacity.readbackRecords = 1;
  capacity.colorFillRecords = 1;
  capacity.depthResolveRecords = 1;
  capacity.presentRecords = 1;
  const auto layout = makeSourcePayloadLayout(capacity, 4096, 4);
  check(layout.has_value(), "consolidated nondraw layout must build");
  std::vector<std::max_align_t> backing;
  const auto memory = alignedBacking(backing, layout->usedBytes);

  const dxmt9::core::SurfaceCopyDesc copy{
      .source = {.value = 11},
      .destination = {.value = 12},
      .linear = true,
  };
  const dxmt9::core::StretchRectDesc stretch{
      .source = {.value = 21},
      .destination = {.value = 22},
      .linear = true,
  };
  const dxmt9::core::ReadbackDesc readback{
      .source = {.value = 31},
      .destination = {.value = 32},
      .sourceLevel = 2,
  };
  const dxmt9::core::ColorFillDesc fill{
      .destination = {.value = 41},
      .hasRect = true,
      .color = {.r = 1.0f, .g = 0.5f, .b = 0.25f, .a = 1.0f},
  };
  const dxmt9::core::DepthResolveDesc resolve{
      .msaaDepth = {.value = 51},
      .intzDest = {.value = 52},
  };
  dxmt9::core::PresentCommandRecord present{
      .present = {.sourceSurface = {.value = 61}},
      .presentSource = {.value = 62},
  };

  ChunkSlot legacy;
  legacy.appendSurfaceCopy(copy);
  legacy.appendStretchRect(stretch);
  legacy.appendReadback(readback);
  legacy.appendColorFill(fill);
  legacy.appendDepthResolve(resolve);
  legacy.appendPresent(present.present, present.presentSource);

  ArenaSourcePayloadBlock arena;
  ArenaSourcePayloadBuilder builder(arena, *layout, memory);
  check(builder.tryAppendSurfaceCopyCommand(copy) &&
            builder.tryAppendStretchRectCommand(stretch) &&
            builder.tryAppendReadbackCommand(readback) &&
            builder.tryAppendColorFillCommand(fill) &&
            builder.tryAppendDepthResolveCommand(resolve) &&
            builder.tryAppendPresentCommand(std::move(present)) &&
            builder.publish(),
        "all nondraw command records must append and publish together");

  const SourcePayloadView legacyView(legacy);
  const SourcePayloadView arenaView(arena);
  check(legacyView.commandCount() == 6 && arenaView.commandCount() == 6,
        "consolidated nondraw views must expose all commands");
  const std::array kinds{
      MetalCommandKind::SurfaceCopy,
      MetalCommandKind::StretchRect,
      MetalCommandKind::Readback,
      MetalCommandKind::ColorFill,
      MetalCommandKind::DepthResolve,
      MetalCommandKind::Present,
  };
  for (std::size_t i = 0; i < kinds.size(); ++i) {
    check(legacyView.commandAt(i).kind() == kinds[i] &&
              arenaView.commandAt(i).kind() == kinds[i],
          "nondraw commandAt kind must match legacy order");
  }
  const auto legacyCopy = legacyView.commandAt(0).command.surfaceCopy;
  const auto arenaCopy = arenaView.commandAt(0).command.surfaceCopy;
  const auto legacyStretch = legacyView.commandAt(1).command.stretchRect;
  const auto arenaStretch = arenaView.commandAt(1).command.stretchRect;
  const auto legacyReadback = legacyView.commandAt(2).command.readback;
  const auto arenaReadback = arenaView.commandAt(2).command.readback;
  const auto legacyFill = legacyView.commandAt(3).command.colorFill;
  const auto arenaFill = arenaView.commandAt(3).command.colorFill;
  const auto legacyResolve = legacyView.commandAt(4).command.depthResolve;
  const auto arenaResolve = arenaView.commandAt(4).command.depthResolve;
  const auto legacyPresent = legacyView.commandAt(5).command.present;
  const auto arenaPresent = arenaView.commandAt(5).command.present;
  check(legacyCopy && arenaCopy &&
            legacyCopy->source == arenaCopy->source &&
            legacyCopy->destination == arenaCopy->destination &&
            legacyStretch && arenaStretch &&
            legacyStretch->source == arenaStretch->source &&
            legacyStretch->destination == arenaStretch->destination &&
            legacyReadback && arenaReadback &&
            legacyReadback->source == arenaReadback->source &&
            legacyReadback->destination == arenaReadback->destination &&
            legacyFill && arenaFill &&
            legacyFill->destination == arenaFill->destination &&
            legacyFill->color == arenaFill->color &&
            legacyResolve && arenaResolve &&
            legacyResolve->msaaDepth == arenaResolve->msaaDepth &&
            legacyResolve->intzDest == arenaResolve->intzDest &&
            legacyPresent && arenaPresent &&
            legacyPresent->presentSource == arenaPresent->presentSource &&
            legacyPresent->present.sourceSurface ==
                arenaPresent->present.sourceSurface,
        "all nondraw typed command pointers must match legacy payloads");
  arena.destroyConstructed();
}

void testReplayAssemblerConsumesScratchIntoFinalArena() {
  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 6;
  capacity.drawHotStates = 1;
  capacity.drawShaderLayouts = 1;
  capacity.drawDebugSnapshots = 1;
  capacity.drawPsoSubviews = 1;
  capacity.drawUniformFixedPayloads = 1;
  capacity.drawUniformVertexConstants = 1;
  capacity.drawUniformVertexConstantBytes =
      sizeof(dxmt9::core::VertexShaderConstants);
  capacity.drawUniformPixelConstants = 1;
  capacity.drawUniformPixelConstantBytes =
      sizeof(dxmt9::core::PixelShaderConstants);
  capacity.drawUniformPayloads = 1;
  capacity.drawParams = 1;
  capacity.drawPayloadBytes = 64;
  capacity.drawRunRecords = 1;
  capacity.clearRecords = 1;
  capacity.clearRects = 1;
  capacity.surfaceCopyRecords = 1;
  capacity.stretchRectRecords = 1;
  capacity.colorFillRecords = 1;
  capacity.depthResolveRecords = 1;
  const auto layout = makeSourcePayloadLayout(capacity, 4096, 8);
  check(layout.has_value(), "assembler fixture layout must build");
  std::vector<std::max_align_t> backing;
  const auto memory = alignedBacking(backing, layout->usedBytes);

  std::array<dxmt9::core::u8, 3> vertexBytes{1, 2, 3};
  std::array<dxmt9::core::u8, 2> indexBytes{4, 5};
  DrawRunSubmission submission{
      .state = dxmt9::core::CanonicalDrawState{},
      .uniforms = dxmt9::core::DrawUniformPayload{},
      .draw = DrawParam{},
      .payload = {
          .userVertexData = vertexBytes,
          .userIndexData = indexBytes,
      },
      .uniformGeneration = 1,
  };
  ClearDesc clear{};
  clear.rects.push_back(Rect{.left = 1, .top = 2, .right = 3, .bottom = 4});

  ArenaSourcePayloadBlock block;
  ArenaSourcePayloadBuilder builder(block, *layout, memory);
  ArenaSourcePayloadAssembler assembler(builder);
  check(assembler.tryAppendDrawRunBatch(
            std::span<DrawRunSubmission>(&submission, 1)) &&
            assembler.tryAppendClear(clear) &&
            assembler.tryAppendSurfaceCopy({}) &&
            assembler.tryAppendStretchRect({}) &&
            assembler.tryAppendColorFill({}) &&
            assembler.tryAppendDepthResolve({}) &&
            assembler.commandCount() == 6 && builder.publish(),
        "replay assembler moves draw and eligible nondraw work into final SoAs");

  const SourcePayloadView view(block);
  check(view.valid() && view.commandCount() == 6 &&
            view.commandAt(0).kind() == MetalCommandKind::DrawRun &&
            view.commandAt(1).kind() == MetalCommandKind::Clear &&
            view.commandAt(2).kind() == MetalCommandKind::SurfaceCopy &&
            view.commandAt(3).kind() == MetalCommandKind::StretchRect &&
            view.commandAt(4).kind() == MetalCommandKind::ColorFill &&
            view.commandAt(5).kind() == MetalCommandKind::DepthResolve,
        "assembler preserves replay command order and typed kinds");
  const auto draw = view.commandAt(0).command;
  check(draw.drawRunRecord && draw.drawParams.size() == 1 &&
            draw.drawParams[0].uniformHandle.valid() &&
            draw.drawParams[0].userVertexRange.size == vertexBytes.size() &&
            draw.drawParams[0].userIndexRange.size == indexBytes.size() &&
            view.drawUniformPayloads().size() == 1,
        "assembler publishes draw state, uniform, param, and payload locators");
  check(view.commandAt(1).clear.has_value() &&
            view.commandAt(1).clear->rects.size() == 1 &&
            view.commandAt(1).clear->rects[0] == clear.rects[0],
        "assembler routes flattened Clear rects through the arena view");
  block.destroyConstructed();
}

void testPublishRejectsInvalidCommandRanges() {
  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 1;
  capacity.drawRunRecords = 1;
  const auto layout = makeSourcePayloadLayout(capacity, 4096, 2);
  check(layout.has_value(), "invalid-range fixture layout must build");
  std::vector<std::max_align_t> backing;
  const auto memory = alignedBacking(backing, layout->usedBytes);
  ArenaSourcePayloadBlock block;
  ArenaSourcePayloadBuilder builder(block, *layout, memory);
  check(builder.tryAppendDrawRun(DrawRunCommandRecord{
            .stateIndex = 9,
            .firstParam = 0,
            .paramCount = 1,
        }) &&
            builder.tryAppendCommand(MetalCommandKind::DrawRun, 0),
        "invalid draw ranges may be constructed while Writing");
  check(!builder.publish() && builder.failed() && !block.published(),
        "publish must reject invalid draw state/param/pso ranges");
  check(!SourcePayloadView(block).valid(),
        "failed publish must keep arena SourcePayloadView invalid");
  block.destroyConstructed();
}

}  // namespace

int main() {
  try {
    testArenaSoABindAndExactCapacity();
    testArenaSoAReverseDestruction();
    testArenaByteBufferAlignmentAndFailureAtomicity();
    testTypedLayoutAlignmentNonOverlapAndPages();
    testLayoutOverflowAndPageLimit();
    testArenaBlockAlignmentAndStickyFailure();
    testBlockBindPreflightIsFailureAtomic();
    testEmptyAndNonBijectiveStreamsRejectPublish();
    testLegacyArenaSourcePayloadViewParity();
    testConsolidatedNondrawCommandParity();
    testReplayAssemblerConsumesScratchIntoFinalArena();
    testPublishRejectsInvalidCommandRanges();
  } catch (const std::exception& error) {
    std::cerr << "source_payload_spec: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
