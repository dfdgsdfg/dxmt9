#pragma once

// Pipeline state cache — keys draw/fill/stretch render pipelines by their
// variant signature, plus depth/stencil states by their D3D9 key. Lifted
// out of backend_metal.mm's anonymous namespace so the cache has a named
// home matching dxmt's architecture (dxmt has equivalents scattered across
// dxmt_pipeline.cpp / dxmt_sampler.cpp / dxmt_depth_stencil_state.cpp).
//
// The pipeline-builder CLOSURES still live on MetalBackendDevice (they
// capture wrappedDevice_ / shaderArchive_); this class is the storage +
// the depth/stencil state builder.

#include "dxmt9/core.hpp"
#include "dxmt9_draw_shader.hpp"
#include "../winemetal/Metal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace dxmt9 { namespace resources { struct Pool; } }

namespace dxmt9::pipeline {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// Shader-source identity that is orthogonal to D3D9 input state. Bump the
// emitter/layout versions when MSL text or host-visible source layout changes;
// debugEnvKey covers source-affecting env toggles for the current process.
// v3: H228 single-variant alpha test — the fragment alpha-test tail reads the
// per-draw FsVolatile immediate at fragment buffer 5 (new prelude struct +
// entry-point param) instead of FfpPsConsts, and the alpha-test variant-key
// bit is gone.
// Debug-env schema v3 retires four rejected translated-VS/VSOut diagnostic
// axes; the remaining key covers only live source-affecting env surfaces.
inline constexpr u32 kShaderEmitterVersion = 3u;
inline constexpr u32 kShaderSourceLayoutVersion = 3u;
inline constexpr u32 kShaderDebugEnvSchemaVersion = 3u;

struct BlendAttachmentKey {
  bool blendingEnabled = false;
  u32 rgbBlendOperation = static_cast<u32>(core::BlendOp::Add);
  u32 alphaBlendOperation = static_cast<u32>(core::BlendOp::Add);
  u32 sourceRGBBlendFactor = static_cast<u32>(core::BlendFactor::One);
  u32 destinationRGBBlendFactor = static_cast<u32>(core::BlendFactor::Zero);
  u32 sourceAlphaBlendFactor = static_cast<u32>(core::BlendFactor::One);
  u32 destinationAlphaBlendFactor = static_cast<u32>(core::BlendFactor::Zero);
  u32 colorWriteMask = 0xfu;
  u32 pixelFormat = 0;

  friend bool operator==(const BlendAttachmentKey&, const BlendAttachmentKey&) = default;
};

struct BlendAttachmentKeyHash {
  std::size_t operator()(const BlendAttachmentKey& key) const noexcept;
};

struct StencilFaceKey {
  bool enabled = false;
  u32 compareFunction = static_cast<u32>(core::CompareFunc::Always);
  u32 failureOperation = static_cast<u32>(core::StencilOp::Keep);
  u32 depthFailureOperation = static_cast<u32>(core::StencilOp::Keep);
  u32 passOperation = static_cast<u32>(core::StencilOp::Keep);
  u32 readMask = 0xffu;
  u32 writeMask = 0xffu;

