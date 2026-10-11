#include "device_c_render_tape_first_access_locator.hpp"
#include "dxmt9/device_c.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace dxmt9::d3d9;

struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition)
    throw Failure(std::string(message));
}

struct SectionSpec {
  std::uint16_t kind = 0u;
  std::uint32_t handleIndex = 0u;
  std::uint32_t slot = 0u;
};

struct Fixture {
  D9CCommandChunkWireRecordHeader record{};
  std::vector<D9CCommandChunkWireHandleEntry> handles{};
  std::vector<std::byte> payload{};
  std::vector<D9CCommandChunkWireSectionDesc> sections{};

  ImportedChunkView view() const noexcept {
    return ImportedChunkView{
        .records = std::span<const D9CCommandChunkWireRecordHeader>(&record, 1u),
        .handles = handles,
        .payloadArena = payload,
    };
  }
};

D9CWireObjectIdentity identity(std::uint32_t kind, std::uint32_t generation,
                               std::uint64_t objectId) {
  return {kind, generation, objectId};
}

Fixture sparse(std::uint32_t recordType, std::span<const SectionSpec> specs,
               D9CWireObjectIdentity handleIdentity) {
  Fixture fixture;
  fixture.handles.push_back({handleIdentity.kind, handleIdentity.generation,
                             handleIdentity.objectId});
  D9CCommandChunkWireDrawHeader draw{};
  draw.sectionCount = static_cast<std::uint32_t>(specs.size());
  draw.sectionTableOffset = sizeof(draw);
  draw.sectionPayloadOffset =
      draw.sectionTableOffset + specs.size() * sizeof(fixture.sections[0]);
  fixture.sections.reserve(specs.size());
  std::size_t payloadOffset = draw.sectionPayloadOffset;
  for (const auto& spec : specs) {
    const auto elementSize = static_cast<std::uint16_t>(
        spec.kind == D9C_COMMAND_CHUNK_SECTION_TEXTURE
            ? sizeof(D9CCommandChunkWireTextureBinding)
            : spec.kind == D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET
                  ? sizeof(D9CCommandChunkWireRenderTargetBinding)
                  : sizeof(D9CCommandChunkWireDepthStencilBinding));
    fixture.sections.push_back({spec.kind, elementSize, 1u,
                                static_cast<std::uint32_t>(payloadOffset),
                                elementSize});
    payloadOffset += elementSize;
  }
  fixture.payload.resize(payloadOffset);
  std::memcpy(fixture.payload.data(), &draw, sizeof(draw));
  if (!fixture.sections.empty()) {
    std::memcpy(fixture.payload.data() + draw.sectionTableOffset,
                fixture.sections.data(),
                fixture.sections.size() * sizeof(fixture.sections[0]));
  }
  payloadOffset = draw.sectionPayloadOffset;
  for (const auto& spec : specs) {
    if (spec.kind == D9C_COMMAND_CHUNK_SECTION_TEXTURE) {
      const D9CCommandChunkWireTextureBinding binding{
          spec.slot, 1u, spec.handleIndex, 0u};
      std::memcpy(fixture.payload.data() + payloadOffset, &binding,
                  sizeof(binding));
      payloadOffset += sizeof(binding);
    } else if (spec.kind == D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET) {
      const D9CCommandChunkWireRenderTargetBinding binding{
          spec.slot, 1u, spec.handleIndex, 0u};
      std::memcpy(fixture.payload.data() + payloadOffset, &binding,
                  sizeof(binding));
      payloadOffset += sizeof(binding);
    } else {
      const D9CCommandChunkWireDepthStencilBinding binding{1u, spec.handleIndex};
      std::memcpy(fixture.payload.data() + payloadOffset, &binding,
                  sizeof(binding));
      payloadOffset += sizeof(binding);
    }
  }
  std::memcpy(fixture.payload.data(), &draw, sizeof(draw));
  fixture.record = {
      .type = recordType,
      .payloadOffset = 0u,
      .payloadSize = static_cast<std::uint32_t>(fixture.payload.size()),
      .firstHandle = 0u,
      .handleCount = static_cast<std::uint32_t>(fixture.handles.size()),
  };
  return fixture;
}

Fixture fixed(std::uint32_t recordType, const void* value, std::size_t bytes,
              D9CWireObjectIdentity handleIdentity = {}) {
  Fixture fixture;
  fixture.handles.push_back({handleIdentity.kind, handleIdentity.generation,
                             handleIdentity.objectId});
  fixture.payload.resize(bytes);
  std::memcpy(fixture.payload.data(), value, bytes);
  fixture.record = {
      .type = recordType,
      .payloadOffset = 0u,
      .payloadSize = static_cast<std::uint32_t>(bytes),
      .firstHandle = 0u,
      .handleCount = handleIdentity.objectId == 0u ? 0u : 1u,
  };
  return fixture;
}

Fixture clearWithRects(const D9CCommandChunkWireClear& header,
                       std::span<const D9CRect> rects) {
  Fixture fixture;
  fixture.payload.resize(sizeof(header) + rects.size_bytes());
  std::memcpy(fixture.payload.data(), &header, sizeof(header));
  if (!rects.empty()) {
    std::memcpy(fixture.payload.data() + sizeof(header), rects.data(),
                rects.size_bytes());
  }
  fixture.record = {
      .type = D9C_COMMAND_RECORD_CLEAR,
      .payloadOffset = 0u,
      .payloadSize = static_cast<std::uint32_t>(fixture.payload.size()),
      .firstHandle = 0u,
      .handleCount = 0u,
  };
  return fixture;
}

void arm(RenderTapeFirstAccessLedger& ledger,
         const D9CWireObjectIdentity& origin,
         const D9CWireObjectIdentity& resolved) {
  renderTapeFirstAccessArm(ledger, origin, resolved);
}

