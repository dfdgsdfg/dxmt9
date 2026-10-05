#include "device_c_render_tape_identity_transfer_v3.hpp"
#include "device_c_render_tape_descriptors.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace transfer_allocation_probe {
bool enabled = false;
std::size_t failAfter = 0u;
} // namespace transfer_allocation_probe

void* operator new(std::size_t size) {
  if (transfer_allocation_probe::enabled) {
    if (!transfer_allocation_probe::failAfter) {
      transfer_allocation_probe::enabled = false;
      throw std::bad_alloc();
    }
    --transfer_allocation_probe::failAfter;
  }
  if (auto* value = std::malloc(size)) return value;
  throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { ::operator delete(value); }
void operator delete(void* value, std::size_t) noexcept { ::operator delete(value); }
void operator delete[](void* value, std::size_t) noexcept { ::operator delete(value); }

namespace {

using namespace dxmt9::d3d9;
using Status = RenderTapeIdentityV3TransferStatus;
constexpr RenderTapeIdentityV3CaptureKey kKey{7u, 8u};
constexpr D9CWireObjectIdentity kQuery{D9C_CHUNK_HANDLE_KIND_QUERY, 3u, 42u};

void check(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}

RenderTapeIdentityV3View snapshotFixture() {
  RenderTapeIdentityV3View snapshot;
  snapshot.header.frameId = 9u;
  snapshot.header.presentOrdinal = 4u;
  snapshot.header.captureToken = kKey.token;
  snapshot.header.captureEpoch = kKey.epoch;
  snapshot.header.queue = 1u;
  snapshot.header.initializerEpoch = 1u;
  snapshot.header.baselineCompletionEdge = 1u;
  snapshot.header.baselineKind = static_cast<std::uint32_t>(
      RenderTapeIdentityV3PrefixKind::EmptyBaseline);
  // View vectors own the extents. Stale sidecar offsets/counts do not.
  snapshot.header.sources = {99u, 99u, 99u};
  snapshot.header.initializers = {99u, 99u, 99u};
  snapshot.header.commands = {99u, 99u, 99u};
  snapshot.header.dispositions = {99u, 99u, 99u};
  snapshot.header.coordinators = {99u, 99u, 99u};
  snapshot.header.settlements = {99u, 99u, 99u};
  snapshot.sources = {{.sourceOrdinal = 1u, .seqId = 1u,
      .writerCaptureId = 1u, .writerGeneration = 2u, .storageGeneration = 3u,
      .publicationEdge = 7u, .submissionEdge = 8u, .completionEdge = 9u,
      .completedSequence = 1u, .cutEventOrdinal = 3u, .commandCount = 1u,
      .cutKind = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterEvent)}};
  snapshot.initializers = {{.sequence = 1u, .epoch = 1u,
      .publicationEdge = 3u, .submissionEdge = 4u, .completionEdge = 5u,
      .completedSequence = 1u, .cutEventOrdinal = 3u,
      .cutKind = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterRecord),
      .cutRecord = 1u}};
  snapshot.commands = {{.sourceIndex = 0u,
      .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3CommandKind::Present),
      .membership = static_cast<std::uint32_t>(RenderTapeIdentityV3Membership::NoPassCommand)}};
  snapshot.dispositions = {
      {.eventOrdinal = 2u, .recordCount = 1u,
       .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::StateOnly)},
      {.eventOrdinal = 3u, .recordCount = 1u,
       .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::Coordinator),
       .coordinatorIndex = 0u},
      {.eventOrdinal = 3u, .firstRecord = 1u, .recordCount = 1u,
       .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::GpuCommand),
       .commandIndex = 0u},
  };
  snapshot.coordinators = {{.eventOrdinal = 3u, .rawOrdinal = 30u,
      .buildGeneration = 1u, .dispatchEdge = 6u,
      .releaseOrdinal = 1u, .releaseGeneration = 1u, .queryIdentity = kQuery,
      .issueCycle = 1u, .issueOrdinal = 1u, .deviceEpoch = 2u,
      .prefixInitializer = 1u, .prefixInitializerEpoch = 1u,
      .recordType = D9C_COMMAND_RECORD_QUERY_ISSUE, .flags = 1u, .queryType = 8u,
      .prefixKind = static_cast<std::uint32_t>(RenderTapeIdentityV3PrefixKind::EmptyBaseline),
      .initializerIndex = 0u,
      .observation = static_cast<std::uint32_t>(RenderTapeIdentityV3Observation::Completed)}};
  snapshot.settlements = {{2u, 20u, 1u, 2u, 1u}, {3u, 30u, 1u, 10u, 2u}};
  return snapshot;
}