  friend bool operator==(const StencilFaceKey&, const StencilFaceKey&) = default;
};

struct StencilFaceKeyHash {
  std::size_t operator()(const StencilFaceKey& key) const noexcept;
};

// PSO cache key. The FFP variant-key contribution is indirect: when the
// PE state contains a fixed-function shader stage the corresponding
// `core::FfpVertexKey` / `core::FfpPixelKey` is wrapped in a
// `core::ShaderRef`, the shader-source generator (`dxmt9_ffp_shaders.cpp`)
// emits MSL text that is content-hashed into `vertexSourceHash` /
// `fragmentSourceHash` here. The determinism contract on the FFP keys
// (same `DeviceState` -> same key -> same source text -> same source
// hash) is therefore load-bearing for the PSO cache hit rate. See the
// header comments on `core::FfpVertexKey` / `core::FfpPixelKey` in
// `include/dxmt9/core_constants.hpp` for the canonical builders and the
// dedicated regression tests.
struct ShaderVariantKey {
  u64 hash = 0;
  u64 vertexSourceHash = 0;
  u64 fragmentSourceHash = 0;
  u64 tileSourceHash = 0;
  u32 emitterVersion = kShaderEmitterVersion;
  u32 sourceLayoutVersion = kShaderSourceLayoutVersion;
  u32 debugEnvSchemaVersion = kShaderDebugEnvSchemaVersion;
  u64 debugEnvKey = 0;
  bool textured = false;
  u32 textureMask = 0;
  bool linear = false;
  bool clipPlanes = false;
  // H224 — fragment fog-tail PSO-variant gate. True iff the resolved draw
  // state can produce a non-zero ffpPs.fogMode at upload time
  // (state::fragmentFogCouldApply: D3DRS_FOGENABLE plus a non-None
  // table/vertex fog mode). When clear, the generated fragment source omits
  // the dxmt9_apply_fog tail and its per-fragment FfpPsConsts loads; when
  // set, the tail keeps the historical runtime ffpPs.fogMode switch. Fog
  // params stay runtime uniform loads, bounding the fog fan-out at 2x per
  // shader. Alpha test deliberately has NO key bit (H228): the alpha-test
  // tail is a single always-emitted variant evaluated from the per-draw
  // FsVolatile immediate, so alpha-test render-state toggles never split the
  // PSO and draw runs / submission batches can span them.
  bool fogActive = false;
  bool alphaToCoverage = false;
  // R-BACK-13.3 — tile-FFP-mode bit. Two draws with the same FFPKeyPS
  // but different tile-mode selection compile separate pipeline states
  // (one fragment PSO, one tile PSO). The portable-path key always sets
  // this to false; the selector flips it on at encoder open when the
  // pass is chosen to run on the tile path.
  bool tileFfpMode = false;
  // R-BACK-13.1 — tile-FFP base-colour pass sub-key. A tile-FFP draw needs
  // BOTH a render PSO (the base-colour FFP fragment, fog/alpha-test/A2C
  // stripped) AND the tile PSO (the imageblock post-pass). The two PSOs
  // share the same FFPKeyPS but must hash to distinct cache entries: the
  // tile PSO sets tileFfpMode=true (built via newRenderPipelineStateWithTileDescriptor),
  // the base-colour render PSO sets tileFfpBaseColor=true (built as an
  // ordinary fragment PSO whose source has stripFogAlphaTestForTileBase set
  // and whose descriptor forces alpha_to_coverage_enabled off). Never both
  // true on the same key. Portable-path keys leave both false.
  bool tileFfpBaseColor = false;
  // R-BACK-12.22 / 12.23 — Stage 2 argbuf-hybrid mode bit. Two draws with
  // the same shaders but different argbuf-mode selection compile separate
  // PSOs (one Stage 1 prelude reading slot 0/3, one Stage 2 prelude
  // reading the slot-30 argbuf). The selector flips this on at encoder
  // open when `argbufHybridEnabled()` holds and the pass is eligible.
  bool argbufHybridMode = false;
  // Stage 2b direct-cbuf sub-key. This is a separate PSO identity from
  // Stage 1 and Stage 2 even when the generated source currently keeps the
  // same direct slot 0/3 cbuf signature as Stage 1. The split gives the
  // future host binding path a non-aliasing cache lane.
  bool argbufDirectCbufMode = false;
  // R-BACK-12.22..12.26 (resource-array sub-mode) — opt-in sub-bit of the
  // Stage 2 hybrid. When set (only ever alongside argbufHybridMode) the
  // emitters carry the per-stage texture/sampler resources through the
  // slot-30 argbuf texture/sampler arrays instead of direct
  // [[texture(N)]] / [[sampler(N)]] binds, and the host populator writes
  // them into the argbuf + issues `useResource` residency. Default off;
  // gated on the `DXMT9_ARGBUF_RESOURCE_ARRAY` env flag (read once at
  // makeShaderVariantKey time). A Stage 2 constants-only PSO and a Stage 2
  // resource-array PSO therefore hash to distinct cache entries so the
  // default constants-only lane stays byte-identical.
  bool argbufResourceArray = false;
  // D3DSAMP_MIPMAPLODBIAS (gap_d3d9 B.3) PSO-variant gate. Set by
  // makeShaderVariantKey from state::anySamplerLodBiasNonzero — true iff some
  // active sampler stage has a non-zero mip LOD bias. When set, the fragment
  // emitters declare `constant SamplerLodBias& samplerLodBias [[buffer(4)]]`
  // and thread bias() through every implicit-gradient sample, and the encoder
  // binds the slot-4 uniform. When clear, none of that is emitted or bound —
  // the MSL is byte-identical to the pre-MIPMAPLODBIAS plain-sample form, and
  // the common no-bias draw skips the per-draw slot-4 upload + bind. bias-on
  // and bias-off draws therefore hash to distinct PSOs (mirrors tileFfpMode).
  bool samplerLodBias = false;
  // ATI FETCH4 compatibility mask. Bits identify active fragment samplers
  // whose raw MIPMAPLODBIAS state is GET4, whose texture is a supported
  // single-channel format, and whose MAGFILTER is POINT. GET1 and ordinary
  // numeric LOD bias never set this mask.
  u32 fetch4SamplerMask = 0;
  // Diagnostic depth-only backend-shape probe. When set, the render PSO uses
  // the ordinary vertex stage but omits the fragment function. This bit is
  // separate from source hashes so fragmentless and normal PSOs cannot alias.
  bool fragmentlessDepthOnly = false;
  // Pair-local VSOut layout selected from fragment liveness when
  // DXMT9_TRIM_UNUSED_VARYINGS is enabled. Participates in the probe key so
  // two shader pairs with different stage-in structs never share a stale PSO.
  u32 vsOutLayoutKey = 0;
  // Fragment sampler mask for the X8 alpha-fill shader variant. This is
  // separate from textureTypes because it is format/contract state, not Metal
  // dimensionality state.
  u32 x8AlphaOneTextureMask = 0;
  u32 sampleCount = 1;
  std::array<u32, core::kMaxTextureStages> textureTypes{};
  std::array<u32, core::kMaxRenderTargets> colorFormats{};
  std::array<BlendAttachmentKey, core::kMaxRenderTargets> blend{};
  u32 depthFormat = 0;
  u32 stencilFormat = 0;

