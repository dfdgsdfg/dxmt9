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

// Optional recorder seam for tests that need to observe the final draw-issue
// commands after encodeDraw has selected UP vs bound resources and built the
// per-draw volatile constants. Production leaves this null.
struct EncodeDrawRecorder {
  void* userdata = nullptr;
  // Suppresses the recorded draw-issue calls below so tests can pass fake
  // Metal handles. Callers must still skip or satisfy base-state binding paths.
  bool suppressMetalCalls = false;
  // Test-only path for skipBaseStateBind=false without a live Metal device.
  // When set, encodeDraw uses the fake objects below instead of consulting the
  // pipeline/DSSO/sampler caches.
  bool suppressBaseStateLookup = false;
  WMT::RenderPipelineState renderPipelineState{};
  WMT::DepthStencilState depthStencilState{};
  WMT::SamplerState fragmentSamplerState{};

  void (*setRenderPipelineState)(void* userdata,
                                 WMT::RenderPipelineState pipeline) = nullptr;
  void (*setDepthStencilState)(void* userdata,
                               WMT::DepthStencilState depthStencil,
                               std::uint8_t stencilRef) = nullptr;
  void (*setViewport)(void* userdata, WMTViewport viewport) = nullptr;
  void (*setScissorRect)(void* userdata, WMTScissorRect rect) = nullptr;
  void (*setRasterizerState)(void* userdata,
                             WMTTriangleFillMode fillMode,
                             WMTCullMode cullMode,
                             WMTDepthClipMode depthClipMode,
                             WMTWinding winding,
                             float depthBias,
                             float slopeScale,
                             float depthBiasClamp) = nullptr;
  void (*setFragmentTexture)(void* userdata,
                             WMT::Texture texture,
                             std::uint8_t index) = nullptr;
  void (*setFragmentSamplerState)(void* userdata,
                                  WMT::SamplerState sampler,
                                  std::uint8_t index) = nullptr;
  void (*setVertexTexture)(void* userdata,
                           WMT::Texture texture,
                           std::uint8_t index) = nullptr;
  void (*setVertexSamplerState)(void* userdata,
                                WMT::SamplerState sampler,
                                std::uint8_t index) = nullptr;
  void (*setVertexBuffer)(void* userdata,
                          WMT::Buffer buffer,
                          std::uint64_t offset,
                          std::uint8_t index) = nullptr;
  void (*setVertexBytes)(void* userdata,
                         const void* bytes,
                         std::uint64_t length,
                         std::uint8_t index) = nullptr;
  void (*drawPrimitives)(void* userdata,
                         WMTPrimitiveType primitiveType,
                         std::uint64_t vertexStart,
                         std::uint64_t vertexCount) = nullptr;
  void (*drawIndexedPrimitives)(void* userdata,
                                WMTPrimitiveType primitiveType,
                                WMTIndexType indexType,
                                std::uint64_t indexCount,
                                WMT::Buffer indexBuffer,
                                std::uint64_t indexBufferOffset,
                                std::uint32_t instanceCount,
                                std::int32_t baseVertex,
                                std::uint32_t baseInstance) = nullptr;
};

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
  EncodeDrawRecorder* drawRecorder = nullptr;
};

// Sampler factory helpers used by the draw encoder. Previously
// MetalBackendDevice::makeSampler. Split into nearest/linear (the "bool
// linear" variant), a pure SamplerSnapshot -> WMTSamplerInfo mapper, and the
// full SamplerSnapshot variant that creates the Metal object.
WMT::Reference<WMT::SamplerState> makeSampler(WMT::Reference<WMT::Device> device, bool linear);
WMTSamplerInfo makeSamplerInfo(const core::SamplerSnapshot& snapshot, float lodMinClamp = 0.0f);
WMTSamplerInfo makeSamplerInfo(const core::FlatStateSet<core::kMaxSamplerStates>& states,
                               float lodMinClamp = 0.0f);
WMT::Reference<WMT::SamplerState> makeSampler(WMT::Reference<WMT::Device> device,
                                                const core::SamplerSnapshot& snapshot);
WMT::Reference<WMT::SamplerState> makeSampler(WMT::Reference<WMT::Device> device,
                                                const core::SamplerSnapshot& snapshot,
                                                float lodMinClamp);
WMT::Reference<WMT::SamplerState> makeSampler(WMT::Reference<WMT::Device> device,
                                                const core::FlatStateSet<core::kMaxSamplerStates>& states);
