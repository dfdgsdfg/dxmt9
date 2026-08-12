#include "device_c_render_tape.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
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

template <typename T> std::span<const std::byte> bytesOf(const T& value) {
  return std::as_bytes(std::span(&value, 1u));
}

std::size_t alignUp(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

std::vector<std::byte> makePresentChunk() {
  const D9CCommandChunkWirePresent present{};
  const std::uint32_t recordTableOffset = sizeof(D9CCommandChunkWireHeader);
  const std::uint32_t handleTableOffset = static_cast<std::uint32_t>(
      alignUp(recordTableOffset + sizeof(D9CCommandChunkWireRecordHeader),
              alignof(D9CCommandChunkWireHandleEntry)));
  const std::uint32_t payloadArenaOffset = static_cast<std::uint32_t>(
      alignUp(handleTableOffset, alignof(std::uint32_t)));
  const D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = recordTableOffset,
      .recordCount = 1u,
      .handleTableOffset = handleTableOffset,
      .handleCount = 0u,
      .payloadArenaOffset = payloadArenaOffset,
      .payloadArenaSize = sizeof(present),
      .reserved0 = 0u,
      .reserved1 = 0u,
  };
  const D9CCommandChunkWireRecordHeader record{
      .type = D9C_COMMAND_RECORD_PRESENT,
      .flags = 0u,
      .payloadOffset = 0u,
      .payloadSize = sizeof(present),
      .firstHandle = 0u,
      .handleCount = 0u,
      .reserved0 = 0u,
      .reserved1 = 0u,
  };
  std::vector<std::byte> bytes(payloadArenaOffset + sizeof(present));
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + recordTableOffset, &record, sizeof(record));
  std::memcpy(bytes.data() + payloadArenaOffset, &present, sizeof(present));
  return bytes;
}

std::vector<std::byte>
makeColorFillChunk(const D9CWireObjectIdentity& surfaceIdentity,
                   bool includePresent) {
  const D9CCommandChunkWireColorFill colorFill{
      .surfaceHandleIndex = 0u,
      .colorARGB = 0xff102030u,
      .hasRect = 0u,
      .reserved0 = 0u,
      .rect = {},
  };
  const D9CCommandChunkWirePresent present{};
  const std::uint32_t recordCount = includePresent ? 2u : 1u;
  const std::uint32_t recordTableOffset = sizeof(D9CCommandChunkWireHeader);
  const std::uint32_t handleTableOffset = static_cast<std::uint32_t>(alignUp(
      recordTableOffset + recordCount * sizeof(D9CCommandChunkWireRecordHeader),
      alignof(D9CCommandChunkWireHandleEntry)));
  const std::uint32_t payloadArenaOffset = static_cast<std::uint32_t>(
      alignUp(handleTableOffset + sizeof(D9CCommandChunkWireHandleEntry),
              alignof(std::uint32_t)));
  const std::uint32_t presentOffset = static_cast<std::uint32_t>(
      alignUp(sizeof(colorFill), alignof(std::uint64_t)));
  const std::uint32_t payloadSize =
      includePresent ? presentOffset + sizeof(present) : sizeof(colorFill);
  const D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = recordTableOffset,
      .recordCount = recordCount,
      .handleTableOffset = handleTableOffset,
      .handleCount = 1u,
      .payloadArenaOffset = payloadArenaOffset,
      .payloadArenaSize = payloadSize,
      .reserved0 = 0u,
      .reserved1 = 0u,
  };
  const std::array<D9CCommandChunkWireRecordHeader, 2u> records{{
      {
          .type = D9C_COMMAND_RECORD_COLOR_FILL,
          .flags = 0u,
          .payloadOffset = 0u,
          .payloadSize = sizeof(colorFill),
          .firstHandle = 0u,
          .handleCount = 1u,
          .reserved0 = 0u,
          .reserved1 = 0u,
      },
      {
          .type = D9C_COMMAND_RECORD_PRESENT,
          .flags = 0u,
          .payloadOffset = presentOffset,
          .payloadSize = sizeof(present),
          .firstHandle = 1u,
          .handleCount = 0u,
          .reserved0 = 0u,
          .reserved1 = 0u,
      },
  }};
  const D9CCommandChunkWireHandleEntry handle{
      .kind = surfaceIdentity.kind,
      .generation = surfaceIdentity.generation,
      .objectId = surfaceIdentity.objectId,
  };
  std::vector<std::byte> bytes(payloadArenaOffset + payloadSize);
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + recordTableOffset, records.data(),
              recordCount * sizeof(records[0]));
  std::memcpy(bytes.data() + handleTableOffset, &handle, sizeof(handle));
  std::memcpy(bytes.data() + payloadArenaOffset, &colorFill, sizeof(colorFill));
  if (includePresent) {
    std::memcpy(bytes.data() + payloadArenaOffset + presentOffset, &present,
                sizeof(present));
  }
  return bytes;
}

