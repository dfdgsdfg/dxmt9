#include "device_c_render_tape_initial_setup_v4.hpp"
#include "device_c_render_tape_capture_layout.hpp"
#include "device_c_render_tape_runtime_objects.hpp"
#include "render_tape_cpu_destroy_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace dxmt9::d3d9;
using render_tape_runtime::GeneralPreflightPlan;
using test::general_cpu_destroy_fixture_detail::bytesOf;
using test::general_cpu_destroy_fixture_detail::makeChunk;
using test::general_cpu_destroy_fixture_detail::Record;
using Status = RenderTapeInitialSetupV4Status;
using Phase = RenderTapeInitialSetupV4OwnerPhase;
using Action = RenderTapeInitialSetupV4Action;
using State = RenderTapeInitialSetupV4OwnerState;
using Mode = RenderTapeInitialSetupV4Mode;
constexpr auto noIndex = kRenderTapeIdentityV4NoIndex;
std::uint64_t checks = 0u;

void check(bool value, std::string_view message) {
  ++checks;
  if (!value) throw std::runtime_error(std::string(message));
}

constexpr D9CWireObjectIdentity query{D9C_CHUNK_HANDLE_KIND_QUERY, 7u, 900u};
constexpr D9CWireObjectIdentity spareQuery{D9C_CHUNK_HANDLE_KIND_QUERY, 7u, 901u};

enum class Variant {
  Normal, Empty, InterveningDefinition, InterveningCommand, InterveningControl,
  InterveningDestroy, InitialCpuUnlock, UnsupportedSurface, NoncanonicalBuffer,
  ReversedSubresources, Produced, ProducedTexture, ProducedDepthTexture, ProducedFpTexture, AllocationOnly,
  AllocationWithSeeds, AllocationTwo, AllocationExtraAlias, AllocationOverflow,
  AllocationGenerationReuse,
};

struct Fixture {
  std::vector<std::byte> tape{}, identityBytes{}, topologyBytes{};
  std::vector<std::vector<std::byte>> blobs{};
  GeneralPreflightPlan replay{};
  CaptureSourceReplayPlanV4 source{};
  std::uint32_t prefix = 0u, clearEvent = 0u, queryEvent = 0u;
  std::uint32_t trueWriteEvent = noIndex;
};

std::vector<std::byte> textureDescriptor(RenderTapeTextureDimension dimension,
    std::uint32_t mips) {
  const auto count = mips * (dimension == RenderTapeTextureDimension::Cube ? 6u : 1u);
  const RenderTapeTextureDescriptorV2 fixed{.dimension = static_cast<std::uint32_t>(dimension),
      .mipLevelCount = mips, .subresourceCount = count,
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteSeed)};
  auto bytes = bytesOf(fixed);
  for (std::uint32_t index = 0u; index < count; ++index) {
    const auto mip = renderTapeTextureDescriptorMipLevel(dimension, mips, index);
    const D9CSurfaceDesc surface{.format = 21u,
        .resourceType = dimension == RenderTapeTextureDimension::Texture2D ? 3u
            : dimension == RenderTapeTextureDimension::Cube ? 5u : 4u,
        .width = 4u >> mip, .height = 4u >> mip,
        .depth = dimension == RenderTapeTextureDimension::Volume ? 4u >> mip : 1u};
    const auto next = bytesOf(surface);
    bytes.insert(bytes.end(), next.begin(), next.end());
  }
  return bytes;
}