  friend bool operator==(const ShaderVariantKey&, const ShaderVariantKey&) = default;
};

struct ShaderVariantKeyHash {
  std::size_t operator()(const ShaderVariantKey& key) const noexcept;
};

// Canonical probe key used as a fast index into a previously published
// source-backed PSO key. The returned key deliberately drops actual MSL source
// hashes; it is never authoritative for correctness and only avoids source
// generation when it maps to an existing final key in Cache::draw.
ShaderVariantKey makeShaderVariantProbeKey(ShaderVariantKey key) noexcept;

// Stable value transform for source-affecting debug flags. Kept public so
// native tests can verify the key shape without mutating process env.
u64 makeShaderSourceDebugEnvKey(bool trimUnusedVaryings,
                                bool forceFullscreenVertex,
                                bool flipTranslatedVertexY,
                                bool forceFragmentShaderColor,
                                bool disableAlphaTest,
                                bool disableFog,
                                bool forceTextureWhite,
                                std::string_view fragmentMode,
                                bool forcePixelVFlip,
                                bool debugFfpUv,
                                bool debugFfpTexture,
                                bool debugFfpAlpha,
                                bool probeHalfVSOut,
                                bool probeFragmentlessKeepVSOut) noexcept;

// Reads the current process env knobs that can change emitted draw MSL.
u64 currentShaderSourceDebugEnvKey() noexcept;
u64 currentShaderSourceDebugEnvKey(
    std::optional<bool> forceTextureWhiteOverride) noexcept;

// R-BACK-3.11 — true when the process's current shader debug-env key
// equals the default (no DXMT_DISABLE_*/DXMT_FORCE_*/DXMT9_PROBE_*
// classifier env active). Exposed so archive persistence
// (dxmt9_command_queue.cpp, at CommandQueue construction) can gate the
// R-BACK-3.11 pollution guard without duplicating the 14-parameter
// makeShaderSourceDebugEnvKey() default-argument shape.
bool shaderSourceDebugEnvIsDefault() noexcept;

namespace detail {

struct DrawShaderSources {
  std::string vertex;
  std::string fragment;
  u64 vertexHash = 0;
  u64 fragmentHash = 0;
};

// Generate VS+FS draw sources while containing translator failures. Returns
// std::nullopt when a bytecode translator or source emitter throws so async
// PSO builders can resolve to "no pipeline" instead of rethrowing through
// shared_future::get().
std::optional<DrawShaderSources>
makeContainedDrawShaderSources(const drawshader::ShaderSourceContext& shaderSource,
                               u64 variantHash);

// Render-state-only blend attachment key mapping. Pixel formats are resolved
// by Cache::getOrBuildDrawPipelineForState after surface lookup.
std::array<BlendAttachmentKey, core::kMaxRenderTargets>
makeBlendAttachmentKeys(core::FlatDrawStateView state,
                        bool forceVisibleDraw = false,
                        bool disableAlphaBlend = false);

// Fill PSOs embed the fill colour directly into the generated fragment source,
// so every RGBA channel must participate in the immutable pipeline key.
ShaderVariantKey makeFillPipelineKey(const core::ColorRGBA& color,
                                     u32 pixelFormat) noexcept;

}  // namespace detail

struct DepthStencilKey {
  bool depthEnable = false;
  bool depthWrite = false;
  u32 depthFunc = static_cast<u32>(core::CompareFunc::Always);
  StencilFaceKey front{};
  StencilFaceKey back{};

