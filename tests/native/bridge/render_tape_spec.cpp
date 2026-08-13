#include "device_c_render_tape.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace dxmt9::d3d9;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

std::size_t alignUp(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

std::vector<std::byte> makeSingleRecordChunk(
    std::uint32_t type, std::span<const std::byte> payload,
    std::span<const D9CCommandChunkWireHandleEntry> handles = {}) {
  const auto recordTableOffset = sizeof(D9CCommandChunkWireHeader);
  const auto handleTableOffset = alignUp(
      recordTableOffset + sizeof(D9CCommandChunkWireRecordHeader),
      alignof(D9CCommandChunkWireHandleEntry));
  const auto payloadArenaOffset =
      alignUp(handleTableOffset +
                  handles.size() * sizeof(D9CCommandChunkWireHandleEntry),
              alignof(std::uint32_t));
  const D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = static_cast<std::uint32_t>(recordTableOffset),
      .recordCount = 1u,
      .handleTableOffset = static_cast<std::uint32_t>(handleTableOffset),
      .handleCount = static_cast<std::uint32_t>(handles.size()),
      .payloadArenaOffset = static_cast<std::uint32_t>(payloadArenaOffset),
      .payloadArenaSize = static_cast<std::uint32_t>(payload.size()),
  };
  const D9CCommandChunkWireRecordHeader record{
      .type = type,
      .payloadSize = static_cast<std::uint32_t>(payload.size()),
      .handleCount = static_cast<std::uint32_t>(handles.size()),
  };
  std::vector<std::byte> bytes(payloadArenaOffset + payload.size());
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + recordTableOffset, &record, sizeof(record));
  if (!handles.empty()) {
    std::memcpy(bytes.data() + handleTableOffset, handles.data(),
                handles.size_bytes());
  }
  if (!payload.empty()) {
    std::memcpy(bytes.data() + payloadArenaOffset, payload.data(),
                payload.size());
  }
  return bytes;
}

std::vector<std::byte> makeApplyStateChunk(
    bool referenceHandle = false,
    D9CWireObjectIdentity referencedIdentity = {}) {
  std::array<D9CCommandChunkWireTextureBinding,
             D9C_DRAW_PACKET_MAX_TEXTURES>
      textures{};
  for (std::uint32_t slot = 0u; slot < textures.size(); ++slot) {
    textures[slot] = D9CCommandChunkWireTextureBinding{
        .slot = slot,
        .valid = 1u,
        .handleIndex = referenceHandle && slot == 0u
                           ? 0u
                           : D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
    };
  }
  std::array<D9CCommandChunkWireStreamBinding, D9C_DRAW_PACKET_MAX_STREAMS>
      streams{};
  for (std::uint32_t slot = 0u; slot < streams.size(); ++slot) {
    streams[slot] = D9CCommandChunkWireStreamBinding{
        .slot = slot,
        .valid = 1u,
        .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
    };
  }

  constexpr std::uint32_t sectionCount = 2u;
  constexpr std::size_t textureBytes =
      sizeof(D9CCommandChunkWireTextureBinding) *
      D9C_DRAW_PACKET_MAX_TEXTURES;
  constexpr std::size_t streamBytes =
      sizeof(D9CCommandChunkWireStreamBinding) * D9C_DRAW_PACKET_MAX_STREAMS;
  const auto sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader);
  const auto sectionPayloadOffset =
      alignUp(sectionTableOffset +
                  sectionCount * sizeof(D9CCommandChunkWireSectionDesc),
              alignof(std::uint32_t));
  const auto streamOffset =
      sectionPayloadOffset + textureBytes;
  const D9CCommandChunkWireDrawHeader apply{
      .flags = D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT,
      .sectionCount = sectionCount,
      .sectionTableOffset = static_cast<std::uint32_t>(sectionTableOffset),
      .sectionPayloadOffset = static_cast<std::uint32_t>(sectionPayloadOffset),
  };
  const std::array sections{
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_TEXTURE,
          .elementSize = sizeof(D9CCommandChunkWireTextureBinding),
          .count = static_cast<std::uint32_t>(textures.size()),
          .payloadOffset = static_cast<std::uint32_t>(sectionPayloadOffset),
          .byteSize = static_cast<std::uint32_t>(textureBytes),
      },
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_STREAM,
          .elementSize = sizeof(D9CCommandChunkWireStreamBinding),
          .count = static_cast<std::uint32_t>(streams.size()),
          .payloadOffset = static_cast<std::uint32_t>(streamOffset),
          .byteSize = static_cast<std::uint32_t>(streamBytes),
      },
  };
  std::vector<std::byte> payload(streamOffset + streamBytes);
  std::memcpy(payload.data(), &apply, sizeof(apply));
  std::memcpy(payload.data() + sectionTableOffset, sections.data(),
              sizeof(sections));
  std::memcpy(payload.data() + sectionPayloadOffset, textures.data(),
              textureBytes);
  std::memcpy(payload.data() + streamOffset, streams.data(),
              streamBytes);
  const D9CCommandChunkWireHandleEntry handle{
      .kind = referencedIdentity.kind,
      .generation = referencedIdentity.generation,
      .objectId = referencedIdentity.objectId,
  };
  return makeSingleRecordChunk(
      D9C_COMMAND_RECORD_APPLY_STATE, payload,
      referenceHandle ? std::span<const D9CCommandChunkWireHandleEntry>(&handle,
                                                                         1u)
                      : std::span<const D9CCommandChunkWireHandleEntry>{});
}