WMT::Reference<WMT::SamplerState> makeSampler(
    WMT::Reference<WMT::Device> device,
    const core::FlatStateSet<core::kMaxSamplerStates>& states,
    float lodMinClamp);

// Render-pass setup. Builds a WMTRenderPassInfo from the current draw
// target + optional clear, then begins the encoder on the given command
// buffer. Reads (and clears, on entry) ctx.queue.backBufferDiscardAfterPresent_
// so the next draw to the same RT can choose DontCare over Load.
//
// `lookaheadSlot` + `lookaheadStartIndex` (R-BACK-15.7 simple form): when
// non-null, the encoder runs the depth/stencil DontCare-store look-ahead
// described in specs/backend/render-pass-actions/design.md section 4.2.
// Pass nullptr to keep the legacy unconditional-Store behavior (e.g. for
// callers that don't have access to the imported chunk records).
WMT::Reference<WMT::RenderCommandEncoder> beginRenderPass(
    EncodeContext& ctx,
    WMT::CommandBuffer& commandBuffer,
    core::FlatDrawStateView drawState,
    const std::optional<core::ClearDesc>& clear,
    const core::ChunkSlot* lookaheadSlot = nullptr,
    std::size_t lookaheadStartIndex = 0);

// Depth/stencil DontCare-store look-ahead (R-BACK-15.7 simple form,
// specs/backend/render-pass-actions/design.md section 4.2). Returns true
// when the very next record in `slot` after `startCommandIndex` that
// touches `depthHandle` is a Clear of that handle. Any prior live read
// or surface op on the handle (Readback / SurfaceCopy / StretchRect /
// ColorFill source-or-dest, or a DrawRun that re-binds the handle as
// depth target) — or hitting a Present / end of slot before such a
// Clear — flips the proof to defensive Store (returns false).
//
// Exposed publicly so the render-pass-actions test fixture
// (tests/native/backend/render_pass_actions_spec.cpp, R-BACK-15.16) can
// drive R-BACK-15.7 / 15.9 / 15.15 cases without standing up a Metal
// device. Always returns false on a null/zero `depthHandle`.
bool nextDepthOperationIsClear(const core::ChunkSlot& slot,
                               std::size_t startCommandIndex,
                               core::Handle depthHandle);

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
//
// `tileFfpMode` (R-BACK-13.1..13.6): when true, the encoder is on the
// tile-shader FFP path (selectTileFfpForPass returned Tile) and
// encodeDraw must bind the tile pipeline via setTileRenderPipelineState
// + dispatchThreadsPerTile rather than the portable
// setRenderPipelineState. The flag is sticky for the encoder's lifetime
// — mid-pass demotions force a render-pass split (R-BACK-13.6) so a
// fresh encoder opens on the portable path.
bool encodeDraw(EncodeContext& ctx,
                 WMT::CommandBuffer& commandBuffer,
                 WMT::RenderCommandEncoder& encoder,
                 core::FlatDrawStateView drawState,
                 std::uint64_t seqId,
                 bool skipBaseStateBind = false,
                 const PreUploadedDrawData* preUploaded = nullptr,
                 const core::DrawParam* paramOverride = nullptr,
                 std::span<const std::uint8_t> paramPayloadArena = {},
                 uniform::DirtyState* dirty = nullptr,
                 bool tileFfpMode = false,
                 // R-BACK-12.22 / 12.24 — Stage 2 argbuf-hybrid mode for
                 // this render encoder. When true, the per-frequency UBO
                 // dirty consume path rewrites the matching argbuf
                 // sub-regions (via argbuf_hybrid::updateDirtyArgbufRegions)
                 // while the Stage 2 PSO reads slot 30. Direct slot 0 / 3
                 // Stage 1 shadow binds are skipped in this mode.
                 // DrawVolatile (slot 5) and the vertex stream (slot 1)
                 // are unchanged in either mode (design.md §11.2).
                 bool argbufHybridMode = false);

// Encode a single chunk's commands into a fresh WMT::CommandBuffer.
// Returns a QueueSubmissionRecord that the finish loop commits; nullopt
// on allocation failure. Previously MetalBackendDevice::encodeChunk.
std::optional<core::metalqueue::QueueSubmissionRecord> encodeChunk(
    EncodeContext& ctx,
    std::size_t slotIndex,
    const core::ChunkSlot& slot);

}  // namespace encoders
}  // namespace dxmt9
