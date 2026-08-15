#include "device_c_render_tape_first_access_locator.hpp"
#include "dxmt9/device_c.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
        "standalone depth full clear is observed without widening Produced proof");
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

} // namespace

int main() {
  try {
    testCrossChunkDrawAndExactlyOnce();
    testReadWriteConflictAndClear();
    testMismatchMalformedAndPresentHandle();
    testStandaloneDepthFullClearObservation();
    testProducedByCapturedPassProof();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "render tape first access locator spec passed\n";
  return 0;
}
