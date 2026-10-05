#include "device_c_render_tape_mutation_admission_v4.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace dxmt9::d3d9;
using Decision = RenderTapeMutationDrainV4Decision;
using Phase = RenderTapeInitialSetupV4OwnerPhase;
using Provenance = RenderTapeMutationDrainV4ContentProvenance;
std::uint64_t checks = 0u;

void check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

struct Fixture {
  std::array<std::byte, sizeof(D9CBufferDesc)> bytes{};
  D9CBufferDesc descriptor{64u, 0x208u, 0u, 0u, 0u};
  RenderTapeMutationDrainV4Input input{};

  explicit Fixture(bool prefix) {
    const D9CWireObjectIdentity identity{D9C_CHUNK_HANDLE_KIND_BUFFER, 7u, 11u};
    input.sourcePlanDigest[0] = std::byte{13u};
    input.snapshotPrefix = 8u;
    input.definitionEventIndex = prefix ? 2u : 9u;
    input.mutationEventIndex = 12u;
    input.definition = {.identity = identity,
        .descriptorKind = static_cast<std::uint32_t>(RenderTapeDescriptorKind::Buffer),
        .descriptorBytes = sizeof(D9CBufferDesc), .expectedContentBytes = descriptor.size,
        .expectedContentCount = 1u};
    input.mutation = {.identity = identity,
        .kind = static_cast<std::uint32_t>(RenderTapeMutationKind::CpuUnlock),
        .bufferDisposition = static_cast<std::uint32_t>(RenderTapeBufferMutationDisposition::NoOverwrite),
        .byteOffset = 16u, .byteSize = 8u};
    input.activeIdentity = identity;
    input.ownerPhase = Phase::Active;
    input.initialContent = {
        .provenance = prefix ? Provenance::PrefixCheckpoint : Provenance::SetupCheckpointAndConsumed,
        .sourcePlanDigest = input.sourcePlanDigest,
        .definitionEventIndex = input.definitionEventIndex,
        .initialUploadEventIndex = input.definitionEventIndex + 1u,
        .initialUpload = {.identity = identity,
            .kind = static_cast<std::uint32_t>(RenderTapeMutationKind::Upload),
            .bufferDisposition = static_cast<std::uint32_t>(RenderTapeBufferMutationDisposition::Plain),
            .byteSize = descriptor.size},
        .completedBytes = descriptor.size, .completedCount = 1u, .consumedSeeds = 1u};
    bindDescriptor();
  }

  void bindDescriptor() {
    std::memcpy(bytes.data(), &descriptor, sizeof(descriptor));
    input.descriptor = bytes;
  }
  bool bypasses() const { return renderTapeMutationDrainV4Decision(input) == Decision::OmitProviderPreflush; }
};

void finiteOriginalWriteClassTruthTable() {
  for (const bool prefix : {false, true}) {
    Fixture fixture(prefix);
    for (const auto phase : {Phase::Unprepared, Phase::Hidden, Phase::Active, Phase::Released}) {
      for (const std::uint32_t pool : {0u, 1u, 2u}) {
        for (const std::uint32_t usage : {0u, 0x200u, 0x208u}) {
          for (const std::uint32_t format : {0u, 101u, 102u, 103u}) {
            for (const auto kind : {RenderTapeMutationKind::CpuUnlock,
                     RenderTapeMutationKind::Upload, RenderTapeMutationKind::Palette}) {
              for (const std::uint32_t disposition : {0u, 1u, 2u, 3u, 0x1000u}) {
                fixture.input.ownerPhase = phase;
                fixture.descriptor.pool = pool;
                fixture.descriptor.usage = usage;
                fixture.descriptor.format = format;
                fixture.bindDescriptor();
                fixture.input.mutation.kind = static_cast<std::uint32_t>(kind);
                fixture.input.mutation.bufferDisposition = disposition;
                const bool allowed = phase == Phase::Active && pool == 0u && usage != 0u &&
                    format != 103u && kind == RenderTapeMutationKind::CpuUnlock && disposition == 1u;
                check(fixture.bypasses() == allowed,
                    "only active exact DEFAULT+DYNAMIC original CpuUnlock/NoOverwrite classes omit the extra flush");
              }
            }
          }
        }
      }
    }
  }
}