std::vector<std::byte> makeCompleteTape() {
  constexpr D9CWireObjectIdentity surface{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 2u,
      .objectId = 17u,
  };
  constexpr std::array<std::byte, 4> state{std::byte{0x10}, std::byte{0x20},
                                           std::byte{0x30}, std::byte{0x40}};
  constexpr std::array<std::byte, 8> descriptor{
      std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
      std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
  constexpr std::array<std::byte, 4> pixels{std::byte{0xaa}, std::byte{0xbb},
                                            std::byte{0xcc}, std::byte{0xdd}};
  const auto chunk = makeColorFillChunk(surface, true);

  RenderTapeBuilder builder;
  builder.appendCheckpoint(1u, {}, state);
  builder.appendObjectCreate(surface, 1u, descriptor);
  builder.appendResourceWrite(surface, 0u, 0u, pixels);
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 2u, .handleCount = 1u}, chunk);
  builder.appendPresentBoundary(1u);
  return builder.seal();
}

class RecordingSink final : public RenderTapeReplaySink {
public:
  bool checkpoint(std::uint32_t stateVersion,
                  std::span<const D9CWireObjectIdentity> initialObjects,
                  std::span<const std::byte> state) override {
    calls.push_back(RenderTapeEventType::Checkpoint);
    checkpointVersion = stateVersion;
    checkpointObjects = initialObjects.size();
    checkpointBytes = state.size();
    return true;
  }

  bool objectCreate(const RenderTapeObjectCreateHeader& fixed,
                    std::span<const std::byte> descriptor) override {
    calls.push_back(RenderTapeEventType::ObjectCreate);
    objectId = fixed.identity.objectId;
    descriptorBytes = descriptor.size();
    return true;
  }

  bool resourceWrite(const RenderTapeResourceWriteHeader& fixed,
                     std::span<const std::byte> data) override {
    calls.push_back(RenderTapeEventType::ResourceWrite);
    writeSubresource = fixed.subresource;
    writeBytes = data.size();
    return true;
  }

  bool commandChunk(const CommandChunkEnvelope& envelope,
                    std::span<const std::byte> chunk) override {
    calls.push_back(RenderTapeEventType::CommandChunk);
    ImportedChunkView imported;
    const auto result = validateCommandChunk(chunk, envelope, &imported);
    chunkRecords = imported.records.size();
    return result.valid();
  }

  bool objectDestroy(const D9CWireObjectIdentity&) override {
    calls.push_back(RenderTapeEventType::ObjectDestroy);
    return true;
  }

  bool presentBoundary(const RenderTapePresentBoundary& boundary) override {
    calls.push_back(RenderTapeEventType::PresentBoundary);
    presentOrdinal = boundary.presentOrdinal;
    return true;
  }

  std::vector<RenderTapeEventType> calls;
  std::uint32_t checkpointVersion = 0u;
  std::size_t checkpointObjects = 0u;
  std::size_t checkpointBytes = 0u;
  std::uint64_t objectId = 0u;
  std::size_t descriptorBytes = 0u;
  std::uint32_t writeSubresource = 0u;
  std::size_t writeBytes = 0u;
  std::size_t chunkRecords = 0u;
  std::uint64_t presentOrdinal = 0u;
};