  friend bool operator==(const DepthStencilKey&, const DepthStencilKey&) = default;
};

struct DepthStencilKeyHash {
  std::size_t operator()(const DepthStencilKey& key) const noexcept;
};

struct SamplerKey {
  core::FlatStateSet<core::kMaxSamplerStates> states{};
  u32 lodMinClampBits = 0;
  bool supportArgumentBuffers = false;

  friend bool operator==(const SamplerKey&, const SamplerKey&) = default;
};

struct SamplerKeyHash {
  std::size_t operator()(const SamplerKey& key) const noexcept;
};

struct Entry {
  std::shared_future<WMT::Reference<WMT::RenderPipelineState>> future;
};

struct SourceLibraryEntry {
  std::shared_future<WMT::Reference<WMT::Library>> future;
};

struct PsoSlot {
  u32 generation = 0;
  ShaderVariantKey key{};
  Entry entry{};
  bool occupied = false;
};

struct DepthStencilSlot {
  u32 generation = 0;
  DepthStencilKey key{};
  WMT::Reference<WMT::DepthStencilState> state{};
  bool occupied = false;
};

struct DrawPipelineLookup {
  std::shared_future<WMT::Reference<WMT::RenderPipelineState>> future;
  core::PsoHandle handle{};
};

struct ResolvedDrawPipelineState {
  ShaderVariantKey key{};
  drawshader::ShaderSourceContext shaderSource{};
};

struct DepthStencilLookup {
  WMT::Reference<WMT::DepthStencilState> state{};
  core::DepthStencilHandle handle{};
};

struct HandleLookupContext {
  u64 chunkSeqId = 0;
  u32 commandIndex = std::numeric_limits<u32>::max();
  const char* role = nullptr;
};

using PipelineMap = std::unordered_map<ShaderVariantKey, Entry, ShaderVariantKeyHash>;
using DrawProbeMap = std::unordered_map<ShaderVariantKey, ShaderVariantKey, ShaderVariantKeyHash>;
using DrawHandleMap = std::unordered_map<ShaderVariantKey, core::PsoHandle, ShaderVariantKeyHash>;
using SourceLibraryMap = std::unordered_map<u64, SourceLibraryEntry>;
using DepthMap =
    std::unordered_map<DepthStencilKey, WMT::Reference<WMT::DepthStencilState>, DepthStencilKeyHash>;
using DepthHandleMap =
    std::unordered_map<DepthStencilKey, core::DepthStencilHandle, DepthStencilKeyHash>;
using SamplerMap =
    std::unordered_map<SamplerKey, WMT::Reference<WMT::SamplerState>, SamplerKeyHash>;

// Container for the draw-side pipeline caches + the depth/stencil state
// cache. Members are public (same shape as the earlier in-file struct) so
// existing callers in backend_metal.mm can reach .draw / .fill / .stretch /
// .depth / .mutex unchanged.
class Cache {
 public:
  Cache() = default;
  Cache(const Cache&) = delete;
  Cache& operator=(const Cache&) = delete;