RenderTapeIdentityV3Status validateFixtureRows(const RenderTapeIdentityV3View& snapshot) {
  const std::array events{
      RenderTapeIdentityV3EventFact{1u,
          static_cast<std::uint32_t>(RenderTapeEventType::BootstrapState), 0u},
      RenderTapeIdentityV3EventFact{2u,
          static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk), 1u},
      RenderTapeIdentityV3EventFact{3u,
          static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk), 2u},
      RenderTapeIdentityV3EventFact{4u,
          static_cast<std::uint32_t>(RenderTapeEventType::PresentComplete), 0u},
  };
  const std::array records{
      RenderTapeIdentityV3RecordFact{.eventOrdinal = 2u, .recordIndex = 0u,
          .recordType = D9C_COMMAND_RECORD_APPLY_STATE},
      RenderTapeIdentityV3RecordFact{.eventOrdinal = 3u, .recordIndex = 0u,
          .recordType = D9C_COMMAND_RECORD_QUERY_ISSUE, .queryFlags = 1u,
          .queryIdentity = kQuery, .queryType = 8u, .issueCycle = 1u, .issueOrdinal = 1u},
      RenderTapeIdentityV3RecordFact{.eventOrdinal = 3u, .recordIndex = 1u,
          .recordType = D9C_COMMAND_RECORD_PRESENT},
  };
  return validateRenderTapeIdentityV3Rows(snapshot, events, records);
}

template <typename T>
bool sameRows(const std::vector<T>& a, const std::vector<T>& b) {
  return a.size() == b.size() &&
      (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}

bool emptySnapshot(const RenderTapeIdentityV3View& snapshot) {
  return !snapshot.header.captureToken && !snapshot.header.captureEpoch &&
      snapshot.sources.empty() && snapshot.initializers.empty() &&
      snapshot.commands.empty() && snapshot.dispositions.empty() &&
      snapshot.coordinators.empty() && snapshot.settlements.empty();
}

std::vector<std::byte> singleRecordChunk(
    std::uint32_t type, std::span<const std::byte> payload) {
  const auto recordOffset = sizeof(D9CCommandChunkWireHeader);
  const auto payloadOffset = (recordOffset + sizeof(D9CCommandChunkWireRecordHeader) + 7u) & ~7u;
  const D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = static_cast<std::uint32_t>(recordOffset), .recordCount = 1u,
      .handleTableOffset = static_cast<std::uint32_t>(payloadOffset),
      .payloadArenaOffset = static_cast<std::uint32_t>(payloadOffset),
      .payloadArenaSize = static_cast<std::uint32_t>(payload.size())};
  const D9CCommandChunkWireRecordHeader record{
      .type = type, .payloadSize = static_cast<std::uint32_t>(payload.size())};
  std::vector<std::byte> bytes(payloadOffset + payload.size());
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + recordOffset, &record, sizeof(record));
  std::memcpy(bytes.data() + payloadOffset, payload.data(), payload.size());
  return bytes;
}

