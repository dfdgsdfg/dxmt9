#pragma once

// Free-function draw encoder — lifted from MetalBackendDevice (Step 3d).
// Hosts the EncodeContext bundle, makeSampler, beginRenderPass,
// encodeDraw, and encodeChunk (3c-remainder).

#include "../winemetal/Metal.hpp"
#include "dxmt9_backend_types.hpp"
#include "dxmt9_queue.hpp"
#include "dxmt9/core.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace dxmt9 {

class CommandQueue;
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
// linear" variant) and a full SamplerSnapshot variant.
WMT::Reference<WMT::SamplerState> makeSampler(WMT::Reference<WMT::Device> device, bool linear);
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
void encodeDraw(EncodeContext& ctx,
                 WMT::CommandBuffer& commandBuffer,
                 WMT::RenderCommandEncoder& encoder,
                 const core::DrawDesc& draw,
                 std::uint64_t seqId,
                 bool skipBaseStateBind = false);

// Encode a single chunk's commands into a fresh WMT::CommandBuffer.
// Returns a QueueSubmissionRecord that the finish loop commits; nullopt
// on allocation failure. Previously MetalBackendDevice::encodeChunk.
std::optional<core::metalqueue::QueueSubmissionRecord> encodeChunk(
    EncodeContext& ctx,
    std::size_t slotIndex,
    const core::ChunkSlot& slot);

}  // namespace encoders
}  // namespace dxmt9