void testCrossChunkDrawAndExactlyOnce() {
  const auto origin = identity(D9C_CHUNK_HANDLE_KIND_SURFACE, 28u, 7527u);
  const auto resolved = identity(D9C_CHUNK_HANDLE_KIND_TEXTURE, 14u, 7525u);
  RenderTapeFirstAccessLedger ledger{};
  arm(ledger, origin, resolved);
  const std::array<SectionSpec, 1> binding{{
      {D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET, 0u, 0u}}};
  auto apply = sparse(D9C_COMMAND_RECORD_APPLY_STATE, binding, origin);
  auto first = renderTapeFirstAccessObserve(ledger, apply.view());
  check(first.status == RenderTapeFirstAccessStatus::Observing &&
            first.classification == RenderTapeFirstAccessClass::BindingOnly,
        "APPLY_STATE binding is non-terminal");

  auto draw = sparse(D9C_COMMAND_RECORD_DRAW_PRIMITIVE, {}, origin);
  auto observation = renderTapeFirstAccessObserve(ledger, draw.view());
  check(observation.status == RenderTapeFirstAccessStatus::Terminal &&
            observation.classification ==
                RenderTapeFirstAccessClass::DrawWriteUnknownCoverage &&
            observation.aliasOrigin &&
            observation.originIdentity.objectId == 7527u &&
            observation.resolvedIdentity.objectId == 7525u,
        "carried RT binding reaches a sparse next-chunk Draw");
  check(renderTapeFirstAccessObserve(ledger, draw.view()).status ==
            RenderTapeFirstAccessStatus::Complete,
        "terminal first access is emitted exactly once");
}

void testReadWriteConflictAndClear() {
  const auto target = identity(D9C_CHUNK_HANDLE_KIND_TEXTURE, 14u, 7525u);
  RenderTapeFirstAccessLedger ledger{};
  arm(ledger, target, target);
  const std::array<SectionSpec, 1> binding{{
      {D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET, 0u, 0u}}};
  auto apply = sparse(D9C_COMMAND_RECORD_APPLY_STATE, binding, target);
  check(renderTapeFirstAccessObserve(ledger, apply.view()).status ==
            RenderTapeFirstAccessStatus::Observing,
        "RT binding arms carried state");
  const std::array<SectionSpec, 1> texture{{
      {D9C_COMMAND_CHUNK_SECTION_TEXTURE, 0u, 0u}}};
  auto draw = sparse(D9C_COMMAND_RECORD_DRAW_PRIMITIVE, texture, target);
  check(renderTapeFirstAccessObserve(ledger, draw.view()).classification ==
            RenderTapeFirstAccessClass::Unknown,
        "same draw read/write conflict fails closed");

  ledger = {};
  arm(ledger, target, target);
  check(renderTapeFirstAccessObserve(ledger, apply.view()).status ==
            RenderTapeFirstAccessStatus::Observing,
        "clear fixture binding");
  const D9CCommandChunkWireClear clear{
      1u, 0u, 1.0f, 0u, 0u, sizeof(D9CCommandChunkWireClear)};
  auto clearFixture = fixed(D9C_COMMAND_RECORD_CLEAR, &clear, sizeof(clear));
  check(clear.rectOffset == sizeof(D9CCommandChunkWireClear) &&
            clearFixture.payload.size() == sizeof(D9CCommandChunkWireClear),
        "full clear uses production appendClear wire shape");
  auto full = renderTapeFirstAccessObserve(ledger, clearFixture.view());
  check(full.classification == RenderTapeFirstAccessClass::FullClearWrite,
        "unrestricted target clear is full");

  ledger = {};
  arm(ledger, target, target);
  check(renderTapeFirstAccessObserve(ledger, apply.view()).status ==
            RenderTapeFirstAccessStatus::Observing,
        "partial clear fixture binding");
  const D9CRect rect{0, 0, 8, 8};
  const D9CCommandChunkWireClear partialClear{
      1u, 0u, 1.0f, 0u, 1u, sizeof(D9CCommandChunkWireClear)};
  const std::array<D9CRect, 1> rects{{rect}};
  auto partialFixture = clearWithRects(partialClear, rects);
  auto partial = renderTapeFirstAccessObserve(ledger, partialFixture.view());
  check(partial.classification ==
            RenderTapeFirstAccessClass::PartialClearWrite,
        "rect-restricted target clear is partial");

  RenderTapeFirstAccessLedger readLedger{};
  arm(readLedger, target, target);
  const std::array<SectionSpec, 1> readBinding{{
      {D9C_COMMAND_CHUNK_SECTION_TEXTURE, 0u, 0u}}};
  auto readDraw = sparse(D9C_COMMAND_RECORD_DRAW_PRIMITIVE, readBinding, target);
  auto read = renderTapeFirstAccessObserve(readLedger, readDraw.view());
  check(read.classification == RenderTapeFirstAccessClass::ShaderReadCandidate,
        "texture use before any write is a read candidate");

  RenderTapeFirstAccessLedger generateLedger{};
  arm(generateLedger, target, target);
  const D9CCommandChunkWireGenerateMipmaps generate{
      .textureHandleIndex = 0u,
  };
  const auto generateFixture = fixed(
      D9C_COMMAND_RECORD_GENERATE_MIPMAPS, &generate, sizeof(generate), target);
  const auto generateObservation =
      renderTapeFirstAccessObserve(generateLedger, generateFixture.view());
  check(generateObservation.status == RenderTapeFirstAccessStatus::Terminal &&
            generateObservation.classification ==
                RenderTapeFirstAccessClass::CopySource,
        "GenerateMipmaps requires the exact incoming base-level contents");
}

void testMismatchMalformedAndPresentHandle() {
  const auto target = identity(D9C_CHUNK_HANDLE_KIND_SURFACE, 4u, 40u);
  const auto other = identity(D9C_CHUNK_HANDLE_KIND_SURFACE, 5u, 40u);
  RenderTapeFirstAccessLedger ledger{};
  arm(ledger, target, target);
  const std::array<SectionSpec, 1> binding{{
      {D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET, 0u, 0u}}};
  auto mismatch = sparse(D9C_COMMAND_RECORD_APPLY_STATE, binding, other);
  check(renderTapeFirstAccessObserve(ledger, mismatch.view()).status ==
            RenderTapeFirstAccessStatus::Observing,
        "generation mismatch does not bind the target");

  D9CCommandChunkWirePresent present{};
  auto presentFixture = fixed(D9C_COMMAND_RECORD_PRESENT, &present,
                              sizeof(present), target);
  auto presentObservation =
      renderTapeFirstAccessObserve(ledger, presentFixture.view());
  check(presentObservation.classification ==
            RenderTapeFirstAccessClass::PresentRead &&
            presentObservation.handleIndex == 0u,
        "present uses its exact target handle when supplied");

  RenderTapeFirstAccessLedger malformedLedger{};
  arm(malformedLedger, target, target);
  Fixture malformedFixture;
  malformedFixture.record.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
  malformedFixture.record.payloadSize = 1u;
  malformedFixture.payload.resize(1u);
  check(renderTapeFirstAccessObserve(malformedLedger, malformedFixture.view())
                .status == RenderTapeFirstAccessStatus::Malformed,
        "malformed validated-view input fails closed");
}