std::vector<std::byte> fullBootstrapChunk() {
  std::array<D9CCommandChunkWireTextureBinding, D9C_DRAW_PACKET_MAX_TEXTURES> textures{};
  std::array<D9CCommandChunkWireStreamBinding, D9C_DRAW_PACKET_MAX_STREAMS> streams{};
  for (std::uint32_t i = 0u; i < textures.size(); ++i)
    textures[i] = {.slot = i, .valid = 1u, .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX};
  for (std::uint32_t i = 0u; i < streams.size(); ++i)
    streams[i] = {.slot = i, .valid = 1u, .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX};
  const auto tableOffset = sizeof(D9CCommandChunkWireDrawHeader);
  const auto textureOffset = tableOffset + 2u * sizeof(D9CCommandChunkWireSectionDesc);
  const auto streamOffset = textureOffset + sizeof(textures);
  const D9CCommandChunkWireDrawHeader header{
      .flags = D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT, .sectionCount = 2u,
      .sectionTableOffset = static_cast<std::uint32_t>(tableOffset),
      .sectionPayloadOffset = static_cast<std::uint32_t>(textureOffset)};
  const std::array sections{
      D9CCommandChunkWireSectionDesc{.kind = D9C_COMMAND_CHUNK_SECTION_TEXTURE,
          .elementSize = sizeof(D9CCommandChunkWireTextureBinding),
          .count = D9C_DRAW_PACKET_MAX_TEXTURES,
          .payloadOffset = static_cast<std::uint32_t>(textureOffset), .byteSize = sizeof(textures)},
      D9CCommandChunkWireSectionDesc{.kind = D9C_COMMAND_CHUNK_SECTION_STREAM,
          .elementSize = sizeof(D9CCommandChunkWireStreamBinding),
          .count = D9C_DRAW_PACKET_MAX_STREAMS,
          .payloadOffset = static_cast<std::uint32_t>(streamOffset), .byteSize = sizeof(streams)}};
  std::vector<std::byte> payload(streamOffset + sizeof(streams));
  std::memcpy(payload.data(), &header, sizeof(header));
  std::memcpy(payload.data() + tableOffset, sections.data(), sizeof(sections));
  std::memcpy(payload.data() + textureOffset, textures.data(), sizeof(textures));
  std::memcpy(payload.data() + streamOffset, streams.data(), sizeof(streams));
  return singleRecordChunk(D9C_COMMAND_RECORD_APPLY_STATE, payload);
}

std::vector<std::byte> authenticatedSidecarFixture(std::vector<std::byte>& tape) {
  RenderTapeBuilder builder;
  builder.appendBootstrapState(fullBootstrapChunk());
  constexpr D9CWireObjectIdentity output{D9C_CHUNK_HANDLE_KIND_SURFACE, 1u, 51u};
  const RenderTapeSurfaceDescriptorV2 descriptor{
      .storage = static_cast<std::uint32_t>(RenderTapeSurfaceStorage::SwapchainBackbuffer),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::ProducedPresentOutput),
      .surface = {.format = 21u, .resourceType = 1u, .usage = 1u,
                  .width = 4u, .height = 4u, .depth = 1u}};
  builder.appendObjectDefine(output,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&descriptor, 1u)), 0u, {});
  const D9CCommandChunkWirePresent present{};
  builder.appendCommandChunk({.recordCount = 1u},
      singleRecordChunk(D9C_COMMAND_RECORD_PRESENT, std::as_bytes(std::span(&present, 1u))));
  const RenderTapeOracleAttachment oracle{.identity = output,
      .descriptorKind = static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface)};
  builder.appendPresentComplete(3u, 11u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  tape = builder.seal();
  auto snapshot = snapshotFixture();
  snapshot.header.presentOrdinal = 3u;
  snapshot.initializers.clear();
  snapshot.coordinators.clear();
  snapshot.dispositions = {{.eventOrdinal = 3u, .recordCount = 1u,
      .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::GpuCommand),
      .commandIndex = 0u}};
  snapshot.settlements = {{3u, 30u, 1u, 2u, 1u}};
  RenderTapeIdentityV3Status status{};
  auto bytes = buildRenderTapeIdentityV3(tape, {}, snapshot, &status);
  check(status == RenderTapeIdentityV3Status::Valid && !bytes.empty(),
        "sidecar isolation fixture authenticates a complete canonical tape");
  return bytes;
}

