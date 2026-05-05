#include "dxmt9_pipeline_cache.hpp"
#include "dxmt9_draw_shader.hpp"
#include "dxmt9_ffp_shaders.hpp"
#include "dxmt9_format_convert.hpp"
#include "dxmt9_perf_counters.hpp"
#include "dxmt9_resource_pool.hpp"
#include "dxmt9_shader_sources.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <future>
#include <utility>

namespace dxmt9::pipeline {

namespace {
constexpr u64 kFnvOffset = 1469598103934665603ull;
constexpr u64 kFnvPrime = 1099511628211ull;

inline u64 mix(u64 hash, u64 value) {
  hash ^= value;
  hash *= kFnvPrime;
  return hash;
}

// Mirror of backend_metal.mm's file-local debugForceVisibleDraw. Keeps this
// translation unit self-contained so Cache doesn't depend on backend state.
bool debugForceVisibleDraw() {
  const char* env = std::getenv("DXMT9_DEBUG_FORCE_VISIBLE_DRAW");
  return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
}

}  // namespace

std::array<BlendAttachmentKey, core::kMaxRenderTargets>
detail::makeBlendAttachmentKeys(core::FlatDrawStateView state, bool forceVisibleDraw) {
  std::array<BlendAttachmentKey, core::kMaxRenderTargets> blendAttachments{};
  const auto& rs = state.hot->renderStates;
  const bool blendEnabled =
      !forceVisibleDraw && core::flatStateOr(rs, core::RS_ALPHABLEND_ENABLE, 0u) != 0;
  const u32 rgbBlendOperation =
      core::flatStateOr(rs, core::RS_BLEND_OP, static_cast<u32>(core::BlendOp::Add));
  const u32 alphaBlendOperation =
      core::flatStateOr(rs, core::RS_BLEND_OP_ALPHA, rgbBlendOperation);
  const u32 sourceRGBBlendFactor =
      core::flatStateOr(rs, core::RS_SRC_BLEND, static_cast<u32>(core::BlendFactor::One));
  const u32 destinationRGBBlendFactor =
      core::flatStateOr(rs, core::RS_DEST_BLEND, static_cast<u32>(core::BlendFactor::Zero));
  const u32 sourceAlphaBlendFactor =
      core::flatStateOr(rs, core::RS_SRC_BLEND_ALPHA, sourceRGBBlendFactor);
  const u32 destinationAlphaBlendFactor =
      core::flatStateOr(rs, core::RS_DEST_BLEND_ALPHA, destinationRGBBlendFactor);
  const u32 colorWriteMask =
      forceVisibleDraw ? 0xfu : core::flatStateOr(rs, core::RS_COLOR_WRITE_ENABLE, 0xfu);

  for (auto& blend : blendAttachments) {
    blend.blendingEnabled = blendEnabled;
    blend.rgbBlendOperation = rgbBlendOperation;
    blend.alphaBlendOperation = alphaBlendOperation;
    blend.sourceRGBBlendFactor = sourceRGBBlendFactor;
    blend.destinationRGBBlendFactor = destinationRGBBlendFactor;
    blend.sourceAlphaBlendFactor = sourceAlphaBlendFactor;
    blend.destinationAlphaBlendFactor = destinationAlphaBlendFactor;
    blend.colorWriteMask = colorWriteMask;
  }
  return blendAttachments;
}

std::array<BlendAttachmentKey, core::kMaxRenderTargets>
detail::makeBlendAttachmentKeys(const core::DrawDesc& draw, bool forceVisibleDraw) {
  const auto hot = core::makeFlatDrawStateRecord(draw);
  return detail::makeBlendAttachmentKeys(
      core::FlatDrawStateView{.hot = &hot, .coldDesc = &draw}, forceVisibleDraw);
}

std::size_t BlendAttachmentKeyHash::operator()(const BlendAttachmentKey& key) const noexcept {
  u64 hash = kFnvOffset;
  hash = mix(hash, static_cast<u64>(key.blendingEnabled));
  hash = mix(hash, key.rgbBlendOperation);
  hash = mix(hash, key.alphaBlendOperation);
  hash = mix(hash, key.sourceRGBBlendFactor);
  hash = mix(hash, key.destinationRGBBlendFactor);
  hash = mix(hash, key.sourceAlphaBlendFactor);
  hash = mix(hash, key.destinationAlphaBlendFactor);
  hash = mix(hash, key.colorWriteMask);
  hash = mix(hash, key.pixelFormat);
  return static_cast<std::size_t>(hash);
}

std::size_t StencilFaceKeyHash::operator()(const StencilFaceKey& key) const noexcept {
  u64 hash = kFnvOffset;
  hash = mix(hash, static_cast<u64>(key.enabled));
  hash = mix(hash, key.compareFunction);
  hash = mix(hash, key.failureOperation);
  hash = mix(hash, key.depthFailureOperation);
  hash = mix(hash, key.passOperation);
  hash = mix(hash, key.readMask);
  hash = mix(hash, key.writeMask);
  return static_cast<std::size_t>(hash);
}

std::size_t ShaderVariantKeyHash::operator()(const ShaderVariantKey& key) const noexcept {
  u64 hash = key.hash;
  hash = mix(hash, static_cast<u64>(key.textured));
  hash = mix(hash, static_cast<u64>(key.linear));
  hash = mix(hash, static_cast<u64>(key.clipPlanes));
  hash = mix(hash, static_cast<u64>(key.alphaTest));
  hash = mix(hash, static_cast<u64>(key.alphaToCoverage));
  hash = mix(hash, key.sampleCount);
  for (auto fmt : key.colorFormats) {
    hash = mix(hash, fmt);
  }
  for (const auto& b : key.blend) {
    hash = mix(hash, static_cast<u64>(b.blendingEnabled));
    hash = mix(hash, b.rgbBlendOperation);
    hash = mix(hash, b.alphaBlendOperation);
    hash = mix(hash, b.sourceRGBBlendFactor);
    hash = mix(hash, b.destinationRGBBlendFactor);
    hash = mix(hash, b.sourceAlphaBlendFactor);
    hash = mix(hash, b.destinationAlphaBlendFactor);
    hash = mix(hash, b.colorWriteMask);
    hash = mix(hash, b.pixelFormat);
  }
  hash = mix(hash, key.depthFormat);
  hash = mix(hash, key.stencilFormat);
  return static_cast<std::size_t>(hash);
}

std::size_t DepthStencilKeyHash::operator()(const DepthStencilKey& key) const noexcept {
  u64 hash = kFnvOffset;
  hash = mix(hash, static_cast<u64>(key.depthEnable));
  hash = mix(hash, static_cast<u64>(key.depthWrite));
  hash = mix(hash, key.depthFunc);
  StencilFaceKeyHash faceHash{};
  hash = mix(hash, static_cast<u64>(faceHash(key.front)));
  hash = mix(hash, static_cast<u64>(faceHash(key.back)));
  return static_cast<std::size_t>(hash);
}

WMT::Reference<WMT::DepthStencilState> Cache::depthStencilStateFor(WMT::Device& device,
                                                                     const DepthStencilKey& key) {
  std::lock_guard lock(mutex);
  if (auto it = depth.find(key); it != depth.end()) {
    return it->second;
  }
  WMTDepthStencilInfo info{};
  info.depth_compare_function =
      static_cast<WMTCompareFunction>(convert::toCompareFunction(key.depthFunc));
  info.depth_write_enabled = key.depthEnable && key.depthWrite;
  auto applyFace = [](WMTStencilInfo& stencilInfo, const StencilFaceKey& face) {
    stencilInfo.enabled = face.enabled;
    stencilInfo.stencil_compare_function =
        static_cast<WMTCompareFunction>(convert::toCompareFunction(face.compareFunction));
    stencilInfo.stencil_fail_op =
        static_cast<WMTStencilOperation>(convert::toStencilOperation(face.failureOperation));
    stencilInfo.depth_fail_op =
        static_cast<WMTStencilOperation>(convert::toStencilOperation(face.depthFailureOperation));
    stencilInfo.depth_stencil_pass_op =
        static_cast<WMTStencilOperation>(convert::toStencilOperation(face.passOperation));
    stencilInfo.read_mask = static_cast<uint8_t>(face.readMask);
    stencilInfo.write_mask = static_cast<uint8_t>(face.writeMask);
  };
  if (key.front.enabled || key.back.enabled) {
    applyFace(info.front_stencil, key.front);
    applyFace(info.back_stencil, key.back.enabled ? key.back : key.front);
  }
  auto state = device.newDepthStencilState(info);
  depth.emplace(key, state);
  return state;
}

std::shared_future<WMT::Reference<WMT::RenderPipelineState>>
Cache::getOrBuildFillPipeline(WMT::Reference<WMT::Device> device,
                                const core::ColorRGBA& color,
                                u32 pixelFormat,
                                WMT::Reference<WMT::BinaryArchive>* archive,
                                const std::string* archivePath) {
  (void)archivePath;
  ShaderVariantKey key{};
  // Hash includes color channels + format so matching fills share a pipeline.
  key.hash = static_cast<u64>(std::bit_cast<u32>(color.r)) ^
             (static_cast<u64>(std::bit_cast<u32>(color.g)) << 1) ^ pixelFormat;
  key.colorFormats[0] = pixelFormat;
  key.blend[0].pixelFormat = pixelFormat;
  std::lock_guard lock(mutex);
  if (auto it = fill.find(key); it != fill.end()) {
    return it->second.future;
  }
  auto future = std::async(std::launch::async,
                            [device, color, pixelFormat, archive]() mutable {
    auto vsLib = shaders::makeLibrary(device, shaders::makeGenericVertexSource(shaders::makeHash("fill")));
    auto fsLib = shaders::makeLibrary(device, shaders::makeGenericFragmentSource(color, shaders::makeHash("fill")));
    if (!vsLib || !fsLib) {
      return WMT::Reference<WMT::RenderPipelineState>{};
    }
    auto vs = vsLib.newFunction("dxmt9_vs");
    auto fs = fsLib.newFunction("dxmt9_fs");
    WMTRenderPipelineInfo info{};
    info.max_tessellation_factor = 1;
    info.vertex_function = vs.handle;
    info.fragment_function = fs.handle;
    info.colors[0].pixel_format = static_cast<WMTPixelFormat>(pixelFormat);
    info.colors[0].write_mask = WMTColorWriteMaskAll;
    info.rasterization_enabled = true;
    info.raster_sample_count = 1;
    if (archive && *archive) info.binary_archive_for_serialization = (*archive).handle;
    WMT::Error err{};
    perf::countPipelineBuild();
    auto pso = device.newRenderPipelineState(info, err);
    return pso;
  });
  auto shared = future.share();
  fill.emplace(key, Entry{shared});
  return shared;
}

std::shared_future<WMT::Reference<WMT::RenderPipelineState>>
Cache::getOrBuildStretchPipeline(WMT::Reference<WMT::Device> device,
                                   const core::StretchRectDesc& stretch,
                                   u32 pixelFormat,
                                   WMT::Reference<WMT::BinaryArchive>* archive,
                                   const std::string* archivePath) {
  (void)archivePath;
  ShaderVariantKey key{};
  key.hash = stretch.linear ? 1u : 0u;
  key.textured = true;
  key.linear = stretch.linear;
  key.sampleCount = std::max(1u, stretch.destinationSampleCount);
  key.colorFormats[0] = pixelFormat;
  key.blend[0].pixelFormat = pixelFormat;
  std::lock_guard lock(mutex);
  if (auto it = this->stretch.find(key); it != this->stretch.end()) {
    return it->second.future;
  }
  const u32 sampleCountVal = key.sampleCount;
  auto future = std::async(std::launch::async,
                            [device, sampleCountVal, pixelFormat, archive]() mutable {
    auto vsLib = shaders::makeLibrary(device, shaders::makeTexturedVertexSource(shaders::makeHash("stretch")));
    auto fsLib = shaders::makeLibrary(device, shaders::makeTexturedFragmentSource(shaders::makeHash("stretch")));
    if (!vsLib || !fsLib) return WMT::Reference<WMT::RenderPipelineState>{};
    auto vs = vsLib.newFunction("dxmt9_vs");
    auto fs = fsLib.newFunction("dxmt9_fs");
    WMTRenderPipelineInfo info{};
    info.max_tessellation_factor = 1;
    info.vertex_function = vs.handle;
    info.fragment_function = fs.handle;
    info.raster_sample_count = sampleCountVal;
    info.colors[0].pixel_format = static_cast<WMTPixelFormat>(pixelFormat);
    info.colors[0].write_mask = WMTColorWriteMaskAll;
    info.rasterization_enabled = true;
    if (archive && *archive) info.binary_archive_for_serialization = (*archive).handle;
    WMT::Error err{};
    perf::countPipelineBuild();
    auto pso = device.newRenderPipelineState(info, err);
    return pso;
  });
  auto shared = future.share();
  this->stretch.emplace(key, Entry{shared});
  return shared;
}

std::shared_future<WMT::Reference<WMT::RenderPipelineState>>
Cache::getOrBuildDrawPipeline(WMT::Reference<WMT::Device> device,
                                const ShaderVariantKey& key,
                                const core::DrawDesc& draw,
                                WMT::Reference<WMT::BinaryArchive>* archive,
                                const std::string* archivePath) {
  (void)archivePath;
  std::lock_guard lock(mutex);
  if (auto it = this->draw.find(key); it != this->draw.end()) {
    return it->second.future;
  }
  auto future = std::async(std::launch::async,
                            [device, key, draw, archive]() mutable {
    auto vsSource = drawshader::makeDrawShaderSource(draw, true);
    auto fsSource = drawshader::makeDrawShaderSource(draw, false);
    auto vsLib = shaders::makeLibrary(device, vsSource);
    auto fsLib = shaders::makeLibrary(device, fsSource);
    if (!vsLib || !fsLib) {
      return WMT::Reference<WMT::RenderPipelineState>{};
    }
    auto vs = vsLib.newFunction("dxmt9_vs");
    auto fs = fsLib.newFunction("dxmt9_fs");
    if (!vs || !fs) {
      return WMT::Reference<WMT::RenderPipelineState>{};
    }
    WMTRenderPipelineInfo info{};
    info.max_tessellation_factor = 1;
    info.vertex_function = vs.handle;
    info.fragment_function = fs.handle;
    info.raster_sample_count = std::max(1u, key.sampleCount);
    info.alpha_to_coverage_enabled = key.alphaToCoverage;
    info.depth_pixel_format = static_cast<WMTPixelFormat>(key.depthFormat);
    info.stencil_pixel_format = static_cast<WMTPixelFormat>(key.stencilFormat);
    info.rasterization_enabled = true;
    if (archive && *archive) {
      info.binary_archive_for_serialization = (*archive).handle;
    }
    for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
      auto& ca = info.colors[i];
      ca.pixel_format = static_cast<WMTPixelFormat>(key.colorFormats[i]);
      ca.blending_enabled = key.blend[i].blendingEnabled;
      ca.rgb_blend_operation = convert::toBlendOperation(key.blend[i].rgbBlendOperation);
      ca.alpha_blend_operation = convert::toBlendOperation(key.blend[i].alphaBlendOperation);
      ca.src_rgb_blend_factor = convert::toBlendFactor(key.blend[i].sourceRGBBlendFactor);
      ca.dst_rgb_blend_factor = convert::toBlendFactor(key.blend[i].destinationRGBBlendFactor);
      ca.src_alpha_blend_factor = convert::toBlendFactor(key.blend[i].sourceAlphaBlendFactor);
      ca.dst_alpha_blend_factor = convert::toBlendFactor(key.blend[i].destinationAlphaBlendFactor);
      ca.write_mask = convert::toColorWriteMask(key.blend[i].colorWriteMask);
    }
    WMT::Error err{};
    perf::countPipelineBuild();
    auto pso = device.newRenderPipelineState(info, err);
    return pso;
  });
  auto shared = future.share();
  this->draw.emplace(key, Entry{shared});
  return shared;
}