void testCarriedTextureReads() {
  const auto origin = identity(D9C_CHUNK_HANDLE_KIND_SURFACE, 2u, 90u);
  const auto texture = identity(D9C_CHUNK_HANDLE_KIND_TEXTURE, 3u, 91u);
  const auto other = identity(D9C_CHUNK_HANDLE_KIND_TEXTURE, 4u, 91u);
  for (std::uint32_t slot = 0u; slot < D9C_DRAW_PACKET_MAX_TEXTURES; ++slot) {
    for (unsigned replacement = 0u; replacement < 3u; ++replacement) {
      RenderTapeFirstAccessLedger ledger;
      arm(ledger, origin, texture);
      const std::array<SectionSpec, 1> binding{{{D9C_COMMAND_CHUNK_SECTION_TEXTURE, 0u, slot}}};
      const auto apply = sparse(D9C_COMMAND_RECORD_APPLY_STATE, binding, texture);
      check(renderTapeFirstAccessObserve(ledger, apply.view()).status ==
          RenderTapeFirstAccessStatus::Observing, "texture binding alone is not a read");
      if (replacement) {
        const std::array<SectionSpec, 1> next{{{D9C_COMMAND_CHUNK_SECTION_TEXTURE,
            replacement == 1u ? D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX : 0u, slot}}};
        const auto replace = sparse(D9C_COMMAND_RECORD_APPLY_STATE, next, other);
        check(renderTapeFirstAccessObserve(ledger, replace.view()).status ==
            RenderTapeFirstAccessStatus::Observing, "unbind/rebind remains nonterminal");
      }
      auto draw = sparse(D9C_COMMAND_RECORD_DRAW_PRIMITIVE, {}, other);
      draw.handles.clear();
      draw.record.handleCount = 0u;
      const auto result = renderTapeFirstAccessObserve(ledger, draw.view());
      check(result.status == (replacement ? RenderTapeFirstAccessStatus::Observing
                                          : RenderTapeFirstAccessStatus::Terminal),
          "sparse next-chunk draw uses carried identity, not a new handle-table index");
      if (!replacement) {
        check(result.classification == RenderTapeFirstAccessClass::ShaderReadCandidate &&
            result.observedIdentity.generation == texture.generation &&
            result.bindingSlot == slot && result.handleIndex == kRenderTapeFirstAccessSentinel,
            "carried read preserves exact generation/slot without stale handle index");
        const D9CCommandChunkWireClear clear{.flags = 1u,
            .rectOffset = sizeof(D9CCommandChunkWireClear)};
        check(renderTapeFirstAccessObserve(ledger,
            fixed(D9C_COMMAND_RECORD_CLEAR, &clear, sizeof(clear)).view()).status ==
            RenderTapeFirstAccessStatus::Complete, "later clear cannot repair a read-first result");
      }
    }
  }
  for (const bool unbindAtDraw : {false, true}) {
    RenderTapeFirstAccessLedger ledger;
    arm(ledger, origin, texture);
    const std::array<SectionSpec, 1> binding{{{D9C_COMMAND_CHUNK_SECTION_TEXTURE, 0u, 0u}}};
    const auto apply = sparse(D9C_COMMAND_RECORD_APPLY_STATE, binding, texture);
    check(renderTapeFirstAccessObserve(ledger, apply.view()).status ==
        RenderTapeFirstAccessStatus::Observing, "carried texture precedes RT draw");
    const std::array<SectionSpec, 1> attachment{{{D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET, 0u, 0u}}};
    check(renderTapeFirstAccessObserve(ledger,
        sparse(D9C_COMMAND_RECORD_APPLY_STATE, attachment, origin).view()).status ==
        RenderTapeFirstAccessStatus::Observing, "alias attachment is carried into draw");
    std::vector<SectionSpec> sections;
    if (unbindAtDraw)
      sections.push_back({D9C_COMMAND_CHUNK_SECTION_TEXTURE,
                         D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX, 0u});
    const auto draw = sparse(D9C_COMMAND_RECORD_DRAW_PRIMITIVE, sections, origin);
    check(renderTapeFirstAccessObserve(ledger, draw.view()).classification ==
        (unbindAtDraw ? RenderTapeFirstAccessClass::DrawWriteUnknownCoverage
                      : RenderTapeFirstAccessClass::Unknown),
        "same-draw unbind precedes access; carried read plus alias write is ambiguous");
  }
  RenderTapeFirstAccessLedger ledger;
  arm(ledger, origin, texture);
  const std::array<SectionSpec, 1> invalid{{{D9C_COMMAND_CHUNK_SECTION_TEXTURE,
      D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX, D9C_DRAW_PACKET_MAX_TEXTURES}}};
  check(renderTapeFirstAccessObserve(ledger,
      sparse(D9C_COMMAND_RECORD_APPLY_STATE, invalid, texture).view()).status ==
      RenderTapeFirstAccessStatus::Malformed, "invalid null texture slot fails closed");
}

void testExplicitNullAttachmentBindings() {
  const auto target = identity(D9C_CHUNK_HANDLE_KIND_SURFACE, 1u, 91u);
  const D9CCommandChunkWireClear clear{.flags = 3u,
      .rectOffset = sizeof(D9CCommandChunkWireClear)};
  const auto write = fixed(D9C_COMMAND_RECORD_CLEAR, &clear, sizeof(clear));
  for (const auto kind : {D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET,
                          D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL,
                          D9C_COMMAND_CHUNK_SECTION_TEXTURE}) {
    const std::array<SectionSpec, 1> binding{{{static_cast<std::uint16_t>(kind), 0u, 0u}}};
    const std::array<SectionSpec, 1> nullBinding{{{static_cast<std::uint16_t>(kind),
        D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX, 0u}}};
    const auto apply = sparse(D9C_COMMAND_RECORD_APPLY_STATE, binding, target);
    const auto unbind = sparse(D9C_COMMAND_RECORD_APPLY_STATE, nullBinding, target);
    RenderTapeFirstAccessLedger ledger;
    arm(ledger, target, target);
    check(renderTapeFirstAccessObserve(ledger, apply.view()).status == RenderTapeFirstAccessStatus::Observing &&
        renderTapeFirstAccessObserve(ledger, unbind.view()).status == RenderTapeFirstAccessStatus::Observing,
        "valid=true with null handle is an explicit unbind, not malformed handle ownership");
    check(renderTapeFirstAccessObserve(ledger, write.view()).status == RenderTapeFirstAccessStatus::Observing,
        "clear after explicit unbind cannot retain the old attachment's first write");
    const std::array<SectionSpec, 1> stale{{{static_cast<std::uint16_t>(kind), 1u, 0u}}};
    auto invalid = sparse(D9C_COMMAND_RECORD_APPLY_STATE, stale, target);
    arm(ledger, target, target);
    const auto result = renderTapeFirstAccessObserve(ledger, invalid.view());
    check(result.status == RenderTapeFirstAccessStatus::Malformed,
        "nonnull out-of-range resource still fails closed");
  }
}