std::vector<std::byte> makePresentChunk() {
  const D9CCommandChunkWirePresent present{};
  return makeSingleRecordChunk(D9C_COMMAND_RECORD_PRESENT,
                               std::as_bytes(std::span(&present, 1u)));
}

constexpr D9CWireObjectIdentity kSurface{
    .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
    .generation = 2u,
    .objectId = 17u,
};

constexpr D9CWireObjectIdentity kTexture{
    .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
    .generation = 3u,
    .objectId = 29u,
};

RenderTapeDigest digest(std::byte seed) {
  RenderTapeDigest value{};
  for (std::size_t i = 0u; i < value.size(); ++i) {
    value[i] = static_cast<std::byte>(
        static_cast<unsigned>(seed) + static_cast<unsigned>(i));
  }
  return value;
}

RenderTapeDigest mutationDigest() {
  return RenderTapeDigest{
      std::byte{0x8d}, std::byte{0x70}, std::byte{0xd6}, std::byte{0x91},
      std::byte{0xc8}, std::byte{0x22}, std::byte{0xd5}, std::byte{0x56},
      std::byte{0x38}, std::byte{0xb6}, std::byte{0xe7}, std::byte{0xfd},
      std::byte{0x54}, std::byte{0xcd}, std::byte{0x94}, std::byte{0x17},
      std::byte{0x0c}, std::byte{0x87}, std::byte{0xd1}, std::byte{0x9e},
      std::byte{0xb1}, std::byte{0xf6}, std::byte{0x28}, std::byte{0xb7},
      std::byte{0x57}, std::byte{0x50}, std::byte{0x6e}, std::byte{0xde},
      std::byte{0x56}, std::byte{0x88}, std::byte{0xd2}, std::byte{0x97},
  };
}

std::vector<std::byte> makeCompleteTape(
    D9CWireObjectIdentity mutationIdentity = kSurface,
    std::uint64_t mutationOffset = 0u, std::uint64_t mutationBytes = 4u,
    std::uint32_t controlBytes = sizeof(RenderTapeFlushWaitControl),
    std::uint64_t controlCompletion = 10u,
    std::uint64_t presentCompletion = 10u,
    RenderTapeDigestValidity digestValidity =
        RenderTapeDigestValidity::NotCaptured) {
  const auto bootstrap = makeApplyStateChunk();
  const auto present = makePresentChunk();
  constexpr std::array<std::byte, 8u> descriptor{};
  const auto resourceDigest = mutationDigest();
  const RenderTapeFlushWaitControl wait{.waitedSeqId = 9u};
  const RenderTapeOrderedControlHeader control{
      .kind = static_cast<std::uint32_t>(RenderTapeControlKind::FlushWait),
      .disposition = static_cast<std::uint32_t>(
          RenderTapeControlDisposition::Completed),
      .controlBytes = controlBytes,
      .completionOrdinal = controlCompletion,
  };
  const RenderTapeOracleAttachment oracle{
      .identity = kSurface,
      .descriptorKind = 1u,
  };

  RenderTapeBuilder builder;
  builder.appendBootstrapState(bootstrap);
  builder.appendObjectDefine(kSurface, 1u, descriptor, 0u, {});
  builder.appendResourceMutation(mutationIdentity,
                                 RenderTapeMutationKind::CpuUnlock, 0u,
                                 mutationOffset, mutationBytes,
                                 resourceDigest);
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u}, present);
  builder.appendOrderedControl(control, std::as_bytes(std::span(&wait, 1u)));
  builder.appendPresentComplete(
      4u, presentCompletion, digestValidity,
      digestValidity == RenderTapeDigestValidity::Sha256
          ? digest(std::byte{0x40})
          : RenderTapeDigest{},
      std::as_bytes(std::span(&oracle, 1u)));
  return builder.seal();
}