void roundTripAndAuthoritySeparation() {
  const auto snapshot = snapshotFixture();
  check(validateFixtureRows(snapshot) == RenderTapeIdentityV3Status::Valid,
        "snapshot fixture contains complete source/initializer/coordinator value evidence");
  std::vector<std::byte> bytes;
  check(encodeRenderTapeIdentityV3SnapshotTransfer(snapshot, bytes) == Status::Valid,
        "complete witnessed snapshot serializes with independent transfer grammar");
  RenderTapeIdentityV3TransferHeader header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  check(header.magic != kRenderTapeIdentityMagic && header.totalBytes == bytes.size() &&
            header.identity.sources.offset == sizeof(header) &&
            header.identity.sources.count == snapshot.sources.size() &&
            header.identity.sources.entrySize == sizeof(RenderTapeIdentityV3Source) &&
            !header.identity.eventsBytes && header.identity.eventsDigest == RenderTapeDigest{},
        "transfer derives canonical table layout and makes no tape-digest claim");
  RenderTapeIdentityV3View decoded;
  check(decodeRenderTapeIdentityV3SnapshotTransfer(bytes, kKey, decoded) == Status::Valid &&
            sameRows(snapshot.sources, decoded.sources) &&
            sameRows(snapshot.initializers, decoded.initializers) &&
            sameRows(snapshot.commands, decoded.commands) &&
            sameRows(snapshot.dispositions, decoded.dispositions) &&
            sameRows(snapshot.coordinators, decoded.coordinators) &&
            sameRows(snapshot.settlements, decoded.settlements) &&
            validateFixtureRows(decoded) == RenderTapeIdentityV3Status::Valid,
        "trusted-key decode owns all six tables and preserves semantic values");
  std::vector<std::byte> repeated;
  check(encodeRenderTapeIdentityV3SnapshotTransfer(decoded, repeated) == Status::Valid &&
            repeated == bytes && snapshot.header.sources.offset == 99u,
        "transfer is deterministic and does not modify the caller's stale table descriptors");
  bytes[0] = std::byte{};
  check(decoded.header.captureToken == kKey.token && decoded.sources[0].seqId == 1u,
        "decoded snapshot owns its values independently of borrowed transfer storage");

  auto incomplete = snapshot;
  incomplete.sources[0].completionEdge = incomplete.sources[0].completedSequence = 0u;
  check(encodeRenderTapeIdentityV3SnapshotTransfer(incomplete, bytes) == Status::Valid &&
            decodeRenderTapeIdentityV3SnapshotTransfer(bytes, kKey, decoded) == Status::Valid &&
            !decoded.sources[0].completionEdge &&
            validateFixtureRows(decoded) == RenderTapeIdentityV3Status::Incomplete,
        "structural transfer success does not promote incomplete source evidence");
  std::vector<std::byte> tape;
  const auto sidecar = authenticatedSidecarFixture(tape);
  check(validateRenderTapeIdentityV3(tape, {}, bytes) == RenderTapeIdentityV3Status::InvalidHeader,
        "authenticated sidecar reader rejects the snapshot transfer magic");
  decoded = snapshot;
  check(decodeRenderTapeIdentityV3SnapshotTransfer(sidecar, kKey, decoded) ==
            Status::InvalidHeader && emptySnapshot(decoded),
        "identity sidecar bytes cannot be reinterpreted as a snapshot transfer");
}

void malformedTransferAndExpectedKey() {
  const auto snapshot = snapshotFixture();
  std::vector<std::byte> original;
  check(encodeRenderTapeIdentityV3SnapshotTransfer(snapshot, original) == Status::Valid,
        "corruption fixture begins with a complete transfer");
  for (std::uint32_t mutation = 0u; mutation < 23u; ++mutation) {
    auto bytes = original;
    RenderTapeIdentityV3TransferHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    auto expectedKey = kKey;
    auto expected = Status::InvalidHeader;
    if (mutation == 0u) header.magic = kRenderTapeIdentityMagic;
    if (mutation == 1u) ++header.version;
    if (mutation == 2u) --header.headerSize;
    if (mutation == 3u) header.reserved0 = 1u;
    if (mutation == 4u) header.reserved1 = 1u;
    if (mutation == 5u) header.identity.reserved3 = 1u;
    if (mutation == 6u) header.identity.eventsBytes = 1u;
    if (mutation == 7u) header.identity.eventsDigest[0] = std::byte{1};
    if (mutation == 8u) {
      header.identity.initializers.offset = header.identity.sources.offset;
      expected = Status::InvalidLayout;
    }
    if (mutation == 9u) {
      header.identity.coordinators.entrySize = 1u;
      expected = Status::InvalidLayout;
    }
    if (mutation == 10u) {
      ++header.totalBytes;
      expected = Status::InvalidLayout;
    }
    if (mutation == 11u || mutation == 12u) {
      if (mutation == 11u) bytes.pop_back();
      else bytes.push_back(std::byte{});
      expected = Status::InvalidLayout;
    }
    if (mutation == 13u) { ++expectedKey.token; expected = Status::CaptureMismatch; }
    if (mutation == 14u) { ++expectedKey.epoch; expected = Status::CaptureMismatch; }
    if (mutation == 15u) { expectedKey.token = 0u; expected = Status::CaptureMismatch; }
    if (mutation == 16u) { expectedKey.epoch = 0u; expected = Status::CaptureMismatch; }
    if (mutation >= 17u) {
      const std::array tables{&header.identity.sources, &header.identity.initializers,
          &header.identity.commands, &header.identity.dispositions,
          &header.identity.coordinators, &header.identity.settlements};
      tables[mutation - 17u]->count = kRenderTapeIdentityV3MaxRows + 1u;
      expected = Status::CapacityExceeded;
    }
    std::memcpy(bytes.data(), &header, sizeof(header));
    auto decoded = snapshot;
    check(decodeRenderTapeIdentityV3SnapshotTransfer(bytes, expectedKey, decoded) == expected &&
              emptySnapshot(decoded),
          "corrupt layout/count/reserved/binding and stale expected keys expose no partial snapshot");
  }
  RenderTapeIdentityV3View decoded = snapshot;
  check(decodeRenderTapeIdentityV3SnapshotTransfer(
            std::span<const std::byte>(original).first(sizeof(RenderTapeIdentityV3TransferHeader) - 1u),
            kKey, decoded) == Status::InvalidHeader && emptySnapshot(decoded),
        "truncated transport header empties a previously admitted snapshot");
}