void testConstantSectionsPreserveFirstAccess() {
  const auto target = identity(D9C_CHUNK_HANDLE_KIND_SURFACE, 1u, 91u);
  const std::array<SectionSpec, 1> binding{{{D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET, 0u, 0u}}};
  const auto apply = sparse(D9C_COMMAND_RECORD_APPLY_STATE, binding, target);
  const D9CCommandChunkWireClear clear{.flags = 1u,
      .rectOffset = sizeof(D9CCommandChunkWireClear)};
  const auto write = fixed(D9C_COMMAND_RECORD_CLEAR, &clear, sizeof(clear));
  for (const auto& schema : kSectionRules) {
    if (!(schema.ruleFlags & SectionRuleConstantRange)) continue;
    for (unsigned variant = 0u; variant < 8u; ++variant) {
      Fixture constants;
      D9CCommandChunkWireDrawHeader draw{};
      draw.sectionCount = 1u;
      draw.sectionTableOffset = sizeof(draw);
      draw.sectionPayloadOffset = sizeof(draw) + sizeof(D9CCommandChunkWireSectionDesc);
      D9CCommandChunkWireConstantRange range{0u, variant == 1u ? schema.maxCount : 1u};
      D9CCommandChunkWireSectionDesc section{schema.kind, schema.elementSize, range.registerCount,
          draw.sectionPayloadOffset,
          static_cast<std::uint32_t>(sizeof(range) + range.registerCount * schema.elementSize)};
      if (variant == 2u) section.count = 0u;
      if (variant == 3u) ++section.elementSize;
      if (variant == 4u) --section.byteSize;
      if (variant == 5u) ++range.registerCount;
      if (variant == 6u) range.startRegister = schema.maxCount;
      if (variant == 7u) section.kind = UINT16_MAX;
      constants.payload.resize(draw.sectionPayloadOffset + section.byteSize);
      std::memcpy(constants.payload.data(), &draw, sizeof(draw));
      std::memcpy(constants.payload.data() + draw.sectionTableOffset, &section, sizeof(section));
      std::memcpy(constants.payload.data() + draw.sectionPayloadOffset, &range, sizeof(range));
      constants.record = {.type = D9C_COMMAND_RECORD_APPLY_STATE,
          .payloadSize = static_cast<std::uint32_t>(constants.payload.size())};
      RenderTapeFirstAccessLedger ledger;
      arm(ledger, target, target);
      check(renderTapeFirstAccessObserve(ledger, apply.view()).status ==
          RenderTapeFirstAccessStatus::Observing, "attachment binding precedes unrelated constants");
      const auto result = renderTapeFirstAccessObserve(ledger, constants.view());
      check(result.status == (variant < 2u ? RenderTapeFirstAccessStatus::Observing
                                          : RenderTapeFirstAccessStatus::Malformed),
          "six constant families admit range-header extents and reject malformed views");
      if (variant < 2u) {
        const auto next = renderTapeFirstAccessObserve(ledger, write.view());
        check(next.status == RenderTapeFirstAccessStatus::Terminal &&
            next.classification == RenderTapeFirstAccessClass::FullClearWrite &&
            next.observedIdentity.objectId == target.objectId,
            "unrelated constant range does not consume or alter the first attachment access");
      }
    }
  }
}

void testStandaloneDepthFullClearObservation() {
  const auto depth =
      identity(D9C_CHUNK_HANDLE_KIND_SURFACE, 1u, 4294967545ull);
  RenderTapeFirstAccessLedger ledger{};
  arm(ledger, depth, depth);
  const std::array<SectionSpec, 1> binding{{
      {D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL, 0u, 0u}}};
  const auto apply = sparse(D9C_COMMAND_RECORD_APPLY_STATE, binding, depth);
  const auto bound = renderTapeFirstAccessObserve(ledger, apply.view());
  check(bound.status == RenderTapeFirstAccessStatus::Observing &&
            bound.classification == RenderTapeFirstAccessClass::BindingOnly,
        "standalone D24X8 binding is observation-only");
  const D9CCommandChunkWireClear clear{
      .flags = 2u,
      .z = 1.0f,
      .rectOffset = sizeof(D9CCommandChunkWireClear),
  };
  const auto clearFixture = fixed(D9C_COMMAND_RECORD_CLEAR, &clear,
                                  sizeof(clear));
  const auto observation =
      renderTapeFirstAccessObserve(ledger, clearFixture.view());
  check(observation.status == RenderTapeFirstAccessStatus::Terminal &&
            observation.classification ==
                RenderTapeFirstAccessClass::FullClearWrite &&
            !observation.aliasOrigin &&
            observation.observedIdentity.kind ==
                D9C_CHUNK_HANDLE_KIND_SURFACE &&
            observation.observedIdentity.objectId == depth.objectId,
        "standalone depth full clear is observed exactly");

  std::array<D9CCommandChunkWireRecordHeader, 2> records{
      apply.record, clearFixture.record};
  records[1].payloadOffset = static_cast<std::uint32_t>(apply.payload.size());
  std::vector<std::byte> payload = apply.payload;
  payload.insert(payload.end(), clearFixture.payload.begin(),
                 clearFixture.payload.end());
  const ImportedChunkView view{
      .records = records,
      .handles = apply.handles,
      .payloadArena = payload,
  };
  const D9CSurfaceDesc d24x8{
      .format = 77u, .resourceType = 1u, .usage = 2u, .pool = 0u,
      .width = 2048u, .height = 2048u, .depth = 1u,
  };
  check(renderTapeClassifyProducedStandaloneSurfaceByCapturedPass(
            view, depth, d24x8).accepted(),
        "standalone D24X8 is admitted after an unrestricted depth clear");

  auto d24s8 = d24x8;
  d24s8.format = 75u;
  check(renderTapeClassifyProducedStandaloneSurfaceByCapturedPass(
            view, depth, d24s8).status ==
            RenderTapeProducedProofStatus::UnsupportedStandaloneDescriptor,
        "stencil-bearing depth remains fail-closed");

  auto stencilOnly = clear;
  stencilOnly.flags = 4u;
  const auto stencilFixture = fixed(D9C_COMMAND_RECORD_CLEAR, &stencilOnly,
                                    sizeof(stencilOnly));
  records[1] = stencilFixture.record;
  records[1].payloadOffset = static_cast<std::uint32_t>(apply.payload.size());
  payload = apply.payload;
  payload.insert(payload.end(), stencilFixture.payload.begin(),
                 stencilFixture.payload.end());
  const ImportedChunkView stencilView{
      .records = records,
      .handles = apply.handles,
      .payloadArena = payload,
  };
  check(renderTapeClassifyProducedStandaloneSurfaceByCapturedPass(
            stencilView, depth, d24x8).status ==
            RenderTapeProducedProofStatus::MissingRequiredClearAspect,
        "D24X8 rejects a stencil-only clear");
}

