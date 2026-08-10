#pragma once

// R-BACK-12.22..12.26 — Stage 2 argument-buffer hybrid runtime adopter.
//
// Pure value-transform helpers shared between the encoder hot path and
// the native test fixture. The actual per-encoder bind / sub-region
// write logic lives inline in dxmt9_draw_encoder_draw.mm; this header
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
#include <span>

namespace dxmt9 {
class CommandQueue;
namespace core { struct FlatDrawStateView; }
}  // namespace dxmt9

namespace dxmt9::argbuf_hybrid {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

inline constexpr u32 kConstantBufferVsIndex = 0u;
inline constexpr u32 kConstantBufferFfpVsIndex = 1u;
inline constexpr u32 kConstantBufferPsIndex = 2u;
inline constexpr u32 kConstantBufferFfpPsIndex = 3u;

inline u64 hashConstantBufferBytes(const void* data, u64 bytes) noexcept {
  constexpr u64 kFnvOffset = 1469598103934665603ull;
  constexpr u64 kFnvPrime = 1099511628211ull;
  const auto* ptr = static_cast<const unsigned char*>(data);
  u64 hash = kFnvOffset ^ bytes;
  for (u64 i = 0; i < bytes; ++i) {
    hash ^= static_cast<u64>(ptr[i]);
    hash *= kFnvPrime;
  }
  return hash;
}

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

// R-BACK-12.22..12.26 (resource-array sub-mode) — extended descriptor table:
// the four constant-buffer pointers at id 0..3 PLUS a texture array
// (kArgbufResourceArrayStageCount entries starting at
// kArgbufResourceArrayTextureBaseId) and a sampler array (same count,
// starting at kArgbufResourceArraySamplerBaseId). The texture entries are
// tagged WMTTextureType2D — the dominant case; cube/volume sampling
// reinterprets the same slot in MSL since the gpuResourceID written by
// MTLArgumentEncoder_setTexture is type-agnostic on the wire. The returned
// struct is consumed by MTLDevice::newArgumentEncoder exactly like
// buildArgumentDescriptors; only the queue's resource-array encoder uses
// it (a second MTLArgumentEncoder, built only when the resource-array lane
// is enabled — the constants-only encoder is left untouched so the default
// path stays byte-identical).
struct ResourceArrayArgumentDescriptors {
  std::array<WMTArgumentDescriptor, shaders::kArgbufResourceArrayDescriptorCount>
      entries{};
  std::size_t count() const noexcept {
    return shaders::kArgbufResourceArrayDescriptorCount;
  }
};
ResourceArrayArgumentDescriptors buildResourceArrayArgumentDescriptors();

// Capability-gate sense check for unit tests. The actual gate result is
// cached on `resources::Pool::argbufHybridEnabled_` at queue init; this
// helper exposes the predicate shape so tests can assert it without
// instantiating a real WMT::Device. Returns true iff both inputs hold,
// matching spec.md §11.1.
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
  // R-BACK-12.22..12.26 (resource-array sub-mode) — build the encoder from
  // the extended 20-entry descriptor table (4 cbuf + 8 texture + 8 sampler)
  // instead of the constants-only 4-entry table. A queue owning the
  // resource-array lane builds a SECOND ArgbufEncoderResource with this so
  // the constants-only encoder (and the byte-identical default path) is
  // never disturbed.
  void initResourceArray(WMT::Device device);
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

// Captured backing slice for one argbuf constant-buffer entry. The Metal
// argument buffer table is per draw in the resource-array lane, but the
// cbuf backing slabs can be reused while the uniform payload is unchanged.
struct ConstantBufferBinding {
  WMT::Buffer buffer{};
  u64 offset = 0;
  u64 bytes = 0;
  u64 contentHash = 0;
  u64 identityHash = 0;

  explicit operator bool() const noexcept {
    return static_cast<bool>(buffer) && bytes != 0;
  }

  bool contentMatches(u64 hash, u64 byteCount) const noexcept {
    return *this && hash != 0 && contentHash != 0 &&
           bytes == byteCount && contentHash == hash;
  }

  bool identityMatches(u64 hash, u64 byteCount) const noexcept {
    return *this && bytes == byteCount && identityHash == hash;
  }
};

struct ConstantBufferBindings {
  std::array<ConstantBufferBinding, shaders::kArgbufHybridConstantBufferCount>
      entries{};

