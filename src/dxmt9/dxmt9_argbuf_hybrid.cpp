#include "dxmt9_argbuf_hybrid.hpp"

#include "dxmt9_command_queue.hpp"
#include "dxmt9_draw_encoder.hpp"
#include "dxmt9_draw_state.hpp"
#include "dxmt9_resource_pool.hpp"
#include "dxmt9/core.hpp"

#include <cstring>

namespace dxmt9::argbuf_hybrid {

namespace {

constexpr u32 kArgumentAccessReadOnly = 0u;

// 16 B keeps the constant-block alignment large enough for the Tier-2
// GPU-pointer slot and float4-leading host structs (VsConsts /
// PsConsts) without over-aligning the smaller FFP scalars.
constexpr u32 kConstantBlockAlignment = 16u;

// MTLTextureType2D — pinned at the value used by the existing
// fragment-stage texture binding loop in dxmt9_draw_encoder.mm. The
// host-side argbuf populates this slot via
// `MTLArgumentEncoder::setTexture(texture, idx)` regardless, so this
// is metadata for the encoder, not a runtime constraint on the bound
// texture.
constexpr u32 kTextureType2D = 2u;
constexpr u32 kTextureTypeCube = 5u;
constexpr u32 kTextureType3D = 7u;

void appendTextureDescriptors(ArgumentDescriptors& descriptors,
                              std::size_t& cursor,
                              u32 baseIndex,
                              u32 textureType) {
  auto& d = descriptors.entries[cursor++];
  d.argumentType = WMTArgumentTypeTexture;
  d.index = baseIndex;
  d.arrayLength = shaders::kArgbufHybridTextureSlotCount;
  d.access = kArgumentAccessReadOnly;
  d.textureType = textureType;
  d.constantBlockAlignment = 0;
}

}  // namespace

ArgumentDescriptors buildArgumentDescriptors() {
  ArgumentDescriptors descriptors{};
  std::size_t cursor = 0;

  // Constant-buffer entries — VsConsts, FfpVsConsts, PsConsts, FfpPsConsts.
  // Each lives at consecutive [[id(N)]] indices 0..3.
  for (u32 i = 0; i < shaders::kArgbufHybridConstantBufferCount; ++i) {
    auto& d = descriptors.entries[cursor++];
    d.argumentType = WMTArgumentTypeBuffer;
    d.index = i;
    d.arrayLength = 0;
    d.access = kArgumentAccessReadOnly;
    d.textureType = 0;
    d.constantBlockAlignment = kConstantBlockAlignment;
  }

  appendTextureDescriptors(descriptors, cursor, shaders::kArgbufHybridTexture2DBase, kTextureType2D);
  appendTextureDescriptors(descriptors, cursor, shaders::kArgbufHybridTextureCubeBase, kTextureTypeCube);
  appendTextureDescriptors(descriptors, cursor, shaders::kArgbufHybridTexture3DBase, kTextureType3D);

  // One sampler array descriptor after the typed texture array ranges.
  auto& sampler = descriptors.entries[cursor++];
  sampler.argumentType = WMTArgumentTypeSampler;
  sampler.index = shaders::kArgbufHybridSamplerBase;
  sampler.arrayLength = shaders::kArgbufHybridSamplerSlotCount;
  sampler.access = kArgumentAccessReadOnly;
  sampler.textureType = 0;
  sampler.constantBlockAlignment = 0;

  return descriptors;
}

bool computeCapabilityGate(WMTArgumentBuffersTier argumentBuffersTier, bool apple3) {
  return argumentBuffersTier >= WMTArgumentBuffersTier2 && apple3;
}

void ArgbufEncoderResource::init(WMT::Device device) {
  if (initialized_) {
    return;
  }
  // The descriptor set must outlive the newArgumentEncoder call (the WMT
  // bridge reads the entries.data() span synchronously) — keep the local
  // alive until the call returns.
  const auto descriptors = buildArgumentDescriptors();
  encoder_ = device.newArgumentEncoder(
      descriptors.entries.data(),
      static_cast<u32>(descriptors.count()));
  if (!encoder_) {
    // Sentinel-null device (test fixture) or Metal failure. Stay
    // uninitialized so `openArgbuf` short-circuits to an empty handle
    // and the caller falls back to Stage 1 binding.
    return;
  }
  encodedLength_ = encoder_.encodedLength();
  alignment_ = std::max<u64>(16u, encoder_.alignment());
  initialized_ = true;
}

void ArgbufEncoderResource::initForTest(u64 encodedLength, u64 alignment) noexcept {
  encodedLength_ = encodedLength;
  alignment_ = std::max<u64>(16u, alignment);
  initialized_ = true;
}

namespace {

// Bytes the encoder rewrites for fixed-function categories. Shader constant
// categories use uniform::ShaderConstantUploadPlan so known fixed ranges may
// upload only the MSL-visible struct prefix they can read.
constexpr u64 kFfpVsBytes    = sizeof(state::FfpVsConsts);
constexpr u64 kFfpPsBytes    = sizeof(state::FfpPsConsts);

// argbuf [[id(N)]] indices for the four cbuf entries.
constexpr u32 kVsConstsArgbufIdx = 0u;
constexpr u32 kFfpVsArgbufIdx    = 1u;
constexpr u32 kPsConstsArgbufIdx = 2u;
constexpr u32 kFfpPsArgbufIdx    = 3u;

// argbuf [[id(N)]] bases for typed textures / samplers; the encoder writes
// stages 0..7 at consecutive ids within each typed range.
constexpr u32 kTexture2DArgbufBase = shaders::kArgbufHybridTexture2DBase;
constexpr u32 kTextureCubeArgbufBase = shaders::kArgbufHybridTextureCubeBase;
constexpr u32 kTexture3DArgbufBase = shaders::kArgbufHybridTexture3DBase;
constexpr u32 kSamplerArgbufBase = shaders::kArgbufHybridSamplerBase;

void recordedSetArgumentBuffer(ArgbufEncoderResource& encoderResource,
                               WMT::Buffer buffer,
                               u64 offset,
                               const ArgbufRecorder* recorder) {
  if (recorder && recorder->setArgumentBuffer) {
    recorder->setArgumentBuffer(recorder->userdata, buffer, offset);
  }
  if (recorder && recorder->suppressMetalCalls) {
    return;
  }
  encoderResource.argumentEncoder().setArgumentBuffer(buffer, offset);
}

void recordedSetBuffer(ArgbufEncoderResource& encoderResource,
                       WMT::Buffer buffer,
                       u64 offset,
                       u32 index,
                       const ArgbufRecorder* recorder) {
  if (recorder && recorder->setBuffer) {
    recorder->setBuffer(recorder->userdata, buffer, offset, index);
  }
  if (recorder && recorder->suppressMetalCalls) {
    return;
  }
  encoderResource.argumentEncoder().setBuffer(buffer, offset, index);
}

void recordedSetTexture(ArgbufEncoderResource& encoderResource,
                        WMT::Texture texture,
                        u32 index,
                        const ArgbufRecorder* recorder) {
  if (recorder && recorder->setTexture) {
    recorder->setTexture(recorder->userdata, texture, index);
  }
  if (recorder && recorder->suppressMetalCalls) {
    return;
  }
  encoderResource.argumentEncoder().setTexture(texture, index);
}

void recordedSetSamplerState(ArgbufEncoderResource& encoderResource,
                             WMT::SamplerState sampler,
                             u32 index,
                             const ArgbufRecorder* recorder) {
  if (recorder && recorder->setSamplerState) {
    recorder->setSamplerState(recorder->userdata, sampler, index);
  }
  if (recorder && recorder->suppressMetalCalls) {
    return;
  }
  encoderResource.argumentEncoder().setSamplerState(sampler, index);
}

}  // namespace

u64 dirtyBytesEstimate(const uniform::DirtyState& dirty) noexcept {
  uniform::ShaderConstantUsageBounds unknown{};
  return dirtyBytesEstimate(dirty, unknown, unknown);
}

u64 dirtyBytesEstimate(const uniform::DirtyState& dirty,
                       uniform::ShaderConstantUsageBounds vsUsage,
                       uniform::ShaderConstantUsageBounds psUsage) noexcept {
  u64 bytes = 0;
  if (uniform::anyDirty(dirty, uniform::kVsAny)) {
    bytes += uniform::vsConstantUploadBytes(
        uniform::makeVsConstantUploadPlan(dirty, vsUsage));
  }
  if (uniform::anyDirty(dirty, uniform::kPsAny)) {
    bytes += uniform::psConstantUploadBytes(
        uniform::makePsConstantUploadPlan(dirty, psUsage));
  }
  if (uniform::anyDirty(dirty, uniform::kFfpVsAny)) bytes += kFfpVsBytes;
  if (uniform::anyDirty(dirty, uniform::kFfpPsAny)) bytes += kFfpPsBytes;
  return bytes;
}

PopulatedArgbuf openArgbuf(CommandQueue& queue,
                            ArgbufEncoderResource& encoderResource,
                            std::uint64_t seqId,
                            const ArgbufRecorder* recorder) {
  if (!encoderResource.initialized() || encoderResource.encodedLength() == 0) {
    return {};
  }
  const auto length = encoderResource.encodedLength();
  const auto alignment = encoderResource.alignment();
  auto reservation = queue.reserveTransientBuffer(
      static_cast<std::size_t>(length),
      static_cast<std::size_t>(alignment),
      seqId);
  if (!reservation) {
    return {};
  }
  // Anchor the encoder onto the reservation. `setArgumentBuffer` is the
  // prerequisite for any subsequent setBuffer/setTexture/setSamplerState
  // call — none of the populate* helpers below should run before this.
  recordedSetArgumentBuffer(encoderResource, reservation.slice.buffer,
                             reservation.slice.offset, recorder);
  PopulatedArgbuf populated{};
  populated.storage = reservation.slice.buffer;
  populated.offset = reservation.slice.offset;
  populated.length = length;
  return populated;
}

namespace {

// Single-argbuf cbuf upload helper. Reuses `uploadTransientBuffer` so
// the per-cbuf backing storage retains against the same seqId as the
// argbuf itself; the argbuf entry then points at the fresh transient
// slab via `setBuffer`. Returns the bytes written into the transient
// ring, or 0 on reservation failure (caller treats as "skipped").
template <typename HostStruct>
u64 uploadAndPointEntry(CommandQueue& queue,
                         ArgbufEncoderResource& encoderResource,
                         const HostStruct& host,
                         std::size_t byteCount,
                         u32 argbufIdx,
                         u64 seqId,
                         const ArgbufRecorder* recorder) {
  if (byteCount == 0) return 0;
  auto slice = queue.uploadTransientBuffer(
      std::span<const std::byte>(
          reinterpret_cast<const std::byte*>(&host), byteCount),
      alignof(HostStruct), seqId);
  if (!slice) return 0;
  recordedSetBuffer(encoderResource, slice.buffer, slice.offset, argbufIdx,
                    recorder);
  return byteCount;
}

template <typename HostStruct>
u64 uploadAndPointEntry(CommandQueue& queue,
                         ArgbufEncoderResource& encoderResource,
                         const HostStruct& host,
                         u32 argbufIdx,
                         u64 seqId,
                         const ArgbufRecorder* recorder) {
  return uploadAndPointEntry(
      queue, encoderResource, host, sizeof(HostStruct), argbufIdx, seqId, recorder);
}

}  // namespace

u64 populateConstantBuffers(CommandQueue& queue,
                             ArgbufEncoderResource& encoderResource,
                             core::FlatDrawStateView state,
                             std::uint64_t seqId,
                             const ArgbufRecorder* recorder) {
  if (!encoderResource.initialized()) return 0;
  u64 bytes = 0;
  // VsConsts / FfpVsConsts / PsConsts / FfpPsConsts mirror the Stage 1
  // bind sequence in encodeDraw — same builders, same alignment, same
  // upload path; the only delta is the argbuf [[id(N)]] target instead
  // of the slot 0 / 3 vert/frag slots.
  const auto vs = state::buildVsConsts(state);
  bytes += uploadAndPointEntry(queue, encoderResource, vs,
                                kVsConstsArgbufIdx, seqId, recorder);
  const auto ffpVs = state::buildFfpVsConsts(state);
  bytes += uploadAndPointEntry(queue, encoderResource, ffpVs,
                                kFfpVsArgbufIdx, seqId, recorder);
  const auto ps = state::buildPsConsts(state);
  bytes += uploadAndPointEntry(queue, encoderResource, ps,
                                kPsConstsArgbufIdx, seqId, recorder);
  const auto ffpPs = state::buildFfpPsConsts(state);
  bytes += uploadAndPointEntry(queue, encoderResource, ffpPs,
                                kFfpPsArgbufIdx, seqId, recorder);
  return bytes;
}

void populateResourceBindings(WMT::Reference<WMT::Device> device,
                               resources::Pool& pool,
                               ArgbufEncoderResource& encoderResource,
                               core::FlatDrawStateView state,
                               const ArgbufRecorder* recorder) {
  if (!encoderResource.initialized() || !state.hot) return;
  const auto& hot = *state.hot;
  // Stages 0..7 — the argbuf descriptor table reserves exactly 8 slots
  // for textures and 8 for samplers (shaders::kArgbufHybridTextureSlotCount).
  // Stages without a bound texture leave the argbuf entry untouched;
  // shaders that don't sample those slots never deref a stale id.
  for (u32 stage = 0; stage < shaders::kArgbufHybridTextureSlotCount; ++stage) {
    const auto textureHandle = hot.textures[stage];
    if (!textureHandle) continue;
    if (auto* texture = pool.findTexture(textureHandle.value);
        texture && texture->texture) {
      const bool srgbTexture =
          core::flatStateOr(hot.samplerStates[stage], core::SAMP_SRGB_TEXTURE, 0u) != 0;
      u32 textureArgbufBase = kTexture2DArgbufBase;
      if (texture->desc.type == core::TextureType::Cube) {
        textureArgbufBase = kTextureCubeArgbufBase;
      } else if (texture->desc.type == core::TextureType::Volume) {
        textureArgbufBase = kTexture3DArgbufBase;
      }
      recordedSetTexture(encoderResource, resources::textureForShaderRead(*texture, srgbTexture),
                         textureArgbufBase + stage, recorder);
    }
    if (recorder && recorder->suppressMetalCalls) {
      if (recorder->samplerState) {
        recordedSetSamplerState(encoderResource, recorder->samplerState,
                                kSamplerArgbufBase + stage, recorder);
      }
    } else {
      auto sampler = encoders::makeSampler(device, hot.samplerStates[stage],
                                           static_cast<float>(hot.textureLods[stage]));
      if (sampler) {
        recordedSetSamplerState(encoderResource,
                                WMT::SamplerState{sampler.handle},
                                kSamplerArgbufBase + stage, recorder);
      }
    }
  }
}

u64 updateDirtyArgbufRegions(CommandQueue& queue,
                              ArgbufEncoderResource& encoderResource,
                              core::FlatDrawStateView state,
                              const uniform::DirtyState& dirty,
                              std::uint64_t seqId,
                              const ArgbufRecorder* recorder) {
  uniform::ShaderConstantUsageBounds unknown{};
  return updateDirtyArgbufRegions(
      queue, encoderResource, state, dirty, unknown, unknown, seqId, recorder);
}

u64 updateDirtyArgbufRegions(CommandQueue& queue,
                              ArgbufEncoderResource& encoderResource,
                              core::FlatDrawStateView state,
                              const uniform::DirtyState& dirty,
                              uniform::ShaderConstantUsageBounds vsUsage,
                              uniform::ShaderConstantUsageBounds psUsage,
                              std::uint64_t seqId,
                              const ArgbufRecorder* recorder) {
  if (!encoderResource.initialized()) return 0;
  u64 bytes = 0;
  if (uniform::anyDirty(dirty, uniform::kVsAny)) {
    const auto vs = state::buildVsConsts(state);
    const auto plan = uniform::makeVsConstantUploadPlan(dirty, vsUsage);
    bytes += uploadAndPointEntry(queue, encoderResource, vs,
                                  static_cast<std::size_t>(
                                      uniform::vsConstantUploadBytes(plan)),
                                  kVsConstsArgbufIdx, seqId, recorder);
  }
  if (uniform::anyDirty(dirty, uniform::kFfpVsAny)) {
    const auto ffpVs = state::buildFfpVsConsts(state);
    bytes += uploadAndPointEntry(queue, encoderResource, ffpVs,
                                  kFfpVsArgbufIdx, seqId, recorder);
  }
  if (uniform::anyDirty(dirty, uniform::kPsAny)) {
    const auto ps = state::buildPsConsts(state);
    const auto plan = uniform::makePsConstantUploadPlan(dirty, psUsage);
    bytes += uploadAndPointEntry(queue, encoderResource, ps,
                                  static_cast<std::size_t>(
                                      uniform::psConstantUploadBytes(plan)),
                                  kPsConstsArgbufIdx, seqId, recorder);
  }
  if (uniform::anyDirty(dirty, uniform::kFfpPsAny)) {
    const auto ffpPs = state::buildFfpPsConsts(state);
    bytes += uploadAndPointEntry(queue, encoderResource, ffpPs,
                                  kFfpPsArgbufIdx, seqId, recorder);
  }
  return bytes;
}

void pointFfpVsAtSlice(ArgbufEncoderResource& encoderResource,
                       WMT::Buffer buffer,
                       u64 offset,
                       const ArgbufRecorder* recorder) {
  if (!encoderResource.initialized() || !buffer) return;
  recordedSetBuffer(encoderResource, buffer, offset, kFfpVsArgbufIdx, recorder);
}

}  // namespace dxmt9::argbuf_hybrid
