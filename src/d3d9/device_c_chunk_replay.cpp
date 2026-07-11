// Batched-chunk C ABI: dxmt9c_device_commit_chunk and the packet->state
// replay machinery it invokes. Per-call dxmt9c_* setters live in
// device_c_draw.cpp (interactive entry points) and device_c_state.cpp
// (state setters); both are referenced via extern "C" forward decls.

#include "device_c_provider.hpp"
#include "device_c_record_utils.hpp"
#include "device_c_replay_offload.hpp"
#include "util/unixcall_marshal.hpp"

#include "../dxmt9/dxmt9_perf_counters.hpp"
// Need the full dxmt9::Device type to call markChunkResources on the
// upperDevice shared_ptr that the chunk importer's Phase 4-B path
// hands the per-chunk retention list to.
#include "dxmt9/dxmt9_device.hpp"
#include "util/log/log.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#endif

using namespace dxmt9::d3d9::devicec;

// Forward declarations for state-setter ABI entry points implemented in
// device_c_state.cpp. The packet-replay path (applyDrawPacketStateViaIface)
// and the chunk importer (dxmt9c_device_commit_chunk) call these to dispatch
// per-record state deltas. The provider macro renames apply uniformly via
// device_c_provider.hpp, so the linker resolves to the same symbols.
extern "C" int32_t dxmt9c_device_set_viewport(D9CDevice* d, const D9CViewport* vp);
extern "C" int32_t dxmt9c_device_set_scissor_rect(D9CDevice* d, const D9CRect* r);
extern "C" int32_t dxmt9c_device_set_transform(D9CDevice* d, uint32_t state,
                                               const D9CMatrix* m);
extern "C" int32_t dxmt9c_device_set_material(D9CDevice* d, const D9CMaterial* m);
extern "C" int32_t dxmt9c_device_set_light(D9CDevice* d, uint32_t idx, const D9CLight* l);
extern "C" int32_t dxmt9c_device_light_enable(D9CDevice* d, uint32_t i, uint32_t en);
extern "C" int32_t dxmt9c_device_set_texture_stage_state(D9CDevice* d, uint32_t st,
                                                         uint32_t type, uint32_t val);
extern "C" int32_t dxmt9c_device_set_sampler_state(D9CDevice* d, uint32_t s, uint32_t type,
                                                   uint32_t val);
extern "C" int32_t dxmt9c_device_set_clip_plane(D9CDevice* d, uint32_t idx,
                                                const float plane[4]);
extern "C" int32_t dxmt9c_device_set_vs_const_f(D9CDevice* d, uint32_t s, const float* data,
                                                uint32_t cnt);
extern "C" int32_t dxmt9c_device_set_ps_const_f(D9CDevice* d, uint32_t s, const float* data,
                                                uint32_t cnt);
extern "C" int32_t dxmt9c_device_set_vs_const_i(D9CDevice* d, uint32_t s, const int32_t* data,
                                                uint32_t cnt);
extern "C" int32_t dxmt9c_device_set_ps_const_i(D9CDevice* d, uint32_t s, const int32_t* data,
                                                uint32_t cnt);
extern "C" int32_t dxmt9c_device_set_vs_const_b(D9CDevice* d, uint32_t s,
                                                const uint32_t* data, uint32_t cnt);
extern "C" int32_t dxmt9c_device_set_ps_const_b(D9CDevice* d, uint32_t s,
                                                const uint32_t* data, uint32_t cnt);

// Forward declaration for the query-issue ABI entry implemented in
// device_c_swapchain_query_stateblock.cpp; chunk replay routes
// D9C_COMMAND_RECORD_QUERY_ISSUE through it.
extern "C" int32_t dxmt9c_query_issue(D9CQuery* q, uint32_t flags);

// Forward declarations for the per-call interactive C ABI entries that
// remain in device_c_draw.cpp. The chunk dispatcher routes the matching
// D9C_COMMAND_RECORD_* and the per-packet shader/render-target/depth/
// vertex-decl/index-buffer deltas through these so behavior matches the
// per-call path bit-for-bit.
extern "C" int32_t dxmt9c_device_present(D9CDevice* d, const D9CRect* src, const D9CRect* dst,
                                         uint64_t destWindow, const void* dirty, uint32_t flags);
extern "C" int32_t dxmt9c_device_clear(D9CDevice* d, uint32_t count, const D9CRect* rects,
                                       uint32_t flags, uint32_t colorARGB, float z,
                                       uint32_t stencil);
extern "C" int32_t dxmt9c_device_set_stream_source(D9CDevice* d, uint32_t stream,
                                                   D9CBuffer* buf, uint32_t off,
                                                   uint32_t stride);
extern "C" int32_t dxmt9c_device_set_vertex_declaration(D9CDevice* d, D9CVertexDecl* vd);
extern "C" int32_t dxmt9c_device_set_indices(D9CDevice* d, D9CBuffer* buf);
extern "C" int32_t dxmt9c_device_set_vertex_shader(D9CDevice* d, D9CShader* s);
extern "C" int32_t dxmt9c_device_set_pixel_shader(D9CDevice* d, D9CShader* s);
extern "C" int32_t dxmt9c_device_set_render_target(D9CDevice* d, uint32_t idx,
                                                   D9CSurface* surf);
extern "C" int32_t dxmt9c_device_set_depth_stencil(D9CDevice* d, D9CSurface* surf);
extern "C" int32_t dxmt9c_device_update_surface(D9CDevice* d, D9CSurface* src,
                                                const D9CRect*, D9CSurface* dst,
                                                const D9CRect*);
extern "C" int32_t dxmt9c_device_update_texture(D9CDevice* d, D9CTexture* src,
                                                D9CTexture* dst);
extern "C" int32_t dxmt9c_device_stretch_rect(D9CDevice* d, D9CSurface* src, const D9CRect* sr,
                                              D9CSurface* dst, const D9CRect* dr,
                                              uint32_t filter);
extern "C" int32_t dxmt9c_device_color_fill(D9CDevice* d, D9CSurface* surf, const D9CRect* r,
                                            uint32_t colorARGB);
extern "C" int32_t dxmt9c_device_get_render_target_data(D9CDevice* d, D9CSurface* rt,
                                                        D9CSurface* dst);

namespace {

bool peRecorderStatsEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("DXMT9_PE_RECORDER_STATS");
    return value && value[0] != '\0' && !(value[0] == '0' && value[1] == '\0');
  }();
  return enabled;
}

bool compactUniformSubmissionsEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("DXMT9_ENABLE_COMPACT_UNIFORM_SUBMISSIONS");
    return value && value[0] != '\0' && !(value[0] == '0' && value[1] == '\0');
  }();
  return enabled;
}

bool drawRunCanonicalFastPathEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("DXMT9_ENABLE_DRAW_RUN_CANONICAL_FAST_PATH");
    return value && value[0] != '\0' && !(value[0] == '0' && value[1] == '\0');
  }();
  return enabled;
}

bool chunkEndFlushProbeEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("DXMT9_PERF_CHUNK_END_FLUSH_PROBE");
    return value && value[0] != '\0' && !(value[0] == '0' && value[1] == '\0');
  }();
  return enabled;
}

bool renderTraceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("DXMT_TRACE_RENDER");
    return value && value[0] != '\0' && !(value[0] == '0' && value[1] == '\0');
  }();
  return enabled;
}

bool compactSubmissionCarrierEnabled() {
  return compactUniformSubmissionsEnabled() && !renderTraceEnabled();
}

std::uint64_t currentNativeThreadId() {
#if defined(__APPLE__)
  std::uint64_t tid = 0;
  if (pthread_threadid_np(nullptr, &tid) == 0) {
    return tid;
  }
#endif
  return 0;
}

std::uint64_t currentPthreadSelfBits() {
#if defined(__APPLE__)
  return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pthread_self()));
#else
  return 0;
#endif
}

struct PendingDrawSubmissionScratch {
  std::vector<dxmt9::core::DrawRunSubmission> submissions;
  std::vector<dxmt9::core::DrawRunCompactSubmission> compactSubmissions;
  dxmt9::core::DrawSubmissionUniformScratch uniformScratch;
  bool inUse = false;
};

PendingDrawSubmissionScratch& pendingDrawSubmissionScratch() {
  static thread_local PendingDrawSubmissionScratch scratch;
  return scratch;
}

class ScopedPendingDrawSubmissionScratchUse {
public:
  explicit ScopedPendingDrawSubmissionScratchUse(
      PendingDrawSubmissionScratch& scratch) noexcept
      : scratch_(scratch) {
    DXMT_ASSERT(!scratch_.inUse);
    scratch_.inUse = true;
    scratch_.submissions.clear();
    scratch_.compactSubmissions.clear();
    scratch_.uniformScratch.clear();
  }

  ~ScopedPendingDrawSubmissionScratchUse() {
    scratch_.submissions.clear();
    scratch_.compactSubmissions.clear();
    scratch_.uniformScratch.clear();
    scratch_.inUse = false;
  }

  ScopedPendingDrawSubmissionScratchUse(
      const ScopedPendingDrawSubmissionScratchUse&) = delete;
  ScopedPendingDrawSubmissionScratchUse& operator=(
      const ScopedPendingDrawSubmissionScratchUse&) = delete;

private:
  PendingDrawSubmissionScratch& scratch_;
};

// C1 helper: route per-record dirty marking through the dxmt9
// CommandQueue's pendingDirty_ accumulator. Returns nullptr on test /
// stub paths where no upperDevice exists; callers no-op in that case.
dxmt9::CommandQueue* findDirtyQueue(D9CDevice* d) {
  if (!d) return nullptr;
  auto upper = d->dev().upperDevice();
  if (!upper) return nullptr;
  return &upper->queue();
}

// C1 helper: D3D9 render-state IDs that affect each FFP PS uniform
// sub-block. Centralized so commit_chunk's APPLY_STATE handler and
// the per-call SetRenderState dispatcher (if it ever wires direct
// dirty-marking) stay in sync.
bool isFogRenderState(uint32_t state) {
  return state == dxmt9::core::RS_FOG_ENABLE ||
         state == dxmt9::core::RS_FOG_COLOR ||
         state == dxmt9::core::RS_FOG_TABLE_MODE ||
         state == dxmt9::core::RS_FOG_START ||
         state == dxmt9::core::RS_FOG_END ||
         state == dxmt9::core::RS_FOG_DENSITY ||
         state == dxmt9::core::RS_FOG_FROM_VERTEX ||
         state == dxmt9::core::RS_RANGE_FOG;
}

bool isAlphaRenderState(uint32_t state) {
  return state == dxmt9::core::RS_ALPHA_TEST_ENABLE ||
         state == dxmt9::core::RS_ALPHA_FUNC ||
         state == dxmt9::core::RS_ALPHA_REF;
}

// C1 helper: scan a draw-packet state delta and OR matching dirty bits
// onto the queue's pendingDirty_. Used by APPLY_STATE record
// dispatch AND by every DRAW_* record (which folds a state delta in
// front of its draw call via applyDrawPacketState). Safe to call when
// q is nullptr (no-op for stub / test paths).
void markDirtyFromDrawPacketState(dxmt9::CommandQueue* q,
                                  const D9CDrawPrimitivePacket& packet) {
  if (!q) return;
  for (uint32_t i = 0; i < packet.renderStateCount; ++i) {
    const auto& entry = packet.renderStates[i];
    if (isFogRenderState(entry.state)) q->applyDirtyRenderStateFog();
    if (isAlphaRenderState(entry.state)) q->applyDirtyRenderStateAlpha();
    if (entry.state == dxmt9::core::RS_TEXTURE_FACTOR) {
      q->applyDirtyRenderStateTexFactor();
    }
  }
  for (uint32_t i = 0; i < packet.tssCount; ++i) {
    if (packet.tss[i].type == dxmt9::core::TSS_CONSTANT) {
      q->applyDirtyTextureStageConstant();
    }
  }
  if (packet.transformCount > 0) q->applyDirtyTransformChange();
  // Light slot/enable + material deltas share the FFP VS uniform
  // block with transforms (spec.md §10 — lights[8] sits next to
  // worldViewMatrix); folded into the transforms-bit so the FFP VS
  // uniform block re-uploads.
  if (packet.lightSlotMask != 0 ||
      packet.lightEnableValidMask != 0 ||
      packet.materialValid) {
    q->applyDirtyTransformChange();
  }
  if (packet.clipPlaneMask != 0) q->applyDirtyClipPlaneChange();
  if (packet.viewportValid) q->applyDirtyViewportChange();
}

uint64_t wireHandleValue(const D9CWireHandle& handle) {
  return static_cast<uint64_t>(handle.lo) | (static_cast<uint64_t>(handle.hi) << 32);
}

template <typename T>
T* wireValuePtr(uint64_t value) {
  if (!value) {
    return nullptr;
  }
  if (value <= 0xffffffffull) {
    if (auto* decoded =
            dxmt9::util::marshal::wow64::decodeHandle<T*>(static_cast<uint32_t>(value))) {
      return decoded;
    }
    if (isWow64NativePointerAllowed(value)) {
      return reinterpret_cast<T*>(static_cast<uintptr_t>(value));
    }
    if (requiresWow64PointerShadow()) {
      return nullptr;
    }
  }
  return reinterpret_cast<T*>(static_cast<uintptr_t>(value));
}

template <typename T>
T* wireHandlePtr(const D9CWireHandle& handle) {
  return wireValuePtr<T>(wireHandleValue(handle));
}

uint64_t bufferObjectHandleValue(const std::shared_ptr<dxmt9::core::Buffer>& buffer) {
  return buffer ? buffer->handle().value : 0ull;
}

uint64_t wireBufferObjectHandleValue(const D9CWireHandle& handle) {
  auto* buffer = wireHandlePtr<D9CBuffer>(handle);
  return buffer && buffer->obj ? buffer->obj->handle().value : 0ull;
}

bool failed(int32_t hr) {
  return hr < 0;
}

void countDurationSince(std::chrono::steady_clock::time_point start,
                        void (*counter)(std::uint64_t)) {
  counter(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - start).count()));
}

bool commitChunkRecordIsDrawReplay(std::uint32_t type);
void (*commitChunkReplayRecordDetailCounter(std::uint32_t type))(std::uint64_t);

class ReplayRecordCpuScope {
 public:
  explicit ReplayRecordCpuScope(std::uint32_t recordType)
      : start_(std::chrono::steady_clock::now()),
        broadCounter_(commitChunkRecordIsDrawReplay(recordType)
                          ? dxmt9::perf::countCommitChunkReplayDrawRecordCpuTime
                          : dxmt9::perf::countCommitChunkReplayNonDrawRecordCpuTime),
        detailCounter_(commitChunkReplayRecordDetailCounter(recordType)) {
  }

  ~ReplayRecordCpuScope() {
    const auto nanoseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start_).count());
    broadCounter_(nanoseconds);
    if (detailCounter_) {
      detailCounter_(nanoseconds);
    }
  }

  ReplayRecordCpuScope(const ReplayRecordCpuScope&) = delete;
  ReplayRecordCpuScope& operator=(const ReplayRecordCpuScope&) = delete;

 private:
  std::chrono::steady_clock::time_point start_;
  void (*broadCounter_)(std::uint64_t);
  void (*detailCounter_)(std::uint64_t);
};

bool commitChunkRecordAllowsPendingDrawBatchThrough(std::uint32_t type) {
  switch (type) {
  case D9C_COMMAND_RECORD_SET_VS_CONST_F:
  case D9C_COMMAND_RECORD_SET_VS_CONST_I:
  case D9C_COMMAND_RECORD_SET_VS_CONST_B:
  case D9C_COMMAND_RECORD_SET_PS_CONST_F:
  case D9C_COMMAND_RECORD_SET_PS_CONST_I:
  case D9C_COMMAND_RECORD_SET_PS_CONST_B:
    return true;
  default:
    return false;
  }
}

enum class PendingDrawFlushReason : std::uint8_t {
  BeforeRecord,
  DrawRun,
  DrawFallback,
  Failure,
  End,
};

void countPendingDrawFlushReason(PendingDrawFlushReason reason,
                                 std::uint64_t nanoseconds) {
  switch (reason) {
  case PendingDrawFlushReason::BeforeRecord:
    dxmt9::perf::countCommitChunkReplayPendingFlushBeforeRecordCpuTime(
        nanoseconds);
    break;
  case PendingDrawFlushReason::DrawRun:
    dxmt9::perf::countCommitChunkReplayPendingFlushDrawRunCpuTime(
        nanoseconds);
    break;
  case PendingDrawFlushReason::DrawFallback:
    dxmt9::perf::countCommitChunkReplayPendingFlushDrawFallbackCpuTime(
        nanoseconds);
    break;
  case PendingDrawFlushReason::Failure:
    dxmt9::perf::countCommitChunkReplayPendingFlushFailureCpuTime(
        nanoseconds);
    break;
  case PendingDrawFlushReason::End:
    dxmt9::perf::countCommitChunkReplayPendingFlushEndCpuTime(nanoseconds);
    break;
  }
}

void countPendingDrawFlushReasonVolume(PendingDrawFlushReason reason,
                                       std::uint64_t records) {
  switch (reason) {
  case PendingDrawFlushReason::BeforeRecord:
    dxmt9::perf::countCommitChunkReplayPendingFlushBeforeRecord(records);
    break;
  case PendingDrawFlushReason::DrawRun:
    dxmt9::perf::countCommitChunkReplayPendingFlushDrawRun(records);
    break;
  case PendingDrawFlushReason::DrawFallback:
    dxmt9::perf::countCommitChunkReplayPendingFlushDrawFallback(records);
    break;
  case PendingDrawFlushReason::Failure:
    dxmt9::perf::countCommitChunkReplayPendingFlushFailure(records);
    break;
  case PendingDrawFlushReason::End:
    dxmt9::perf::countCommitChunkReplayPendingFlushEnd(records);
    break;
  }
}