RenderTapeBlobCatalogue completeCatalogue(std::uint64_t bytes = 4u,
                                          bool verified = true) {
  return RenderTapeBlobCatalogue{.blobs = {{
                                      .digest = mutationDigest(),
                                      .size = bytes,
                                      .verified = verified ? 1u : 0u,
                                  }}};
}

template <typename T>
T* eventPayload(std::vector<std::byte>& tape, std::size_t eventIndex) {
  auto* header = reinterpret_cast<RenderTapeHeader*>(tape.data());
  auto* events = reinterpret_cast<RenderTapeEventHeader*>(
      tape.data() + header->eventTableOffset);
  return reinterpret_cast<T*>(tape.data() + header->payloadArenaOffset +
                              events[eventIndex].payloadOffset);
}

D9CCommandChunkWireDrawHeader* bootstrapDrawHeader(
    std::vector<std::byte>& tape) {
  auto* bootstrap = eventPayload<RenderTapeBootstrapHeader>(tape, 0u);
  auto* overlay = reinterpret_cast<std::byte*>(bootstrap + 1u);
  auto* chunk = reinterpret_cast<D9CCommandChunkWireHeader*>(overlay);
  return reinterpret_cast<D9CCommandChunkWireDrawHeader*>(
      overlay + chunk->payloadArenaOffset);
}

class RecordingSink final : public RenderTapeReplaySink {
public:
  bool bootstrap(const RenderTapeBootstrapHeader&,
                 std::span<const std::byte>,
                 RenderTapeBootstrapReplayMode mode) override {
    check(mode == RenderTapeBootstrapReplayMode::JournalOnlyDeferredProvider,
          "bootstrap must defer provider application explicitly");
    calls.push_back(RenderTapeEventType::BootstrapState);
    return true;
  }
  bool objectDefine(const RenderTapeObjectDefineHeader&,
                    std::span<const std::byte>) override {
    calls.push_back(RenderTapeEventType::ObjectDefine);
    return true;
  }
  bool objectDestroy(const RenderTapeObjectDestroyHeader&) override {
    calls.push_back(RenderTapeEventType::ObjectDestroy);
    return true;
  }
  bool resourceMutation(const RenderTapeResourceMutationHeader&) override {
    calls.push_back(RenderTapeEventType::ResourceMutation);
    return true;
  }
  bool commandChunk(const CommandChunkEnvelope& envelope,
                    std::span<const std::byte> chunk) override {
    calls.push_back(RenderTapeEventType::CommandChunk);
    return validateCommandChunk(chunk, envelope).valid();
  }
  bool orderedControl(const RenderTapeOrderedControlHeader&,
                      std::span<const std::byte>) override {
    calls.push_back(RenderTapeEventType::OrderedControl);
    return true;
  }
  bool presentComplete(const RenderTapePresentCompleteHeader&,
                       std::span<const std::byte>) override {
    calls.push_back(RenderTapeEventType::PresentComplete);
    return true;
  }

  std::vector<RenderTapeEventType> calls;
};

void validTapeReplaysExactlyOnce() {
  const auto tape = makeCompleteTape();
  const auto catalogue = completeCatalogue();
  ImportedRenderTapeView imported;
  const auto validation = validateRenderTape(tape, catalogue, &imported);
  check(validation.valid(), renderTapeValidationStatusName(validation.status));
  RecordingSink sink;
  const auto replay = replayPrevalidatedRenderTape(imported, catalogue, sink);
  check(replay.complete, "valid tape replay must complete");
  const std::array expected{
      RenderTapeEventType::BootstrapState,
      RenderTapeEventType::ObjectDefine,
      RenderTapeEventType::ResourceMutation,
      RenderTapeEventType::CommandChunk,
      RenderTapeEventType::OrderedControl,
      RenderTapeEventType::PresentComplete,
  };
  check(std::equal(sink.calls.begin(), sink.calls.end(), expected.begin(),
                   expected.end()),
        "events must replay exactly once in serial order");
}

