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
#include "dxmt9/core.hpp"

#include <array>
#include <cstdint>
#include <optional>
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
};

// Sampler factory helpers used by the draw encoder. Previously
// MetalBackendDevice::makeSampler. Split into nearest/linear (the "bool
// linear" variant), a pure SamplerSnapshot -> WMTSamplerInfo mapper, and the
// full SamplerSnapshot variant that creates the Metal object.
WMT::Reference<WMT::SamplerState> makeSampler(WMT::Reference<WMT::Device> device, bool linear);
WMTSamplerInfo makeSamplerInfo(const core::SamplerSnapshot& snapshot);
WMT::Reference<WMT::SamplerState> makeSampler(WMT::Reference<WMT::Device> device,
                                                const core::SamplerSnapshot& snapshot);

// Render-pass setup. Builds a WMTRenderPassInfo from the current draw
// target + optional clear, then begins the encoder on the given command
// buffer. Reads (and clears, on entry) ctx.queue.backBufferDiscardAfterPresent_
// so the next draw to the same RT can choose DontCare over Load.
WMT::Reference<WMT::RenderCommandEncoder> beginRenderPass(
    EncodeContext& ctx,
    WMT::CommandBuffer& commandBuffer,
    const core::DrawDesc& draw,
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
// Consumes ctx.allocators.argbuf for the transient DrawUniforms buffer,
// ctx.cache for pipeline lookup, ctx.pool for resource reads, and
// ctx.device for transient buffer allocation.
//
// `skipBaseStateBind` (Phase 3-E): when true, skip the BaseDrawState
// binding work that doesn't change between draws sharing one
// BackendDrawRunRecord — pipeline lookup, depth state, render
// pipeline state, viewport / scissor / cull, texture / sampler
// binding. Used by the Kind::DrawRun handler for iterations 2..N
// after iteration 1 has already bound the base state into the Metal
// render encoder. The per-draw issue path (DrawUniforms upload,
// vertex/index prep, drawPrimitives/drawIndexedPrimitives) always
// runs.
//
// `paramOverride` (Phase 13 step 2): when non-null, the 8 per-draw
// fields (primitiveType, primitiveCount, startVertex, baseVertexIndex,
// startIndex, indexType, userVertexData, userIndexData) are read from
// the override instead of `draw`. All other fields (RT/DS/VS/PS/VDecl/
// VBuffers/IB/viewport/scissor/render-state/transform/etc.) still come
// from `draw`. Lets the Kind::DrawRun handler skip the synthetic
// DrawDesc copy + per-iter scalar overrides + per-iter UP byte vector
// assigns. `draw` then represents the run's base state once.
void encodeDraw(EncodeContext& ctx,
                 WMT::CommandBuffer& commandBuffer,
                 WMT::RenderCommandEncoder& encoder,
                 const core::DrawDesc& draw,
                 std::uint64_t seqId,
                 bool skipBaseStateBind = false,
                 const PreUploadedDrawData* preUploaded = nullptr,
                 const core::DrawParam* paramOverride = nullptr);

// Encode a single chunk's commands into a fresh WMT::CommandBuffer.
// Returns a QueueSubmissionRecord that the finish loop commits; nullopt
// on allocation failure. Previously MetalBackendDevice::encodeChunk.
std::optional<core::metalqueue::QueueSubmissionRecord> encodeChunk(
    EncodeContext& ctx,
    std::size_t slotIndex,
    const core::ChunkSlot& slot);

}  // namespace encoders
}  // namespace dxmt9