std::shared_future<WMT::Reference<WMT::RenderPipelineState>>
buildPresentPipeline(WMT::Reference<WMT::Device> device, bool opaqueAlpha,
                     WMT::Reference<WMT::BinaryArchive>* archive,
                     const std::string* archivePath) {
  (void)archivePath;
  auto future = std::async(std::launch::async,
                            [device, opaqueAlpha, archive]() mutable {
    auto vsLib = shaders::makeLibrary(device, shaders::makeTexturedVertexSource(shaders::makeHash("present")));
    auto fsLib = shaders::makeLibrary(device, shaders::makeTexturedFragmentSource(
                                          shaders::makeHash(opaqueAlpha ? "present-opaque" : "present"),
                                          opaqueAlpha));
    if (!vsLib || !fsLib) return WMT::Reference<WMT::RenderPipelineState>{};
    auto vs = vsLib.newFunction("dxmt9_vs");
    auto fs = fsLib.newFunction("dxmt9_fs");
    WMTRenderPipelineInfo info{};
    info.max_tessellation_factor = 1;
    info.vertex_function = vs.handle;
    info.fragment_function = fs.handle;
    info.raster_sample_count = 1;
    info.colors[0].pixel_format = WMTPixelFormatBGRA8Unorm;
    info.colors[0].write_mask = WMTColorWriteMaskAll;
    info.rasterization_enabled = true;
    if (archive && *archive) info.binary_archive_for_serialization = (*archive).handle;
    WMT::Error err{};
    perf::countPipelineBuild();
    auto pso = device.newRenderPipelineState(info, err);
    return pso;
  });
  return future.share();
}