// V4 rows are authored from a genuine indexed/resource Tape through the
// production General parser, canonical provenance validator and pair builder.
// Clear remains pending across late logical setup and publishes only at END.
Fixture makeFixture(Variant variant = Variant::Normal) {
  Fixture out;
  auto original = test::makeGeneralCpuDestroyFixture(false, false);
  out.blobs = std::move(original.blobs);
  ImportedRenderTapeView imported;
  check(importPrevalidatedRenderTape(original.tape, imported), "strict original Tape imports");
  RenderTapeBuilder builder;
  std::uint32_t nextEvent = 0u;
  std::uint64_t present = 0u;
  bool inserted = false;
  const auto appendQuery = [&](D9CWireObjectIdentity identity) {
    const RenderTapeQueryDescriptorV2 descriptor{.type = 8u, .dataBytes = 4u,
        .initialState = static_cast<std::uint32_t>(RenderTapeQueryInitialState::Unissued)};
    builder.appendObjectDefine(identity, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Query),
        std::as_bytes(std::span(&descriptor, 1u)), 0u, {});
    ++nextEvent;
  };
  const auto appendClear = [&] {
    const D9CCommandChunkWireClear clear{.flags = 1u, .colorARGB = 0xff123456u,
        .z = 1.0f, .rectOffset = sizeof(D9CCommandChunkWireClear)};
    const std::array records{Record{.type = D9C_COMMAND_RECORD_CLEAR, .payload = bytesOf(clear)}};
    builder.appendCommandChunk({.recordCount = 1u}, makeChunk(records));
    ++nextEvent;
  };
  const auto addBlob = [&](std::size_t bytes, std::uint32_t salt) {
    std::vector<std::byte> contents(bytes);
    for (std::size_t index = 0u; index < bytes; ++index)
      contents[index] = static_cast<std::byte>((index + salt) & 0xffu);
    auto digest = RenderTapeCaptureSession::sha256(contents);
    if (std::none_of(out.blobs.begin(), out.blobs.end(), [&](const auto& prior) { return prior == contents; }))
      out.blobs.push_back(std::move(contents));
    return digest;
  };
  const auto insertInterruption = [&] {
    switch (variant) {
    case Variant::InterveningDefinition: appendQuery({D9C_CHUNK_HANDLE_KIND_QUERY, 8u, 902u}); break;
    case Variant::InterveningCommand: {
      const D9CCommandChunkWireSetConst fixed{.registerCount = 1u};
      auto payload = bytesOf(fixed);
      payload.resize(sizeof(fixed) + 4u * sizeof(float));
      const std::array records{Record{.type = D9C_COMMAND_RECORD_SET_VS_CONST_F,
          .payload = std::move(payload)}};
      builder.appendCommandChunk({.recordCount = 1u}, makeChunk(records));
      ++nextEvent;
      break;
    }
    case Variant::InterveningControl: {
      const std::vector<std::byte> gamma(kRenderTapeGammaRampBytes);
      builder.appendOrderedControl({.kind = static_cast<std::uint32_t>(RenderTapeControlKind::GammaRampSet),
          .disposition = static_cast<std::uint32_t>(RenderTapeControlDisposition::Completed),
          .controlBytes = kRenderTapeGammaRampBytes}, gamma);
      ++nextEvent;
      break;
    }
    case Variant::InterveningDestroy:
      builder.appendObjectDestroy(spareQuery);
      ++nextEvent;
      break;
    default: break;
    }
  };
  const auto appendOwner = [&](D9CWireObjectIdentity identity, RenderTapeDescriptorKind kind,
      const std::vector<std::byte>& descriptor, bool first) {
    const auto contract = renderTapeDeriveExpectedContentContract(identity.kind, descriptor);
    check(contract.status == RenderTapeExpectedContentStatus::Accepted, "typed seed byte contract exists");
    std::vector<std::uint64_t> extents(contract.count);
    renderTapeDeriveExpectedContentContract(identity.kind, descriptor, extents);
    if (variant == Variant::NoncanonicalBuffer && identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER)
      extents[0] /= 2u;
    const auto promisedBytes = variant == Variant::NoncanonicalBuffer &&
        identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER ? contract.bytes / 2u : contract.bytes;
    builder.appendObjectDefine(identity, static_cast<std::uint32_t>(kind), descriptor,
        0u, {}, promisedBytes, contract.count);
    ++nextEvent;
    if (first) insertInterruption();
    for (std::uint32_t index = 0u; index < contract.count; ++index) {
      const auto subresource = variant == Variant::ReversedSubresources ? contract.count - index - 1u : index;
      builder.appendResourceMutation(identity, variant == Variant::InitialCpuUnlock && first
          ? RenderTapeMutationKind::CpuUnlock : RenderTapeMutationKind::Upload,
          subresource, 0u, extents[subresource], addBlob(extents[subresource],
              static_cast<std::uint32_t>(identity.objectId) + subresource));
      ++nextEvent;
    }
  };
  const auto appendAlias = [&](D9CWireObjectIdentity parent, std::uint32_t subresource,
      const std::vector<std::byte>& descriptor, std::uint64_t id) {
    D9CSurfaceDesc surface{};
    check(renderTapeTextureSubresourceDescriptor(descriptor, subresource, surface), "alias subresource exists");
    surface.resourceType = 1u;
    const RenderTapeSurfaceDescriptorV2 alias{
        .storage = static_cast<std::uint32_t>(RenderTapeSurfaceStorage::TextureSubresource),
        .initialContentDisposition = static_cast<std::uint32_t>(RenderTapeInitialContentDisposition::Unavailable),
        .subresource = subresource, .parentTexture = parent, .surface = surface};
    builder.appendObjectDefine({D9C_CHUNK_HANDLE_KIND_SURFACE, 11u, id},
        static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
        std::as_bytes(std::span(&alias, 1u)), 0u, {});
    ++nextEvent;
  };
  const auto appendAllocation = [&](std::uint64_t id, std::uint64_t aliasId,
      std::uint32_t generation = 11u) {
    auto descriptor = textureDescriptor(RenderTapeTextureDimension::Texture2D, 1u);
    RenderTapeTextureDescriptorV2 fixed{};
    std::memcpy(&fixed, descriptor.data(), sizeof(fixed));
    fixed.initialContentDisposition = static_cast<std::uint32_t>(
        RenderTapeInitialContentDisposition::NullRtDescriptorOnly);
    D9CSurfaceDesc surface{};
    std::memcpy(&surface, descriptor.data() + sizeof(fixed), sizeof(surface));
    surface.format = kRenderTapeNullRtD3DFormat;
    surface.usage = 1u;
    if (variant == Variant::AllocationOverflow) surface.width = surface.height = UINT32_MAX;
    std::memcpy(descriptor.data(), &fixed, sizeof(fixed));
    std::memcpy(descriptor.data() + sizeof(fixed), &surface, sizeof(surface));
    const D9CWireObjectIdentity identity{D9C_CHUNK_HANDLE_KIND_TEXTURE, generation, id};
    builder.appendObjectDefine(identity, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
        descriptor, 0u, {});
    ++nextEvent;
    appendAlias(identity, 0u, descriptor, aliasId);
    if (variant == Variant::AllocationExtraAlias) {
      // Wrappers for one parent/subresource share a logical slot, so the
      // metadata-bound fixture replaces the unused alias only after retirement.
      builder.appendObjectDestroy({D9C_CHUNK_HANDLE_KIND_SURFACE, 11u, aliasId});
      ++nextEvent;
      appendAlias(identity, 0u, descriptor, aliasId + 1u);
    }
  };
  const auto appendProduced = [&] {
    const bool depth = variant == Variant::ProducedDepthTexture;
    const bool fp = variant == Variant::ProducedFpTexture;
    const D9CWireObjectIdentity produced{D9C_CHUNK_HANDLE_KIND_SURFACE, 11u, 1021u};
    const RenderTapeSurfaceDescriptorV2 surface{
        .storage = static_cast<std::uint32_t>(RenderTapeSurfaceStorage::Standalone),
        .initialContentDisposition = static_cast<std::uint32_t>(RenderTapeInitialContentDisposition::ProducedByCapturedPass),
        .surface = {.format = depth ? kRenderTapeDf24D3DFormat : fp ? 113u : 21u,
            .resourceType = 1u, .usage = depth ? 2u : 1u,
            .width = 16u, .height = 16u, .depth = 1u}};
    if (variant == Variant::ProducedTexture || depth || fp) {
      auto descriptor = textureDescriptor(RenderTapeTextureDimension::Texture2D, 1u);
      RenderTapeTextureDescriptorV2 fixed{};
      std::memcpy(&fixed, descriptor.data(), sizeof(fixed));
      fixed.initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::ProducedByCapturedPass);
      auto level = surface.surface;
      level.resourceType = 3u;
      std::memcpy(descriptor.data(), &fixed, sizeof(fixed));
      std::memcpy(descriptor.data() + sizeof(fixed), &level, sizeof(level));
      const D9CWireObjectIdentity parent{D9C_CHUNK_HANDLE_KIND_TEXTURE, 11u, 1022u};
      builder.appendObjectDefine(parent, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture), descriptor, 0u, {});
      ++nextEvent;
      appendAlias(parent, 0u, descriptor, produced.objectId);
    } else {
      builder.appendObjectDefine(produced, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
          std::as_bytes(std::span(&surface, 1u)), 0u, {});
      ++nextEvent;
    }
    // The actual whole-target Clear stays an interval command, never a
    // setup seed. The snapshot routes it to the newly produced surface.
    if (depth) {
      const D9CCommandChunkWireDrawHeader state{.sectionCount = 1u,
          .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader),
          .sectionPayloadOffset = sizeof(D9CCommandChunkWireDrawHeader) + sizeof(D9CCommandChunkWireSectionDesc)};
      const D9CCommandChunkWireSectionDesc section{.kind = D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL,
          .elementSize = sizeof(D9CCommandChunkWireDepthStencilBinding), .count = 1u,
          .payloadOffset = state.sectionPayloadOffset, .byteSize = sizeof(D9CCommandChunkWireDepthStencilBinding)};
      const D9CCommandChunkWireDepthStencilBinding binding{.valid = 1u, .handleIndex = 0u};
      auto payload = bytesOf(state);
      const auto sectionBytes = bytesOf(section), bindingBytes = bytesOf(binding);
      payload.insert(payload.end(), sectionBytes.begin(), sectionBytes.end());
      payload.insert(payload.end(), bindingBytes.begin(), bindingBytes.end());
      const D9CCommandChunkWireClear clear{.flags = 2u, .z = 1.0f,
          .rectOffset = sizeof(D9CCommandChunkWireClear)};
      const std::array records{Record{.type = D9C_COMMAND_RECORD_APPLY_STATE,
          .payload = std::move(payload), .handles = {{produced.kind, produced.generation, produced.objectId}}},
          Record{.type = D9C_COMMAND_RECORD_CLEAR, .payload = bytesOf(clear)}};
      builder.appendCommandChunk({.recordCount = 2u, .handleCount = 1u}, makeChunk(records));
      ++nextEvent;
      return;
    }
    auto stateBytes = test::general_cpu_destroy_fixture_detail::bootstrapChunk();
    D9CCommandChunkWireHeader stateHeader{};
    std::memcpy(&stateHeader, stateBytes.data(), sizeof(stateHeader));
    const D9CCommandChunkWireHandleEntry replacement{produced.kind, produced.generation, produced.objectId};
    std::memcpy(stateBytes.data() + stateHeader.handleTableOffset + 4u * sizeof(replacement),
        &replacement, sizeof(replacement));
    ImportedChunkView stateChunk;
    check(importPrevalidatedCommandChunk(stateBytes, {.recordCount = 1u, .handleCount = 5u}, stateChunk),
        "produced-target snapshot remains canonical");
    const D9CCommandChunkWireClear clear{.flags = 1u, .colorARGB = 0xff123456u,
        .z = 1.0f, .rectOffset = sizeof(D9CCommandChunkWireClear)};
    const std::array records{Record{.type = stateChunk.records[0].type,
        .payload = std::vector<std::byte>(stateChunk.record(0u).payload.begin(), stateChunk.record(0u).payload.end()),
        .handles = std::vector<D9CCommandChunkWireHandleEntry>(stateChunk.handles.begin(), stateChunk.handles.end())},
        Record{.type = D9C_COMMAND_RECORD_CLEAR, .payload = bytesOf(clear)}};
    builder.appendCommandChunk({.recordCount = 2u, .handleCount = 5u}, makeChunk(records));
    ++nextEvent;
  };

  for (std::size_t originalIndex = 0u; originalIndex < imported.events.size(); ++originalIndex) {
    const auto event = imported.event(originalIndex);
    const auto type = static_cast<RenderTapeEventType>(event.header.type);
    if (type == RenderTapeEventType::CommandChunk && !inserted) {
      appendQuery(query);
      if (variant == Variant::InterveningDestroy) appendQuery(spareQuery);
      out.prefix = out.clearEvent = nextEvent;
      appendClear();
      if (variant == Variant::AllocationOnly || variant == Variant::AllocationWithSeeds ||
          variant == Variant::AllocationTwo || variant == Variant::AllocationExtraAlias ||
          variant == Variant::AllocationOverflow || variant == Variant::AllocationGenerationReuse) {
        appendAllocation(1100u, 1120u);
        if (variant == Variant::AllocationTwo) appendAllocation(1101u, 1121u);
        if (variant == Variant::AllocationGenerationReuse) {
          builder.appendObjectDestroy({D9C_CHUNK_HANDLE_KIND_SURFACE, 11u, 1120u});
          ++nextEvent;
          builder.appendObjectDestroy({D9C_CHUNK_HANDLE_KIND_TEXTURE, 11u, 1100u});
          ++nextEvent;
          appendAllocation(1100u, 1121u, 12u);
        }
      }
      if (variant != Variant::Empty && variant != Variant::AllocationOnly &&
          variant != Variant::AllocationTwo && variant != Variant::AllocationExtraAlias &&
          variant != Variant::AllocationOverflow && variant != Variant::AllocationGenerationReuse) {
        const D9CWireObjectIdentity texture2d{D9C_CHUNK_HANDLE_KIND_TEXTURE, 11u, 1000u};
        const D9CWireObjectIdentity cube{D9C_CHUNK_HANDLE_KIND_TEXTURE, 11u, 1001u};
        const D9CWireObjectIdentity volume{D9C_CHUNK_HANDLE_KIND_TEXTURE, 11u, 1002u};
        const D9CWireObjectIdentity vb{D9C_CHUNK_HANDLE_KIND_BUFFER, 11u, 1003u};
        const D9CWireObjectIdentity ib{D9C_CHUNK_HANDLE_KIND_BUFFER, 11u, 1004u};
        const auto t2d = textureDescriptor(RenderTapeTextureDimension::Texture2D, 2u);
        const auto tcube = textureDescriptor(RenderTapeTextureDimension::Cube, 2u);
        appendOwner(texture2d, RenderTapeDescriptorKind::Texture, t2d, true);
        appendAlias(texture2d, 0u, t2d, 1010u);
        appendOwner(cube, RenderTapeDescriptorKind::Texture, tcube, false);
        appendAlias(cube, 7u, tcube, 1011u);
        appendOwner(volume, RenderTapeDescriptorKind::Texture,
            textureDescriptor(RenderTapeTextureDimension::Volume, 1u), false);
        appendOwner(vb, RenderTapeDescriptorKind::Buffer, bytesOf(D9CBufferDesc{.size = 32u, .usage = 0x200u}), false);
        appendOwner(ib, RenderTapeDescriptorKind::Buffer, bytesOf(D9CBufferDesc{.size = 16u, .format = 101u}), false);
        if (variant == Variant::UnsupportedSurface) {
          const RenderTapeSurfaceDescriptorV2 surface{
              .storage = static_cast<std::uint32_t>(RenderTapeSurfaceStorage::Standalone),
              .initialContentDisposition = static_cast<std::uint32_t>(RenderTapeInitialContentDisposition::CompleteSeed),
              .surface = {.format = 21u, .resourceType = 1u, .usage = 1u,
                  .width = 4u, .height = 4u, .depth = 1u}};
          appendOwner({D9C_CHUNK_HANDLE_KIND_SURFACE, 11u, 1020u},
              RenderTapeDescriptorKind::Surface, bytesOf(surface), false);
        }
        out.trueWriteEvent = nextEvent;
        builder.appendResourceMutation(vb, RenderTapeMutationKind::CpuUnlock, 0u, 8u, 8u,
            addBlob(8u, 123u), RenderTapeBufferMutationDisposition::NoOverwrite);
        ++nextEvent;
      }
      out.queryEvent = nextEvent;
      const std::array records{Record{.type = D9C_COMMAND_RECORD_QUERY_ISSUE,
          .payload = bytesOf(D9CCommandChunkWireQueryIssue{.queryHandleIndex = 0u, .flags = 1u}),
          .handles = {{query.kind, query.generation, query.objectId}}}};
      builder.appendCommandChunk({.recordCount = 1u, .handleCount = 1u}, makeChunk(records));
      ++nextEvent;
      if (variant == Variant::Produced || variant == Variant::ProducedTexture ||
          variant == Variant::ProducedDepthTexture || variant == Variant::ProducedFpTexture) appendProduced();
      inserted = true;
    }
    if (type == RenderTapeEventType::PresentComplete) {
      auto payload = std::vector<std::byte>(event.payload.begin(), event.payload.end());
      RenderTapePresentCompleteHeader completion{};
      std::memcpy(&completion, payload.data(), sizeof(completion));
      completion.presentOrdinal = present;
      std::memcpy(payload.data(), &completion, sizeof(completion));
      builder.appendRawEvent(type, payload);
    } else {
      if (type == RenderTapeEventType::CommandChunk) {
        RenderTapeCommandChunkHeader fixed{};
        std::memcpy(&fixed, event.payload.data(), sizeof(fixed));
        ImportedChunkView chunk;
        check(importPrevalidatedCommandChunk(event.payload.subspan(sizeof(fixed)),
            {fixed.wireVersion, fixed.recordCount, fixed.handleCount}, chunk), "original command imports");
        if (chunk.records[0].type == D9C_COMMAND_RECORD_PRESENT) present = nextEvent + 1u;
      }
      builder.appendRawEvent(type, event.payload);
    }
    ++nextEvent;
  }
  out.tape = builder.seal();
  std::vector<RenderTapeProviderBlob> blobs;
  for (const auto& bytes : out.blobs) blobs.push_back({RenderTapeCaptureSession::sha256(bytes), bytes});
  const auto replayResult = render_tape_runtime::buildGeneralReplayPlan(out.tape, blobs, out.replay);
  check(replayResult.complete(), std::string("genuine late-resource chronology passes full General grammar: ") +
      frameTapeReplayStatusName(replayResult.status) + " event=" + std::to_string(replayResult.failedEventIndex));

  RenderTapeIdentityV4View identity;
  identity.header.coordinatorDomain = static_cast<std::uint32_t>(TapeCoordinatorAuthority::OrdinarySerial);
  identity.header.frameId = 9u;
  identity.header.captureToken = 100u;
  identity.header.captureEpoch = 101u;
  identity.header.presentOrdinal = present;
  identity.header.queue = 102u;
  identity.header.initializerEpoch = 103u;
  identity.header.baselineCompletionEdge = 1u;
  identity.header.baselineKind = static_cast<std::uint32_t>(RenderTapeIdentityV3PrefixKind::EmptyBaseline);
  RenderTapeExecutionTopologyV1View topology;
  topology.header.captureToken = identity.header.captureToken;
  topology.header.captureEpoch = identity.header.captureEpoch;
  topology.header.queue = identity.header.queue;
  std::uint64_t edge = 2u, generation = 1u;
  for (const auto& event : out.replay.events) {
    if (event.type != RenderTapeEventType::CommandChunk) continue;
    const auto ordinal = out.replay.tape.event(event.eventIndex).header.ordinal;
    const auto raw = 500u + ordinal;
    const auto& admitted = out.replay.commandChunks[event.index];
    ImportedChunkView chunk;
    check(importPrevalidatedCommandChunk(admitted.bytes, admitted.envelope, chunk), "strict provenance raw imports");
    const bool isQuery = event.eventIndex == out.queryEvent;
    const auto begin = edge++, effect = edge++;
    const auto admission = isQuery ? edge++ : 0u;
    const auto dispatch = isQuery ? edge++ : 0u;
    std::uint32_t commandCount = 0u;
    for (const auto& record : chunk.records)
      commandCount += record.type == D9C_COMMAND_RECORD_CLEAR || record.type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE ||
          record.type == D9C_COMMAND_RECORD_PRESENT;
    const RenderTapeIdentityV4ReplaySourceIdentity projection{9u, 600u + ordinal, 600u + ordinal, raw};
    const auto thisGeneration = generation++;
    if (isQuery) {
      auto& source = identity.sources[0];
      source.lifecycle.publicationEdge = edge++;
      topology.plans[0].acceptanceEdge = edge++;
      source.lifecycle.submissionEdge = edge++;
      source.lifecycle.completionEdge = edge++;
      source.lifecycle.cutEventOrdinal = ordinal;
      source.lifecycle.cutKind = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterRecord);
      source.lifecycle.cutRecord = 1u;
      source.rawOrdinal = raw;
      source.publicationBuildGeneration = thisGeneration;
      identity.dispositions.push_back({.eventOrdinal = ordinal, .recordCount = 1u,
          .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::Coordinator), .coordinatorIndex = 0u});
      identity.coordinators.push_back({.provenance = {.eventOrdinal = ordinal, .rawOrdinal = raw,
          .buildGeneration = thisGeneration, .dispatchEdge = dispatch, .queryIdentity = query,
          .issueCycle = 1u, .issueOrdinal = 1u, .deviceEpoch = 903u,
          .writerCaptureId = source.lifecycle.writerCaptureId,
          .writerGeneration = source.lifecycle.writerGeneration,
          .storageGeneration = source.lifecycle.storageGeneration, .prefixSequence = 1u,
          .recordType = D9C_COMMAND_RECORD_QUERY_ISSUE, .flags = 1u, .queryType = 8u,
          .prefixKind = static_cast<std::uint32_t>(RenderTapeIdentityV3PrefixKind::RealSource),
          .sourceIndex = 0u, .commandEnd = 1u}, .admissionEdge = admission,
          .authority = static_cast<std::uint32_t>(TapeCoordinatorAuthority::OrdinarySerial)});
    } else if (!commandCount) {
      identity.dispositions.push_back({.eventOrdinal = ordinal, .recordCount = admitted.envelope.recordCount,
          .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::StateOnly)});
    } else {
      const auto source = static_cast<std::uint32_t>(identity.sources.size());
      const auto command = static_cast<std::uint32_t>(identity.commands.size());
      const bool first = event.eventIndex == out.clearEvent;
      const auto publication = first ? 0u : edge++;
      const auto acceptance = first ? 0u : edge++;
      const auto submission = first ? 0u : edge++;
      const auto completion = first ? 0u : edge++;
      identity.sources.push_back({.lifecycle = {.sourceOrdinal = source + 1u, .seqId = source + 1u,
          .writerCaptureId = source + 1u, .writerGeneration = 2u, .storageGeneration = 3u,
          .publicationEdge = publication, .submissionEdge = submission, .completionEdge = completion,
          .completedSequence = source + 1u, .cutEventOrdinal = ordinal, .commandCount = commandCount,
          .cutKind = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterEvent)},
          .rawOrdinal = raw, .publicationBuildGeneration = thisGeneration, .publicationControlIndex = source});
      for (std::uint32_t recordIndex = 0u; recordIndex < chunk.records.size(); ++recordIndex) {
        const auto type = chunk.records[recordIndex].type;
        const bool gpu = type == D9C_COMMAND_RECORD_CLEAR || type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE ||
            type == D9C_COMMAND_RECORD_PRESENT;
        if (!gpu) {
          identity.dispositions.push_back({.eventOrdinal = ordinal, .firstRecord = recordIndex, .recordCount = 1u,
              .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::StateOnly)});
          continue;
        }
        const auto commandIndex = static_cast<std::uint32_t>(identity.commands.size());
        const bool draw = type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
        identity.commands.push_back({.sourceIndex = source, .physicalCommandIndex = commandIndex - command,
            .kind = static_cast<std::uint32_t>(draw ? RenderTapeIdentityV3CommandKind::DrawRun
                : type == D9C_COMMAND_RECORD_CLEAR ? RenderTapeIdentityV3CommandKind::Clear : RenderTapeIdentityV3CommandKind::Present),
            .drawCount = draw ? 1u : 0u});
        identity.dispositions.push_back({.eventOrdinal = ordinal, .firstRecord = recordIndex, .recordCount = 1u,
            .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::GpuCommand),
            .commandIndex = commandIndex, .subdrawCount = draw ? 1u : 0u});
        topology.placements.push_back({.planIndex = source, .commandIndex = commandIndex,
            .outcome = static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1Outcome::ExecutedWithoutDag),
            .executionOrdinal = commandIndex - command});
      }
      topology.plans.push_back({.planId = source + 11u, .acceptanceEdge = acceptance,
          .route = static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1Route::NaturalOrder),
          .firstPlacement = command, .placementCount = commandCount, .executedCount = commandCount});
    }
    identity.attempts.push_back({.eventOrdinal = ordinal, .rawOrdinal = raw,
        .attemptGeneration = thisGeneration, .beginEdge = begin, .terminalEdge = edge++, .effectEdge = effect,
        .identity = projection, .receipt = {.identity = projection, .queueSequence = raw,
            .kind = static_cast<std::uint32_t>(RenderTapeIdentityV4ReceiptKind::Compatibility), .commandCount = commandCount},
        .recordCount = admitted.envelope.recordCount,
        .outcome = static_cast<std::uint32_t>(RenderTapeIdentityV4AttemptOutcome::Committed), .stagedCommandCount = commandCount});
    identity.settlements.push_back({ordinal, raw, edge++, admitted.envelope.recordCount});
  }
  RenderTapeIdentityV4Status identityStatus{};
  out.identityBytes = buildRenderTapeIdentityV4(out.tape, out.replay.catalogue, identity, &identityStatus);
  check(identityStatus == RenderTapeIdentityV4Status::Valid, "late initial setup preserves genuine Clear-to-END source provenance");
  RenderTapeExecutionTopologyV1Status topologyStatus{};
  out.topologyBytes = buildRenderTapeExecutionTopologyV1(out.tape, out.replay.catalogue,
      out.identityBytes, topology, &topologyStatus);
  check(topologyStatus == RenderTapeExecutionTopologyV1Status::Valid &&
      buildCaptureSourceReplayPlanV4(out.tape, out.replay.catalogue, out.identityBytes,
          out.topologyBytes, out.source) == CaptureSourceReplayPlanV4Status::Valid,
      "authenticated exact V4 pair precedes setup admission");
  return out;
}