template <typename Submission>
void storeChunkEndFlushProbe(D9CDevice* d,
                             const Submission& submission,
                             std::uint64_t pendingRecords) {
  if (!chunkEndFlushProbeEnabled() || !d) {
    return;
  }
  d->chunkEndFlushProbe = D9CDevice::ChunkEndFlushProbe{
      .valid = true,
      .pendingRecords = pendingRecords,
      .stateGeneration = submission.stateGeneration,
      .uniformGeneration = submission.uniformGeneration,
      .uniformPayloadHash = submission.uniformPayloadHash,
      .stateLane = submission.stateLane,
  };
  dxmt9::perf::countCommitChunkReplayEndFlushProbeStored(pendingRecords);
}

template <typename Submission>
void resolveChunkEndFlushProbeWithSubmission(D9CDevice* d,
                                             const Submission& submission) {
  if (!chunkEndFlushProbeEnabled() || !d || !d->chunkEndFlushProbe.valid) {
    return;
  }
  const auto probe = d->chunkEndFlushProbe;
  d->chunkEndFlushProbe = {};
  const bool sameStateLane =
      probe.stateGeneration == submission.stateGeneration &&
      probe.stateLane == submission.stateLane;
  dxmt9::perf::countCommitChunkReplayEndFlushProbeFirstSubmission(
      probe.pendingRecords, sameStateLane,
      probe.uniformGeneration == submission.uniformGeneration,
      probe.uniformPayloadHash == submission.uniformPayloadHash);
}

void resolveChunkEndFlushProbeWithDrawRun(D9CDevice* d,
                                          std::uint64_t runRecords) {
  if (!chunkEndFlushProbeEnabled() || !d || !d->chunkEndFlushProbe.valid) {
    return;
  }
  const auto pendingRecords = d->chunkEndFlushProbe.pendingRecords;
  d->chunkEndFlushProbe = {};
  dxmt9::perf::countCommitChunkReplayEndFlushProbeFirstDrawRun(
      pendingRecords, runRecords);
}

void blockChunkEndFlushProbe(D9CDevice* d, bool drawFallback) {
  if (!chunkEndFlushProbeEnabled() || !d || !d->chunkEndFlushProbe.valid) {
    return;
  }
  const auto pendingRecords = d->chunkEndFlushProbe.pendingRecords;
  d->chunkEndFlushProbe = {};
  dxmt9::perf::countCommitChunkReplayEndFlushProbeBlocked(
      drawFallback, pendingRecords);
}

template <typename Submission>
void attachCompactUniformArena(
    std::vector<Submission>& submissions,
    dxmt9::core::DrawSubmissionUniformScratch& scratch) {
  const auto fixedPayloads =
      std::span<const dxmt9::core::DrawUniformFixedPayload>(
          scratch.fixedPayloads.data(), scratch.fixedPayloads.size());
  const auto stageBytes = std::span<const dxmt9::core::u8>(
      scratch.stageBytes.data(), scratch.stageBytes.size());
  for (auto& submission : submissions) {
    if (submission.compactUniforms.has_value()) {
      submission.compactUniformArena =
          dxmt9::core::DrawUniformCompactPayloadArenaView{
              .fixedPayloads = fixedPayloads,
              .stageBytes = stageBytes,
          };
    }
  }
}

bool commitChunkRecordIsConstantUpload(std::uint32_t type) {
  return commitChunkRecordAllowsPendingDrawBatchThrough(type);
}

bool commitChunkRecordIsDrawReplay(std::uint32_t type) {
  switch (type) {
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP:
    return true;
  default:
    return false;
  }
}

dxmt9::core::metalqueue::NoEnqueueCommitChunkRecordShape
summarizeNoEnqueueCommitChunkRecordShape(const ImportedWireChunkView& chunk) {
  dxmt9::core::metalqueue::NoEnqueueCommitChunkRecordShape shape{};
  std::uint32_t recordIndex = 0;
  while (auto recordView = nextImportedRecord(chunk, recordIndex)) {
    if (!recordView->valid()) {
      break;
    }
    ++shape.recordCount;
    switch (recordView->header.type) {
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP:
      ++shape.drawRecords;
      break;
    case D9C_COMMAND_RECORD_SET_VS_CONST_F:
    case D9C_COMMAND_RECORD_SET_VS_CONST_I:
    case D9C_COMMAND_RECORD_SET_VS_CONST_B:
    case D9C_COMMAND_RECORD_SET_PS_CONST_F:
    case D9C_COMMAND_RECORD_SET_PS_CONST_I:
    case D9C_COMMAND_RECORD_SET_PS_CONST_B:
      ++shape.constRecords;
      break;
    case D9C_COMMAND_RECORD_APPLY_STATE:
      ++shape.applyStateRecords;
      break;
    case D9C_COMMAND_RECORD_CLEAR:
      ++shape.clearRecords;
      break;
    case D9C_COMMAND_RECORD_PRESENT:
      ++shape.presentRecords;
      break;
    case D9C_COMMAND_RECORD_STRETCH_RECT:
    case D9C_COMMAND_RECORD_COLOR_FILL:
    case D9C_COMMAND_RECORD_UPDATE_TEXTURE:
    case D9C_COMMAND_RECORD_UPDATE_SURFACE:
    case D9C_COMMAND_RECORD_READBACK:
    case D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE:
      ++shape.surfaceRecords;
      break;
    case D9C_COMMAND_RECORD_QUERY_ISSUE:
      ++shape.queryRecords;
      break;
    default:
      ++shape.otherRecords;
      break;
    }
    recordIndex = recordView->nextIndex();
  }
  return shape;
}

void (*commitChunkReplayRecordDetailCounter(std::uint32_t type))(std::uint64_t) {
  switch (type) {
  case D9C_COMMAND_RECORD_SET_VS_CONST_F:
  case D9C_COMMAND_RECORD_SET_VS_CONST_I:
  case D9C_COMMAND_RECORD_SET_VS_CONST_B:
  case D9C_COMMAND_RECORD_SET_PS_CONST_F:
  case D9C_COMMAND_RECORD_SET_PS_CONST_I:
  case D9C_COMMAND_RECORD_SET_PS_CONST_B:
    return dxmt9::perf::countCommitChunkReplayConstRecordCpuTime;
  case D9C_COMMAND_RECORD_APPLY_STATE:
    return dxmt9::perf::countCommitChunkReplayApplyStateRecordCpuTime;
  case D9C_COMMAND_RECORD_CLEAR:
    return dxmt9::perf::countCommitChunkReplayClearRecordCpuTime;
  case D9C_COMMAND_RECORD_PRESENT:
    return dxmt9::perf::countCommitChunkReplayPresentRecordCpuTime;
  case D9C_COMMAND_RECORD_STRETCH_RECT:
  case D9C_COMMAND_RECORD_COLOR_FILL:
  case D9C_COMMAND_RECORD_UPDATE_TEXTURE:
  case D9C_COMMAND_RECORD_UPDATE_SURFACE:
  case D9C_COMMAND_RECORD_READBACK:
  case D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE:
    return dxmt9::perf::countCommitChunkReplaySurfaceRecordCpuTime;
  case D9C_COMMAND_RECORD_QUERY_ISSUE:
    return dxmt9::perf::countCommitChunkReplayQueryRecordCpuTime;
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP:
    return nullptr;
  default:
    return dxmt9::perf::countCommitChunkReplayOtherRecordCpuTime;
  }
}

struct ConstantUploadRecordSize {
  std::uint64_t payloadBytes = 0;
  std::uint32_t registerCount = 0;
};

ConstantUploadRecordSize constantUploadRecordSize(
    const ImportedRecordView& record) {
  if (!record.valid() || !record.record ||
      !commitChunkRecordIsConstantUpload(record.header.type) ||
      record.header.size < sizeof(D9CCommandRecordSetConst)) {
    return {};
  }
  D9CCommandRecordSetConst decoded{};
  std::memcpy(&decoded, record.record, sizeof(decoded));
  const auto availableBytes =
      static_cast<std::uint64_t>(record.header.size - sizeof(decoded));
  std::uint64_t expectedBytes = 0;
  switch (record.header.type) {
  case D9C_COMMAND_RECORD_SET_VS_CONST_F:
  case D9C_COMMAND_RECORD_SET_VS_CONST_I:
  case D9C_COMMAND_RECORD_SET_PS_CONST_F:
  case D9C_COMMAND_RECORD_SET_PS_CONST_I:
    expectedBytes =
        static_cast<std::uint64_t>(decoded.count) * 4u * sizeof(std::uint32_t);
    break;
  case D9C_COMMAND_RECORD_SET_VS_CONST_B:
  case D9C_COMMAND_RECORD_SET_PS_CONST_B:
    expectedBytes = static_cast<std::uint64_t>(decoded.count) * sizeof(std::uint32_t);
    break;
  default:
    break;
  }
  return ConstantUploadRecordSize{
      .payloadBytes = std::min(availableBytes, expectedBytes),
      .registerCount = decoded.count,
  };
}

std::uint32_t commitChunkDrawDeltaMask(const D9CDrawPrimitivePacket& packet) {
  std::uint32_t mask = 0;
  using namespace dxmt9::perf;
  if (packet.renderStateCount != 0) mask |= CommitChunkDrawDeltaRenderState;
  if (packet.textureMask != 0) mask |= CommitChunkDrawDeltaTexture;
  if (packet.streamSourceMask != 0) mask |= CommitChunkDrawDeltaStream;
  if (packet.fvfValid != 0) mask |= CommitChunkDrawDeltaFvf;
  if (packet.vsValid != 0 || packet.psValid != 0) mask |= CommitChunkDrawDeltaShader;
  if (packet.vdeclValid != 0) mask |= CommitChunkDrawDeltaVertexDecl;
  if (packet.rtMask != 0) mask |= CommitChunkDrawDeltaRenderTarget;
  if (packet.dsValid != 0) mask |= CommitChunkDrawDeltaDepthStencil;
  if (packet.viewportValid != 0) mask |= CommitChunkDrawDeltaViewport;
  if (packet.scissorValid != 0) mask |= CommitChunkDrawDeltaScissor;
  if (packet.tssCount != 0) mask |= CommitChunkDrawDeltaTextureStageState;
  if (packet.samplerStateCount != 0) mask |= CommitChunkDrawDeltaSamplerState;
  if (packet.materialValid != 0) mask |= CommitChunkDrawDeltaMaterial;
  if (packet.clipPlaneMask != 0) mask |= CommitChunkDrawDeltaClipPlane;
  if (packet.transformCount != 0) mask |= CommitChunkDrawDeltaTransform;
  if (packet.lightSlotMask != 0) mask |= CommitChunkDrawDeltaLight;
  if (packet.lightEnableValidMask != 0) mask |= CommitChunkDrawDeltaLightEnable;
  return mask;
}

std::uint32_t commitChunkDrawDeltaMask(const D9CDrawIndexedPrimitivePacket& packet) {
  auto mask = commitChunkDrawDeltaMask(packet.state);
  if (packet.ibValid != 0) {
    mask |= dxmt9::perf::CommitChunkDrawDeltaIndexBuffer;
  }
  return mask;
}

std::uint32_t drawStateInvalidationReasonFromCommitDeltaMask(std::uint32_t deltaMask) {
  using namespace dxmt9::core;
  using namespace dxmt9::perf;
  std::uint32_t reason = DrawStateInvalidationDrawPacket;
  if ((deltaMask & CommitChunkDrawDeltaRenderState) != 0) {
    reason |= DrawStateInvalidationRenderState;
  }
  if ((deltaMask & CommitChunkDrawDeltaTexture) != 0) {
    reason |= DrawStateInvalidationTexture;
  }
  if ((deltaMask & CommitChunkDrawDeltaStream) != 0) {
    reason |= DrawStateInvalidationStream;
  }
  if ((deltaMask & (CommitChunkDrawDeltaFvf |
                    CommitChunkDrawDeltaVertexDecl)) != 0) {
    reason |= DrawStateInvalidationFvfVdecl;
  }
  if ((deltaMask & CommitChunkDrawDeltaShader) != 0) {
    reason |= DrawStateInvalidationShader;
  }
  if ((deltaMask & (CommitChunkDrawDeltaRenderTarget |
                    CommitChunkDrawDeltaDepthStencil)) != 0) {
    reason |= DrawStateInvalidationRenderTargetDepth;
  }
  if ((deltaMask & (CommitChunkDrawDeltaViewport |
                    CommitChunkDrawDeltaScissor)) != 0) {
    reason |= DrawStateInvalidationViewportScissor;
  }
  if ((deltaMask & (CommitChunkDrawDeltaTextureStageState |
                    CommitChunkDrawDeltaSamplerState)) != 0) {
    reason |= DrawStateInvalidationTextureStageSampler;
  }
  if ((deltaMask & CommitChunkDrawDeltaTextureStageState) != 0) {
    reason |= DrawStateInvalidationTextureStageState;
  }
  if ((deltaMask & CommitChunkDrawDeltaSamplerState) != 0) {
    reason |= DrawStateInvalidationSamplerState;
  }
  if ((deltaMask & (CommitChunkDrawDeltaMaterial |
                    CommitChunkDrawDeltaTransform |
                    CommitChunkDrawDeltaLight |
                    CommitChunkDrawDeltaLightEnable)) != 0) {
    reason |= DrawStateInvalidationFfpState;
  }
  if ((deltaMask & CommitChunkDrawDeltaClipPlane) != 0) {
    reason |= DrawStateInvalidationClipPlane;
  }
  if ((deltaMask & CommitChunkDrawDeltaIndexBuffer) != 0) {
    reason |= DrawStateInvalidationIndexBuffer;
  }
  return reason;
}

void countCommitChunkDrawStreamDeltaDetails(
    D9CDevice* d, const D9CDrawPrimitivePacket& packet) {
  if (!d || packet.streamSourceMask == 0) {
    return;
  }
  const auto& state = d->dev().state();
  std::uint32_t handleChanges = 0;
  std::uint32_t offsetChanges = 0;
  std::uint32_t strideChanges = 0;
  for (uint32_t stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
    if ((packet.streamSourceMask & (1u << stream)) == 0) {
      continue;
    }
    const auto& source = packet.streamSources[stream];
    if (bufferObjectHandleValue(state.streamBuffers[stream]) !=
        wireBufferObjectHandleValue(source.buffer)) {
      ++handleChanges;
    }
    if (state.streamOffsets[stream] != source.offset) {
      ++offsetChanges;
    }
    if (state.streamStrides[stream] != source.stride) {
      ++strideChanges;
    }
  }
  dxmt9::perf::countCommitChunkDrawStreamDeltaDetails(
      handleChanges, offsetChanges, strideChanges);
}

void countCommitChunkDrawReplay(D9CDevice* d,
                                const D9CDrawPrimitivePacket& packet) {
  dxmt9::perf::countCommitChunkDrawReplay(
      /*indexed=*/false, commitChunkDrawDeltaMask(packet));
  countCommitChunkDrawStreamDeltaDetails(d, packet);
}

void countCommitChunkDrawReplay(D9CDevice* d,
                                const D9CDrawIndexedPrimitivePacket& packet) {
  dxmt9::perf::countCommitChunkDrawReplay(
      /*indexed=*/true, commitChunkDrawDeltaMask(packet));
  countCommitChunkDrawStreamDeltaDetails(d, packet.state);
  if (d && packet.ibValid) {
    const auto& state = d->dev().state();
    if (bufferObjectHandleValue(state.indexBuffer) !=
        wireBufferObjectHandleValue(packet.ibHandle)) {
      dxmt9::perf::countCommitChunkDrawIndexBufferHandleDelta();
    }
  }
}

void countCommitChunkDrawRunScan(const ImportedDrawRunScan& scan) {
  const auto constSize = constantUploadRecordSize(scan.stopRecord);
  dxmt9::perf::countCommitChunkDrawRunScan(
      static_cast<std::uint32_t>(scan.stop),
      scan.recordCount,
      scan.stopRecord.header.type,
      constSize.payloadBytes,
      constSize.registerCount);
  if (scan.replayAsRun() ||
      scan.stop != ImportedDrawRunScanStop::StateDelta ||
      !scan.stopRecord.valid()) {
    return;
  }
  switch (scan.stopRecord.header.type) {
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE: {
      D9CCommandRecordDrawPrimitive decoded{};
      std::memcpy(&decoded, scan.stopRecord.record, sizeof(decoded));
      dxmt9::perf::countCommitChunkDrawRunStateDeltaBucket(
          commitChunkDrawDeltaMask(decoded.packet));
      break;
    }
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE: {
      D9CCommandRecordDrawIndexedPrimitive decoded{};
      std::memcpy(&decoded, scan.stopRecord.record, sizeof(decoded));
      dxmt9::perf::countCommitChunkDrawRunStateDeltaBucket(
          commitChunkDrawDeltaMask(decoded.packet));
      break;
    }
    default:
      break;
  }
}

struct EffectiveDrawBindings {
  std::array<dxmt9::core::Handle, dxmt9::core::kMaxStreams> streamHandles{};
  std::array<D9CBuffer*, dxmt9::core::kMaxStreams> streamBuffers{};
  std::array<std::uint32_t, dxmt9::core::kMaxStreams> streamOffsets{};
  std::array<std::uint32_t, dxmt9::core::kMaxStreams> streamStrides{};
  dxmt9::core::Handle indexHandle{};
  D9CBuffer* indexBuffer = nullptr;
  dxmt9::core::IndexType indexType = dxmt9::core::IndexType::UInt16;
};

dxmt9::core::Handle bufferHandle(const D9CBuffer* buffer) {
  return buffer && buffer->obj ? buffer->obj->handle() : dxmt9::core::Handle{};
}

EffectiveDrawBindings effectiveBindingsFromState(const dxmt9::core::DeviceState& state) {
  EffectiveDrawBindings bindings{};
  for (std::uint32_t stream = 0; stream < dxmt9::core::kMaxStreams; ++stream) {
    bindings.streamHandles[stream] =
        state.streamBuffers[stream] ? state.streamBuffers[stream]->handle()
                                    : dxmt9::core::Handle{};
    bindings.streamOffsets[stream] = state.streamOffsets[stream];
    bindings.streamStrides[stream] = state.streamStrides[stream];
  }
  bindings.indexHandle = state.indexBuffer ? state.indexBuffer->handle()
                                           : dxmt9::core::Handle{};
  bindings.indexType = state.indexType;
  return bindings;
}

