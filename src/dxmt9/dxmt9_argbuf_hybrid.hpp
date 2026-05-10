#pragma once

// R-BACK-12.22..12.26 — Stage 2 argument-buffer hybrid runtime adopter.
//
// Pure value-transform helpers shared between the encoder hot path and
// the native test fixture. The actual per-encoder bind / sub-region
// write logic lives inline in dxmt9_draw_encoder.mm; this header
// owns the bits a CPU-only test can exercise without a Metal device:
//
//   - The argbuf MTLArgumentDescriptor layout build (R-BACK-12.23).
//   - The host-side per-region offset map (used when the encoder
//     writes a dirty sub-region into the argbuf storage; R-BACK-12.24).
//   - The capability-gate predicate sense check (R-BACK-12.22).
//
// Keeping these in their own translation unit keeps the encoder TU
// light and lets the native test link against this module without
// dragging the encoder or any winemetal Metal calls.

#include "../winemetal/Metal.hpp"
#include "dxmt9_shader_sources.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace dxmt9::argbuf_hybrid {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// R-BACK-12.23 — argbuf descriptor layout. Mirrors the MSL ArgbufLayout
// struct emitted by `shaders::makeShaderPreludeArgbufHybrid`. The
// encoder calls `buildArgumentDescriptors` once at queue init (or per
// encoder open if the descriptor set is encoder-local) and feeds the
// resulting array into `MTLDevice::newArgumentEncoder`.
//
// Indices are pinned to the MSL [[id(N)]] attributes so descriptor
// position is stable across MSL versions:
//
//   id 0..3   : VsConsts / FfpVsConsts / PsConsts / FfpPsConsts pointers
//   id 4..11  : 8 textures
//   id 12..19 : 8 samplers
struct ArgumentDescriptors {
  std::array<WMTArgumentDescriptor, shaders::kArgbufHybridDescriptorCount> entries{};

  // Total number of valid entries — equals the static descriptor count.
  std::size_t count() const noexcept {
    return shaders::kArgbufHybridDescriptorCount;
  }
};

// Build the descriptor table for the Stage 2 argument buffer. The
// returned struct is consumed by `MTLDevice::newArgumentEncoder` (which
// reads count + entries.data()); callers do not retain it long-term.
//
// Constant-buffer entries are tagged WMTArgumentTypeBuffer and use 16 B
// constant-block alignment (the smallest alignment compatible with the
// Stage 1 host structs — VsConsts/PsConsts have float4 leading members,
// FfpVsConsts/FfpPsConsts also begin on 4-byte boundaries; 16 B keeps
// the Tier-2 GPU-pointer slot aligned). Texture and sampler entries
// carry no alignment.
ArgumentDescriptors buildArgumentDescriptors();

// Capability-gate sense check for unit tests. The actual gate result is
// cached on `resources::Pool::argbufHybridEnabled_` at queue init; this
// helper exposes the predicate shape so tests can assert it without
// instantiating a real WMT::Device. Returns true iff both inputs hold,
// matching design.md §11.1.
//
// `argumentBuffersTier` is the WMTArgumentBuffersTier value (0=Tier1,
// 1=Tier2). `apple3` is the cached `supportsFamily(Apple3)` result.
bool computeCapabilityGate(WMTArgumentBuffersTier argumentBuffersTier, bool apple3);

// R-BACK-12.24 — per-region byte offsets within the argbuf, derived
// from `MTLArgumentEncoder.encodedLength()` at runtime. The encoder
// computes the layout once per encoder open by querying the encoder
// for each member's offset (via setArgumentBuffer + encoder slot
// addresses) and caches the result in this struct. The dirty-mask
// sub-region writer reads the matching offset to compute the GPU
// destination address.
//
// All offsets are in bytes from the argbuf base. The "size" field is
// the total `encodedLength()` of the argbuf — used to size the
// reserveTransientBuffer reservation.
struct ArgbufRegionOffsets {
  u64 vsConstsOffset = 0;
  u64 ffpVsOffset = 0;
  u64 psConstsOffset = 0;
  u64 ffpPsOffset = 0;
  u64 textureSlot0Offset = 0;
  u64 samplerSlot0Offset = 0;
  u64 totalSize = 0;
};

}  // namespace dxmt9::argbuf_hybrid
