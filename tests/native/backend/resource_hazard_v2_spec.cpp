#include "device_c_chunk_v2_replay.hpp"
#include "device_c_chunk_v2_registry.hpp"
#include "device_c_record_utils.hpp"
#include "device_c_replay_offload.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using dxmt9::d3d9::ImportedChunkV2View;
using dxmt9::d3d9::ResolvedRecordV2View;
using dxmt9::d3d9::V2ChunkEnvelope;
using dxmt9::d3d9::WireObjectRegistry;
using dxmt9::d3d9::devicec::ImportedRecordResourceHazards;
using dxmt9::d3d9::devicec::ImportedReplayHazardState;
using dxmt9::d3d9::devicec::ImportedReplayOrderingAction;
using dxmt9::d3d9::devicec::collectImportedRecordResourceHazardsV2;
using dxmt9::d3d9::devicec::evaluateImportedReplayOrderingV2;
using dxmt9::d3d9::devicec::nextImportedReplayHazardState;
using dxmt9::d3d9::devicec::v2RecordRequiresEffectiveResourceMarking;

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

template <typename T>
std::vector<std::byte> bytesOf(const T& value) {
  std::vector<std::byte> bytes(sizeof(T));
  std::memcpy(bytes.data(), &value, sizeof(value));
  return bytes;
}

struct ResolvedFixture {
  std::uint32_t type = 0u;
  std::uint32_t firstHandle = 0u;
  D9CCommandChunkWireDrawHeaderV2 draw{};
  std::vector<std::byte> payload;
  std::vector<void*> objects;

  ResolvedRecordV2View view() const {
    const auto* sections =
        draw.sectionCount == 0u
            ? nullptr
            : reinterpret_cast<const D9CCommandChunkWireSectionDescV2*>(
                  payload.data() + draw.sectionTableOffset);
    return ResolvedRecordV2View{
        .wire = dxmt9::d3d9::ImportedRecordV2View{
            .header = D9CCommandChunkWireRecordHeaderV2{
                .type = type,
                .payloadSize = static_cast<std::uint32_t>(payload.size()),
                .firstHandle = firstHandle,
                .handleCount = static_cast<std::uint32_t>(objects.size()),
            },
            .payload = payload,
            .drawHeader = draw,
            .sections = std::span<const D9CCommandChunkWireSectionDescV2>(
                sections, draw.sectionCount),
        },
        .objects = std::span<void* const>(objects.data(), objects.size()),
    };
  }
};

template <typename Binding>
ResolvedFixture makeDrawFixture(std::uint16_t sectionKind,
                                std::span<const Binding> bindings,
                                std::span<void* const> objects,
                                std::uint32_t firstHandle = 0u) {
  D9CCommandChunkWireDrawHeaderV2 draw{
      .primitiveType = 4u,
      .primitiveCount = 1u,
      .sectionCount = 1u,
      .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeaderV2),
  };
  draw.sectionPayloadOffset = static_cast<std::uint32_t>(alignUp(
      sizeof(draw) + sizeof(D9CCommandChunkWireSectionDescV2), 4u));
  const auto sectionBytes = std::as_bytes(bindings);
  const auto* rule = dxmt9::d3d9::v2SectionRule(sectionKind);
  check(rule != nullptr, "hazard draw fixture section is known");
  std::vector<std::byte> payload(draw.sectionPayloadOffset);
  const D9CCommandChunkWireSectionDescV2 descriptor{
      .kind = sectionKind,
      .elementSize = rule->elementSize,
      .count = static_cast<std::uint32_t>(bindings.size()),
      .payloadOffset = draw.sectionPayloadOffset,
      .byteSize = static_cast<std::uint32_t>(sectionBytes.size()),
  };
  payload.insert(payload.end(), sectionBytes.begin(), sectionBytes.end());
  std::memcpy(payload.data(), &draw, sizeof(draw));
  std::memcpy(payload.data() + draw.sectionTableOffset, &descriptor,
              sizeof(descriptor));
  return ResolvedFixture{
      .type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
      .firstHandle = firstHandle,
      .draw = draw,
      .payload = std::move(payload),
      .objects = std::vector<void*>(objects.begin(), objects.end()),
  };
}

template <typename Fixed>
ResolvedFixture makeFixedFixture(std::uint32_t type, const Fixed& fixed,
                                 std::span<void* const> objects,
                                 std::uint32_t firstHandle = 0u) {
  return ResolvedFixture{
      .type = type,
      .firstHandle = firstHandle,
      .payload = bytesOf(fixed),
      .objects = std::vector<void*>(objects.begin(), objects.end()),
  };
}

