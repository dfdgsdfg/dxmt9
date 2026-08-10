#pragma once

// IRenderBackend — strategy seam for the modern-renderer transition (Task A1).
// A backend observes device/frame lifecycle and is handed each ready source.
// onSourceReady mirrors the source-neutral encoders::encodeChunk entry point;
// onChunkReady remains the legacy ChunkSlot compatibility wrapper.

#include "../dxmt9_backend_types.hpp"
#include "../dxmt9_encode_chunk_types.hpp"
#include "../framegraph/fg_multi_source_planner.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace dxmt9::resources { struct Pool; }

namespace dxmt9::render {

enum class BackendMode { Traditional, FrameGraph };

struct BackendCaps {
  bool supports_mesh = false;
  bool supports_icb = false;
  bool supports_argbuf_tier2 = false;
  std::uint32_t max_mesh_threadgroup = 0;
};

// Typed replay frontier for bounded cross-source planning. A clean frontier
// carries no virtual predecessor; an active frontier must carry one complete
// dependency snapshot. All other storage states are ineligible.
struct MultiSourceSessionReplayFrontier {
  encoders::EncodeSessionReplayFrontierState state =
      encoders::EncodeSessionReplayFrontierState::InjectedUnknown;
  std::optional<encoders::ActiveRenderDependencySnapshot> activeRender{};
  bool collectActiveSeedMergeWitnesses = false;

  bool valid() const noexcept {
    if (state == encoders::EncodeSessionReplayFrontierState::
                     CleanClosedEncoderNoPendingClear) {
      return !activeRender.has_value();
    }
    return state == encoders::EncodeSessionReplayFrontierState::
                        ActiveRenderComplete &&
           activeRender.has_value() && activeRender->complete;
  }
};

class IRenderBackend {
 public:
  virtual ~IRenderBackend() = default;
  virtual void onDeviceCreated() {}
  virtual void onDeviceDestroyed() {}
  virtual void onFrameBegin(std::uint64_t /*frame_id*/) {}
  virtual void onFrameEnd() {}
  // Legacy compatibility entry point. Implementations must forward through
  // onSourceReady so Arena and ChunkSlot sources share one production path.
  virtual std::optional<core::metalqueue::QueueSubmissionRecord> onChunkReady(
      encoders::EncodeContext& ctx,
      std::size_t slotIndex,
      const core::ChunkSlot& slot,
      encoders::EncodeChunkOptions options = {}) {
    return onSourceReady(ctx, slotIndex, core::SourcePayloadView(slot),
                         slot.seqId, std::move(options));
  }
  virtual std::optional<core::metalqueue::QueueSubmissionRecord> onSourceReady(
      encoders::EncodeContext& ctx,
      std::size_t slotIndex,
      core::SourcePayloadView payload,
      std::uint64_t seqId,
      encoders::EncodeChunkOptions options = {}) = 0;
  virtual BackendMode mode() const = 0;
  virtual BackendCaps caps() const { return {}; }
  // Opt-in queue planning seam for bounded cross-chunk proofs. The queue may
  // retain one dequeued Encoding slot while it encodes a safe prefix, then use
  // an already-ready FIFO successor or fail open without waiting. It must
  // preserve per-chunk submission/completion order. Traditional and default
  // FrameGraph paths return false.
  virtual bool wantsNextChunkLookahead() const { return false; }
  // Optional scheduling hint for the DCE lookahead lane. The backend may use
  // previously observed proof shape to identify a non-empty prefix of the
  // optimized replay permutation that can be encoded before the actual
  // successor is available.
  // This hint never authorizes omission; onChunkReady must still validate the
  // selected successor before producing a DCE replay subset.
  virtual std::vector<std::uint32_t> dceLookaheadReplayPrefix(
      encoders::EncodeContext& /*ctx*/,
      std::size_t /*slotIndex*/,
      const core::ChunkSlot& /*slot*/) {
    return {};
  }
  // Bounded, side-effect-free planning seam for a FIFO window pinned by a
  // TentativeRepresented reservation. The coordinator calls this with its
  // scheduling mutex released, then revalidates the exact reservation before
  // committing it. The default is InvalidInput so non-FrameGraph backends and
  // unsupported feature profiles preserve the natural per-source loop.
  virtual framegraph::MultiSourceReplayPlan planMultiSourceSessionReplay(
      const resources::Pool& /*pool*/,
      std::span<const core::metalqueue::ResolvedPublishedSource> /*sources*/,
      const MultiSourceSessionReplayFrontier& /*frontier*/) {
    return {};
  }
  // Transaction-level observer seam for a qualified composite replay. After
  // every fragment effect and carrier fold succeeds, the queue invokes it
  // exactly once in natural FIFO source order with the scheduling mutex
  // released. Fragment calls set skipBackendPlanning so per-source debug
  // observation is neither lost nor duplicated.
  virtual void observeMultiSourceSessionReplay(
      const resources::Pool& /*pool*/,
      std::span<const core::metalqueue::ResolvedPublishedSource> /*sources*/) {}
};

}  // namespace dxmt9::render
