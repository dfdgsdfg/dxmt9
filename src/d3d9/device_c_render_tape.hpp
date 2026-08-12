#pragma once

#include "device_c_chunk_validate.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dxmt9::d3d9 {

inline constexpr std::uint64_t kRenderTapeMagic = 0x31505452544d5844ull;
inline constexpr std::uint32_t kRenderTapeVersion = 1u;
inline constexpr std::uint32_t kRenderTapeProfileFrame = 1u;
inline constexpr std::uint32_t kRenderTapePayloadAlignment = 8u;

enum class RenderTapeEventType : std::uint32_t {
  Checkpoint = 1u,
  ObjectCreate = 2u,
  ResourceWrite = 3u,
  CommandChunk = 4u,
  ObjectDestroy = 5u,
  PresentBoundary = 6u,
};

struct RenderTapeHeader {
  std::uint64_t magic = kRenderTapeMagic;
  std::uint32_t version = kRenderTapeVersion;
  std::uint32_t headerSize = sizeof(RenderTapeHeader);
  std::uint32_t profile = kRenderTapeProfileFrame;
  std::uint32_t eventHeaderSize = 0u;
  std::uint32_t eventTableOffset = 0u;
  std::uint32_t eventCount = 0u;
  std::uint32_t payloadArenaOffset = 0u;
  std::uint32_t payloadArenaSize = 0u;
  std::uint32_t wireVersion = D9C_COMMAND_CHUNK_WIRE_VERSION;
  std::uint32_t presentCount = 0u;
  std::uint32_t reserved0 = 0u;
  std::uint32_t reserved1 = 0u;
  std::uint32_t reserved2 = 0u;
  std::uint32_t reserved3 = 0u;
};

struct RenderTapeEventHeader {
  std::uint32_t type = 0u;
  std::uint32_t flags = 0u;
  std::uint64_t ordinal = 0u;
  std::uint32_t payloadOffset = 0u;
  std::uint32_t payloadSize = 0u;
  std::uint32_t reserved0 = 0u;
  std::uint32_t reserved1 = 0u;
};

struct RenderTapeCheckpointHeader {
  std::uint32_t stateVersion = 0u;
  std::uint32_t objectCount = 0u;
  std::uint32_t stateBytes = 0u;
  std::uint32_t reserved0 = 0u;
};

struct RenderTapeObjectCreateHeader {
  D9CWireObjectIdentity identity{};
  std::uint32_t descriptorKind = 0u;
  std::uint32_t descriptorBytes = 0u;
};

struct RenderTapeResourceWriteHeader {
  D9CWireObjectIdentity identity{};
  std::uint32_t subresource = 0u;
  std::uint32_t flags = 0u;
  std::uint64_t byteOffset = 0u;
  std::uint32_t dataBytes = 0u;
  std::uint32_t reserved0 = 0u;
};

struct RenderTapeCommandChunkHeader {
  std::uint32_t wireVersion = D9C_COMMAND_CHUNK_WIRE_VERSION;
  std::uint32_t recordCount = 0u;
  std::uint32_t handleCount = 0u;
  std::uint32_t chunkBytes = 0u;
};

struct RenderTapePresentBoundary {
  std::uint64_t presentOrdinal = 0u;
  std::uint32_t flags = 0u;
  std::uint32_t reserved0 = 0u;
};

static_assert(sizeof(RenderTapeHeader) == 64u);
static_assert(alignof(RenderTapeHeader) == 8u);
static_assert(sizeof(RenderTapeEventHeader) == 32u);
static_assert(alignof(RenderTapeEventHeader) == 8u);
static_assert(sizeof(RenderTapeCheckpointHeader) == 16u);
static_assert(sizeof(RenderTapeObjectCreateHeader) == 24u);
static_assert(sizeof(RenderTapeResourceWriteHeader) == 40u);
static_assert(sizeof(RenderTapeCommandChunkHeader) == 16u);
static_assert(sizeof(RenderTapePresentBoundary) == 16u);

enum class RenderTapeValidationStatus : std::uint8_t {
  Valid,
  MissingHeader,
  InvalidHeader,
  InvalidLayout,
  NonZeroReserved,
  InvalidEventType,
  InvalidEventFlags,
  InvalidEventOrdinal,
  InvalidEventRange,
  NonCanonicalEventLayout,
  NonZeroPadding,
  MissingCheckpoint,
  InvalidCheckpoint,
  InvalidIdentity,
  DuplicateIdentity,
  UnknownIdentity,
  InvalidObjectCreate,
  InvalidResourceWrite,
  InvalidCommandChunk,
  InvalidObjectDestroy,
  InvalidPresentBoundary,
  IncompleteFrame,
  ScratchAllocationFailed,
};