void immutableObjectsAndRetirementFailClosed() {
  constexpr D9CWireObjectIdentity shader{
      .kind = D9C_CHUNK_HANDLE_KIND_SHADER,
      .generation = 1u,
      .objectId = 23u,
  };
  constexpr std::array<std::byte, 4u> descriptor{};
  const auto shaderDigest = digest(std::byte{0x70});
  const auto bootstrap = makeApplyStateChunk();
  const auto present = makePresentChunk();
  const RenderTapeOracleAttachment oracle{
      .identity = kSurface,
      .descriptorKind = 1u,
  };

  RenderTapeBuilder immutable;
  immutable.appendBootstrapState(bootstrap);
  immutable.appendObjectDefine(kSurface, 1u, descriptor, 0u, {});
  immutable.appendObjectDefine(shader, 2u, descriptor, 12u, shaderDigest);
  immutable.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u}, present);
  immutable.appendPresentComplete(
      4u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  const auto immutableTape = immutable.seal();
  const RenderTapeBlobCatalogue immutableCatalogue{.blobs = {{
      .digest = shaderDigest,
      .size = 12u,
      .verified = 1u,
  }}};
  check(validateRenderTape(immutableTape, immutableCatalogue).valid(),
        "verified shader payload definition must validate");
  check(validateRenderTape(immutableTape, {}).status ==
            RenderTapeValidationStatus::UnknownBlob,
        "shader payload definition without its blob must fail");

  RenderTapeBuilder reused;
  reused.appendBootstrapState(bootstrap);
  reused.appendObjectDefine(kSurface, 1u, descriptor, 0u, {});
  reused.appendObjectDestroy(kSurface);
  auto newerSurface = kSurface;
  ++newerSurface.generation;
  reused.appendObjectDefine(newerSurface, 1u, descriptor, 0u, {});
  reused.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u}, present);
  reused.appendPresentComplete(
      5u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  check(validateRenderTape(reused.seal(), {}).status ==
            RenderTapeValidationStatus::RetainedSlotReuse,
        "a retired slot cannot be reused while the tape retains it");

  RenderTapeBuilder mismatched;
  mismatched.appendBootstrapState(bootstrap);
  mismatched.appendObjectDefine(kSurface, 1u, descriptor, 0u, {});
  mismatched.appendObjectDefine(kSurface, 2u, descriptor, 0u, {});
  mismatched.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u}, present);
  mismatched.appendPresentComplete(
      4u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  check(validateRenderTape(mismatched.seal(), {}).status ==
            RenderTapeValidationStatus::DescriptorMismatch,
        "a reused identity with a different descriptor must fail closed");
}

