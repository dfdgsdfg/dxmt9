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
#include "dxmt9_uniform_dirty.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace dxmt9 {
class CommandQueue;
namespace core { struct FlatDrawStateView; }
}  // namespace dxmt9

namespace dxmt9::argbuf_hybrid {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// R-BACK-12.23 — argbuf descriptor layout. Mirrors the MSL ArgbufLayout
// struct emitted by `shaders::makeShaderPreludeArgbufHybrid`. The
// encoder calls `buildArgumentDescriptors` once at queue init and feeds
// the resulting array into `MTLDevice::newArgumentEncoder`.
//
// Indices are pinned to the MSL [[id(N)]] attributes so descriptor
// position is stable across MSL versions:
//
//   id 0..3   : VsConsts / FfpVsConsts / PsConsts / FfpPsConsts pointers
//
// Texture and sampler resources stay on the direct render-encoder
// `[[texture(N)]]` / `[[sampler(N)]]` binding lane (the validated
// Stage 1 path); the argument buffer carries constants only.
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
// the Tier-2 GPU-pointer slot aligned).
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
  u64 totalSize = 0;
};

// R-BACK-12.22 — per-CommandQueue-owned MTLArgumentEncoder. Built once
// at queue construction from the descriptor table returned by
// `buildArgumentDescriptors()`. The encoder's `encodedLength()` and
// `alignment()` are queried on init and cached so the per-encoder
// `openArgbuf` reservation is a single struct-field read on the hot
// path. Lifetime: cleared automatically on `WMT::Reference` dtor when
// the queue tears down.
//
// `init(WMT::Device)` is the production entry; `initForTest` lets a
// CPU-only spec stand the resource up with a synthetic encodedLength /
// alignment without standing up Metal — used by the populator spec to
// assert the `openArgbuf().length` reservation matches the encoder's
// reported size.
class ArgbufEncoderResource {
 public:
  void init(WMT::Device device);
  void initForTest(u64 encodedLength, u64 alignment) noexcept;

  bool initialized() const noexcept { return initialized_; }
  u64 encodedLength() const noexcept { return encodedLength_; }
  u64 alignment() const noexcept { return alignment_; }
  WMT::ArgumentEncoder& argumentEncoder() noexcept { return encoder_; }

 private:
  WMT::Reference<WMT::ArgumentEncoder> encoder_{};
  u64 encodedLength_ = 0;
  // Default to 16 B — matches the constant-block alignment of the four
  // [[id(0..3)]] cbuf entries; production `init` overwrites with the
  // value returned by `MTLArgumentEncoder::alignment()`.
  u64 alignment_ = 16;
  bool initialized_ = false;
};

// R-BACK-12.24 — per-encoder argbuf storage handle. `storage` is the
// transient ring buffer the queue handed out; `offset` is where the
// encoder bound the argument buffer; `length` is the number of bytes
// reserved for this argbuf (encoderResource.encodedLength()). Empty
// instance (length == 0) means open failed (transient ring exhausted).
struct PopulatedArgbuf {
  WMT::Buffer storage{};
  u64 offset = 0;
  u64 length = 0;

  explicit operator bool() const noexcept {
    return static_cast<bool>(storage) && length != 0;
  }
};

// Test/diagnostic recorder for the Stage 2 argument-buffer populator.
// Production passes nullptr. Tests may set suppressMetalCalls=true so
// the exact MTLArgumentEncoder write indices and ordering can be
// asserted without a live Metal device. Only the constant-buffer
// pointer writes (setArgumentBuffer / setBuffer) are recorded — texture
// and sampler bindings travel on the direct render-encoder lane and
// are observed through other harnesses.
struct ArgbufRecorder {
  void* userdata = nullptr;
  bool suppressMetalCalls = false;

  void (*setArgumentBuffer)(void* userdata,
                            WMT::Buffer buffer,
                            u64 offset) = nullptr;
  void (*setBuffer)(void* userdata,
                    WMT::Buffer buffer,
                    u64 offset,
                    u32 index) = nullptr;
};