Status build(const Fixture& fixture, RenderTapeInitialSetupV4Plan& plan,
    RenderTapeInitialSetupV4Limits limits = {}) {
  return buildRenderTapeInitialSetupV4(fixture.source, fixture.replay,
      fixture.tape, fixture.prefix, plan, limits);
}

void admission() {
  auto fixture = makeFixture();
  RenderTapeInitialSetupV4Plan plan;
  const auto actionCount = fixture.source.actions().size();
  check(build(fixture, plan) == Status::Valid && plan.valid(), "full typed setup admission succeeds without effects");
  check(plan.entries().size() == 5u && plan.seeds().size() == 17u && plan.aliases().size() == 2u &&
      plan.totalSeedBytes() == 864u && plan.firstIntervalEvent() == fixture.prefix &&
      plan.sourcePlanDigest() == fixture.source.digest(), "all five families and canonical byte extents are owned");
  const std::array families{RenderTapeInitialSetupV4Family::Texture2D, RenderTapeInitialSetupV4Family::TextureCube,
      RenderTapeInitialSetupV4Family::TextureVolume, RenderTapeInitialSetupV4Family::VertexBuffer, RenderTapeInitialSetupV4Family::IndexBuffer};
  for (std::size_t index = 0u; index < families.size(); ++index) {
    const auto& owner = plan.entries()[index];
    check(owner.family == families[index] && owner.definitionEventIndex > fixture.clearEvent &&
        owner.descriptor.size() == owner.definition.descriptorBytes,
        "late logical definitions retain typed copied descriptors and exact original indexes");
    check(std::equal(owner.descriptor.begin(), owner.descriptor.end(),
        fixture.replay.definitions[owner.definitionIndex].descriptor.begin()), "descriptor is byte-exact owned metadata");
    for (std::uint32_t relative = 0u; relative < owner.seedCount; ++relative) {
      const auto& seed = plan.seeds()[owner.firstSeedIndex + relative];
      const auto& original = fixture.replay.mutations[seed.mutationIndex];
      check(seed.ownerIndex == index && seed.eventIndex == owner.definitionEventIndex + relative + 1u &&
          seed.mutation.digest == original.digest && seed.mutation.byteSize == original.byteSize &&
          renderTapeIdentityV3SameIdentity(seed.mutation.identity, original.identity),
          "original Upload identity/digest/extent/event/mutation indexes remain exact");
    }
  }
  for (const auto& alias : plan.aliases()) {
    const auto& owner = plan.entries()[alias.parentOwnerIndex];
    check(renderTapeIdentityV3SameIdentity(alias.parentIdentity, owner.definition.identity) &&
        alias.eventIndex > plan.seeds()[owner.firstSeedIndex + owner.seedCount - 1u].eventIndex,
        "aliases name only the completed logical seed block and exact activated parent generation");
  }
  check(fixture.source.actions().size() == actionCount && fixture.source.identity().initializers.empty() &&
      fixture.source.identity().sources[0].lifecycle.cutEventOrdinal == fixture.queryEvent + 1u &&
      fixture.source.identity().sources[0].lifecycle.cutKind == static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterRecord) &&
      fixture.source.identity().sources[0].lifecycle.cutRecord == 1u &&
      std::none_of(plan.seeds().begin(), plan.seeds().end(), [&](const auto& seed) { return seed.eventIndex == fixture.trueWriteEvent; }),
      "setup owner neither rewrites publication cuts/actions nor admits genuine later CpuUnlock/NoOverwrite");
  check(fixture.replay.mutations[fixture.replay.events[fixture.trueWriteEvent].index].bufferDisposition ==
      static_cast<std::uint32_t>(RenderTapeBufferMutationDisposition::NoOverwrite), "later write stays in immutable General stream");

  RenderTapeInitialSetupV4Plan exact;
  check(build(fixture, exact, {.maxObjects = 5u, .maxSeeds = 17u, .maxSeedBytes = 864u}) == Status::Valid,
      "exact object/subresource/resource-extent byte limits admit");
  for (const auto limits : {RenderTapeInitialSetupV4Limits{4u, 17u, 864u},
          RenderTapeInitialSetupV4Limits{5u, 16u, 864u}, RenderTapeInitialSetupV4Limits{5u, 17u, 863u}}) {
    const auto digest = exact.sourcePlanDigest();
    check(build(fixture, exact, limits) == Status::CapacityExceeded && exact.valid() &&
        exact.sourcePlanDigest() == digest && exact.entries().size() == 5u && exact.totalSeedBytes() == 864u,
        "one-over object/seed/byte admission rolls back the existing destination");
  }
  check(buildRenderTapeInitialSetupV4(fixture.source, fixture.replay, fixture.tape,
      fixture.prefix + 1u, exact) == Status::InvalidBoundary && exact.valid(),
      "caller-selected boundary cannot replace execution-derived snapshot prefix");

  auto moved = std::move(plan);
  check(moved.valid() && !plan.valid() && plan.entries().empty() && plan.seeds().empty() && plan.aliases().empty() &&
      plan.sourcePlanDigest() == RenderTapeDigest{} && plan.firstIntervalEvent() == noIndex && plan.totalSeedBytes() == 0u,
      "move construction removes all moved-from setup authority");
  plan = std::move(moved);
  check(plan.valid() && !moved.valid() && moved.firstIntervalEvent() == noIndex && moved.sourcePlanDigest() == RenderTapeDigest{},
      "move assignment also invalidates the old owner");
  const auto descriptor = plan.entries()[0].descriptor;
  const auto digest = plan.seeds()[0].mutation.digest;
  std::fill(fixture.tape.begin(), fixture.tape.end(), std::byte{0});
  for (auto& blob : fixture.blobs) std::fill(blob.begin(), blob.end(), std::byte{0});
  check(plan.entries()[0].descriptor == descriptor && plan.seeds()[0].mutation.digest == digest,
      "setup metadata retains no caller tape/blob borrow");

  auto empty = makeFixture(Variant::Empty);
  check(build(empty, exact, {0u, 0u, 0u}) == Status::Valid && exact.valid() && exact.entries().empty() &&
      exact.seeds().empty() && exact.aliases().empty() && !exact.totalSeedBytes(), "empty closure is valid under zero budgets");
  auto reversed = makeFixture(Variant::ReversedSubresources);
  check(build(reversed, exact) == Status::Valid && exact.seeds()[0].mutation.subresource == 1u,
      "exact complete seed block may preserve captured nonascending subresource order");
  auto produced = makeFixture(Variant::Produced);
  check(build(produced, exact) == Status::Valid && exact.entries().size() == 5u && exact.seeds().size() == 17u,
      "late ProducedByCapturedPass resource and its actual Clear remain outside initial setup");
}

