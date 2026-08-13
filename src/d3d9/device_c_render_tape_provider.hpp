#pragma once

#include "device_c_render_tape.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

struct D9CDevice;

namespace dxmt9::d3d9 {

struct RenderTapeProviderBlob {
  RenderTapeDigest digest{};
  std::span<const std::byte> bytes{};
};

enum class FrameTapeReplayStatus : std::uint8_t {
  Complete,
  InvalidTape,
  InvalidBlobCatalogue,
  UnsupportedGrammar,
  ObjectCreationFailed,
  MutationFailed,
  BootstrapFailed,
  CommandReplayFailed,
  PresentOutputFailed,
  ReadbackFailed,
  OutputMismatch,
};

struct FrameTapeValidityEvidence {
  bool structurallyValid = false;
  bool digestsValid = false;
  bool outputReadback = false;
  bool expectedDigestCaptured = false;
  bool expectedDigestMatched = false;
  bool outputNonDegenerate = false;
  std::uint64_t outputBytes = 0u;
  RenderTapeDigest outputDigest{};
};

struct FrameTapeReplayRequirements {
  std::uint32_t outputWidth = 0u;
  std::uint32_t outputHeight = 0u;
  std::uint32_t outputFormat = 0u;
};

enum class FrameTapeBootstrapOutputDisposition : std::uint8_t {
  Malformed,
  ImplicitDefault,
  ExplicitExact,
  ExplicitNull,
  WrongIdentity,
  SlotOutOfRange,
  Ambiguous,
};

struct FrameTapeCoverageEvidence {
  std::uint32_t eventCount = 0u;
  std::uint32_t objectDefinitions = 0u;
  std::uint32_t seedMutations = 0u;
  std::uint32_t bootstrapChunks = 0u;
  std::uint32_t commandChunks = 0u;
  std::uint32_t commandRecords = 0u;
  std::uint32_t clearRecords = 0u;
  std::uint32_t drawPrimitiveUpRecords = 0u;
  std::uint32_t presentRecords = 0u;
  std::uint32_t presentOutputs = 0u;
};

struct FrameTapeConservationEvidence {
  std::uint32_t inputBlobs = 0u;
  std::uint32_t referencedBlobs = 0u;
  std::uint32_t objectsCreated = 0u;
  std::uint32_t objectsReleased = 0u;
  std::uint64_t presentOrdinal = 0u;
  std::uint64_t completionOrdinal = 0u;
};

struct FrameTapeReplayResult {
  FrameTapeReplayStatus status = FrameTapeReplayStatus::InvalidTape;
  std::uint32_t failedEventIndex = 0xffffffffu;
  FrameTapeReplayRequirements requirements{};
  FrameTapeValidityEvidence validity{};
  FrameTapeCoverageEvidence coverage{};
  FrameTapeConservationEvidence conservation{};

  bool complete() const noexcept {
    return status == FrameTapeReplayStatus::Complete;
  }
};

// Classifies the only accepted bootstrap attachment forms without changing
// the canonical wire bytes. The implicit form is the production default RT0
// created and bound by the replay device itself.
FrameTapeBootstrapOutputDisposition classifyFrameTapeBootstrapOutput(
    std::span<const std::byte> bytes, const CommandChunkEnvelope& envelope,
    const D9CWireObjectIdentity& output) noexcept;

// Pure checked extent predicate used immediately before a texture lock copy.
bool renderTapeTextureSeedExtentMatches(std::uint64_t blobBytes,
                                        std::int32_t pitch,
                                        std::uint32_t mipHeight) noexcept;

// Transactional admission for the one-frame identity grammar. No provider or
// device operation is performed until this returns Complete.
FrameTapeReplayResult preflightFrameTapeIdentity(
    std::span<const std::byte> tape,
    std::span<const RenderTapeProviderBlob> blobs) noexcept;

// Replays one preflighted Clear -> [textured DrawPrimitiveUP ->] Present frame
// through production factories, mutation APIs, DeviceReplaySink and the
// production queue. On Metal devices a typed offscreen PresentOutput is
// installed and read back after the completion waterline. Stub devices still
// exercise the exact replay routing deterministically.
FrameTapeReplayResult replayFrameTapeIdentity(
    D9CDevice* device, std::span<const std::byte> tape,
    std::span<const RenderTapeProviderBlob> blobs) noexcept;

const char* frameTapeReplayStatusName(FrameTapeReplayStatus status) noexcept;

} // namespace dxmt9::d3d9