void applyStreamBindingDeltas(EffectiveDrawBindings& bindings,
                              const D9CDrawPrimitivePacket& packet) {
  for (std::uint32_t stream = 0;
       stream < std::min<std::uint32_t>(D9C_DRAW_PACKET_MAX_STREAMS,
                                        dxmt9::core::kMaxStreams);
       ++stream) {
    if ((packet.streamSourceMask & (1u << stream)) == 0) {
      continue;
    }
    const auto& source = packet.streamSources[stream];
    auto* buffer = wireHandlePtr<D9CBuffer>(source.buffer);
    bindings.streamBuffers[stream] = buffer;
    bindings.streamHandles[stream] = bufferHandle(buffer);
    bindings.streamOffsets[stream] = source.offset;
    bindings.streamStrides[stream] = source.stride;
  }
}

void applyIndexBindingDelta(EffectiveDrawBindings& bindings,
                            const D9CDrawIndexedPrimitivePacket& packet) {
  if (!packet.ibValid) {
    return;
  }
  auto* buffer = wireHandlePtr<D9CBuffer>(packet.ibHandle);
  bindings.indexBuffer = buffer;
  bindings.indexHandle = bufferHandle(buffer);
  bindings.indexType =
      buffer ? idxTypeFromD3D(buffer->desc.format) : dxmt9::core::IndexType::UInt16;
}

bool makeBindingOverride(const EffectiveDrawBindings& base,
                         const EffectiveDrawBindings& effective,
                         dxmt9::core::DrawBindingOverride& out) {
  out = {};
  bool changed = false;
  for (std::uint32_t stream = 0; stream < dxmt9::core::kMaxStreams; ++stream) {
    if (base.streamHandles[stream] == effective.streamHandles[stream] &&
        base.streamOffsets[stream] == effective.streamOffsets[stream] &&
        base.streamStrides[stream] == effective.streamStrides[stream]) {
      continue;
    }
    out.streamMask |= 1u << stream;
    out.streams[stream].buffer = effective.streamHandles[stream];
    out.streams[stream].offset = effective.streamOffsets[stream];
    out.streams[stream].stride = effective.streamStrides[stream];
    changed = true;
  }
  if (base.indexHandle != effective.indexHandle ||
      base.indexType != effective.indexType) {
    out.indexBuffer = effective.indexHandle;
    out.indexType = effective.indexType;
    out.indexBufferValid = true;
    changed = true;
  }
  return changed;
}

dxmt9::core::DrawParamPayloadView bindingOverridePayloadView(
    const dxmt9::core::DrawBindingOverride& override) {
  return {
      .bindingOverrideData =
          std::span<const dxmt9::core::u8>(
              reinterpret_cast<const dxmt9::core::u8*>(&override),
              sizeof(dxmt9::core::DrawBindingOverride)),
  };
}

int32_t applyFinalBindingState(D9CDevice* d,
                               const EffectiveDrawBindings& base,
                               const EffectiveDrawBindings& effective) {
  for (std::uint32_t stream = 0; stream < dxmt9::core::kMaxStreams; ++stream) {
    if (base.streamHandles[stream] == effective.streamHandles[stream] &&
        base.streamOffsets[stream] == effective.streamOffsets[stream] &&
        base.streamStrides[stream] == effective.streamStrides[stream]) {
      continue;
    }
    const int32_t hr = dxmt9c_device_set_stream_source(
        d, stream, effective.streamBuffers[stream], effective.streamOffsets[stream],
        effective.streamStrides[stream]);
    if (failed(hr)) {
      return hr;
    }
  }
  if (base.indexHandle != effective.indexHandle ||
      base.indexType != effective.indexType) {
    const auto indexBindStart = std::chrono::steady_clock::now();
    const int32_t hr = dxmt9c_device_set_indices(d, effective.indexBuffer);
    countDurationSince(indexBindStart,
                       dxmt9::perf::countCommitChunkIndexBindCpuTime);
    if (failed(hr)) {
      return hr;
    }
  }
  return dxmt9::core::D3D_OK;
}

void recordStateBlockRenderState(D9CDevice* d, uint32_t state, uint32_t value) {
  d->stateBlockRenderStates.insert(state);
  d->stateBlockRenderStateValues[state] = value;
}

dxmt9::core::Matrix4x4 matrixFromC(const D9CMatrix& m) {
  dxmt9::core::Matrix4x4 matrix;
  std::memcpy(matrix.m.data(), m.m, 16 * sizeof(float));
  return matrix;
}

dxmt9::core::Viewport viewportFromC(const D9CViewport& vp) {
  return {vp.x, vp.y, vp.width, vp.height, vp.minZ, vp.maxZ};
}

bool viewportValid(const dxmt9::core::Viewport& viewport) {
  return viewport.width != 0 && viewport.height != 0 &&
         std::isfinite(viewport.minZ) && std::isfinite(viewport.maxZ) &&
         viewport.minZ >= 0.0f && viewport.maxZ <= 1.0f &&
         viewport.minZ <= viewport.maxZ;
}

dxmt9::core::Rect rectFromC(const D9CRect& r) {
  return {r.left, r.top, r.right, r.bottom};
}

dxmt9::core::Material materialFromC(const D9CMaterial& m) {
  dxmt9::core::Material material;
  std::memcpy(&material.diffuse, &m.diffuse, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&material.ambient, &m.ambient, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&material.specular, &m.specular, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&material.emissive, &m.emissive, sizeof(dxmt9::core::ColorRGBA));
  material.power = m.power;
  return material;
}

