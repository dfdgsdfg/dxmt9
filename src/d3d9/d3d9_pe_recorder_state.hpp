#pragma once

// The recorder state is deliberately a flat owner.  D3D9DeviceImpl keeps
// references to these members for the mechanical migration below; the
// references do not add an allocation, virtual call, or setter-side work.
// Keeping the owner separate also gives item 6 a stable seam for moving the
// pure setter/settlement transitions without widening the COM object.

#include <mutex>
#include <type_traits>

#include "d3d9_pe_chunk_builder.hpp"
#include "d3d9_pe_producer_views.hpp"
#include "d3d9_pe_state_shadow.hpp"
#include "dxmt9/thread_ownership.hpp"

namespace dxmt9::d3d9::pe {

struct PeRecorderState {
  explicit PeRecorderState(bool lockRequired = false) noexcept
      : recorderLockRequired(lockRequired) {}

  // R-BACK-43.4 `producer-owned` (PE game thread). The ordinary D3D9 device
  // contract is single-threaded.  The recursive
  // mutex is only entered when D3DCREATE_MULTITHREADED (or the explicit
  // rollback knob) requires it; the ownership token catches contract
  // violations in debug builds on the unlocked path.
  bool recorderLockRequired = false;
  std::recursive_mutex recorderMutex{};
  dxmt9::core::ThreadOwnershipToken recorderOwnership{};

  PeHotStateShadow peState{};
  PeConstShadowBlock peConsts{};
  StateBlockRecorded stateBlock{};
  mutable PeBindingView peBindingView{};
  mutable PeSparseScratch peSparseScratch{};
  mutable SparseStateInput peSparseState{};
  mutable D9CCommandChunkWireDrawHeader peSparseHeader{};
  mutable PeDrawPayloads peSparsePayloads{};

  CommandChunkBuilder commandChunk{};
  bool commandChunkNegotiated = false;

  // These are recorder protocol state rather than diagnostics: a failed
  // append must leave them available for retry, and a successful commit must
  // advance them exactly once.
  std::uint64_t commandChunkCommits = 0;
  std::uint64_t commandChunkRecords = 0;
  std::uint64_t commandChunkBytes = 0;
};

}  // namespace dxmt9::d3d9::pe