std::uint64_t pointerValue(const void* value) {
  return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(value));
}

bool contains(const dxmt9::d3d9::devicec::ImportedChunkHandleSet& set,
              std::uint32_t kind, const void* object) {
  const auto value = pointerValue(object);
  const auto& bucket = set.byKind[kind];
  return std::find(bucket.begin(), bucket.end(), value) != bucket.end();
}

void testSparseDrawHazardsAndEffectiveStateMarking() {
  int texture = 1;
  int buffer = 2;
  int surface = 3;
  std::array<void*, 3> objects{&texture, &buffer, &surface};
  constexpr std::uint32_t firstHandle = 5u;
  const std::array textures = {
      D9CCommandChunkWireTextureBindingV2{
          .slot = 0u, .valid = 1u, .handleIndex = firstHandle},
  };
  auto textureFixture = makeDrawFixture(
      D9C_COMMAND_CHUNK_V2_SECTION_TEXTURE,
      std::span<const D9CCommandChunkWireTextureBindingV2>(textures),
      objects, firstHandle);
  ImportedRecordResourceHazards hazards;
  collectImportedRecordResourceHazardsV2(textureFixture.view(), hazards);
  check(contains(hazards.reads, D9C_CHUNK_HANDLE_KIND_TEXTURE, &texture) &&
            hazards.writes.byKind[D9C_CHUNK_HANDLE_KIND_TEXTURE].empty(),
        "texture binding is a direct V2 draw read hazard");

  const std::array streams = {
      D9CCommandChunkWireStreamBindingV2{
          .slot = 0u,
          .valid = 1u,
          .handleIndex = firstHandle + 1u,
      },
  };
  auto streamFixture = makeDrawFixture(
      D9C_COMMAND_CHUNK_V2_SECTION_STREAM,
      std::span<const D9CCommandChunkWireStreamBindingV2>(streams), objects,
      firstHandle);
  hazards = {};
  collectImportedRecordResourceHazardsV2(streamFixture.view(), hazards);
  check(contains(hazards.reads, D9C_CHUNK_HANDLE_KIND_BUFFER, &buffer),
        "stream binding is a direct V2 draw read hazard");

  const std::array targets = {
      D9CCommandChunkWireRenderTargetBindingV2{
          .slot = 0u,
          .valid = 1u,
          .handleIndex = firstHandle + 2u,
      },
  };
  auto targetFixture = makeDrawFixture(
      D9C_COMMAND_CHUNK_V2_SECTION_RENDER_TARGET,
      std::span<const D9CCommandChunkWireRenderTargetBindingV2>(targets),
      objects, firstHandle);
  hazards = {};
  collectImportedRecordResourceHazardsV2(targetFixture.view(), hazards);
  check(contains(hazards.writes, D9C_CHUNK_HANDLE_KIND_SURFACE, &surface) &&
            v2RecordRequiresEffectiveResourceMarking(targetFixture.view()),
        "render target is a write and every V2 draw keeps effective marking");

  D9CCommandChunkWireDrawHeaderV2 emptyDraw{
      .primitiveType = 4u,
      .primitiveCount = 1u,
      .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeaderV2),
      .sectionPayloadOffset = sizeof(D9CCommandChunkWireDrawHeaderV2),
  };
  ResolvedFixture carried{
      .type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
      .draw = emptyDraw,
      .payload = bytesOf(emptyDraw),
  };
  hazards = {};
  collectImportedRecordResourceHazardsV2(carried.view(), hazards);
  check(hazards.reads.byKind[D9C_CHUNK_HANDLE_KIND_TEXTURE].empty() &&
            hazards.writes.byKind[D9C_CHUNK_HANDLE_KIND_SURFACE].empty() &&
            v2RecordRequiresEffectiveResourceMarking(carried.view()),
        "empty delta does not fake direct refs but still marks carried draw state");
}