dxmt9::core::Light lightFromC(const D9CLight& l) {
  dxmt9::core::Light light;
  light.type = static_cast<dxmt9::core::LightType>(l.type);
  std::memcpy(&light.diffuse, &l.diffuse, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&light.specular, &l.specular, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(&light.ambient, &l.ambient, sizeof(dxmt9::core::ColorRGBA));
  std::memcpy(light.position.data(), l.position, 3 * sizeof(float));
  std::memcpy(light.direction.data(), l.direction, 3 * sizeof(float));
  light.range = l.range;
  light.falloff = l.falloff;
  light.attenuation0 = l.attenuation0;
  light.attenuation1 = l.attenuation1;
  light.attenuation2 = l.attenuation2;
  light.theta = l.theta;
  light.phi = l.phi;
  return light;
}

dxmt9::core::RenderTargetAttachment attachmentFromSurface(
    const std::shared_ptr<dxmt9::core::Surface>& surface) {
  return surface ? dxmt9::core::RenderTargetAttachment{
                       surface->handle(), surface->level(), surface->multiSampleCount()}
                 : dxmt9::core::RenderTargetAttachment{};
}

bool drawPacketActualChangePerfEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_DRAW_PACKET_ACTUAL_CHANGE");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

std::uint32_t drawPacketActualChangeMask(
    const dxmt9::core::DeviceState& state,
    const D9CDrawPrimitivePacket& packet) {
  std::uint32_t mask = 0;
  using namespace dxmt9::perf;

  auto addIf = [&](bool changed, std::uint32_t bit) {
    if (changed) {
      mask |= bit;
    }
  };

  for (uint32_t i = 0; i < packet.renderStateCount; ++i) {
    const auto& entry = packet.renderStates[i];
    bool changed = false;
    if (dxmt9::core::RenderStateTable::validKey(entry.state)) {
      changed = state.renderStates.valueOr(entry.state, 0u) != entry.value;
    }
    if (entry.state == dxmt9::core::RS_SCISSOR_TEST_ENABLE &&
        state.scissorEnabled != (entry.value != 0)) {
      changed = true;
    }
    addIf(changed, CommitChunkDrawDeltaRenderState);
  }

  for (uint32_t stage = 0;
       stage < std::min<std::uint32_t>(D9C_DRAW_PACKET_MAX_TEXTURES,
                                       dxmt9::core::kMaxTextures);
       ++stage) {
    if ((packet.textureMask & (1u << stage)) == 0) {
      continue;
    }
    auto* texture = wireHandlePtr<D9CTexture>(packet.textures[stage]);
    addIf(state.textures[stage] != (texture ? texture->obj : nullptr),
          CommitChunkDrawDeltaTexture);
  }

  for (uint32_t stream = 0;
       stream < std::min<std::uint32_t>(D9C_DRAW_PACKET_MAX_STREAMS,
                                        dxmt9::core::kMaxStreams);
       ++stream) {
    if ((packet.streamSourceMask & (1u << stream)) == 0) {
      continue;
    }
    const auto& source = packet.streamSources[stream];
    addIf(bufferObjectHandleValue(state.streamBuffers[stream]) !=
              wireBufferObjectHandleValue(source.buffer) ||
          state.streamOffsets[stream] != source.offset ||
          state.streamStrides[stream] != source.stride,
          CommitChunkDrawDeltaStream);
  }

  if (packet.fvfValid) {
    addIf(state.fvf != packet.fvf ||
          state.vertexDecl.fvf != packet.fvf ||
          !state.vertexDecl.elements.empty(),
          CommitChunkDrawDeltaFvf);
  }

  if (packet.vsValid) {
    auto* vs = wireHandlePtr<D9CShader>(packet.vsHandle);
    addIf(state.vertexShader != (vs ? vs->ref : dxmt9::core::ShaderRef{}),
          CommitChunkDrawDeltaShader);
  }
  if (packet.psValid) {
    auto* ps = wireHandlePtr<D9CShader>(packet.psHandle);
    addIf(state.pixelShader != (ps ? ps->ref : dxmt9::core::ShaderRef{}),
          CommitChunkDrawDeltaShader);
  }

  if (packet.vdeclValid) {
    auto* vd = wireHandlePtr<D9CVertexDecl>(packet.vdeclHandle);
    const bool elementsChanged = vd ? state.vertexDecl.elements != vd->elements
                                    : !state.vertexDecl.elements.empty();
    addIf(elementsChanged || state.vertexDecl.fvf != state.fvf,
          CommitChunkDrawDeltaVertexDecl);
  }

  for (uint32_t slot = 0;
       slot < std::min<std::uint32_t>(D9C_DRAW_PACKET_MAX_RENDER_TARGETS,
                                      dxmt9::core::kMaxRenderTargets);
       ++slot) {
    if ((packet.rtMask & (1u << slot)) == 0) {
      continue;
    }
    auto* rt = wireHandlePtr<D9CSurface>(packet.rtHandles[slot]);
    const auto surface = rt ? rt->obj : nullptr;
    bool changed = state.renderTargets[slot] != attachmentFromSurface(surface);
    if (slot == 0 && surface) {
      const auto& desc = surface->desc();
      const auto width = std::max(1u, desc.width);
      const auto height = std::max(1u, desc.height);
      const dxmt9::core::Viewport viewport{0, 0, width, height, 0.0f, 1.0f};
      const dxmt9::core::Rect scissor{0, 0, static_cast<int32_t>(width),
                                      static_cast<int32_t>(height)};
      changed = changed || state.viewport != viewport || state.scissorRect != scissor;
    }
    addIf(changed, CommitChunkDrawDeltaRenderTarget);
  }

  if (packet.dsValid) {
    auto* ds = wireHandlePtr<D9CSurface>(packet.dsHandle);
    addIf(state.depthStencil != attachmentFromSurface(ds ? ds->obj : nullptr),
          CommitChunkDrawDeltaDepthStencil);
  }

  if (packet.viewportValid) {
    addIf(state.viewport != viewportFromC(packet.viewport),
          CommitChunkDrawDeltaViewport);
  }
  if (packet.scissorValid) {
    addIf(state.scissorRect != rectFromC(packet.scissor),
          CommitChunkDrawDeltaScissor);
  }

  for (uint32_t i = 0; i < packet.tssCount; ++i) {
    const auto& e = packet.tss[i];
    const uint32_t stage = std::min(e.stage, dxmt9::core::kMaxTextureStages - 1);
    const uint32_t key = std::min(e.type, dxmt9::core::kMaxTextureStageStates - 1);
    addIf(state.textureStageStates[stage].valueOr(key, 0u) != e.value,
          CommitChunkDrawDeltaTextureStageState);
  }
  for (uint32_t i = 0; i < packet.samplerStateCount; ++i) {
    const auto& e = packet.samplerStates[i];
    const bool valid =
        e.sampler < dxmt9::core::kMaxSamplers &&
        dxmt9::core::SamplerStateTable::validKey(e.type);
    addIf(valid && state.samplerStates[e.sampler].valueOr(e.type, 0u) != e.value,
          CommitChunkDrawDeltaSamplerState);
  }

  if (packet.materialValid) {
    addIf(state.material != materialFromC(packet.material),
          CommitChunkDrawDeltaMaterial);
  }

  for (uint32_t i = 0; i < dxmt9::core::kMaxClipPlanes; ++i) {
    if ((packet.clipPlaneMask & (1u << i)) == 0) {
      continue;
    }
    const dxmt9::core::ClipPlane plane{packet.clipPlanes[i * 4 + 0],
                                       packet.clipPlanes[i * 4 + 1],
                                       packet.clipPlanes[i * 4 + 2],
                                       packet.clipPlanes[i * 4 + 3]};
    addIf(state.clipPlanes[i] != plane, CommitChunkDrawDeltaClipPlane);
  }

  for (uint32_t i = 0; i < packet.transformCount; ++i) {
    const auto& e = packet.transforms[i];
    const auto key = transformStateFromD3D(e.state);
    const auto matrix = matrixFromC(e.matrix);
    addIf(dxmt9::core::TransformTable::validKey(key) &&
              state.transforms.valueOr(key, dxmt9::core::Matrix4x4{}) != matrix,
          CommitChunkDrawDeltaTransform);
  }

  for (uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_LIGHTS; ++i) {
    if ((packet.lightSlotMask & (1u << i)) == 0) {
      continue;
    }
    addIf(i < dxmt9::core::kMaxLights &&
              state.lights[i] != lightFromC(packet.lights[i]),
          CommitChunkDrawDeltaLight);
  }
  for (uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_LIGHTS; ++i) {
    if ((packet.lightEnableValidMask & (1u << i)) == 0) {
      continue;
    }
    const bool enabled = (packet.lightEnableMask & (1u << i)) != 0;
    addIf(i < dxmt9::core::kMaxLights &&
              (state.lightEnabled[i] != enabled ||
               state.lights[i].enabled != enabled),
          CommitChunkDrawDeltaLightEnable);
  }

  return mask;
}

int32_t validateDrawPacketStateDelta(const D9CDrawPrimitivePacket& packet) {
  if (packet.renderStateCount > D9C_DRAW_PACKET_MAX_RENDER_STATES ||
      packet.tssCount > D9C_DRAW_PACKET_MAX_TSS ||
      packet.samplerStateCount > D9C_DRAW_PACKET_MAX_SAMPLER ||
      packet.transformCount > D9C_DRAW_PACKET_MAX_TRANSFORMS) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  for (uint32_t i = 0; i < packet.samplerStateCount; ++i) {
    if (packet.samplerStates[i].sampler >= dxmt9::core::kMaxSamplers) {
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
  }
  if (packet.viewportValid && !viewportValid(viewportFromC(packet.viewport))) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  return dxmt9::core::D3D_OK;
}

int32_t commitChunkFail(const char* reason,
                        std::uint32_t index = 0xffffffffu,
                        std::uint32_t type = 0,
                        int32_t hr = dxmt9::core::D3DERR_INVALIDCALL) {
  dxmt9DebugLog("commit_chunk fail reason=%s index=%u type=%u hr=0x%08x",
                reason, index, type, static_cast<std::uint32_t>(hr));
  dxmt9::util::logf(dxmt9::util::LogLevel::Info, "dxmt9-device",
                    "commit_chunk_fail reason=%s index=%u type=%u hr=0x%08x",
                    reason ? reason : "unknown", index, type,
                    static_cast<std::uint32_t>(hr));
  // R-BACK-2.10: every commit_chunk reject path funnels through here.
  dxmt9::perf::countChunkReject();
  return hr;
}

int32_t applyDrawPacketStateViaIface(D9CDevice* d, const D9CDrawPrimitivePacket& packet) {
  if (packet.renderStateCount > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }

  for (uint32_t i = 0; i < packet.renderStateCount; ++i) {
    const auto& state = packet.renderStates[i];
    if (d->stateBlockRecording) {
      recordStateBlockRenderState(d, state.state, state.value);
      continue;
    }
    const int32_t hr = d->iface->SetRenderState(state.state, state.value);
    if (failed(hr)) {
      return hr;
    }
  }

  for (uint32_t stage = 0; stage < D9C_DRAW_PACKET_MAX_TEXTURES; ++stage) {
    if ((packet.textureMask & (1u << stage)) == 0) {
      continue;
    }
    auto texture = wireHandlePtr<D9CTexture>(packet.textures[stage]);
    const int32_t hr = d->iface->SetTexture(stage, texture ? texture->obj : nullptr);
    if (failed(hr)) {
      return hr;
    }
  }

  for (uint32_t stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
    if ((packet.streamSourceMask & (1u << stream)) == 0) {
      continue;
    }
    const auto& source = packet.streamSources[stream];
    auto buffer = wireHandlePtr<D9CBuffer>(source.buffer);
    const int32_t hr = d->iface->SetStreamSource(stream, buffer ? buffer->obj : nullptr,
                                                 source.offset, source.stride);
    if (failed(hr)) {
      return hr;
    }
  }

  if (packet.fvfValid) {
    const int32_t hr = d->iface->SetFVF(packet.fvf);
    if (failed(hr)) {
      return hr;
    }
  }

  // Phase 12: shader-handle delta dispatch. Wire handles are server-side
  // D9CShader* casts (rawVS/rawPS); decode + call dxmt9c_device_set_*_shader.
  if (packet.vsValid) {
    auto* vs = wireHandlePtr<D9CShader>(packet.vsHandle);
    const int32_t hr = dxmt9c_device_set_vertex_shader(d, vs);
    if (failed(hr)) return hr;
  }
  if (packet.psValid) {
    auto* ps = wireHandlePtr<D9CShader>(packet.psHandle);
    const int32_t hr = dxmt9c_device_set_pixel_shader(d, ps);
    if (failed(hr)) return hr;
  }

  // Phase 12: vertex-decl handle delta.
  if (packet.vdeclValid) {
    auto* vd = wireHandlePtr<D9CVertexDecl>(packet.vdeclHandle);
    const int32_t hr = dxmt9c_device_set_vertex_declaration(d, vd);
    if (failed(hr)) return hr;
  }

  // Phase 12: render-target deltas. One dispatch per set bit in rtMask.
  for (uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_RENDER_TARGETS; ++slot) {
    if ((packet.rtMask & (1u << slot)) == 0) continue;
    auto* rt = wireHandlePtr<D9CSurface>(packet.rtHandles[slot]);
    const int32_t hr = dxmt9c_device_set_render_target(d, slot, rt);
    if (failed(hr)) return hr;
  }

  // Phase 12: depth-stencil delta.
  if (packet.dsValid) {
    auto* ds = wireHandlePtr<D9CSurface>(packet.dsHandle);
    const int32_t hr = dxmt9c_device_set_depth_stencil(d, ds);
    if (failed(hr)) return hr;
  }

  // Phase 12: viewport / scissor deltas.
  if (packet.viewportValid) {
    const int32_t hr = dxmt9c_device_set_viewport(d, &packet.viewport);
    if (failed(hr)) return hr;
  }
  if (packet.scissorValid) {
    const int32_t hr = dxmt9c_device_set_scissor_rect(d, &packet.scissor);
    if (failed(hr)) return hr;
  }

  // Phase 12: TSS / SamplerState delta dispatch.
  if (packet.tssCount > D9C_DRAW_PACKET_MAX_TSS ||
      packet.samplerStateCount > D9C_DRAW_PACKET_MAX_SAMPLER) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  for (uint32_t i = 0; i < packet.tssCount; ++i) {
    const auto& e = packet.tss[i];
    const int32_t hr = dxmt9c_device_set_texture_stage_state(d, e.stage, e.type, e.value);
    if (failed(hr)) return hr;
  }
  for (uint32_t i = 0; i < packet.samplerStateCount; ++i) {
    const auto& e = packet.samplerStates[i];
    const int32_t hr = dxmt9c_device_set_sampler_state(d, e.sampler, e.type, e.value);
    if (failed(hr)) return hr;
  }

  // Phase 12: material delta.
  if (packet.materialValid) {
    const int32_t hr = dxmt9c_device_set_material(d, &packet.material);
    if (failed(hr)) return hr;
  }

  // Phase 12: clip-plane delta — one dispatch per set bit in
  // clipPlaneMask. Each plane is 4 floats stored contiguously at
  // clipPlanes[i*4..i*4+3].
  for (uint32_t i = 0; i < 6; ++i) {
    if ((packet.clipPlaneMask & (1u << i)) == 0) continue;
    const int32_t hr = dxmt9c_device_set_clip_plane(d, i, &packet.clipPlanes[i * 4]);
    if (failed(hr)) return hr;
  }

  // Phase 12: Transform delta — packet.transforms[0..transformCount).
  if (packet.transformCount > D9C_DRAW_PACKET_MAX_TRANSFORMS) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  for (uint32_t i = 0; i < packet.transformCount; ++i) {
    const auto& e = packet.transforms[i];
    const int32_t hr = dxmt9c_device_set_transform(d, e.state, &e.matrix);
    if (failed(hr)) return hr;
  }

  // Phase 12: Light delta — one dispatch per set bit in lightSlotMask.
  for (uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_LIGHTS; ++i) {
    if ((packet.lightSlotMask & (1u << i)) == 0) continue;
    const int32_t hr = dxmt9c_device_set_light(d, i, &packet.lights[i]);
    if (failed(hr)) return hr;
  }

  // Phase 12: LightEnable delta. ValidMask says which slots have a fresh
  // value; lightEnableMask carries the new enabled bit per slot.
  for (uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_LIGHTS; ++i) {
    if ((packet.lightEnableValidMask & (1u << i)) == 0) continue;
    const uint32_t enabled = (packet.lightEnableMask & (1u << i)) ? 1u : 0u;
    const int32_t hr = dxmt9c_device_light_enable(d, i, enabled);
    if (failed(hr)) return hr;
  }

  return dxmt9::core::D3D_OK;
}

// Normal chunk replay path: apply the packet's flat delta directly to the
// core DeviceState and invalidate derived draw-state caches once. The iface
// replay above remains only for state-block recording semantics.
//
// This routine is mode-agnostic: it observes valid/mask bits and applies
// whatever is set. A delta packet sets only what changed since the last
// draw; a full-snapshot packet (DXMT9_PE_DRAW_FULL_SNAPSHOT=1, produced
// in d3d9_pe_device.cpp::buildDrawPrimitivePacket) sets every field from
// the PE shadow. Both yield the same DeviceState — proved by
// tests/native/bridge/pe_full_snapshot_equivalence_spec.cpp.
int32_t applyDrawPacketStateDirect(D9CDevice* d, const D9CDrawPrimitivePacket& packet) {
  const int32_t validationHr = validateDrawPacketStateDelta(packet);
  if (failed(validationHr)) {
    return validationHr;
  }

  if (packetHasNoStateDelta(packet)) {
    return dxmt9::core::D3D_OK;
  }

  const std::uint32_t deltaMask = commitChunkDrawDeltaMask(packet);
  if (drawPacketActualChangePerfEnabled()) {
    dxmt9::perf::countDrawPacketActualChange(
        deltaMask, drawPacketActualChangeMask(d->dev().state(), packet));
  }

  auto& state = d->dev().mutableState(
      drawStateInvalidationReasonFromCommitDeltaMask(deltaMask));

  for (uint32_t i = 0; i < packet.renderStateCount; ++i) {
    const auto& entry = packet.renderStates[i];
    state.renderStates.set(entry.state, entry.value);
    if (entry.state == dxmt9::core::RS_SCISSOR_TEST_ENABLE) {
      state.scissorEnabled = entry.value != 0;
    }
  }

  for (uint32_t stage = 0; stage < D9C_DRAW_PACKET_MAX_TEXTURES; ++stage) {
    if ((packet.textureMask & (1u << stage)) == 0) {
      continue;
    }
    auto* texture = wireHandlePtr<D9CTexture>(packet.textures[stage]);
    state.textures[stage] = texture ? texture->obj : nullptr;
  }

  for (uint32_t stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
    if ((packet.streamSourceMask & (1u << stream)) == 0) {
      continue;
    }
    const auto& source = packet.streamSources[stream];
    auto* buffer = wireHandlePtr<D9CBuffer>(source.buffer);
    state.streamBuffers[stream] = buffer ? buffer->obj : nullptr;
    state.streamOffsets[stream] = source.offset;
    state.streamStrides[stream] = source.stride;
  }

  if (packet.fvfValid) {
    state.fvf = packet.fvf;
    state.vertexDecl.fvf = packet.fvf;
    state.vertexDecl.elements.clear();
  }

  if (packet.vsValid) {
    auto* vs = wireHandlePtr<D9CShader>(packet.vsHandle);
    state.vertexShader = vs ? vs->ref : dxmt9::core::ShaderRef{};
  }
  if (packet.psValid) {
    auto* ps = wireHandlePtr<D9CShader>(packet.psHandle);
    state.pixelShader = ps ? ps->ref : dxmt9::core::ShaderRef{};
  }

  if (packet.vdeclValid) {
    auto* vd = wireHandlePtr<D9CVertexDecl>(packet.vdeclHandle);
    if (vd) {
      state.vertexDecl.elements = vd->elements;
    } else {
      state.vertexDecl.elements.clear();
    }
    state.vertexDecl.fvf = state.fvf;
  }

  for (uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_RENDER_TARGETS; ++slot) {
    if ((packet.rtMask & (1u << slot)) == 0) {
      continue;
    }
    auto* rt = wireHandlePtr<D9CSurface>(packet.rtHandles[slot]);
    const auto surface = rt ? rt->obj : nullptr;
    state.renderTargets[slot] = attachmentFromSurface(surface);
    if (slot == 0 && surface) {
      const auto& desc = surface->desc();
      const auto width = std::max(1u, desc.width);
      const auto height = std::max(1u, desc.height);
      state.viewport = {0, 0, width, height, 0.0f, 1.0f};
      state.scissorRect = {0, 0, static_cast<int32_t>(width),
                           static_cast<int32_t>(height)};
    }
  }

  if (packet.dsValid) {
    auto* ds = wireHandlePtr<D9CSurface>(packet.dsHandle);
    state.depthStencil = attachmentFromSurface(ds ? ds->obj : nullptr);
  }

  if (packet.viewportValid) {
    state.viewport = viewportFromC(packet.viewport);
  }
  if (packet.scissorValid) {
    state.scissorRect = rectFromC(packet.scissor);
  }

  for (uint32_t i = 0; i < packet.tssCount; ++i) {
    const auto& e = packet.tss[i];
    const uint32_t stage = std::min(e.stage, dxmt9::core::kMaxTextureStages - 1);
    const uint32_t key = std::min(e.type, dxmt9::core::kMaxTextureStageStates - 1);
    state.textureStageStates[stage].set(key, e.value);
  }
  for (uint32_t i = 0; i < packet.samplerStateCount; ++i) {
    const auto& e = packet.samplerStates[i];
    state.samplerStates[e.sampler].set(e.type, e.value);
  }

  if (packet.materialValid) {
    state.material = materialFromC(packet.material);
  }

  for (uint32_t i = 0; i < dxmt9::core::kMaxClipPlanes; ++i) {
    if ((packet.clipPlaneMask & (1u << i)) == 0) {
      continue;
    }
    state.clipPlanes[i] = {packet.clipPlanes[i * 4 + 0],
                           packet.clipPlanes[i * 4 + 1],
                           packet.clipPlanes[i * 4 + 2],
                           packet.clipPlanes[i * 4 + 3]};
  }

  for (uint32_t i = 0; i < packet.transformCount; ++i) {
    const auto& e = packet.transforms[i];
    state.transforms.set(transformStateFromD3D(e.state), matrixFromC(e.matrix));
  }

  for (uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_LIGHTS; ++i) {
    if ((packet.lightSlotMask & (1u << i)) == 0) {
      continue;
    }
    state.lights[i] = lightFromC(packet.lights[i]);
  }
  for (uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_LIGHTS; ++i) {
    if ((packet.lightEnableValidMask & (1u << i)) == 0) {
      continue;
    }
    const bool enabled = (packet.lightEnableMask & (1u << i)) != 0;
    state.lightEnabled[i] = enabled;
    state.lights[i].enabled = enabled;
  }

  return dxmt9::core::D3D_OK;
}

int32_t applyDrawPacketState(D9CDevice* d, const D9CDrawPrimitivePacket& packet) {
  if (d->stateBlockRecording) {
    return applyDrawPacketStateViaIface(d, packet);
  }
  return applyDrawPacketStateDirect(d, packet);
}

int32_t timedApplyDrawPacketState(D9CDevice* d, const D9CDrawPrimitivePacket& packet) {
  const auto start = std::chrono::steady_clock::now();
  const int32_t hr = applyDrawPacketState(d, packet);
  countDurationSince(start, dxmt9::perf::countCommitChunkApplyDrawStateCpuTime);
  return hr;
}

bool recordRangeValid(std::uint32_t recordSize, std::uint32_t offset, std::uint32_t bytes) {
  return offset <= recordSize && bytes <= recordSize - offset;
}

// R-BACK-2.52(d): apply a Draw* record's folded inline const-delta sections
// (DXMT9_PE_INLINE_CONST_DELTA) through the SAME per-register setters the
// standalone D9C_COMMAND_RECORD_SET_VS_CONST_F.._PS_CONST_B case below
// dispatches to, in canonical section order (VS_F, VS_I, VS_B, PS_F, PS_I,
// PS_B), so the resulting server-side constant-register state is
// observably identical to replaying the equivalent standalone records
// immediately before this draw. `constDeltaBaseOffset` is the record-
// relative offset of the trailing const-delta payload area for this
// record's kind (see the d9c_command_record_draw_*_const_delta_offset
// helpers in device_c.h). Every section's register range and the record's
// total wire size were already gated by validateCommandRecord
// (device_c_record_validate.cpp) before this chunk ever reached replay, so
// this function trusts `packet.constDeltaSections` and only resolves +
// forwards each section's trailing payload bytes; it does not re-validate
// ranges.
int32_t applyDrawPacketConstDeltaSections(D9CDevice* d,
                                          const D9CDrawPrimitivePacket& packet,
                                          const dxmt9::core::u8* record,
                                          std::uint32_t constDeltaBaseOffset) {
  auto* q = findDirtyQueue(d);
  for (uint32_t kind = 0; kind < D9C_DRAW_PACKET_CONST_DELTA_COUNT; ++kind) {
    const auto& section = packet.constDeltaSections[kind];
    if (!section.valid) {
      continue;
    }
    const auto slice = d9c_draw_packet_const_delta_section_slice(
        &packet, constDeltaBaseOffset, kind);
    const auto* payload = record + slice.payloadOffset;
    int32_t hr = dxmt9::core::D3D_OK;
    switch (kind) {
    case D9C_DRAW_PACKET_CONST_DELTA_VS_F:
      hr = dxmt9c_device_set_vs_const_f(d, section.startRegister,
                                        reinterpret_cast<const float*>(payload),
                                        section.registerCount);
      if (q) q->applyDirtyConstantSetVsF(section.startRegister, section.registerCount);
      break;
    case D9C_DRAW_PACKET_CONST_DELTA_VS_I:
      hr = dxmt9c_device_set_vs_const_i(d, section.startRegister,
                                        reinterpret_cast<const int32_t*>(payload),
                                        section.registerCount);
      if (q) q->applyDirtyConstantSetVsI(section.startRegister, section.registerCount);
      break;
    case D9C_DRAW_PACKET_CONST_DELTA_VS_B:
      hr = dxmt9c_device_set_vs_const_b(d, section.startRegister,
                                        reinterpret_cast<const uint32_t*>(payload),
                                        section.registerCount);
      if (q) q->applyDirtyConstantSetVsB(section.startRegister, section.registerCount);
      break;
    case D9C_DRAW_PACKET_CONST_DELTA_PS_F:
      hr = dxmt9c_device_set_ps_const_f(d, section.startRegister,
                                        reinterpret_cast<const float*>(payload),
                                        section.registerCount);
      if (q) q->applyDirtyConstantSetPsF(section.startRegister, section.registerCount);
      break;
    case D9C_DRAW_PACKET_CONST_DELTA_PS_I:
      hr = dxmt9c_device_set_ps_const_i(d, section.startRegister,
                                        reinterpret_cast<const int32_t*>(payload),
                                        section.registerCount);
      if (q) q->applyDirtyConstantSetPsI(section.startRegister, section.registerCount);
      break;
    case D9C_DRAW_PACKET_CONST_DELTA_PS_B:
      hr = dxmt9c_device_set_ps_const_b(d, section.startRegister,
                                        reinterpret_cast<const uint32_t*>(payload),
                                        section.registerCount);
      if (q) q->applyDirtyConstantSetPsB(section.startRegister, section.registerCount);
      break;
    default:
      break;
    }
    if (failed(hr)) {
      return hr;
    }
  }
  return dxmt9::core::D3D_OK;
}

// Direct core::Device dispatch — bypasses the COM iface (Direct3DDevice9Impl)
// hop. The COM Draw* methods are 1-line forwarders to core::Device, so
// dispatching one record at a time through them costs an extra virtual call
// and an AddRef/Release-bearing path with no behavioral effect. The chunk
// importer is hot — every D9CCommandRecord_DRAW_* takes this path — so
// removing that hop is meaningful per-draw cost relief.
int32_t applyDrawPrimitivePacket(D9CDevice* d, const D9CDrawPrimitivePacket& packet) {
  const int32_t stateHr = timedApplyDrawPacketState(d, packet);
  if (failed(stateHr)) {
    return stateHr;
  }
  return d->dev().drawPrimitive(ptFromD3D(packet.primitiveType),
                                packet.primitiveCount,
                                packet.startVertex);
}

int32_t applyDrawIndexedPrimitivePacket(D9CDevice* d,
                                        const D9CDrawIndexedPrimitivePacket& packet) {
  const int32_t stateHr = timedApplyDrawPacketState(d, packet.state);
  if (failed(stateHr)) {
    return stateHr;
  }
  // Phase 12: index buffer delta — applied AFTER applyDrawPacketState so
  // it overrides any prior IB state, BEFORE the actual indexed draw.
  if (packet.ibValid) {
    auto* ib = wireHandlePtr<D9CBuffer>(packet.ibHandle);
    const auto indexBindStart = std::chrono::steady_clock::now();
    const int32_t hr = dxmt9c_device_set_indices(d, ib);
    countDurationSince(indexBindStart,
                       dxmt9::perf::countCommitChunkIndexBindCpuTime);
    if (failed(hr)) return hr;
  }
  const auto& state = d->dev().state();
  return d->dev().drawIndexedPrimitive(ptFromD3D(packet.state.primitiveType),
                                       packet.primitiveCount, 0, packet.baseVertex,
                                       packet.startIndex, state.indexType);
}

bool drawSubmitBatchEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_DISABLE_DRAW_SUBMIT_BATCH");
    return !env || env[0] == '\0' || std::strcmp(env, "0") == 0;
  }();
  return enabled;
}

bool canBatchDrawPacket(D9CDevice* d, const D9CDrawPrimitivePacket& packet) {
  return drawSubmitBatchEnabled() && d && !d->stateBlockRecording &&
         ptFromD3D(packet.primitiveType) != dxmt9::core::PrimitiveType::TriangleFan;
}