  bool complete() const noexcept {
    for (const auto& entry : entries) {
      if (!entry) return false;
    }
    return true;
  }
};

struct ConstantBufferUploadObserver {
  void* userdata = nullptr;
  void (*upload)(void* userdata,
                 u32 argbufIndex,
                 const void* data,
                 u64 bytes,
                 u64 hostStructBytes) = nullptr;
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
// Caller binds the returned storage at slot 30 with
// `setVertexBuffer(populated.storage, populated.offset, slot=30)` and
// `setFragmentBuffer(...)` on the render encoder.
PopulatedArgbuf openArgbuf(CommandQueue& queue,
                            ArgbufEncoderResource& encoderResource,
                            std::uint64_t seqId,
                            const ArgbufRecorder* recorder = nullptr);
PopulatedArgbuf openArgbufWithCompletedSeqId(CommandQueue& queue,
                                             ArgbufEncoderResource& encoderResource,
                                             std::uint64_t seqId,
                                             std::uint64_t completedSeqId,
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
                             WMT::RenderCommandEncoder residencyEncoder = {},
                             ConstantBufferBindings* writtenBindings = nullptr,
                             const ConstantBufferUploadObserver* uploadObserver = nullptr);

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
                              WMT::RenderCommandEncoder residencyEncoder = {},
                              ConstantBufferBindings* writtenBindings = nullptr,
                              const ConstantBufferUploadObserver* uploadObserver = nullptr);
u64 updateDirtyArgbufRegions(CommandQueue& queue,
                              ArgbufEncoderResource& encoderResource,
                              core::FlatDrawStateView state,
                              const uniform::DirtyState& dirty,
                              uniform::ShaderConstantUsageBounds vsUsage,
                              uniform::ShaderConstantUsageBounds psUsage,
                              std::uint64_t seqId,
                              const ArgbufRecorder* recorder = nullptr,
                              WMT::RenderCommandEncoder residencyEncoder = {},
                              ConstantBufferBindings* writtenBindings = nullptr,
                              const ConstantBufferUploadObserver* uploadObserver = nullptr);

// R-BACK-12.22..12.26 (resource-array sub-mode) — one resolved
// texture/sampler binding the encoder hands to the resource-array
// populator. `texture` / `sampler` are the SAME WMT handles the Stage 1
// direct lane would bind (resources::textureForShaderRead +
// makeSampler). A zero-handle texture or sampler leaves that argbuf slot
// unwritten (the shader must not sample an unbound stage). `stage` is the
// D3D9 fragment sampler stage (0..kArgbufResourceArrayStageCount-1) and
// indexes both the texture and sampler arrays in ArgbufLayout.
struct ResourceArrayBinding {
  u32 stage = 0;
  WMT::Texture texture{};
  WMT::SamplerState sampler{};
};

// Recorder hook for the resource-array populator. Tests set
// suppressMetalCalls=true to capture the exact (stage, texture-handle,
// sampler-handle, argbuf-id, residency-usage) tuples without a Metal
// device. Production passes nullptr.
struct ResourceArrayRecorder {
  void* userdata = nullptr;
  bool suppressMetalCalls = false;

  void (*setTexture)(void* userdata, u64 textureHandle, u32 argbufId) = nullptr;
  void (*setSampler)(void* userdata, u64 samplerHandle, u32 argbufId) = nullptr;
  // Residency: usage is the WMTResourceUsage bitmask, stages the
  // WMTRenderStages bitmask passed to useResource.
  void (*useTexture)(void* userdata, u64 textureHandle, u32 usage, u32 stages) =
      nullptr;
};

// R-BACK-12.22..12.26 (resource-array sub-mode, fault candidates 1/3/4/5)
// — write the resolved per-stage textures + samplers into the
// resource-array argbuf via MTLArgumentEncoder_setTexture /
// _setSamplerState (fault candidate 3: gpuResourceID ABI) at the [[id]]
// positions pinned by kArgbufResourceArray*BaseId, and — critically —
// issue `useResource(texture, Read|Sample, Vertex|Fragment)` on the
// render encoder for every argbuf-pointed texture (fault candidate 4:
// argbuf-referenced resources are NOT made resident by the slot-30 bind
// alone; the GPU faults on access without an explicit useResource). The
// argument encoder must already be anchored on the argbuf storage
// (openArgbuf / setArgumentBuffer) before this runs.
//
// Samplers do not require useResource (they carry no backing allocation);
// their lifetime is held by the caller retaining the WMT::SamplerState
// reference against the argbuf's seqId (fault candidate 2). This function
// does not retain — the caller (encoder) owns retention through the same
// transient-ring seqId waterline as the constant buffers.
//
// Returns the number of texture slots made resident (for perf accounting).
u32 populateResourceBindings(ArgbufEncoderResource& encoderResource,
                             std::span<const ResourceArrayBinding> bindings,
                             const ResourceArrayRecorder* recorder = nullptr,
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

void pointConstantBufferBinding(ArgbufEncoderResource& encoderResource,
                                u32 argbufIndex,
                                ConstantBufferBinding binding,
                                const ArgbufRecorder* recorder = nullptr,
                                WMT::RenderCommandEncoder residencyEncoder = {});

}  // namespace dxmt9::argbuf_hybrid
