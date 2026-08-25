#pragma once

// R-BACK-44.2a — the concrete rename-ring lease a committed Managed buffer
// mutation task owns until it is applied or terminally discarded, and the
// per-apply byte accounting the worker reports.
//
// This lives in its own header, rather than in `dxmt9_resource_pool.hpp` where
// the ring itself is defined, because both ends of the handoff need it and only
// one of them is a Metal translation unit: the producer/worker side
// (`src/d3d9/device_c_replay_offload.hpp`) carries the lease inside a queue
// task and must not pull `Metal.hpp` in to do so. The Metal object is therefore
// named by its opaque handle value, exactly as
// `core::DrawBufferBindingSnapshot::metalHandle` names it.

#include <cstddef>
#include <cstdint>
#include <memory>

namespace dxmt9::resources {

struct ManagedBufferMutationLease {
  // False means "no rotation happened": the handle did not resolve, or the
  // record was not Managed-versioned. R-BACK-44.2 relies on this being an
  // all-or-nothing answer — an invalid lease is a pre-effect rejection with no
  // rotation, no revision bump, and nothing for the caller to unwind.
  bool valid = false;
  // Index into `BufferRecord::renameRing`. Stable for the life of the record:
  // the ring only ever grows, and the lease's `backingResidency` reference
  // keeps `rotateBufferBacking` from re-selecting this entry meanwhile.
  std::uint32_t renameIndex = 0;
  // Identity of the leased entry at rotation time. The worker re-checks both
  // against the live ring before writing a byte, so a lease that somehow
  // stopped naming the same allocation fails the apply (and therefore
  // fail-stops) instead of scribbling on an unrelated backing.
  std::uint64_t metalHandle = 0;
  void* contents = nullptr;
  // `BufferRecord::contentRevision` as published by the synchronous rotation.
  // Every commit-time capture taken after the unlock returned observes this
  // value, which is what `captureRevisionIsCurrent` asserts.
  std::uint64_t contentRevision = 0;
  std::uint64_t byteSize = 0;
  // The same `BufferRenameRingEntry::replayResidency` reference a chunk
  // capture takes. Holding it is what makes the leased entry ineligible for
  // reuse by a later rotation while this task is still queued.
  std::shared_ptr<void> backingResidency;
};

struct ManagedBufferMutationApplyResult {
  bool applied = false;
  // Untouched region carried forward from the pool CPU shadow into the leased
  // backing, and the staged dirty span written into both. Their sum is the
  // logical buffer size whenever the record has CPU-visible `contents`.
  std::uint64_t copyForwardBytes = 0;
  std::uint64_t patchBytes = 0;
};

}  // namespace dxmt9::resources