std::shared_future<WMT::Reference<WMT::RenderPipelineState>>
Cache::getOrBuildDrawPipelineForState(WMT::Reference<WMT::Device> device,
                                      const core::BackendLimits& limits,
                                      resources::Pool& pool,
                                      core::FlatDrawStateView state,
                                      WMT::Reference<WMT::BinaryArchive>* archive,
                                      const std::string* archivePath) {
  auto resolvePixelFormat = [&](core::Handle handle) -> u32 {
    if (!handle) {
      return 0;
    }
    if (auto* surface = pool.findSurface(handle.value); surface) {
      return static_cast<u32>(dxmt9::convert::toPixelFormat(surface->desc.format, limits));
    }
    return 0;
  };

  std::array<u32, core::kMaxRenderTargets> colorFormats{};
  auto blendAttachments = detail::makeBlendAttachmentKeys(state, debugForceVisibleDraw());
  for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
    colorFormats[i] = resolvePixelFormat(state.hot->colorAttachments[i].handle);
    blendAttachments[i].pixelFormat = colorFormats[i];
  }
  u32 depthFormat = 0;
  u32 stencilFormat = 0;
  if (state.hot->depthStencil.handle) {
    if (auto* surface = pool.findSurface(state.hot->depthStencil.handle.value);
        surface && surface->desc.depthStencil) {
      const auto pixelFormat =
          static_cast<u32>(dxmt9::convert::toPixelFormat(surface->desc.format, limits));
      depthFormat = dxmt9::convert::formatHasDepthAspect(surface->desc.format) ? pixelFormat : 0u;
      stencilFormat =
          dxmt9::convert::formatHasStencilAspect(surface->desc.format) ? pixelFormat : 0u;
    }
  }
  const auto key = makeShaderVariantKey(state, colorFormats, blendAttachments, depthFormat, stencilFormat);
  return getOrBuildDrawPipeline(device, key, state.desc(), archive, archivePath);
}