void identitiesExtentsAndOriginalContentCannotBeBorrowed() {
  for (const bool prefix : {false, true}) {
    for (std::uint32_t variant = 0u; variant < 33u; ++variant) {
      Fixture fixture(prefix);
      auto& input = fixture.input;
      auto& proof = input.initialContent;
      if (variant == 0u) ++input.activeIdentity.generation;
      if (variant == 1u) ++input.mutation.identity.generation;
      if (variant == 2u) ++proof.initialUpload.identity.generation;
      if (variant == 3u) ++input.activeIdentity.objectId;
      if (variant == 4u) proof.sourcePlanDigest[0] = std::byte{14u};
      if (variant == 5u) ++proof.definitionEventIndex;
      if (variant == 6u) proof.provenance = Provenance::Unavailable;
      if (variant == 7u) proof.completedBytes = 63u;
      if (variant == 8u) proof.completedCount = 0u;
      if (variant == 9u) proof.consumedSeeds = 0u;
      if (variant == 10u) proof.consumedSeeds = 2u;
      if (variant == 11u) proof.initialUpload.kind = static_cast<std::uint32_t>(RenderTapeMutationKind::CpuUnlock);
      if (variant == 12u) proof.initialUpload.bufferDisposition = 1u;
      if (variant == 13u) proof.initialUpload.byteOffset = 1u;
      if (variant == 14u) proof.initialUpload.byteSize = 63u;
      if (variant == 15u) proof.initialUpload.subresource = 1u;
      if (variant == 16u) input.definition.expectedContentBytes = 63u;
      if (variant == 17u) input.definition.expectedContentCount = 2u;
      if (variant == 18u) input.mutation.subresource = 1u;
      if (variant == 19u) input.mutation.byteSize = 0u;
      if (variant == 20u) input.mutation.byteOffset = 65u;
      if (variant == 21u) input.mutation.byteOffset = 63u;
      if (variant == 22u) input.mutation.byteOffset = std::numeric_limits<std::uint64_t>::max();
      if (variant == 23u) input.mutation.byteSize = std::numeric_limits<std::uint64_t>::max();
      if (variant == 24u) input.definition.descriptorKind = static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture);
      if (variant == 25u) --input.definition.descriptorBytes;
      if (variant == 26u) input.descriptor = input.descriptor.first(sizeof(D9CBufferDesc) - 1u);
      if (variant == 27u) input.snapshotPrefix = kRenderTapeIdentityV4NoIndex;
      if (variant == 28u) input.mutationEventIndex = input.snapshotPrefix - 1u;
      if (variant == 29u) proof.initialUploadEventIndex = input.mutationEventIndex;
      if (variant == 30u) proof.initialUploadEventIndex = input.definitionEventIndex;
      if (variant == 31u) input.definition.identity.generation = 0u;
      if (variant == 32u) {
        fixture.descriptor.format = 101u;
        fixture.descriptor.fvf = 1u;
        fixture.bindDescriptor();
      }
      check(!fixture.bypasses(),
          "stale, partial, missing, malformed, mixed, out-of-bounds or wrong-event facts retain the drain");
    }
    Fixture fixture(prefix);
    fixture.input.ownerPhase = Phase::Released;
    check(!fixture.bypasses(), "destruction revokes the exact active-owner premise");
    ++fixture.input.definition.identity.generation;
    fixture.input.activeIdentity = fixture.input.definition.identity;
    fixture.input.mutation.identity = fixture.input.definition.identity;
    fixture.input.ownerPhase = Phase::Active;
    check(!fixture.bypasses(), "redefinition cannot inherit the old generation's completed full seed");
    fixture.input.definition.identity.kind = D9C_CHUNK_HANDLE_KIND_TEXTURE;
    fixture.input.activeIdentity = fixture.input.definition.identity;
    fixture.input.mutation.identity = fixture.input.definition.identity;
    check(!fixture.bypasses(), "a texture never inherits buffer NoOverwrite drain admission");
  }
}

