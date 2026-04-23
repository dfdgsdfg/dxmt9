#pragma once

// Free-function draw encoder helpers — lifted from MetalBackendDevice
// (Step 3d). encodeDraw itself still lives on the backend; this module
// currently hosts makeSampler + beginRenderPass, which were the cleanest
// extraction candidates. EncodeContext is pre-defined so the eventual
// encodeDraw extraction just plugs in here.

#include "../winemetal/Metal.hpp"
#include "dxmt9/core.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace dxmt9 {

class CommandQueue;

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
void encodeDraw(EncodeContext& ctx,
                 WMT::CommandBuffer& commandBuffer,
                 WMT::RenderCommandEncoder& encoder,
                 const core::DrawDesc& draw,
                 std::uint64_t seqId);

}  // namespace encoders
}  // namespace dxmt9