void failClosed() {
  auto valid = makeFixture();
  RenderTapeInitialSetupV4Plan output;
  check(build(valid, output) == Status::Valid, "transactional failure target starts valid");
  for (const auto variant : {Variant::InterveningDefinition, Variant::InterveningCommand,
          Variant::InterveningControl, Variant::InterveningDestroy, Variant::InitialCpuUnlock,
          Variant::UnsupportedSurface, Variant::NoncanonicalBuffer}) {
    auto fixture = makeFixture(variant);
    const auto expected = variant == Variant::UnsupportedSurface ? Status::UnsupportedClosure : Status::IncompleteClosure;
    check(build(fixture, output) == expected && output.valid() && output.sourcePlanDigest() == valid.source.digest() &&
        output.entries().size() == 5u && output.seeds().size() == 17u && output.totalSeedBytes() == 864u,
        "authenticated unsupported/noncanonical/interrupted initial closure fails transactionally before effects");
  }
  CaptureSourceReplayPlanV4 invalid;
  check(buildRenderTapeInitialSetupV4(invalid, valid.replay, valid.tape, valid.prefix, output) == Status::InvalidBinding &&
      output.valid(), "invalid immutable owner cannot authorize setup");
  // Every changed supplied derived field must fail whole-plan authentication,
  // before a seed/alias predicate can grant any physical preparation authority.
  const auto seedEvent = valid.prefix + 2u;
  const auto seedIndex = valid.replay.events[seedEvent].index;
  for (std::uint32_t mutation = 0u; mutation < 12u; ++mutation) {
    auto forged = valid.replay;
    auto& seed = forged.mutations[seedIndex];
    if (mutation == 0u) forged.events.erase(forged.events.begin() + seedEvent);
    if (mutation == 1u) --seed.byteSize;
    if (mutation == 2u) seed.byteSize = 0u;
    if (mutation == 3u) seed.subresource = 1u;
    if (mutation == 4u) seed.subresource = UINT32_MAX;
    if (mutation == 5u) ++seed.identity.generation;
    if (mutation == 6u) seed.digest[0] ^= std::byte{1};
    if (mutation == 7u) seed.kind = static_cast<std::uint32_t>(RenderTapeMutationKind::CpuUnlock);
    if (mutation == 8u) seed.byteOffset = 1u;
    if (mutation == 9u) seed.bufferDisposition = static_cast<std::uint32_t>(RenderTapeBufferMutationDisposition::NoOverwrite);
    if (mutation == 10u) forged.mutations.erase(forged.mutations.begin() + seedIndex);
    if (mutation == 11u) forged.catalogue.blobs[0].verified = 0u;
    check(buildRenderTapeInitialSetupV4(valid.source, forged, valid.tape, valid.prefix, output) == Status::InvalidBinding &&
        output.sourcePlanDigest() == valid.source.digest() && output.seeds().size() == 17u,
        "missing/partial/duplicate/out-of-range/stale/foreign/write seed metadata cannot escape exact authentication");
  }
  for (std::uint32_t mutation = 0u; mutation < 4u; ++mutation) {
    auto forged = valid.replay;
    const auto alias = std::find_if(forged.definitions.begin(), forged.definitions.end(), [](const auto& definition) {
      return definition.fixed.identity.objectId == 1010u;
    });
    auto descriptor = std::vector<std::byte>(alias->descriptor.begin(), alias->descriptor.end());
    RenderTapeSurfaceDescriptorV2 fixed{};
    std::memcpy(&fixed, descriptor.data(), sizeof(fixed));
    if (mutation == 0u) ++fixed.parentTexture.generation;
    if (mutation == 1u) ++fixed.parentTexture.objectId;
    if (mutation == 2u) ++fixed.surface.width;
    if (mutation == 3u) alias->fixed.expectedContentCount = 1u;
    std::memcpy(descriptor.data(), &fixed, sizeof(fixed));
    alias->descriptor = descriptor;
    check(buildRenderTapeInitialSetupV4(valid.source, forged, valid.tape, valid.prefix, output) == Status::InvalidBinding &&
        output.sourcePlanDigest() == valid.source.digest(), "stale/wrong-parent/shape/independent-seed alias metadata fails before effects");
  }
}

