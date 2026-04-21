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
#include "../winemetal/Metal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <unordered_map>

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

}  // namespace dxmt9::pipeline