bool canBatchDrawPacket(D9CDevice* d,
                        const D9CDrawIndexedPrimitivePacket& packet) {
  return canBatchDrawPacket(d, packet.state);
}

int32_t queueDrawPrimitiveSubmission(
    D9CDevice* d, const D9CDrawPrimitivePacket& packet,
    std::vector<dxmt9::core::DrawRunSubmission>& submissions,
    dxmt9::core::DrawSubmissionUniformScratch& uniformScratch) {
  const auto queueStart = std::chrono::steady_clock::now();
  const auto finish = [&](int32_t hr) {
    countDurationSince(queueStart,
                       dxmt9::perf::countCommitChunkQueueDrawSubmissionCpuTime);
    return hr;
  };
  const int32_t stateHr = timedApplyDrawPacketState(d, packet);
  if (failed(stateHr)) {
    return finish(stateHr);
  }
  const std::size_t previousIndex = submissions.size();
  const auto emplaceStart = std::chrono::steady_clock::now();
  auto& submission = submissions.emplace_back();
  countDurationSince(
      emplaceStart,
      dxmt9::perf::countCommitChunkQueueDrawSubmissionEmplaceCpuTime);
  const auto* previousSubmission =
      previousIndex == 0u ? nullptr : &submissions[previousIndex - 1u];
  const auto snapshotStart = std::chrono::steady_clock::now();
  const int32_t hr =
      d->dev().snapshotDrawSubmissionFromCurrentState(makeRunParam(packet),
                                                      submission,
                                                      previousSubmission,
                                                      &uniformScratch,
                                                      compactUniformSubmissionsEnabled());
  countDurationSince(
      snapshotStart,
      dxmt9::perf::countCommitChunkQueueDrawSubmissionSnapshotCpuTime);
  if (failed(hr)) {
    submissions.pop_back();
    return finish(hr);
  }
  resolveChunkEndFlushProbeWithSubmission(d, submission);
  return finish(dxmt9::core::D3D_OK);
}

int32_t queueDrawIndexedPrimitiveSubmission(
    D9CDevice* d, const D9CDrawIndexedPrimitivePacket& packet,
    std::vector<dxmt9::core::DrawRunSubmission>& submissions,
    dxmt9::core::DrawSubmissionUniformScratch& uniformScratch) {
  const auto queueStart = std::chrono::steady_clock::now();
  const auto finish = [&](int32_t hr) {
    countDurationSince(queueStart,
                       dxmt9::perf::countCommitChunkQueueDrawSubmissionCpuTime);
    return hr;
  };
  const int32_t stateHr = timedApplyDrawPacketState(d, packet.state);
  if (failed(stateHr)) {
    return finish(stateHr);
  }
  if (packet.ibValid) {
    auto* ib = wireHandlePtr<D9CBuffer>(packet.ibHandle);
    const auto indexBindStart = std::chrono::steady_clock::now();
    const int32_t hr = dxmt9c_device_set_indices(d, ib);
    countDurationSince(indexBindStart,
                       dxmt9::perf::countCommitChunkIndexBindCpuTime);
    if (failed(hr)) return finish(hr);
  }
  auto draw = makeRunParam(packet);
  draw.indexType = d->dev().state().indexType;
  const std::size_t previousIndex = submissions.size();
  const auto emplaceStart = std::chrono::steady_clock::now();
  auto& submission = submissions.emplace_back();
  countDurationSince(
      emplaceStart,
      dxmt9::perf::countCommitChunkQueueDrawSubmissionEmplaceCpuTime);
  const auto* previousSubmission =
      previousIndex == 0u ? nullptr : &submissions[previousIndex - 1u];
  const auto snapshotStart = std::chrono::steady_clock::now();
  const int32_t hr =
      d->dev().snapshotDrawSubmissionFromCurrentState(draw, submission,
                                                      previousSubmission,
                                                      &uniformScratch,
                                                      compactUniformSubmissionsEnabled());
  countDurationSince(
      snapshotStart,
      dxmt9::perf::countCommitChunkQueueDrawSubmissionSnapshotCpuTime);
  if (failed(hr)) {
    submissions.pop_back();
    return finish(hr);
  }
  resolveChunkEndFlushProbeWithSubmission(d, submission);
  return finish(dxmt9::core::D3D_OK);
}

int32_t queueCompactDrawPrimitiveSubmission(
    D9CDevice* d, const D9CDrawPrimitivePacket& packet,
    std::vector<dxmt9::core::DrawRunCompactSubmission>& submissions,
    dxmt9::core::DrawSubmissionUniformScratch& uniformScratch) {
  const auto queueStart = std::chrono::steady_clock::now();
  const auto finish = [&](int32_t hr) {
    countDurationSince(queueStart,
                       dxmt9::perf::countCommitChunkQueueDrawSubmissionCpuTime);
    return hr;
  };
  const int32_t stateHr = timedApplyDrawPacketState(d, packet);
  if (failed(stateHr)) {
    return finish(stateHr);
  }
  const std::size_t previousIndex = submissions.size();
  const auto emplaceStart = std::chrono::steady_clock::now();
  auto& submission = submissions.emplace_back();
  countDurationSince(
      emplaceStart,
      dxmt9::perf::countCommitChunkQueueDrawSubmissionEmplaceCpuTime);
  const auto* previousSubmission =
      previousIndex == 0u ? nullptr : &submissions[previousIndex - 1u];
  const auto snapshotStart = std::chrono::steady_clock::now();
  const int32_t hr = d->dev().snapshotDrawSubmissionFromCurrentState(
      makeRunParam(packet), submission, previousSubmission, &uniformScratch);
  countDurationSince(
      snapshotStart,
      dxmt9::perf::countCommitChunkQueueDrawSubmissionSnapshotCpuTime);
  if (failed(hr)) {
    submissions.pop_back();
    return finish(hr);
  }
  resolveChunkEndFlushProbeWithSubmission(d, submission);
  DXMT_ASSERT(submission.compactUniforms.has_value() ||
              previousSubmission == nullptr ||
              submission.uniformGeneration == previousSubmission->uniformGeneration);
  return finish(dxmt9::core::D3D_OK);
}

int32_t queueCompactDrawIndexedPrimitiveSubmission(
    D9CDevice* d, const D9CDrawIndexedPrimitivePacket& packet,
    std::vector<dxmt9::core::DrawRunCompactSubmission>& submissions,
    dxmt9::core::DrawSubmissionUniformScratch& uniformScratch) {
  const auto queueStart = std::chrono::steady_clock::now();
  const auto finish = [&](int32_t hr) {
    countDurationSince(queueStart,
                       dxmt9::perf::countCommitChunkQueueDrawSubmissionCpuTime);
    return hr;
  };
  const int32_t stateHr = timedApplyDrawPacketState(d, packet.state);
  if (failed(stateHr)) {
    return finish(stateHr);
  }
  if (packet.ibValid) {
    auto* ib = wireHandlePtr<D9CBuffer>(packet.ibHandle);
    const auto indexBindStart = std::chrono::steady_clock::now();
    const int32_t hr = dxmt9c_device_set_indices(d, ib);
    countDurationSince(indexBindStart,
                       dxmt9::perf::countCommitChunkIndexBindCpuTime);
    if (failed(hr)) return finish(hr);
  }
  auto draw = makeRunParam(packet);
  draw.indexType = d->dev().state().indexType;
  const std::size_t previousIndex = submissions.size();
  const auto emplaceStart = std::chrono::steady_clock::now();
  auto& submission = submissions.emplace_back();
  countDurationSince(
      emplaceStart,
      dxmt9::perf::countCommitChunkQueueDrawSubmissionEmplaceCpuTime);
  const auto* previousSubmission =
      previousIndex == 0u ? nullptr : &submissions[previousIndex - 1u];
  const auto snapshotStart = std::chrono::steady_clock::now();
  const int32_t hr = d->dev().snapshotDrawSubmissionFromCurrentState(
      draw, submission, previousSubmission, &uniformScratch);
  countDurationSince(
      snapshotStart,
      dxmt9::perf::countCommitChunkQueueDrawSubmissionSnapshotCpuTime);
  if (failed(hr)) {
    submissions.pop_back();
    return finish(hr);
  }
  resolveChunkEndFlushProbeWithSubmission(d, submission);
  DXMT_ASSERT(submission.compactUniforms.has_value() ||
              previousSubmission == nullptr ||
              submission.uniformGeneration == previousSubmission->uniformGeneration);
  return finish(dxmt9::core::D3D_OK);
}

int32_t applyDrawPrimitiveUPPacket(D9CDevice* d,
                                   const D9CDrawPrimitiveUPPacket& packet,
                                   const dxmt9::core::u8* record,
                                   std::uint32_t recordSize) {
  const int32_t stateHr = timedApplyDrawPacketState(d, packet.state);
  if (failed(stateHr)) {
    return stateHr;
  }
  if (!recordRangeValid(recordSize, packet.vertexDataOffset, packet.vertexDataSize)) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  auto span = std::span<const dxmt9::core::u8>(record + packet.vertexDataOffset,
                                               packet.vertexDataSize);
  return d->dev().drawPrimitiveUP(ptFromD3D(packet.state.primitiveType),
                                  packet.primitiveCount, span, packet.stride);
}

int32_t applyDrawIndexedPrimitiveUPPacket(D9CDevice* d,
                                          const D9CDrawIndexedPrimitiveUPPacket& packet,
                                          const dxmt9::core::u8* record,
                                          std::uint32_t recordSize) {
  const int32_t stateHr = timedApplyDrawPacketState(d, packet.state);
  if (failed(stateHr)) {
    return stateHr;
  }
  if (!recordRangeValid(recordSize, packet.indexDataOffset, packet.indexDataSize) ||
      !recordRangeValid(recordSize, packet.vertexDataOffset, packet.vertexDataSize)) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  auto vertexSpan = std::span<const dxmt9::core::u8>(record + packet.vertexDataOffset,
                                                     packet.vertexDataSize);
  auto indexSpan = std::span<const dxmt9::core::u8>(record + packet.indexDataOffset,
                                                    packet.indexDataSize);
  return d->dev().drawIndexedPrimitiveUP(ptFromD3D(packet.state.primitiveType),
                                         packet.primitiveCount, vertexSpan, indexSpan,
                                         idxTypeFromD3D(packet.indexFormat), packet.stride);
}