void testFixedHazardIntentMatrix() {
  int src = 1;
  int dst = 2;
  std::array<void*, 2> objects{&src, &dst};
  constexpr std::uint32_t firstHandle = 8u;
  const auto checkPair = [&](std::uint32_t type, const auto& fixed,
                             std::uint32_t srcKind,
                             std::uint32_t dstKind) {
    const auto fixture = makeFixedFixture(type, fixed, objects, firstHandle);
    ImportedRecordResourceHazards hazards;
    collectImportedRecordResourceHazardsV2(fixture.view(), hazards);
    check(contains(hazards.reads, srcKind, &src) &&
              contains(hazards.writes, dstKind, &dst),
          "fixed V2 operation preserves source-read/destination-write intent");
  };
  checkPair(D9C_COMMAND_RECORD_STRETCH_RECT,
            D9CCommandChunkWireStretchRectV2{
                .srcHandleIndex = firstHandle,
                .dstHandleIndex = firstHandle + 1u,
            },
            D9C_CHUNK_HANDLE_KIND_SURFACE,
            D9C_CHUNK_HANDLE_KIND_SURFACE);
  checkPair(D9C_COMMAND_RECORD_UPDATE_TEXTURE,
            D9CCommandChunkWireUpdateTextureV2{firstHandle,
                                                firstHandle + 1u},
            D9C_CHUNK_HANDLE_KIND_TEXTURE,
            D9C_CHUNK_HANDLE_KIND_TEXTURE);
  checkPair(D9C_COMMAND_RECORD_UPDATE_SURFACE,
            D9CCommandChunkWireUpdateSurfaceV2{
                .srcHandleIndex = firstHandle,
                .dstHandleIndex = firstHandle + 1u,
            },
            D9C_CHUNK_HANDLE_KIND_SURFACE,
            D9C_CHUNK_HANDLE_KIND_SURFACE);
  checkPair(D9C_COMMAND_RECORD_READBACK,
            D9CCommandChunkWireReadbackV2{firstHandle,
                                          firstHandle + 1u},
            D9C_CHUNK_HANDLE_KIND_SURFACE,
            D9C_CHUNK_HANDLE_KIND_SURFACE);
  checkPair(D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE,
            D9CCommandChunkWireReszDepthResolveV2{firstHandle,
                                                   firstHandle + 1u},
            D9C_CHUNK_HANDLE_KIND_SURFACE,
            D9C_CHUNK_HANDLE_KIND_TEXTURE);

  const auto color = makeFixedFixture(
      D9C_COMMAND_RECORD_COLOR_FILL,
      D9CCommandChunkWireColorFillV2{.surfaceHandleIndex = firstHandle},
      std::span<void* const>(objects).first(1u), firstHandle);
  ImportedRecordResourceHazards hazards;
  collectImportedRecordResourceHazardsV2(color.view(), hazards);
  check(hazards.reads.byKind[D9C_CHUNK_HANDLE_KIND_SURFACE].empty() &&
            contains(hazards.writes, D9C_CHUNK_HANDLE_KIND_SURFACE, &src),
        "ColorFill is write-only in V2 hazards");
}

void testV2OrderingDecisions() {
  int firstSurface = 1;
  int secondSurface = 2;
  const auto targetFixture = [&](void* surface) {
    std::array<void*, 1> objects{surface};
    const std::array targets = {
        D9CCommandChunkWireRenderTargetBindingV2{
            .slot = 0u, .valid = 1u, .handleIndex = 0u},
    };
    return makeDrawFixture(
        D9C_COMMAND_CHUNK_V2_SECTION_RENDER_TARGET,
        std::span<const D9CCommandChunkWireRenderTargetBindingV2>(targets),
        objects);
  };
  const auto first = targetFixture(&firstSurface);
  const auto disjoint = targetFixture(&secondSurface);
  const auto overlap = targetFixture(&firstSurface);
  ImportedReplayHazardState state;
  auto decision = evaluateImportedReplayOrderingV2(first.view(), state);
  check(decision.action == ImportedReplayOrderingAction::Continue,
        "first V2 draw starts a hazard scope");
  state = nextImportedReplayHazardState(state, decision);
  decision = evaluateImportedReplayOrderingV2(disjoint.view(), state);
  check(decision.action == ImportedReplayOrderingAction::Continue,
        "disjoint V2 draw continues the active scope");
  state = nextImportedReplayHazardState(state, decision);
  decision = evaluateImportedReplayOrderingV2(overlap.view(), state);
  check(decision.action == ImportedReplayOrderingAction::HazardBoundary &&
            decision.writeAfterWrite,
        "overlapping V2 draw write produces a WAW boundary");

  std::array<void*, 2> surfaces{&firstSurface, &secondSurface};
  const auto readback = makeFixedFixture(
      D9C_COMMAND_RECORD_READBACK,
      D9CCommandChunkWireReadbackV2{0u, 1u}, surfaces);
  decision = evaluateImportedReplayOrderingV2(readback.view(), state);
  check(decision.action ==
            ImportedReplayOrderingAction::SynchronousReadBoundary &&
            decision.resetsActiveHazards &&
            !nextImportedReplayHazardState(state, decision).active,
        "V2 readback drains and resets active hazards");
}

struct RecordSpec {
  std::uint32_t type = 0u;
  std::vector<std::byte> payload;
  std::vector<D9CCommandChunkWireHandleEntryV2> handles;
};

