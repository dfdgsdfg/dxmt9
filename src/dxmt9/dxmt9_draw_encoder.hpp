#pragma once

// Free-function draw encoder — lifted from MetalBackendDevice (Step 3d).
// Hosts the EncodeContext bundle, makeSampler, beginRenderPass,
// encodeDraw, and encodeChunk (3c-remainder).

#include "../winemetal/Metal.hpp"
#include "dxmt9_backend_types.hpp"
// Full CommandQueue type needed by EncodeContext::queue (reference) and
// PreUploadedDrawData::{vertex,index} (CommandQueue::TransientBufferSlice
// member). The previous forward decl of CommandQueue worked while only a
// reference was used; the Phase 5-B PreUploadedDrawData struct holds the
// nested type by value, requiring the complete definition.
#include "dxmt9_command_queue.hpp"
#include "dxmt9_queue.hpp"
#include "dxmt9_uniform_dirty.hpp"
#include "dxmt9/core.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace dxmt9 {

class Device;

namespace resources { struct Pool; }
namespace pipeline { class Cache; }
namespace scratch { struct FrameAllocators; }

namespace encoders {

// Bundle of references the draw encoder needs from its owner (DeviceImpl
// during transition). All references must outlive the current chunk
// encode pass. Not yet consumed by encodeDraw — prepared for the body
// extraction that follows.
struct EncodeContext {
  WMT::Reference<WMT::Device> device;
  const core::BackendLimits& limits;
  resources::Pool& pool;
  pipeline::Cache& cache;
  scratch::FrameAllocators& allocators;
  WMT::Reference<WMT::BinaryArchive>* shaderArchive;
  const std::string* shaderArchivePath;
  CommandQueue& queue;
  // Per-encoder draw-uniforms dirty state (R-BACK-12.8..12.12). Default
  // construction = all clean; C2 (encoder consumption) calls
  // markAllDirty(dirty) at encoder init and clears bits as it issues
  // sub-allocations / binds. C1 sets the bits as records arrive.
  uniform::DirtyState dirty{};
};

// Sampler factory helpers used by the draw encoder. Previously
// MetalBackendDevice::makeSampler. Split into nearest/linear (the "bool
// linear" variant), a pure SamplerSnapshot -> WMTSamplerInfo mapper, and the
// full SamplerSnapshot variant that creates the Metal object.
WMT::Reference<WMT::SamplerState> makeSampler(WMT::Reference<WMT::Device> device, bool linear);
WMTSamplerInfo makeSamplerInfo(const core::SamplerSnapshot& snapshot);
WMTSamplerInfo makeSamplerInfo(const core::FlatStateSet<core::kMaxSamplerStates>& states);
WMT::Reference<WMT::SamplerState> makeSampler(WMT::Reference<WMT::Device> device,
                                                const core::SamplerSnapshot& snapshot);
WMT::Reference<WMT::SamplerState> makeSampler(WMT::Reference<WMT::Device> device,
                                                const core::FlatStateSet<core::kMaxSamplerStates>& states);

// Render-pass setup. Builds a WMTRenderPassInfo from the current draw
// target + optional clear, then begins the encoder on the given command
// buffer. Reads (and clears, on entry) ctx.queue.backBufferDiscardAfterPresent_
// so the next draw to the same RT can choose DontCare over Load.
WMT::Reference<WMT::RenderCommandEncoder> beginRenderPass(
    EncodeContext& ctx,
    WMT::CommandBuffer& commandBuffer,
    core::FlatDrawStateView drawState,
    const std::optional<core::ClearDesc>& clear);

// Pre-uploaded transient slices for a single draw within a DrawRun
// batch (Phase 5-B). When the Kind::DrawRun handler pre-batches all UP
// vertex/index payloads for the run via
// CommandQueue::uploadTransientBufferBatch, it hands the per-draw
// resolved slices in via this struct so encodeDraw skips its own
// per-draw makeTransientBuffer calls. Empty slices fall through to
// the existing per-draw upload.
struct PreUploadedDrawData {
  CommandQueue::TransientBufferSlice vertex{};
  CommandQueue::TransientBufferSlice index{};
};

// Main per-draw encoder. Previously MetalBackendDevice::encodeDraw.
// Consumes ctx.cache for pipeline lookup, ctx.pool for resource reads,
// and ctx.queue for transient buffer slabs (per-frequency UBOs).
//
// `skipBaseStateBind` (Phase 3-E): when true, skip the BaseDrawState
// binding work that doesn't change between draws sharing one
// BackendDrawRunRecord — pipeline lookup, depth state, render
// pipeline state, viewport / scissor / cull, texture / sampler
// binding. Used by the Kind::DrawRun handler for iterations 2..N
// after iteration 1 has already bound the base state into the Metal
// render encoder. The per-draw issue path (per-frequency UBO bind on
// dirty, DrawVolatile push via setVertexBytes, vertex/index prep,
// drawPrimitives/drawIndexedPrimitives) always runs.
//
// Returns true after a Metal draw call is emitted. Callers use this to
// record that the FlatDrawStateKey is now live on the active render encoder.
//
// `paramOverride` (Phase 13 step 2): when non-null, the per-draw
// fields (primitiveType, primitiveCount, startVertex, baseVertexIndex,
// startIndex, indexType, user payload ranges/data) are read from the
// override. `paramPayloadArena` resolves arena-backed ranges when present;
// vectors remain the fallback. All other fields
// (RT/DS/VS/PS/VDecl/VBuffers/IB/viewport/scissor/render-state/transform)
// come from `drawState`'s hot record and shader layout.
//
// `dirty` (R-BACK-12): per-render-encoder uniform dirty state. Bits gate
// the per-frequency UBO sub-allocation + bind sequence; encodeDraw
// reads, binds, and clears matching bits. Caller passes a pointer that
// lives across all draws on one Metal render encoder and calls
// markAllDirty(...) when a fresh encoder opens (R-BACK-12.12). nullptr
// is treated as "every draw rebinds everything".
bool encodeDraw(EncodeContext& ctx,
                 WMT::CommandBuffer& commandBuffer,
                 WMT::RenderCommandEncoder& encoder,
                 core::FlatDrawStateView drawState,
                 std::uint64_t seqId,
                 bool skipBaseStateBind = false,
                 const PreUploadedDrawData* preUploaded = nullptr,
                 const core::DrawParam* paramOverride = nullptr,
                 std::span<const std::uint8_t> paramPayloadArena = {},
                 uniform::DirtyState* dirty = nullptr);

// Encode a single chunk's commands into a fresh WMT::CommandBuffer.
// Returns a QueueSubmissionRecord that the finish loop commits; nullopt
// on allocation failure. Previously MetalBackendDevice::encodeChunk.
std::optional<core::metalqueue::QueueSubmissionRecord> encodeChunk(
    EncodeContext& ctx,
    std::size_t slotIndex,
    const core::ChunkSlot& slot);

}  // namespace encoders
}  // namespace dxmt9
