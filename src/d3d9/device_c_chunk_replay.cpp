// Batched-chunk C ABI: dxmt9c_device_commit_chunk and the packet->state
// replay machinery it invokes. Per-call dxmt9c_* setters live in
// device_c_draw.cpp (interactive entry points) and device_c_state.cpp
// (state setters); both are referenced via extern "C" forward decls.

#include "device_c_provider.hpp"
#include "device_c_chunk_v2_replay.hpp"
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
// device_c_state.cpp. The chunk importer (dxmt9c_device_commit_chunk) calls
// these to dispatch per-record state deltas; the fat-packet applier chain that
// used to share them is gone (see the tombstone below). The provider macro renames apply uniformly via
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
extern "C" int32_t dxmt9c_device_set_render_state(D9CDevice* d, uint32_t state,
                                                   uint32_t value);
extern "C" int32_t dxmt9c_device_set_fvf(D9CDevice* d, uint32_t fvf);

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
extern "C" int32_t dxmt9c_device_set_texture(D9CDevice* d, uint32_t stage,
                                              D9CTexture* texture);
extern "C" int32_t dxmt9c_device_draw_primitive(D9CDevice* d, uint32_t type,
                                                 uint32_t startVertex,
                                                 uint32_t count);
extern "C" int32_t dxmt9c_device_draw_indexed_primitive(
    D9CDevice* d, uint32_t type, int32_t baseVertex, uint32_t minVertex,
    uint32_t numVertices, uint32_t startIndex, uint32_t count);
extern "C" int32_t dxmt9c_device_draw_primitive_up(
    D9CDevice* d, uint32_t type, uint32_t count, const void* data,
    uint32_t stride);
extern "C" int32_t dxmt9c_device_draw_indexed_primitive_up(
    D9CDevice* d, uint32_t type, uint32_t minVertex, uint32_t numVertices,
    uint32_t count, const void* indexData, uint32_t indexFormat,
    const void* vertexData, uint32_t stride);
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

struct ReplayScratchArena {
  std::vector<dxmt9::core::DrawRunSubmission> submissions;
  std::vector<dxmt9::core::DrawParam> runParams;
  std::vector<dxmt9::core::DrawBindingOverride> bindingOverrides;
  std::vector<dxmt9::core::DrawParamPayloadView> runPayloads;
  std::vector<dxmt9::core::ChunkHandleEntry> coreEntries;
  bool inUse = false;

  void clear() noexcept {
    submissions.clear();
    runParams.clear();
    bindingOverrides.clear();
    runPayloads.clear();
    coreEntries.clear();
  }
};

ReplayScratchArena& replayScratchArena() {
  static thread_local ReplayScratchArena scratch;
  return scratch;
}

class ScopedReplayScratchUse {
public:
  explicit ScopedReplayScratchUse(ReplayScratchArena& scratch) noexcept
      : scratch_(scratch) {
    DXMT_ASSERT(!scratch_.inUse);
    scratch_.inUse = true;
    scratch_.clear();
  }

  ~ScopedReplayScratchUse() {
    scratch_.clear();
    scratch_.inUse = false;
  }

  ScopedReplayScratchUse(const ScopedReplayScratchUse&) = delete;
  ScopedReplayScratchUse& operator=(const ScopedReplayScratchUse&) = delete;

private:
  ReplayScratchArena& scratch_;
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

uint64_t wireHandleValue(const D9CWireHandle& handle) {
  return static_cast<uint64_t>(handle.lo) |
         (static_cast<uint64_t>(handle.hi) << 32);
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



bool failed(int32_t hr) {
  return hr < 0;
}

void countDurationSince(std::chrono::steady_clock::time_point start,
                        void (*counter)(std::uint64_t)) {
  counter(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - start).count()));
}

// Heavy opt-in phase split of the synchronous half of commit_chunk. Off by
// default because it adds five clock pairs to a call that runs tens of times
// per present; see state-churn-encode-append-decomposition.05 for why the
// split is wanted at all (69-72% of that half is fixed per call, and which
// phase owns the fixed term is not otherwise observable).
bool commitChunkPhaseSplitEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_COMMIT_CHUNK_PHASE_SPLIT");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

// Scope that costs one branch when the split is off. Deliberately not a
// constructor/destructor pair: two of the five phases end in an early return,
// and a destructor would time the unwind rather than the phase.
struct CommitChunkPhaseTimer {
  bool enabled = false;
  std::chrono::steady_clock::time_point t0{};