// Pure value-transform: bytes the encoder would re-write for the given
// dirty mask. Sums the four per-frequency host-struct sizes for the
// matching kVsAny / kPsAny / kFfpVsAny / kFfpPsAny categories. Returns
// 0 when no relevant bit is set (steady-state).
//
// Used by:
//   - the encoder's mid-pass argbuf rewrite to feed
//     `perf::countArgbufHybridBytes`
//   - the populator spec to verify "no dirty bits ⇒ 0 bytes".
u64 dirtyBytesEstimate(const uniform::DirtyState& dirty) noexcept;
u64 dirtyBytesEstimate(const uniform::DirtyState& dirty,
                       uniform::ShaderConstantUsageBounds vsUsage,
                       uniform::ShaderConstantUsageBounds psUsage) noexcept;

// R-BACK-12.24 — open a per-encoder argbuf. Reserves
// `encoderResource.encodedLength()` bytes from the queue's transient
// ring (alignment from the encoder), points the MTLArgumentEncoder at
// the reservation via `setArgumentBuffer`, and returns the
// (storage, offset, length) tuple the caller binds at slot 30. Returns
// an empty `PopulatedArgbuf` if the encoder resource is uninitialized
// or the transient reservation fails.
//
// Caller binds the constant buffers / textures / samplers via the
// `bindResources` helper below, then issues
// `setVertexBuffer(populated.storage, populated.offset, slot=30)` and
// `setFragmentBuffer(...)` on the render encoder.
PopulatedArgbuf openArgbuf(CommandQueue& queue,
                            ArgbufEncoderResource& encoderResource,
                            std::uint64_t seqId,
                            const ArgbufRecorder* recorder = nullptr);

// R-BACK-12.24 — populate the four constant-buffer entries in the
// argbuf from per-frequency uniform host-structs. The call writes
// VsConsts, PsConsts, FfpVsConsts, FfpPsConsts into the transient ring
// (so the GPU has a stable backing buffer for each pointer) and points
// the encoder's [[id(0..3)]] slots at them. Returns the total bytes
// written into the transient ring (i.e., the four struct sizes).
//
// Texture and sampler resources are bound directly on the render
// encoder by the encoder hot path — they never travel through this
// argument buffer.
//
// Production-only — invokes Metal calls on `encoderResource`. Tests
// drive `dirtyBytesEstimate` directly.
u64 populateConstantBuffers(CommandQueue& queue,
                             ArgbufEncoderResource& encoderResource,
                             core::FlatDrawStateView state,
                             std::uint64_t seqId,
                             const ArgbufRecorder* recorder = nullptr,
                             WMT::RenderCommandEncoder residencyEncoder = {});

// R-BACK-12.24 — mid-pass dirty rewrite. Re-uploads the per-frequency
// host structs corresponding to the dirty bits and re-points the
// matching argbuf entries at the fresh transient slabs. Returns the
// total bytes uploaded (matches `dirtyBytesEstimate(dirty)` on the
// happy path). Returns 0 when no relevant bit is set.
//
// Called from the encoder's per-draw path between draws on a single
// render encoder. Texture and sampler resources travel on the direct
// render-encoder lane and are never touched here.
u64 updateDirtyArgbufRegions(CommandQueue& queue,
                              ArgbufEncoderResource& encoderResource,
                              core::FlatDrawStateView state,
                              const uniform::DirtyState& dirty,
                              std::uint64_t seqId,
                              const ArgbufRecorder* recorder = nullptr,
                              WMT::RenderCommandEncoder residencyEncoder = {});
u64 updateDirtyArgbufRegions(CommandQueue& queue,
                              ArgbufEncoderResource& encoderResource,
                              core::FlatDrawStateView state,
                              const uniform::DirtyState& dirty,
                              uniform::ShaderConstantUsageBounds vsUsage,
                              uniform::ShaderConstantUsageBounds psUsage,
                              std::uint64_t seqId,
                              const ArgbufRecorder* recorder = nullptr,
                              WMT::RenderCommandEncoder residencyEncoder = {});

// R-BACK-12.24 — point the Stage 2 FfpVsConsts entry at an already
// uploaded host slice. Used by encodeDraw after the FFP preTransformed
// viewport override mutates the lazily-built host block; reusing the same
// slice keeps Stage 1 slot 3 and Stage 2 argbuf id(1) coherent.
void pointFfpVsAtSlice(ArgbufEncoderResource& encoderResource,
                       WMT::Buffer buffer,
                       u64 offset,
                       const ArgbufRecorder* recorder = nullptr,
                       WMT::RenderCommandEncoder residencyEncoder = {});

}  // namespace dxmt9::argbuf_hybrid