int32_t replayImportedChunk(D9CDevice* d,
                            const ImportedWireChunkView& importedChunk,
                            bool skipDrawResourceMarking,
                            std::chrono::steady_clock::time_point bridgeCommitStart,
                            std::chrono::steady_clock::time_point replayStageStart,
                            bool pacedByPresentOrdinal) {
  auto commitChunkStageStart = replayStageStart;

  // Phase 14: bulk markChunkResources has already pinned every resource
  // in this chunk against its seqId. Suppress the submitDrawRun
  // markDrawResources walk for the duration of this record-iter block;
  // the RAII guard clears the flag even if a record returns early.
  // Chunks with no retained pool resources keep the normal run-level
  // hot-state marking path.
  struct ResetSkipDrawMarkGuard {
    std::shared_ptr<dxmt9::Device> upper;
    ~ResetSkipDrawMarkGuard() {
      if (upper) upper->setSkipDrawResourceMarking(false);
    }
  } resetGuard{};
  if (skipDrawResourceMarking) {
    if (auto upper = d->dev().upperDevice()) {
      upper->setSkipDrawResourceMarking(true);
      resetGuard.upper = std::move(upper);
    }
  }
  if (auto* q = findDirtyQueue(d)) {
    q->noteCommitChunkReplayStartForCompletionGap();
  }

  auto& pendingScratch = pendingDrawSubmissionScratch();
  ScopedPendingDrawSubmissionScratchUse pendingScratchUse(pendingScratch);
  auto& pendingDrawSubmissions = pendingScratch.submissions;
  auto& pendingCompactDrawSubmissions = pendingScratch.compactSubmissions;
  auto& pendingUniformScratch = pendingScratch.uniformScratch;
  pendingDrawSubmissions.reserve(std::min<std::uint32_t>(importedChunk.recordCount, 256u));
  pendingCompactDrawSubmissions.reserve(
      std::min<std::uint32_t>(importedChunk.recordCount, 256u));
  const auto flushPendingDrawSubmissions =
      [&](PendingDrawFlushReason reason) -> int32_t {
    if (pendingDrawSubmissions.empty() &&
        pendingCompactDrawSubmissions.empty()) {
      return dxmt9::core::D3D_OK;
    }
    const auto pendingRecordCount =
        static_cast<std::uint64_t>(pendingDrawSubmissions.size()) +
        static_cast<std::uint64_t>(pendingCompactDrawSubmissions.size());
    const auto flushStart = std::chrono::steady_clock::now();
    if (!pendingDrawSubmissions.empty()) {
      dxmt9::perf::countCommitChunkDrawSubmissionBatch(
          static_cast<std::uint32_t>(pendingDrawSubmissions.size()));
      attachCompactUniformArena(pendingDrawSubmissions, pendingUniformScratch);
      const auto submitStart = std::chrono::steady_clock::now();
      d->dev().submitDrawSubmissionBatch(pendingDrawSubmissions);
      dxmt9::perf::countCommitChunkDrawBatchSubmitCpuTime(
          static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - submitStart)
                  .count()));
      if (reason == PendingDrawFlushReason::End) {
        storeChunkEndFlushProbe(d, pendingDrawSubmissions.back(),
                                pendingRecordCount);
      }
      pendingDrawSubmissions.clear();
    }
    if (!pendingCompactDrawSubmissions.empty()) {
      dxmt9::perf::countCommitChunkDrawSubmissionBatch(
          static_cast<std::uint32_t>(pendingCompactDrawSubmissions.size()));
      attachCompactUniformArena(pendingCompactDrawSubmissions,
                                pendingUniformScratch);
      const auto submitStart = std::chrono::steady_clock::now();
      d->dev().submitCompactDrawSubmissionBatch(
          pendingCompactDrawSubmissions);
      dxmt9::perf::countCommitChunkDrawBatchSubmitCpuTime(
          static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - submitStart)
                  .count()));
      if (reason == PendingDrawFlushReason::End) {
        storeChunkEndFlushProbe(d, pendingCompactDrawSubmissions.back(),
                                pendingRecordCount);
      }
      pendingCompactDrawSubmissions.clear();
    }
    pendingUniformScratch.clear();
    const auto flushNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - flushStart)
            .count());
    dxmt9::perf::countCommitChunkReplayPendingFlushCpuTime(flushNs);
    countPendingDrawFlushReason(reason, flushNs);
    countPendingDrawFlushReasonVolume(reason, pendingRecordCount);
    return dxmt9::core::D3D_OK;
  };
  std::uint32_t recordIndex = 0;
  while (auto recordView = nextImportedRecord(importedChunk, recordIndex)) {
    if (!recordView->valid()) {
      return commitChunkFail("record-view", recordIndex);
    }

    const auto header = recordView->header;
    const auto* record = recordView->record;
    int32_t hr = dxmt9::core::D3DERR_INVALIDCALL;
    const bool batchableDrawRecord =
        header.type == D9C_COMMAND_RECORD_DRAW_PRIMITIVE ||
        header.type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
    const bool batchThroughRecord =
        commitChunkRecordAllowsPendingDrawBatchThrough(header.type);
    if (!batchableDrawRecord && !batchThroughRecord) {
      blockChunkEndFlushProbe(d, /*drawFallback=*/false);
      hr = flushPendingDrawSubmissions(PendingDrawFlushReason::BeforeRecord);
      if (failed(hr)) {
        return commitChunkFail("draw-batch-flush", recordIndex, header.type, hr);
      }
    } else if (batchThroughRecord &&
               (!pendingDrawSubmissions.empty() ||
                !pendingCompactDrawSubmissions.empty())) {
      dxmt9::perf::countCommitChunkDrawBatchConstUploadPassthrough();
    }
    ReplayRecordCpuScope replayRecordCpu(header.type);
    switch (header.type) {
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE: {
      D9CCommandRecordDrawPrimitive decoded{};
      std::memcpy(&decoded, record, sizeof(decoded));
      // R-BACK-2.52(d): apply any folded inline const-delta sections
      // BEFORE this draw's own state-delta application below, so the
      // effective server-side constant state matches replaying the
      // equivalent standalone SET_*_CONST_* records immediately before
      // this draw.
      hr = applyDrawPacketConstDeltaSections(
          d, decoded.packet, record,
          d9c_command_record_draw_primitive_const_delta_offset());
      if (failed(hr)) {
        break;
      }
      // C1: every DRAW_* packet folds a state delta in front of its
      // draw via applyDrawPacketState; mark the matching dirty bits so
      // C2 sees the same categories the canonical state changed.
      markDirtyFromDrawPacketState(findDirtyQueue(d), decoded.packet);
      // Try to coalesce into a draw run: scan ahead for compatible direct
      // or indexed draw records whose deltas resolve to the same run base,
      // append scanned DrawParam records into a flat span, then submit once
      // (single canonical hot-state build + single queue submission).
      // Falls through to per-record path if the run is just length-1.
      const auto scanStart = std::chrono::steady_clock::now();
      const auto scan = scanImportedDrawRun(importedChunk, *recordView);
      countDurationSince(scanStart, dxmt9::perf::countCommitChunkDrawRunScanCpuTime);
      countCommitChunkDrawRunScan(scan);
      if (scan.replayAsRun()) {
        resolveChunkEndFlushProbeWithDrawRun(d, scan.recordCount);
        hr = flushPendingDrawSubmissions(PendingDrawFlushReason::DrawRun);
        if (failed(hr)) return commitChunkFail("draw-run-flush", recordIndex, header.type, hr);
        // Apply the first record's full state once. Stream/IB changes from
        // later records ride alongside each DrawParam as low-level binding
        // overrides, then the public D3D state is advanced to the final
        // binding after the run is submitted.
        hr = timedApplyDrawPacketState(d, decoded.packet);
        if (failed(hr)) return commitChunkFail("draw-run-state", recordIndex, header.type, hr);
        const auto baseBindings = effectiveBindingsFromState(d->dev().state());
        auto effectiveBindings = baseBindings;
        const auto runBuildStart = std::chrono::steady_clock::now();
        std::vector<dxmt9::core::DrawParam> runParams;
        runParams.reserve(scan.recordCount);
        std::vector<dxmt9::core::DrawBindingOverride> bindingOverrides;
        bindingOverrides.reserve(scan.recordCount);
        std::vector<dxmt9::core::DrawParamPayloadView> runPayloads;
        runPayloads.reserve(scan.recordCount);
        const auto appendDirectRunParam = [&](const D9CDrawPrimitivePacket& packet) {
          applyStreamBindingDeltas(effectiveBindings, packet);
          auto param = makeRunParam(packet);
          param.indexType = effectiveBindings.indexType;
          dxmt9::core::DrawBindingOverride override{};
          if (makeBindingOverride(baseBindings, effectiveBindings, override)) {
            dxmt9::perf::countCommitChunkDrawRunBindingOverride(
                override.streamMask != 0, override.indexBufferValid, sizeof(override));
            bindingOverrides.push_back(override);
            runPayloads.push_back(bindingOverridePayloadView(bindingOverrides.back()));
          } else {
            runPayloads.push_back({});
          }
          runParams.push_back(param);
        };
        const auto appendIndexedRunParam = [&](const D9CDrawIndexedPrimitivePacket& packet) {
          applyStreamBindingDeltas(effectiveBindings, packet.state);
          applyIndexBindingDelta(effectiveBindings, packet);
          auto param = makeRunParam(packet);
          param.indexType = effectiveBindings.indexType;
          dxmt9::core::DrawBindingOverride override{};
          if (makeBindingOverride(baseBindings, effectiveBindings, override)) {
            dxmt9::perf::countCommitChunkDrawRunBindingOverride(
                override.streamMask != 0, override.indexBufferValid, sizeof(override));
            bindingOverrides.push_back(override);
            runPayloads.push_back(bindingOverridePayloadView(bindingOverrides.back()));
          } else {
            runPayloads.push_back({});
          }
          runParams.push_back(param);
        };
        appendDirectRunParam(decoded.packet);
        countCommitChunkDrawReplay(d, decoded.packet);

        std::uint32_t runIndex = recordView->nextIndex();
        while (runIndex < scan.endIndex) {
          const auto nextRecord = nextImportedRecord(importedChunk, runIndex);
          if (!nextRecord || !nextRecord->valid()) {
            return commitChunkFail("draw-run-record", runIndex, header.type);
          }
          if (nextRecord->header.type == D9C_COMMAND_RECORD_DRAW_PRIMITIVE) {
            D9CCommandRecordDrawPrimitive nextDecoded{};
            std::memcpy(&nextDecoded, nextRecord->record, sizeof(nextDecoded));
            appendDirectRunParam(nextDecoded.packet);
            countCommitChunkDrawReplay(d, nextDecoded.packet);
          } else if (nextRecord->header.type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE) {
            D9CCommandRecordDrawIndexedPrimitive nextDecoded{};
            std::memcpy(&nextDecoded, nextRecord->record, sizeof(nextDecoded));
            appendIndexedRunParam(nextDecoded.packet);
            countCommitChunkDrawReplay(d, nextDecoded.packet);
          } else {
            return commitChunkFail("draw-run-type", runIndex, nextRecord->header.type);
          }
          runIndex = nextRecord->nextIndex();
        }
        if (runParams.size() != scan.recordCount) {
          return commitChunkFail("draw-run-count", recordIndex, header.type);
        }
        if (runPayloads.size() != runParams.size()) {
          return commitChunkFail("draw-run-payload-count", recordIndex, header.type);
        }
        countDurationSince(runBuildStart,
                           dxmt9::perf::countCommitChunkDrawRunBuildCpuTime);

        {
          const auto runSubmitStart = std::chrono::steady_clock::now();
          hr = drawRunCanonicalFastPathEnabled()
                   ? d->dev().drawPrimitiveRunCanonical(runParams, runPayloads)
                   : d->dev().drawPrimitiveRun(runParams, runPayloads);
          countDurationSince(runSubmitStart,
                             dxmt9::perf::countCommitChunkDrawRunSubmitCpuTime);
        }
        if (failed(hr)) return commitChunkFail("draw-run", recordIndex, header.type, hr);
        const auto finalBindStart = std::chrono::steady_clock::now();
        hr = applyFinalBindingState(d, baseBindings, effectiveBindings);
        countDurationSince(finalBindStart,
                           dxmt9::perf::countCommitChunkDrawRunFinalBindCpuTime);
        if (failed(hr)) return commitChunkFail("draw-run-final-bind", recordIndex, header.type, hr);
        recordIndex = scan.endIndex;
        continue;
      }
      countCommitChunkDrawReplay(d, decoded.packet);
      if (canBatchDrawPacket(d, decoded.packet)) {
        if (compactSubmissionCarrierEnabled()) {
          hr = queueCompactDrawPrimitiveSubmission(
              d, decoded.packet, pendingCompactDrawSubmissions,
              pendingUniformScratch);
        } else {
          hr = queueDrawPrimitiveSubmission(d, decoded.packet,
                                            pendingDrawSubmissions,
                                            pendingUniformScratch);
        }
      } else {
        blockChunkEndFlushProbe(d, /*drawFallback=*/true);
        hr = flushPendingDrawSubmissions(PendingDrawFlushReason::DrawFallback);
        if (failed(hr)) return commitChunkFail("draw-flush", recordIndex, header.type, hr);
        hr = applyDrawPrimitivePacket(d, decoded.packet);
      }
      break;
    }
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE: {
      D9CCommandRecordDrawIndexedPrimitive decoded{};
      std::memcpy(&decoded, record, sizeof(decoded));
      // R-BACK-2.52(d): see the DRAW_PRIMITIVE case above.
      hr = applyDrawPacketConstDeltaSections(
          d, decoded.packet.state, record,
          d9c_command_record_draw_indexed_primitive_const_delta_offset());
      if (failed(hr)) {
        break;
      }
      markDirtyFromDrawPacketState(findDirtyQueue(d), decoded.packet.state);
      // Same coalescing as DRAW_PRIMITIVE. Direct and indexed records may
      // share a run; DrawParam carries the per-draw indexed flag used by
      // the encoder to dispatch drawIndexed vs draw.
      const auto scanStart = std::chrono::steady_clock::now();
      const auto scan = scanImportedDrawRun(importedChunk, *recordView);
      countDurationSince(scanStart, dxmt9::perf::countCommitChunkDrawRunScanCpuTime);
      countCommitChunkDrawRunScan(scan);
      if (scan.replayAsRun()) {
        resolveChunkEndFlushProbeWithDrawRun(d, scan.recordCount);
        hr = flushPendingDrawSubmissions(PendingDrawFlushReason::DrawRun);
        if (failed(hr)) return commitChunkFail("indexed-draw-run-flush", recordIndex, header.type, hr);
        hr = timedApplyDrawPacketState(d, decoded.packet.state);
        if (failed(hr)) return commitChunkFail("indexed-draw-run-state", recordIndex, header.type, hr);
        if (decoded.packet.ibValid) {
          auto* ib = wireHandlePtr<D9CBuffer>(decoded.packet.ibHandle);
          const auto indexBindStart = std::chrono::steady_clock::now();
          hr = dxmt9c_device_set_indices(d, ib);
          countDurationSince(indexBindStart,
                             dxmt9::perf::countCommitChunkIndexBindCpuTime);
          if (failed(hr)) return commitChunkFail("indexed-draw-run-ib", recordIndex, header.type, hr);
        }
        const auto baseBindings = effectiveBindingsFromState(d->dev().state());
        auto effectiveBindings = baseBindings;
        const auto runBuildStart = std::chrono::steady_clock::now();
        std::vector<dxmt9::core::DrawParam> runParams;
        runParams.reserve(scan.recordCount);
        std::vector<dxmt9::core::DrawBindingOverride> bindingOverrides;
        bindingOverrides.reserve(scan.recordCount);
        std::vector<dxmt9::core::DrawParamPayloadView> runPayloads;
        runPayloads.reserve(scan.recordCount);
        const auto appendDirectRunParam = [&](const D9CDrawPrimitivePacket& packet) {
          applyStreamBindingDeltas(effectiveBindings, packet);
          auto param = makeRunParam(packet);
          param.indexType = effectiveBindings.indexType;
          dxmt9::core::DrawBindingOverride override{};
          if (makeBindingOverride(baseBindings, effectiveBindings, override)) {
            dxmt9::perf::countCommitChunkDrawRunBindingOverride(
                override.streamMask != 0, override.indexBufferValid, sizeof(override));
            bindingOverrides.push_back(override);
            runPayloads.push_back(bindingOverridePayloadView(bindingOverrides.back()));
          } else {
            runPayloads.push_back({});
          }
          runParams.push_back(param);
        };
        const auto appendIndexedRunParam = [&](const D9CDrawIndexedPrimitivePacket& packet) {
          applyStreamBindingDeltas(effectiveBindings, packet.state);
          applyIndexBindingDelta(effectiveBindings, packet);
          auto param = makeRunParam(packet);
          param.indexType = effectiveBindings.indexType;
          dxmt9::core::DrawBindingOverride override{};
          if (makeBindingOverride(baseBindings, effectiveBindings, override)) {
            dxmt9::perf::countCommitChunkDrawRunBindingOverride(
                override.streamMask != 0, override.indexBufferValid, sizeof(override));
            bindingOverrides.push_back(override);
            runPayloads.push_back(bindingOverridePayloadView(bindingOverrides.back()));
          } else {
            runPayloads.push_back({});
          }
          runParams.push_back(param);
        };
        appendIndexedRunParam(decoded.packet);
        countCommitChunkDrawReplay(d, decoded.packet);

        std::uint32_t runIndex = recordView->nextIndex();
        while (runIndex < scan.endIndex) {
          const auto nextRecord = nextImportedRecord(importedChunk, runIndex);
          if (!nextRecord || !nextRecord->valid()) {
            return commitChunkFail("indexed-draw-run-record", runIndex, header.type);
          }
          if (nextRecord->header.type == D9C_COMMAND_RECORD_DRAW_PRIMITIVE) {
            D9CCommandRecordDrawPrimitive nextDecoded{};
            std::memcpy(&nextDecoded, nextRecord->record, sizeof(nextDecoded));
            appendDirectRunParam(nextDecoded.packet);
            countCommitChunkDrawReplay(d, nextDecoded.packet);
          } else if (nextRecord->header.type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE) {
            D9CCommandRecordDrawIndexedPrimitive nextDecoded{};
            std::memcpy(&nextDecoded, nextRecord->record, sizeof(nextDecoded));
            appendIndexedRunParam(nextDecoded.packet);
            countCommitChunkDrawReplay(d, nextDecoded.packet);
          } else {
            return commitChunkFail("indexed-draw-run-type", runIndex, nextRecord->header.type);
          }
          runIndex = nextRecord->nextIndex();
        }
        if (runParams.size() != scan.recordCount) {
          return commitChunkFail("indexed-draw-run-count", recordIndex, header.type);
        }
        if (runPayloads.size() != runParams.size()) {
          return commitChunkFail("indexed-draw-run-payload-count", recordIndex, header.type);
        }
        countDurationSince(runBuildStart,
                           dxmt9::perf::countCommitChunkDrawRunBuildCpuTime);

        {
          const auto runSubmitStart = std::chrono::steady_clock::now();
          hr = drawRunCanonicalFastPathEnabled()
                   ? d->dev().drawPrimitiveRunCanonical(runParams, runPayloads)
                   : d->dev().drawPrimitiveRun(runParams, runPayloads);
          countDurationSince(runSubmitStart,
                             dxmt9::perf::countCommitChunkDrawRunSubmitCpuTime);
        }
        if (failed(hr)) return commitChunkFail("indexed-draw-run", recordIndex, header.type, hr);
        const auto finalBindStart = std::chrono::steady_clock::now();
        hr = applyFinalBindingState(d, baseBindings, effectiveBindings);
        countDurationSince(finalBindStart,
                           dxmt9::perf::countCommitChunkDrawRunFinalBindCpuTime);
        if (failed(hr)) return commitChunkFail("indexed-draw-run-final-bind", recordIndex, header.type, hr);
        recordIndex = scan.endIndex;
        continue;
      }
      countCommitChunkDrawReplay(d, decoded.packet);
      if (canBatchDrawPacket(d, decoded.packet)) {
        if (compactSubmissionCarrierEnabled()) {
          hr = queueCompactDrawIndexedPrimitiveSubmission(
              d, decoded.packet, pendingCompactDrawSubmissions,
              pendingUniformScratch);
        } else {
          hr = queueDrawIndexedPrimitiveSubmission(d, decoded.packet,
                                                   pendingDrawSubmissions,
                                                   pendingUniformScratch);
        }
      } else {
        blockChunkEndFlushProbe(d, /*drawFallback=*/true);
        hr = flushPendingDrawSubmissions(PendingDrawFlushReason::DrawFallback);
        if (failed(hr)) return commitChunkFail("indexed-draw-flush", recordIndex, header.type, hr);
        hr = applyDrawIndexedPrimitivePacket(d, decoded.packet);
      }
      break;
    }
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP: {
      D9CCommandRecordDrawPrimitiveUP decoded{};
      std::memcpy(&decoded, record, sizeof(decoded));
      // R-BACK-2.52(d): see the DRAW_PRIMITIVE case above.
      hr = applyDrawPacketConstDeltaSections(
          d, decoded.packet.state, record,
          d9c_command_record_draw_primitive_up_const_delta_offset(&decoded.packet));
      if (failed(hr)) {
        break;
      }
      blockChunkEndFlushProbe(d, /*drawFallback=*/true);
      markDirtyFromDrawPacketState(findDirtyQueue(d), decoded.packet.state);
      hr = applyDrawPrimitiveUPPacket(d, decoded.packet, record, header.size);
      break;
    }
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP: {
      D9CCommandRecordDrawIndexedPrimitiveUP decoded{};
      std::memcpy(&decoded, record, sizeof(decoded));
      // R-BACK-2.52(d): see the DRAW_PRIMITIVE case above.
      hr = applyDrawPacketConstDeltaSections(
          d, decoded.packet.state, record,
          d9c_command_record_draw_indexed_primitive_up_const_delta_offset(&decoded.packet));
      if (failed(hr)) {
        break;
      }
      blockChunkEndFlushProbe(d, /*drawFallback=*/true);
      markDirtyFromDrawPacketState(findDirtyQueue(d), decoded.packet.state);
      hr = applyDrawIndexedPrimitiveUPPacket(d, decoded.packet, record, header.size);
      break;
    }
    case D9C_COMMAND_RECORD_CLEAR: {
      D9CCommandRecordClear cl{};
      std::memcpy(&cl, record, sizeof(cl));
      const auto* rects = cl.rectCount != 0
                              ? reinterpret_cast<const D9CRect*>(record + cl.rectOffset)
                              : nullptr;
      hr = dxmt9c_device_clear(d, cl.rectCount, rects, cl.flags, cl.colorARGB,
                                cl.z, cl.stencil);
      break;
    }
    case D9C_COMMAND_RECORD_PRESENT: {
      D9CCommandRecordPresent pr{};
      std::memcpy(&pr, record, sizeof(pr));
      const auto* srcRect = pr.hasSrc ? &pr.src : nullptr;
      const auto* dstRect = pr.hasDst ? &pr.dst : nullptr;
      if (auto* q = findDirtyQueue(d)) {
        const auto activeReplayNs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - commitChunkStageStart)
                .count());
        q->noteCommitChunkActiveReplayCpuBeforePublish(activeReplayNs);
      }
      // R-BACK-2.51(g) — dxmt9c_device_present()'s wire-ABI signature
      // (include/dxmt9/device_c.h) cannot carry this bit directly, so it
      // rides the D9CDevice-reachable core::Device instead: set here, read
      // + reset one presentEx() call later (see setNextPresentPacedByOrdinal
      // doc). Only this chunk-replay call site ever sets it true, and only
      // when this replay itself is running because
      // dxmt9::d3d9::offloadCommitReplayEnabled() was on when the owning
      // chunk was committed (see the two replayImportedChunk call sites
      // below).
      d->dev().setNextPresentPacedByOrdinal(pacedByPresentOrdinal);
      // dirty-region payload was dropped at chunk-record time (PE
      // doesn't ship it); pass nullptr.
      hr = dxmt9c_device_present(d, srcRect, dstRect, pr.hwnd,
                                  /*dirtyRegion=*/nullptr, pr.flags);
      break;
    }
    case D9C_COMMAND_RECORD_STRETCH_RECT: {
      D9CCommandRecordStretchRect sr{};
      std::memcpy(&sr, record, sizeof(sr));
      auto* srcSurf = wireValuePtr<D9CSurface>(sr.srcWire);
      auto* dstSurf = wireValuePtr<D9CSurface>(sr.dstWire);
      const auto* srcR = sr.hasSrcRect ? &sr.srcRect : nullptr;
      const auto* dstR = sr.hasDstRect ? &sr.dstRect : nullptr;
      hr = dxmt9c_device_stretch_rect(d, srcSurf, srcR, dstSurf, dstR, sr.filter);
      break;
    }
    case D9C_COMMAND_RECORD_COLOR_FILL: {
      D9CCommandRecordColorFill cf{};
      std::memcpy(&cf, record, sizeof(cf));
      auto* surf = wireValuePtr<D9CSurface>(cf.surfaceWire);
      const auto* rect = cf.hasRect ? &cf.rect : nullptr;
      hr = dxmt9c_device_color_fill(d, surf, rect, cf.colorARGB);
      break;
    }
    case D9C_COMMAND_RECORD_UPDATE_TEXTURE: {
      D9CCommandRecordUpdateTexture ut{};
      std::memcpy(&ut, record, sizeof(ut));
      auto* srcTex = wireValuePtr<D9CTexture>(ut.srcWire);
      auto* dstTex = wireValuePtr<D9CTexture>(ut.dstWire);
      hr = dxmt9c_device_update_texture(d, srcTex, dstTex);
      break;
    }
    case D9C_COMMAND_RECORD_UPDATE_SURFACE: {
      D9CCommandRecordUpdateSurface us{};
      std::memcpy(&us, record, sizeof(us));
      auto* srcSurf = wireValuePtr<D9CSurface>(us.srcWire);
      auto* dstSurf = wireValuePtr<D9CSurface>(us.dstWire);
      const auto* srcRect = us.hasSrcRect ? &us.srcRect : nullptr;
      const auto* dstPoint = us.hasDstPoint ? &us.dstPoint : nullptr;
      hr = dxmt9c_device_update_surface(d, srcSurf, srcRect, dstSurf, dstPoint);
      break;
    }
    case D9C_COMMAND_RECORD_QUERY_ISSUE: {
      D9CCommandRecordQueryIssue qi{};
      std::memcpy(&qi, record, sizeof(qi));
      auto* query = wireValuePtr<D9CQuery>(qi.queryWire);
      hr = dxmt9c_query_issue(query, qi.flags);
      break;
    }
    case D9C_COMMAND_RECORD_READBACK: {
      D9CCommandRecordReadback rb{};
      std::memcpy(&rb, record, sizeof(rb));
      auto* srcSurf = wireValuePtr<D9CSurface>(rb.srcWire);
      auto* dstSurf = wireValuePtr<D9CSurface>(rb.dstWire);
      // Routes to the same backend path as the legacy bridge call —
      // encodes the copy + waits for GPU completion + writes pixels
      // into dst. HRESULT propagates back through commit_chunk's
      // per-record short-circuit to the PE caller.
      hr = dxmt9c_device_get_render_target_data(d, srcSurf, dstSurf);
      break;
    }
    case D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE: {
      // R-FORMAT-11 — RESZ MSAA depth resolve. Mirrors the StretchRect /
      // Readback two-handle decode: msaaDepthHandle is a SERVER-SIDE
      // D9CSurface* (the bound multisampled depth source), intzDestHandle a
      // SERVER-SIDE D9CTexture* (the stage-0 INTZ destination). Canonicalize
      // each wrapper to its core object and submit through the same backend
      // queue path StretchRect uses — Device::reszDepthResolve builds the
      // DepthResolveDesc (surface handle + the INTZ texture's level-0 surface
      // handle) and submits it. This is server-local: RESZ is record-only, so
      // unlike Readback it needs no dxmt9c_* PE bridge entry / ABI-hash slot.
      // Fire-and-forget — a missing binding resolves to a benign no-op inside
      // reszDepthResolve, matching real-hardware RESZ behavior.
      D9CCommandRecordReszDepthResolve resz{};
      std::memcpy(&resz, record, sizeof(resz));
      auto* msaaDepth = wireValuePtr<D9CSurface>(resz.msaaDepthHandle);
      auto* intzDest = wireValuePtr<D9CTexture>(resz.intzDestHandle);
      // Null wrappers are a benign no-op (fire-and-forget); the PE side
      // already suppresses emitting a record when either binding is absent.
      hr = (msaaDepth && intzDest)
               ? d->dev().reszDepthResolve(msaaDepth->obj, intzDest->obj)
               : dxmt9::core::D3D_OK;
      break;
    }
    case D9C_COMMAND_RECORD_APPLY_STATE: {
      D9CCommandRecordApplyState as{};
      std::memcpy(&as, record, sizeof(as));
      // Apply the state delta only; draw fields in the packet
      // (primitiveType / primitiveCount / startVertex) are unused.
      hr = timedApplyDrawPacketState(d, as.packet);
      // C1 dirty tracking: APPLY_STATE bundles RS / transform / clip /
      // viewport / light / material deltas. False-dirty is safe per
      // task spec; the alternative (missing-dirty) is a correctness bug.
      markDirtyFromDrawPacketState(findDirtyQueue(d), as.packet);
      break;
    }
    case D9C_COMMAND_RECORD_SET_VS_CONST_F:
    case D9C_COMMAND_RECORD_SET_VS_CONST_I:
    case D9C_COMMAND_RECORD_SET_VS_CONST_B:
    case D9C_COMMAND_RECORD_SET_PS_CONST_F:
    case D9C_COMMAND_RECORD_SET_PS_CONST_I:
    case D9C_COMMAND_RECORD_SET_PS_CONST_B: {
      const auto constUploadStart = std::chrono::steady_clock::now();
      D9CCommandRecordSetConst hdr{};
      std::memcpy(&hdr, record, sizeof(hdr));
      const auto* payload = record + sizeof(hdr);
      auto* q = findDirtyQueue(d);
      switch (header.type) {
      case D9C_COMMAND_RECORD_SET_VS_CONST_F:
        hr = dxmt9c_device_set_vs_const_f(d, hdr.start,
                                          reinterpret_cast<const float*>(payload),
                                          hdr.count);
        if (q) q->applyDirtyConstantSetVsF(hdr.start, hdr.count);
        break;
      case D9C_COMMAND_RECORD_SET_PS_CONST_F:
        hr = dxmt9c_device_set_ps_const_f(d, hdr.start,
                                          reinterpret_cast<const float*>(payload),
                                          hdr.count);
        if (q) q->applyDirtyConstantSetPsF(hdr.start, hdr.count);
        break;
      case D9C_COMMAND_RECORD_SET_VS_CONST_I:
        hr = dxmt9c_device_set_vs_const_i(d, hdr.start,
                                          reinterpret_cast<const int32_t*>(payload),
                                          hdr.count);
        if (q) q->applyDirtyConstantSetVsI(hdr.start, hdr.count);
        break;
      case D9C_COMMAND_RECORD_SET_PS_CONST_I:
        hr = dxmt9c_device_set_ps_const_i(d, hdr.start,
                                          reinterpret_cast<const int32_t*>(payload),
                                          hdr.count);
        if (q) q->applyDirtyConstantSetPsI(hdr.start, hdr.count);
        break;
      case D9C_COMMAND_RECORD_SET_VS_CONST_B:
        hr = dxmt9c_device_set_vs_const_b(d, hdr.start,
                                          reinterpret_cast<const uint32_t*>(payload),
                                          hdr.count);
        if (q) q->applyDirtyConstantSetVsB(hdr.start, hdr.count);
        break;
      case D9C_COMMAND_RECORD_SET_PS_CONST_B:
        hr = dxmt9c_device_set_ps_const_b(d, hdr.start,
                                          reinterpret_cast<const uint32_t*>(payload),
                                          hdr.count);
        if (q) q->applyDirtyConstantSetPsB(hdr.start, hdr.count);
        break;
      }
      countDurationSince(constUploadStart,
                         dxmt9::perf::countCommitChunkConstUploadCpuTime);
      break;
    }
    default:
      return commitChunkFail("unknown-record", recordIndex, header.type);
    }

    if (failed(hr)) {
      const auto flushHr =
          flushPendingDrawSubmissions(PendingDrawFlushReason::Failure);
      if (failed(flushHr)) {
        return commitChunkFail("draw-batch-flush-after-fail", recordIndex,
                               header.type, flushHr);
      }
      return commitChunkFail("record-replay", recordIndex, header.type, hr);
    }
    recordIndex = recordView->nextIndex();
  }

  const auto flushHr = flushPendingDrawSubmissions(PendingDrawFlushReason::End);
  if (failed(flushHr)) {
    return commitChunkFail("draw-batch-flush-end", recordIndex,
                           importedChunk.recordCount, flushHr);
  }
  if (recordIndex != importedChunk.recordCount) {
    return commitChunkFail("truncated-records", recordIndex, importedChunk.recordCount);
  }
  auto commitChunkStageEnd = std::chrono::steady_clock::now();
  const auto replayCpuNs = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          commitChunkStageEnd - commitChunkStageStart).count());
  if (auto* q = findDirtyQueue(d)) {
    q->noteCommitChunkReplayCpuBeforePublish(replayCpuNs);
    q->noteCommitChunkReplayEndForCompletionGap(replayCpuNs);
    q->prefetchCurrentWritingSlotPipelines();
  }
  dxmt9::perf::countCommitChunkReplayCpuTime(replayCpuNs);
  // R-BACK-2.10: chunk fully validated + replayed. Bumping admit at the
  // single success point keeps reject + admit symmetric.
  dxmt9::perf::countChunkAdmit();
  // V1 boundary B2 — record bridge crossing latency at the same single
  // success exit so the percentile ring is populated only by complete
  // commits. enabled() is checked inside the helper.
  const auto bridgeCommitDelta =
      std::chrono::steady_clock::now() - bridgeCommitStart;
  dxmt9::perf::countBridgeCommitLatencyNs(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(bridgeCommitDelta)
          .count()));
  return dxmt9::core::D3D_OK;
}

}  // namespace