struct ChunkBlob {
  std::vector<std::byte> bytes;
  V2ChunkEnvelope envelope{};
};

ChunkBlob makeChunk(std::span<const RecordSpec> specs) {
  std::vector<D9CCommandChunkWireRecordHeaderV2> records;
  std::vector<D9CCommandChunkWireHandleEntryV2> handles;
  std::vector<std::byte> payload;
  for (const auto& spec : specs) {
    const auto* rule = dxmt9::d3d9::v2RecordRule(spec.type);
    check(rule != nullptr, "offload fixture record is known");
    payload.resize(alignUp(payload.size(), rule->payloadAlignment));
    records.push_back(D9CCommandChunkWireRecordHeaderV2{
        .type = spec.type,
        .payloadOffset = static_cast<std::uint32_t>(payload.size()),
        .payloadSize = static_cast<std::uint32_t>(spec.payload.size()),
        .firstHandle = static_cast<std::uint32_t>(handles.size()),
        .handleCount = static_cast<std::uint32_t>(spec.handles.size()),
    });
    payload.insert(payload.end(), spec.payload.begin(), spec.payload.end());
    handles.insert(handles.end(), spec.handles.begin(), spec.handles.end());
  }
  D9CCommandChunkWireHeaderV2 header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION_V2,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_V2_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_V2_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_V2_SIZE,
      .recordTableOffset = D9C_COMMAND_CHUNK_WIRE_HEADER_V2_SIZE,
      .recordCount = static_cast<std::uint32_t>(records.size()),
      .handleCount = static_cast<std::uint32_t>(handles.size()),
      .payloadArenaSize = static_cast<std::uint32_t>(payload.size()),
  };
  header.handleTableOffset = static_cast<std::uint32_t>(alignUp(
      header.recordTableOffset + records.size() * sizeof(records[0]),
      alignof(D9CCommandChunkWireHandleEntryV2)));
  header.payloadArenaOffset = static_cast<std::uint32_t>(alignUp(
      header.handleTableOffset + handles.size() * sizeof(handles[0]), 4u));
  ChunkBlob blob{
      .bytes = std::vector<std::byte>(header.payloadArenaOffset +
                                     payload.size()),
      .envelope = V2ChunkEnvelope{
          .version = D9C_COMMAND_CHUNK_VERSION_V2,
          .recordCount = header.recordCount,
          .handleCount = header.handleCount,
      },
  };
  std::memcpy(blob.bytes.data(), &header, sizeof(header));
  if (!records.empty()) {
    std::memcpy(blob.bytes.data() + header.recordTableOffset, records.data(),
                records.size() * sizeof(records[0]));
  }
  if (!handles.empty()) {
    std::memcpy(blob.bytes.data() + header.handleTableOffset, handles.data(),
                handles.size() * sizeof(handles[0]));
  }
  if (!payload.empty()) {
    std::memcpy(blob.bytes.data() + header.payloadArenaOffset, payload.data(),
                payload.size());
  }
  return blob;
}

std::uint32_t retainCount = 0u;

void countRetain(std::uint32_t, void*) noexcept {
  ++retainCount;
}