void allocationOnly() {
  auto fixture = makeFixture(Variant::AllocationOnly);
  RenderTapeInitialSetupV4Plan plan;
  const auto actions = std::vector<CaptureSourceReplayPlanV4Action>(
      fixture.source.actions().begin(), fixture.source.actions().end());
  const RenderTapeInitialSetupV4Limits exact{.maxObjects = 1u, .maxSeeds = 0u,
      .maxSeedBytes = 0u, .maxAllocationBytes = 64u};
  check(build(fixture, plan, exact) == Status::Valid && plan.valid() &&
      plan.entries().size() == 1u && plan.aliases().size() == 1u && plan.seeds().empty() &&
      plan.totalSeedBytes() == 0u && plan.totalAllocationBytes() == 64u,
      "authenticated Clear-to-NULLRT-definition/alias-to-END admits allocation, never seed authority");
  const auto& entry = plan.entries()[0];
  check(entry.mode == Mode::AllocationOnly && entry.family == RenderTapeInitialSetupV4Family::Texture2D &&
      entry.seedCount == 0u && entry.firstSeedIndex == noIndex && entry.totalBytes == 0u &&
      entry.allocationBytes == 64u && entry.definition.expectedContentBytes == 0u &&
      entry.definition.expectedContentCount == 0u && entry.definitionEventIndex == fixture.clearEvent + 1u,
      "allocation-only metadata preserves zero promises and exposes no usable seed index");
  const auto& alias = plan.aliases()[0];
  check(alias.parentOwnerIndex == 0u && alias.eventIndex == entry.definitionEventIndex + 1u &&
      renderTapeIdentityV3SameIdentity(alias.parentIdentity, entry.definition.identity),
      "zero-seed alias admission uses original parent-definition boundary and exact generation without underflow");
  check(std::equal(actions.begin(), actions.end(), fixture.source.actions().begin()) &&
      fixture.source.identity().initializers.empty() &&
      fixture.source.identity().sources[0].lifecycle.cutEventOrdinal == fixture.queryEvent + 1u &&
      fixture.source.identity().sources[0].lifecycle.cutRecord == 1u,
      "allocation admission leaves exact Clear publication at END and immutable action cuts untouched");
  auto tooSmall = exact;
  tooSmall.maxAllocationBytes = 63u;
  check(build(fixture, plan, tooSmall) == Status::CapacityExceeded && plan.valid() &&
      plan.totalAllocationBytes() == 64u && plan.totalSeedBytes() == 0u && plan.entries().size() == 1u,
      "one-over independent allocation budget leaves admitted output unchanged");
  tooSmall.maxAllocationBytes = 0u;
  check(build(fixture, plan, tooSmall) == Status::CapacityExceeded && plan.totalAllocationBytes() == 64u,
      "zero allocation budget cannot borrow unused promised-seed budget");

  auto two = makeFixture(Variant::AllocationTwo);
  const RenderTapeInitialSetupV4Limits twoExact{2u, 0u, 0u, 128u};
  check(build(two, plan, twoExact) == Status::Valid && plan.entries().size() == 2u &&
      plan.aliases().size() == 2u && plan.seeds().empty() && plan.totalAllocationBytes() == 128u,
      "two independent descriptor-only owners count both physical extents");
  auto twoSmall = twoExact;
  twoSmall.maxAllocationBytes = 127u;
  check(build(two, plan, twoSmall) == Status::CapacityExceeded && plan.totalAllocationBytes() == 128u &&
      plan.sourcePlanDigest() == two.source.digest(), "aggregate allocation footprint is bounded transactionally");
  auto extraAlias = makeFixture(Variant::AllocationExtraAlias);
  check(build(extraAlias, plan, exact) == Status::CapacityExceeded && plan.totalAllocationBytes() == 128u,
      "related alias metadata retains its independent object-count budget even for zero-seed owners");

  auto overflow = makeFixture(Variant::AllocationOverflow);
  auto unlimitedBytes = exact;
  unlimitedBytes.maxAllocationBytes = UINT64_MAX;
  check(build(overflow, plan, unlimitedBytes) == Status::CapacityExceeded && plan.totalAllocationBytes() == 128u,
      "canonical UINT32_MAX-square NULLRT allocation rejects width-times-height-times-four overflow");
  auto generationReuse = makeFixture(Variant::AllocationGenerationReuse);
  check(build(generationReuse, plan) == Status::UnsupportedClosure && plan.totalAllocationBytes() == 128u,
      "authenticated destroy/reuse of one NULLRT slot with a new generation cannot precreate conflicting hidden lifetimes");
  auto mixed = makeFixture(Variant::AllocationWithSeeds);
  check(build(mixed, plan, {6u, 17u, 864u, 64u}) == Status::Valid && plan.entries().size() == 6u &&
      plan.seeds().size() == 17u && plan.totalSeedBytes() == 864u && plan.totalAllocationBytes() == 64u,
      "allocation-only footprint and full seeded resource extents have independent exact budgets");
  for (const auto& owner : plan.entries()) {
    check(owner.mode == Mode::AllocationOnly
        ? owner.seedCount == 0u && owner.firstSeedIndex == noIndex && owner.totalBytes == 0u && owner.allocationBytes == 64u
        : owner.seedCount != 0u && owner.firstSeedIndex < plan.seeds().size() && owner.totalBytes != 0u && owner.allocationBytes == 0u,
        "mixed owners keep allocation and content authority disjoint");
  }
  check(build(mixed, plan, {6u, 17u, 863u, 64u}) == Status::CapacityExceeded &&
      build(mixed, plan, {6u, 17u, 864u, 63u}) == Status::CapacityExceeded &&
      plan.totalSeedBytes() == 864u && plan.totalAllocationBytes() == 64u,
      "unused budget in one mode never compensates a one-over extent in the other");

  auto produced = makeFixture(Variant::ProducedTexture);
  check(build(produced, plan, {5u, 17u, 864u, 0u}) == Status::Valid && plan.entries().size() == 5u &&
      plan.totalAllocationBytes() == 0u && plan.seeds().size() == 17u &&
      std::none_of(plan.entries().begin(), plan.entries().end(), [](const auto& owner) {
        return owner.definition.identity.objectId == 1022u;
      }), "genuine late ProducedByCapturedPass texture and its alias/Clear stay in the interval, not allocation setup");

  check(build(fixture, plan, exact) == Status::Valid, "allocation output restored for strict authentication failures");
  for (std::uint32_t mutation = 0u; mutation < 8u; ++mutation) {
    auto forged = fixture.replay;
    const auto definitionIndex = plan.entries()[0].definitionIndex;
    const auto aliasIndex = plan.aliases()[0].definitionIndex;
    auto& definition = forged.definitions[definitionIndex];
    auto descriptor = std::vector<std::byte>(definition.descriptor.begin(), definition.descriptor.end());
    RenderTapeTextureDescriptorV2 fixed{};
    std::memcpy(&fixed, descriptor.data(), sizeof(fixed));
    if (mutation == 0u) definition.fixed.expectedContentCount = 1u;
    if (mutation == 1u) definition.fixed.expectedContentBytes = 64u;
    if (mutation == 2u) fixed.initialContentDisposition = static_cast<std::uint32_t>(RenderTapeInitialContentDisposition::ProducedByCapturedPass);
    if (mutation == 3u) fixed.initialContentDisposition = static_cast<std::uint32_t>(RenderTapeInitialContentDisposition::CompleteSeed);
    if (mutation == 4u) ++definition.fixed.identity.generation;
    if (mutation == 5u) forged.definitions[aliasIndex].fixed.expectedContentCount = 1u;
    std::memcpy(descriptor.data(), &fixed, sizeof(fixed));
    definition.descriptor = descriptor;
    auto aliasDescriptor = std::vector<std::byte>(forged.definitions[aliasIndex].descriptor.begin(),
        forged.definitions[aliasIndex].descriptor.end());
    RenderTapeSurfaceDescriptorV2 aliasFixed{};
    std::memcpy(&aliasFixed, aliasDescriptor.data(), sizeof(aliasFixed));
    if (mutation == 6u) ++aliasFixed.parentTexture.generation;
    if (mutation == 7u) ++aliasFixed.surface.width;
    std::memcpy(aliasDescriptor.data(), &aliasFixed, sizeof(aliasFixed));
    forged.definitions[aliasIndex].descriptor = aliasDescriptor;
    check(buildRenderTapeInitialSetupV4(fixture.source, forged, fixture.tape, fixture.prefix, plan, exact) ==
        Status::InvalidBinding && plan.totalAllocationBytes() == 64u && plan.totalSeedBytes() == 0u &&
        plan.sourcePlanDigest() == fixture.source.digest(),
        "fake content promises, wrong/Produced disposition and stale generation/alias shape cannot forge allocation authority");
  }
  auto moved = std::move(plan);
  check(moved.valid() && moved.totalAllocationBytes() == 64u && !plan.valid() &&
      plan.totalAllocationBytes() == 0u && plan.totalSeedBytes() == 0u && plan.firstIntervalEvent() == noIndex,
      "allocation-only move construction removes moved-from allocation/seed/boundary authority");
  plan = std::move(moved);
  check(plan.valid() && plan.totalAllocationBytes() == 64u && !moved.valid() && moved.totalAllocationBytes() == 0u,
      "allocation-only move assignment invalidates old scalar authority too");
}