void bootstrapAndCommandHandlesCloseBeforeReplay() {
  constexpr std::array<std::byte, 1u> descriptor{};
  const RenderTapeOracleAttachment oracle{
      .identity = kSurface,
      .descriptorKind = 1u,
  };

  RenderTapeBuilder deferred;
  deferred.appendBootstrapState(makeApplyStateChunk(true, kTexture));
  // The definition is intentionally journaled after BootstrapState. The
  // closure index makes this safe without applying the provider state early.
  deferred.appendObjectDefine(kSurface, 1u, descriptor, 0u, {});
  deferred.appendObjectDefine(kTexture, 1u, descriptor, 0u, {});
  deferred.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
      makePresentChunk());
  deferred.appendPresentComplete(
      4u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  ImportedRenderTapeView imported;
  const auto deferredValidation =
      validateRenderTape(deferred.seal(), {}, &imported);
  check(deferredValidation.valid(),
        std::string(renderTapeValidationStatusName(deferredValidation.status)) +
            " chunk=" +
            std::to_string(static_cast<std::uint32_t>(
                deferredValidation.chunkStatus)));

  auto stale = kTexture;
  ++stale.generation;
  RenderTapeBuilder missingBootstrapDefinition;
  missingBootstrapDefinition.appendBootstrapState(
      makeApplyStateChunk(true, stale));
  missingBootstrapDefinition.appendObjectDefine(kTexture, 1u, descriptor, 0u,
                                                {});
  missingBootstrapDefinition.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
      makePresentChunk());
  missingBootstrapDefinition.appendPresentComplete(
      3u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  check(validateRenderTape(missingBootstrapDefinition.seal(), {}).status ==
            RenderTapeValidationStatus::UnknownIdentity,
        "bootstrap stale handles must fail before callbacks");

  RenderTapeBuilder missingCommandDefinition;
  missingCommandDefinition.appendBootstrapState(makeApplyStateChunk());
  missingCommandDefinition.appendObjectDefine(kSurface, 1u, descriptor, 0u,
                                               {});
  missingCommandDefinition.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u},
      makeApplyStateChunk(true, stale));
  missingCommandDefinition.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
      makePresentChunk());
  missingCommandDefinition.appendPresentComplete(
      4u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  check(validateRenderTape(missingCommandDefinition.seal(), {}).status ==
            RenderTapeValidationStatus::UnknownIdentity,
        "later command handles must resolve exact generations");

  RenderTapeBuilder retiredCommandDefinition;
  retiredCommandDefinition.appendBootstrapState(makeApplyStateChunk());
  retiredCommandDefinition.appendObjectDefine(kTexture, 1u, descriptor, 0u,
                                               {});
  retiredCommandDefinition.appendObjectDestroy(kTexture);
  retiredCommandDefinition.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u},
      makeApplyStateChunk(true, kTexture));
  retiredCommandDefinition.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
      makePresentChunk());
  retiredCommandDefinition.appendPresentComplete(
      5u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  check(validateRenderTape(retiredCommandDefinition.seal(), {}).status ==
            RenderTapeValidationStatus::UnknownIdentity,
        "retired command handles must fail closed");
}

void seedContentClosesByUniqueSubresourceAndSummedBytes() {
  constexpr std::array<std::byte, 8u> descriptor{};
  const auto firstDigest = digest(std::byte{0x10});
  const auto secondDigest = digest(std::byte{0x30});
  const RenderTapeBlobCatalogue catalogue{.blobs = {
      RenderTapeBlob{.digest = firstDigest, .size = 3u, .verified = 1u},
      RenderTapeBlob{.digest = secondDigest, .size = 4u, .verified = 1u},
  }};
  const RenderTapeOracleAttachment oracle{
      .identity = kSurface,
      .descriptorKind = 1u,
  };

  const auto appendPrefix = [&](RenderTapeBuilder& builder) {
    builder.appendBootstrapState(makeApplyStateChunk());
    builder.appendObjectDefine(kSurface, 1u, descriptor, 0u, {});
    builder.appendObjectDefine(kTexture, 1u, descriptor, 0u, {}, 7u, 2u);
  };
  const auto appendSuffix = [&](RenderTapeBuilder& builder,
                                std::uint64_t presentOrdinal) {
    builder.appendCommandChunk(
        CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u},
        makePresentChunk());
    builder.appendPresentComplete(
        presentOrdinal, 1u, RenderTapeDigestValidity::NotCaptured, {},
        std::as_bytes(std::span(&oracle, 1u)));
  };

  RenderTapeBuilder complete;
  appendPrefix(complete);
  complete.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload,
                                  0u, 0u, 3u, firstDigest);
  complete.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload,
                                  1u, 0u, 4u, secondDigest);
  appendSuffix(complete, 6u);
  check(validateRenderTape(complete.seal(), catalogue).valid(),
        "unique multi-subresource seeds close by summed bytes and count");

  RenderTapeBuilder postArmMutation;
  appendPrefix(postArmMutation);
  postArmMutation.appendResourceMutation(
      kTexture, RenderTapeMutationKind::Upload, 0u, 0u, 3u, firstDigest);
  postArmMutation.appendResourceMutation(
      kTexture, RenderTapeMutationKind::Upload, 1u, 0u, 4u, secondDigest);
  // Once the declared checkpoint closes, a mutation in the live interval is
  // ordinary even when no CommandChunk has appeared yet. It may target a
  // seeded subresource again and need not match the aggregate seed extent.
  postArmMutation.appendResourceMutation(
      kTexture, RenderTapeMutationKind::CpuUnlock, 0u, 1u, 4u, secondDigest);
  appendSuffix(postArmMutation, 7u);
  check(validateRenderTape(postArmMutation.seal(), catalogue).valid(),
        "post-arm pre-command mutation is not reclassified as a seed");

  RenderTapeBuilder duplicate;
  appendPrefix(duplicate);
  duplicate.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload,
                                   0u, 0u, 3u, firstDigest);
  duplicate.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload,
                                   0u, 0u, 4u, secondDigest);
  appendSuffix(duplicate, 6u);
  check(validateRenderTape(duplicate.seal(), catalogue).status ==
            RenderTapeValidationStatus::InvalidMutationRange,
        "duplicate seed subresources fail closed even when totals match");

  RenderTapeBuilder incomplete;
  appendPrefix(incomplete);
  incomplete.appendResourceMutation(kTexture, RenderTapeMutationKind::Upload,
                                    0u, 0u, 3u, firstDigest);
  appendSuffix(incomplete, 5u);
  const auto incompleteResult =
      validateRenderTape(incomplete.seal(), catalogue);
  check(incompleteResult.status == RenderTapeValidationStatus::IncompleteFrame &&
            incompleteResult.failedEventIndex == 4u,
        "seed extents close before the first non-seed command event");
}