void testProducedByCapturedPassProof() {
  const auto origin = identity(D9C_CHUNK_HANDLE_KIND_SURFACE, 28u, 7527u);
  const auto resolved = identity(D9C_CHUNK_HANDLE_KIND_TEXTURE, 14u, 7525u);
  const std::array<SectionSpec, 1> binding{{
      {D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET, 0u, 0u}}};
  const auto apply = sparse(D9C_COMMAND_RECORD_APPLY_STATE, binding, origin);
  const D9CCommandChunkWireClear clear{
      .flags = 1u, .rectCount = 0u,
      .rectOffset = sizeof(D9CCommandChunkWireClear)};
  const auto clearFixture = fixed(D9C_COMMAND_RECORD_CLEAR, &clear,
                                  sizeof(clear));
  std::array<D9CCommandChunkWireRecordHeader, 2> records{
      apply.record, clearFixture.record};
  records[1].payloadOffset = static_cast<std::uint32_t>(apply.payload.size());
  std::vector<std::byte> payload = apply.payload;
  payload.insert(payload.end(), clearFixture.payload.begin(),
                 clearFixture.payload.end());
  ImportedChunkView view{
      .records = records,
      .handles = apply.handles,
      .payloadArena = payload,
  };
  const auto accepted =
      renderTapeClassifyProducedByCapturedPass(view, origin, resolved);
  check(accepted.accepted() &&
            accepted.observation.classification ==
                RenderTapeFirstAccessClass::FullClearWrite &&
            std::string_view(renderTapeProducedProofStatusName(
                accepted.status)) == "accepted" &&
            renderTapeProveProducedByCapturedPass(view, origin, resolved),
        "exact alias binding followed by full clear is typed and admitted");

  check(renderTapeClassifyProducedByCapturedPass(
            view, resolved, resolved).status ==
            RenderTapeProducedProofStatus::DirectTextureAmbiguity &&
            renderTapeClassifyProducedByCapturedPass(
                view,
                identity(D9C_CHUNK_HANDLE_KIND_BUFFER, 1u, 2u),
                resolved).status ==
                RenderTapeProducedProofStatus::InvalidOriginKind &&
            renderTapeClassifyProducedByCapturedPass(
                view, origin,
                identity(D9C_CHUNK_HANDLE_KIND_SURFACE, 1u, 2u)).status ==
                RenderTapeProducedProofStatus::InvalidResolvedKind,
        "identity-shape proof rejections are distinct");

  const auto bindingOnly =
      renderTapeClassifyProducedByCapturedPass(apply.view(), origin, resolved);
  check(bindingOnly.status == RenderTapeProducedProofStatus::NoTerminalAccess &&
            bindingOnly.observation.classification ==
                RenderTapeFirstAccessClass::BindingOnly,
        "binding-only same-chunk proof is attributed");

  const D9CRect rect{0, 0, 8, 8};
  const D9CCommandChunkWireClear partial{
      .flags = 1u, .rectCount = 1u,
      .rectOffset = sizeof(D9CCommandChunkWireClear)};
  const auto partialFixture = clearWithRects(partial, std::array{rect});
  records[1] = partialFixture.record;
  records[1].payloadOffset = static_cast<std::uint32_t>(apply.payload.size());
  payload = apply.payload;
  payload.insert(payload.end(), partialFixture.payload.begin(),
                 partialFixture.payload.end());
  view.payloadArena = payload;
  const auto partialResult =
      renderTapeClassifyProducedByCapturedPass(view, origin, resolved);
  check(partialResult.status ==
            RenderTapeProducedProofStatus::NotFullClearWrite &&
            partialResult.observation.classification ==
                RenderTapeFirstAccessClass::PartialClearWrite &&
            !renderTapeProveProducedByCapturedPass(view, origin, resolved),
        "partial clear rejection names the terminal access");

  auto draw = sparse(D9C_COMMAND_RECORD_DRAW_PRIMITIVE, {}, origin);
  records[1] = draw.record;
  records[1].payloadOffset = static_cast<std::uint32_t>(apply.payload.size());
  payload = apply.payload;
  payload.insert(payload.end(), draw.payload.begin(), draw.payload.end());
  view.payloadArena = payload;
  const auto drawResult =
      renderTapeClassifyProducedByCapturedPass(view, origin, resolved);
  check(drawResult.status == RenderTapeProducedProofStatus::NotFullClearWrite &&
            drawResult.observation.classification ==
                RenderTapeFirstAccessClass::DrawWriteUnknownCoverage &&
            !renderTapeProveProducedByCapturedPass(view, origin, resolved),
        "draw-first rejection names unknown write coverage");
}

struct ProducedTrace {
  std::vector<D9CCommandChunkWireRecordHeader> records{};
  std::vector<D9CCommandChunkWireHandleEntry> handles{};
  std::vector<std::byte> payload{};

  ImportedChunkView view() const noexcept {
    return {.records = records, .handles = handles, .payloadArena = payload};
  }
};

ProducedTrace producedTrace(std::initializer_list<Fixture> fixtures,
                           std::initializer_list<D9CWireObjectIdentity> identities) {
  ProducedTrace result;
  for (const auto& value : identities)
    result.handles.push_back({value.kind, value.generation, value.objectId});
  for (const auto& fixture : fixtures) {
    auto record = fixture.record;
    record.payloadOffset = static_cast<std::uint32_t>(result.payload.size());
    record.firstHandle = 0u;
    if (record.handleCount) {
      check(fixture.handles.size() == 1u, "produced trace fixtures carry one exact direct handle");
      const auto& direct = fixture.handles[0];
      std::size_t first = 0u;
      while (first < result.handles.size()) {
        const auto& candidate = result.handles[first];
        if (candidate.kind == direct.kind && candidate.generation == direct.generation &&
            candidate.objectId == direct.objectId) break;
        ++first;
      }
      check(first < result.handles.size(), "produced trace directly referenced identity is declared");
      record.firstHandle = static_cast<std::uint32_t>(first);
    }
    result.records.push_back(record);
    result.payload.insert(result.payload.end(), fixture.payload.begin(), fixture.payload.end());
  }
  return result;
}