void requestedProducedAllocation() {
  auto fixture = makeFixture(Variant::ProducedTexture);
  RenderTapeInitialSetupV4Plan plan;
  constexpr D9CWireObjectIdentity parent{D9C_CHUNK_HANDLE_KIND_TEXTURE, 11u, 1022u};
  const std::array requested{parent};
  const RenderTapeInitialSetupV4Limits limits{6u, 17u, 864u, 1024u};
  const auto actions = std::vector<CaptureSourceReplayPlanV4Action>(
      fixture.source.actions().begin(), fixture.source.actions().end());
  const auto admit = [&](std::span<const D9CWireObjectIdentity> requests,
                         RenderTapeInitialSetupV4Limits budget = {6u, 17u, 864u, 1024u}) {
    return buildRenderTapeInitialSetupV4(fixture.source, fixture.replay, fixture.tape,
        fixture.prefix, plan, budget, requests);
  };
  const auto admitted = admit(requested, limits);
  if (admitted != Status::Valid) std::cerr << "produced allocation status=" << unsigned(admitted) << '\n';
  check(admitted == Status::Valid && plan.entries().size() == 6u &&
      plan.seeds().size() == 17u && plan.aliases().size() == 3u &&
      plan.totalSeedBytes() == 864u && plan.totalAllocationBytes() == 1024u,
      "explicit exact produced parent adds allocation and alias metadata, not a seed");
  const auto owner = std::find_if(plan.entries().begin(), plan.entries().end(),
      [&](const auto& entry) { return renderTapeIdentityV3SameIdentity(entry.definition.identity, parent); });
  check(owner != plan.entries().end() && owner->mode == Mode::AllocationOnly &&
      owner->seedCount == 0u && owner->firstSeedIndex == noIndex && owner->totalBytes == 0u &&
      owner->allocationBytes == 1024u && owner->definition.expectedContentCount == 0u &&
      owner->definition.expectedContentBytes == 0u,
      "produced physical preparation carries no initial content promise");
  check(std::equal(actions.begin(), actions.end(), fixture.source.actions().begin()) &&
      plan.firstIntervalEvent() == fixture.prefix,
      "request does not move the defining clear, publication cuts or semantic prefix");
  const auto unchanged = [&] {
    return plan.valid() && plan.entries().size() == 6u && plan.aliases().size() == 3u &&
        plan.totalAllocationBytes() == 1024u && plan.sourcePlanDigest() == fixture.source.digest();
  };
  for (const auto invalid : std::array{
      D9CWireObjectIdentity{parent.kind, parent.generation + 1u, parent.objectId},
      D9CWireObjectIdentity{parent.kind, parent.generation, 999999u},
      D9CWireObjectIdentity{D9C_CHUNK_HANDLE_KIND_SURFACE, parent.generation, parent.objectId},
      D9CWireObjectIdentity{parent.kind, 0u, parent.objectId}}) {
    const std::array request{invalid};
    check(admit(request) == Status::InvalidBinding && unchanged(),
        "stale/absent/wrong-kind/zero-generation request rejects without partial closure");
  }
  const std::array duplicate{parent, parent};
  check(admit(duplicate) == Status::InvalidBinding && unchanged(), "duplicate allocation requests reject");
  const std::array conflicting{parent,
      D9CWireObjectIdentity{parent.kind, parent.generation + 1u, parent.objectId}};
  check(admit(conflicting) == Status::InvalidBinding && unchanged(),
      "two generations of one requested slot cannot be prestaged together");
  const std::array seeded{D9CWireObjectIdentity{parent.kind, 11u, 1000u}};
  check(admit(seeded) == Status::UnsupportedClosure && unchanged(),
      "a seeded owner cannot be downgraded to allocation-only");
  check(admit(requested, {6u, 17u, 864u, 1023u}) == Status::CapacityExceeded && unchanged(),
      "produced allocation charges the independent bounded physical budget");
  check(admit(requested, {5u, 17u, 864u, 1024u}) == Status::CapacityExceeded && unchanged(),
      "produced allocation counts against the owner budget");
  auto forged = fixture.replay;
  ++forged.definitions[owner->definitionIndex].fixed.identity.generation;
  check(buildRenderTapeInitialSetupV4(fixture.source, forged, fixture.tape,
      fixture.prefix, plan, limits, requested) == Status::InvalidBinding && unchanged(),
      "requested allocation does not bypass whole-input authentication");
  check(admit({}) == Status::Valid && plan.entries().size() == 5u &&
      plan.totalAllocationBytes() == 0u && plan.aliases().size() == 2u,
      "unchanged default leaves produced allocation in the original interval");
  auto depthFixture = makeFixture(Variant::ProducedDepthTexture);
  check(buildRenderTapeInitialSetupV4(depthFixture.source, depthFixture.replay, depthFixture.tape,
      depthFixture.prefix, plan, limits, requested) == Status::Valid &&
      plan.entries().size() == 6u && plan.totalAllocationBytes() == 1024u &&
      plan.seeds().size() == 17u && plan.aliases().size() == 3u,
      "DF24 parent is allocation-only; actual Z-only defining clear stays in the admitted stream");
  auto fpFixture = makeFixture(Variant::ProducedFpTexture);
  check(buildRenderTapeInitialSetupV4(fpFixture.source, fpFixture.replay, fpFixture.tape,
      fpFixture.prefix, plan, limits, requested) == Status::UnsupportedClosure &&
      plan.sourcePlanDigest() == depthFixture.source.digest() && plan.totalAllocationBytes() == 1024u,
      "unsupported wider FP backing cannot be charged as a four-byte allocation");
}

