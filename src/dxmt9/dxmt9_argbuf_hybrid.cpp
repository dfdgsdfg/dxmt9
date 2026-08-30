#include "dxmt9_argbuf_hybrid.hpp"

#include "dxmt9_command_queue.hpp"
#include "dxmt9/copy_materialization_ledger.hpp"
#include "dxmt9_draw_state.hpp"
#include "dxmt9_perf_counters.hpp"
#include "dxmt9/core.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace dxmt9::argbuf_hybrid {

namespace {

constexpr u32 kArgumentAccessReadOnly = 0u;

// 16 B keeps the constant-block alignment large enough for the Tier-2
// GPU-pointer slot and float4-leading host structs (VsConsts /
// PsConsts) without over-aligning the smaller FFP scalars.
constexpr u32 kConstantBlockAlignment =
    static_cast<u32>(state::kConstantBufferOffsetAlignment);

class PerfScope {
 public:
  explicit PerfScope(void (*record)(std::uint64_t),
                     void (*recordSecondary)(std::uint64_t) = nullptr) noexcept
      : record_(record),
        recordSecondary_(recordSecondary),
        start_(std::chrono::steady_clock::now()) {}

  ~PerfScope() {
    if (!record_ && !recordSecondary_) return;
    const auto end = std::chrono::steady_clock::now();
    const auto elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_).count());
    if (record_) record_(elapsed);
    if (recordSecondary_) recordSecondary_(elapsed);
  }

  PerfScope(const PerfScope&) = delete;
  PerfScope& operator=(const PerfScope&) = delete;

 private:
  void (*record_)(std::uint64_t) = nullptr;
  void (*recordSecondary_)(std::uint64_t) = nullptr;
  std::chrono::steady_clock::time_point start_;
};

using PerfRecorder = void (*)(std::uint64_t);

}  // namespace

ArgumentDescriptors buildArgumentDescriptors() {
  ArgumentDescriptors descriptors{};

  // Constant-buffer entries — VsConsts, FfpVsConsts, PsConsts, FfpPsConsts.
  // Each lives at consecutive [[id(N)]] indices 0..3. Texture and sampler
  // resources are bound directly on the render encoder (the validated
  // Stage 1 lane); the argbuf never carries them.
  for (u32 i = 0; i < shaders::kArgbufHybridConstantBufferCount; ++i) {
    auto& d = descriptors.entries[i];
    d.argumentType = WMTArgumentTypeBuffer;
    d.index = i;
    d.arrayLength = 0;
    d.access = kArgumentAccessReadOnly;
    d.textureType = 0;
    d.constantBlockAlignment = kConstantBlockAlignment;
  }

  return descriptors;
}