void testTransactionalOffloadAdmissionAndQueueOwnership() {
  WireObjectRegistry registry;
  int srcTexture = 1;
  int dstTexture = 2;
  int query = 3;
  const auto srcIdentity =
      registry.insert(D9C_CHUNK_HANDLE_KIND_TEXTURE, &srcTexture);
  const auto dstIdentity =
      registry.insert(D9C_CHUNK_HANDLE_KIND_TEXTURE, &dstTexture);
  const auto queryIdentity =
      registry.insert(D9C_CHUNK_HANDLE_KIND_QUERY, &query);
  const std::array specs = {
      RecordSpec{
          .type = D9C_COMMAND_RECORD_UPDATE_TEXTURE,
          .payload = bytesOf(D9CCommandChunkWireUpdateTextureV2{0u, 1u}),
          .handles = {
              dxmt9::d3d9::wireHandleEntryV2(srcIdentity),
              dxmt9::d3d9::wireHandleEntryV2(dstIdentity),
          },
      },
      RecordSpec{
          .type = D9C_COMMAND_RECORD_QUERY_ISSUE,
          .payload = bytesOf(D9CCommandChunkWireQueryIssueV2{2u, 1u}),
          .handles = {dxmt9::d3d9::wireHandleEntryV2(queryIdentity)},
      },
      RecordSpec{
          .type = D9C_COMMAND_RECORD_PRESENT,
          .payload = bytesOf(D9CCommandChunkWirePresentV2{}),
      },
  };
  const auto blob = makeChunk(specs);
  ImportedChunkV2View validated;
  check(dxmt9::d3d9::validateCommandChunkV2(
            blob.bytes, blob.envelope, &validated).valid(),
        "offload admission fixture validates before registry entry");

  retainCount = 0u;
  dxmt9::d3d9::RawCommandChunk raw;
  check(dxmt9::d3d9::prepareV2OffloadChunk(
            blob.bytes, blob.envelope, registry, countRetain, raw) &&
            retainCount == 3u && raw.preflightValidated &&
            raw.wireVersion == D9C_COMMAND_CHUNK_VERSION_V2 &&
            raw.recordCount == 3u && raw.handleCount == 3u && raw.hasPresent &&
            raw.resolvedObjects[0] == &srcTexture &&
            raw.resolvedObjects[1] == &dstTexture &&
            raw.resolvedObjects[2] == &query,
        "app thread validates, resolves, and retains every V2 object");

  check(registry.erase(srcIdentity, &srcTexture) &&
            registry.erase(dstIdentity, &dstTexture) &&
            registry.erase(queryIdentity, &query),
        "registry identities may disappear after offload admission");
  dxmt9::d3d9::ReplayOffloadQueue queue(2u, 1u << 20u);
  check(queue.push(std::move(raw)), "admitted V2 chunk enters offload FIFO");
  dxmt9::d3d9::RawCommandChunk popped;
  check(queue.pop(popped) && popped.preflightValidated &&
            popped.resolvedObjects[0] == &srcTexture &&
            popped.resolvedObjects[2] == &query,
        "worker receives resolved pointers without a registry lookup");

  ImportedChunkV2View workerView;
  const auto ownedBytes = std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(popped.recordBlob.data()),
      popped.recordBlob.size());
  check(dxmt9::d3d9::validateCommandChunkV2(
            ownedBytes, blob.envelope, &workerView).valid(),
        "queue-owned immutable blob can rebind decoded spans");
  dxmt9::d3d9::ResolvedChunkV2View resolved{
      .wire = workerView,
      .objects = popped.resolvedObjects,
  };
  ImportedRecordResourceHazards hazards;
  collectImportedRecordResourceHazardsV2(resolved.record(0u), hazards);
  check(contains(hazards.reads, D9C_CHUNK_HANDLE_KIND_TEXTURE, &srcTexture) &&
            contains(hazards.writes, D9C_CHUNK_HANDLE_KIND_TEXTURE,
                     &dstTexture) &&
            resolved.record(1u).objects[0] == &query,
        "offload replay preserves hazard intent and Query lifetime");
  queue.markReplayDone();
  popped.retainedWrappers.clear();
  popped.resolvedObjects.clear();
  retainCount = 0u;

  WireObjectRegistry staleRegistry;
  int staleObject = 4;
  const auto staleIdentity =
      staleRegistry.insert(D9C_CHUNK_HANDLE_KIND_TEXTURE, &staleObject);
  check(staleRegistry.erase(staleIdentity, &staleObject),
        "stale admission fixture removes identity");
  const std::array staleSpec = {
      RecordSpec{
          .type = D9C_COMMAND_RECORD_COLOR_FILL,
          .payload = bytesOf(
              D9CCommandChunkWireColorFillV2{.surfaceHandleIndex = 0u}),
          .handles = {dxmt9::d3d9::wireHandleEntryV2(staleIdentity)},
      },
  };
  auto staleBlob = makeChunk(staleSpec);
  auto& staleHandle = *reinterpret_cast<D9CCommandChunkWireHandleEntryV2*>(
      staleBlob.bytes.data() +
      reinterpret_cast<const D9CCommandChunkWireHeaderV2*>(
          staleBlob.bytes.data())->handleTableOffset);
  staleHandle.kind = D9C_CHUNK_HANDLE_KIND_SURFACE;
  dxmt9::d3d9::RawCommandChunk rejected;
  check(!dxmt9::d3d9::prepareV2OffloadChunk(
            staleBlob.bytes, staleBlob.envelope, staleRegistry, countRetain,
            rejected) &&
            retainCount == 0u && rejected.recordBlob.empty() &&
            rejected.retainedWrappers.empty(),
        "stale/wrong-kind admission fails transactionally before enqueue");
}

}  // namespace

int main() {
  try {
    testSparseDrawHazardsAndEffectiveStateMarking();
    testFixedHazardIntentMatrix();
    testV2OrderingDecisions();
    testTransactionalOffloadAdmissionAndQueueOwnership();
  } catch (const TestFailure& error) {
    std::cerr << "resource_hazard_v2_spec failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "resource_hazard_v2_spec passed\n";
  return EXIT_SUCCESS;
}