void completionProvenanceAndBoundaryAreDistinct() {
  Fixture prefix(true), late(false);
  check(prefix.bypasses() && late.bypasses(), "both honest initial-content provenance variants are admitted");
  prefix.input.initialContent.initialUploadEventIndex = prefix.input.snapshotPrefix;
  check(!prefix.bypasses(), "a seed at the interval boundary is not a completed prefix seed");
  prefix.input.initialContent.initialUploadEventIndex = 7u;
  check(prefix.bypasses(), "an authenticated prefix full seed need not be adjacent to its definition");
  prefix.input.initialContent.provenance = Provenance::SetupCheckpointAndConsumed;
  late.input.initialContent.provenance = Provenance::PrefixCheckpoint;
  check(!prefix.bypasses() && !late.bypasses(), "late and prefix completion witnesses cannot exchange authority");
  late.input.initialContent.provenance = Provenance::SetupCheckpointAndConsumed;
  late.input.initialContent.initialUploadEventIndex = 11u;
  check(!late.bypasses(), "late setup's canonical original full Upload must directly follow its definition");

  Fixture full(false);
  full.input.mutation.byteOffset = 0u;
  full.input.mutation.byteSize = 64u;
  check(full.bypasses(), "a recorded full-size NoOverwrite range is bounded, not automatically a Discard");
  full.input.mutation.byteOffset = 63u;
  full.input.mutation.byteSize = 1u;
  check(full.bypasses(), "a nonempty final byte is accepted without adding overflow-prone endpoints");
}

// Value-only distinguishing sequence. It consumes the actual admission
// decision; it does not manufacture provider/GPU publication evidence.
struct PendingClearSequence {
  bool pendingClear = true;
  bool endSawPendingClear = false;
  std::uint32_t extraFlushes = 0u, writes = 0u;
  void mutation(const RenderTapeMutationDrainV4Input& input) {
    if (renderTapeMutationDrainV4Decision(input) == Decision::KeepDrain) {
      ++extraFlushes;
      pendingClear = false;
    }
    ++writes; // Both decisions still apply the original mutation exactly once.
  }
  void end() { endSawPendingClear = pendingClear; }
};

void pendingClearNoOverwriteEndKeepsTheOriginalCut() {
  for (const bool prefix : {false, true}) {
    Fixture fixture(prefix);
    PendingClearSequence admitted;
    admitted.mutation(fixture.input);
    admitted.end();
    check(admitted.extraFlushes == 0u && admitted.writes == 1u && admitted.endSawPendingClear,
        "pending Clear crosses original NoOverwrite and reaches END without a provider-inserted flush");
    for (std::uint32_t variant = 0u; variant < 7u; ++variant) {
      Fixture rejected(prefix);
      if (variant == 0u) rejected.input.mutation.bufferDisposition = 0u;
      if (variant == 1u) rejected.input.mutation.bufferDisposition = 2u;
      if (variant == 2u) rejected.input.mutation.kind = static_cast<std::uint32_t>(RenderTapeMutationKind::Upload);
      if (variant == 3u) {
        rejected.descriptor.pool = 1u;
        rejected.bindDescriptor();
      }
      if (variant == 4u) rejected.input.ownerPhase = Phase::Hidden;
      if (variant == 5u) ++rejected.input.activeIdentity.generation;
      if (variant == 6u) rejected.input.initialContent.initialUpload.byteSize = 8u;
      PendingClearSequence conservative;
      conservative.mutation(rejected.input);
      conservative.end();
      check(conservative.extraFlushes == 1u && conservative.writes == 1u &&
          !conservative.endSawPendingClear,
          "rejected classes retain their drain and still execute the original write once");
    }
  }
}
} // namespace

int main() {
  try {
    finiteOriginalWriteClassTruthTable();
    identitiesExtentsAndOriginalContentCannotBeBorrowed();
    completionProvenanceAndBoundaryAreDistinct();
    pendingClearNoOverwriteEndKeepsTheOriginalCut();
    std::cout << "render tape mutation admission v4: " << checks << " checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "render tape mutation admission v4: " << error.what() << '\n';
    return 1;
  }
}
