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
#include <limits>
#include <optional>
#include <span>
#include <string>

namespace dxmt9 {

class Device;

namespace perf {
enum class RenderPassDepthStoreProof : std::uint8_t;
enum class RenderPassColorStoreProof : std::uint8_t;
}

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
  void (*setBlendColorAndStencilRef)(void* userdata,
                                     float red,
                                     float green,
                                     float blue,
                                     float alpha,
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
  // Encode-chunk-local completion watermark for transient arena reservation.
  // A stale lower watermark is safe: it can delay reclaim, never release
  // storage before the GPU completion waterline.
  std::uint64_t transientCompletedSeqId = 0;
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
// `lookaheadSlot` + `lookaheadStartIndex` (R-BACK-15.7/15.8): when non-null,
// the encoder runs the depth/stencil DontCare-store proof described in
// specs/backend/render-pass-actions/design.md section 4.2 and the narrower
// color next-clear DontCare-store proof. Depth records the reason in
// render_pass_depth_proof_* counters. Pass nullptr to keep the legacy
// unconditional-Store behavior for callers without chunk records.
struct RenderPassActionSummary {
  std::uint64_t colorAttachmentCount = 0;
  std::uint64_t color0Included = 0;
  std::uint64_t color0LoadAction = 0;
  std::uint64_t color0StoreAction = 0;
  std::uint64_t color0Clear = 0;
  std::uint64_t colorLoadBytes = 0;
  std::uint64_t colorStoreBytes = 0;
  std::uint64_t depthIncluded = 0;
  std::uint64_t depthLoadAction = 0;
  std::uint64_t depthStoreAction = 0;
  std::uint64_t depthClear = 0;
  std::uint64_t depthLoadBytes = 0;
  std::uint64_t depthStoreBytes = 0;
  std::uint64_t stencilIncluded = 0;
  std::uint64_t stencilLoadAction = 0;
  std::uint64_t stencilStoreAction = 0;
  std::uint64_t stencilClear = 0;
  std::uint64_t stencilLoadBytes = 0;
  std::uint64_t stencilStoreBytes = 0;
};

WMT::Reference<WMT::RenderCommandEncoder> beginRenderPass(
    EncodeContext& ctx,
    WMT::CommandBuffer& commandBuffer,
    core::FlatDrawStateView drawState,
    const std::optional<core::ClearDesc>& clear,
    const core::ChunkSlot* lookaheadSlot = nullptr,
    std::size_t lookaheadStartIndex = 0,
    std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments = {},
    WMT::Buffer visibilityBuffer = {},
    RenderPassActionSummary* actionSummary = nullptr);

// Depth/stencil DontCare-store look-ahead (R-BACK-15.7,
// specs/backend/render-pass-actions/design.md section 4.2). Returns the
// specific proof result for the remaining records in `slot` after
// `startCommandIndex`. Allow results mean DontCare-store is safe. Block
// results explain why the encoder must Store instead.
//
// Exposed publicly so the render-pass-actions test fixture
// (tests/native/backend/render_pass_actions_spec.cpp, R-BACK-15.16) can
// drive R-BACK-15.7 / 15.9 / 15.15 cases without standing up a Metal
// device.
dxmt9::perf::RenderPassDepthStoreProof depthStoreProofForLookahead(
    const core::ChunkSlot& slot,
    std::size_t startCommandIndex,
    core::Handle depthHandle,
    std::uint32_t* firstTouchCommandDistance = nullptr);

// Compatibility bool used by existing callers/tests.
bool nextDepthOperationIsClear(const core::ChunkSlot& slot,
                               std::size_t startCommandIndex,
                               core::Handle depthHandle);

// Conservative color twin of the depth next-clear proof. Returns true only
// when the next later record that touches `colorHandle` is a color Clear of
// that handle. It intentionally does not use the broader dead-at-end proof:
// color surfaces are more commonly presented, sampled, or reused across
// chunk boundaries.
bool nextColorOperationIsClear(const core::ChunkSlot& slot,
                               std::size_t startCommandIndex,
                               core::Handle colorHandle);

dxmt9::perf::RenderPassColorStoreProof colorStoreProofForLookahead(
    const core::ChunkSlot& slot,
    std::size_t startCommandIndex,
    core::Handle colorHandle,
    std::uint32_t* firstTouchCommandDistance = nullptr);

// R-FORMAT-12 — colorless (D3DFMT_NULL) render-pass attachment decisions,
// extracted as pure value transforms so the depth-only-pass policy is
// unit-testable without a Metal device / ObjC++ encoder (the begin path
// itself lives in beginRenderPass and consumes these). Modeled on the
// existing applyColorLoadPolicy transcription in
// tests/native/backend/render_pass_actions_spec.cpp.
//
// Inputs are the three observable facts about render-target slot 0 that
// gate whether the render pass opens at all:
//   surfaceExists — pool.findSurface(rt0) returned a record.
//   hasTexture    — that record owns a backend (Metal) color texture.
//   isNullRt      — that record's format is Format::NullRt (colorless).
struct ColorlessRenderPassRt0 {
  bool surfaceExists = false;
  bool hasTexture = false;
  bool isNullRt = false;
};

// True iff beginRenderPass should proceed to build the pass for this RT0.
// A NULL render target has a surface record but deliberately no color
// texture, so it admits a depth/stencil-only pass; a genuinely missing RT0
// or a normal color RT whose texture failed to allocate aborts the pass.
// One-to-one with the guard at dxmt9_draw_encoder.mm beginRenderPass entry.
inline constexpr bool renderPassAdmitsRt0(const ColorlessRenderPassRt0& rt0) {
  return rt0.surfaceExists && (rt0.hasTexture || rt0.isNullRt);
}

// True iff a color-attachment slot contributes a color attachment to the
// pass. The per-attachment loop includes a slot only when its surface owns
// a backend texture, so a NULL render target (surface present, no texture)
// is omitted — the depth/stencil attachment becomes the effective target.
// One-to-one with the `if (!surface || !surface->texture) continue;` filter
// in the color-attachment loop.
inline constexpr bool colorAttachmentIncluded(bool surfaceExists,
                                              bool hasTexture) {
  return surfaceExists && hasTexture;
}

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

struct TextureSamplerBindShadowSlot {
  bool valid = false;
  std::uint64_t hash = 0;
  obj_handle_t handle = 0;
};

struct SamplerBindShadowSlot {
  bool valid = false;
  std::uint64_t hash = 0;
  obj_handle_t handle = 0;
  std::uint32_t textureLod = 0;
  bool supportArgumentBuffers = false;
  core::FlatStateSet<core::kMaxSamplerStates> samplerStates{};
};

struct BufferBindShadowSlot {
  bool valid = false;
  obj_handle_t handle = 0;
  std::uint64_t offset = 0;
};

struct TextureSamplerBindShadow {
  TextureSamplerBindShadowSlot renderPipeline{};
  TextureSamplerBindShadowSlot depthStencil{};
  std::array<BufferBindShadowSlot, 32> vertexBuffers{};
  std::array<TextureSamplerBindShadowSlot, core::kMaxSamplers> fragmentTextures{};
  std::array<SamplerBindShadowSlot, core::kMaxSamplers> fragmentSamplers{};
  std::array<TextureSamplerBindShadowSlot, core::kMaxVertexTextureSamplers> vertexTextures{};
  std::array<SamplerBindShadowSlot, core::kMaxVertexTextureSamplers> vertexSamplers{};

  void reset() noexcept {
    *this = TextureSamplerBindShadow{};
  }
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
                 bool argbufHybridMode = false,
                 // R-BACK-12.22..12.26 (resource-array sub-mode) — when
                 // true (only ever alongside argbufHybridMode), the
                 // per-stage fragment textures/samplers are written into
                 // the slot-30 resource-array argbuf via
                 // argbuf_hybrid::populateResourceBindings (which also
                 // issues the useResource residency the GPU needs for
                 // argbuf-pointed textures) and the direct
                 // [[texture(N)]] / [[sampler(N)]] binds below are skipped.
                 // The PSO built for the draw carries the matching
                 // ShaderVariantKey::argbufResourceArray bit. Default off
                 // keeps the constants-only Stage 2 path byte-identical.
                 bool argbufResourceArray = false,
                 // Stage 2b/direct-cbuf ABI lane. Only meaningful alongside
                 // argbufHybridMode and never with argbufResourceArray. When
                 // true, shaders keep direct cbuf slots 0/3 and encodeDraw
                 // skips slot-30 argbuf table open/reopen for cbuf pointer
                 // turnover.
                 bool argbufDirectCbufMode = false,
                 // R-BACK-12.22..12.26 — when true, reserve a FRESH argbuf
                 // descriptor table for this draw and rebind slot 30 (each
                 // draw self-contained). When false, reuse the encoder's
                 // current argbuf table (correct only when this draw's
                 // constants/resources are unchanged from the previous draw
                 // on the same encoder). The caller decides via the uniform
                 // payload hash; default true preserves the per-draw-reopen
                 // safe floor for direct callers/tests.
                 bool reopenArgbufHybrid = true,
                 // Encoder-local direct resource binding shadow. When present,
                 // texture/sampler direct lane binds are hash-checked against
                 // the currently open Metal render encoder state and unchanged
                 // setTexture/setSampler calls are skipped. Callers must reset
                 // it whenever a render encoder boundary is crossed.
                 TextureSamplerBindShadow* textureSamplerShadow = nullptr,
                 // Chunk-local command index used only for stale handle
                 // provenance logs. Direct encodeDraw callers can leave it
                 // invalid.
                 std::uint32_t commandIndex = std::numeric_limits<std::uint32_t>::max(),
                 const core::DrawBindingSnapshot* bindingSnapshot = nullptr);

struct EncodeChunkOptions {
  // Optional open command buffer supplied by an encoded-pending-tail carrier.
  // When present, encodeChunk appends work into this command buffer and must
  // not internally commit/split it before the Present tail is appended.
  WMT::Reference<WMT::CommandBuffer> commandBuffer{};
  // Open-CB pre-encode carriers must not commit sub-CBs before the Present tail
  // has been appended; keep the default false for the current byte-identical
  // path.
  bool disableMidChunkCommits = false;
  // Same carrier class must also avoid the optional pre-acquire split that can
  // commit the pre-Present head immediately before encoding Present.
  bool disablePresentAcquireSplit = false;

  bool hasInjectedCommandBuffer() const noexcept {
    return static_cast<bool>(commandBuffer);
  }
};

// Encode a single chunk's commands into a fresh or supplied WMT::CommandBuffer.
// Returns a QueueSubmissionRecord that the finish loop commits; nullopt
// on allocation failure. Previously MetalBackendDevice::encodeChunk.
std::optional<core::metalqueue::QueueSubmissionRecord> encodeChunk(
    EncodeContext& ctx,
    std::size_t slotIndex,
    const core::ChunkSlot& slot,
    EncodeChunkOptions options = {});

}  // namespace encoders
}  // namespace dxmt9
