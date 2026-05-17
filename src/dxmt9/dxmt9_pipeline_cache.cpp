#include "dxmt9_pipeline_cache.hpp"
#include "dxmt9_draw_shader.hpp"
#include "dxmt9_ffp_shaders.hpp"
#include "dxmt9_format_convert.hpp"
#include "dxmt9_metal_labels.hpp"
#include "dxmt9_perf_counters.hpp"
#include "dxmt9_resource_pool.hpp"
#include "dxmt9_shader_sources.hpp"
#include "util/log/log.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <future>
#include <optional>
#include <string_view>
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

bool envFlag(const char* name) noexcept {
  const char* env = std::getenv(name);
  return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
}

std::shared_future<WMT::Reference<WMT::RenderPipelineState>>
makeReadyPipelineFuture(WMT::Reference<WMT::RenderPipelineState> value = {}) {
  std::promise<WMT::Reference<WMT::RenderPipelineState>> promise;
  promise.set_value(std::move(value));
  return promise.get_future().share();
}

}  // namespace

u64 makeShaderSourceDebugEnvKey(bool trimUnusedVaryings,
                                bool fsHalfPrecision,
                                bool forceFullscreenVertex,
                                bool flipTranslatedVertexY,
                                bool forceFragmentShaderColor,
                                std::string_view fragmentMode,
                                bool forcePixelVFlip,
                                bool debugFfpUv,
                                bool debugFfpTexture,
                                bool debugFfpAlpha) noexcept {
  u64 hash = kFnvOffset;
  hash = mix(hash, kShaderDebugEnvSchemaVersion);
  hash = mix(hash, static_cast<u64>(trimUnusedVaryings));
  hash = mix(hash, static_cast<u64>(fsHalfPrecision));
  hash = mix(hash, static_cast<u64>(forceFullscreenVertex));
  hash = mix(hash, static_cast<u64>(flipTranslatedVertexY));
  hash = mix(hash, static_cast<u64>(forceFragmentShaderColor));
  hash = mix(hash, core::hashString(fragmentMode));
  hash = mix(hash, static_cast<u64>(forcePixelVFlip));
  hash = mix(hash, static_cast<u64>(debugFfpUv));
  hash = mix(hash, static_cast<u64>(debugFfpTexture));
  hash = mix(hash, static_cast<u64>(debugFfpAlpha));
  return hash;
}

u64 currentShaderSourceDebugEnvKey() noexcept {
  const char* fragmentMode = std::getenv("DXMT_DEBUG_FRAGMENT_MODE");
  return makeShaderSourceDebugEnvKey(
      envFlag("DXMT9_TRIM_UNUSED_VARYINGS"),
      envFlag("DXMT9_FS_HALF_PRECISION"),
      envFlag("DXMT_DEBUG_FORCE_FULLSCREEN_VERTEX"),
      envFlag("DXMT_DEBUG_FLIP_VERTEX_Y"),
      envFlag("DXMT_DEBUG_FORCE_FRAGMENT_COLOR"),
      fragmentMode ? std::string_view(fragmentMode) : std::string_view{},
      envFlag("DXMT_DEBUG_FORCE_PIXEL_V_FLIP"),
      envFlag("DXMT_DEBUG_FFP_UV"),
      envFlag("DXMT_DEBUG_FFP_TEXTURE"),
      envFlag("DXMT_DEBUG_FFP_ALPHA"));
}

std::optional<detail::DrawShaderSources>
detail::makeContainedDrawShaderSources(const drawshader::ShaderSourceContext& shaderSource,
                                       u64 variantHash) {
  auto shaderHash = [](const core::ShaderRef& shader) {
    return shader.hash != 0 ? shader.hash : shader.bytecode.hash;
  };

  try {
    auto vertex = drawshader::makeDrawShaderSource(shaderSource, true);
    auto fragment = drawshader::makeDrawShaderSource(shaderSource, false);
    const u64 vertexHash = shaders::makeHash(vertex);
    const u64 fragmentHash = shaders::makeHash(fragment);
    return detail::DrawShaderSources{
        .vertex = std::move(vertex),
        .fragment = std::move(fragment),
        .vertexHash = vertexHash,
        .fragmentHash = fragmentHash,
    };
  } catch (const std::exception& ex) {
    util::logf(util::LogLevel::Error, "dxmt9-pipeline-cache",
               "draw shader source generation failed: %s variant=0x%llx vs=0x%llx ps=0x%llx",
               ex.what(),
               static_cast<unsigned long long>(variantHash),
               static_cast<unsigned long long>(shaderHash(shaderSource.vertexShader)),
               static_cast<unsigned long long>(shaderHash(shaderSource.pixelShader)));
  } catch (...) {
    util::logf(util::LogLevel::Error, "dxmt9-pipeline-cache",
               "draw shader source generation failed: unknown exception variant=0x%llx vs=0x%llx ps=0x%llx",
               static_cast<unsigned long long>(variantHash),
               static_cast<unsigned long long>(shaderHash(shaderSource.vertexShader)),
               static_cast<unsigned long long>(shaderHash(shaderSource.pixelShader)));
  }

  return std::nullopt;
}

