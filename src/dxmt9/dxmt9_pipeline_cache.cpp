#include "dxmt9_pipeline_cache.hpp"
#include "dxmt9_draw_shader.hpp"
#include "dxmt9_format_convert.hpp"
#include "dxmt9_shader_sources.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
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
}  // namespace

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
                            [device, color, pixelFormat, archive, archivePath]() mutable {
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
    auto pso = device.newRenderPipelineState(info, err);
    if (pso && archive && *archive && archivePath) {
      shaders::persistShaderArchive(*archive, *archivePath);
    }
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
                            [device, sampleCountVal, pixelFormat, archive, archivePath]() mutable {
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
    auto pso = device.newRenderPipelineState(info, err);
    if (pso && archive && *archive && archivePath) {
      shaders::persistShaderArchive(*archive, *archivePath);
    }
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
  std::lock_guard lock(mutex);
  if (auto it = this->draw.find(key); it != this->draw.end()) {
    return it->second.future;
  }
  auto future = std::async(std::launch::async,
                            [device, key, draw, archive, archivePath]() mutable {
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
    auto pso = device.newRenderPipelineState(info, err);
    if (pso && archive && *archive && archivePath) {
      shaders::persistShaderArchive(*archive, *archivePath);
    }
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
  auto future = std::async(std::launch::async,
                            [device, opaqueAlpha, archive, archivePath]() mutable {
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
    auto pso = device.newRenderPipelineState(info, err);
    if (pso && archive && *archive && archivePath) {
      shaders::persistShaderArchive(*archive, *archivePath);
    }
    return pso;
  });
  return future.share();
}

}  // namespace dxmt9::pipeline