  // Look up or construct the depth/stencil state for a given D3D9 key.
  // Thread-safe; builds the WMT state object under `mutex`.
  WMT::Reference<WMT::DepthStencilState> depthStencilStateFor(WMT::Device& device,
                                                                const DepthStencilKey& key);
  DepthStencilLookup depthStencilStateHandleFor(WMT::Device& device,
                                                const DepthStencilKey& key);
  WMT::Reference<WMT::DepthStencilState>
  depthStencilStateForHandle(core::DepthStencilHandle handle,
                             HandleLookupContext context = {});

  // Immutable Metal sampler states are cached by the full D3D9 sampler state
  // set plus LOD clamp and the argument-buffer support bit. Returning a
  // Reference lets callers retain it for argbuf-backed draws without creating
  // a fresh MTL sampler per bind.
  WMT::Reference<WMT::SamplerState>
  samplerStateFor(WMT::Reference<WMT::Device> device,
                  const core::FlatStateSet<core::kMaxSamplerStates>& states,
                  float lodMinClamp,
                  bool supportArgumentBuffers);

  // Look up or build a solid-color fill pipeline keyed by (color, pixelFormat).
  // archive + archivePath are borrowed pointers for cache persistence; pass
  // nullptr to skip archive serialization.
  std::shared_future<WMT::Reference<WMT::RenderPipelineState>>
  getOrBuildFillPipeline(WMT::Reference<WMT::Device> device,
                          const core::ColorRGBA& color,
                          u32 pixelFormat,
                          WMT::Reference<WMT::BinaryArchive>* archive,
                          const std::string* archivePath);

  // Look up or build a textured-blit (stretch-rect) pipeline keyed by
  // (linear, sampleCount, pixelFormat).
  std::shared_future<WMT::Reference<WMT::RenderPipelineState>>
  getOrBuildStretchPipeline(WMT::Reference<WMT::Device> device,
                             const core::StretchRectDesc& stretch,
                             u32 pixelFormat,
                             WMT::Reference<WMT::BinaryArchive>* archive,
                             const std::string* archivePath);

  // Look up or build a D3D9 draw pipeline keyed by the fully-resolved
  // ShaderVariantKey (caller computes color-formats, blend, depth/stencil
  // formats, etc.). The closure invokes dxmt9::drawshader::makeDrawShaderSource
  // for VS+FS source generation.
  std::shared_future<WMT::Reference<WMT::RenderPipelineState>>
  getOrBuildDrawPipeline(WMT::Reference<WMT::Device> device,
                          const ShaderVariantKey& key,
                          drawshader::ShaderSourceContext shaderSource,
                          WMT::Reference<WMT::BinaryArchive>* archive,
                          const std::string* archivePath);