std::array<BlendAttachmentKey, core::kMaxRenderTargets>
detail::makeBlendAttachmentKeys(core::FlatDrawStateView state, bool forceVisibleDraw) {
  std::array<BlendAttachmentKey, core::kMaxRenderTargets> blendAttachments{};
  const auto& rs = state.hot->renderStates;
  const bool blendEnabled =
      !forceVisibleDraw && core::flatStateOr(rs, core::RS_ALPHABLEND_ENABLE, 0u) != 0;
  const bool separateAlphaBlend =
      core::flatStateOr(rs, core::RS_SEPARATE_ALPHA_BLEND_ENABLE, 0u) != 0;
  const u32 rgbBlendOperation =
      core::flatStateOr(rs, core::RS_BLEND_OP, static_cast<u32>(core::BlendOp::Add));
  const u32 alphaBlendOperation =
      separateAlphaBlend ? core::flatStateOr(rs, core::RS_BLEND_OP_ALPHA, rgbBlendOperation)
                         : rgbBlendOperation;
  const u32 sourceRGBBlendFactor =
      core::flatStateOr(rs, core::RS_SRC_BLEND, static_cast<u32>(core::BlendFactor::One));
  const u32 destinationRGBBlendFactor =
      core::flatStateOr(rs, core::RS_DEST_BLEND, static_cast<u32>(core::BlendFactor::Zero));
  const u32 sourceAlphaBlendFactor =
      separateAlphaBlend ? core::flatStateOr(rs, core::RS_SRC_BLEND_ALPHA, sourceRGBBlendFactor)
                         : sourceRGBBlendFactor;
  const u32 destinationAlphaBlendFactor =
      separateAlphaBlend ? core::flatStateOr(rs, core::RS_DEST_BLEND_ALPHA, destinationRGBBlendFactor)
                         : destinationRGBBlendFactor;
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
  hash = mix(hash, key.vertexSourceHash);
  hash = mix(hash, key.fragmentSourceHash);
  hash = mix(hash, key.tileSourceHash);
  hash = mix(hash, key.emitterVersion);
  hash = mix(hash, key.sourceLayoutVersion);
  hash = mix(hash, key.debugEnvSchemaVersion);
  hash = mix(hash, key.debugEnvKey);
  hash = mix(hash, static_cast<u64>(key.textured));
  hash = mix(hash, static_cast<u64>(key.linear));
  hash = mix(hash, static_cast<u64>(key.clipPlanes));
  hash = mix(hash, static_cast<u64>(key.alphaTest));
  hash = mix(hash, static_cast<u64>(key.alphaToCoverage));
  // R-BACK-13.3: tile_ffp_mode participates in the PSO key hash so the
  // fragment-stage and tile-stage variants of the same FFPKeyPS hit
  // distinct cache entries.
  hash = mix(hash, static_cast<u64>(key.tileFfpMode));
  // R-BACK-12.22 / 12.23: argbuf-hybrid mode participates in the key
  // hash so Stage 1 and Stage 2 PSOs of the same shader live in
  // distinct cache slots.
  hash = mix(hash, static_cast<u64>(key.argbufHybridMode));
  hash = mix(hash, key.sampleCount);
  for (auto type : key.textureTypes) {
    hash = mix(hash, type);
  }
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
    perf::countPipelineCacheHit(perf::PipelineKind::Fill);
    return it->second.future;
  }
  perf::countPipelineCacheMiss(perf::PipelineKind::Fill);
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
    perf::countPipelineBuild(perf::PipelineKind::Fill);
    auto pso = device.newRenderPipelineState(info, err);
    if (pso) {
      const unsigned colorPacked =
          (static_cast<unsigned>(std::clamp(color.r, 0.0f, 1.0f) * 255) << 24) |
          (static_cast<unsigned>(std::clamp(color.g, 0.0f, 1.0f) * 255) << 16) |
          (static_cast<unsigned>(std::clamp(color.b, 0.0f, 1.0f) * 255) <<  8) |
          (static_cast<unsigned>(std::clamp(color.a, 0.0f, 1.0f) * 255));
      pso.setLabel(labels::makeLabelStringFmt("pso_fill_fmt%u_rgba0x%08x",
                                                static_cast<unsigned>(pixelFormat),
                                                colorPacked));
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
    perf::countPipelineCacheHit(perf::PipelineKind::Stretch);
    return it->second.future;
  }
  perf::countPipelineCacheMiss(perf::PipelineKind::Stretch);
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
    perf::countPipelineBuild(perf::PipelineKind::Stretch);
    auto pso = device.newRenderPipelineState(info, err);
    if (pso) {
      pso.setLabel(labels::makeLabelStringFmt(
          "pso_stretch_fmt%u_msaa%u",
          static_cast<unsigned>(pixelFormat),
          static_cast<unsigned>(sampleCountVal)));
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
                                drawshader::ShaderSourceContext shaderSource,
                                WMT::Reference<WMT::BinaryArchive>* archive,
                                const std::string* archivePath) {
  (void)archivePath;
  ShaderVariantKey sourceKey = key;
  // R-BACK-13.3 / 13.6: when the variant key carries tile_ffp_mode = true
  // the cache builds an MTLTileRenderPipelineState via
  // makeFfpTilePixelSource() instead of the standard fragment PSO. The
  // resulting Reference<RenderPipelineState> is opaque to callers; the
  // encoder switches between setRenderPipelineState() and
  // setTileRenderPipelineState() based on the same bit on the key.
  if (key.tileFfpMode) {
    core::FfpPixelKey psKey{};
    if (shaderSource.pixelShader.kind == core::ShaderRef::Kind::FixedFunctionPixel &&
        shaderSource.pixelShader.pixelKey.has_value()) {
      psKey = *shaderSource.pixelShader.pixelKey;
    }
    std::string tileSource;
    try {
      tileSource = ffp::makeFfpTilePixelSource(psKey, shaderSource, key.colorFormats[0]);
    } catch (const std::exception& ex) {
      util::logf(util::LogLevel::Error, "dxmt9-pipeline-cache",
                 "tile shader source generation failed: %s variant=0x%llx",
                 ex.what(),
                 static_cast<unsigned long long>(key.hash));
      return makeReadyPipelineFuture();
    } catch (...) {
      util::logf(util::LogLevel::Error, "dxmt9-pipeline-cache",
                 "tile shader source generation failed: unknown exception variant=0x%llx",
                 static_cast<unsigned long long>(key.hash));
      return makeReadyPipelineFuture();
    }
    sourceKey.tileSourceHash = shaders::makeHash(tileSource);

    std::lock_guard lock(mutex);
    if (auto it = this->draw.find(sourceKey); it != this->draw.end()) {
      perf::countPipelineCacheHit(perf::PipelineKind::Draw);
      return it->second.future;
    }
    perf::countPipelineCacheMiss(perf::PipelineKind::Draw);
    auto future = std::async(
        std::launch::async,
        [device, sourceKey, tileSource = std::move(tileSource), archive]() mutable {
          auto tileLib = shaders::makeLibrary(device, tileSource);
          if (!tileLib) {
            return WMT::Reference<WMT::RenderPipelineState>{};
          }
          auto tileFn = tileLib.newFunction("ffp_tile");
          if (!tileFn) {
            return WMT::Reference<WMT::RenderPipelineState>{};
          }
          WMTTileRenderPipelineDescriptor desc{};
          desc.tile_function = tileFn.handle;
          desc.raster_sample_count = std::max(1u, sourceKey.sampleCount);
          desc.threadgroup_size_matches_tile_size = 1u;
          desc.max_total_threads_per_threadgroup = 0u;
          std::uint32_t attachmentCount = 0;
          for (std::size_t i = 0; i < core::kMaxRenderTargets && i < 8; ++i) {
            desc.color_attachment_pixel_formats[i] =
                static_cast<WMTPixelFormat>(sourceKey.colorFormats[i]);
            if (sourceKey.colorFormats[i] != 0u) {
              attachmentCount = static_cast<std::uint32_t>(i + 1u);
            }
          }
          for (std::size_t i = core::kMaxRenderTargets; i < 8; ++i) {
            desc.color_attachment_pixel_formats[i] = WMTPixelFormatInvalid;
          }
          desc.color_attachment_count = attachmentCount;
          WMT::Error err{};
          perf::countPipelineBuild(perf::PipelineKind::Draw);
          perf::countColdCompileAfterWarm();
          auto pso = device.newRenderPipelineState(desc, err);
          if (pso) {
            pso.setLabel(labels::makeLabelStringFmt(
                "pso_tile_ffp_h0x%llx_color0fmt%u",
                static_cast<unsigned long long>(sourceKey.hash),
                static_cast<unsigned>(sourceKey.colorFormats[0])));
          }
          (void)archive;
          return pso;
        });
    auto shared = future.share();
    this->draw.emplace(sourceKey, Entry{shared});
    return shared;
  }
  auto sources = detail::makeContainedDrawShaderSources(shaderSource, key.hash);
  if (!sources) {
    return makeReadyPipelineFuture();
  }
  sourceKey.vertexSourceHash = sources->vertexHash;
  sourceKey.fragmentSourceHash = sources->fragmentHash;
  {
    std::lock_guard lock(mutex);
    if (auto it = this->draw.find(sourceKey); it != this->draw.end()) {
      perf::countPipelineCacheHit(perf::PipelineKind::Draw);
      return it->second.future;
    }
    perf::countPipelineCacheMiss(perf::PipelineKind::Draw);
    auto future = std::async(
        std::launch::async,
        [device, sourceKey, sources = std::move(*sources), archive]() mutable {
          auto vsLib = shaders::makeLibrary(device, sources.vertex);
          auto fsLib = shaders::makeLibrary(device, sources.fragment);
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
          info.raster_sample_count = std::max(1u, sourceKey.sampleCount);
          info.alpha_to_coverage_enabled = sourceKey.alphaToCoverage;
          info.depth_pixel_format = static_cast<WMTPixelFormat>(sourceKey.depthFormat);
          info.stencil_pixel_format = static_cast<WMTPixelFormat>(sourceKey.stencilFormat);
          info.rasterization_enabled = true;
          if (archive && *archive) {
            info.binary_archive_for_serialization = (*archive).handle;
          }
          for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
            auto& ca = info.colors[i];
            ca.pixel_format = static_cast<WMTPixelFormat>(sourceKey.colorFormats[i]);
            ca.blending_enabled = sourceKey.blend[i].blendingEnabled;
            ca.rgb_blend_operation = convert::toBlendOperation(sourceKey.blend[i].rgbBlendOperation);
            ca.alpha_blend_operation =
                convert::toBlendOperation(sourceKey.blend[i].alphaBlendOperation);
            ca.src_rgb_blend_factor =
                convert::toBlendFactor(sourceKey.blend[i].sourceRGBBlendFactor);
            ca.dst_rgb_blend_factor =
                convert::toBlendFactor(sourceKey.blend[i].destinationRGBBlendFactor);
            ca.src_alpha_blend_factor =
                convert::toBlendFactor(sourceKey.blend[i].sourceAlphaBlendFactor);
            ca.dst_alpha_blend_factor =
                convert::toBlendFactor(sourceKey.blend[i].destinationAlphaBlendFactor);
            ca.write_mask = convert::toColorWriteMask(sourceKey.blend[i].colorWriteMask);
          }
          WMT::Error err{};
          perf::countPipelineBuild(perf::PipelineKind::Draw);
          // R-BACK-3.8 — every Draw PSO that lands in this closure is a
          // miss against the prewarmed archive. The counter starts at zero
          // after a successful Full prewarm and stays low when the archive
          // is hot; a high value indicates the prewarm path missed (file
          // missing, schema drift, lock_busy, or content mismatch with the
          // current emitter / variant key).
          perf::countColdCompileAfterWarm();
          auto pso = device.newRenderPipelineState(info, err);
          if (pso) {
            pso.setLabel(labels::makeLabelStringFmt(
                "pso_draw_h0x%llx_msaa%u_color0fmt%u",
                static_cast<unsigned long long>(sourceKey.hash),
                static_cast<unsigned>(std::max(1u, sourceKey.sampleCount)),
                static_cast<unsigned>(sourceKey.colorFormats[0])));
          }
          return pso;
        });
    auto shared = future.share();
    this->draw.emplace(sourceKey, Entry{shared});
    return shared;
  }
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
    perf::countPipelineBuild(perf::PipelineKind::Present);
    auto pso = device.newRenderPipelineState(info, err);
    if (pso) {
      pso.setLabel(labels::makeLabelStringFmt("pso_present_%s",
                                                opaqueAlpha ? "opaque" : "alpha"));
    }
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
                                      const std::string* archivePath,
                                      bool tileFfpMode,
                                      bool argbufHybridMode) {
  const bool srgbWrite =
      core::flatStateOr(state.hot->renderStates, core::RS_SRGB_WRITE_ENABLE, 0u) != 0;
  auto resolvePixelFormat = [&](core::Handle handle) -> u32 {
    if (!handle) {
      return 0;
    }
    if (auto* surface = pool.findSurface(handle.value); surface) {
      return static_cast<u32>(
          dxmt9::convert::toPixelFormat(surface->desc.format, limits, srgbWrite));
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
  auto key = makeShaderVariantKey(state, colorFormats, blendAttachments, depthFormat, stencilFormat);
  // R-BACK-13.3: stamp the tile-FFP-mode bit onto the variant key so the
  // tile-stage and fragment-stage variants share an FFPKeyPS but land in
  // distinct cache entries.
  key.tileFfpMode = tileFfpMode;
  // R-BACK-12.22 / 12.23: stamp the argbuf-hybrid-mode bit so Stage 1
  // and Stage 2 variants of the same shader compile separate PSOs.
  key.argbufHybridMode = argbufHybridMode;
  drawshader::ShaderSourceContext shaderSource =
      drawshader::makeShaderSourceContext(state.shaderContext(), *state.hot);
  // R-BACK-12.22..12.26 MSL routing — propagate the variant key bit into
  // the source-emitter context so FFP and DXBC->MSL bodies read uniforms
  // through `ArgbufLayout` at slot 30 instead of slots 0/3.
  shaderSource.argbufHybridMode = argbufHybridMode;
  return getOrBuildDrawPipeline(device, key, std::move(shaderSource), archive, archivePath);
}

TileFfpSelection selectTileFfpForPass(core::FlatDrawStateView state, bool supportsApple3) {
  // R-BACK-13.5: tile-FFP is unconditionally off on non-Apple3.
  if (!supportsApple3) {
    return TileFfpSelection{TileFfpDecision::Portable, TileFfpFallbackReason::GpuFamily};
  }
  // Tile path only applies when the pixel shader is the fixed-function
  // FFP. A programmable shader has no FFP key to translate; portable.
  if (!state.hasShaderContext()) {
    return TileFfpSelection{TileFfpDecision::Portable, TileFfpFallbackReason::NotFfp};
  }
  const auto& shader = state.shaderContext();
  if (shader.pixelShader.kind != core::ShaderRef::Kind::FixedFunctionPixel ||
      !shader.pixelShader.pixelKey.has_value()) {
    return TileFfpSelection{TileFfpDecision::Portable, TileFfpFallbackReason::NotFfp};
  }
  if (shader.vertexShader.kind == core::ShaderRef::Kind::FixedFunctionVertex &&
      shader.vertexShader.vertexKey.has_value() &&
      (shader.vertexShader.vertexKey->vertexBlend != 0 ||
       shader.vertexShader.vertexKey->indexedVertexBlend)) {
    return TileFfpSelection{
        TileFfpDecision::Portable,
        TileFfpFallbackReason::UnsupportedState,
    };
  }
  for (const auto& texture : state.hot->textures) {
    if (texture) {
      // The current tile-FFP kernel is a tile-memory alpha/fog transform,
      // not a replacement for a full fragment shader with interpolated
      // texcoords and texture sampling. Keep textured FFP draws on the
      // portable fragment path until tile/portable readback equality exists.
      return TileFfpSelection{
          TileFfpDecision::Portable,
          TileFfpFallbackReason::UnsupportedState,
      };
    }
  }
  const auto& key = *shader.pixelShader.pixelKey;
  const auto& rs = state.hot->renderStates;
  // RS_ALPHA_REF holds a 0..255 D3D9 byte. Normalize so the precision
  // boundary check uses the same float space as the shader.
  const u32 alphaRefRaw = core::flatStateOr(rs, core::RS_ALPHA_REF, 0u);
  const float alphaRefNorm = static_cast<float>(alphaRefRaw & 0xffu) / 255.0f;
  // R-BACK-13.7: A2C with PS-emitted alpha-test cannot be replicated
  // tile-side. Pull the bit from the same render-state slot the
  // ShaderVariantKey uses so the selector stays consistent with the PSO
  // key.
  // dxmt9 doesn't have a dedicated A2C render state (D3DRS_ADAPTIVETESS_Y
  // is the historical encoding); for now drive A2C off `key.alphaToCoverage`
  // through ShaderVariantKey, which is false until the AdaptiveTess path
  // wires it. Always read it through the ShaderVariantKey contract once
  // populated; for now treat it as false unless the hot record sets it.
  const bool a2c = false;
  auto eligibility = ffp::classifyTileFfpEligibility(key, alphaRefNorm, a2c);
  switch (eligibility) {
    case ffp::TileFfpEligibility::Eligible:
      return TileFfpSelection{TileFfpDecision::Tile, TileFfpFallbackReason::None};
    case ffp::TileFfpEligibility::IneligiblePrecision:
      return TileFfpSelection{TileFfpDecision::Portable, TileFfpFallbackReason::Precision};
    case ffp::TileFfpEligibility::IneligibleUnsupportedState:
      return TileFfpSelection{TileFfpDecision::Portable, TileFfpFallbackReason::UnsupportedState};
  }
  return TileFfpSelection{TileFfpDecision::Portable, TileFfpFallbackReason::Precision};
}

ArgbufHybridDecision selectArgbufHybridForPass(core::FlatDrawStateView state,
                                                 bool argbufHybridEnabled) {
  // Tier-2 argbuf + Apple3 is cached as a single bool on the pool. When
  // the gate fails the pass commits to Stage 1 and never switches
  // (R-BACK-12.22). Texture-bound draws also stay on Stage 1 until the
  // Stage 2 texture/sampler readback lane is stable.
  if (!argbufHybridEnabled) {
    return ArgbufHybridDecision::Stage1;
  }
  // The Stage 2 texture/sampler argbuf path is kept behind a runtime
  // fallback until texture-bound readback evidence is stable. Uniform-only
  // draws still exercise the slot-30 cbuf path.
  if (state.hot) {
    for (const auto& texture : state.hot->textures) {
      if (texture) {
        return ArgbufHybridDecision::Stage1;
      }
    }
  }
  return ArgbufHybridDecision::Stage2;
}

ShaderVariantKey makeShaderVariantKey(core::FlatDrawStateView state,
                                       std::span<const u32> colorFormats,
                                       std::span<const BlendAttachmentKey> blendAttachments,
                                       u32 depthFormat,
                                       u32 stencilFormat) {
  const auto& hot = *state.hot;
  const auto& shader = state.shaderContext();
  const auto& vertexDecl = shader.vertexDecl;
  const auto& vertexShader = shader.vertexShader;
  const auto& pixelShader = shader.pixelShader;
  ShaderVariantKey key{};
  key.emitterVersion = kShaderEmitterVersion;
  key.sourceLayoutVersion = kShaderSourceLayoutVersion;
  key.debugEnvSchemaVersion = kShaderDebugEnvSchemaVersion;
  key.debugEnvKey = currentShaderSourceDebugEnvKey();
  const auto layout =
      vertexShader.kind == core::ShaderRef::Kind::FixedFunctionVertex
          ? ffp::decodeFixedFunctionVertexLayout(vertexDecl)
          : std::optional<ffp::FixedFunctionVertexLayout>{};
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
  for (std::size_t i = 0; i < core::kMaxTextureStages; ++i) {
    key.textureTypes[i] =
        core::flatStateOr(hot.textureStageStates[i], core::TSS_TEXTURE_TYPE,
                          static_cast<u32>(core::TextureType::TwoD));
  }
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

}  // namespace dxmt9::pipeline