std::vector<std::byte> producedTextureDescriptor(
    D9CSurfaceDesc base,
    std::uint32_t dimension = static_cast<std::uint32_t>(RenderTapeTextureDimension::Texture2D),
    std::uint32_t mipCount = 1u, std::uint32_t subresourceCount = 1u) {
  const RenderTapeTextureDescriptorV2 header{
      .dimension = dimension, .mipLevelCount = mipCount,
      .subresourceCount = subresourceCount,
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::ProducedByCapturedPass)};
  std::vector<std::byte> bytes(sizeof(header) + subresourceCount * sizeof(base));
  std::memcpy(bytes.data(), &header, sizeof(header));
  for (std::uint32_t index = 0u; index < subresourceCount; ++index) {
    auto current = base;
    const auto mip = mipCount ? index % mipCount : 0u;
    const auto extent = [mip](std::uint32_t value) {
      const auto shifted = value >> mip;
      return shifted ? shifted : 1u;
    };
    // Preserve invalid level-zero extents for descriptor-negative cases.
    if (mip) {
      current.width = extent(base.width);
      current.height = extent(base.height);
      if (dimension == static_cast<std::uint32_t>(RenderTapeTextureDimension::Volume))
        current.depth = extent(base.depth);
    }
    std::memcpy(bytes.data() + sizeof(header) + index * sizeof(current), &current, sizeof(current));
  }
  return bytes;
}

D9CSurfaceDesc producedDf24Surface() {
  return {.format = 875710020u, .resourceType = 3u, .usage = 2u, .pool = 0u,
      .multiSampleType = 0u, .multiSampleQuality = 0u,
      .width = 8u, .height = 9u, .depth = 1u};
}

RenderTapeSurfaceDescriptorV2 producedAlias(const D9CWireObjectIdentity& parent,
                                           D9CSurfaceDesc surface) {
  surface.resourceType = 1u;
  return {.storage = static_cast<std::uint32_t>(RenderTapeSurfaceStorage::TextureSubresource),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::Unavailable),
      .parentTexture = parent, .surface = surface};
}

Fixture producedClear(std::uint32_t flags, std::uint32_t rectCount = 0u) {
  const D9CCommandChunkWireClear clear{
      .flags = flags, .colorARGB = 0u, .z = 0.25f, .stencil = 0u, .rectCount = rectCount,
      .rectOffset = sizeof(D9CCommandChunkWireClear)};
  const std::array rects{D9CRect{0, 0, 8, 9}};
  return clearWithRects(clear, rectCount ? std::span<const D9CRect>(rects)
                                       : std::span<const D9CRect>{});
}

void testProducedDf24DescriptorPolicy() {
  using Policy = RenderTapeProducedTextureContentPolicy;
  check(kRenderTapeDf24D3DFormat == 875710020u,
        "DF24 policy names the exact FOURCC rather than another depth format");
  std::size_t accepted = 0u;
  for (const auto resource : {1u, 3u, 4u, 5u})
    for (const auto usage : {0u, 1u, 2u, 3u})
      for (const auto pool : {0u, 1u})
        for (const auto width : {0u, 1u, 8u})
          for (const auto height : {0u, 1u, 9u})
            for (const auto depth : {0u, 1u, 2u})
              for (const auto samples : {0u, 1u, 2u})
                for (const auto quality : {0u, 1u}) {
                  auto surface = producedDf24Surface();
                  surface.resourceType = resource; surface.usage = usage; surface.pool = pool;
                  surface.width = width; surface.height = height; surface.depth = depth;
                  surface.multiSampleType = samples; surface.multiSampleQuality = quality;
                  const bool expected = resource == 3u && usage == 2u && pool == 0u &&
                      width && height && depth == 1u && samples == 0u && quality == 0u;
                  const auto policy = renderTapeProducedTexturePolicy(producedTextureDescriptor(surface));
                  check(policy == (expected ? Policy::Df24DepthOnly : Policy::Unsupported),
                        "finite DF24 physical descriptor product admits only exact depth-only DEFAULT shape");
                  accepted += expected;
                }
  check(accepted == 4u, "DF24 descriptor product exercises every positive extent pair nonvacuously");
  for (const auto format : {0u, 75u, 77u, 80u, 875710021u}) {
    auto surface = producedDf24Surface(); surface.format = format;
    check(renderTapeProducedTexturePolicy(producedTextureDescriptor(surface)) == Policy::Unsupported,
          "stencil-bearing, other depth and unknown formats never acquire the DF24 depth-only policy");
  }
  for (const auto dimension : {0u, 1u, 2u, 3u, 4u})
    for (const auto levels : {0u, 1u, 2u})
      for (const auto subresources : {0u, 1u, 2u, 6u, 12u}) {
        auto surface = producedDf24Surface();
        if (dimension == 2u) surface.resourceType = 5u;
        if (dimension == 3u) { surface.resourceType = 4u; surface.depth = 2u; }
        const bool expected = dimension == 1u && levels == 1u && subresources == 1u;
        check(renderTapeProducedTexturePolicy(producedTextureDescriptor(
                  surface, dimension, levels, subresources)) ==
                  (expected ? Policy::Df24DepthOnly : Policy::Unsupported),
              "DF24 topology product rejects cube, volume, multimip and inconsistent subresources");
      }
  for (const auto count : {0u, 1u, 2u, 6u, 4096u}) {
    check(renderTapeProducedTextureAliasClosureSupported(Policy::Df24DepthOnly, count) == (count == 1u) &&
              renderTapeProducedTextureAliasClosureSupported(Policy::ColorRenderTarget, count) &&
              !renderTapeProducedTextureAliasClosureSupported(Policy::Unsupported, count),
          "alias closure is one exact DF24 alias without narrowing legacy color policy");
  }
  const auto origin = identity(D9C_CHUNK_HANDLE_KIND_SURFACE, 7u, 70u);
  const auto parent = identity(D9C_CHUNK_HANDLE_KIND_TEXTURE, 9u, 90u);
  const std::array<SectionSpec, 1> rt{{{D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET, 0u, 0u}}};
  const auto apply = sparse(D9C_COMMAND_RECORD_APPLY_STATE, rt, origin);
  const auto trace = producedTrace({apply, producedClear(1u)}, {origin});
  for (const auto dimension : {1u, 2u}) {
    auto color = producedDf24Surface();
    color.format = 21u; color.usage = 1u; color.resourceType = dimension == 2u ? 5u : 3u;
    if (dimension == 2u) color.height = color.width;
    const auto descriptor = producedTextureDescriptor(color, dimension, 1u, dimension == 2u ? 6u : 1u);
    check(renderTapeProducedTexturePolicy(descriptor) == Policy::ColorRenderTarget &&
              renderTapeClassifyProducedTextureByCapturedPass(
                  trace.view(), origin, parent, descriptor, producedAlias(parent, color)).accepted(),
          "matched one-mip 2D and cube color controls retain the legacy unrestricted RT clear proof");
  }
  for (const auto usage : {1u, 3u}) {
    auto mixed = producedDf24Surface(); mixed.usage = usage;
    const auto descriptor = producedTextureDescriptor(mixed);
    check(renderTapeClassifyProducedTextureByCapturedPass(trace.view(), origin, parent,
              descriptor, producedAlias(parent, mixed)).status ==
              RenderTapeProducedProofStatus::UnsupportedTextureDescriptor,
          "DF24 never falls through to color authority for RT or mixed RT/DS usage");
  }
}