namespace {

// Offload-local tag for wrapper kinds that never enter the wire handle
// table (see the verification comment above retainWrappersForOffload).
// D9C_CHUNK_HANDLE_KIND_* runs 0..D9C_CHUNK_HANDLE_KIND_VERTEX_DECL(4).
constexpr uint32_t kRetainedWrapperKindQuery =
    D9C_CHUNK_HANDLE_KIND_VERTEX_DECL + 1;

void retainWrapperByKind(dxmt9::d3d9::RawCommandChunk& raw, uint32_t kind, void* ptr) {
  if (!ptr) {
    return;
  }
  switch (kind) {
  case D9C_CHUNK_HANDLE_KIND_TEXTURE:
    dxmt9c_texture_addref(static_cast<D9CTexture*>(ptr));
    break;
  case D9C_CHUNK_HANDLE_KIND_SURFACE:
    dxmt9c_surface_addref(static_cast<D9CSurface*>(ptr));
    break;
  case D9C_CHUNK_HANDLE_KIND_BUFFER:
    dxmt9c_buffer_addref(static_cast<D9CBuffer*>(ptr));
    break;
  case D9C_CHUNK_HANDLE_KIND_SHADER:
    dxmt9c_shader_addref(static_cast<D9CShader*>(ptr));
    break;
  case D9C_CHUNK_HANDLE_KIND_VERTEX_DECL:
    dxmt9c_vdecl_addref(static_cast<D9CVertexDecl*>(ptr));
    break;
  case kRetainedWrapperKindQuery:
    dxmt9c_query_addref(static_cast<D9CQuery*>(ptr));
    break;
  default:
    return;
  }
  raw.retainedWrappers.push_back(dxmt9::d3d9::RetainedWireHandle{kind, ptr});
}

// vsHandle/psHandle/vdeclHandle are delta fields (valid only on the record
// where the shader/vdecl actually changed, per device_c.h), so this only
// retains what the record itself stamped -- matching PE's
// retainDrawPacketPayloadObjects, which has the same delta shape.
void retainDrawStateExtraWrappers(dxmt9::d3d9::RawCommandChunk& raw,
                                  const D9CDrawPrimitivePacket& state) {
  if (state.vsValid) {
    retainWrapperByKind(raw, D9C_CHUNK_HANDLE_KIND_SHADER,
                        wireHandlePtr<D9CShader>(state.vsHandle));
  }
  if (state.psValid) {
    retainWrapperByKind(raw, D9C_CHUNK_HANDLE_KIND_SHADER,
                        wireHandlePtr<D9CShader>(state.psHandle));
  }
  if (state.vdeclValid) {
    retainWrapperByKind(raw, D9C_CHUNK_HANDLE_KIND_VERTEX_DECL,
                        wireHandlePtr<D9CVertexDecl>(state.vdeclHandle));
  }
}

// Wrapper retention for the deferred offload path.
//
// dxmt9c_device_commit_chunk() returns as soon as a chunk is enqueued --
// well before the ReplayOffloadWorker thread actually replays it. On the PE
// side, appendRecordDirect()'s retainRecordPayloadObjects() /
// collectRecordPayloadWireHandles() (src/d3d9/d3d9_pe_recorder.hpp) addref
// every wrapper a record touches and PeCommandChunkBuilder::flush()
// releases them all the instant its commit() callback returns SUCCEEDED --
// which, for the offload path, is immediately after enqueue, not after
// replay. Anything only kept alive by that PE-side retention would then be
// a use-after-free on the worker thread.
//
// Verification (required by the Task 4 plan) against this file's own record
// dispatch below plus the PE-side collect/retain functions in
// d3d9_pe_recorder.hpp:
//   - STRETCH_RECT / COLOR_FILL / UPDATE_TEXTURE / UPDATE_SURFACE /
//     READBACK / RESZ_DEPTH_RESOLVE: despite resolving their src/dst
//     wrapper pointers directly from record-embedded fields via
//     wireValuePtr<>() (not via importedChunk.handles[] at the use site),
//     collectRecordPayloadWireHandles()'s cases for these exact record
//     types DO stamp a D9C_CHUNK_HANDLE_KIND_SURFACE/TEXTURE entry into the
//     shared wire handle table for each field at PE append time. So the
//     handle-table pass below re-derives and retains the same objects.
//   - DRAW_* / CLEAR "currently bound" resources (streams, textures, RTs,
//     DS, and the indexed-run IB via packet.ibHandle): also covered by the
//     handle table, via appendCurrentlyBoundDrawHandles()'s
//     "capture full state at append time" pass -- this is true even for
//     interior records of a multi-draw run, since each record is appended
//     (and thus handle-collected) individually before the run is replayed
//     as a batch.
//   - DRAW_* / APPLY_STATE shader (vsHandle/psHandle) and vertex-decl
//     (vdeclHandle) handles do NOT enter the handle table:
//     appendDrawPacketWireHandles() (what collectRecordPayloadWireHandles
//     uses for these record types) only stamps TEXTURE/BUFFER/SURFACE
//     entries. There is no core::Handle for shaders/vertex-decls, so the
//     cross-side generation-check loop above would reject a stamped
//     SHADER/VERTEX_DECL entry outright (see its "bad-handle-generation"
//     cases) -- producers must never emit one. PE-side retention is the
//     only thing keeping these alive between append and replay, so the
//     second (record-scan) pass below re-derives and retains them directly
//     from each record's own vsValid/psValid/vdeclValid delta fields.
//   - QUERY_ISSUE's queryWire is retained by NEITHER the handle table NOR
//     PE's retainRecordPayloadObjects (there is no case for
//     D9C_COMMAND_RECORD_QUERY_ISSUE in either PE-side function, and no
//     D9C_CHUNK_HANDLE_KIND for queries at all). This is a pre-existing gap
//     shared with the synchronous path -- a query Issue()'d then
//     immediately Release()'d before the pending chunk flushes is already
//     unsound there -- so it is retained here only defensively; it is not a
//     regression introduced by offload.
void retainWrappersForOffload(const ImportedWireChunkView& importedChunk,
                              dxmt9::d3d9::RawCommandChunk& raw) {
  for (uint32_t i = 0; i < importedChunk.handleCount; ++i) {
    const auto& entry = importedChunk.handles[i];
    if (entry.opaqueHandle == 0) {
      continue;
    }
    switch (entry.kind) {
    case D9C_CHUNK_HANDLE_KIND_TEXTURE:
      retainWrapperByKind(raw, entry.kind, wireValuePtr<D9CTexture>(entry.opaqueHandle));
      break;
    case D9C_CHUNK_HANDLE_KIND_SURFACE:
      retainWrapperByKind(raw, entry.kind, wireValuePtr<D9CSurface>(entry.opaqueHandle));
      break;
    case D9C_CHUNK_HANDLE_KIND_BUFFER:
      retainWrapperByKind(raw, entry.kind, wireValuePtr<D9CBuffer>(entry.opaqueHandle));
      break;
    default:
      // SHADER / VERTEX_DECL never populate this table (see above); the
      // record-scan pass below retains them instead.
      break;
    }
  }

  uint32_t recordIndex = 0;
  while (auto recordView = nextImportedRecord(importedChunk, recordIndex)) {
    if (!recordView->valid()) {
      break;
    }
    const auto* record = recordView->record;
    switch (recordView->header.type) {
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE: {
      D9CCommandRecordDrawPrimitive decoded{};
      std::memcpy(&decoded, record, sizeof(decoded));
      retainDrawStateExtraWrappers(raw, decoded.packet);
      break;
    }
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE: {
      D9CCommandRecordDrawIndexedPrimitive decoded{};
      std::memcpy(&decoded, record, sizeof(decoded));
      retainDrawStateExtraWrappers(raw, decoded.packet.state);
      break;
    }
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP: {
      D9CCommandRecordDrawPrimitiveUP decoded{};
      std::memcpy(&decoded, record, sizeof(decoded));
      retainDrawStateExtraWrappers(raw, decoded.packet.state);
      break;
    }
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP: {
      D9CCommandRecordDrawIndexedPrimitiveUP decoded{};
      std::memcpy(&decoded, record, sizeof(decoded));
      retainDrawStateExtraWrappers(raw, decoded.packet.state);
      break;
    }
    case D9C_COMMAND_RECORD_APPLY_STATE: {
      D9CCommandRecordApplyState decoded{};
      std::memcpy(&decoded, record, sizeof(decoded));
      retainDrawStateExtraWrappers(raw, decoded.packet);
      break;
    }
    case D9C_COMMAND_RECORD_QUERY_ISSUE: {
      D9CCommandRecordQueryIssue decoded{};
      std::memcpy(&decoded, record, sizeof(decoded));
      retainWrapperByKind(raw, kRetainedWrapperKindQuery,
                          wireValuePtr<D9CQuery>(decoded.queryWire));
      break;
    }
    default:
      break;
    }
    recordIndex = recordView->nextIndex();
  }
}

}  // namespace

