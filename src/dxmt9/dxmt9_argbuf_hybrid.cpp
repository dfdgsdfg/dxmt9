#include "dxmt9_argbuf_hybrid.hpp"

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

  // 8 texture descriptors at [[id(4)..id(11)]].
  for (u32 i = 0; i < shaders::kArgbufHybridTextureSlotCount; ++i) {
    auto& d = descriptors.entries[cursor++];
    d.argumentType = WMTArgumentTypeTexture;
    d.index = shaders::kArgbufHybridConstantBufferCount + i;
    d.arrayLength = 0;
    d.access = kArgumentAccessReadOnly;
    d.textureType = kTextureType2D;
    d.constantBlockAlignment = 0;
  }

  // 8 sampler descriptors at [[id(12)..id(19)]].
  for (u32 i = 0; i < shaders::kArgbufHybridSamplerSlotCount; ++i) {
    auto& d = descriptors.entries[cursor++];
    d.argumentType = WMTArgumentTypeSampler;
    d.index = shaders::kArgbufHybridConstantBufferCount +
              shaders::kArgbufHybridTextureSlotCount + i;
    d.arrayLength = 0;
    d.access = kArgumentAccessReadOnly;
    d.textureType = 0;
    d.constantBlockAlignment = 0;
  }

  return descriptors;
}

bool computeCapabilityGate(WMTArgumentBuffersTier argumentBuffersTier, bool apple3) {
  return argumentBuffersTier >= WMTArgumentBuffersTier2 && apple3;
}

}  // namespace dxmt9::argbuf_hybrid