  DrawPipelineLookup
  getOrBuildDrawPipelineHandle(WMT::Reference<WMT::Device> device,
                               const ShaderVariantKey& key,
                               drawshader::ShaderSourceContext shaderSource,
                               WMT::Reference<WMT::BinaryArchive>* archive,
                               const std::string* archivePath);

  ResolvedDrawPipelineState
  resolveDrawPipelineState(const core::BackendLimits& limits,
                           resources::Pool& pool,
                           core::FlatDrawStateView state,
                           bool tileFfpMode = false,
                           bool argbufHybridMode = false,
                           bool argbufResourceArray = false,
                           bool argbufDirectCbufMode = false,
                           bool disableAlphaBlend = false,
                           std::optional<bool> forceTextureWhiteOverride = std::nullopt,
                           bool fragmentlessDepthOnly = false);

  // High-level entry point used by the encoder: resolves color/depth
  // pixel formats from the pool's surfaces, assembles blend attachment
  // keys from the flat render-state values, composes a ShaderVariantKey,
  // and delegates to getOrBuildDrawPipeline. Previously lived as
  // pipelineForDraw on MetalBackendDevice (Step 3d).
  //
  // R-BACK-13.3 — `tileFfpMode` flips the bit on the variant key so the
  // tile-stage pipeline lands in a separate cache entry. The encoder is
  // responsible for the gating decision (R-BACK-13.1, see
  // selectTileFfpForPass); this entry point only routes the boolean
  // through the key.
  std::shared_future<WMT::Reference<WMT::RenderPipelineState>>
  getOrBuildDrawPipelineForState(WMT::Reference<WMT::Device> device,
                                  const core::BackendLimits& limits,
                                  resources::Pool& pool,
                                  core::FlatDrawStateView state,
                                  WMT::Reference<WMT::BinaryArchive>* archive,
                                  const std::string* archivePath,
                                  bool tileFfpMode = false,
                                  bool argbufHybridMode = false,
                                  // R-BACK-12.22..12.26 (resource-array
                                  // sub-mode) — stamp the resource-array
                                  // PSO sub-bit. Only ever true alongside
                                  // argbufHybridMode; selects the prelude
                                  // that routes texture/sampler reads
                                  // through the slot-30 argbuf arrays.
                                  bool argbufResourceArray = false,
                                  bool argbufDirectCbufMode = false,
                                  bool disableAlphaBlend = false,
                                  std::optional<bool> forceTextureWhiteOverride = std::nullopt,
                                  bool fragmentlessDepthOnly = false);

  DrawPipelineLookup
  getOrBuildDrawPipelineHandleForState(WMT::Reference<WMT::Device> device,
                                       const core::BackendLimits& limits,
                                       resources::Pool& pool,
                                       core::FlatDrawStateView state,
                                       WMT::Reference<WMT::BinaryArchive>* archive,
                                       const std::string* archivePath,
                                       bool tileFfpMode = false,
                                       bool argbufHybridMode = false,
                                       bool argbufResourceArray = false,
                                       bool argbufDirectCbufMode = false,
                                       bool disableAlphaBlend = false,
                                       std::optional<bool> forceTextureWhiteOverride = std::nullopt,
                                       bool fragmentlessDepthOnly = false);

  // R-BACK-13.1 — companion to getOrBuildDrawPipelineForState for the
  // tile-FFP two-stage encode. Returns the BASE-COLOUR render pipeline: an
  // ordinary fragment PSO that rasterizes the FFP geometry colour with fog,
  // alpha-test, and alpha-to-coverage STRIPPED (those run in the tile pass).
  // The encoder binds this with setRenderPipelineState + drawPrimitives,
  // then fetches the tile PSO via getOrBuildDrawPipelineForState(tileFfpMode)
  // and runs dispatchThreadsPerTile over the same render encoder. Keyed with
  // tileFfpBaseColor=true so it never collides with the portable fragment PSO
  // (tileFfpBaseColor=false) or the tile PSO (tileFfpMode=true).
  std::shared_future<WMT::Reference<WMT::RenderPipelineState>>
  getOrBuildTileFfpBaseColorPipelineForState(WMT::Reference<WMT::Device> device,
                                             const core::BackendLimits& limits,
                                             resources::Pool& pool,
                                             core::FlatDrawStateView state,
                                             WMT::Reference<WMT::BinaryArchive>* archive,
                                             const std::string* archivePath,
                                             std::optional<bool> forceTextureWhiteOverride = std::nullopt);

