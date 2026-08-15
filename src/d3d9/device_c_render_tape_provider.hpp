#pragma once

#include "device_c_render_tape.hpp"

#include <array>
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
  bool expectedPixelsCompared = false;
  bool pixelEnvelopeMatched = false;
  bool outputNonDegenerate = false;
  std::uint64_t outputBytes = 0u;
  std::uint64_t allowedDifferingPixels = 0u;
  std::uint64_t differingPixels = 0u;
  std::uint64_t totalRgbDelta = 0u;
  std::uint64_t differingAlphaPixels = 0u;
  std::uint32_t maxRgbDelta = 0u;
  RenderTapeDigest expectedOutputDigest{};
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
  std::uint32_t drawPrimitiveRecords = 0u;
  std::uint32_t drawIndexedPrimitiveRecords = 0u;
  std::uint32_t drawPrimitiveUpRecords = 0u;
  std::uint32_t stateConstantRecords = 0u;
  std::uint32_t applyStateRecords = 0u;
  std::uint32_t presentRecords = 0u;
  std::uint32_t presentSourceMappings = 0u;
  std::uint32_t presentOutputs = 0u;
  std::uint32_t objectDestroys = 0u;
};

struct FrameTapeConservationEvidence {
  std::uint32_t inputBlobs = 0u;
  std::uint32_t referencedBlobs = 0u;
  std::uint32_t objectsCreated = 0u;
  std::uint32_t objectsReleased = 0u;
  std::uint64_t presentOrdinal = 0u;
  std::uint64_t completionOrdinal = 0u;
};

inline constexpr std::uint32_t kRenderTapeMaxReplayIntervals = 2u;

struct RenderTapeIntervalEvidence {
  std::uint64_t presentOrdinal = 0u;
  std::uint64_t completionOrdinal = 0u;
  FrameTapeValidityEvidence validity{};
};

struct FrameTapeReplayResult {
  FrameTapeReplayStatus status = FrameTapeReplayStatus::InvalidTape;
  std::uint32_t failedEventIndex = 0xffffffffu;
  std::uint32_t profile = 0u;
  std::uint32_t intervalCount = 0u;
  FrameTapeReplayRequirements requirements{};
  FrameTapeValidityEvidence validity{};
  FrameTapeCoverageEvidence coverage{};
  FrameTapeConservationEvidence conservation{};
  std::array<RenderTapeIntervalEvidence, kRenderTapeMaxReplayIntervals>
      intervals{};
  // Cold replay-harness artifact: tightly packed bytes from the last
  // offscreen Present readback. Production rendering never constructs this
  // result type, and callers that only need the digest may ignore the bytes.
  std::vector<std::byte> outputPixels{};

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

// Event-ordering predicate used by replay: a CPU-visible mutation or object
// retirement must not overtake command work submitted by an earlier event.
bool renderTapeProviderEventRequiresDrain(
    bool submittedCommandWork, RenderTapeEventType event) noexcept;

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

// Profile-aware entry points additionally admit exactly two textured Present
// intervals separated by one full digest-backed texture mutation. Both
// completions and output digests are conserved independently. The frame
// wrappers above retain their historical strictness and reject a sequence tape
// before provider effects.
FrameTapeReplayResult preflightRenderTapeIdentity(
    std::span<const std::byte> tape,
    std::span<const RenderTapeProviderBlob> blobs) noexcept;

FrameTapeReplayResult replayRenderTapeIdentity(
    D9CDevice* device, std::span<const std::byte> tape,
    std::span<const RenderTapeProviderBlob> blobs) noexcept;

// Applies the frame-tape pixel oracle only after strict digest comparison has
// failed. The expected bytes must be the digest-authenticated production
// capture sidecar; this never weakens tape/blob validation or sequence replay.
bool applyRenderTapePixelOracleEnvelope(
    FrameTapeReplayResult& result,
    std::span<const std::byte> expectedPixels) noexcept;

const char* frameTapeReplayStatusName(FrameTapeReplayStatus status) noexcept;

} // namespace dxmt9::d3d9