void prefixGrammar() {
  // Direct shared-prefix tests bind the precise execution grammar, separately
  // from authenticated builder tests that reject an altered General plan.
  GeneralPreflightPlan replay;
  const D9CWireObjectIdentity identity{D9C_CHUNK_HANDLE_KIND_BUFFER, 3u, 40u};
  replay.definitions.push_back({.fixed = {.identity = identity, .expectedContentBytes = 8u, .expectedContentCount = 1u}});
  replay.mutations.push_back({.identity = identity, .kind = static_cast<std::uint32_t>(RenderTapeMutationKind::Upload), .byteSize = 8u});
  replay.events = {{RenderTapeEventType::BootstrapState, 0u}, {RenderTapeEventType::ObjectDefine, 1u},
      {RenderTapeEventType::ResourceMutation, 2u}, {RenderTapeEventType::CommandChunk, 3u}};
  std::uint32_t prefix = 777u;
  check(renderTapeInitialSetupV4SnapshotPrefix(replay, prefix) && prefix == 3u, "shared setup prefix completes exact promised resource");
  for (std::uint32_t mutation = 0u; mutation < 6u; ++mutation) {
    auto bad = replay;
    if (mutation == 0u) ++bad.mutations[0].identity.generation;
    if (mutation == 1u) bad.mutations[0].byteOffset = 1u;
    if (mutation == 2u) bad.mutations[0].byteSize = 9u;
    if (mutation == 3u) bad.mutations[0].byteSize = 7u;
    if (mutation == 4u) bad.events[2].eventIndex = 9u;
    if (mutation == 5u) bad.events.pop_back();
    prefix = 777u;
    check(!renderTapeInitialSetupV4SnapshotPrefix(bad, prefix) && prefix == 777u,
        "stale-generation/incomplete/overflow/ordinal/terminal prefix rejects without changing output");
  }
  replay.events.insert(replay.events.begin() + 3u, {RenderTapeEventType::ResourceMutation, 3u});
  replay.events[4].eventIndex = 4u;
  check(renderTapeInitialSetupV4SnapshotPrefix(replay, prefix) && prefix == 3u,
      "second mutation of a completed seed is the unchanged execution interval boundary");
}

bool sameState(const State& a, const State& b) {
  return a.phase == b.phase && a.seedCount == b.seedCount && a.consumedSeeds == b.consumedSeeds &&
      a.mode == b.mode;
}