  DrawPipelineLookup
  getOrBuildTileFfpBaseColorPipelineHandleForState(WMT::Reference<WMT::Device> device,
                                                   const core::BackendLimits& limits,
                                                   resources::Pool& pool,
                                                   core::FlatDrawStateView state,
                                                   WMT::Reference<WMT::BinaryArchive>* archive,
                                                   const std::string* archivePath,
                                                   std::optional<bool> forceTextureWhiteOverride = std::nullopt);

  std::shared_future<WMT::Reference<WMT::RenderPipelineState>>
  drawPipelineForHandle(core::PsoHandle handle,
                        HandleLookupContext context = {});

  // Lock-free immutable handle metadata lookup for pre-effect binding-ABI
  // validation. Unlike drawPipelineForHandle this does not resolve or count a
  // cache hit; callers use it only to prove that a child binding path matches
  // the already-prefetched PSO.
  std::optional<ShaderVariantKey>
  drawPipelineKeyForHandle(core::PsoHandle handle) const noexcept;

  std::mutex mutex{};
  PipelineMap draw{};
  DrawProbeMap drawProbe{};
  DrawHandleMap drawHandles{};
  SourceLibraryMap sourceLibraries{};
  std::vector<PsoSlot> drawSlots{};
  std::shared_ptr<const std::vector<PsoSlot>> drawSlotSnapshot =
      std::make_shared<const std::vector<PsoSlot>>();
  PipelineMap fill{};
  PipelineMap stretch{};
  DepthMap depth{};
  DepthHandleMap depthHandles{};
  std::vector<DepthStencilSlot> depthSlots{};
  std::shared_ptr<const std::vector<DepthStencilSlot>> depthSlotSnapshot =
      std::make_shared<const std::vector<DepthStencilSlot>>();
  SamplerMap sampler{};
};

// Build the textured blit (present) pipeline on a background task. Used by
// dxmt9::Presenter. opaqueAlpha=true forces the fragment shader to output
// alpha=1 for X8R8G8B8 / X8B8G8R8 swap chains. archive + archivePath are
// borrowed pointers to DeviceImpl-owned state (nullptr allowed = no archive
// persistence).
std::shared_future<WMT::Reference<WMT::RenderPipelineState>>
buildPresentPipeline(WMT::Reference<WMT::Device> device, bool opaqueAlpha,
                     WMT::Reference<WMT::BinaryArchive>* archive,
                     const std::string* archivePath);

// Build the gamma-apply present pipeline variant. Same vertex shader as
// the textured blit pipeline above; the fragment shader runs a 256-entry
// per-channel LUT lookup against the source texture sample. Bound only
// when SwapDesc::gammaRampIsIdentity is false — the existing
// buildPresentPipeline variant is the identity fast-path.
std::shared_future<WMT::Reference<WMT::RenderPipelineState>>
buildGammaApplyPresentPipeline(WMT::Reference<WMT::Device> device, bool opaqueAlpha,
                               WMT::Reference<WMT::BinaryArchive>* archive,
                               const std::string* archivePath);

// Compose a ShaderVariantKey from a flat draw state view + pre-resolved attachment
// state. The layoutHash incorporates the FFP layout hash or, if not FFP,
// a vertex-declaration hash.
ShaderVariantKey makeShaderVariantKey(core::FlatDrawStateView state,
                                       std::span<const u32> colorFormats,
                                       std::span<const BlendAttachmentKey> blendAttachments,
                                       u32 depthFormat,
                                       u32 stencilFormat,
                                       std::optional<bool> forceTextureWhiteOverride = std::nullopt);

// Resolve the bounded ATI FETCH4 selector from active sampler state and the
// pool's concrete texture formats. The pipeline cache stamps this result on
// both the PSO key and the shader-source context.
u32 fetch4SamplerMaskForDraw(const resources::Pool& pool,
                             core::FlatDrawStateView state,
                             u32 activeFragmentTextureMask) noexcept;

// R-BACK-13.* — per-pass tile-shader FFP classifier/selector. Encapsulates
// the selection flow described in spec.md §13.1. Pure value transform; no
// Metal calls. Reads the pixel-key + alpha-test reference + A2C state from
// `state` and combines with `supportsApple3`.
//
// Decision tree (in order):
//   1. !supportsApple3                                 -> Portable, GpuFamily
//   2. PS not fixed-function                           -> Portable, NotFfp
//   3. classifyTileFfpEligibility() -> Eligible        -> Tile, None
//   4. classifyTileFfpEligibility() -> reason          -> Portable, reason
//
// `reason == None` implies the tile path is eligible/chosen.
// `reason == GpuFamily` is recorded but spec.md §13.3 does NOT bump
// tileFfpFallbackByReason for it (it's not an "almost made it" case);
// the encoder bumps the dedicated GpuFamily fallback counter.
enum class TileFfpDecision : std::uint8_t { Tile, Portable };
enum class TileFfpFallbackReason : std::uint8_t {
  None,
  GpuFamily,
  NotFfp,
  Precision,
  UnsupportedState,
};
struct TileFfpSelection {
  TileFfpDecision decision = TileFfpDecision::Portable;
  TileFfpFallbackReason reason = TileFfpFallbackReason::GpuFamily;
};

// Ignores the DXMT9_TILE_FFP off/auto/force override. Use this for diagnostics
// that need to know whether the draw/pass would be tile-eligible while the
// production route remains default-off.
TileFfpSelection classifyTileFfpForPass(core::FlatDrawStateView state, bool supportsApple3);

// Applies the DXMT9_TILE_FFP override first, then falls through to the genuine
// classifier. Use this for routing.
TileFfpSelection selectTileFfpForPass(core::FlatDrawStateView state, bool supportsApple3);

// R-BACK-12.22 / 12.23 — Stage 2 argbuf-hybrid per-pass selector.
//
// Decision tree (in order):
//   1. !argbufHybridEnabled (capability gate fail) -> Stage1
//   2. otherwise                                   -> Stage2
//
// `argbufHybridEnabled` is the cached AND of `argumentBuffersTier ≥ 2`
// and `supportsApple3`, populated once on the resource pool at queue
// init. The selector is deliberately conservative: when the capability
// gate fails for any reason the pass falls back to Stage 1 — never
// mid-pass switches (R-BACK-12.22 sentence 2).
//
// Texture-free and texture-bound draws use the same Stage 2 path when the
// capability gate holds. CPU-side argbuf descriptor and encoder-recorder tests
// pin the typed texture/sampler ids; shader-corpus texture probes provide the
// live sampling evidence on capable devices.
enum class ArgbufHybridDecision : std::uint8_t { Stage1, Stage2 };

ArgbufHybridDecision selectArgbufHybridForPass(core::FlatDrawStateView state,
                                                bool argbufHybridEnabled);
// Stage 2b direct-cbuf is the default for constants-only Stage 2 draws.
// An explicitly present empty value or "0" is the rollback escape hatch;
// any other non-empty value enables it. A null value represents an unset
// environment variable and therefore resolves to the default-on policy.
bool resolveArgbufDirectCbufEnabled(const char* envValue) noexcept;
bool argbufDirectCbufEnabled() noexcept;

}  // namespace dxmt9::pipeline