void invalidInputsFailBeforeCallbacks() {
  const auto validCatalogue = completeCatalogue();

  auto incomplete = makeCompleteTape();
  eventPayload<RenderTapeBootstrapHeader>(incomplete, 0u)
      ->requiredCategoryMask &= ~std::uint64_t{1};
  check(validateRenderTape(incomplete, validCatalogue).status ==
            RenderTapeValidationStatus::BootstrapCoverageIncomplete,
        "missing bootstrap category must fail");

  auto unknownBaseline = makeCompleteTape();
  eventPayload<RenderTapeBootstrapHeader>(unknownBaseline, 0u)
      ->baselineProfileVersion = kRenderTapeBaselineProfileVersion + 1u;
  check(validateRenderTape(unknownBaseline, validCatalogue).status ==
            RenderTapeValidationStatus::InvalidBootstrap,
        "unknown bootstrap baseline must fail");

  auto sparseBootstrap = makeCompleteTape();
  bootstrapDrawHeader(sparseBootstrap)->flags = 0u;
  check(validateRenderTape(sparseBootstrap, validCatalogue).status ==
            RenderTapeValidationStatus::InvalidBootstrapChunk,
        "bootstrap APPLY_STATE must retain FULL_SNAPSHOT semantics");

  RenderTapeBuilder forbidden;
  const auto present = makePresentChunk();
  forbidden.appendBootstrapState(present);
  constexpr std::array<std::byte, 1u> descriptor{};
  forbidden.appendObjectDefine(kSurface, 1u, descriptor, 0u, {});
  forbidden.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u}, present);
  const RenderTapeOracleAttachment oracle{.identity = kSurface,
                                          .descriptorKind = 1u};
  forbidden.appendPresentComplete(
      3u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  check(validateRenderTape(forbidden.seal(), {}).status ==
            RenderTapeValidationStatus::BootstrapForbiddenRecord,
        "bootstrap must reject Present records");

  const auto tape = makeCompleteTape();
  check(validateRenderTape(tape, {}).status ==
            RenderTapeValidationStatus::UnknownBlob,
        "missing mutation blob must fail");
  check(validateRenderTape(tape, completeCatalogue(5u)).status ==
            RenderTapeValidationStatus::BlobSizeMismatch,
        "blob size mismatch must fail");
  check(validateRenderTape(tape, completeCatalogue(4u, false)).status ==
            RenderTapeValidationStatus::BlobDigestMismatch,
        "unverified blob must fail");

  auto stale = kSurface;
  ++stale.generation;
  check(validateRenderTape(makeCompleteTape(stale), validCatalogue).status ==
            RenderTapeValidationStatus::UnknownIdentity,
        "stale mutation generation must fail");

  auto shaderDefineWithoutPayload = makeCompleteTape();
  eventPayload<RenderTapeObjectDefineHeader>(shaderDefineWithoutPayload, 1u)
      ->identity.kind = D9C_CHUNK_HANDLE_KIND_SHADER;
  check(validateRenderTape(shaderDefineWithoutPayload, validCatalogue).status ==
            RenderTapeValidationStatus::InvalidObjectDefine,
        "shader definitions must carry immutable payload bytes and digest");

  auto invalidMutationObject = kSurface;
  invalidMutationObject.kind = D9C_CHUNK_HANDLE_KIND_QUERY;
  check(validateRenderTape(makeCompleteTape(invalidMutationObject),
                           validCatalogue)
            .status == RenderTapeValidationStatus::InvalidMutationKind,
        "queries cannot be resource-mutation targets");
  check(validateRenderTape(
            makeCompleteTape(kSurface,
                             std::numeric_limits<std::uint64_t>::max(), 2u),
            completeCatalogue(2u))
            .status == RenderTapeValidationStatus::InvalidMutationRange,
        "overflowed mutation range must fail");

  check(validateRenderTape(
            makeCompleteTape(kSurface, 0u, 4u,
                             sizeof(RenderTapeFlushWaitControl) - 1u),
            validCatalogue)
            .status == RenderTapeValidationStatus::InvalidControlSize,
        "wrong ordered-control size must fail");

  auto invalidControlDisposition = makeCompleteTape();
  eventPayload<RenderTapeOrderedControlHeader>(invalidControlDisposition, 4u)
      ->disposition = static_cast<std::uint32_t>(
          RenderTapeControlDisposition::Terminal);
  check(validateRenderTape(invalidControlDisposition, validCatalogue).status ==
            RenderTapeValidationStatus::InvalidControlKind,
        "flush/wait cannot carry a terminal disposition");
  check(validateRenderTape(
            makeCompleteTape(kSurface, 0u, 4u,
                             sizeof(RenderTapeFlushWaitControl), 10u, 9u),
            validCatalogue)
            .status == RenderTapeValidationStatus::InvalidPresentComplete,
        "completion regression must fail");

  auto malformedPresent = makeCompleteTape();
  eventPayload<RenderTapePresentCompleteHeader>(malformedPresent, 5u)
      ->digestValidity = 99u;
  check(validateRenderTape(malformedPresent, validCatalogue).status ==
            RenderTapeValidationStatus::InvalidPresentComplete,
        "unknown Present digest validity must fail");

  auto missingOracle = makeCompleteTape();
  eventPayload<RenderTapePresentCompleteHeader>(missingOracle, 5u)
      ->oracleCount = 0u;
  check(validateRenderTape(missingOracle, validCatalogue).status ==
            RenderTapeValidationStatus::InvalidPresentComplete,
        "PresentComplete must identify at least one oracle attachment");

  auto mismatchedOracle = makeCompleteTape();
  auto* mismatchedAttachment = reinterpret_cast<RenderTapeOracleAttachment*>(
      eventPayload<RenderTapePresentCompleteHeader>(mismatchedOracle, 5u) +
      1u);
  ++mismatchedAttachment->descriptorKind;
  check(validateRenderTape(mismatchedOracle, validCatalogue).status ==
            RenderTapeValidationStatus::InvalidPresentComplete,
        "oracle attachment descriptor must match its object definition");

  auto trailingPayload = makeCompleteTape();
  trailingPayload.push_back(std::byte{});
  ++reinterpret_cast<RenderTapeHeader*>(trailingPayload.data())
        ->payloadArenaSize;
  check(validateRenderTape(trailingPayload, validCatalogue).status ==
            RenderTapeValidationStatus::NonCanonicalEventLayout,
        "unreferenced trailing payload bytes must fail");

  RecordingSink sink;
  ImportedRenderTapeView imported;
  const auto rejected = validateRenderTape(incomplete, validCatalogue, &imported);
  if (rejected.valid()) {
    static_cast<void>(replayPrevalidatedRenderTape(imported, validCatalogue,
                                                   sink));
  }
  check(sink.calls.empty(), "validation failure must produce zero callbacks");
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string_view(argv[1]) == "--write-fixture") {
      const auto tape = makeCompleteTape();
      std::ofstream output(argv[2], std::ios::binary);
      output.write(reinterpret_cast<const char*>(tape.data()),
                   static_cast<std::streamsize>(tape.size()));
      check(output.good(), "failed to write render tape fixture");
      return 0;
    }
    check(argc == 1, "usage: render_tape_spec [--write-fixture path]");
    validTapeReplaysExactlyOnce();
    immutableObjectsAndRetirementFailClosed();
    bootstrapAndCommandHandlesCloseBeforeReplay();
    seedContentClosesByUniqueSubresourceAndSummedBytes();
    invalidInputsFailBeforeCallbacks();
  } catch (const std::exception& error) {
    std::cerr << "render tape spec failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "render tape spec passed\n";
  return 0;
}