struct RenderTapeValidationResult {
  RenderTapeValidationStatus status = RenderTapeValidationStatus::MissingHeader;
  std::uint32_t failedEventIndex = 0xffffffffu;
  CommandChunkValidationStatus chunkStatus =
      CommandChunkValidationStatus::Valid;

  bool valid() const noexcept {
    return status == RenderTapeValidationStatus::Valid;
  }
};

struct ImportedRenderTapeEventView {
  RenderTapeEventHeader header{};
  std::span<const std::byte> payload{};
};

struct ImportedRenderTapeView {
  RenderTapeHeader header{};
  std::span<const RenderTapeEventHeader> events{};
  std::span<const std::byte> payloadArena{};

  ImportedRenderTapeEventView event(std::size_t index) const noexcept;
};

struct RenderTapeValidationScratch {
  struct LiveIdentity {
    D9CWireObjectIdentity identity{};
  };

  std::vector<LiveIdentity> liveObjects;
  std::vector<LiveIdentity> retainedObjects;
  CommandChunkValidationScratch chunk;
};

RenderTapeValidationResult
validateRenderTape(std::span<const std::byte> blob, ImportedRenderTapeView* out,
                   RenderTapeValidationScratch& scratch) noexcept;

RenderTapeValidationResult
validateRenderTape(std::span<const std::byte> blob,
                   ImportedRenderTapeView* out = nullptr) noexcept;

bool importPrevalidatedRenderTape(std::span<const std::byte> blob,
                                  ImportedRenderTapeView& out) noexcept;

class RenderTapeReplaySink {
public:
  virtual ~RenderTapeReplaySink() = default;

  virtual bool checkpoint(std::uint32_t stateVersion,
                          std::span<const D9CWireObjectIdentity> initialObjects,
                          std::span<const std::byte> state) = 0;
  virtual bool objectCreate(const RenderTapeObjectCreateHeader& fixed,
                            std::span<const std::byte> descriptor) = 0;
  virtual bool resourceWrite(const RenderTapeResourceWriteHeader& fixed,
                             std::span<const std::byte> data) = 0;
  virtual bool commandChunk(const CommandChunkEnvelope& envelope,
                            std::span<const std::byte> chunk) = 0;
  virtual bool objectDestroy(const D9CWireObjectIdentity& identity) = 0;
  virtual bool presentBoundary(const RenderTapePresentBoundary& boundary) = 0;
};

struct RenderTapeReplayResult {
  bool complete = false;
  std::uint32_t failedEventIndex = 0xffffffffu;
};

RenderTapeReplayResult
replayPrevalidatedRenderTape(const ImportedRenderTapeView& tape,
                             RenderTapeReplaySink& sink) noexcept;

class RenderTapeBuilder {
public:
  void appendCheckpoint(std::uint32_t stateVersion,
                        std::span<const D9CWireObjectIdentity> initialObjects,
                        std::span<const std::byte> state);
  void appendObjectCreate(const D9CWireObjectIdentity& identity,
                          std::uint32_t descriptorKind,
                          std::span<const std::byte> descriptor);
  void appendResourceWrite(const D9CWireObjectIdentity& identity,
                           std::uint32_t subresource, std::uint64_t byteOffset,
                           std::span<const std::byte> data);
  void appendCommandChunk(const CommandChunkEnvelope& envelope,
                          std::span<const std::byte> chunk);
  void appendObjectDestroy(const D9CWireObjectIdentity& identity);
  void appendPresentBoundary(std::uint64_t presentOrdinal);

  std::vector<std::byte> seal() const;
  std::size_t eventCount() const noexcept { return events_.size(); }

private:
  struct PendingEvent {
    RenderTapeEventType type = RenderTapeEventType::Checkpoint;
    std::vector<std::byte> payload;
  };

  void append(RenderTapeEventType type, std::vector<std::byte> payload);

  std::vector<PendingEvent> events_;
};

const char*
renderTapeValidationStatusName(RenderTapeValidationStatus status) noexcept;

} // namespace dxmt9::d3d9