void validTapeRoundTripsAndReplaysInOrder() {
  const auto bytes = makeCompleteTape();
  ImportedRenderTapeView tape;
  const auto validation = validateRenderTape(bytes, &tape);
  check(validation.valid(), "complete frame tape validates");
  check(tape.header.eventCount == 5u, "event count is conserved");
  check(tape.header.presentCount == 1u, "one Present is declared");

  ImportedRenderTapeView fastView;
  check(importPrevalidatedRenderTape(bytes, fastView),
        "prevalidated import reconstructs spans");
  RecordingSink sink;
  const auto replay = replayPrevalidatedRenderTape(fastView, sink);
  check(replay.complete, "prevalidated tape replays completely");
  check(sink.calls ==
            std::vector<RenderTapeEventType>{
                RenderTapeEventType::Checkpoint,
                RenderTapeEventType::ObjectCreate,
                RenderTapeEventType::ResourceWrite,
                RenderTapeEventType::CommandChunk,
                RenderTapeEventType::PresentBoundary,
            },
        "event order is exact");
  check(sink.checkpointVersion == 1u && sink.checkpointObjects == 0u &&
            sink.checkpointBytes == 4u,
        "checkpoint reaches sink intact");
  check(sink.objectId == 17u && sink.descriptorBytes == 8u,
        "object creation reaches sink intact");
  check(sink.writeSubresource == 0u && sink.writeBytes == 4u,
        "resource mutation reaches sink intact");
  check(sink.chunkRecords == 2u && sink.presentOrdinal == 1u,
        "production chunk validation and Present boundary execute");
}

template <typename Mutator>
void checkReject(Mutator mutate, RenderTapeValidationStatus status,
                 std::string_view message) {
  auto bytes = makeCompleteTape();
  mutate(bytes);
  const auto result = validateRenderTape(bytes);
  check(result.status == status, message);
}

RenderTapeHeader& tapeHeader(std::vector<std::byte>& bytes) {
  return *reinterpret_cast<RenderTapeHeader*>(bytes.data());
}

RenderTapeEventHeader& tapeEvent(std::vector<std::byte>& bytes,
                                 std::size_t index) {
  auto& header = tapeHeader(bytes);
  return *reinterpret_cast<RenderTapeEventHeader*>(
      bytes.data() + header.eventTableOffset +
      index * sizeof(RenderTapeEventHeader));
}

void invalidTapesFailBeforeReplay() {
  checkReject([](auto& bytes) { tapeHeader(bytes).version += 1u; },
              RenderTapeValidationStatus::InvalidHeader,
              "unknown tape version rejects");
  checkReject([](auto& bytes) { tapeEvent(bytes, 2u).ordinal = 9u; },
              RenderTapeValidationStatus::InvalidEventOrdinal,
              "ordinal gap rejects");
  checkReject([](auto& bytes) { tapeEvent(bytes, 3u).payloadSize -= 1u; },
              RenderTapeValidationStatus::InvalidCommandChunk,
              "truncated canonical chunk rejects");
  checkReject([](auto& bytes) { tapeHeader(bytes).presentCount = 0u; },
              RenderTapeValidationStatus::IncompleteFrame,
              "frame without declared Present rejects");

  {
    auto complete = makeCompleteTape();
    std::vector<std::byte> misaligned(complete.size() + 1u);
    std::memcpy(misaligned.data() + 1u, complete.data(), complete.size());
    check(validateRenderTape(std::span<const std::byte>(misaligned).subspan(1u))
                  .status == RenderTapeValidationStatus::InvalidLayout,
          "misaligned packed tape rejects before span construction");
  }

  auto bytes = makeCompleteTape();
  auto& create = tapeEvent(bytes, 1u);
  auto& write = tapeEvent(bytes, 2u);
  std::swap(create.type, write.type);
  const auto result = validateRenderTape(bytes);
  check(!result.valid(), "mutation before object creation rejects");
}

