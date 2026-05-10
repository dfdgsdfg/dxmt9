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
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>

namespace dxmt9 { namespace resources { struct Pool; } }

namespace dxmt9::pipeline {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

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

struct ShaderVariantKey {
  u64 hash = 0;
  bool textured = false;
  bool linear = false;
  bool clipPlanes = false;
  bool alphaTest = false;
  bool alphaToCoverage = false;
  // R-BACK-13.3 — tile-FFP-mode bit. Two draws with the same FFPKeyPS
  // but different tile-mode selection compile separate pipeline states
  // (one fragment PSO, one tile PSO). The portable-path key always sets
  // this to false; the selector flips it on at encoder open when the
  // pass is chosen to run on the tile path.
  bool tileFfpMode = false;
  u32 sampleCount = 1;
  std::array<u32, core::kMaxRenderTargets> colorFormats{};
  std::array<BlendAttachmentKey, core::kMaxRenderTargets> blend{};
  u32 depthFormat = 0;
  u32 stencilFormat = 0;

  friend bool operator==(const ShaderVariantKey&, const ShaderVariantKey&) = default;
};

struct ShaderVariantKeyHash {
  std::size_t operator()(const ShaderVariantKey& key) const noexcept;
};

namespace detail {

// Render-state-only blend attachment key mapping. Pixel formats are resolved
// by Cache::getOrBuildDrawPipelineForState after surface lookup.
std::array<BlendAttachmentKey, core::kMaxRenderTargets>
makeBlendAttachmentKeys(core::FlatDrawStateView state, bool forceVisibleDraw = false);

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

struct Entry {
  std::shared_future<WMT::Reference<WMT::RenderPipelineState>> future;
};

using PipelineMap = std::unordered_map<ShaderVariantKey, Entry, ShaderVariantKeyHash>;
using DepthMap =
    std::unordered_map<DepthStencilKey, WMT::Reference<WMT::DepthStencilState>, DepthStencilKeyHash>;

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
                                  bool tileFfpMode = false);

  std::mutex mutex{};
  PipelineMap draw{};
  PipelineMap fill{};
  PipelineMap stretch{};
  DepthMap depth{};
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

// Compose a ShaderVariantKey from a flat draw state view + pre-resolved attachment
// state. The layoutHash incorporates the FFP layout hash or, if not FFP,
// a vertex-declaration hash.
ShaderVariantKey makeShaderVariantKey(core::FlatDrawStateView state,
                                       std::span<const u32> colorFormats,
                                       std::span<const BlendAttachmentKey> blendAttachments,
                                       u32 depthFormat,
                                       u32 stencilFormat);

// R-BACK-13.* — per-pass tile-shader FFP selector. Encapsulates the
// selection flow described in design.md §13.1. Pure value transform; no
// Metal calls. Reads the pixel-key + alpha-test reference + A2C state
// from `state` and combines with `supportsApple3`.
//
// Decision tree (in order):
//   1. !supportsApple3                                 -> Portable, GpuFamily
//   2. PS not fixed-function                           -> Portable, NotFfp
//   3. classifyTileFfpEligibility() -> Eligible        -> Tile, None
//   4. classifyTileFfpEligibility() -> reason          -> Portable, reason
//
// `reason == None` implies the tile path was chosen.
// `reason == GpuFamily` is recorded but design.md §13.3 does NOT bump
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

TileFfpSelection selectTileFfpForPass(core::FlatDrawStateView state, bool supportsApple3);

}  // namespace dxmt9::pipeline