ResourceArrayArgumentDescriptors buildResourceArrayArgumentDescriptors() {
  ResourceArrayArgumentDescriptors descriptors{};
  // ids 0..3 — the same four constant-buffer pointers as the constants-only
  // table. Keeping them identical means a resource-array PSO's uniform reads
  // are byte-for-byte the constants-only path; only the texture/sampler
  // arrays are new.
  for (u32 i = 0; i < shaders::kArgbufHybridConstantBufferCount; ++i) {
    auto& d = descriptors.entries[i];
    d.argumentType = WMTArgumentTypeBuffer;
    d.index = i;
    d.arrayLength = 0;
    d.access = kArgumentAccessReadOnly;
    d.textureType = 0;
    d.constantBlockAlignment = kConstantBlockAlignment;
  }
  // texture array — kArgbufResourceArrayStageCount entries starting at id 4.
  // MTLArgumentDescriptor for an MSL `array<texture2d<float>, N>` is N
  // consecutive single-texture descriptors (arrayLength stays 0 per entry;
  // the array shape comes from the consecutive [[id]] run + the MSL decl).
  // textureType=WMTTextureType2D is the dominant case; cube/volume reuse the
  // same slot (gpuResourceID is type-agnostic on the wire — see header).
  std::size_t cursor = shaders::kArgbufHybridConstantBufferCount;
  for (u32 s = 0; s < shaders::kArgbufResourceArrayStageCount; ++s, ++cursor) {
    auto& d = descriptors.entries[cursor];
    d.argumentType = WMTArgumentTypeTexture;
    d.index = shaders::kArgbufResourceArrayTextureBaseId + s;
    d.arrayLength = 0;
    d.access = kArgumentAccessReadOnly;
    d.textureType = static_cast<u32>(WMTTextureType2D);
    d.constantBlockAlignment = 0;
  }
  // sampler array — kArgbufResourceArrayStageCount entries starting at id 12.
  for (u32 s = 0; s < shaders::kArgbufResourceArrayStageCount; ++s, ++cursor) {
    auto& d = descriptors.entries[cursor];
    d.argumentType = WMTArgumentTypeSampler;
    d.index = shaders::kArgbufResourceArraySamplerBaseId + s;
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

void ArgbufEncoderResource::initResourceArray(WMT::Device device) {
  if (initialized_) {
    return;
  }
  // Extended 20-entry table — keep the local alive across the
  // newArgumentEncoder call (the WMT bridge reads entries.data() span
  // synchronously).
  const auto descriptors = buildResourceArrayArgumentDescriptors();
  encoder_ = device.newArgumentEncoder(
      descriptors.entries.data(),
      static_cast<u32>(descriptors.count()));
  if (!encoder_) {
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

PerfRecorder cbufBuildRecorderForArgbufIndex(u32 argbufIdx) {
  switch (argbufIdx) {
    case kVsConstsArgbufIdx:
      return perf::countEncodeDrawArgbufCbufBuildVsCpuTime;
    case kPsConstsArgbufIdx:
      return perf::countEncodeDrawArgbufCbufBuildPsCpuTime;
    case kFfpVsArgbufIdx:
      return perf::countEncodeDrawArgbufCbufBuildFfpVsCpuTime;
    case kFfpPsArgbufIdx:
      return perf::countEncodeDrawArgbufCbufBuildFfpPsCpuTime;
    default:
      return nullptr;
  }
}

PerfRecorder cbufUploadRecorderForArgbufIndex(u32 argbufIdx) {
  switch (argbufIdx) {
    case kVsConstsArgbufIdx:
      return perf::countEncodeDrawArgbufCbufUploadVsCpuTime;
    case kPsConstsArgbufIdx:
      return perf::countEncodeDrawArgbufCbufUploadPsCpuTime;
    case kFfpVsArgbufIdx:
      return perf::countEncodeDrawArgbufCbufUploadFfpVsCpuTime;
    case kFfpPsArgbufIdx:
      return perf::countEncodeDrawArgbufCbufUploadFfpPsCpuTime;
    default:
      return nullptr;
  }
}

PerfRecorder cbufSetBufferRecorderForArgbufIndex(u32 argbufIdx) {
  switch (argbufIdx) {
    case kVsConstsArgbufIdx:
      return perf::countEncodeDrawArgbufCbufSetBufferVsCpuTime;
    case kPsConstsArgbufIdx:
      return perf::countEncodeDrawArgbufCbufSetBufferPsCpuTime;
    case kFfpVsArgbufIdx:
      return perf::countEncodeDrawArgbufCbufSetBufferFfpVsCpuTime;
    case kFfpPsArgbufIdx:
      return perf::countEncodeDrawArgbufCbufSetBufferFfpPsCpuTime;
    default:
      return nullptr;
  }
}

PerfRecorder cbufBindingHashRecorderForArgbufIndex(u32 argbufIdx) {
  switch (argbufIdx) {
    case kVsConstsArgbufIdx:
      return perf::countEncodeDrawArgbufCbufBindingHashVsCpuTime;
    case kPsConstsArgbufIdx:
      return perf::countEncodeDrawArgbufCbufBindingHashPsCpuTime;
    case kFfpVsArgbufIdx:
      return perf::countEncodeDrawArgbufCbufBindingHashFfpVsCpuTime;
    case kFfpPsArgbufIdx:
      return perf::countEncodeDrawArgbufCbufBindingHashFfpPsCpuTime;
    default:
      return nullptr;
  }
}

PerfRecorder cbufBindingWriteRecorderForArgbufIndex(u32 argbufIdx) {
  switch (argbufIdx) {
    case kVsConstsArgbufIdx:
      return perf::countEncodeDrawArgbufCbufBindingWriteVsCpuTime;
    case kPsConstsArgbufIdx:
      return perf::countEncodeDrawArgbufCbufBindingWritePsCpuTime;
    case kFfpVsArgbufIdx:
      return perf::countEncodeDrawArgbufCbufBindingWriteFfpVsCpuTime;
    case kFfpPsArgbufIdx:
      return perf::countEncodeDrawArgbufCbufBindingWriteFfpPsCpuTime;
    default:
      return nullptr;
  }
}

PerfRecorder cbufObserverRecorderForArgbufIndex(u32 argbufIdx) {
  switch (argbufIdx) {
    case kVsConstsArgbufIdx:
      return perf::countEncodeDrawArgbufCbufObserverVsCpuTime;
    case kPsConstsArgbufIdx:
      return perf::countEncodeDrawArgbufCbufObserverPsCpuTime;
    case kFfpVsArgbufIdx:
      return perf::countEncodeDrawArgbufCbufObserverFfpVsCpuTime;
    case kFfpPsArgbufIdx:
      return perf::countEncodeDrawArgbufCbufObserverFfpPsCpuTime;
    default:
      return nullptr;
  }
}

bool argbufCbufContentHashEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_ARGBUF_CBUF_CONTENT_HASH");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

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
                       const ArgbufRecorder* recorder,
                       WMT::RenderCommandEncoder residencyEncoder) {
  if (recorder && recorder->setBuffer) {
    recorder->setBuffer(recorder->userdata, buffer, offset, index);
  }
  if (recorder && recorder->suppressMetalCalls) {
    return;
  }
  encoderResource.argumentEncoder().setBuffer(buffer, offset, index);
  if (residencyEncoder && buffer) {
    residencyEncoder.useResource(
        WMT::Resource{buffer.handle},
        WMTResourceUsageRead,
        static_cast<WMTRenderStages>(WMTRenderStageVertex | WMTRenderStageFragment));
  }
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

static PopulatedArgbuf openArgbufImpl(CommandQueue& queue,
                                      ArgbufEncoderResource& encoderResource,
                                      std::uint64_t seqId,
                                      std::uint64_t completedSeqId,
                                      bool useCompletedSeqIdSnapshot,
                                      const ArgbufRecorder* recorder) {
  if (!encoderResource.initialized() || encoderResource.encodedLength() == 0) {
    return {};
  }
  const auto length = encoderResource.encodedLength();
  const auto alignment = encoderResource.alignment();
  auto reservation = [&]() {
    PerfScope scope(perf::countEncodeDrawArgbufOpenReserveCpuTime);
    if (useCompletedSeqIdSnapshot) {
      return queue.reserveTransientBufferWithCompletedSeqId(
          static_cast<std::size_t>(length),
          static_cast<std::size_t>(alignment),
          seqId,
          completedSeqId);
    }
    return queue.reserveTransientBuffer(static_cast<std::size_t>(length),
                                        static_cast<std::size_t>(alignment),
                                        seqId);
  }();
  if (!reservation) {
    return {};
  }
  // Anchor the encoder onto the reservation. `setArgumentBuffer` is the
  // prerequisite for any subsequent setBuffer/setTexture/setSamplerState
  // call — none of the populate* helpers below should run before this.
  {
    PerfScope scope(perf::countEncodeDrawArgbufOpenSetArgumentBufferCpuTime);
    recordedSetArgumentBuffer(encoderResource, reservation.slice.buffer,
                               reservation.slice.offset, recorder);
  }
  PopulatedArgbuf populated{};
  populated.storage = reservation.slice.buffer;
  populated.offset = reservation.slice.offset;
  populated.length = length;
  return populated;
}

PopulatedArgbuf openArgbuf(CommandQueue& queue,
                            ArgbufEncoderResource& encoderResource,
                            std::uint64_t seqId,
                            const ArgbufRecorder* recorder) {
  return openArgbufImpl(queue, encoderResource, seqId, 0u,
                        /*useCompletedSeqIdSnapshot=*/false, recorder);
}

PopulatedArgbuf openArgbufWithCompletedSeqId(CommandQueue& queue,
                                             ArgbufEncoderResource& encoderResource,
                                             std::uint64_t seqId,
                                             std::uint64_t completedSeqId,
                                             const ArgbufRecorder* recorder) {
  return openArgbufImpl(queue, encoderResource, seqId, completedSeqId,
                        /*useCompletedSeqIdSnapshot=*/true, recorder);
}

namespace {

// Single-argbuf cbuf upload helper. Reuses `uploadTransientBuffer` so
// the per-cbuf backing storage retains against the same seqId as the
// argbuf itself; the argbuf entry then points at the fresh transient
// slab via `setBuffer`. Returns the bytes written into the transient
// ring, or 0 on reservation failure (caller treats as "skipped").
u64 uploadAndPointEntryBytes(CommandQueue& queue,
                              ArgbufEncoderResource& encoderResource,
                              const void* host,
                              std::size_t byteCount,
                              std::size_t alignment,
                              std::size_t hostStructBytes,
                              u32 argbufIdx,
                              u64 seqId,
                              const ArgbufRecorder* recorder,
                              WMT::RenderCommandEncoder residencyEncoder,
                              ConstantBufferBindings* writtenBindings = nullptr,
                              const ConstantBufferUploadObserver* uploadObserver = nullptr,
                              bool countDirtyPhase = false) {
  if (byteCount == 0) return 0;
  auto doUpload = [&]() {
    return queue.uploadTransientBuffer(
        std::span<const std::byte>(
            static_cast<const std::byte*>(host), byteCount),
        alignment, seqId);
  };
  auto slice = [&]() {
    if (!countDirtyPhase) return doUpload();
    PerfScope scope(perf::countEncodeDrawArgbufCbufUploadCpuTime,
                    cbufUploadRecorderForArgbufIndex(argbufIdx));
    return doUpload();
  }();
  if (!slice) return 0;
  auto doSetBuffer = [&]() {
    recordedSetBuffer(encoderResource, slice.buffer, slice.offset, argbufIdx,
                      recorder, residencyEncoder);
  };
  if (countDirtyPhase) {
    PerfScope scope(perf::countEncodeDrawArgbufCbufSetBufferCpuTime,
                    cbufSetBufferRecorderForArgbufIndex(argbufIdx));
    doSetBuffer();
  } else {
    doSetBuffer();
  }
  if (writtenBindings &&
      argbufIdx < writtenBindings->entries.size()) {
    u64 contentHash = 0;
    if (argbufCbufContentHashEnabled()) {
      auto doHash = [&]() {
        contentHash = hashConstantBufferBytes(host, static_cast<u64>(byteCount));
      };
      if (countDirtyPhase) {
        PerfScope scope(perf::countEncodeDrawArgbufCbufBindingHashCpuTime,
                        cbufBindingHashRecorderForArgbufIndex(argbufIdx));
        doHash();
      } else {
        doHash();
      }
    }
    auto doWriteBinding = [&]() {
      writtenBindings->entries[argbufIdx] = ConstantBufferBinding{
          .buffer = slice.buffer,
          .offset = slice.offset,
          .bytes = static_cast<u64>(byteCount),
          .contentHash = contentHash,
      };
    };
    if (countDirtyPhase) {
      PerfScope scope(perf::countEncodeDrawArgbufCbufBindingWriteCpuTime,
                      cbufBindingWriteRecorderForArgbufIndex(argbufIdx));
      doWriteBinding();
    } else {
      doWriteBinding();
    }
  }
  if (uploadObserver && uploadObserver->upload) {
    auto doObserve = [&]() {
      uploadObserver->upload(
          uploadObserver->userdata, argbufIdx, host,
          static_cast<u64>(byteCount), static_cast<u64>(hostStructBytes));
    };
    if (countDirtyPhase) {
      PerfScope scope(perf::countEncodeDrawArgbufCbufObserverCpuTime,
                      cbufObserverRecorderForArgbufIndex(argbufIdx));
      doObserve();
    } else {
      doObserve();
    }
  }
  return byteCount;
}

// Construct-in-place cbuf upload (R-ARCH-7.4 / R-ARCH-7.5). Instead of building
// the dirty constant bytes into a stack buffer and copying them into the
// transient slab, reserve the MTLStorageModeShared slab first and let `build`
// write the bytes directly into it: the in-place build IS the upload, so this
// removes one stack buffer and one memcpy per dirty upload while keeping the
// slice / setBuffer / binding / observer plumbing identical to
// uploadAndPointEntryBytes. `build` must write exactly `byteCount` bytes.
template <typename BuildFn>
u64 buildAndPointEntryBytes(CommandQueue& queue,
                            ArgbufEncoderResource& encoderResource,
                            std::size_t byteCount,
                            std::size_t alignment,
                            std::size_t hostStructBytes,
                            u32 argbufIdx,
                            u64 seqId,
                            const ArgbufRecorder* recorder,
                            WMT::RenderCommandEncoder residencyEncoder,
                            ConstantBufferBindings* writtenBindings,
                            const ConstantBufferUploadObserver* uploadObserver,
                            bool countDirtyPhase,
                            BuildFn&& build) {
  if (byteCount == 0) return 0;
  auto doReserve = [&]() {
    return queue.reserveTransientBuffer(byteCount, alignment, seqId);
  };
  auto reservation = [&]() {
    if (!countDirtyPhase) return doReserve();
    PerfScope scope(perf::countEncodeDrawArgbufCbufUploadCpuTime,
                    cbufUploadRecorderForArgbufIndex(argbufIdx));
    return doReserve();
  }();
  if (!reservation) return 0;
  build(std::span<std::byte>(reservation.contents, byteCount));
  if (auto* ledger = dxmt9::core::activeCopyMaterializationLedger(
          dxmt9::core::CopyMaterializationOwner::Unix)) {
    ledger->recordMaterialization(
        dxmt9::core::CopyMaterializationClass::GpuSharedMaterialization,
        byteCount);
  }
  const void* host = static_cast<const void*>(reservation.contents);
  auto doSetBuffer = [&]() {
    recordedSetBuffer(encoderResource, reservation.slice.buffer,
                      reservation.slice.offset, argbufIdx, recorder,
                      residencyEncoder);
  };
  if (countDirtyPhase) {
    PerfScope scope(perf::countEncodeDrawArgbufCbufSetBufferCpuTime,
                    cbufSetBufferRecorderForArgbufIndex(argbufIdx));
    doSetBuffer();
  } else {
    doSetBuffer();
  }
  if (writtenBindings &&
      argbufIdx < writtenBindings->entries.size()) {
    u64 contentHash = 0;
    if (argbufCbufContentHashEnabled()) {
      auto doHash = [&]() {
        contentHash = hashConstantBufferBytes(host, static_cast<u64>(byteCount));
      };
      if (countDirtyPhase) {
        PerfScope scope(perf::countEncodeDrawArgbufCbufBindingHashCpuTime,
                        cbufBindingHashRecorderForArgbufIndex(argbufIdx));
        doHash();
      } else {
        doHash();
      }
    }
    auto doWriteBinding = [&]() {
      writtenBindings->entries[argbufIdx] = ConstantBufferBinding{
          .buffer = reservation.slice.buffer,
          .offset = reservation.slice.offset,
          .bytes = static_cast<u64>(byteCount),
          .contentHash = contentHash,
      };
    };
    if (countDirtyPhase) {
      PerfScope scope(perf::countEncodeDrawArgbufCbufBindingWriteCpuTime,
                      cbufBindingWriteRecorderForArgbufIndex(argbufIdx));
      doWriteBinding();
    } else {
      doWriteBinding();
    }
  }
  if (uploadObserver && uploadObserver->upload) {
    auto doObserve = [&]() {
      uploadObserver->upload(
          uploadObserver->userdata, argbufIdx, host,
          static_cast<u64>(byteCount), static_cast<u64>(hostStructBytes));
    };
    if (countDirtyPhase) {
      PerfScope scope(perf::countEncodeDrawArgbufCbufObserverCpuTime,
                      cbufObserverRecorderForArgbufIndex(argbufIdx));
      doObserve();
    } else {
      doObserve();
    }
  }
  return byteCount;
}

template <typename HostStruct>
u64 uploadAndPointEntry(CommandQueue& queue,
                         ArgbufEncoderResource& encoderResource,
                         const HostStruct& host,
                         std::size_t byteCount,
                         u32 argbufIdx,
                         u64 seqId,
                         const ArgbufRecorder* recorder,
                         WMT::RenderCommandEncoder residencyEncoder,
                         ConstantBufferBindings* writtenBindings = nullptr,
                         const ConstantBufferUploadObserver* uploadObserver = nullptr,
                         bool countDirtyPhase = false) {
  return uploadAndPointEntryBytes(
      queue, encoderResource, &host, byteCount,
      state::kConstantBufferOffsetAlignment,
      sizeof(HostStruct), argbufIdx, seqId, recorder, residencyEncoder,
      writtenBindings, uploadObserver, countDirtyPhase);
}

template <typename HostStruct>
u64 uploadAndPointEntry(CommandQueue& queue,
                         ArgbufEncoderResource& encoderResource,
                         const HostStruct& host,
                         u32 argbufIdx,
                         u64 seqId,
                         const ArgbufRecorder* recorder,
                         WMT::RenderCommandEncoder residencyEncoder,
                         ConstantBufferBindings* writtenBindings = nullptr,
                         const ConstantBufferUploadObserver* uploadObserver = nullptr,
                         bool countDirtyPhase = false) {
  return uploadAndPointEntry(
      queue, encoderResource, host, sizeof(HostStruct), argbufIdx, seqId,
      recorder, residencyEncoder, writtenBindings, uploadObserver,
      countDirtyPhase);
}

}  // namespace

u64 populateConstantBuffers(CommandQueue& queue,
                             ArgbufEncoderResource& encoderResource,
                             core::FlatDrawStateView state,
                             std::uint64_t seqId,
                             const ArgbufRecorder* recorder,
                             WMT::RenderCommandEncoder residencyEncoder,
                             ConstantBufferBindings* writtenBindings,
                             const ConstantBufferUploadObserver* uploadObserver) {
  if (!encoderResource.initialized()) return 0;
  u64 bytes = 0;
  // VsConsts / FfpVsConsts / PsConsts / FfpPsConsts mirror the Stage 1
  // bind sequence in encodeDraw — same builders, same alignment, same
  // upload path; the only delta is the argbuf [[id(N)]] target instead
  // of the slot 0 / 3 vert/frag slots.
  const auto vs = state::buildVsConsts(state);
  bytes += uploadAndPointEntry(queue, encoderResource, vs,
                                kVsConstsArgbufIdx, seqId, recorder, residencyEncoder,
                                writtenBindings, uploadObserver);
  const auto ffpVs = state::buildFfpVsConsts(state);
  bytes += uploadAndPointEntry(queue, encoderResource, ffpVs,
                                kFfpVsArgbufIdx, seqId, recorder, residencyEncoder,
                                writtenBindings, uploadObserver);
  const auto ps = state::buildPsConsts(state);
  bytes += uploadAndPointEntry(queue, encoderResource, ps,
                                kPsConstsArgbufIdx, seqId, recorder, residencyEncoder,
                                writtenBindings, uploadObserver);
  const auto ffpPs = state::buildFfpPsConsts(state);
  bytes += uploadAndPointEntry(queue, encoderResource, ffpPs,
                                kFfpPsArgbufIdx, seqId, recorder, residencyEncoder,
                                writtenBindings, uploadObserver);
  return bytes;
}

u64 updateDirtyArgbufRegions(CommandQueue& queue,
                              ArgbufEncoderResource& encoderResource,
                              core::FlatDrawStateView state,
                              const uniform::DirtyState& dirty,
                              std::uint64_t seqId,
                              const ArgbufRecorder* recorder,
                              WMT::RenderCommandEncoder residencyEncoder,
                              ConstantBufferBindings* writtenBindings,
                              const ConstantBufferUploadObserver* uploadObserver) {
  uniform::ShaderConstantUsageBounds unknown{};
  return updateDirtyArgbufRegions(
      queue, encoderResource, state, dirty, unknown, unknown, seqId,
      recorder, residencyEncoder, writtenBindings, uploadObserver);
}

u64 updateDirtyArgbufRegions(CommandQueue& queue,
                              ArgbufEncoderResource& encoderResource,
                              core::FlatDrawStateView state,
                              const uniform::DirtyState& dirty,
                              uniform::ShaderConstantUsageBounds vsUsage,
                              uniform::ShaderConstantUsageBounds psUsage,
                              std::uint64_t seqId,
                              const ArgbufRecorder* recorder,
                              WMT::RenderCommandEncoder residencyEncoder,
                              ConstantBufferBindings* writtenBindings,
                              const ConstantBufferUploadObserver* uploadObserver) {
  if (!encoderResource.initialized()) return 0;
  u64 bytes = 0;
  if (uniform::anyDirty(dirty, uniform::kVsAny)) {
    perf::countEncodeDrawArgbufCbufUpdateVsCalls(1u);
    PerfScope blockScope(perf::countEncodeDrawArgbufCbufUpdateVsCpuTime);
    uniform::ShaderConstantUploadPlan plan{};
    std::size_t byteCount = 0;
    {
      PerfScope planScope(perf::countEncodeDrawArgbufCbufUploadPlanCpuTime,
                          perf::countEncodeDrawArgbufCbufUploadPlanVsCpuTime);
      plan = uniform::makeVsConstantUploadPlan(dirty, vsUsage);
      byteCount = static_cast<std::size_t>(
          uniform::vsConstantUploadBytes(plan));
    }
    // R-ARCH-7.4 / R-ARCH-7.5: build the dirty VS constants directly into the
    // shared transient slab instead of a stack buffer plus copy.
    const auto written = buildAndPointEntryBytes(
        queue, encoderResource, byteCount,
        state::kConstantBufferOffsetAlignment,
        sizeof(state::VsConsts), kVsConstsArgbufIdx, seqId, recorder,
        residencyEncoder, writtenBindings, uploadObserver,
        /*countDirtyPhase=*/true,
        [&](std::span<std::byte> dst) {
          PerfScope buildScope(perf::countEncodeDrawArgbufCbufBuildCpuTime,
                               cbufBuildRecorderForArgbufIndex(kVsConstsArgbufIdx));
          state::buildVsConstsUploadBytes(state, plan, dst);
        });
    bytes += written;
    if (written != 0) {
      perf::countEncodeDrawArgbufCbufUpdateVsBytes(written);
    }
  }
  if (uniform::anyDirty(dirty, uniform::kFfpVsAny)) {
    perf::countEncodeDrawArgbufCbufUpdateFfpVsCalls(1u);
    PerfScope blockScope(perf::countEncodeDrawArgbufCbufUpdateFfpVsCpuTime);
    state::FfpVsConsts ffpVs{};
    {
      PerfScope buildScope(perf::countEncodeDrawArgbufCbufBuildCpuTime,
                           cbufBuildRecorderForArgbufIndex(kFfpVsArgbufIdx));
      ffpVs = state::buildFfpVsConsts(state);
    }
    const auto written = uploadAndPointEntry(
        queue, encoderResource, ffpVs, kFfpVsArgbufIdx, seqId, recorder,
        residencyEncoder, writtenBindings, uploadObserver,
        /*countDirtyPhase=*/true);
    bytes += written;
    if (written != 0) {
      perf::countEncodeDrawArgbufCbufUpdateFfpVsBytes(written);
    }
  }
  if (uniform::anyDirty(dirty, uniform::kPsAny)) {
    perf::countEncodeDrawArgbufCbufUpdatePsCalls(1u);
    PerfScope blockScope(perf::countEncodeDrawArgbufCbufUpdatePsCpuTime);
    uniform::ShaderConstantUploadPlan plan{};
    std::size_t byteCount = 0;
    {
      PerfScope planScope(perf::countEncodeDrawArgbufCbufUploadPlanCpuTime,
                          perf::countEncodeDrawArgbufCbufUploadPlanPsCpuTime);
      plan = uniform::makePsConstantUploadPlan(dirty, psUsage);
      byteCount = static_cast<std::size_t>(
          uniform::psConstantUploadBytes(plan));
    }
    // R-ARCH-7.4 / R-ARCH-7.5: build the dirty PS constants directly into the
    // shared transient slab instead of a stack buffer plus copy.
    const auto written = buildAndPointEntryBytes(
        queue, encoderResource, byteCount,
        state::kConstantBufferOffsetAlignment,
        sizeof(state::PsConsts), kPsConstsArgbufIdx, seqId, recorder,
        residencyEncoder, writtenBindings, uploadObserver,
        /*countDirtyPhase=*/true,
        [&](std::span<std::byte> dst) {
          PerfScope buildScope(perf::countEncodeDrawArgbufCbufBuildCpuTime,
                               cbufBuildRecorderForArgbufIndex(kPsConstsArgbufIdx));
          state::buildPsConstsUploadBytes(state, plan, dst);
        });
    bytes += written;
    if (written != 0) {
      perf::countEncodeDrawArgbufCbufUpdatePsBytes(written);
    }
  }
  if (uniform::anyDirty(dirty, uniform::kFfpPsAny)) {
    perf::countEncodeDrawArgbufCbufUpdateFfpPsCalls(1u);
    PerfScope blockScope(perf::countEncodeDrawArgbufCbufUpdateFfpPsCpuTime);
    const auto written = buildAndPointEntryBytes(
        queue, encoderResource, sizeof(state::FfpPsConsts),
        state::kConstantBufferOffsetAlignment, sizeof(state::FfpPsConsts),
        kFfpPsArgbufIdx, seqId, recorder, residencyEncoder, writtenBindings,
        uploadObserver, /*countDirtyPhase=*/true,
        [&](std::span<std::byte> dst) {
          PerfScope buildScope(perf::countEncodeDrawArgbufCbufBuildCpuTime,
                               cbufBuildRecorderForArgbufIndex(kFfpPsArgbufIdx));
          state::buildFfpPsConstsUploadBytes(state, dst);
        });
    bytes += written;
    if (written != 0) {
      perf::countEncodeDrawArgbufCbufUpdateFfpPsBytes(written);
    }
  }
  return bytes;
}

u32 populateResourceBindings(ArgbufEncoderResource& encoderResource,
                             std::span<const ResourceArrayBinding> bindings,
                             const ResourceArrayRecorder* recorder,
                             WMT::RenderCommandEncoder residencyEncoder) {
  if (!encoderResource.initialized()) return 0;
  // Residency usage for an argbuf-pointed sampled texture: Read | Sample,
  // visible to both the vertex and fragment stages. Fault candidate 4 — a
  // resource only referenced through an argument buffer is NOT made
  // resident by binding the argbuf at slot 30; without this explicit
  // useResource the GPU faults (the historical texture-corpus readback
  // fault). Read covers texelFetch-style access, Sample covers
  // .sample(); both are cheap to over-request and the validation layer
  // rejects a missing one, not a superset.
  const auto kUsage = static_cast<WMTResourceUsage>(
      WMTResourceUsageRead | WMTResourceUsageSample);
  const auto kStages = static_cast<WMTRenderStages>(
      WMTRenderStageVertex | WMTRenderStageFragment);
  const bool suppress = recorder && recorder->suppressMetalCalls;
  u32 residentTextures = 0;
  for (const auto& b : bindings) {
    if (b.stage >= shaders::kArgbufResourceArrayStageCount) {
      continue;
    }
    const u32 textureId = shaders::kArgbufResourceArrayTextureBaseId + b.stage;
    const u32 samplerId = shaders::kArgbufResourceArraySamplerBaseId + b.stage;
    if (b.texture) {
      if (recorder && recorder->setTexture) {
        recorder->setTexture(recorder->userdata, b.texture.handle, textureId);
      }
      if (!suppress) {
        encoderResource.argumentEncoder().setTexture(b.texture, textureId);
      }
      // Residency — fault candidate 4. Issue useResource even in the
      // recorder path so the (handle, usage, stages) tuple is observable;
      // the Metal call itself is gated on a live residencyEncoder.
      if (recorder && recorder->useTexture) {
        recorder->useTexture(recorder->userdata, b.texture.handle,
                             static_cast<u32>(kUsage),
                             static_cast<u32>(kStages));
      }
      if (residencyEncoder && !suppress) {
        residencyEncoder.useResource(WMT::Resource{b.texture.handle}, kUsage,
                                     kStages);
      }
      ++residentTextures;
    }
    if (b.sampler) {
      if (recorder && recorder->setSampler) {
        recorder->setSampler(recorder->userdata, b.sampler.handle, samplerId);
      }
      if (!suppress) {
        encoderResource.argumentEncoder().setSamplerState(b.sampler, samplerId);
      }
      // Samplers carry no backing allocation — no useResource. Lifetime is
      // the caller's WMT::SamplerState retention against the argbuf seqId
      // (fault candidate 2).
    }
  }
  return residentTextures;
}

void pointFfpVsAtSlice(ArgbufEncoderResource& encoderResource,
                       WMT::Buffer buffer,
                       u64 offset,
                       const ArgbufRecorder* recorder,
                       WMT::RenderCommandEncoder residencyEncoder) {
  if (!encoderResource.initialized() || !buffer) return;
  recordedSetBuffer(encoderResource, buffer, offset, kFfpVsArgbufIdx,
                    recorder, residencyEncoder);
}

void pointConstantBufferBinding(ArgbufEncoderResource& encoderResource,
                                u32 argbufIndex,
                                ConstantBufferBinding binding,
                                const ArgbufRecorder* recorder,
                                WMT::RenderCommandEncoder residencyEncoder) {
  if (!encoderResource.initialized() || !binding ||
      argbufIndex >= shaders::kArgbufHybridConstantBufferCount) {
    return;
  }
  recordedSetBuffer(encoderResource, binding.buffer, binding.offset, argbufIndex,
                    recorder, residencyEncoder);
}

}  // namespace dxmt9::argbuf_hybrid