void builderPreservesExplicitDestroyOrdering() {
  constexpr D9CWireObjectIdentity texture{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
      .generation = 3u,
      .objectId = 23u,
  };
  constexpr std::array<std::byte, 1> state{std::byte{1}};
  constexpr std::array<std::byte, 1> descriptor{std::byte{2}};
  const auto chunk = makePresentChunk();
  RenderTapeBuilder builder;
  builder.appendCheckpoint(1u, {}, state);
  builder.appendObjectCreate(texture, 1u, descriptor);
  builder.appendObjectDestroy(texture);
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u}, chunk);
  builder.appendPresentBoundary(1u);
  const auto bytes = builder.seal();
  check(validateRenderTape(bytes).valid(),
        "destroyed unreferenced identity remains a valid frame");
}

void builderRefusesToSealIncompleteOrAliasedFrames() {
  constexpr std::array<std::byte, 1> state{std::byte{1}};
  RenderTapeBuilder incomplete;
  incomplete.appendCheckpoint(1u, {}, state);
  bool rejected = false;
  try {
    static_cast<void>(incomplete.seal());
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "seal rejects a frame without canonical Present");

  constexpr D9CWireObjectIdentity generation1{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
      .generation = 1u,
      .objectId = 99u,
  };
  constexpr D9CWireObjectIdentity generation2{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
      .generation = 2u,
      .objectId = 99u,
  };
  constexpr std::array<std::byte, 1> descriptor{std::byte{2}};
  const auto chunk = makePresentChunk();
  RenderTapeBuilder aliased;
  aliased.appendCheckpoint(1u, {}, state);
  aliased.appendObjectCreate(generation1, 1u, descriptor);
  aliased.appendObjectCreate(generation2, 1u, descriptor);
  aliased.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u}, chunk);
  aliased.appendPresentBoundary(1u);
  rejected = false;
  try {
    static_cast<void>(aliased.seal());
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "seal rejects two live generations of one object slot");

  constexpr D9CWireObjectIdentity missingSurface{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 1u,
      .objectId = 100u,
  };
  constexpr D9CWireObjectIdentity referencedSurface{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 1u,
      .objectId = 101u,
  };
  const auto unknownChunk = makeColorFillChunk(referencedSurface, true);
  RenderTapeBuilder unknownHandle;
  unknownHandle.appendCheckpoint(1u, {}, state);
  unknownHandle.appendObjectCreate(missingSurface, 1u, descriptor);
  unknownHandle.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 2u, .handleCount = 1u}, unknownChunk);
  unknownHandle.appendPresentBoundary(1u);
  rejected = false;
  try {
    static_cast<void>(unknownHandle.seal());
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "seal rejects a chunk that references an unknown object");

  constexpr D9CWireObjectIdentity retainedGeneration1{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 1u,
      .objectId = 102u,
  };
  constexpr D9CWireObjectIdentity retainedGeneration2{
      .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
      .generation = 2u,
      .objectId = 102u,
  };
  const auto retainedChunk = makeColorFillChunk(retainedGeneration1, false);
  RenderTapeBuilder retainedReuse;
  retainedReuse.appendCheckpoint(1u, {}, state);
  retainedReuse.appendObjectCreate(retainedGeneration1, 1u, descriptor);
  retainedReuse.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u},
      retainedChunk);
  retainedReuse.appendObjectDestroy(retainedGeneration1);
  retainedReuse.appendObjectCreate(retainedGeneration2, 1u, descriptor);
  retainedReuse.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 0u}, chunk);
  retainedReuse.appendPresentBoundary(1u);
  rejected = false;
  try {
    static_cast<void>(retainedReuse.seal());
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected,
        "seal rejects object-slot reuse while an earlier chunk retains it");
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string_view(argv[1]) == "--write-fixture") {
      const auto bytes = makeCompleteTape();
      std::ofstream stream(argv[2], std::ios::binary | std::ios::trunc);
      stream.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
      check(static_cast<bool>(stream), "fixture tape write succeeds");
      return 0;
    }
    check(argc == 1, "unknown render_tape_spec arguments");
    validTapeRoundTripsAndReplaysInOrder();
    invalidTapesFailBeforeReplay();
    builderPreservesExplicitDestroyOrdering();
    builderRefusesToSealIncompleteOrAliasedFrames();
  } catch (const std::exception& error) {
    std::cerr << "render_tape_spec failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "render_tape_spec passed\n";
  return 0;
}
