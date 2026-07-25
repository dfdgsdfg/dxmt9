#pragma once

// IRenderBackend — strategy seam for the modern-renderer transition (Task A1).
// A backend observes device/frame lifecycle and is handed each ready chunk.
// onChunkReady mirrors encoders::encodeChunk exactly so a Traditional backend
// can forward straight to it while a FrameGraph backend reinterprets the chunk.

// dxmt9_draw_encoder.hpp transitively provides everything this seam needs:
//   - encoders::EncodeContext + the encoders::encodeChunk declaration
//   - core::ChunkSlot (via dxmt9_backend_types.hpp)
//   - core::metalqueue::QueueSubmissionRecord (via dxmt9_queue.hpp)
#include "../dxmt9_draw_encoder.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace dxmt9::render {

enum class BackendMode { Traditional, FrameGraph };

struct BackendCaps {
  bool supports_mesh = false;
  bool supports_icb = false;
  bool supports_argbuf_tier2 = false;
  std::uint32_t max_mesh_threadgroup = 0;
};

class IRenderBackend {
 public:
  virtual ~IRenderBackend() = default;
  virtual void onDeviceCreated() {}
  virtual void onDeviceDestroyed() {}
  virtual void onFrameBegin(std::uint64_t /*frame_id*/) {}
  virtual void onFrameEnd() {}
  // Must mirror the signature/return type of encoders::encodeChunk exactly so a
  // Traditional backend can forward without an adapter (see dxmt9_draw_encoder.hpp).
  virtual std::optional<core::metalqueue::QueueSubmissionRecord> onChunkReady(
      encoders::EncodeContext& ctx,
      std::size_t slotIndex,
      const core::ChunkSlot& slot,
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
};

}  // namespace dxmt9::render