  explicit CommitChunkPhaseTimer(bool on) : enabled(on) {
    if (enabled) {
      t0 = std::chrono::steady_clock::now();
    }
  }
  void stop(void (*counter)(std::uint64_t)) {
    if (!enabled) {
      return;
    }
    enabled = false;
    countDurationSince(t0, counter);
  }
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

// The fat-packet applier chain lived here: applyDrawPacketStateViaIface,
// applyDrawPacketStateDirect, applyDrawPacketState, timedApplyDrawPacketState and
// applyDrawPrimitivePacket, plus validateDrawPacketStateDelta and
// commitChunkDrawDeltaMask below them. All of it served
// dxmt9c_device_draw_primitive_packet / _chunk, the two direct fat-packet bridge
// ops, which had no PE-side caller; the chunk path has rejected any wire version
// other than V2 since long before this deletion. Task 10 stage D removed the
// whole component.

}  // namespace

// V2 admission retains every resolved wrapper before inline or offloaded
// replay. Both replay completion and queue teardown release the same list.
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
    case D9C_CHUNK_HANDLE_KIND_QUERY:
      dxmt9c_query_release(static_cast<D9CQuery*>(entry.ptr));
      break;
    default:
      break;
    }
  }
  chunk.retainedWrappers.clear();
}