void lifecycle() {
  for (std::uint32_t mode = 0u; mode <= 2u; ++mode) {
    for (std::uint32_t count = 0u; count <= 3u; ++count) {
      for (std::uint32_t consumed = 0u; consumed <= 4u; ++consumed) {
        for (std::uint32_t phase = 0u; phase <= 4u; ++phase) {
          for (std::uint32_t action = 0u; action <= 4u; ++action) {
            for (const auto relative : {noIndex, 0u, 1u, 2u, 3u, 4u}) {
              State state{static_cast<Phase>(phase), count, consumed, static_cast<Mode>(mode)};
              const auto before = state;
              // Exhaustive truth table uses the real shared transition; it does
              // not provide a test-only lifecycle implementation to the provider.
              const bool validCount = mode == 0u ? count && consumed <= count
                  : mode == 1u && !count && !consumed;
              const bool noSeedIndex = relative == noIndex;
              const std::array accepted{
                  validCount && phase == 0u && !consumed && noSeedIndex,
                  validCount && phase == 1u && !consumed && noSeedIndex,
                  validCount && mode == 0u && phase == 2u && consumed < count && relative == consumed,
                  validCount && (phase == 1u || phase == 2u) && noSeedIndex,
                  false};
              const bool result = renderTapeInitialSetupV4Advance(state, static_cast<Action>(action), relative);
              check(result == accepted[action], "bounded explicit-mode shared ownership transition truth table");
              if (!result) check(sameState(state, before), "every rejected transition is value-transactional");
              else if (action == 2u) check(state.consumedSeeds == consumed + 1u && state.phase == before.phase && state.mode == before.mode,
                  "one admitted original seed consumes exactly once without changing owner mode");
              else check(state.consumedSeeds == consumed && state.mode == before.mode && state.phase ==
                  (action == 0u ? Phase::Hidden : action == 1u ? Phase::Active : Phase::Released),
                  "prepare/activate/release change only ownership phase in either explicit mode");
            }
          }
        }
      }
    }
  }
  State overflow{Phase::Active, UINT32_MAX, UINT32_MAX};
  check(!renderTapeInitialSetupV4Advance(overflow, Action::ConsumeSeed, UINT32_MAX),
      "full uint32 seed counter cannot wrap");

  State allocation{Phase::Unprepared, 0u, 0u, Mode::AllocationOnly};
  check(renderTapeInitialSetupV4Advance(allocation, Action::Prepare) && allocation.phase == Phase::Hidden &&
      renderTapeInitialSetupV4Advance(allocation, Action::Activate) && allocation.phase == Phase::Active,
      "explicit allocation-only original definition activates acquired zero-seed owner once");
  for (const auto relative : {0u, 1u, noIndex}) {
    const auto before = allocation;
    check(!renderTapeInitialSetupV4Advance(allocation, Action::ConsumeSeed, relative) && sameState(allocation, before),
        "allocation-only owner can never consume an Upload or acquire fake content authority");
  }
  check(renderTapeInitialSetupV4Advance(allocation, Action::Release) &&
      !renderTapeInitialSetupV4Advance(allocation, Action::Release), "active zero-seed owner releases exactly once");

  for (const bool allocationFirst : {false, true}) {
    for (std::uint32_t prepared = 0u; prepared <= 2u; ++prepared) {
      for (std::uint32_t activated = 0u; activated <= prepared; ++activated) {
        for (std::uint32_t consumed = 0u; consumed <= 2u; ++consumed) {
          std::array<State, 2u> owners{
              State{Phase::Unprepared, 2u, 0u, Mode::Seeded},
              State{Phase::Unprepared, 0u, 0u, Mode::AllocationOnly}};
          if (allocationFirst) std::swap(owners[0], owners[1]);
          const auto seedOwner = allocationFirst ? 1u : 0u;
          std::uint32_t acquisitions = 0u, releases = 0u;
          for (std::uint32_t index = 0u; index < prepared; ++index) {
            check(renderTapeInitialSetupV4Advance(owners[index], Action::Prepare), "mixed-mode acquired owner prepares at real allocation boundary");
            ++acquisitions;
          }
          for (std::uint32_t index = 0u; index < activated; ++index)
            check(renderTapeInitialSetupV4Advance(owners[index], Action::Activate), "mixed-mode logical activation keeps exact acquisition");
          if (seedOwner < activated) {
            for (std::uint32_t index = 0u; index < consumed; ++index)
              check(renderTapeInitialSetupV4Advance(owners[seedOwner], Action::ConsumeSeed, index), "mixed-mode failure prefix can consume only actual seeded owner");
          }
          for (std::uint32_t index = 0u; index < owners.size(); ++index) {
            const auto before = owners[index];
            if (index < prepared) {
              check(renderTapeInitialSetupV4Advance(owners[index], Action::Release), "mixed-mode fault cleanup releases hidden/active zero-or-positive-seed owner");
              ++releases;
              check(!renderTapeInitialSetupV4Advance(owners[index], Action::Release), "mixed-mode duplicate cleanup cannot count release twice");
            } else check(!renderTapeInitialSetupV4Advance(owners[index], Action::Release) && sameState(owners[index], before),
                "mixed-mode allocation failure before acquisition has no release authority");
          }
          check(acquisitions == releases, "every zero-seed/seeded failure boundary balances exact owned references");
        }
      }
    }
  }

  // Fault boundaries cover every partially prepared, activated, and consumed
  // prefix of two owners. Release accounting is bound to the shared predicate,
  // not GPU readiness: Hidden alone never establishes checkpoint completion.
  for (std::uint32_t prepared = 0u; prepared <= 2u; ++prepared) {
    for (std::uint32_t activated = 0u; activated <= prepared; ++activated) {
      for (std::uint32_t consumed = 0u; consumed <= activated * 2u; ++consumed) {
        std::array<State, 2u> owners{State{Phase::Unprepared, 2u, 0u}, State{Phase::Unprepared, 2u, 0u}};
        std::array<bool, 2u> acquired{};
        std::uint32_t acquisitionCount = 0u, releaseCount = 0u;
        for (std::uint32_t index = 0u; index < prepared; ++index) {
          acquired[index] = true;
          ++acquisitionCount;
          check(renderTapeInitialSetupV4Advance(owners[index], Action::Prepare), "physical acquisition owns hidden setup exactly once");
          check(!renderTapeInitialSetupV4Advance(owners[index], Action::Prepare), "duplicate prepare cannot acquire/count again");
        }
        for (std::uint32_t index = 0u; index < activated; ++index) {
          check(renderTapeInitialSetupV4Advance(owners[index], Action::Activate), "original ObjectDefine activates hidden owner once");
          check(!renderTapeInitialSetupV4Advance(owners[index], Action::Activate), "duplicate logical definition cannot reactivate");
        }
        for (std::uint32_t seed = 0u; seed < consumed; ++seed) {
          auto& owner = owners[seed / 2u];
          check(renderTapeInitialSetupV4Advance(owner, Action::ConsumeSeed, seed % 2u), "original admitted Upload consumes captured seed once");
          check(!renderTapeInitialSetupV4Advance(owner, Action::ConsumeSeed, seed % 2u), "duplicate original Upload cannot consume/count twice");
        }
        for (std::size_t index = 0u; index < owners.size(); ++index) {
          if (!acquired[index]) {
            check(!renderTapeInitialSetupV4Advance(owners[index], Action::Release), "failed/unattempted creation carries no acquired reference");
            continue;
          }
          check(renderTapeInitialSetupV4Advance(owners[index], Action::Release), "fault cleanup releases every hidden or partially active reference");
          acquired[index] = false;
          ++releaseCount;
          check(!renderTapeInitialSetupV4Advance(owners[index], Action::Release), "second cleanup cannot release/count a reference twice");
        }
        check(releaseCount == acquisitionCount && std::none_of(acquired.begin(), acquired.end(), [](bool value) { return value; }),
            "every failure chronology has balanced exact acquisition/release accounting");
      }
    }
  }
}

} // namespace

int main() {
  try {
    admission();
    failClosed();
    allocationOnly();
    requestedProducedAllocation();
    prefixGrammar();
    lifecycle();
    std::cout << "render tape initial setup v4: " << checks << " checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "render tape initial setup v4 failed after " << checks << " checks: " << error.what() << '\n';
    return 1;
  }
}
