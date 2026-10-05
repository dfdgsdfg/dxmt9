#pragma once

#include "device_c_render_tape_initial_setup_v4.hpp"

#include <cstdint>
#include <cstring>
#include <span>

namespace dxmt9::d3d9 {

enum class RenderTapeMutationDrainV4Decision : std::uint8_t {
  KeepDrain,
  OmitProviderPreflush,
};

enum class RenderTapeMutationDrainV4ContentProvenance : std::uint8_t {
  Unavailable,
  PrefixCheckpoint,
  SetupCheckpointAndConsumed,
};

// The provider constructs this only from an admitted original full Upload
// and its real checked pre-arm completion. SetupCheckpointAndConsumed also
// requires successful logical consumption on the current active setup owner.
// A later partial write or seededSubresources membership cannot establish it.
struct RenderTapeMutationDrainV4ContentProof {
  RenderTapeMutationDrainV4ContentProvenance provenance =
      RenderTapeMutationDrainV4ContentProvenance::Unavailable;
  RenderTapeDigest sourcePlanDigest{};
  std::uint32_t definitionEventIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t initialUploadEventIndex = kRenderTapeIdentityV4NoIndex;
  RenderTapeResourceMutationHeader initialUpload{};
  std::uint64_t completedBytes = 0u;
  std::uint32_t completedCount = 0u;
  std::uint32_t consumedSeeds = 0u;
};

// Cold, synchronous value input. The provider must already have authenticated
// the General plan against this valid V4 source-plan digest; these event
// indices, definition/descriptor and mutation come from that same bound plan.
// The helper checks provenance equality, not authentication by hash recompute.
// Descriptor storage is borrowed only for this call, never retained.
struct RenderTapeMutationDrainV4Input {
  RenderTapeDigest sourcePlanDigest{};
  std::uint32_t snapshotPrefix = kRenderTapeIdentityV4NoIndex;
  std::uint32_t definitionEventIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t mutationEventIndex = kRenderTapeIdentityV4NoIndex;
  RenderTapeObjectDefineHeader definition{};
  std::span<const std::byte> descriptor{};
  RenderTapeResourceMutationHeader mutation{};
  D9CWireObjectIdentity activeIdentity{};
  RenderTapeInitialSetupV4OwnerPhase ownerPhase =
      RenderTapeInitialSetupV4OwnerPhase::Unprepared;
  RenderTapeMutationDrainV4ContentProof initialContent{};
};

// This is narrower than bufferLockClassBypassesReplay: it grants no Managed
// or Discard policy. It omits only the General provider's additional flush;
// production applyGeneralMutation, Lock/Unlock and NoOverwrite range upload
// remain unchanged. D3D9's disjoint-range promise is the semantic premise.
inline RenderTapeMutationDrainV4Decision renderTapeMutationDrainV4Decision(
    const RenderTapeMutationDrainV4Input& input) noexcept {
  using Decision = RenderTapeMutationDrainV4Decision;
  using Provenance = RenderTapeMutationDrainV4ContentProvenance;
  constexpr std::uint32_t d3dPoolDefault = 0u;
  constexpr std::uint32_t d3dUsageDynamic = 0x00000200u;
  const auto& definition = input.definition;
  const auto& mutation = input.mutation;
  const auto& proof = input.initialContent;
  if (input.snapshotPrefix == kRenderTapeIdentityV4NoIndex ||
      input.definitionEventIndex == kRenderTapeIdentityV4NoIndex ||
      input.mutationEventIndex == kRenderTapeIdentityV4NoIndex ||
      input.mutationEventIndex < input.snapshotPrefix ||
      input.ownerPhase != RenderTapeInitialSetupV4OwnerPhase::Active ||
      definition.identity.kind != D9C_CHUNK_HANDLE_KIND_BUFFER ||
      !definition.identity.generation || !definition.identity.objectId ||
      definition.descriptorKind != static_cast<std::uint32_t>(RenderTapeDescriptorKind::Buffer) ||
      definition.descriptorBytes != sizeof(D9CBufferDesc) ||
      input.descriptor.size() != sizeof(D9CBufferDesc) || !input.descriptor.data() ||
      !renderTapeIdentityV3SameIdentity(definition.identity, input.activeIdentity) ||
      !renderTapeIdentityV3SameIdentity(definition.identity, mutation.identity) ||
      mutation.kind != static_cast<std::uint32_t>(RenderTapeMutationKind::CpuUnlock) ||
      mutation.bufferDisposition != static_cast<std::uint32_t>(
          RenderTapeBufferMutationDisposition::NoOverwrite) ||
      mutation.subresource || !mutation.byteSize) return Decision::KeepDrain;

  D9CBufferDesc buffer{};
  std::memcpy(&buffer, input.descriptor.data(), sizeof(buffer));
  // D3DFMT_INDEX16/INDEX32 are the only admitted index-buffer formats. Zero
  // selects a vertex buffer; an index descriptor has no vertex FVF.
  if (!buffer.size || buffer.pool != d3dPoolDefault ||
      !(buffer.usage & d3dUsageDynamic) ||
      (buffer.format != 0u && buffer.format != 101u && buffer.format != 102u) ||
      (buffer.format != 0u && buffer.fvf != 0u) ||
      mutation.byteOffset > buffer.size ||
      mutation.byteSize > static_cast<std::uint64_t>(buffer.size) - mutation.byteOffset ||
      definition.expectedContentCount != 1u ||
      definition.expectedContentBytes != buffer.size ||
      proof.sourcePlanDigest != input.sourcePlanDigest ||
      proof.definitionEventIndex != input.definitionEventIndex ||
      proof.initialUploadEventIndex == kRenderTapeIdentityV4NoIndex ||
      input.definitionEventIndex >= proof.initialUploadEventIndex ||
      proof.initialUploadEventIndex >= input.mutationEventIndex ||
      proof.completedBytes != buffer.size || proof.completedCount != 1u ||
      proof.consumedSeeds != 1u) return Decision::KeepDrain;

  const auto& seed = proof.initialUpload;
  if (!renderTapeIdentityV3SameIdentity(seed.identity, definition.identity) ||
      seed.kind != static_cast<std::uint32_t>(RenderTapeMutationKind::Upload) ||
      seed.bufferDisposition != static_cast<std::uint32_t>(RenderTapeBufferMutationDisposition::Plain) ||
      seed.subresource || seed.byteOffset || seed.byteSize != buffer.size)
    return Decision::KeepDrain;

  if (proof.provenance == Provenance::PrefixCheckpoint) {
    if (proof.initialUploadEventIndex >= input.snapshotPrefix) return Decision::KeepDrain;
  } else if (proof.provenance == Provenance::SetupCheckpointAndConsumed) {
    if (input.definitionEventIndex < input.snapshotPrefix ||
        proof.initialUploadEventIndex - input.definitionEventIndex != 1u)
      return Decision::KeepDrain;
  } else {
    return Decision::KeepDrain;
  }
  return Decision::OmitProviderPreflush;
}

} // namespace dxmt9::d3d9