namespace {

void retainV2Wrapper(std::uint32_t kind, void* object) noexcept {
  if (!object) return;
  switch (kind) {
  case D9C_CHUNK_HANDLE_KIND_TEXTURE:
    dxmt9c_texture_addref(static_cast<D9CTexture*>(object));
    break;
  case D9C_CHUNK_HANDLE_KIND_SURFACE:
    dxmt9c_surface_addref(static_cast<D9CSurface*>(object));
    break;
  case D9C_CHUNK_HANDLE_KIND_BUFFER:
    dxmt9c_buffer_addref(static_cast<D9CBuffer*>(object));
    break;
  case D9C_CHUNK_HANDLE_KIND_SHADER:
    dxmt9c_shader_addref(static_cast<D9CShader*>(object));
    break;
  case D9C_CHUNK_HANDLE_KIND_VERTEX_DECL:
    dxmt9c_vdecl_addref(static_cast<D9CVertexDecl*>(object));
    break;
  case D9C_CHUNK_HANDLE_KIND_QUERY:
    dxmt9c_query_addref(static_cast<D9CQuery*>(object));
    break;
  default:
    break;
  }
}

class DeviceReplaySinkV2 final : public dxmt9::d3d9::NonDrawReplaySinkV2,
                                 public dxmt9::d3d9::SparseReplaySinkV2 {
public:
  DeviceReplaySinkV2(
      D9CDevice* device, bool pacedByPresentOrdinal,
      std::vector<dxmt9::core::DrawRunSubmission>* pendingDrawSubmissions)
      : device_(device),
        pacedByPresentOrdinal_(pacedByPresentOrdinal),
        pendingDrawSubmissions_(pendingDrawSubmissions) {}

  void setBatchCurrentDraw(bool value) noexcept {
    batchCurrentDraw_ = value;
  }

  std::int32_t setConstants(
      std::uint32_t type, const D9CCommandChunkWireSetConstV2& fixed,
      std::span<const std::byte> bytes) override {
    return setConstantBytes(type, fixed.startRegister, fixed.registerCount,
                            bytes);
  }

  std::int32_t clear(const D9CCommandChunkWireClearV2& fixed,
                     std::span<const D9CRect> rects) override {
    return dxmt9c_device_clear(device_, fixed.rectCount, rects.data(),
                               fixed.flags, fixed.colorARGB, fixed.z,
                               fixed.stencil);
  }

  std::int32_t present(
      const D9CCommandChunkWirePresentV2& fixed) override {
    device_->dev().setNextPresentPacedByOrdinal(pacedByPresentOrdinal_);
    return dxmt9c_device_present(device_, fixed.hasSrc ? &fixed.src : nullptr,
                                 fixed.hasDst ? &fixed.dst : nullptr,
                                 fixed.hwnd, nullptr, fixed.flags);
  }

  std::int32_t stretchRect(
      const D9CCommandChunkWireStretchRectV2& fixed, void* src,
      void* dst) override {
    return dxmt9c_device_stretch_rect(
        device_, static_cast<D9CSurface*>(src),
        fixed.hasSrcRect ? &fixed.srcRect : nullptr,
        static_cast<D9CSurface*>(dst),
        fixed.hasDstRect ? &fixed.dstRect : nullptr, fixed.filter);
  }

  std::int32_t colorFill(
      const D9CCommandChunkWireColorFillV2& fixed, void* surface) override {
    return dxmt9c_device_color_fill(
        device_, static_cast<D9CSurface*>(surface),
        fixed.hasRect ? &fixed.rect : nullptr, fixed.colorARGB);
  }

  std::int32_t updateTexture(
      const D9CCommandChunkWireUpdateTextureV2&, void* src,
      void* dst) override {
    return dxmt9c_device_update_texture(device_, static_cast<D9CTexture*>(src),
                                        static_cast<D9CTexture*>(dst));
  }

  std::int32_t updateSurface(
      const D9CCommandChunkWireUpdateSurfaceV2& fixed, void* src,
      void* dst) override {
    return dxmt9c_device_update_surface(
        device_, static_cast<D9CSurface*>(src),
        fixed.hasSrcRect ? &fixed.srcRect : nullptr,
        static_cast<D9CSurface*>(dst),
        fixed.hasDstPoint ? &fixed.dstPoint : nullptr);
  }

  std::int32_t queryIssue(
      const D9CCommandChunkWireQueryIssueV2& fixed, void* query) override {
    return dxmt9c_query_issue(static_cast<D9CQuery*>(query), fixed.flags);
  }

  std::int32_t readback(
      const D9CCommandChunkWireReadbackV2&, void* src, void* dst) override {
    return dxmt9c_device_get_render_target_data(
        device_, static_cast<D9CSurface*>(src),
        static_cast<D9CSurface*>(dst));
  }

  std::int32_t reszDepthResolve(
      const D9CCommandChunkWireReszDepthResolveV2&, void* msaaDepth,
      void* intzDest) override {
    auto* surface = static_cast<D9CSurface*>(msaaDepth);
    auto* texture = static_cast<D9CTexture*>(intzDest);
    return surface && texture
               ? device_->dev().reszDepthResolve(surface->obj, texture->obj)
               : dxmt9::core::D3D_OK;
  }

  std::int32_t applyState(
      const dxmt9::d3d9::ResolvedRecordV2View& record) override {
    return dxmt9::d3d9::replaySparseRecordV2(record, *this);
  }

  std::int32_t setRenderStates(
      std::span<const D9CCommandChunkWireRenderStateV2> values) override {
    for (const auto& value : values) {
      const auto hr = dxmt9c_device_set_render_state(
          device_, value.state, value.value);
      if (failed(hr)) return hr;
      if (auto* queue = findDirtyQueue(device_)) {
        if (isFogRenderState(value.state)) queue->applyDirtyRenderStateFog();
        if (isAlphaRenderState(value.state)) queue->applyDirtyRenderStateAlpha();
        if (value.state == dxmt9::core::RS_TEXTURE_FACTOR) {
          queue->applyDirtyRenderStateTexFactor();
        }
      }
    }
    return dxmt9::core::D3D_OK;
  }

  std::int32_t setTexture(std::uint32_t slot, void* texture) override {
    return dxmt9c_device_set_texture(
        device_, slot, static_cast<D9CTexture*>(texture));
  }

  std::int32_t setStream(
      const D9CCommandChunkWireStreamBindingV2& value,
      void* buffer) override {
    return dxmt9c_device_set_stream_source(
        device_, value.slot, static_cast<D9CBuffer*>(buffer), value.offset,
        value.stride);
  }

  std::int32_t setShader(std::uint32_t stage, void* shader) override {
    return stage == D9C_COMMAND_CHUNK_V2_SHADER_STAGE_VERTEX
               ? dxmt9c_device_set_vertex_shader(
                     device_, static_cast<D9CShader*>(shader))
               : dxmt9c_device_set_pixel_shader(
                     device_, static_cast<D9CShader*>(shader));
  }

  std::int32_t setVertexInput(std::uint32_t kind, std::uint32_t value,
                              void* declaration) override {
    if (kind == D9C_COMMAND_CHUNK_V2_VERTEX_INPUT_FVF) {
      return dxmt9c_device_set_fvf(device_, value);
    }
    const auto fvfHr = dxmt9c_device_set_fvf(device_, value);
    return failed(fvfHr)
               ? fvfHr
               : dxmt9c_device_set_vertex_declaration(
                     device_, static_cast<D9CVertexDecl*>(declaration));
  }

  std::int32_t setIndexBuffer(void* buffer) override {
    return dxmt9c_device_set_indices(device_, static_cast<D9CBuffer*>(buffer));
  }

  std::int32_t setRenderTarget(std::uint32_t slot, void* surface) override {
    return dxmt9c_device_set_render_target(
        device_, slot, static_cast<D9CSurface*>(surface));
  }

  std::int32_t setDepthStencil(void* surface) override {
    return dxmt9c_device_set_depth_stencil(
        device_, static_cast<D9CSurface*>(surface));
  }

  std::int32_t setViewport(const D9CViewport& value) override {
    if (auto* queue = findDirtyQueue(device_)) {
      queue->applyDirtyViewportChange();
    }
    return dxmt9c_device_set_viewport(device_, &value);
  }

  std::int32_t setScissor(const D9CRect& value) override {
    return dxmt9c_device_set_scissor_rect(device_, &value);
  }

  std::int32_t setMaterial(const D9CMaterial& value) override {
    if (auto* queue = findDirtyQueue(device_)) {
      queue->applyDirtyTransformChange();
    }
    return dxmt9c_device_set_material(device_, &value);
  }

  std::int32_t setClipPlane(
      const D9CCommandChunkWireClipPlaneV2& value) override {
    if (auto* queue = findDirtyQueue(device_)) {
      queue->applyDirtyClipPlaneChange();
    }
    return dxmt9c_device_set_clip_plane(device_, value.slot, value.values);
  }

  std::int32_t setTextureStageStates(
      std::span<const D9CDrawPacketTextureStageState> values) override {
    for (const auto& value : values) {
      const auto hr = dxmt9c_device_set_texture_stage_state(
          device_, value.stage, value.type, value.value);
      if (failed(hr)) return hr;
      if (value.type == dxmt9::core::TSS_CONSTANT) {
        if (auto* queue = findDirtyQueue(device_)) {
          queue->applyDirtyTextureStageConstant();
        }
      }
    }
    return dxmt9::core::D3D_OK;
  }

  std::int32_t setSamplerStates(
      std::span<const D9CDrawPacketSamplerState> values) override {
    for (const auto& value : values) {
      const auto hr = dxmt9c_device_set_sampler_state(
          device_, value.sampler, value.type, value.value);
      if (failed(hr)) return hr;
    }
    return dxmt9::core::D3D_OK;
  }

  std::int32_t setTransforms(
      std::span<const D9CDrawPacketTransform> values) override {
    if (!values.empty()) {
      if (auto* queue = findDirtyQueue(device_)) {
        queue->applyDirtyTransformChange();
      }
    }
    for (const auto& value : values) {
      const auto hr = dxmt9c_device_set_transform(
          device_, value.state, &value.matrix);
      if (failed(hr)) return hr;
    }
    return dxmt9::core::D3D_OK;
  }

  std::int32_t setLights(
      std::span<const D9CCommandChunkWireLightV2> values) override {
    if (!values.empty()) {
      if (auto* queue = findDirtyQueue(device_)) {
        queue->applyDirtyTransformChange();
      }
    }
    for (const auto& value : values) {
      const auto hr = dxmt9c_device_set_light(
          device_, value.slot, &value.light);
      if (failed(hr)) return hr;
    }
    return dxmt9::core::D3D_OK;
  }

  std::int32_t setLightEnables(
      std::span<const D9CCommandChunkWireLightEnableV2> values) override {
    if (!values.empty()) {
      if (auto* queue = findDirtyQueue(device_)) {
        queue->applyDirtyTransformChange();
      }
    }
    for (const auto& value : values) {
      const auto hr = dxmt9c_device_light_enable(
          device_, value.slot, value.enabled);
      if (failed(hr)) return hr;
    }
    return dxmt9::core::D3D_OK;
  }

  std::int32_t setConstants(
      std::uint16_t sectionKind,
      const D9CCommandChunkWireConstantRangeV2& range,
      std::span<const std::byte> bytes) override {
    std::uint32_t type = 0u;
    switch (sectionKind) {
    case D9C_COMMAND_CHUNK_V2_SECTION_VS_CONST_F:
      type = D9C_COMMAND_RECORD_SET_VS_CONST_F;
      break;
    case D9C_COMMAND_CHUNK_V2_SECTION_VS_CONST_I:
      type = D9C_COMMAND_RECORD_SET_VS_CONST_I;
      break;
    case D9C_COMMAND_CHUNK_V2_SECTION_VS_CONST_B:
      type = D9C_COMMAND_RECORD_SET_VS_CONST_B;
      break;
    case D9C_COMMAND_CHUNK_V2_SECTION_PS_CONST_F:
      type = D9C_COMMAND_RECORD_SET_PS_CONST_F;
      break;
    case D9C_COMMAND_CHUNK_V2_SECTION_PS_CONST_I:
      type = D9C_COMMAND_RECORD_SET_PS_CONST_I;
      break;
    case D9C_COMMAND_CHUNK_V2_SECTION_PS_CONST_B:
      type = D9C_COMMAND_RECORD_SET_PS_CONST_B;
      break;
    default:
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    return setConstantBytes(type, range.startRegister, range.registerCount,
                            bytes);
  }

  std::int32_t finishApplyState(std::uint32_t) override {
    return dxmt9::core::D3D_OK;
  }

  std::int32_t draw(const dxmt9::d3d9::SparseDrawCallV2& call) override {
    if (!call.payload.userIndexData.empty()) {
      return device_->dev().drawIndexedPrimitiveUP(
          call.param.primitiveType, call.param.primitiveCount,
          call.payload.userVertexData, call.payload.userIndexData,
          call.param.indexType, call.stride);
    }
    if (!call.payload.userVertexData.empty()) {
      return device_->dev().drawPrimitiveUP(
          call.param.primitiveType, call.param.primitiveCount,
          call.payload.userVertexData, call.stride);
    }
    auto draw = call.param;
    if (draw.indexed) {
      draw.indexType = device_->dev().state().indexType;
    }
    if (batchCurrentDraw_ && pendingDrawSubmissions_) {
      const std::size_t previousIndex = pendingDrawSubmissions_->size();
      auto& submission = pendingDrawSubmissions_->emplace_back();
      const auto* previous =
          previousIndex == 0u
              ? nullptr
              : &(*pendingDrawSubmissions_)[previousIndex - 1u];
      const auto hr = device_->dev().snapshotDrawSubmissionFromCurrentState(
          draw, submission, previous);
      if (failed(hr)) {
        pendingDrawSubmissions_->pop_back();
      }
      return hr;
    }
    if (draw.indexed) {
      return device_->dev().drawIndexedPrimitive(
          draw.primitiveType, draw.primitiveCount, 0,
          draw.baseVertexIndex, draw.startIndex, draw.indexType);
    }
    return device_->dev().drawPrimitive(
        draw.primitiveType, draw.primitiveCount, draw.startVertex);
  }

private:
  std::int32_t setConstantBytes(std::uint32_t type, std::uint32_t start,
                                std::uint32_t count,
                                std::span<const std::byte> bytes) {
    const void* data = bytes.data();
    switch (type) {
    case D9C_COMMAND_RECORD_SET_VS_CONST_F:
      return dxmt9c_device_set_vs_const_f(
          device_, start, static_cast<const float*>(data), count);
    case D9C_COMMAND_RECORD_SET_VS_CONST_I:
      return dxmt9c_device_set_vs_const_i(
          device_, start, static_cast<const std::int32_t*>(data), count);
    case D9C_COMMAND_RECORD_SET_VS_CONST_B:
      return dxmt9c_device_set_vs_const_b(
          device_, start, static_cast<const std::uint32_t*>(data), count);
    case D9C_COMMAND_RECORD_SET_PS_CONST_F:
      return dxmt9c_device_set_ps_const_f(
          device_, start, static_cast<const float*>(data), count);
    case D9C_COMMAND_RECORD_SET_PS_CONST_I:
      return dxmt9c_device_set_ps_const_i(
          device_, start, static_cast<const std::int32_t*>(data), count);
    case D9C_COMMAND_RECORD_SET_PS_CONST_B:
      return dxmt9c_device_set_ps_const_b(
          device_, start, static_cast<const std::uint32_t*>(data), count);
    default:
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
  }

  D9CDevice* device_ = nullptr;
  bool pacedByPresentOrdinal_ = false;
  std::vector<dxmt9::core::DrawRunSubmission>* pendingDrawSubmissions_ = nullptr;
  bool batchCurrentDraw_ = false;
};

bool v2RecordCanBatchDraw(
    D9CDevice* device,
    const dxmt9::d3d9::ImportedRecordV2View& record) noexcept {
  if (!device || device->stateBlockRecording ||
      (record.header.type != D9C_COMMAND_RECORD_DRAW_PRIMITIVE &&
       record.header.type != D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE)) {
    return false;
  }
  return static_cast<dxmt9::core::PrimitiveType>(
             record.drawHeader.primitiveType - 1u) !=
         dxmt9::core::PrimitiveType::TriangleFan;
}

int32_t replayResolvedV2Chunk(
    D9CDevice* device, dxmt9::d3d9::RawCommandChunk& raw,
    bool pacedByPresentOrdinal) {
  const auto bytes = std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(raw.recordBlob.data()),
      raw.recordBlob.size());
  dxmt9::d3d9::ImportedChunkV2View imported;
  const dxmt9::d3d9::V2ChunkEnvelope envelope{
      .version = raw.wireVersion,
      .recordCount = raw.recordCount,
      .handleCount = raw.handleCount,
  };
  if (!raw.preflightValidated ||
      !dxmt9::d3d9::importPrevalidatedCommandChunkV2(
          bytes, envelope, imported) ||
      raw.resolvedObjects.size() != imported.handles.size()) {
    return commitChunkFail("v2-replay-preflight-view");
  }

  dxmt9::d3d9::ResolvedChunkV2View resolved{
      .wire = imported,
      .objects = raw.resolvedObjects,
  };
  auto& replayScratch = replayScratchArena();
  ScopedReplayScratchUse replayScratchUse(replayScratch);
  auto& pendingDrawSubmissions = replayScratch.submissions;
  pendingDrawSubmissions.reserve(
      std::min<std::uint32_t>(raw.recordCount, 256u));
  const auto flushPendingDrawSubmissions = [&]() {
    if (pendingDrawSubmissions.empty()) {
      return;
    }
    dxmt9::perf::countCommitChunkDrawSubmissionBatch(
        static_cast<std::uint32_t>(pendingDrawSubmissions.size()));
    device->dev().submitDrawSubmissionBatch(pendingDrawSubmissions);
    pendingDrawSubmissions.clear();
  };
  DeviceReplaySinkV2 sink(
      device, pacedByPresentOrdinal, &pendingDrawSubmissions);
  for (std::size_t index = 0u; index < imported.records.size(); ++index) {
    const auto record = resolved.record(index);
    const bool batchableDraw = v2RecordCanBatchDraw(device, record.wire);
    sink.setBatchCurrentDraw(batchableDraw);
    if (!batchableDraw &&
        !commitChunkRecordAllowsPendingDrawBatchThrough(
            record.wire.header.type)) {
      flushPendingDrawSubmissions();
    }
    const auto hr = dxmt9::d3d9::isSparseRecordV2(record.wire.header.type)
                        ? dxmt9::d3d9::replaySparseRecordV2(record, sink)
                        : dxmt9::d3d9::replayNonDrawRecordV2(record, sink);
    if (failed(hr)) {
      flushPendingDrawSubmissions();
      return commitChunkFail("v2-replay", static_cast<std::uint32_t>(index),
                             record.wire.header.type, hr);
    }
  }
  flushPendingDrawSubmissions();
  dxmt9::perf::countChunkAdmit();
  return dxmt9::core::D3D_OK;
}

void markResolvedV2Resources(
    D9CDevice* device, const dxmt9::d3d9::RawCommandChunk& raw,
    const dxmt9::d3d9::ImportedChunkV2View& imported) {
  auto& scratch = replayScratchArena();
  ScopedReplayScratchUse scratchUse(scratch);
  for (std::size_t i = 0u; i < imported.handles.size(); ++i) {
    dxmt9::core::Handle handle{};
    switch (imported.handles[i].kind) {
    case D9C_CHUNK_HANDLE_KIND_TEXTURE: {
      auto* value = static_cast<D9CTexture*>(raw.resolvedObjects[i]);
      if (value && value->obj) handle = value->obj->handle();
      break;
    }
    case D9C_CHUNK_HANDLE_KIND_SURFACE: {
      auto* value = static_cast<D9CSurface*>(raw.resolvedObjects[i]);
      if (value && value->obj) handle = value->obj->handle();
      break;
    }
    case D9C_CHUNK_HANDLE_KIND_BUFFER: {
      auto* value = static_cast<D9CBuffer*>(raw.resolvedObjects[i]);
      if (value && value->obj) handle = value->obj->handle();
      break;
    }
    default:
      break;
    }
    if (handle.value == 0u) continue;
    const auto kind = static_cast<dxmt9::core::ChunkHandleKind>(
        imported.handles[i].kind);
    const bool duplicate = std::any_of(
        scratch.coreEntries.begin(), scratch.coreEntries.end(),
        [&](const dxmt9::core::ChunkHandleEntry& existing) {
          return existing.kind == kind && existing.handle == handle;
        });
    if (!duplicate) {
      scratch.coreEntries.push_back({.kind = kind, .handle = handle});
    }
  }
  if (!scratch.coreEntries.empty()) {
    if (auto upper = device->dev().upperDevice()) {
      upper->markChunkResources(scratch.coreEntries);
    }
  }
}

bool v2ChunkRequiresInlineReplay(
    const dxmt9::d3d9::ImportedChunkV2View& imported) noexcept {
  return std::any_of(
      imported.records.begin(), imported.records.end(),
      [](const D9CCommandChunkWireRecordHeaderV2& record) {
        return replayInfoForCommandRecordType(record.type)
            .synchronousReadBoundary;
      });
}

}  // namespace

// Runs on the ReplayOffloadWorker thread. V2 admission has already validated
// the blob and resolved/retained every object, so the worker replays only that
// owned representation and releases wrappers on both success and failure.
int32_t dxmt9::d3d9::replayRawChunk(D9CDevice* d, dxmt9::d3d9::RawCommandChunk& chunk) {
  if (chunk.wireVersion != D9C_COMMAND_CHUNK_VERSION_V2) {
    releaseRetainedWrappers(chunk);
    dxmt9::perf::countCommandChunkV2Reject();
    return commitChunkFail("offload-unsupported-wire-version");
  }
  const auto replayCpuStart = std::chrono::steady_clock::now();
  const int32_t hr = replayResolvedV2Chunk(
      d, chunk, /*pacedByPresentOrdinal=*/true);
  countDurationSince(replayCpuStart, dxmt9::perf::countOffloadReplayCpuTime);
  releaseRetainedWrappers(chunk);
  if (failed(hr)) {
    dxmt9::perf::countCommandChunkV2Reject();
  }
  return hr;
}

extern "C" int32_t dxmt9c_device_commit_chunk(D9CDevice* d, const D9CCommandChunk* chunk) {
  // Boundary B2 — wall-clock latency of one commit_chunk bridge call.
  // This includes importer validation, handle/resource marking, record
  // replay, and queue submission construction. It excludes asynchronous
  // encode/GPU work after this call returns.
  const auto bridgeCommitStart = std::chrono::steady_clock::now();
  if (!d || !chunk) {
    return commitChunkFail("bad-header");
  }
#if defined(__APPLE__)
  // The Metal System Trace CPU sidecar
  // (scripts/tools/summarize_xctrace_cpu_threads.py) resolves the P4 producer
  // thread by scanning the run log for this line. Its only other selector is
  // the PE fallback, which reports a Win32 thread id; xctrace reports native
  // Mach thread ids, so that namespace mismatch makes the producer wait/hold
  // verdict unresolvable. pthread_threadid_np yields the same 64-bit id
  // Instruments displays, which is what closes the correlation.
  //
  // commit_chunk runs thousands of times per second, so this is latched once
  // per thread and gated on the already-computed log level: with Info logging
  // off the hot-path cost is one thread-local load plus one level check, and
  // the log itself cannot perturb the timing the sidecar exists to measure.
  static thread_local bool loggedNativeThreadId = false;
  if (!loggedNativeThreadId &&
      dxmt9::util::shouldLog(dxmt9::util::LogLevel::Info)) {
    loggedNativeThreadId = true;
    std::uint64_t nativeThreadId = 0;
    pthread_threadid_np(nullptr, &nativeThreadId);
    dxmt9::util::logf(dxmt9::util::LogLevel::Info, "dxmt9-device",
                      "unix_commit_chunk_entry device=%p native_tid=0x%llx",
                      static_cast<const void*>(d),
                      static_cast<unsigned long long>(nativeThreadId));
  }
#endif
  if (chunk->version != D9C_COMMAND_CHUNK_VERSION_V2) {
    dxmt9::perf::countCommandChunkV2Reject();
    return commitChunkFail("unsupported-wire-version", chunk->version);
  }
  {
    if (wireHandleValue(chunk->handles) != 0u ||
        chunk->recordBytes == 0u) {
      dxmt9::perf::countCommandChunkV2Reject();
      return commitChunkFail("v2-bad-outer");
    }
    const auto* records = wireHandlePtr<const std::byte>(chunk->records);
    if (!records) {
      dxmt9::perf::countCommandChunkV2Reject();
      return commitChunkFail("v2-missing-records");
    }
    const auto blob = std::span<const std::byte>(records,
                                                 chunk->recordBytes);
    const dxmt9::d3d9::V2ChunkEnvelope envelope{
        .version = chunk->version,
        .recordCount = chunk->recordCount,
        .handleCount = chunk->handleCount,
    };
    const bool phaseSplit = commitChunkPhaseSplitEnabled();
    if (phaseSplit) {
      dxmt9::perf::countCommitChunkPhaseCall();
    }
    dxmt9::d3d9::RawCommandChunk raw;
    CommitChunkPhaseTimer preparePhase(phaseSplit);
    const bool prepared = dxmt9::d3d9::prepareV2OffloadChunk(
        blob, envelope, d->wireObjects, retainV2Wrapper, raw);
    preparePhase.stop(dxmt9::perf::countCommitChunkPhasePrepareCpuTime);
    if (!prepared) {
      dxmt9::perf::countCommandChunkV2Reject();
      return commitChunkFail("v2-admission");
    }
    dxmt9::d3d9::ImportedChunkV2View imported;
    const auto ownedBytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(raw.recordBlob.data()),
        raw.recordBlob.size());
    CommitChunkPhaseTimer importPhase(phaseSplit);
    const bool importedOk =
        raw.preflightValidated &&
        dxmt9::d3d9::importPrevalidatedCommandChunkV2(
            ownedBytes, envelope, imported);
    importPhase.stop(dxmt9::perf::countCommitChunkPhaseImportCpuTime);
    if (!importedOk) {
      dxmt9::d3d9::releaseRetainedWrappers(raw);
      dxmt9::perf::countCommandChunkV2Reject();
      return commitChunkFail("v2-owned-preflight-view");
    }
    CommitChunkPhaseTimer markPhase(phaseSplit);
    markResolvedV2Resources(d, raw, imported);
    markPhase.stop(dxmt9::perf::countCommitChunkPhaseMarkCpuTime);
    dxmt9::perf::countCommandChunkWire(
        D9C_COMMAND_CHUNK_VERSION_V2, chunk->recordCount,
        chunk->recordBytes, chunk->handleCount);

    if (auto* q = findDirtyQueue(d)) {
      q->noteCommitChunkEntryForCompletionGap();
    }
    if (dxmt9::d3d9::offloadCommitReplayEnabled() &&
        !v2ChunkRequiresInlineReplay(imported)) {
      if (!d->replayOffload) {
        d->replayOffload =
            std::make_unique<dxmt9::d3d9::ReplayOffloadWorker>();
        d->replayOffload->start(d);
      }
      if (d->replayOffload->failed()) {
        dxmt9::d3d9::releaseRetainedWrappers(raw);
        dxmt9::perf::countCommandChunkV2Reject();
        return commitChunkFail("v2-offload-worker-failed");
      }
      const bool hasPresent = raw.hasPresent;
      CommitChunkPhaseTimer enqueuePhase(phaseSplit);
      dxmt9::perf::countOffloadReplayQueueDepth(
          static_cast<std::uint64_t>(d->replayOffload->queue().depth()));
      const bool pushed = d->replayOffload->queue().push(std::move(raw));
      enqueuePhase.stop(dxmt9::perf::countCommitChunkPhaseEnqueueCpuTime);
      if (!pushed) {
        dxmt9::d3d9::releaseRetainedWrappers(raw);
        dxmt9::perf::countCommandChunkV2Reject();
        return commitChunkFail("v2-offload-queue-stopped");
      }
      if (hasPresent) {
        ++d->presentOrdinal;
        if (auto upper = d->dev().upperDevice()) {
          const auto& presentParameters = d->dev().presentParameters();
          CommitChunkPhaseTimer presentWaitPhase(phaseSplit);
          upper->waitPresentOrdinalBoundary(
              d->presentOrdinal,
              presentParameters.backBufferCount,
              presentParameters.presentationInterval !=
                  dxmt9::core::PresentInterval::Immediate);
          presentWaitPhase.stop(
              dxmt9::perf::countCommitChunkPhasePresentWaitTime);
        }
      }
      countDurationSince(bridgeCommitStart,
                         dxmt9::perf::countOffloadCommitAppCpuTime);
      return dxmt9::core::D3D_OK;
    }

    const int32_t hr = replayResolvedV2Chunk(
        d, raw, /*pacedByPresentOrdinal=*/false);
    dxmt9::d3d9::releaseRetainedWrappers(raw);
    if (!failed(hr)) {
      const auto elapsed = std::chrono::steady_clock::now() - bridgeCommitStart;
      dxmt9::perf::countBridgeCommitLatencyNs(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
              .count()));
    }
    if (failed(hr)) {
      dxmt9::perf::countCommandChunkV2Reject();
    }
    return hr;
  }
}