void testProducedDf24ClearTruthTable() {
  const auto origin = identity(D9C_CHUNK_HANDLE_KIND_SURFACE, 7u, 70u);
  const auto parent = identity(D9C_CHUNK_HANDLE_KIND_TEXTURE, 9u, 90u);
  const auto surface = producedDf24Surface();
  const auto descriptor = producedTextureDescriptor(surface);
  const auto alias = producedAlias(parent, surface);
  for (const auto role : {D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL,
                          D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET,
                          D9C_COMMAND_CHUNK_SECTION_TEXTURE})
    for (const auto flags : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 10u, UINT32_MAX})
      for (const auto rectCount : {0u, 1u}) {
        const std::array<SectionSpec, 1> binding{{{static_cast<std::uint16_t>(role), 0u, 0u}}};
        const auto apply = sparse(D9C_COMMAND_RECORD_APPLY_STATE, binding, origin);
        const auto trace = producedTrace({apply, producedClear(flags, rectCount)}, {origin});
        const bool expected = role == D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL &&
            flags == 2u && rectCount == 0u;
        check(renderTapeProducedDf24ClearSupported(role, flags, rectCount) == expected &&
                  renderTapeClassifyProducedTextureByCapturedPass(
                      trace.view(), origin, parent, descriptor, alias).accepted() == expected,
              "DF24 typed proof and shared predicate agree for every role/flag/rectangle combination");
      }
  const std::array<SectionSpec, 1> binding{{{D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL, 0u, 0u}}};
  const auto apply = sparse(D9C_COMMAND_RECORD_APPLY_STATE, binding, origin);
  for (const auto mutation : {0u, 1u, 2u}) {
    auto clear = producedClear(2u);
    D9CCommandChunkWireClear header{};
    std::memcpy(&header, clear.payload.data(), sizeof(header));
    if (mutation == 0u) header.rectOffset = 0u;
    if (mutation == 1u) header.rectCount = 1u;
    if (mutation == 2u) clear.payload.resize(clear.payload.size() + sizeof(D9CRect));
    std::memcpy(clear.payload.data(), &header, sizeof(header));
    clear.record.payloadSize = static_cast<std::uint32_t>(clear.payload.size());
    const auto trace = producedTrace({apply, clear}, {origin});
    check(renderTapeClassifyProducedTextureByCapturedPass(trace.view(), origin, parent,
              descriptor, alias).status == RenderTapeProducedProofStatus::Malformed,
          "empty rectangle authority requires an exact valid tail rather than missing or excess bytes");
  }
}

void testProducedDf24FirstTerminalOrdering() {
  const auto origin = identity(D9C_CHUNK_HANDLE_KIND_SURFACE, 7u, 70u);
  const auto parent = identity(D9C_CHUNK_HANDLE_KIND_TEXTURE, 9u, 90u);
  const auto surface = producedDf24Surface();
  const auto descriptor = producedTextureDescriptor(surface);
  const auto alias = producedAlias(parent, surface);
  const std::array<SectionSpec, 1> depth{{{D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL, 0u, 0u}}};
  const std::array<SectionSpec, 1> sampled{{{D9C_COMMAND_CHUNK_SECTION_TEXTURE, 1u, 0u}}};
  const auto apply = sparse(D9C_COMMAND_RECORD_APPLY_STATE, depth, origin);
  const auto sampleBinding = sparse(D9C_COMMAND_RECORD_APPLY_STATE, sampled, parent);
  const auto read = sparse(D9C_COMMAND_RECORD_DRAW_PRIMITIVE, sampled, parent);
  const auto z = producedClear(2u);
  const auto good = producedTrace({apply, sampleBinding, z, read}, {origin, parent});
  const auto result = renderTapeClassifyProducedTextureByCapturedPass(
      good.view(), origin, parent, descriptor, alias);
  check(result.accepted() && result.observation.recordIndex == 2u &&
            result.observation.sectionKind == D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL &&
            result.observation.classification == RenderTapeFirstAccessClass::FullClearWrite,
        "binding-only prefix then exact Z producer admits later texture reads without changing the first terminal");
  const auto readFirst = producedTrace({read, apply, z}, {origin, parent});
  const auto readResult = renderTapeClassifyProducedTextureByCapturedPass(
      readFirst.view(), origin, parent, descriptor, alias);
  check(!readResult.accepted() && readResult.observation.recordIndex == 0u &&
            readResult.observation.classification == RenderTapeFirstAccessClass::ShaderReadCandidate,
        "later Z clear cannot repair the earliest parent texture read");
  const D9CCommandChunkWireGenerateMipmaps copy{.textureHandleIndex = 1u};
  const D9CCommandChunkWirePresent present{};
  auto draw = sparse(D9C_COMMAND_RECORD_DRAW_PRIMITIVE, {}, origin);
  draw.record.handleCount = 0u; // Only the carried DS binding is used by this draw.
  const std::array firstTerminals{
      producedClear(2u, 1u), producedClear(4u), producedClear(3u), producedClear(6u),
      draw,
      fixed(D9C_COMMAND_RECORD_GENERATE_MIPMAPS, &copy, sizeof(copy), parent),
      fixed(D9C_COMMAND_RECORD_PRESENT, &present, sizeof(present), origin)};
  for (const auto& first : firstTerminals) {
    const auto trace = producedTrace({apply, first, z}, {origin, parent});
    const auto rejected = renderTapeClassifyProducedTextureByCapturedPass(
        trace.view(), origin, parent, descriptor, alias);
    check(!rejected.accepted() && rejected.observation.recordIndex == 1u,
          "partial, stencil, mixed, draw, copy and Present terminals remain rejected before a later full Z clear");
  }
}