std::shared_future<WMT::Reference<WMT::RenderPipelineState>>
Cache::getOrBuildDrawPipelineForDraw(WMT::Reference<WMT::Device> device,
                                      const core::BackendLimits& limits,
                                      resources::Pool& pool,
                                      const core::DrawDesc& draw,
                                      WMT::Reference<WMT::BinaryArchive>* archive,
                                      const std::string* archivePath) {
  const auto hot = core::makeFlatDrawStateRecord(draw);
  return getOrBuildDrawPipelineForState(
      device, limits, pool, core::FlatDrawStateView{.hot = &hot, .coldDesc = &draw},
      archive, archivePath);
}

ShaderVariantKey makeShaderVariantKey(core::FlatDrawStateView state,
                                       std::span<const u32> colorFormats,
                                       std::span<const BlendAttachmentKey> blendAttachments,
                                       u32 depthFormat,
                                       u32 stencilFormat) {
  const auto& desc = state.desc();
  const auto& hot = *state.hot;
  const auto* shader = state.hasShaderContext() ? &state.shaderContext() : nullptr;
  const auto& vertexDecl = shader ? shader->vertexDecl : desc.vertexDecl;
  const auto& vertexShader = shader ? shader->vertexShader : desc.vertexShader;
  const auto& pixelShader = shader ? shader->pixelShader : desc.pixelShader;
  ShaderVariantKey key{};
  const auto layout = ffp::decodeFixedFunctionVertexLayout(vertexDecl);
  const u64 layoutHash = layout ? layout->hash : ffp::hashVertexDeclaration(vertexDecl);
  key.hash = vertexShader.hash ^ (pixelShader.hash << 1) ^ hot.clipPlaneMask ^ depthFormat ^
             (stencilFormat << 1) ^ (layoutHash << 1) ^ vertexDecl.fvf;
  key.textured = hot.textures[0] != core::Handle{};
  key.linear =
      core::flatStateOr(hot.samplerStates[0], core::SAMP_MIN_FILTER, 0u) == 2u ||
      core::flatStateOr(hot.samplerStates[0], core::SAMP_MAG_FILTER, 0u) == 2u;
  key.clipPlanes = hot.clipPlaneMask != 0;
  key.alphaTest = core::flatStateOr(hot.renderStates, core::RS_ALPHA_TEST_ENABLE, 0u) != 0;
  key.alphaToCoverage = false;
  key.sampleCount = std::max(1u, hot.colorAttachments[0].sampleCount);
  for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
    key.colorFormats[i] = i < colorFormats.size() ? colorFormats[i] : 0u;
    if (i < blendAttachments.size()) {
      key.blend[i] = blendAttachments[i];
    }
  }
  key.depthFormat = depthFormat;
  key.stencilFormat = stencilFormat;
  return key;
}

ShaderVariantKey makeShaderVariantKey(const core::DrawDesc& desc,
                                       std::span<const u32> colorFormats,
                                       std::span<const BlendAttachmentKey> blendAttachments,
                                       u32 depthFormat,
                                       u32 stencilFormat) {
  const auto hot = core::makeFlatDrawStateRecord(desc);
  return makeShaderVariantKey(core::FlatDrawStateView{.hot = &hot, .coldDesc = &desc},
                              colorFormats, blendAttachments, depthFormat, stencilFormat);
}

}  // namespace dxmt9::pipeline