// Declared in device_c_replay_offload.hpp (dxmt9::d3d9 namespace, external
// linkage) rather than kept file-local like its sibling
// retainWrappersForOffload() above, because device_c_replay_offload.cpp's
// ReplayOffloadWorker also needs to call this -- both the commit-branch
// push() failure path just below and the worker's fail-stop/teardown drain
// release a chunk's wrappers without ever having replayed it.
void dxmt9::d3d9::releaseRetainedWrappers(dxmt9::d3d9::RawCommandChunk& chunk) {
  for (const auto& entry : chunk.retainedWrappers) {
    switch (entry.kind) {
    case D9C_CHUNK_HANDLE_KIND_TEXTURE:
      dxmt9c_texture_release(static_cast<D9CTexture*>(entry.ptr));
      break;
    case D9C_CHUNK_HANDLE_KIND_SURFACE:
      dxmt9c_surface_release(static_cast<D9CSurface*>(entry.ptr));
      break;
    case D9C_CHUNK_HANDLE_KIND_BUFFER:
      dxmt9c_buffer_release(static_cast<D9CBuffer*>(entry.ptr));
      break;
    case D9C_CHUNK_HANDLE_KIND_SHADER:
      dxmt9c_shader_release(static_cast<D9CShader*>(entry.ptr));
      break;
    case D9C_CHUNK_HANDLE_KIND_VERTEX_DECL:
      dxmt9c_vdecl_release(static_cast<D9CVertexDecl*>(entry.ptr));
      break;
    case kRetainedWrapperKindQuery:
      dxmt9c_query_release(static_cast<D9CQuery*>(entry.ptr));
      break;
    default:
      break;
    }
  }
  chunk.retainedWrappers.clear();
}

namespace {

// Iterates once looking only for a Present record; short-circuits on the
// first hit. Present is typically the chunk's tail record, so this is
// usually O(1) in practice despite the general O(n) shape. Kept separate
// from summarizeNoEnqueueCommitChunkRecordShape() (which already exposes a
// presentRecords count) so the always-on completion-gap bookkeeping path
// stays untouched by the offload-only branch below.
bool importedChunkHasPresentRecord(const ImportedWireChunkView& importedChunk) {
  uint32_t recordIndex = 0;
  while (auto recordView = nextImportedRecord(importedChunk, recordIndex)) {
    if (!recordView->valid()) {
      break;
    }
    if (recordView->header.type == D9C_COMMAND_RECORD_PRESENT) {
      return true;
    }
    recordIndex = recordView->nextIndex();
  }
  return false;
}

}  // namespace

// Runs on the ReplayOffloadWorker thread (see device_c_replay_offload.hpp):
// rebuilds the wire view directly over the retained recordBlob copy (the
// original PE-side wire buffer this chunk was enqueued from is long gone by
// the time this runs), replays it through the same replayImportedChunk()
// the synchronous path uses, then releases the wrappers
// retainWrappersForOffload() addref'd at enqueue time -- on both the
// success and failure path, since a failed replay may still have resolved
// and used some of those wrappers.
int32_t dxmt9::d3d9::replayRawChunk(D9CDevice* d, dxmt9::d3d9::RawCommandChunk& chunk) {
  const auto replayStart = std::chrono::steady_clock::now();
  const auto wireBlob =
      makeImportedWireChunkBlobView(chunk.recordBlob.data(), chunk.recordBytes);
  if (!wireBlob.valid()) {
    releaseRetainedWrappers(chunk);
    return commitChunkFail("offload-bad-wire-blob", 0xffffffffu,
                           static_cast<std::uint32_t>(wireBlob.status));
  }
  // Reproduce the committing thread's wow64 client-call context: the
  // pointer-decode helpers consult a thread_local depth, and without it the
  // worker resolves unregistered 32-bit wire values as raw pointers.
  std::optional<ScopedWow64ClientCall> wow64Scope;
  if (chunk.wow64ClientCall) {
    wow64Scope.emplace();
  }
  const auto replayCpuStart = std::chrono::steady_clock::now();
  // R-BACK-2.51(g) — this worker thread only ever replays chunks that took
  // the offload branch in dxmt9c_device_commit_chunk, so any Present record
  // in this chunk was already paced by that branch's
  // waitPresentOrdinalBoundary() call; pass true so the eventual
  // submitPresent() skips its own inline boundary for it.
  const int32_t hr = replayImportedChunk(d, wireBlob.chunk, chunk.skipDrawResourceMarking,
                                        chunk.bridgeCommitStart, replayStart,
                                        /*pacedByPresentOrdinal=*/true);
  countDurationSince(replayCpuStart, dxmt9::perf::countOffloadReplayCpuTime);
  releaseRetainedWrappers(chunk);
  return hr;
}

extern "C" int32_t dxmt9c_device_commit_chunk(D9CDevice* d, const D9CCommandChunk* chunk) {
  // V1 boundary B2 — wall-clock latency of one commit_chunk bridge call.
  // This includes importer validation, handle/resource marking, record
  // replay, and queue submission construction. It excludes asynchronous
  // encode/GPU work after this call returns.
  const auto bridgeCommitStart = std::chrono::steady_clock::now();
  auto commitChunkStageStart = bridgeCommitStart;
  if (!d || !chunk || chunk->version != D9C_COMMAND_CHUNK_VERSION) {
    return commitChunkFail("bad-header");
  }
  if (peRecorderStatsEnabled()) {
    dxmt9::util::logf(dxmt9::util::LogLevel::Info, "dxmt9-device",
                      "unix_commit_chunk_entry device=%p native_tid=0x%llx "
                      "pthread_self=0x%llx recordCount=%u recordBytes=%u "
                      "handleCount=%u records=0x%llx handles=0x%llx",
                      static_cast<void*>(d),
                      static_cast<unsigned long long>(currentNativeThreadId()),
                      static_cast<unsigned long long>(currentPthreadSelfBits()),
                      chunk->recordCount, chunk->recordBytes, chunk->handleCount,
                      static_cast<unsigned long long>(wireHandleValue(chunk->records)),
                      static_cast<unsigned long long>(wireHandleValue(chunk->handles)));
  }
  if (auto* q = findDirtyQueue(d)) {
    q->noteCommitChunkEntryForCompletionGap();
  }
  const auto* records = chunk->recordBytes != 0
                            ? wireHandlePtr<const dxmt9::core::u8>(chunk->records)
                            : nullptr;
  if (!records && chunk->recordBytes != 0) {
    return commitChunkFail("missing-records");
  }

  ImportedWireChunkView importedChunk{};
  const auto wireBlob = makeImportedWireChunkBlobView(records, chunk->recordBytes);
  if (!wireBlob.valid()) {
    return commitChunkFail("bad-wire-blob", 0xffffffffu,
                           static_cast<std::uint32_t>(wireBlob.status));
  }
  importedChunk = wireBlob.chunk;
  if ((chunk->recordCount != 0 && importedChunk.recordCount != chunk->recordCount) ||
      (chunk->handleCount != 0 && importedChunk.handleCount != chunk->handleCount)) {
    return commitChunkFail("count-mismatch", importedChunk.recordCount,
                           importedChunk.handleCount);
  }

  const auto validation = validateImportedWireChunk(importedChunk);
  if (!validation.valid()) {
    return commitChunkFail("validation", validation.failedRecordIndex,
                           static_cast<std::uint32_t>(validation.status));
  }
  if (auto* q = findDirtyQueue(d)) {
    q->noteCommitChunkRecordShapeForCompletionGap(
        summarizeNoEnqueueCommitChunkRecordShape(importedChunk));
  }
  auto commitChunkStageEnd = std::chrono::steady_clock::now();
  dxmt9::perf::countCommitChunkImportCpuTime(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          commitChunkStageEnd - commitChunkStageStart).count()));
  commitChunkStageStart = commitChunkStageEnd;

  // Phase 4 / 18: validate and retain only handles selected by record
  // handle ranges.
  //
  // The wire payload from PE carries the SERVER-SIDE D9C wrapper
  // pointer (D9CTexture* / D9CBuffer* / D9CSurface*) cast to uint64,
  // not the backend's core::Handle. Decode each pointer to its
  // underlying core::*::handle() value before handing the entry list
  // to CommandQueue::markChunkResources — otherwise pool.find{Texture,
  // Surface,Buffer} on a wrapper-pointer-as-handle would never match
  // and the bulk mark would silently be a no-op.
  bool didBulkMarkResources = false;
  if (importedChunk.handleCount > 0) {
    // Cross-side generation check (wire-record bounds-checkable, per
    // `agents/rules/codebase_conventions.rules.md`): when a producer
    // stamped a non-NONE generation into the wire handle entry, decode
    // the wrapper's current `core::Handle.value` and verify the stamped
    // generation matches the encoded generation. A mismatch indicates a
    // zombie / use-after-free wrapper pointer (the slot was released and
    // re-used between PE record and unix import) and the chunk MUST be
    // rejected before any record is dispatched. NONE-stamped entries are
    // the legacy path; they pass through unchanged so the PE recorder's
    // existing opaque-pointer encoding still imports.
    for (std::uint32_t i = 0; i < importedChunk.handleCount; ++i) {
      const auto& entry = importedChunk.handles[i];
      if (entry.generation == D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_NONE) {
        continue;
      }
      if (entry.opaqueHandle == 0) {
        continue;
      }
      dxmt9::core::Handle resolved{};
      switch (entry.kind) {
      case D9C_CHUNK_HANDLE_KIND_TEXTURE: {
        auto* wrapper = wireValuePtr<D9CTexture>(entry.opaqueHandle);
        if (wrapper && wrapper->obj) resolved = wrapper->obj->handle();
        break;
      }
      case D9C_CHUNK_HANDLE_KIND_SURFACE: {
        auto* wrapper = wireValuePtr<D9CSurface>(entry.opaqueHandle);
        if (wrapper && wrapper->obj) resolved = wrapper->obj->handle();
        break;
      }
      case D9C_CHUNK_HANDLE_KIND_BUFFER: {
        auto* wrapper = wireValuePtr<D9CBuffer>(entry.opaqueHandle);
        if (wrapper && wrapper->obj) resolved = wrapper->obj->handle();
        break;
      }
      case D9C_CHUNK_HANDLE_KIND_SHADER:
      case D9C_CHUNK_HANDLE_KIND_VERTEX_DECL:
        // Shaders / vertex decls have no `core::Handle` representation
        // on the server side; the producer must not stamp a non-NONE
        // generation for these kinds. Treating any stamped value as a
        // mismatch keeps the importer honest about the supported set.
        return commitChunkFail("bad-handle-generation", i, entry.kind);
      default:
        return commitChunkFail("bad-handle-generation", i, entry.kind);
      }
      if (resolved.value == 0) {
        // Wrapper resolved to a null core handle — the producer stamped
        // a generation but the wrapper is no longer live. This is the
        // exact zombie case the cross-side check is meant to catch.
        return commitChunkFail("bad-handle-generation", i, entry.generation);
      }
      if (!d9c_command_chunk_wire_handle_generation_matches(
              entry.generation, resolved.value)) {
        return commitChunkFail("bad-handle-generation", i, entry.generation);
      }
    }
    ImportedChunkHandleSet retainedWireHandles;
    if (!collectImportedWireChunkHandles(importedChunk, retainedWireHandles)) {
      return commitChunkFail("collect-handles");
    }
    const auto retainedEntries = makeImportedChunkHandleEntries(retainedWireHandles);

    std::vector<dxmt9::core::ChunkHandleEntry> coreEntries;
    coreEntries.reserve(retainedEntries.size());
    for (const auto& handle : retainedEntries) {
      const auto kind = static_cast<dxmt9::core::ChunkHandleKind>(handle.kind);
      const auto wirePtr = handle.handle;
      if (wirePtr == 0) continue;
      dxmt9::core::Handle resolved{};
      switch (kind) {
      case dxmt9::core::ChunkHandleKind::Texture: {
        auto* wrapper = wireValuePtr<D9CTexture>(wirePtr);
        if (wrapper && wrapper->obj) resolved = wrapper->obj->handle();
        break;
      }
      case dxmt9::core::ChunkHandleKind::Surface: {
        auto* wrapper = wireValuePtr<D9CSurface>(wirePtr);
        if (wrapper && wrapper->obj) resolved = wrapper->obj->handle();
        break;
      }
      case dxmt9::core::ChunkHandleKind::Buffer: {
        auto* wrapper = wireValuePtr<D9CBuffer>(wirePtr);
        if (wrapper && wrapper->obj) resolved = wrapper->obj->handle();
        break;
      }
      case dxmt9::core::ChunkHandleKind::Shader:
      case dxmt9::core::ChunkHandleKind::VertexDecl:
        // No pool retention table for shaders / vertex decls — skip.
        break;
      }
      if (resolved.value == 0) continue;
      coreEntries.push_back(dxmt9::core::ChunkHandleEntry{
          .kind = kind,
          .handle = resolved,
      });
    }
    if (!coreEntries.empty()) {
      if (auto upper = d->dev().upperDevice()) {
        upper->markChunkResources(coreEntries);
        didBulkMarkResources = true;
      }
    }
  }

  commitChunkStageEnd = std::chrono::steady_clock::now();
  dxmt9::perf::countCommitChunkHandleCpuTime(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          commitChunkStageEnd - commitChunkStageStart).count()));
  commitChunkStageStart = commitChunkStageEnd;

  if (dxmt9::d3d9::offloadCommitReplayEnabled()) {
    // Raw-enqueue CPU time covers building + retaining + pushing the raw
    // chunk only. The present-ordinal boundary wait below already has its
    // own dedicated counters (countPresentOrdinalBoundaryWait /
    // countPresentOrdinalBoundaryWaitNs, added with
    // waitPresentOrdinalBoundary) and must not be folded into this one, or
    // a blocking frame-latency wait would masquerade as CPU work.
    const auto rawEnqueueStart = std::chrono::steady_clock::now();
    if (!d->replayOffload) {
      d->replayOffload = std::make_unique<dxmt9::d3d9::ReplayOffloadWorker>();
      d->replayOffload->start(d);
    }
    if (d->replayOffload->failed()) {
      return commitChunkFail("offload-worker-failed");
    }
    dxmt9::d3d9::RawCommandChunk raw;
    raw.recordBlob.assign(records, records + chunk->recordBytes);
    raw.recordCount = importedChunk.recordCount;
    raw.recordBytes = chunk->recordBytes;
    // Deliberately NOT didBulkMarkResources: the synchronous bulk mark pins
    // resources against nextSeqId_ observed on the app thread, but the worker
    // publishes this chunk's draws into a later slot once it runs ahead. The
    // worker's per-draw markDrawResources must therefore re-pin at the real
    // append-time seqId, or a resource released in its final frame could be
    // reclaimed/recycled before the deferred replay's GPU use completes. The
    // sync bulk mark plus the wrapper addrefs still cover liveness up to the
    // deferred replay itself.
    raw.skipDrawResourceMarking = false;
    raw.wow64ClientCall = requiresWow64PointerShadow();
    raw.bridgeCommitStart = bridgeCommitStart;
    raw.hasPresent = importedChunkHasPresentRecord(importedChunk);
    retainWrappersForOffload(importedChunk, raw);
    const bool hasPresent = raw.hasPresent;  // read before std::move(raw) below.
    dxmt9::perf::countOffloadReplayQueueDepth(
        static_cast<std::uint64_t>(d->replayOffload->queue().depth()));
    // This scope includes potential raw-queue push backpressure (blocking
    // on ReplayOffloadQueue's spaceCv_ while the ring is full) but still
    // excludes the present-ordinal boundary wait below, which is measured
    // separately.
    const bool pushed = d->replayOffload->queue().push(std::move(raw));
    countDurationSince(rawEnqueueStart, dxmt9::perf::countCommitChunkRawEnqueueCpuTime);
    if (!pushed) {
      // push() guarantees it does not move from `raw` on the false path
      // (see ReplayOffloadQueue::push doc in device_c_replay_offload.hpp),
      // so `raw` still owns every wrapper retainWrappersForOffload() just
      // addref'd above; release them here or they leak forever since this
      // chunk will never reach replayRawChunk()'s own release call.
      dxmt9::d3d9::releaseRetainedWrappers(raw);
      return commitChunkFail("offload-queue-stopped");
    }
    if (hasPresent) {
      ++d->presentOrdinal;
      if (auto upper = d->dev().upperDevice()) {
        // R-BACK-2.51: honor DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS the same
        // way the inline seqId-based boundary's presentBoundaryLatency()
        // does. The current swapchain's backBufferCount is available here
        // via presentParameters() -- the same value snapshotSwapDesc() will
        // embed into the SwapDesc this chunk's Present record eventually
        // replays through.
        upper->waitPresentOrdinalBoundary(
            d->presentOrdinal, d->dev().presentParameters().backBufferCount);
      }
    }
    // App-thread commit wall for the offload branch (validation/import +
    // copy/retain/push incl. backpressure + ordinal wait). This — not
    // bridge_commit_latency, which the deferred replay closes at
    // worker-replay end and therefore measures commit->replay pipeline
    // latency — is the producer-serial cost the offload is meant to shrink.
    countDurationSince(bridgeCommitStart,
                       dxmt9::perf::countOffloadCommitAppCpuTime);
    return dxmt9::core::D3D_OK;
  }

  // R-BACK-2.51(g) — this synchronous tail is only reached when
  // dxmt9::d3d9::offloadCommitReplayEnabled() was off above (the offload
  // branch returns early), so no present-ordinal wait ran for this chunk;
  // any Present record in it must keep the inline seqId-based boundary.
  return replayImportedChunk(d, importedChunk, didBulkMarkResources,
                             bridgeCommitStart, commitChunkStageStart,
                             /*pacedByPresentOrdinal=*/false);
}

extern "C" int32_t dxmt9c_device_draw_primitive_packet(D9CDevice* d,
                                                       const D9CDrawPrimitivePacket* packet) {
  if (!d || !packet) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  return applyDrawPrimitivePacket(d, *packet);
}

extern "C" int32_t dxmt9c_device_draw_primitive_chunk(D9CDevice* d,
                                                      const D9CDrawPrimitivePacket* packets,
                                                      uint32_t packetCount) {
  if (!d || (!packets && packetCount != 0)) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  for (uint32_t i = 0; i < packetCount; ++i) {
    const int32_t hr = applyDrawPrimitivePacket(d, packets[i]);
    if (failed(hr)) {
      return hr;
    }
  }
  return dxmt9::core::D3D_OK;
}