void malformedExportAndAllocationFailure() {
  auto snapshot = snapshotFixture();
  for (std::uint32_t mutation = 0u; mutation < 5u; ++mutation) {
    auto invalid = snapshot;
    if (mutation == 0u) invalid.header.eventsBytes = 1u;
    if (mutation == 1u) invalid.header.eventsDigest[0] = std::byte{1};
    if (mutation == 2u) invalid.header.reserved0 = 1u;
    if (mutation == 3u) invalid.header.captureToken = 0u;
    if (mutation == 4u) invalid.header.captureEpoch = 0u;
    std::vector<std::byte> out{std::byte{1}};
    check(encodeRenderTapeIdentityV3SnapshotTransfer(invalid, out) == Status::InvalidHeader &&
              out.empty(),
          "export rejects claimed event binding and reserved/zero capture keys without hiding evidence");
  }
  auto excessive = snapshot;
  excessive.settlements.resize(kRenderTapeIdentityV3MaxRows + 1u);
  std::vector<std::byte> bytes{std::byte{1}};
  check(encodeRenderTapeIdentityV3SnapshotTransfer(excessive, bytes) ==
            Status::CapacityExceeded && bytes.empty(),
        "export rejects excessive owned table counts before transfer allocation");
  excessive = {};

  transfer_allocation_probe::failAfter = 0u;
  transfer_allocation_probe::enabled = true;
  const auto exportFailure = encodeRenderTapeIdentityV3SnapshotTransfer(snapshot, bytes);
  transfer_allocation_probe::enabled = false;
  check(exportFailure == Status::AllocationFailed && bytes.empty(),
        "export allocation failure publishes no transfer bytes");
  check(encodeRenderTapeIdentityV3SnapshotTransfer(snapshot, bytes) == Status::Valid,
        "decode allocation fixture exports successfully");
  for (std::size_t allocation = 0u; allocation < 6u; ++allocation) {
    auto decoded = snapshot;
    transfer_allocation_probe::failAfter = allocation;
    transfer_allocation_probe::enabled = true;
    const auto status = decodeRenderTapeIdentityV3SnapshotTransfer(bytes, kKey, decoded);
    transfer_allocation_probe::enabled = false;
    check(status == Status::AllocationFailed && emptySnapshot(decoded),
          "failure in each owned-table copy clears all previous and partial decoded values");
  }
}

} // namespace

int main() {
  try {
    roundTripAndAuthoritySeparation();
    malformedTransferAndExpectedKey();
    malformedExportAndAllocationFailure();
    for (const auto status : {Status::Valid, Status::InvalidHeader, Status::InvalidLayout,
             Status::CaptureMismatch, Status::CapacityExceeded, Status::AllocationFailed})
      check(std::string_view(renderTapeIdentityV3TransferStatusName(status)) != "unknown",
            "every transfer result has stable diagnostic text");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "render tape v3 identity transfer spec failed: " << error.what() << '\n';
    return 1;
  }
}