void testProducedDf24ExactAliasAndIdentity() {
  const auto origin = identity(D9C_CHUNK_HANDLE_KIND_SURFACE, 7u, 70u);
  const auto parent = identity(D9C_CHUNK_HANDLE_KIND_TEXTURE, 9u, 90u);
  const auto surface = producedDf24Surface();
  const auto descriptor = producedTextureDescriptor(surface);
  const auto alias = producedAlias(parent, surface);
  const std::array<SectionSpec, 1> depth{{{D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL, 0u, 0u}}};
  const auto apply = sparse(D9C_COMMAND_RECORD_APPLY_STATE, depth, origin);
  const auto trace = producedTrace({apply, producedClear(2u)}, {origin});
  check(renderTapeClassifyProducedTextureByCapturedPass(
            trace.view(), origin, parent, descriptor, alias).accepted(),
        "exact generation-qualified DF24 parent and subresource-zero alias are nonvacuously admitted");
  const std::array<std::string_view, 19> fields{
      "schemaVersion", "storage", "initialContentDisposition", "subresource",
      "parentTexture.kind", "parentTexture.generation", "parentTexture.objectId",
      "surface.format", "surface.resourceType", "surface.usage.rt", "surface.usage.mixed",
      "surface.pool", "surface.multiSampleType", "surface.multiSampleQuality",
      "surface.width.zero", "surface.width.mismatch", "surface.height.zero",
      "surface.height.mismatch", "surface.depth"};
  for (std::uint32_t mutation = 0u; mutation < 19u; ++mutation) {
    auto changed = alias;
    if (mutation == 0u) changed.schemaVersion = 1u;
    if (mutation == 1u) changed.storage = static_cast<std::uint32_t>(RenderTapeSurfaceStorage::Standalone);
    if (mutation == 2u) changed.initialContentDisposition = static_cast<std::uint32_t>(RenderTapeInitialContentDisposition::CompleteSeed);
    if (mutation == 3u) changed.subresource = 1u;
    if (mutation == 4u) changed.parentTexture.kind = D9C_CHUNK_HANDLE_KIND_SURFACE;
    if (mutation == 5u) ++changed.parentTexture.generation;
    if (mutation == 6u) ++changed.parentTexture.objectId;
    if (mutation == 7u) changed.surface.format = 75u;
    if (mutation == 8u) changed.surface.resourceType = 3u;
    if (mutation == 9u) changed.surface.usage = 1u;
    if (mutation == 10u) changed.surface.usage = 3u;
    if (mutation == 11u) changed.surface.pool = 1u;
    if (mutation == 12u) changed.surface.multiSampleType = 1u;
    if (mutation == 13u) changed.surface.multiSampleQuality = 1u;
    if (mutation == 14u) changed.surface.width = 0u;
    if (mutation == 15u) ++changed.surface.width;
    if (mutation == 16u) changed.surface.height = 0u;
    if (mutation == 17u) ++changed.surface.height;
    if (mutation == 18u) changed.surface.depth = 2u;
    const auto result = renderTapeClassifyProducedTextureByCapturedPass(
        trace.view(), origin, parent, descriptor, changed);
    check(result.status == RenderTapeProducedProofStatus::TextureAliasDescriptorMismatch,
          "every alias physical field, topology and parent identity must match exact DF24 storage: mutation=" +
              std::to_string(mutation) + " field=" + std::string(fields[mutation]) +
              " status=" + renderTapeProducedProofStatusName(result.status));
  }
  for (const auto mutation : {0u, 1u}) {
    auto stale = origin;
    if (mutation == 0u) ++stale.generation;
    else ++stale.objectId;
    const auto staleApply = sparse(D9C_COMMAND_RECORD_APPLY_STATE, depth, stale);
    const auto staleTrace = producedTrace({staleApply, producedClear(2u)}, {stale});
    check(renderTapeClassifyProducedTextureByCapturedPass(staleTrace.view(), origin, parent,
              descriptor, alias).status == RenderTapeProducedProofStatus::NoTerminalAccess,
          "stale generation or different alias object never proves the exact obligation");
  }
  const auto parentApply = sparse(D9C_COMMAND_RECORD_APPLY_STATE, depth, parent);
  const auto parentBinding = producedTrace({parentApply, producedClear(2u)}, {parent});
  check(renderTapeClassifyProducedTextureByCapturedPass(parentBinding.view(), origin, parent,
            descriptor, alias).status == RenderTapeProducedProofStatus::ObservedIdentityMismatch,
        "direct parent texture binding cannot impersonate the exact Surface alias");
  for (const auto mutation : {0u, 1u, 2u, 3u}) {
    auto invalidOrigin = origin;
    auto invalidParent = parent;
    if (mutation == 0u) invalidOrigin.generation = 0u;
    if (mutation == 1u) invalidOrigin.objectId = 0u;
    if (mutation == 2u) invalidParent.generation = 0u;
    if (mutation == 3u) invalidParent.objectId = 0u;
    const auto invalidApply = sparse(D9C_COMMAND_RECORD_APPLY_STATE, depth, invalidOrigin);
    const auto invalidTrace = producedTrace({invalidApply, producedClear(2u)}, {invalidOrigin});
    check(renderTapeClassifyProducedTextureByCapturedPass(invalidTrace.view(), invalidOrigin,
              invalidParent, descriptor, producedAlias(invalidParent, surface)).status ==
              RenderTapeProducedProofStatus::InvalidTextureIdentity,
          "zero generation or object id cannot create produced texture authority");
  }
}

} // namespace

int main() {
  try {
    testCrossChunkDrawAndExactlyOnce();
    testReadWriteConflictAndClear();
    testMismatchMalformedAndPresentHandle();
    testExplicitNullAttachmentBindings();
    testCarriedTextureReads();
    testConstantSectionsPreserveFirstAccess();
    testStandaloneDepthFullClearObservation();
    testProducedByCapturedPassProof();
    testProducedDf24DescriptorPolicy();
    testProducedDf24ClearTruthTable();
    testProducedDf24FirstTerminalOrdering();
    testProducedDf24ExactAliasAndIdentity();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "render tape first access locator spec passed\n";
  return 0;
}
