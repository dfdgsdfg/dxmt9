// Batched-chunk C ABI: dxmt9c_device_commit_chunk and the packet->state
// replay machinery it invokes. Per-call dxmt9c_* setters live in
// device_c_draw.cpp (interactive entry points) and device_c_state.cpp
// (state setters); both are referenced via extern "C" forward decls.

#include "device_c_provider.hpp"
#include "device_c_cpu_ready_plan.hpp"
#include "device_c_ordered_control.hpp"
#include "device_c_chunk_replay.hpp"
#include "device_c_presence_table.hpp"
#include "device_c_record_utils.hpp"
#include "device_c_replay_projection.hpp"
#include "device_c_replay_offload.hpp"
#include "device_c_cpu_ready_transfer.hpp"
#include "util/unixcall_marshal.hpp"

#include "../dxmt9/dxmt9_perf_counters.hpp"
#include "dxmt9/copy_materialization_ledger.hpp"
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
#include <limits>
#include <memory>
#include <new>
#include <optional>
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

// Native production-routing observer. It never replaces dispatch: tests may
// observe the exact ordered-control phases while replayResolvedChunk still
// executes the queue release API and the real record sink. Production leaves
// the callback null, so non-control records pay no cost.
using Dxmt9OrderedControlReplayObserver = void (*)(
    void* userdata, std::uint32_t phase, std::uint32_t recordIndex,
    std::uint32_t recordType, std::int32_t result);
namespace {
Dxmt9OrderedControlReplayObserver gOrderedControlReplayObserver = nullptr;
void* gOrderedControlReplayObserverUserdata = nullptr;

void observeOrderedControlReplay(std::uint32_t phase,
                                 std::size_t recordIndex,
                                 std::uint32_t recordType,
                                 std::int32_t result = 0) {
  if (gOrderedControlReplayObserver) {
    gOrderedControlReplayObserver(
        gOrderedControlReplayObserverUserdata, phase,
        static_cast<std::uint32_t>(recordIndex), recordType, result);
  }
}
}  // namespace

extern "C" void dxmt9_test_set_ordered_control_replay_observer(
    void* userdata, Dxmt9OrderedControlReplayObserver observer) {
  gOrderedControlReplayObserverUserdata = userdata;
  gOrderedControlReplayObserver = observer;
}

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

struct LedgerTargetHash {
  std::size_t operator()(
      dxmt9::d3d9::ReplayDrainTarget* value) const noexcept {
    return reinterpret_cast<std::uintptr_t>(value) >> 4u;
  }
};
using LedgerTargetPresence = dxmt9::d3d9::PresenceTable<
    dxmt9::d3d9::ReplayDrainTarget*, LedgerTargetHash>;

struct CoreEntryPresenceKey {
  dxmt9::core::ChunkHandleKind kind{};
  dxmt9::core::Handle handle{};
  friend bool operator==(const CoreEntryPresenceKey&,
                         const CoreEntryPresenceKey&) = default;
};
struct CoreEntryPresenceHash {
  std::size_t operator()(const CoreEntryPresenceKey& key) const noexcept {
    std::uint64_t hash = key.handle.value * 0x9E3779B97F4A7C15ull;
    hash ^= static_cast<std::uint64_t>(key.kind) + 0x517CC1B727220A95ull;
    hash ^= hash >> 33u;
    return static_cast<std::size_t>(hash);
  }
};
using CoreEntryPresence = dxmt9::d3d9::PresenceTable<
    CoreEntryPresenceKey, CoreEntryPresenceHash>;

struct ReplayScratchArena {
  std::vector<dxmt9::core::DrawBindingSnapshot> bindingSnapshots;
  std::vector<dxmt9::core::ChunkHandleEntry> coreEntries;
  LedgerTargetPresence ledgerTargetPresence;
  CoreEntryPresence coreEntryPresence;
  dxmt9::d3d9::ReplayTransaction transaction;
  bool inUse = false;

  void clear() noexcept {
    bindingSnapshots.clear();
    coreEntries.clear();
    // ledgerTargetPresence / coreEntryPresence are NOT cleared here: they are
    // resized-and-refilled explicitly by
    // persistResolvedResourcesAndCaptureBindings() via reset(capacityHint),
    // which needs the current call's handle count to size them. Leaving
    // stale slot contents around between calls is harmless because reset()
    // always overwrites every slot before first use.
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

const char* cpuReadyArenaFailureClassName(
    dxmt9::CommandQueue::CpuReadyArenaFailureClass failureClass) noexcept {
  using Failure = dxmt9::CommandQueue::CpuReadyArenaFailureClass;
  switch (failureClass) {
  case Failure::None: return "none";
  case Failure::Capacity: return "capacity";
  case Failure::Validation: return "validation";
  case Failure::ResourceRetain: return "resource_retain";
  case Failure::Publication: return "publication";
  case Failure::Completion: return "completion";
  case Failure::BuilderInitialization: return "builder_initialization";
  case Failure::ContextInvalid: return "context_invalid";
  case Failure::Append: return "append";
  case Failure::CaptureRanges: return "capture_ranges";
  case Failure::SegmentSelection: return "segment_selection";
  case Failure::ActiveArenaRejected: return "active_arena_rejected";
  case Failure::InjectedBuilder: return "injected_builder";
  case Failure::InjectedRollback: return "injected_rollback";
  case Failure::InjectedPostSemanticPublish:
    return "injected_post_semantic_publish";
  case Failure::PayloadSeal: return "payload_seal";
  case Failure::Planner: return "planner";
  case Failure::CaptureProjection: return "capture_projection";
  case Failure::SnapshotValidation: return "snapshot_validation";
  case Failure::Abort: return "abort";
  }
  return "unknown";
}

const char* cpuReadyArenaBeginStopReasonName(
    dxmt9::CommandQueue::CpuReadyArenaBeginStopReason reason) noexcept {
  using Reason = dxmt9::CommandQueue::CpuReadyArenaBeginStopReason;
  switch (reason) {
  case Reason::None: return "none";
  case Reason::QueueAlreadyStopped: return "queue_already_stopped";
  case Reason::CompatibilityFlushStopped:
    return "compatibility_flush_stopped";
  case Reason::CpuReadyTapeAlreadyStopped:
    return "cpu_ready_tape_already_stopped";
  }
  return "unknown";
}

// The fat-packet applier chain lived here: applyDrawPacketStateViaIface,
// applyDrawPacketStateDirect, applyDrawPacketState, timedApplyDrawPacketState and
// applyDrawPrimitivePacket, plus validateDrawPacketStateDelta and
// commitChunkDrawDeltaMask below them. All of it served
// dxmt9c_device_draw_primitive_packet / _chunk, the two direct fat-packet bridge
// ops, which had no PE-side caller; the chunk path has rejected any wire version
// other than canonical since long before this deletion. Task 10 stage D removed the
// whole component.

}  // namespace

// canonical admission retains every resolved wrapper before inline or offloaded
// replay. Both replay completion and queue teardown release the same list.
dxmt9::d3d9::RetainedWireHandleBatch::~RetainedWireHandleBatch() {
  reset();
}

dxmt9::d3d9::RetainedWireHandleBatch&
dxmt9::d3d9::RetainedWireHandleBatch::operator=(
    RetainedWireHandleBatch&& other) noexcept {
  if (this == &other) return *this;
  reset();
  entries_.swap(other.entries_);
  return *this;
}

void dxmt9::d3d9::RetainedWireHandleBatch::reset() noexcept {
  for (const auto& entry : entries_) {
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
  entries_.clear();
}

void dxmt9::d3d9::releaseRetainedWrappers(dxmt9::d3d9::RawCommandChunk& chunk) {
  chunk.bridgeRawLedgerCharge.reset();
  chunk.segmentedRegions.reset();
  chunk.retainedWrappers.reset();
}

namespace {

void retainWrapper(std::uint32_t kind, void* object) noexcept {
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

class DeviceReplaySink final : public dxmt9::d3d9::NonDrawReplaySink,
                                 public dxmt9::d3d9::SparseReplaySink {
public:
  DeviceReplaySink(
      D9CDevice* device, bool pacedByPresentOrdinal,
      std::vector<dxmt9::core::DrawBindingSnapshot>* bindingSnapshots,
      std::span<const dxmt9::core::ChunkBufferBindingSnapshot> capturedBuffers,
      bool capturedBuffersRequired, bool directFinalDraws,
      dxmt9::d3d9::ReplayTransaction& transaction,
      const dxmt9::core::DirectReplayDrawAppendCapability*
          directRangeAppender = nullptr)
      : device_(device),
        pacedByPresentOrdinal_(pacedByPresentOrdinal),
        bindingSnapshots_(bindingSnapshots),
        snapshotResolver_(capturedBuffers),
        capturedBuffersRequired_(capturedBuffersRequired &&
                                 snapshotResolver_.hasCapturedBackings()),
        directFinalDraws_(directFinalDraws), transaction_(&transaction),
        directRangeAppender_(directRangeAppender) {
    if (capturedBuffersRequired_) {
      const auto& state = device_->dev().state();
      for (dxmt9::core::u32 stream = 0;
           stream < dxmt9::core::kMaxStreams; ++stream) {
        const auto& buffer = state.streamBuffers[stream];
        classifyStreamBinding(
            stream, buffer ? buffer->handle() : dxmt9::core::Handle{});
      }
      classifyIndexBinding(
          state.indexBuffer ? state.indexBuffer->handle()
                            : dxmt9::core::Handle{});
    }
  }

  void setBatchCurrentDraw(bool value) noexcept {
    batchCurrentDraw_ = value;
  }

  bool startIrreversibleEffect() noexcept {
    return transaction_ && transaction_->startIrreversibleEffect();
  }

  std::int32_t setConstants(
      std::uint32_t type, const D9CCommandChunkWireSetConst& fixed,
      std::span<const std::byte> bytes) override {
    return setConstantBytes(type, fixed.startRegister, fixed.registerCount,
                            bytes);
  }

  std::int32_t clear(const D9CCommandChunkWireClear& fixed,
                     std::span<const D9CRect> rects) override {
    return dxmt9c_device_clear(device_, fixed.rectCount, rects.data(),
                               fixed.flags, fixed.colorARGB, fixed.z,
                               fixed.stencil);
  }

  std::int32_t present(
      const D9CCommandChunkWirePresent& fixed) override {
    device_->dev().setNextPresentPacedByOrdinal(pacedByPresentOrdinal_);
    return dxmt9c_device_present(device_, fixed.hasSrc ? &fixed.src : nullptr,
                                 fixed.hasDst ? &fixed.dst : nullptr,
                                 fixed.hwnd, nullptr, fixed.flags);
  }

  std::int32_t stretchRect(
      const D9CCommandChunkWireStretchRect& fixed, void* src,
      void* dst) override {
    return dxmt9c_device_stretch_rect(
        device_, static_cast<D9CSurface*>(src),
        fixed.hasSrcRect ? &fixed.srcRect : nullptr,
        static_cast<D9CSurface*>(dst),
        fixed.hasDstRect ? &fixed.dstRect : nullptr, fixed.filter);
  }

  std::int32_t colorFill(
      const D9CCommandChunkWireColorFill& fixed, void* surface) override {
    return dxmt9c_device_color_fill(
        device_, static_cast<D9CSurface*>(surface),
        fixed.hasRect ? &fixed.rect : nullptr, fixed.colorARGB);
  }

  std::int32_t updateTexture(
      const D9CCommandChunkWireUpdateTexture&, void* src,
      void* dst) override {
    return dxmt9c_device_update_texture(device_, static_cast<D9CTexture*>(src),
                                        static_cast<D9CTexture*>(dst));
  }

  std::int32_t updateSurface(
      const D9CCommandChunkWireUpdateSurface& fixed, void* src,
      void* dst) override {
    return dxmt9c_device_update_surface(
        device_, static_cast<D9CSurface*>(src),
        fixed.hasSrcRect ? &fixed.srcRect : nullptr,
        static_cast<D9CSurface*>(dst),
        fixed.hasDstPoint ? &fixed.dstPoint : nullptr);
  }

  std::int32_t queryIssue(
      const D9CCommandChunkWireQueryIssue& fixed, void* query) override {
    return dxmt9c_query_issue(static_cast<D9CQuery*>(query), fixed.flags);
  }

  std::int32_t readback(
      const D9CCommandChunkWireReadback&, void* src, void* dst) override {
    return dxmt9c_device_get_render_target_data(
        device_, static_cast<D9CSurface*>(src),
        static_cast<D9CSurface*>(dst));
  }

  std::int32_t reszDepthResolve(
      const D9CCommandChunkWireReszDepthResolve&, void* msaaDepth,
      void* intzDest) override {
    auto* surface = static_cast<D9CSurface*>(msaaDepth);
    auto* texture = static_cast<D9CTexture*>(intzDest);
    return surface && texture
               ? device_->dev().reszDepthResolve(surface->obj, texture->obj)
               : dxmt9::core::D3D_OK;
  }

  std::int32_t generateMipmaps(
      const D9CCommandChunkWireGenerateMipmaps&, void* texture) override {
    auto* resolved = static_cast<D9CTexture*>(texture);
    return resolved && resolved->obj
               ? resolved->obj->enqueueGenerateMipSubLevels()
               : dxmt9::core::D3DERR_INVALIDCALL;
  }

  std::int32_t applyState(
      const dxmt9::d3d9::ResolvedRecordView& record) override {
    return dxmt9::d3d9::replaySparseRecord(record, *this);
  }

  std::int32_t setRenderStates(
      std::span<const D9CCommandChunkWireRenderState> values) override {
    for (const auto& value : values) {
      const auto& state = device_->dev().state();
      if (!transaction_->journal().captureRenderState(state, value.state) ||
          (value.state == dxmt9::core::RS_SCISSOR_TEST_ENABLE &&
           !transaction_->journal().captureScissor(state))) {
        return dxmt9::core::D3DERR_INVALIDCALL;
      }
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
    const auto& state = device_->dev().state();
    if (!transaction_->journal().captureTexture(state, slot) ||
        (slot < dxmt9::core::kMaxTextureStages &&
         !transaction_->journal().captureTextureStageState(
             state, slot, dxmt9::core::TSS_TEXTURE_TYPE))) {
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    return dxmt9c_device_set_texture(
        device_, slot, static_cast<D9CTexture*>(texture));
  }

  std::int32_t setStream(
      const D9CCommandChunkWireStreamBinding& value,
      void* buffer) override {
    if (!transaction_->journal().captureStream(
            device_->dev().state(), value.slot)) {
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    const auto hr = dxmt9c_device_set_stream_source(
        device_, value.slot, static_cast<D9CBuffer*>(buffer), value.offset,
        value.stride);
    if (!failed(hr) && capturedBuffersRequired_ &&
        value.slot < dxmt9::core::kMaxStreams) {
      const auto* wire = static_cast<D9CBuffer*>(buffer);
      classifyStreamBinding(
          value.slot, wire && wire->obj ? wire->obj->handle()
                                       : dxmt9::core::Handle{});
    }
    return hr;
  }

  std::int32_t setShader(std::uint32_t stage, void* shader) override {
    const bool vertex =
        stage == D9C_COMMAND_CHUNK_SHADER_STAGE_VERTEX;
    if (!transaction_->journal().captureShader(device_->dev().state(),
                                               vertex)) {
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    return vertex
               ? dxmt9c_device_set_vertex_shader(
                     device_, static_cast<D9CShader*>(shader))
               : dxmt9c_device_set_pixel_shader(
                     device_, static_cast<D9CShader*>(shader));
  }

  std::int32_t setVertexInput(std::uint32_t kind, std::uint32_t value,
                              void* declaration) override {
    const auto& state = device_->dev().state();
    if (!transaction_->journal().captureFvf(state) ||
        !transaction_->journal().captureVertexDeclaration(state)) {
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    if (kind == D9C_COMMAND_CHUNK_VERTEX_INPUT_FVF) {
      return dxmt9c_device_set_fvf(device_, value);
    }
    const auto fvfHr = dxmt9c_device_set_fvf(device_, value);
    return failed(fvfHr)
               ? fvfHr
               : dxmt9c_device_set_vertex_declaration(
                     device_, static_cast<D9CVertexDecl*>(declaration));
  }

  std::int32_t setIndexBuffer(void* buffer) override {
    if (!transaction_->journal().captureIndex(device_->dev().state())) {
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    const auto hr =
        dxmt9c_device_set_indices(device_, static_cast<D9CBuffer*>(buffer));
    if (!failed(hr) && capturedBuffersRequired_) {
      const auto* wire = static_cast<D9CBuffer*>(buffer);
      classifyIndexBinding(
          wire && wire->obj ? wire->obj->handle()
                            : dxmt9::core::Handle{});
    }
    return hr;
  }

  std::int32_t setRenderTarget(std::uint32_t slot, void* surface) override {
    const auto& state = device_->dev().state();
    if (!transaction_->journal().captureRenderTarget(state, slot) ||
        (slot == 0u &&
         (!transaction_->journal().captureViewport(state) ||
          !transaction_->journal().captureScissor(state)))) {
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    return dxmt9c_device_set_render_target(
        device_, slot, static_cast<D9CSurface*>(surface));
  }

  std::int32_t setDepthStencil(void* surface) override {
    if (!transaction_->journal().captureDepthStencil(
            device_->dev().state())) {
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    return dxmt9c_device_set_depth_stencil(
        device_, static_cast<D9CSurface*>(surface));
  }

  std::int32_t setViewport(const D9CViewport& value) override {
    if (!transaction_->journal().captureViewport(device_->dev().state())) {
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    if (auto* queue = findDirtyQueue(device_)) {
      queue->applyDirtyViewportChange();
    }
    return dxmt9c_device_set_viewport(device_, &value);
  }

  std::int32_t setScissor(const D9CRect& value) override {
    if (!transaction_->journal().captureScissor(device_->dev().state())) {
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    return dxmt9c_device_set_scissor_rect(device_, &value);
  }

  std::int32_t setMaterial(const D9CMaterial& value) override {
    if (!transaction_->journal().captureMaterial(device_->dev().state())) {
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    if (auto* queue = findDirtyQueue(device_)) {
      queue->applyDirtyTransformChange();
    }
    return dxmt9c_device_set_material(device_, &value);
  }

  std::int32_t setClipPlane(
      const D9CCommandChunkWireClipPlane& value) override {
    if (!transaction_->journal().captureClipPlane(
            device_->dev().state(), value.slot)) {
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    if (auto* queue = findDirtyQueue(device_)) {
      queue->applyDirtyClipPlaneChange();
    }
    return dxmt9c_device_set_clip_plane(device_, value.slot, value.values);
  }

  std::int32_t setTextureStageStates(
      std::span<const D9CDrawPacketTextureStageState> values) override {
    for (const auto& value : values) {
      if (!transaction_->journal().captureTextureStageState(
              device_->dev().state(), value.stage, value.type)) {
        return dxmt9::core::D3DERR_INVALIDCALL;
      }
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
      if (!transaction_->journal().captureSamplerState(
              device_->dev().state(), value.sampler, value.type)) {
        return dxmt9::core::D3DERR_INVALIDCALL;
      }
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
      if (!transaction_->journal().captureTransform(
              device_->dev().state(), value.state)) {
        return dxmt9::core::D3DERR_INVALIDCALL;
      }
      const auto hr = dxmt9c_device_set_transform(
          device_, value.state, &value.matrix);
      if (failed(hr)) return hr;
    }
    return dxmt9::core::D3D_OK;
  }

  std::int32_t setLights(
      std::span<const D9CCommandChunkWireLight> values) override {
    if (!values.empty()) {
      if (auto* queue = findDirtyQueue(device_)) {
        queue->applyDirtyTransformChange();
      }
    }
    for (const auto& value : values) {
      if (!transaction_->journal().captureLight(
              device_->dev().state(), value.slot)) {
        return dxmt9::core::D3DERR_INVALIDCALL;
      }
      const auto hr = dxmt9c_device_set_light(
          device_, value.slot, &value.light);
      if (failed(hr)) return hr;
    }
    return dxmt9::core::D3D_OK;
  }

  std::int32_t setLightEnables(
      std::span<const D9CCommandChunkWireLightEnable> values) override {
    if (!values.empty()) {
      if (auto* queue = findDirtyQueue(device_)) {
        queue->applyDirtyTransformChange();
      }
    }
    for (const auto& value : values) {
      const auto& state = device_->dev().state();
      if (!transaction_->journal().captureLight(state, value.slot) ||
          !transaction_->journal().captureLightEnabled(state, value.slot)) {
        return dxmt9::core::D3DERR_INVALIDCALL;
      }
      const auto hr = dxmt9c_device_light_enable(
          device_, value.slot, value.enabled);
      if (failed(hr)) return hr;
    }
    return dxmt9::core::D3D_OK;
  }

  std::int32_t setConstants(
      std::uint16_t sectionKind,
      const D9CCommandChunkWireConstantRange& range,
      std::span<const std::byte> bytes) override {
    std::uint32_t type = 0u;
    switch (sectionKind) {
    case D9C_COMMAND_CHUNK_SECTION_VS_CONST_F:
      type = D9C_COMMAND_RECORD_SET_VS_CONST_F;
      break;
    case D9C_COMMAND_CHUNK_SECTION_VS_CONST_I:
      type = D9C_COMMAND_RECORD_SET_VS_CONST_I;
      break;
    case D9C_COMMAND_CHUNK_SECTION_VS_CONST_B:
      type = D9C_COMMAND_RECORD_SET_VS_CONST_B;
      break;
    case D9C_COMMAND_CHUNK_SECTION_PS_CONST_F:
      type = D9C_COMMAND_RECORD_SET_PS_CONST_F;
      break;
    case D9C_COMMAND_CHUNK_SECTION_PS_CONST_I:
      type = D9C_COMMAND_RECORD_SET_PS_CONST_I;
      break;
    case D9C_COMMAND_CHUNK_SECTION_PS_CONST_B:
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

  std::int32_t draw(const dxmt9::d3d9::SparseDrawCall& call) override {
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
    if (batchCurrentDraw_) {
      auto payload = call.payload;
      if (!attachCapturedBindingSnapshot(draw, payload)) {
        return dxmt9::core::D3DERR_INVALIDCALL;
      }
      // A private Direct/Arena destination remains rollbackable until commit.
      // The ordinary serial path publishes directly into final queue storage,
      // so its effect cut must precede the synchronous borrowed-state call.
      if (!directFinalDraws_ && !startIrreversibleEffect()) {
        return dxmt9::core::D3DERR_DEVICELOST;
      }
      const auto result =
          device_->dev().submitDirectReplayDrawFromCurrentState(
              draw, payload, directRangeAppender_);
      if (directFinalDraws_ &&
          result.disposition !=
              dxmt9::core::DirectReplayDrawDisposition::Appended &&
          !startIrreversibleEffect()) {
        return dxmt9::core::D3DERR_DEVICELOST;
      }
      return result.result;
    }
    auto payload = call.payload;
    if (!attachCapturedBindingSnapshot(draw, payload)) {
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    return device_->dev().drawPrimitiveRun(
        std::span<const dxmt9::core::DrawParam>(&draw, 1u),
        std::span<const dxmt9::core::DrawParamPayloadView>(&payload, 1u));
  }

private:
  bool attachCapturedBindingSnapshot(
      const dxmt9::core::DrawParam& draw,
      dxmt9::core::DrawParamPayloadView& payload) {
    if (!capturedBuffersRequired_) {
      return true;
    }
    if (unresolvedStreamMask_ != 0u ||
        (draw.indexed && unresolvedIndexBinding_)) {
      return false;
    }
    if (capturedStreamMask_ == 0u &&
        (!draw.indexed || !capturedIndexBinding_)) {
      // The chunk has captured backings, but this draw uses none of them. Keep
      // the hot live-only path free of both the 16-stream scan and the 832-byte
      // replay payload.
      return true;
    }
    dxmt9::core::DrawBindingSnapshot binding;
    const auto& state = device_->dev().state();
    std::array<dxmt9::core::Handle, dxmt9::core::kMaxStreams> streams{};
    for (dxmt9::core::u32 stream = 0;
         stream < dxmt9::core::kMaxStreams; ++stream) {
      const auto& buffer = state.streamBuffers[stream];
      streams[stream] = buffer ? buffer->handle() : dxmt9::core::Handle{};
    }
    const auto indexBuffer = state.indexBuffer
        ? state.indexBuffer->handle()
        : dxmt9::core::Handle{};
    bool usedCapturedBacking = false;
    if (!snapshotResolver_.resolve(
            streams, state.streamOffsets, state.streamStrides,
            indexBuffer, draw.indexType, draw.indexed, binding,
            &usedCapturedBacking)) {
      return false;
    }
    if (!usedCapturedBacking) {
      return true;
    }
    bindingSnapshots_->push_back(binding);
    payload.bindingSnapshotData = dxmt9::core::drawBindingSnapshotBytes(
        bindingSnapshots_->back());
    return true;
  }

  void classifyStreamBinding(dxmt9::core::u32 stream,
                             dxmt9::core::Handle handle) noexcept {
    const auto bit = 1u << stream;
    capturedStreamMask_ &= ~bit;
    unresolvedStreamMask_ &= ~bit;
    switch (snapshotResolver_.classify(handle)) {
    case dxmt9::d3d9::ReplayBufferSnapshotResolver::BindingClass::Missing:
      unresolvedStreamMask_ |= bit;
      break;
    case dxmt9::d3d9::ReplayBufferSnapshotResolver::BindingClass::Captured:
      capturedStreamMask_ |= bit;
      break;
    case dxmt9::d3d9::ReplayBufferSnapshotResolver::BindingClass::Live:
      break;
    }
  }

  void classifyIndexBinding(dxmt9::core::Handle handle) noexcept {
    const auto classification = snapshotResolver_.classify(handle);
    capturedIndexBinding_ =
        classification ==
        dxmt9::d3d9::ReplayBufferSnapshotResolver::BindingClass::Captured;
    unresolvedIndexBinding_ =
        classification ==
        dxmt9::d3d9::ReplayBufferSnapshotResolver::BindingClass::Missing;
  }

  std::int32_t setConstantBytes(std::uint32_t type, std::uint32_t start,
                                std::uint32_t count,
                                std::span<const std::byte> bytes) {
    const void* data = bytes.data();
    using ConstantKind =
        dxmt9::d3d9::DeviceStateUndoJournal::ConstantKind;
    std::optional<ConstantKind> kind;
    switch (type) {
    case D9C_COMMAND_RECORD_SET_VS_CONST_F:
      kind = ConstantKind::VertexFloat;
      break;
    case D9C_COMMAND_RECORD_SET_VS_CONST_I:
      kind = ConstantKind::VertexInt;
      break;
    case D9C_COMMAND_RECORD_SET_VS_CONST_B:
      kind = ConstantKind::VertexBool;
      break;
    case D9C_COMMAND_RECORD_SET_PS_CONST_F:
      kind = ConstantKind::PixelFloat;
      break;
    case D9C_COMMAND_RECORD_SET_PS_CONST_I:
      kind = ConstantKind::PixelInt;
      break;
    case D9C_COMMAND_RECORD_SET_PS_CONST_B:
      kind = ConstantKind::PixelBool;
      break;
    default:
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    if (!transaction_->journal().captureConstantRange(
            device_->dev().state(), *kind, start, count)) {
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
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
  std::vector<dxmt9::core::DrawBindingSnapshot>* bindingSnapshots_ = nullptr;
  dxmt9::d3d9::ReplayBufferSnapshotResolver snapshotResolver_;
  bool capturedBuffersRequired_ = false;
  dxmt9::core::u32 capturedStreamMask_ = 0u;
  dxmt9::core::u32 unresolvedStreamMask_ = 0u;
  bool capturedIndexBinding_ = false;
  bool unresolvedIndexBinding_ = false;
  bool batchCurrentDraw_ = false;
  bool directFinalDraws_ = false;
  dxmt9::d3d9::ReplayTransaction* transaction_ = nullptr;
  const dxmt9::core::DirectReplayDrawAppendCapability*
      directRangeAppender_ = nullptr;
};

bool recordCanBatchDraw(
    D9CDevice* device,
    const dxmt9::d3d9::ImportedRecordView& record) noexcept {
  if (!device || device->stateBlockRecording ||
      (record.header.type != D9C_COMMAND_RECORD_DRAW_PRIMITIVE &&
       record.header.type != D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE)) {
    return false;
  }
  return static_cast<dxmt9::core::PrimitiveType>(
             record.drawHeader.primitiveType - 1u) !=
         dxmt9::core::PrimitiveType::TriangleFan;
}

bool recordProducesArenaCommand(std::uint32_t type) noexcept {
  switch (type) {
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP:
  case D9C_COMMAND_RECORD_CLEAR:
  case D9C_COMMAND_RECORD_UPDATE_SURFACE:
  case D9C_COMMAND_RECORD_STRETCH_RECT:
  case D9C_COMMAND_RECORD_COLOR_FILL:
  case D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE:
  case D9C_COMMAND_RECORD_GENERATE_MIPMAPS:
  case D9C_COMMAND_RECORD_PRESENT:
    return true;
  default:
    return false;
  }
}

bool importRawChunk(const dxmt9::d3d9::RawCommandChunk& raw,
                   dxmt9::d3d9::ImportedChunkView& imported) noexcept {
  const dxmt9::d3d9::CommandChunkEnvelope envelope{
      .version = raw.wireVersion,
      .recordCount = raw.recordCount,
      .handleCount = raw.handleCount,
  };
  if (!raw.preflightValidated) return false;
  if (raw.segmentedTransport) {
    const auto records = raw.segmentedRegions.recordsBytes();
    const auto handles = raw.segmentedRegions.handlesBytes();
    const auto payload = raw.segmentedRegions.payloadBytes();
    return dxmt9::d3d9::importPrevalidatedSegmentedCommandChunk(
        raw.wireHeader, records, handles, payload, envelope, imported);
  }
  const auto bytes = std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(raw.recordBlob.data()),
      raw.recordBlob.size());
  return dxmt9::d3d9::importPrevalidatedCommandChunk(bytes, envelope, imported);
}

// One executable slice of the raw. The default value is the whole raw, which
// is what every pre-span caller passes. A lease-span driver replays the raw as
// several consecutive ranges; record ordinals stay raw-absolute so the
// transaction projection and every receipt remain source-relative.
struct ReplayRecordRange {
  std::size_t firstRecordIndex = 0;
  std::size_t recordCount = std::numeric_limits<std::size_t>::max();
  // The driver performs the whole-raw ordered-control preflight once, before
  // any span executes, so no later malformed control can fail after an
  // earlier span has already applied effects. A per-span call must not repeat
  // it: the span may legitimately contain no control at all.
  bool orderedControlPreflightDone = false;
  // `chunk_admit` counts admitted raws, not executed ranges. A span driver
  // suppresses it per span and counts the raw once.
  bool countAdmit = true;

  bool wholeRaw() const noexcept {
    return firstRecordIndex == 0 &&
           recordCount == std::numeric_limits<std::size_t>::max();
  }
};

int32_t replayResolvedChunk(
    D9CDevice* device, dxmt9::d3d9::RawCommandChunk& raw,
    bool pacedByPresentOrdinal,
    dxmt9::d3d9::ReplayTransaction& transaction,
    dxmt9::CommandQueue::CpuReadyArenaBuildLease* arenaLease = nullptr,
    std::span<const dxmt9::d3d9::CpuReadySegmentPlan> arenaSegments = {},
    bool containsOrderedControls = false,
    std::span<const dxmt9::d3d9::CpuReadySourcePlan> arenaSources = {},
    bool activeDirectChunkSlotPath = false,
    const dxmt9::core::DirectReplayDrawAppendCapability*
        directRangeAppender = nullptr,
    ReplayRecordRange range = {}) {
  dxmt9::d3d9::ImportedChunkView imported;
  if (!raw.preflightValidated ||
      !importRawChunk(raw, imported) ||
      raw.resolvedObjects.size() != imported.handles.size()) {
    return commitChunkFail("chunk-replay-preflight-view");
  }
  const std::size_t rangeFirst = range.firstRecordIndex;
  const std::size_t rangeEnd =
      range.recordCount == std::numeric_limits<std::size_t>::max()
          ? imported.records.size()
          : rangeFirst + range.recordCount;
  if (rangeFirst > imported.records.size() ||
      rangeEnd > imported.records.size() || rangeEnd < rangeFirst ||
      // Arena segment/source selection describes the complete raw and has no
      // meaning over a sub-range.
      (arenaLease != nullptr && !range.wholeRaw())) {
    return commitChunkFail("chunk-replay-record-range");
  }
  if (arenaLease) {
    std::size_t coveredRecords = 0;
    for (const auto& segment : arenaSegments) {
      if (segment.recordCount == 0 ||
          segment.firstRecordIndex != coveredRecords ||
          coveredRecords > imported.records.size() ||
          segment.recordCount > imported.records.size() - coveredRecords) {
        return commitChunkFail("chunk-replay-segment-range");
      }
      coveredRecords += segment.recordCount;
    }
    if (coveredRecords != imported.records.size()) {
      return commitChunkFail("chunk-replay-segment-cover");
    }
  }
  if (containsOrderedControls && !range.orderedControlPreflightDone) {
    // The planner already performed this complete scan. Repeat it against the
    // exact immutable replay view before constructing the sink or applying any
    // semantic effect, so no later malformed control can fail after an older
    // record has executed.
    bool foundOrderedControl = false;
    for (std::size_t index = 0; index < imported.records.size(); ++index) {
      const auto record = imported.record(index);
      switch (record.header.type) {
      case D9C_COMMAND_RECORD_QUERY_ISSUE:
      case D9C_COMMAND_RECORD_READBACK:
      case D9C_COMMAND_RECORD_UPDATE_TEXTURE:
        foundOrderedControl = true;
        if (!dxmt9::d3d9::makeOrderedControlDisposition(
                record, raw.replaySeq, index)) {
          return commitChunkFail(
              "chunk-ordered-control-preflight",
              static_cast<std::uint32_t>(index), record.header.type);
        }
        break;
      default:
        break;
      }
    }
    if (!foundOrderedControl) {
      return commitChunkFail("chunk-ordered-control-preflight-empty");
    }
  }

  dxmt9::d3d9::ResolvedChunkView resolved{
      .wire = imported,
      .objects = raw.resolvedObjects,
  };
  auto& replayScratch = replayScratchArena();
  ScopedReplayScratchUse replayScratchUse(replayScratch);
  replayScratch.bindingSnapshots.reserve(raw.recordCount);
  bool captureIdentity = arenaLease &&
      raw.renderTapeCaptureToken != 0u;
  if (captureIdentity &&
      !arenaLease->beginCaptureIdentity(raw.recordCount)) {
    return commitChunkFail("chunk-capture-identity-begin");
  }
  // A private destination keeps replay rollbackable. The ordinary serial
  // path uses the same borrowed-state ingress but publishes directly into
  // final queue storage instead of materializing an intermediate carrier.
  const bool directFinalDraws = arenaLease != nullptr ||
      activeDirectChunkSlotPath;
  DeviceReplaySink sink(
      device, pacedByPresentOrdinal, &replayScratch.bindingSnapshots,
      raw.bufferSnapshots,
      raw.bufferSnapshotsCaptured, directFinalDraws, transaction,
      directRangeAppender);
  std::size_t activeSegment = 0;
  std::size_t activeSource = 0;
  std::size_t activeSourceSegment = 0;
  if ((!arenaLease && !arenaSegments.empty()) ||
      (arenaLease && arenaSegments.empty()) ||
      (arenaLease && arenaSources.empty() && !arenaLease->selectSegment(0)) ||
      (arenaLease && !arenaSources.empty() &&
       !arenaLease->selectSourceSegment(0, 0))) {
    return commitChunkFail("chunk-replay-segment-initial");
  }
  for (std::size_t index = rangeFirst; index < rangeEnd; ++index) {
    if (arenaLease && !arenaSources.empty()) {
      if (activeSource >= arenaSources.size()) {
        return commitChunkFail("chunk-replay-source-range");
      }
      const auto* source = &arenaSources[activeSource];
      if (activeSource + 1u < arenaSources.size()) {
        const std::size_t nextSourceFirst =
            arenaSources[activeSource + 1u].firstRecordIndex;
        if (index > nextSourceFirst) {
          return commitChunkFail("chunk-replay-source-edge",
                                 static_cast<std::uint32_t>(index));
        }
        if (index == nextSourceFirst) {
          ++activeSource;
          activeSourceSegment = 0;
          if (!arenaLease->selectSourceSegment(activeSource, 0)) {
            return commitChunkFail("chunk-replay-source-select",
                                   static_cast<std::uint32_t>(index));
          }
          source = &arenaSources[activeSource];
        }
      }
      if (activeSourceSegment + 1u < source->segmentCount) {
        const auto nextFirst = arenaSegments[source->firstSegmentIndex +
                                              activeSourceSegment + 1u]
                                   .firstRecordIndex;
        if (index > nextFirst) {
          return commitChunkFail("chunk-replay-segment-edge",
                                 static_cast<std::uint32_t>(index));
        }
        if (index == nextFirst) {
          ++activeSourceSegment;
          if (!arenaLease->selectSourceSegment(activeSource,
                                               activeSourceSegment)) {
            return commitChunkFail("chunk-replay-segment-select",
                                   static_cast<std::uint32_t>(index));
          }
        }
      }
    } else if (arenaLease && activeSegment + 1u < arenaSegments.size()) {
      const std::size_t nextFirstRecord =
          arenaSegments[activeSegment + 1u].firstRecordIndex;
      if (index > nextFirstRecord) {
        return commitChunkFail("chunk-replay-segment-edge",
                               static_cast<std::uint32_t>(index));
      }
      if (index == nextFirstRecord) {
        ++activeSegment;
        if (!arenaLease->selectSegment(activeSegment)) {
          return commitChunkFail("chunk-replay-segment-select",
                                 static_cast<std::uint32_t>(index));
        }
      }
    }
    const auto record = resolved.record(index);
    const auto projectedSource = !arenaSources.empty()
        ? transaction.state().identity.source + activeSource
        : transaction.state().identity.source;
    if (!transaction.project({
            .stateGeneration = raw.replaySeq != 0u ? raw.replaySeq : 1u,
            .source = projectedSource,
            .recordOrdinal = static_cast<std::uint32_t>(index),
        })) {
      return commitChunkFail("chunk-replay-transaction-project",
                             static_cast<std::uint32_t>(index),
                             record.wire.header.type);
    }
    const bool batchableDraw = recordCanBatchDraw(device, record.wire);
    sink.setBatchCurrentDraw(batchableDraw);
    if (captureIdentity && batchableDraw) {
      if (arenaLease) {
        const std::array recordIndex{static_cast<std::uint32_t>(index)};
        if (!arenaLease->captureNextDrawRecords(recordIndex)) {
          captureIdentity = false;
        }
      }
    }
    std::optional<dxmt9::d3d9::OrderedControlDisposition> orderedControl;
    if (containsOrderedControls) {
      switch (record.wire.header.type) {
      case D9C_COMMAND_RECORD_QUERY_ISSUE:
      case D9C_COMMAND_RECORD_READBACK:
      case D9C_COMMAND_RECORD_UPDATE_TEXTURE:
        orderedControl = dxmt9::d3d9::makeOrderedControlDisposition(
            record.wire, raw.replaySeq, index);
        if (!orderedControl) {
          return commitChunkFail("chunk-ordered-control-rebuild",
                                 static_cast<std::uint32_t>(index),
                                 record.wire.header.type);
        }
        break;
      default:
        break;
      }
    }
    if (orderedControl) {
      if (!sink.startIrreversibleEffect()) {
        return commitChunkFail("chunk-ordered-control-effect-cut",
                               static_cast<std::uint32_t>(index),
                               record.wire.header.type);
      }
      observeOrderedControlReplay(/*BeforeRelease=*/1u, index,
                                  record.wire.header.type);
      auto upper = device->dev().upperDevice();
      auto* queue = upper ? &upper->queue() : nullptr;
      const auto reason = orderedControl->kind ==
                                  dxmt9::d3d9::OrderedControlKind::UpdateTexture
                              ? dxmt9::core::metalqueue::
                                    SessionReleaseReason::IndependentSubmission
                              : dxmt9::core::metalqueue::
                                    SessionReleaseReason::DirectObservation;
      if (!orderedControl->valid() || !queue ||
          !queue->releaseCpuReadySessionBeforeOrderedControl(
              reason, orderedControl->requiredReleaseAction,
              orderedControl->rawOrdinal)) {
        return commitChunkFail("chunk-ordered-control-release",
                               static_cast<std::uint32_t>(index),
                               record.wire.header.type);
      }
      observeOrderedControlReplay(/*AfterRelease=*/2u, index,
                                  record.wire.header.type);
    }
    const bool dispatchingOrderedControl = orderedControl.has_value();
    if (dispatchingOrderedControl) {
      observeOrderedControlReplay(/*BeforeDispatch=*/3u, index,
                                  record.wire.header.type);
    }
    if (captureIdentity && !batchableDraw &&
        recordProducesArenaCommand(record.wire.header.type) &&
        !arenaLease->captureNextCommandRecord(
            static_cast<std::uint32_t>(index))) {
      captureIdentity = false;
    }
    const auto replayInfo = replayInfoForCommandRecordType(
        record.wire.header.type);
    const bool stateOnly =
        replayInfo.category == ImportedRecordReplayCategory::ConstantUpload ||
        replayInfo.category == ImportedRecordReplayCategory::StateApply;
    if (!stateOnly && !batchableDraw && !dispatchingOrderedControl &&
        !sink.startIrreversibleEffect()) {
      return commitChunkFail("chunk-replay-effect-cut",
                             static_cast<std::uint32_t>(index),
                             record.wire.header.type);
    }
    const auto hr = dxmt9::d3d9::isSparseRecord(record.wire.header.type)
                        ? dxmt9::d3d9::replaySparseRecord(record, sink)
                        : dxmt9::d3d9::replayNonDrawRecord(record, sink);
    if (dispatchingOrderedControl) {
      observeOrderedControlReplay(/*AfterDispatch=*/4u, index,
                                  record.wire.header.type, hr);
    }
    if (failed(hr)) {
      // A later malformed record must not turn an otherwise rollbackable
      // private prefix into an effect merely to preserve the old batching
      // side effect. The transaction owner decides rollback versus fail-stop.
      return commitChunkFail("chunk-replay", static_cast<std::uint32_t>(index),
                             record.wire.header.type, hr);
    }
    if (recordProducesArenaCommand(record.wire.header.type) &&
        !transaction.stage({
            .commandCount = 1u,
            .byteCount = record.wire.header.payloadSize,
        })) {
      return commitChunkFail("chunk-replay-transaction-stage",
                             static_cast<std::uint32_t>(index),
                             record.wire.header.type);
    }
  }
  if (arenaLease && arenaSources.empty() &&
      activeSegment + 1u != arenaSegments.size()) {
    return commitChunkFail("chunk-replay-segment-incomplete");
  }
  if (arenaLease && !arenaSources.empty() &&
      (activeSource + 1u != arenaSources.size() ||
       activeSourceSegment + 1u != arenaSources.back().segmentCount)) {
    return commitChunkFail("chunk-replay-source-incomplete");
  }
  if (range.countAdmit) {
    dxmt9::perf::countChunkAdmit();
  }
  return dxmt9::core::D3D_OK;
}

// Body of commit_chunk's mark phase (commit_chunk_phase_mark_cpu_ms), which
// GT2 measures at 0.888 ms/present, 62% of the sync half, 56.5us/call. Split
// into its three owners so a subsequent optimization knows which one to
// target: the PE-side resolved-handle dedup loop (originally two O(n^2)
// scans — a std::find over raw.ledgerTargets per buffer handle, a
// std::any_of over scratch.coreEntries per handle; both are now O(1)
// amortized via LedgerTargetPresence/CoreEntryPresence, falling back to the
// original linear scan only past a measured capacity —
// state-churn-encode-append-decomposition.{26,28}, R-BACK-43.7), the single
// call into the core upperDevice (which, on the default non-CpuReadyTape
// lane, subsumes commit_chunk_phase_mark_lock_cpu_ms's queue-mutex acquire
// wait), and the buffer-snapshot sort. All gated on the same
// DXMT9_PERF_COMMIT_CHUNK_PHASE_SPLIT env as the parent phase split.
bool persistResolvedResourcesAndCaptureBindings(
    D9CDevice* device, dxmt9::d3d9::RawCommandChunk& raw,
    const dxmt9::d3d9::ImportedChunkView& imported) {
  const bool phaseSplit = commitChunkPhaseSplitEnabled();
  CommitChunkPhaseTimer dedupPhase(phaseSplit);
  auto& scratch = replayScratchArena();
  ScopedReplayScratchUse scratchUse(scratch);
  // Size both presence accelerators off this call's handle count before the
  // loop below; reset() only reallocates when growing past a previous call's
  // capacity, so steady-state chunks pay a fill, not an allocation.
  scratch.ledgerTargetPresence.reset(imported.handles.size());
  scratch.coreEntryPresence.reset(imported.handles.size());
  std::uint64_t bufferHandleCount = 0u;
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
      if (value && value->obj) {
        handle = value->obj->handle();
        ++bufferHandleCount;
        auto* target = value->replayDrainTarget.get();
        bool targetDuplicate;
        if (!scratch.ledgerTargetPresence.overflowed()) {
          targetDuplicate = scratch.ledgerTargetPresence.contains(target);
        } else {
          // Overflow fallback: raw.ledgerTargets is always kept complete, so
          // the linear scan is still correct, just the pre-overflow O(n).
          targetDuplicate = std::find(
              raw.ledgerTargets.begin(), raw.ledgerTargets.end(),
              target) != raw.ledgerTargets.end();
        }
        if (!targetDuplicate) {
          raw.ledgerTargets.push_back(target);
        if (!scratch.ledgerTargetPresence.overflowed()) {
            scratch.ledgerTargetPresence.insert(target);
          }
        }
      }
      break;
    }
    default:
      break;
    }
    if (handle.value == 0u) continue;
    const auto kind = static_cast<dxmt9::core::ChunkHandleKind>(
        imported.handles[i].kind);
    bool duplicate;
    if (!scratch.coreEntryPresence.overflowed()) {
      duplicate = scratch.coreEntryPresence.contains({kind, handle});
    } else {
      // Overflow fallback: scratch.coreEntries is always kept complete, so
      // the linear scan is still correct, just the pre-overflow O(n).
      duplicate = std::any_of(
          scratch.coreEntries.begin(), scratch.coreEntries.end(),
          [&](const dxmt9::core::ChunkHandleEntry& existing) {
            return existing.kind == kind && existing.handle == handle;
          });
    }
    if (!duplicate) {
      scratch.coreEntries.push_back({.kind = kind, .handle = handle});
      if (!scratch.coreEntryPresence.overflowed()) {
        scratch.coreEntryPresence.insert({kind, handle});
      }
    }
  }
  raw.resourceEntries = scratch.coreEntries;
  if (phaseSplit) {
    dxmt9::perf::countCommitChunkPhaseMarkHandles(imported.handles.size());
    dxmt9::perf::countCommitChunkPhaseMarkBuffers(bufferHandleCount);
  }
  dedupPhase.stop(dxmt9::perf::countCommitChunkPhaseMarkDedupCpuTime);
  if (auto upper = device->dev().upperDevice()) {
    raw.cpuReadyTapePlanningEnabled =
        upper->supportsCpuReadyArenaReplay();
    CommitChunkPhaseTimer corePhase(phaseSplit);
    const auto captureResult = raw.cpuReadyTapePlanningEnabled
        ? upper->captureChunkBufferBindings(raw.resourceEntries,
                                            raw.bufferSnapshots)
        : upper->markChunkResourcesAndCaptureBufferBindings(
              raw.resourceEntries, raw.bufferSnapshots);
    corePhase.stop(dxmt9::perf::countCommitChunkPhaseMarkCoreCpuTime);
    raw.resourcesMarkedBeforeReplay =
        !raw.cpuReadyTapePlanningEnabled;
    raw.bufferSnapshotsCaptured =
        captureResult ==
        dxmt9::core::ChunkBufferBindingCaptureResult::Complete;
    CommitChunkPhaseTimer sortPhase(phaseSplit);
    std::sort(raw.bufferSnapshots.begin(), raw.bufferSnapshots.end(),
              [](const auto& left, const auto& right) {
                return left.buffer.value < right.buffer.value;
              });
    sortPhase.stop(dxmt9::perf::countCommitChunkPhaseMarkSortCpuTime);
    return captureResult !=
           dxmt9::core::ChunkBufferBindingCaptureResult::MissingRequired;
  }
  return true;
}

bool chunkRequiresInlineReplay(
    const dxmt9::d3d9::ImportedChunkView& imported) noexcept {
  return std::any_of(
      imported.records.begin(), imported.records.end(),
      [](const D9CCommandChunkWireRecordHeader& record) {
        return replayInfoForCommandRecordType(record.type)
            .synchronousReadBoundary;
      });
}

bool importOwnedChunk(dxmt9::d3d9::RawCommandChunk& raw,
                        dxmt9::d3d9::ImportedChunkView& imported) noexcept {
  return raw.preflightValidated && raw.replaySeq != 0 &&
         importRawChunk(raw, imported) &&
         raw.resolvedObjects.size() == imported.handles.size();
}

void markLegacyResources(D9CDevice* device,
                           dxmt9::d3d9::RawCommandChunk& raw) {
  if (raw.resourcesMarkedBeforeReplay) {
    return;
  }
  if (auto upper = device->dev().upperDevice()) {
    upper->markChunkResources(raw.resourceEntries);
    raw.resourcesMarkedBeforeReplay = true;
  }
}

class ScopedCpuReadySupplyReplayEntry {
public:
  ScopedCpuReadySupplyReplayEntry(
      dxmt9::CommandQueue& queue,
      dxmt9::core::CpuReadyTape::PayloadKind sourceClass) noexcept
      : queue_(&queue), sourceClass_(sourceClass) {
    attemptToken_ = queue_->noteCpuReadySupplyReplayEntry(sourceClass_);
  }

  ~ScopedCpuReadySupplyReplayEntry() {
    cancel();
  }

  ScopedCpuReadySupplyReplayEntry(const ScopedCpuReadySupplyReplayEntry&) =
      delete;
  ScopedCpuReadySupplyReplayEntry& operator=(
      const ScopedCpuReadySupplyReplayEntry&) = delete;

  void cancel() noexcept {
    if (!queue_) {
      return;
    }
    queue_->cancelCpuReadySupplyReplayEntry(sourceClass_, attemptToken_);
    queue_ = nullptr;
  }

  void releaseAfterPublish() noexcept {
    queue_ = nullptr;
  }

  dxmt9::core::metalqueue::CpuReadySupplyObservationToken attemptToken()
      const noexcept {
    return attemptToken_;
  }

private:
  dxmt9::CommandQueue* queue_ = nullptr;
  dxmt9::core::CpuReadyTape::PayloadKind sourceClass_ =
      dxmt9::core::CpuReadyTape::PayloadKind::Legacy;
  dxmt9::core::metalqueue::CpuReadySupplyObservationToken attemptToken_{};
};

constexpr dxmt9::core::CpuReadyProducerIdentity cpuReadyProducerIdentity(
    const D9CCommandChunkProducerIdentity& identity) noexcept {
  return {
      .firstEventOrdinal = identity.firstEventOrdinal,
      .lastEventOrdinal = identity.lastEventOrdinal,
      .firstSourceOrdinal = identity.firstSourceOrdinal,
      .lastSourceOrdinal = identity.lastSourceOrdinal,
  };
}

dxmt9::d3d9::ReplaySourceIdentity replaySourceIdentity(
    const dxmt9::d3d9::RawCommandChunk& raw) noexcept {
  const auto sequence = raw.replaySeq != 0u ? raw.replaySeq : 1u;
  const auto source = raw.producerIdentity.firstSourceOrdinal != 0u
      ? raw.producerIdentity.firstSourceOrdinal
      : sequence;
  const auto lastSource = raw.producerIdentity.lastSourceOrdinal >= source
      ? raw.producerIdentity.lastSourceOrdinal
      : source;
  return {
      .source = source,
      .lastSource = lastSource,
      .sequence = sequence,
  };
}

dxmt9::d3d9::ReplayTransaction& beginReplayTransaction(
    D9CDevice* device, const dxmt9::d3d9::RawCommandChunk& raw,
    std::size_t compatibilitySourceCount = 1u,
    std::uint32_t firstRecordOrdinal = 0u) noexcept {
  auto& transaction = replayScratchArena().transaction;
  auto identity = replaySourceIdentity(raw);
  if (raw.producerIdentity.firstSourceOrdinal == 0u &&
      compatibilitySourceCount != 0u &&
      compatibilitySourceCount - 1u <=
          std::numeric_limits<std::uint64_t>::max() - identity.source) {
    identity.lastSource = identity.source + compatibilitySourceCount - 1u;
  }
  transaction.begin(identity, device ? &device->dev() : nullptr,
                    firstRecordOrdinal);
  return transaction;
}

bool rollbackReplayTransaction(
    D9CDevice* device,
    dxmt9::d3d9::ReplayTransaction& transaction) noexcept {
  if (!device || transaction.state().irreversible()) {
    (void)transaction.failStop();
    return false;
  }
  // mutableState() invalidates every derived draw cache. The journal restores
  // the exact semantic fields; conservative dirty generations are preferable
  // to reviving a cache built from the abandoned working state.
  return transaction.rollback(device->dev());
}

dxmt9::d3d9::ReplayDestinationReceipt compatibilityReplayReceipt(
    const dxmt9::d3d9::ReplayTransaction& transaction) noexcept {
  return {
      .kind = dxmt9::d3d9::ReplayDestinationKind::Compatibility,
      .identity = transaction.state().identity,
      .queueSequence = transaction.state().identity.sequence,
      .commandCount = transaction.state().stagedCommandCount,
  };
}

dxmt9::d3d9::ReplayDestinationReceipt publishedReplayReceipt(
    const dxmt9::d3d9::ReplayTransaction& transaction,
    const dxmt9::core::CpuReadyPublicationTicket& ticket,
    std::size_t controlIndex,
    dxmt9::d3d9::ReplayDestinationKind kind) noexcept {
  const auto& identity = transaction.state().identity;
  const bool producerIdentityPresent = !ticket.producerIdentity.absent();
  if (kind == dxmt9::d3d9::ReplayDestinationKind::Compatibility ||
      !ticket.strictIdentityValid() || ticket.rawOrdinal != identity.sequence ||
      (producerIdentityPresent &&
       (ticket.producerIdentity.firstSourceOrdinal != identity.source ||
        ticket.producerIdentity.lastSourceOrdinal != identity.lastSource)) ||
      controlIndex > std::numeric_limits<std::uint32_t>::max()) {
    return {};
  }
  return {
      .kind = kind,
      .identity = identity,
      .queueSequence = ticket.seqId,
      .buildGeneration = ticket.buildGeneration,
      .sourceGeneration = ticket.id.generation,
      .storageGeneration = ticket.storage.generation,
      .controlIndex = static_cast<std::uint32_t>(controlIndex),
      .commandCount = transaction.state().stagedCommandCount,
  };
}

bool commitReplayTransaction(
    dxmt9::d3d9::ReplayTransaction& transaction,
    dxmt9::d3d9::ReplayDestinationReceipt receipt) noexcept {
  if (!transaction.receiveDestination(receipt)) {
    (void)transaction.failStop();
    return false;
  }
  if (!transaction.commit()) {
    (void)transaction.failStop();
    return false;
  }
  return true;
}

int32_t replayCompatibilityChunk(
    D9CDevice* device, dxmt9::d3d9::RawCommandChunk& raw,
    bool pacedByPresentOrdinal, bool containsOrderedControls = false) {
  auto& transaction = beginReplayTransaction(device, raw);
  const auto hr = replayResolvedChunk(
      device, raw, pacedByPresentOrdinal, transaction, nullptr, {},
      containsOrderedControls);
  if (failed(hr)) {
    (void)rollbackReplayTransaction(device, transaction);
    return hr;
  }
  if (!commitReplayTransaction(transaction,
                               compatibilityReplayReceipt(transaction))) {
    return commitChunkFail("chunk-replay-transaction-commit");
  }
  return hr;
}

// ---------------------------------------------------------------------------
// Lease-span replay (R-BACK-2.102).
//
// The whole-raw gate answers one question: may this *entire* raw construct
// directly? One coordinator command anywhere demoted every draw in the raw to
// compatibility replay. The executable partition replaces that with an ordered
// sequence of spans: each maximal run of direct islands and coordinator
// locators between an ordered control or a compatibility range owns exactly
// one final-slot lease, and the cuts execute at their exact serial positions
// through the ordinary sink they already used.
//
// Per span, exactly one of each owner: one ReplayTransaction, one destination
// receipt, one resource-marking/retention owner. Source-level completion
// settles every span exactly once, in order.
//
// The failure algebra is the reason this is not just a loop. Before any span
// has applied an effect the raw is still wholly rollbackable and Legacy may
// own it, exactly as today. Once any span has committed, or any separator has
// executed, a later failure MUST NOT hand the raw back to Legacy: that would
// replay an already-executed prefix. It is a typed fail-stop cut instead.
// `outcome` reports what actually happened to the *direct* attempt, which is
// not derivable from the returned HRESULT: this driver can return D3D_OK after
// having handed the raw to compatibility replay, and reporting that as
// `Committed` would put a fallback population inside the committed row. It is
// seeded with the pre-effect fallback value and narrowed as the driver makes
// progress, so every early return is already classified.
int32_t replayEmissionSpans(
    D9CDevice* device, dxmt9::d3d9::RawCommandChunk& raw,
    bool pacedByPresentOrdinal,
    const dxmt9::d3d9::ImportedChunkView& imported,
    const dxmt9::d3d9::ReplayEmissionPlan& plan,
    dxmt9::CommandQueue& queue, bool observability,
    dxmt9::perf::DirectChunkSlotReplayOutcome& outcome) {
  outcome = dxmt9::perf::DirectChunkSlotReplayOutcome::NotAttempted;
  // Whole-raw ordered-control preflight, once, before any span executes.
  // Every span call below then skips its own preflight: a span may
  // legitimately contain no control, and repeating the scan per span would
  // let a late malformed control fail after an earlier span's effects.
  if (plan.orderedControlCount != 0) {
    bool foundOrderedControl = false;
    for (std::size_t index = 0; index < imported.records.size(); ++index) {
      const auto record = imported.record(index);
      switch (record.header.type) {
      case D9C_COMMAND_RECORD_QUERY_ISSUE:
      case D9C_COMMAND_RECORD_READBACK:
      case D9C_COMMAND_RECORD_UPDATE_TEXTURE:
        foundOrderedControl = true;
        if (!dxmt9::d3d9::makeOrderedControlDisposition(
                record, raw.replaySeq, index)) {
          outcome = dxmt9::perf::DirectChunkSlotReplayOutcome::PlanRejected;
          return commitChunkFail(
              "chunk-span-ordered-control-preflight",
              static_cast<std::uint32_t>(index), record.header.type);
        }
        break;
      default:
        break;
      }
    }
    if (!foundOrderedControl) {
      outcome = dxmt9::perf::DirectChunkSlotReplayOutcome::PlanRejected;
      return commitChunkFail("chunk-span-ordered-control-preflight-empty");
    }
  }

  const auto producerIdentity = cpuReadyProducerIdentity(raw.producerIdentity);
  // True once any span has applied a semantic effect. From that point the raw
  // is jointly owned and no whole-raw Legacy retry is sound.
  bool separatorEffectsStarted = false;
  const auto failStopAfterEffects = [&](const char* reason,
                                        std::uint32_t index) noexcept {
    if (observability) dxmt9::perf::countReplaySpanSeparatorFailStop();
    return commitChunkFail(reason, index);
  };

  for (const auto& span : plan.leaseSpans) {
    const ReplayRecordRange range{
        .firstRecordIndex = span.firstRecordIndex,
        .recordCount = span.recordCount,
        .orderedControlPreflightDone = true,
        .countAdmit = false,
    };
    // Only the separator span itself carries the control. A state-only run
    // that merely *precedes* one shares the trailing cut but holds no control
    // record, so key this on the span's own leading segment kind rather than
    // on what follows it.
    const bool spanHasOrderedControl =
        span.firstSegmentIndex < plan.segments.size() &&
        plan.segments[span.firstSegmentIndex].kind ==
            dxmt9::d3d9::EmissionSegmentKind::OrderedControlLocator;

    if (!span.ownsLease) {
      // Separators and draw-free runs replay exactly where they already did:
      // through the ordinary sink, at their exact serial position. A
      // compatibility range therefore cannot poison the direct spans around
      // it -- it only ends the one before it and delays the one after it.
      // Count the cut itself, not a draw-free run that merely precedes one.
      if (observability && span.firstSegmentIndex < plan.segments.size()) {
        switch (plan.segments[span.firstSegmentIndex].kind) {
        case dxmt9::d3d9::EmissionSegmentKind::OrderedControlLocator:
          dxmt9::perf::countReplaySpanOrderedControlCut();
          break;
        case dxmt9::d3d9::EmissionSegmentKind::CompatibilityRange:
          dxmt9::perf::countReplaySpanCompatibilityCut();
          break;
        default:
          break;
        }
      }
      markLegacyResources(device, raw);
      auto& transaction = beginReplayTransaction(
          device, raw, /*compatibilitySourceCount=*/1u,
          span.firstRecordIndex);
      const auto hr = replayResolvedChunk(
          device, raw, pacedByPresentOrdinal, transaction, nullptr, {},
          spanHasOrderedControl, {}, /*activeDirectChunkSlotPath=*/false,
          nullptr, range);
      if (failed(hr)) {
        outcome = dxmt9::perf::DirectChunkSlotReplayOutcome::ReplayFailed;
        if (separatorEffectsStarted) {
          (void)transaction.failStop();
          return failStopAfterEffects("chunk-span-ordinary",
                                      span.firstRecordIndex);
        }
        (void)rollbackReplayTransaction(device, transaction);
        return hr;
      }
      if (!commitReplayTransaction(transaction,
                                   compatibilityReplayReceipt(transaction))) {
        outcome = dxmt9::perf::DirectChunkSlotReplayOutcome::CommitFailed;
        return commitChunkFail("chunk-span-ordinary-commit",
                               span.firstRecordIndex);
      }
      separatorEffectsStarted = true;
      continue;
    }

    auto begin = queue.beginDirectChunkSlotReplay(
        raw.replaySeq, span.capacity, span.plannedBytes, producerIdentity,
        span.recordCount, span.drawCount, span.leaseOrdinal,
        span.finalLeaseSpan, /*allowRotation=*/true);
    if (begin.status !=
            dxmt9::CommandQueue::DirectChunkSlotReplayStatus::Ready ||
        !begin.lease) {
      if (separatorEffectsStarted ||
          begin.status ==
              dxmt9::CommandQueue::DirectChunkSlotReplayStatus::FailStopped) {
        dxmt9::util::logf(
            dxmt9::util::LogLevel::Info, "dxmt9-device",
            "direct_span_begin_reject raw=%llu span=%u status=%u reason=%u "
            "publication_reason=%u admission=%u",
            static_cast<unsigned long long>(raw.replaySeq),
            static_cast<unsigned>(span.leaseOrdinal),
            static_cast<unsigned>(begin.status),
            static_cast<unsigned>(begin.failureReason),
            static_cast<unsigned>(begin.publicationFailure),
            static_cast<unsigned>(begin.spanAdmission));
      }
      if (begin.status ==
          dxmt9::CommandQueue::DirectChunkSlotReplayStatus::FailStopped) {
        outcome = dxmt9::perf::DirectChunkSlotReplayOutcome::BeginFailStopped;
        return commitChunkFail("chunk-span-begin", span.firstRecordIndex);
      }
      if (separatorEffectsStarted) {
        // A pre-effect admission failure for *this* span is still a hard stop
        // once an earlier span has executed: the alternatives are replaying
        // an executed prefix or splitting one span's draws across two
        // representations, and both are worse than failing. It is no longer a
        // *pre-effect* outcome for the raw, so report the fail-stop instead.
        outcome = dxmt9::perf::DirectChunkSlotReplayOutcome::BeginFailStopped;
        return failStopAfterEffects("chunk-span-begin-after-effects",
                                    span.firstRecordIndex);
      }
      outcome = dxmt9::perf::DirectChunkSlotReplayOutcome::
          BeginLegacyPreEffectFailure;
      // Nothing has executed yet, so the raw is still wholly rollbackable and
      // Legacy owns it once -- exactly today's pre-effect fallback. The
      // counter targets zero: a non-zero row means a direct span degraded.
      if (observability) {
        dxmt9::perf::countReplaySpanOrdinaryFallbackDraws(span.drawCount);
      }
      markLegacyResources(device, raw);
      return replayCompatibilityChunk(device, raw, pacedByPresentOrdinal);
    }

    auto lease = std::move(*begin.lease);
    const auto* directRangeAppender = lease.borrowDirectRangeAppender();
    if (!directRangeAppender) {
      outcome = dxmt9::perf::DirectChunkSlotReplayOutcome::ReplayFailed;
      (void)lease.rollbackPreEffect();
      return commitChunkFail("chunk-span-appender", span.firstRecordIndex);
    }
    auto& transaction = beginReplayTransaction(
        device, raw, /*compatibilitySourceCount=*/1u, span.firstRecordIndex);
    const auto hr = replayResolvedChunk(
        device, raw, pacedByPresentOrdinal, transaction, nullptr, {},
        /*containsOrderedControls=*/false, {},
        /*activeDirectChunkSlotPath=*/true, directRangeAppender, range);
    if (failed(hr)) {
      outcome = dxmt9::perf::DirectChunkSlotReplayOutcome::ReplayFailed;
      if (transaction.state().irreversible()) {
        lease.markSemanticEffectsStarted();
        (void)transaction.failStop();
        return hr;
      }
      const bool stateRolledBack =
          rollbackReplayTransaction(device, transaction);
      const bool destinationRolledBack = lease.rollbackPreEffect();
      if (!stateRolledBack || !destinationRolledBack) {
        return hr;
      }
      if (separatorEffectsStarted) {
        return failStopAfterEffects("chunk-span-replay-after-effects",
                                    span.firstRecordIndex);
      }
      if (observability) {
        dxmt9::perf::countDirectChunkSlotReplayPostMaterializationFallback();
        dxmt9::perf::countReplaySpanOrdinaryFallbackDraws(span.drawCount);
      }
      // Already narrowed to ReplayFailed above; compatibility replay owning
      // the raw from here must not report the direct attempt as committed.
      markLegacyResources(device, raw);
      return replayCompatibilityChunk(device, raw, pacedByPresentOrdinal);
    }
    // Explicit cut at the span end. Coordinator appends already seal the open
    // run implicitly, but a span boundary is a cut the assembler never sees,
    // and `Draw, Draw, <cut>, Draw` must produce two run records, not one.
    lease.closeDirectRun();
    const auto destinationTicket = lease.ticket();
    const auto destinationControlIndex = lease.controlIndex();
    if (lease.commit(raw.resourceEntries) !=
        dxmt9::CommandQueue::DirectChunkSlotReplayStatus::Committed) {
      outcome = dxmt9::perf::DirectChunkSlotReplayOutcome::CommitFailed;
      (void)transaction.failStop();
      return commitChunkFail("chunk-span-commit", span.firstRecordIndex);
    }
    const auto destinationReceipt = publishedReplayReceipt(
        transaction, destinationTicket, destinationControlIndex,
        dxmt9::d3d9::ReplayDestinationKind::DirectChunkSlot);
    if (!commitReplayTransaction(transaction, destinationReceipt)) {
      outcome = dxmt9::perf::DirectChunkSlotReplayOutcome::CommitFailed;
      return commitChunkFail("chunk-span-state-commit", span.firstRecordIndex);
    }
    // At least one lease reached publication. A later span may still fail, and
    // will narrow this again; nothing after this point can fall back.
    outcome = dxmt9::perf::DirectChunkSlotReplayOutcome::Committed;
    separatorEffectsStarted = true;
    if (observability) {
      dxmt9::perf::countReplaySpanLease(
          span.drawCount, span.drawCount + span.coordinatorCount,
          span.islandCount, span.coordinatorCount);
      dxmt9::perf::countReplaySpanStateProjections(span.recordCount);
    }
  }
  dxmt9::perf::countChunkAdmit();
  return dxmt9::core::D3D_OK;
}

int32_t replayPlannedChunk(D9CDevice* device,
                             dxmt9::d3d9::RawCommandChunk& raw,
                             bool pacedByPresentOrdinal,
                             bool allowDirectArena,
                             bool forceEventSerial = false,
                             bool reportOffloadReplayStage = false) {
  // 64 pages is a capture planner/source-grouping bound, not queue storage
  // capacity. It applies only after the PE bridge has authenticated this raw
  // item to a capture token and event ordinal; startup/non-capture raws use
  // the queue's established 512-page/source EventSerial admission.
  constexpr std::size_t kCaptureSegmentSerialPagesPerSource = 64u;
  const bool captureIdentityRequested =
      raw.renderTapeCaptureToken != 0u && raw.renderTapeEventOrdinal != 0u;
  const bool segmentSerialRequested =
      device && !forceEventSerial && device->dev().upperDevice() &&
      device->dev().upperDevice()->queue().segmentSerialEnabled();
  const bool captureSegmentSerialRequested =
      captureIdentityRequested && segmentSerialRequested;
  const bool directReplayObservabilityEnabled = dxmt9::perf::enabled();
  static_assert(
      static_cast<std::uint8_t>(
          dxmt9::d3d9::DirectChunkSlotReplayDisposition::Count) ==
      static_cast<std::uint8_t>(
          dxmt9::perf::DirectChunkSlotReplayDisposition::Count));
  const auto toPerfDisposition =
      [](dxmt9::d3d9::DirectChunkSlotReplayDisposition disposition) noexcept {
        switch (disposition) {
        case dxmt9::d3d9::DirectChunkSlotReplayDisposition::Direct:
          return dxmt9::perf::DirectChunkSlotReplayDisposition::Direct;
        case dxmt9::d3d9::DirectChunkSlotReplayDisposition::DirectOversized:
          return dxmt9::perf::DirectChunkSlotReplayDisposition::DirectOversized;
        case dxmt9::d3d9::DirectChunkSlotReplayDisposition::
            DirectWithPresentTail:
          return dxmt9::perf::DirectChunkSlotReplayDisposition::
              DirectWithPresentTail;
        case dxmt9::d3d9::DirectChunkSlotReplayDisposition::LegacyStateOnly:
          return dxmt9::perf::DirectChunkSlotReplayDisposition::LegacyStateOnly;
        case dxmt9::d3d9::DirectChunkSlotReplayDisposition::LegacySegmented:
          return dxmt9::perf::DirectChunkSlotReplayDisposition::LegacySegmented;
        case dxmt9::d3d9::DirectChunkSlotReplayDisposition::LegacyUpDraw:
          return dxmt9::perf::DirectChunkSlotReplayDisposition::LegacyUpDraw;
        case dxmt9::d3d9::DirectChunkSlotReplayDisposition::LegacyPresent:
          return dxmt9::perf::DirectChunkSlotReplayDisposition::LegacyPresent;
        case dxmt9::d3d9::DirectChunkSlotReplayDisposition::LegacyUnsupported:
          return dxmt9::perf::DirectChunkSlotReplayDisposition::LegacyUnsupported;
        case dxmt9::d3d9::DirectChunkSlotReplayDisposition::LegacyOversized:
          return dxmt9::perf::DirectChunkSlotReplayDisposition::LegacyOversized;
        case dxmt9::d3d9::DirectChunkSlotReplayDisposition::LegacyCaptureOrTrace:
          return dxmt9::perf::DirectChunkSlotReplayDisposition::LegacyCaptureOrTrace;
        case dxmt9::d3d9::DirectChunkSlotReplayDisposition::InlineOrderedControl:
          return dxmt9::perf::DirectChunkSlotReplayDisposition::InlineOrderedControl;
        case dxmt9::d3d9::DirectChunkSlotReplayDisposition::RejectInvalid:
          return dxmt9::perf::DirectChunkSlotReplayDisposition::RejectInvalid;
        case dxmt9::d3d9::DirectChunkSlotReplayDisposition::Count:
          break;
        }
        DXMT_ASSERT(false);
        return dxmt9::perf::DirectChunkSlotReplayDisposition::RejectInvalid;
      };
  const auto recordDirectDisposition =
      [&](dxmt9::d3d9::DirectChunkSlotReplayDisposition disposition,
          dxmt9::perf::DirectChunkSlotReplayOutcome outcome) noexcept {
        if (directReplayObservabilityEnabled) {
          dxmt9::perf::recordDirectChunkSlotReplayDisposition(
              toPerfDisposition(disposition),
              outcome, raw.recordCount, raw.recordBytes);
        }
      };
  std::uint32_t capturePlanReason = std::numeric_limits<std::uint32_t>::max();
  const auto failCaptureIdentity = [&](const char* reason) {
    if (raw.renderTapeCaptureToken != 0u) {
      dxmt9::util::logf(
          dxmt9::util::LogLevel::Info, "dxmt9-device",
          "render_tape_identity_capture failed reason=%s token=%llu "
          "event=%llu records=%u planning=%u allow_direct=%u plan_reason=%u",
          reason,
          static_cast<unsigned long long>(raw.renderTapeCaptureToken),
          static_cast<unsigned long long>(raw.renderTapeEventOrdinal),
          raw.recordCount, raw.cpuReadyTapePlanningEnabled ? 1u : 0u,
          allowDirectArena ? 1u : 0u, capturePlanReason);
      device->renderTapeIdentityCapture.fail(raw.renderTapeCaptureToken);
    }
  };
  if ((raw.renderTapeCaptureToken == 0u) !=
      (raw.renderTapeEventOrdinal == 0u)) {
    failCaptureIdentity("token-event-pair");
  }
  if (!raw.cpuReadyTapePlanningEnabled) {
    // Gate-off is the historical R-BACK-2.51(a) lane: admission already used
    // the combined mark/capture hook before handoff and replay performs no
    // planning or repeated marking.
    if (captureIdentityRequested) failCaptureIdentity("planning-disabled");
    if (reportOffloadReplayStage) {
      dxmt9::perf::recordOffloadReplayStage(
          dxmt9::perf::OffloadReplayStage::Encode);
    }
    auto upper = device->dev().upperDevice();
    auto* queue = upper ? &upper->queue() : nullptr;
    if (queue) {
      queue->noteCpuReadySupplyReplayEntry(
          dxmt9::core::CpuReadyTape::PayloadKind::Legacy);
    }
    if (captureIdentityRequested && upper && queue &&
        upper->supportsDirectChunkSlotReplay() &&
        directReplayObservabilityEnabled) {
      // The raw is otherwise classified by this lane, so it owes exactly one
      // disposition; capture identity is why it does not get to construct
      // directly. Without this row the capture population is silently absent
      // rather than typed.
      recordDirectDisposition(
          dxmt9::d3d9::DirectChunkSlotReplayDisposition::LegacyCaptureOrTrace,
          dxmt9::perf::DirectChunkSlotReplayOutcome::NotAttempted);
    }
    if (!captureIdentityRequested && upper && queue &&
        upper->supportsDirectChunkSlotReplay()) {
      dxmt9::d3d9::ImportedChunkView imported;
      if (!importOwnedChunk(raw, imported)) {
        recordDirectDisposition(
            dxmt9::d3d9::DirectChunkSlotReplayDisposition::RejectInvalid,
            dxmt9::perf::DirectChunkSlotReplayOutcome::ImportRejected);
        return commitChunkFail("chunk-direct-slot-import");
      }
      const auto limits = queue->cpuReadyArenaPlanLimits();
      // The source-wide emission plan is the single routing authority for
      // this lane. It replaces the whole-raw gate, which asked only whether
      // the *entire* raw could construct directly and therefore demoted every
      // draw in a raw the moment it met one coordinator command. The plan
      // partitions the raw instead, and the executable spans it derives say
      // which ranges own a final-slot lease and which cuts execute where they
      // already did.
      const auto plan = dxmt9::d3d9::planReplayEmission(
          imported, raw.replaySeq,
          limits.pageSize == 0 ? 4096 : limits.pageSize);
      if (!plan.partitioned()) {
        const auto disposition =
            dxmt9::d3d9::emissionPlanDisposition(plan, /*captureOrTrace=*/false);
        if (directReplayObservabilityEnabled) {
          dxmt9::perf::countReplaySpanPlanRejected();
        }
        if (disposition ==
            dxmt9::d3d9::DirectChunkSlotReplayDisposition::RejectInvalid) {
          recordDirectDisposition(
              disposition,
              dxmt9::perf::DirectChunkSlotReplayOutcome::PlanRejected);
          return commitChunkFail("chunk-direct-slot-plan");
        }
        // A structurally sound view always partitions, so reaching here means
        // a checked overflow, a failed layout, or a failed reservation. All
        // are pre-effect, so Legacy owns the raw once.
        recordDirectDisposition(
            disposition,
            dxmt9::perf::DirectChunkSlotReplayOutcome::NotAttempted);
        markLegacyResources(device, raw);
        return replayCompatibilityChunk(device, raw, pacedByPresentOrdinal);
      }
      const auto disposition =
          dxmt9::d3d9::emissionPlanDisposition(plan, /*captureOrTrace=*/false);
      if (!plan.spansExecutable() || plan.leaseOwningSpanCount() == 0) {
        // Two pre-effect populations, both wholly compatibility-owned:
        //
        //  * `!spansExecutable()` -- a lease-owning span would have had to
        //    emit a Present that is not its trailing coordinator, or a second
        //    Present. Present is parked by the build context and appended once
        //    at commit as the slot's publication boundary, so a lease cannot
        //    represent `Draw, Present, Draw` in source order at all, and a
        //    second Present in one transaction is refused. The whole raw must
        //    fall back BEFORE any direct/Metal/queue effect: routing around
        //    one span is not available, because the remaining spans are only
        //    meaningful as a refinement of a stream this router declined.
        //  * no lease-owning span -- no island-eligible draw anywhere. This is
        //    the state-only / all-compatibility population and it keeps
        //    exactly the ownership it had before spans existed.
        if (directReplayObservabilityEnabled) {
          dxmt9::perf::countDirectChunkSlotReplayCheapRejected();
        }
        recordDirectDisposition(
            disposition,
            dxmt9::perf::DirectChunkSlotReplayOutcome::NotAttempted);
        markLegacyResources(device, raw);
        return replayCompatibilityChunk(device, raw, pacedByPresentOrdinal);
      }
      // The span driver can return D3D_OK after handing the raw to
      // compatibility replay, so the outcome it reports is not derivable from
      // the HRESULT and must not be inferred from it.
      auto outcome = dxmt9::perf::DirectChunkSlotReplayOutcome::NotAttempted;
      const auto hr = replayEmissionSpans(
          device, raw, pacedByPresentOrdinal, imported, plan, *queue,
          directReplayObservabilityEnabled, outcome);
      recordDirectDisposition(disposition, outcome);
      return hr;
    }
    markLegacyResources(device, raw);
    return replayCompatibilityChunk(device, raw, pacedByPresentOrdinal);
  }

  auto upper = device->dev().upperDevice();
  auto* queue = upper ? &upper->queue() : nullptr;
  dxmt9::d3d9::ImportedChunkView imported;
  if (!importOwnedChunk(raw, imported)) {
    if (queue) {
      queue->cancelCpuReadyNextSourceIntent(raw.replaySeq);
    }
    return commitChunkFail("chunk-planned-import");
  }
  const auto limits = queue
      ? queue->cpuReadyArenaPlanLimits()
      : dxmt9::CommandQueue::CpuReadyArenaPlanLimits{};
  const std::size_t plannerMaxPagesPerSource =
      captureSegmentSerialRequested
          ? kCaptureSegmentSerialPagesPerSource
          : (limits.maxPagesPerSource == 0
                 ? std::numeric_limits<std::uint32_t>::max()
                 : limits.maxPagesPerSource);
  const auto plan = dxmt9::d3d9::planCpuReadyChunk(
      imported, raw.replaySeq,
      dxmt9::d3d9::CpuReadyPlanOptions{
          .pageSize = limits.pageSize == 0 ? 4096 : limits.pageSize,
          .maxOrdinaryPagesPerSegment =
              limits.maxOrdinaryPagesPerSegment,
          .maxSegmentsPerSource = limits.maxSegmentsPerSource,
          .maxPagesPerSource = plannerMaxPagesPerSource,
          .maxPages = plannerMaxPagesPerSource,
          .maxSourcesPerChunk = captureSegmentSerialRequested
              ? dxmt9::core::CpuReadyTape::kMaxArenaBatchSources
              : 1,
      });
  if (reportOffloadReplayStage) {
    dxmt9::perf::recordOffloadReplayStage(
        dxmt9::perf::OffloadReplayStage::Encode);
  }
  capturePlanReason = static_cast<std::uint32_t>(plan.reason);
  if (queue && (!allowDirectArena || !plan.directArenaCandidate())) {
    // The predecessor may wait only for a source-producing Direct successor.
    // Cancel before StateOnly/Inline/Legacy/control replay has any effect.
    queue->cancelCpuReadyNextSourceIntent(raw.replaySeq);
  }
  const auto logAdmissionFailure = [&](std::uint32_t beginStatus,
                                       dxmt9::CommandQueue::
                                           CpuReadyArenaBeginStopReason
                                               stopReason) {
    if (!queue) {
      return;
    }
    const std::size_t plannedPages =
        plan.arenaLayout.has_value()
            ? plan.arenaLayout->pageCount
            : (plan.sources.empty() ? 0u
                                    : plan.sources.front().arenaLayout.pageCount);
    // Read the one-shot poison origin once on this failure-only path.  The
    // stop reason comes from the exact returned begin result below, never a
    // mutable queue-global value that a later admission can overwrite.
    const auto poisonOrigin = queue->cpuReadyArenaBeginDiagnostic().poisonOrigin;
    dxmt9::util::logf(
        dxmt9::util::LogLevel::Warn, "dxmt9-device",
        "cpu_ready_arena_admission begin_status=%u raw=%llu records=%u "
        "capture=%u sources=%zu segments=%zu planner_pages=%zu "
        "planner_page_bound=%zu queue_page_bound=%zu stop_reason=%s "
        "poison_origin=%s:%u poison_function=%s",
        beginStatus, static_cast<unsigned long long>(raw.replaySeq),
        raw.recordCount, captureIdentityRequested ? 1u : 0u,
        plan.sourceCount, plan.segmentCount, plannedPages,
        plannerMaxPagesPerSource, limits.maxPagesPerSource,
        cpuReadyArenaBeginStopReasonName(stopReason),
        poisonOrigin.file ? poisonOrigin.file : "none", poisonOrigin.line,
        poisonOrigin.function ? poisonOrigin.function : "none");
  };
  switch (plan.lane) {
  case dxmt9::d3d9::ReplayLane::Reject:
    if (captureSegmentSerialRequested) {
      // Checked planner/descriptor rejection is still pre-effect.  Retry the
      // complete event through the bounded v2 EventSerial planner before
      // treating the event as terminally malformed.
      return replayPlannedChunk(device, raw, pacedByPresentOrdinal,
                                allowDirectArena, /*forceEventSerial=*/true);
    }
    if (captureIdentityRequested) failCaptureIdentity("planner-reject");
    dxmt9::perf::countCpuReadySessionDisposition(
        dxmt9::perf::CpuReadySessionDisposition::Invalid);
    return commitChunkFail("chunk-planned-reject");
  case dxmt9::d3d9::ReplayLane::StateOnly:
    // State-only chunks mutate the replay shadow exactly once but publish no
    // GPU source and therefore consume neither a queue seq nor resource marks.
    if (captureIdentityRequested) failCaptureIdentity("state-only");
    return replayCompatibilityChunk(device, raw, pacedByPresentOrdinal);
  case dxmt9::d3d9::ReplayLane::Legacy:
    if (plan.reason == dxmt9::d3d9::ReplayReason::Oversize) {
      dxmt9::perf::countCpuReadyTapeLegacyOversizeBypass();
      dxmt9::perf::countCpuReadySessionDisposition(
          dxmt9::perf::CpuReadySessionDisposition::LegacyRollback);
    }
    if (queue) {
      queue->noteCpuReadySupplyReplayEntry(
          dxmt9::core::CpuReadyTape::PayloadKind::Legacy);
    }
    [[fallthrough]];
  case dxmt9::d3d9::ReplayLane::Inline:
    if (captureIdentityRequested) failCaptureIdentity("legacy-or-inline");
    markLegacyResources(device, raw);
    return replayCompatibilityChunk(
        device, raw, pacedByPresentOrdinal, plan.containsOrderedControls);
  case dxmt9::d3d9::ReplayLane::DirectArenaCandidate:
    break;
  }

  // Ordered-control records are coordinator-side and never children of a
  // projected pass. A captured event containing one must stay one EventSerial
  // source; deciding this here keeps the fallback pre-effect and preserves
  // its raw ordering.
  if (captureSegmentSerialRequested && plan.containsOrderedControls) {
    return replayPlannedChunk(device, raw, pacedByPresentOrdinal,
                              allowDirectArena, /*forceEventSerial=*/true);
  }

  if (!allowDirectArena) {
    // Inline execution (including synchronous Readback) still needs the
    // ordered-control plan and release hooks when the Tape gate is enabled.
    // Only Direct arena construction depends on worker-side arena admission.
    if (captureIdentityRequested) failCaptureIdentity("direct-arena-disallowed");
    markLegacyResources(device, raw);
    return replayCompatibilityChunk(
        device, raw, pacedByPresentOrdinal, plan.containsOrderedControls);
  }

  if (!queue || (!plan.arenaLayout.has_value() && plan.sources.empty()) ||
      (!segmentSerialRequested && captureIdentityRequested &&
       !plan.arenaLayout.has_value())) {
    // No D3D semantics have run yet, so a stub/non-production backend may
    // still use the compatibility lane without duplication.
    if (captureIdentityRequested) failCaptureIdentity("queue-or-layout-missing");
    markLegacyResources(device, raw);
    return replayCompatibilityChunk(device, raw, pacedByPresentOrdinal);
  }

  std::array<dxmt9::core::ArenaSourcePayloadLayout,
             dxmt9::core::CpuReadyTape::kMaxArenaBatchSources>
      sourceLayouts{};
  const bool segmentSerial = captureSegmentSerialRequested &&
      plan.sourceCount > 1;
  const std::size_t sourceCount = segmentSerial ? plan.sourceCount : 1;
  if (segmentSerial) {
    for (std::size_t i = 0; i < sourceCount; ++i) {
      sourceLayouts[i] = plan.sources[i].arenaLayout;
    }
  } else {
    sourceLayouts[0] = *plan.arenaLayout;
  }
  ScopedCpuReadySupplyReplayEntry supplyReplayEntry(
      *queue, dxmt9::core::CpuReadyTape::PayloadKind::Arena);
  dxmt9::CommandQueue::CpuReadyArenaBeginResult begin;
  while (true) {
    begin = segmentSerial
        ? queue->beginCpuReadyArenaSources(
              plan.rawOrdinal, std::span(sourceLayouts).first(sourceCount),
              supplyReplayEntry.attemptToken(),
              cpuReadyProducerIdentity(raw.producerIdentity))
        : queue->beginCpuReadyArenaSource(
              plan.rawOrdinal, sourceLayouts[0],
              supplyReplayEntry.attemptToken(),
              cpuReadyProducerIdentity(raw.producerIdentity));
    if (begin.status !=
        dxmt9::CommandQueue::CpuReadyArenaBeginStatus::TemporaryPressure) {
      break;
    }
    const auto waitLayouts = std::span(sourceLayouts).first(sourceCount);
    if (!queue->waitForCpuReadyArenaAdmission(waitLayouts)) {
      return commitChunkFail("chunk-arena-pressure-stopped");
    }
  }
  if (begin.status != dxmt9::CommandQueue::CpuReadyArenaBeginStatus::Ready ||
      !begin.has_value()) {
    logAdmissionFailure(static_cast<std::uint32_t>(begin.status),
                        begin.stopReason);
    if (segmentSerial && begin.status ==
            dxmt9::CommandQueue::CpuReadyArenaBeginStatus::RecoverableFailure) {
      // Admission and builder construction happen before replay invokes any
      // semantic sink.  The recoverable batch abort restored all Tape and
      // queue cursors, so EventSerial v2 may own this raw event exactly once.
      supplyReplayEntry.cancel();
      return replayPlannedChunk(device, raw, pacedByPresentOrdinal,
                                allowDirectArena, /*forceEventSerial=*/true);
    }
    // Oversize was classified by the structural planner. Any remaining
    // admission failure is queue terminal/corruption, not a legacy fallback.
    return commitChunkFail("chunk-arena-admission");
  }

  auto lease = std::move(*begin);
  // The imported RawCommandChunk and the Tape reservation now have one
  // explicit Unix-side owner.  The transfer keeps the retained wrappers and
  // immutable replay identity alive through Emit and Publish; its destructor
  // restores the Raw object to the worker so the existing completion/release
  // authority remains unchanged.
  dxmt9::d3d9::CpuReadySemanticTransfer transfer(raw, std::move(lease));
  if (!transfer.adopt()) {
    queue->failStopCpuReadyArena();
    return commitChunkFail("chunk-cpu-ready-transfer-adopt");
  }
  auto& transferRaw = transfer.raw();
  if (captureIdentityRequested && segmentSerial) {
    std::array<std::uint32_t,
               dxmt9::core::CpuReadyTape::kMaxArenaBatchSources>
        firstRecords{};
    std::array<std::uint32_t,
               dxmt9::core::CpuReadyTape::kMaxArenaBatchSources>
        recordCounts{};
    for (std::size_t source = 0; source < sourceCount; ++source) {
      firstRecords[source] = plan.sources[source].firstRecordIndex;
      recordCounts[source] = plan.sources[source].recordCount;
    }
    if (!transfer.lease().setCaptureSourceRanges(
            std::span(firstRecords).first(sourceCount),
            std::span(recordCounts).first(sourceCount))) {
      if (!transfer.abortForFallback()) {
        queue->failStopCpuReadyArena();
        return commitChunkFail("chunk-capture-ranges-rollback");
      }
      supplyReplayEntry.cancel();
      transfer.restoreToSource();
      return replayPlannedChunk(device, raw, pacedByPresentOrdinal,
                                allowDirectArena, /*forceEventSerial=*/true);
    }
  }
  auto& transaction = beginReplayTransaction(
      device, transferRaw, segmentSerial ? sourceCount : 1u);
  const int32_t hr = replayResolvedChunk(
      device, transferRaw, pacedByPresentOrdinal, transaction,
      &transfer.lease(),
      std::span(plan.segments).first(plan.segmentCount), false,
      segmentSerial ? std::span(plan.sources) :
                       std::span<const dxmt9::d3d9::CpuReadySourcePlan>{});
  if (failed(hr)) {
    const bool transactionMayRollback =
        !transaction.state().irreversible();
    const bool stateRolledBack = transactionMayRollback &&
        rollbackReplayTransaction(device, transaction);
    if (stateRolledBack) {
      if (!transfer.abortForFallback()) {
        queue->failStopCpuReadyArena();
        return commitChunkFail("chunk-capture-identity-rollback");
      }
      supplyReplayEntry.cancel();
      transfer.restoreToSource();
      return replayPlannedChunk(device, raw, pacedByPresentOrdinal,
                                /*allowDirectArena=*/false,
                                /*forceEventSerial=*/true);
    }
    (void)transaction.failStop();
    // Lease destruction performs the two-phase fail-stop abort. Replaying the
    // raw chunk through Legacy here would duplicate already-applied semantics.
    if (captureIdentityRequested) failCaptureIdentity("arena-replay");
    return hr;
  }
  if (!transfer.markEmitted()) {
    (void)transaction.failStop();
    queue->failStopCpuReadyArena();
    return commitChunkFail("chunk-cpu-ready-transfer-emit");
  }
  const auto admissionTicket = transfer.lease().ticket();
  const auto admissionControlIndex = transfer.lease().controlIndex();
  if (!segmentSerial && transferRaw.nextQueuedRawOrdinalHint != 0u) {
    (void)queue->armCpuReadyNextSourceIntent(
        admissionTicket, transferRaw.nextQueuedRawOrdinalHint,
        transferRaw.hasPresent);
  }
  dxmt9::CommandQueue::CpuReadyCaptureIdentity captureIdentity{};
  dxmt9::CommandQueue::CpuReadyCaptureIdentityBatch captureBatch{};
  const auto publishStatus = segmentSerial
      ? transfer.publishBatch(
            captureIdentityRequested ? &captureBatch : nullptr)
      : transfer.publish(
            captureIdentityRequested ? &captureIdentity : nullptr);
  if (publishStatus ==
      dxmt9::CommandQueue::CpuReadyArenaPublishStatus::RecoverableFailure) {
    // replayResolvedChunk has already applied D3D semantics and may have
    // touched presenter/Metal state. Recoverable is therefore no longer a
    // legal EventSerial fallback boundary: poison and fail-stop rather than
    // replaying this raw event a second time.
    queue->failStopCpuReadyArena();
    (void)transaction.failStop();
    const auto failure = queue->peekCpuReadyArenaFailure();
    if (failure.failureClass !=
        dxmt9::CommandQueue::CpuReadyArenaFailureClass::None) {
      dxmt9::util::logf(
          dxmt9::util::LogLevel::Warn, "dxmt9-device",
          "cpu_ready_arena post_replay_publish_failure raw=%llu "
          "records=%u class=%s source=%u segment=%u "
          "planned_pages=%u actual_commands=%u",
          static_cast<unsigned long long>(transferRaw.replaySeq),
          transferRaw.recordCount,
          cpuReadyArenaFailureClassName(failure.failureClass),
          failure.source, failure.segment, failure.plannedPages,
          failure.actualCommands);
    }
    if (captureIdentityRequested) failCaptureIdentity("arena-publish-after-effects");
    return commitChunkFail("chunk-arena-publish-after-effects");
  }
  if (publishStatus !=
      dxmt9::CommandQueue::CpuReadyArenaPublishStatus::Published) {
    (void)transaction.failStop();
    if (captureIdentityRequested) failCaptureIdentity("arena-publish");
    return commitChunkFail("chunk-arena-publish");
  }
  // publish/publishBatch has validated every source reservation, segment
  // assembler, storage generation, and commit evidence under the queue's
  // authoritative transaction. The first ticket plus its producer interval
  // identifies the complete physical batch for replay settlement.
  const auto destinationReceipt = publishedReplayReceipt(
      transaction, admissionTicket, admissionControlIndex,
      dxmt9::d3d9::ReplayDestinationKind::Arena);
  if (!commitReplayTransaction(transaction, destinationReceipt)) {
    queue->failStopCpuReadyArena();
    return commitChunkFail("chunk-arena-state-commit");
  }
  supplyReplayEntry.releaseAfterPublish();
  if (captureIdentityRequested && segmentSerial) {
    if (captureBatch.segments.empty()) {
      failCaptureIdentity("event-settlement-metadata");
      return dxmt9::core::D3D_OK;
    }
    for (const auto& segment : captureBatch.segments) {
      std::vector<dxmt9::d3d9::RenderTapeProductionPassRange> ranges;
      try {
        ranges.reserve(segment.ranges.size());
        for (const auto& range : segment.ranges) {
          ranges.push_back(dxmt9::d3d9::RenderTapeProductionPassRange{
              .firstRecord = range.firstRecord,
              .recordCount = range.recordCount,
              .dagPassIndex = range.dagPassIndex,
              .passKind = range.passKind,
              .logicalPassId = range.logicalPassId,
          });
        }
      } catch (...) {
        failCaptureIdentity("range-allocation");
        return dxmt9::core::D3D_OK;
      }
      if (!device->renderTapeIdentityCapture.append(
              transferRaw.renderTapeCaptureToken,
              transferRaw.renderTapeEventOrdinal,
              segment.sourceOrdinal, segment.seqId, segment.firstRecord,
              segment.recordCount, ranges)) {
        failCaptureIdentity("snapshot-or-ledger-append");
        break;
      }
    }
    if (!device->renderTapeIdentityCapture.registerExpectedSettlement(
            transferRaw.renderTapeCaptureToken,
            transferRaw.renderTapeEventOrdinal, transferRaw.replaySeq,
            admissionTicket.buildGeneration,
            captureBatch.segments.front().sourceOrdinal,
            captureBatch.segments.back().seqId,
            static_cast<std::uint32_t>(captureBatch.segments.size()))) {
      failCaptureIdentity("event-settlement");
    }
  } else if (captureIdentityRequested) {
    std::vector<dxmt9::d3d9::RenderTapeProductionPassRange> ranges;
    try {
      ranges.reserve(captureIdentity.ranges.size());
      for (const auto& range : captureIdentity.ranges) {
        ranges.push_back(dxmt9::d3d9::RenderTapeProductionPassRange{
            .firstRecord = range.firstRecord,
            .recordCount = range.recordCount,
            .dagPassIndex = range.dagPassIndex,
            .passKind = range.passKind,
        });
      }
    } catch (...) {
      failCaptureIdentity("range-allocation");
      return dxmt9::core::D3D_OK;
    }
    if (!captureIdentity.valid() ||
        !device->renderTapeIdentityCapture.append(
            transferRaw.renderTapeCaptureToken,
            transferRaw.renderTapeEventOrdinal, captureIdentity.sourceOrdinal,
            captureIdentity.seqId,
            captureIdentity.firstRecord, captureIdentity.recordCount,
            ranges)) {
      failCaptureIdentity("snapshot-or-ledger-append");
    } else if (!device->renderTapeIdentityCapture.registerExpectedSettlement(
                   transferRaw.renderTapeCaptureToken,
                   transferRaw.renderTapeEventOrdinal, transferRaw.replaySeq,
                   admissionTicket.buildGeneration,
                   admissionTicket.sourceOrdinal, admissionTicket.seqId, 1u)) {
      failCaptureIdentity("event-settlement");
    }
  }
  return dxmt9::core::D3D_OK;
}

}  // namespace

// Runs on the ReplayOffloadWorker thread. canonical admission has already validated
// the blob and resolved/retained every object, so the worker replays only that
// owned representation. The caller publishes ledger completion before it
// releases the wrapper retention that keeps ledger targets alive.
int32_t dxmt9::d3d9::replayRawChunk(D9CDevice* d, dxmt9::d3d9::RawCommandChunk& chunk) {
  if (chunk.wireVersion != D9C_COMMAND_CHUNK_VERSION) {
    dxmt9::perf::countCommandChunkReject();
    return commitChunkFail("offload-unsupported-wire-version");
  }
  const bool schedulingObservabilityEnabled = dxmt9::perf::enabled();
  if (schedulingObservabilityEnabled) {
    dxmt9::perf::recordOffloadReplayStage(
        dxmt9::perf::OffloadReplayStage::Plan);
  }
  const auto replayCpuStart = std::chrono::steady_clock::now();
  const int32_t hr = replayPlannedChunk(
      d, chunk, /*pacedByPresentOrdinal=*/true,
      /*allowDirectArena=*/true, /*forceEventSerial=*/false,
      schedulingObservabilityEnabled);
  if (schedulingObservabilityEnabled) {
    dxmt9::perf::recordOffloadReplayStage(
        dxmt9::perf::OffloadReplayStage::Done);
  }
  countDurationSince(replayCpuStart, dxmt9::perf::countOffloadReplayCpuTime);
  if (failed(hr)) {
    dxmt9::perf::countCommandChunkReject();
  }
  return hr;
}

namespace {
class ScopedRetainedWrapperRelease {
public:
  explicit ScopedRetainedWrapperRelease(
      dxmt9::d3d9::RawCommandChunk& chunk) noexcept
      : chunk_(chunk) {}
  ~ScopedRetainedWrapperRelease() noexcept {
    if (armed_) dxmt9::d3d9::releaseRetainedWrappers(chunk_);
  }
  void dismiss() noexcept { armed_ = false; }

private:
  dxmt9::d3d9::RawCommandChunk& chunk_;
  bool armed_ = true;
};
}  // namespace

int32_t dxmt9::d3d9::replayPrevalidatedResolvedCommandChunk(
    D9CDevice* d, std::span<const std::byte> bytes,
    const dxmt9::d3d9::CommandChunkEnvelope& envelope,
    std::span<void* const> resolvedObjects) noexcept {
  if (!d || bytes.empty() ||
      envelope.version != D9C_COMMAND_CHUNK_VERSION) {
    return commitChunkFail("provider-replay-bad-input");
  }

  dxmt9::d3d9::ImportedChunkView imported;
  if (!dxmt9::d3d9::importPrevalidatedCommandChunk(
          bytes, envelope, imported) ||
      resolvedObjects.size() != imported.handles.size()) {
    return commitChunkFail("provider-replay-import");
  }

  dxmt9::d3d9::RawCommandChunk raw;
  ScopedRetainedWrapperRelease retainedRelease(raw);
  bool ledgerPublished = false;
  try {
    auto* ledger = dxmt9::core::activeCopyMaterializationLedger(
        dxmt9::core::CopyMaterializationOwner::Unix);
    std::optional<dxmt9::core::CopyMaterializationEvent> rawOwnershipCopy;
    if (ledger) {
      rawOwnershipCopy.emplace(
          ledger, dxmt9::core::CopyMaterializationClass::BridgeRawOwnership,
          bytes.size());
    }
    raw.recordBlob.assign(
        reinterpret_cast<const dxmt9::core::u8*>(bytes.data()),
        reinterpret_cast<const dxmt9::core::u8*>(bytes.data() + bytes.size()));
    if (ledger) {
      raw.bridgeRawLedgerCharge.retain(ledger, raw.recordBlob.size());
    }
    raw.wireVersion = envelope.version;
    raw.recordCount = envelope.recordCount;
    raw.recordBytes = static_cast<std::uint32_t>(bytes.size());
    raw.handleCount = envelope.handleCount;
    raw.preflightValidated = true;
    raw.resolvedObjects.assign(resolvedObjects.begin(), resolvedObjects.end());
    raw.retainedWrappers.reserve(imported.handles.size());
    for (std::size_t index = 0; index < imported.handles.size(); ++index) {
      const auto kind = imported.handles[index].kind;
      void* object = resolvedObjects[index];
      if (!object) {
        return commitChunkFail("provider-replay-null-object",
                               static_cast<std::uint32_t>(index), kind);
      }
      raw.retainedWrappers.push_back({.kind = kind, .ptr = object});
      retainWrapper(kind, object);
    }
    raw.hasPresent = std::any_of(
        imported.records.begin(), imported.records.end(),
        [](const D9CCommandChunkWireRecordHeader& record) {
          return record.type == D9C_COMMAND_RECORD_PRESENT;
        });

    if (!persistResolvedResourcesAndCaptureBindings(d, raw, imported)) {
      return commitChunkFail("provider-replay-resource-capture");
    }
    if (!dxmt9::d3d9::drainDeferredReplay(d, "provider-render-tape")) {
      return commitChunkFail("provider-replay-drain");
    }
    if (!d->replayDrainLedger.publishInline(raw)) {
      return commitChunkFail("provider-replay-ledger");
    }
    ledgerPublished = true;

    const int32_t hr = replayPlannedChunk(
        d, raw, /*pacedByPresentOrdinal=*/false,
        /*allowDirectArena=*/false);
    if (!failed(hr)) {
      if (auto* observer = d->mutationCompositionObserver.get()) {
        // Inline replay is the rollback lane for commit offload and for
        // synchronous readback chunks; bind the same Render Tape snapshot
        // identity and source ordinal used by the worker path.
        for (const auto& snapshot : raw.bufferSnapshots) {
          observer->observeUse(
              {.resource = snapshot.buffer.value,
               .backingGeneration = snapshot.snapshot.contentRevision != 0u
                                        ? snapshot.snapshot.contentRevision
                                        : 1u,
               .sourceOrdinal = raw.replaySeq},
              dxmt9::resources::mutation_observer::ObserverKind::GpuUse);
        }
        if (raw.bufferSnapshots.empty()) {
          for (const auto& entry : raw.resourceEntries) {
            if (entry.kind != dxmt9::core::ChunkHandleKind::Buffer) continue;
            observer->observeUseForResource(
                entry.handle.value, raw.replaySeq,
                dxmt9::resources::mutation_observer::ObserverKind::GpuUse);
          }
        }
      }
      d->replayDrainLedger.publishReplayed(raw);
    } else {
      d->replayDrainLedger.poison();
      if (auto* observer = d->mutationCompositionObserver.get())
        observer->observeGlobalBarrier(
            dxmt9::resources::mutation_observer::BarrierReason::Failure);
    }
    return hr;
  } catch (const std::bad_alloc&) {
    if (ledgerPublished) d->replayDrainLedger.poison();
    return commitChunkFail("provider-replay-allocation", 0xffffffffu, 0u,
                           dxmt9::core::E_OUTOFMEMORY);
  } catch (...) {
    if (ledgerPublished) d->replayDrainLedger.poison();
    return commitChunkFail("provider-replay-exception");
  }
}

static int32_t dxmt9c_device_commit_chunk_impl(
    D9CDevice* d, const D9CCommandChunk* chunk,
    dxmt9::d3d9::RawCommandChunk* preparedRaw,
    std::chrono::steady_clock::time_point bridgeCommitStart) {
  // Boundary B2 — wall-clock latency of one commit_chunk bridge call.
  // This includes importer validation, handle/resource marking, record
  // replay, and queue submission construction. It excludes asynchronous
  // encode/GPU work after this call returns.
  if (!d || (!chunk && !preparedRaw)) {
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
  if (!preparedRaw && chunk->version != D9C_COMMAND_CHUNK_VERSION) {
    dxmt9::perf::countCommandChunkReject();
    return commitChunkFail("unsupported-wire-version", chunk->version);
  }
  {
    const bool phaseSplit = commitChunkPhaseSplitEnabled();
    if (phaseSplit && !preparedRaw) {
      dxmt9::perf::countCommitChunkPhaseCall();
    }
    dxmt9::d3d9::RawCommandChunk raw;
    bool prepared = false;
    if (preparedRaw) {
      raw = std::move(*preparedRaw);
      prepared = true;
    } else {
      if (wireHandleValue(chunk->handles) != 0u ||
          chunk->recordBytes == 0u ||
          chunk->recordBytes > D9C_COMMAND_CHUNK_MAX_TOTAL_WIRE_BYTES) {
        dxmt9::perf::countCommandChunkReject();
        return commitChunkFail("chunk-bad-outer");
      }
      const auto* records = wireHandlePtr<const std::byte>(chunk->records);
      if (!records) {
        dxmt9::perf::countCommandChunkReject();
        return commitChunkFail("chunk-missing-records");
      }
      const auto blob = std::span<const std::byte>(records, chunk->recordBytes);
      const dxmt9::d3d9::CommandChunkEnvelope envelope{
          .version = chunk->version,
          .recordCount = chunk->recordCount,
          .handleCount = chunk->handleCount,
          .producerIdentity = chunk->producerIdentity,
      };
      CommitChunkPhaseTimer preparePhase(phaseSplit);
      prepared = dxmt9::d3d9::prepareOffloadChunk(
          blob, envelope, d->wireObjects, retainWrapper, raw);
      preparePhase.stop(dxmt9::perf::countCommitChunkPhasePrepareCpuTime);
      if (prepared) {
        raw.renderTapeCaptureToken = chunk->renderTapeCaptureToken;
        raw.renderTapeEventOrdinal = chunk->renderTapeEventOrdinal;
      }
    }
    if (!prepared) {
      dxmt9::perf::countCommandChunkReject();
      return commitChunkFail("chunk-admission");
    }
    ScopedRetainedWrapperRelease retainedRelease(raw);
    dxmt9::d3d9::ImportedChunkView imported;
    CommitChunkPhaseTimer importPhase(phaseSplit);
    const bool importedOk =
        raw.preflightValidated &&
        importRawChunk(raw, imported);
    importPhase.stop(dxmt9::perf::countCommitChunkPhaseImportCpuTime);
    if (!importedOk) {
      dxmt9::d3d9::releaseRetainedWrappers(raw);
      dxmt9::perf::countCommandChunkReject();
      return commitChunkFail("chunk-owned-preflight-view");
    }
    CommitChunkPhaseTimer markPhase(phaseSplit);
    const bool resourcesMarked =
        persistResolvedResourcesAndCaptureBindings(d, raw, imported);
    markPhase.stop(dxmt9::perf::countCommitChunkPhaseMarkCpuTime);
    if (!resourcesMarked) {
      dxmt9::d3d9::releaseRetainedWrappers(raw);
      dxmt9::perf::countCommandChunkReject();
      return commitChunkFail("chunk-buffer-capture");
    }
    dxmt9::perf::countCommandChunkWire(
        raw.wireVersion, raw.recordCount, raw.recordBytes, raw.handleCount);

    if (auto* q = findDirtyQueue(d)) {
      q->noteCommitChunkEntryForCompletionGap();
    }
    if (dxmt9::d3d9::offloadCommitReplayEnabled() &&
        !chunkRequiresInlineReplay(imported)) {
      if (!d->replayOffload) {
        std::unique_ptr<dxmt9::d3d9::ReplayOffloadWorker> worker;
        try {
          worker =
              std::make_unique<dxmt9::d3d9::ReplayOffloadWorker>();
        } catch (...) {
          dxmt9::d3d9::releaseRetainedWrappers(raw);
          dxmt9::perf::countCommandChunkReject();
          return dxmt9::core::E_OUTOFMEMORY;
        }
        if (!worker->start(d)) {
          dxmt9::d3d9::releaseRetainedWrappers(raw);
          dxmt9::perf::countCommandChunkReject();
          return dxmt9::core::E_OUTOFMEMORY;
        }
        d->replayOffload = std::move(worker);
      }
      if (d->replayOffload->failed()) {
        dxmt9::d3d9::releaseRetainedWrappers(raw);
        dxmt9::perf::countCommandChunkReject();
        return commitChunkFail("chunk-offload-worker-failed");
      }
      const bool hasPresent = raw.hasPresent;
      CommitChunkPhaseTimer enqueuePhase(phaseSplit);
      dxmt9::perf::countOffloadReplayQueueDepth(
          static_cast<std::uint64_t>(d->replayOffload->queue().depth()));
      const auto pushDisposition =
          d->replayOffload->queue().pushWithDisposition(
          std::move(raw), &d->replayDrainLedger);
      enqueuePhase.stop(dxmt9::perf::countCommitChunkPhaseEnqueueCpuTime);
      if (pushDisposition ==
          dxmt9::d3d9::ReplayQueuePushDisposition::RejectedPreEffect) {
        dxmt9::d3d9::releaseRetainedWrappers(raw);
        dxmt9::perf::countCommandChunkReject();
        return commitChunkFail("chunk-offload-queue-stopped");
      }
      // Accepted and EffectUnknown both mean the queue adopted the chunk.
      // Transfer wrapper-release authority explicitly instead of relying on
      // the moved-from vectors in `raw` being empty.
      retainedRelease.dismiss();
      if (pushDisposition ==
          dxmt9::d3d9::ReplayQueuePushDisposition::EffectUnknown) {
        d->replayDrainLedger.poison();
        dxmt9::perf::countCommandChunkReject();
        return commitChunkFail("chunk-offload-queue-effect-unknown");
      }
      if (hasPresent) {
        ++d->presentOrdinal;
        if (auto* observer = d->mutationCompositionObserver.get())
          observer->notePresent();
        // Periodic, not at teardown: 3DMark05 never releases the device, so
        // ~D9CDevice does not run and a destructor-emitted report is lost --
        // verified empirically, the run's log ends at [dxmt9-perf] with no
        // site line despite 11.04 drain waits/present. Cumulative every 60
        // presents mirrors the PE decimated stats line for the same reason.
        if (d->presentOrdinal % 60 == 0) {
          dxmt9::d3d9::logDrainFenceSites(d->presentOrdinal);
        }
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
        // Report only after the Present boundary wait. By this point the
        // worker has settled all preceding mutation tasks and replay-use
        // observations; reporting before the wait would classify pending
        // completions as forbidden candidates and then reset their identity.
        if (auto* observer = d->mutationCompositionObserver.get();
            observer && d->presentOrdinal % 60u == 0u) {
          observer->finalize();
          const auto snapshot = observer->snapshot();
          const double presents = observer->windowPresents() == 0u
                                      ? 1.0
                                      : static_cast<double>(
                                            observer->windowPresents());
          const double candidateMs =
              static_cast<double>(snapshot.candidateCpuTimeSavedNs) /
              presents / 1.0e6;
          using RejectionReason =
              dxmt9::resources::mutation_observer::RejectionReason;
          using BarrierReason =
              dxmt9::resources::mutation_observer::BarrierReason;
          const auto rejectionCount = [&](const auto& counts,
                                          RejectionReason reason) {
            return counts[static_cast<std::size_t>(reason)];
          };
          std::uint64_t barriers = 0u;
          for (std::size_t i = 0u;
               i < static_cast<std::size_t>(BarrierReason::Count); ++i)
            barriers += snapshot.barrierCounts[i];
          std::uint64_t provisionalRejections = 0u;
          std::uint64_t finalRejections = 0u;
          for (std::size_t i = 0u;
               i < static_cast<std::size_t>(
                       dxmt9::resources::mutation_observer::RejectionReason::Count);
               ++i) {
            provisionalRejections += snapshot.rejectionCounts[i];
            finalRejections += snapshot.finalRejectionCounts[i];
          }
          const char* gate = candidateMs >= 0.5
                                 ? "open"
                                 : candidateMs < 0.2 ? "closed" : "inconclusive";
          dxmt9::util::logf(
              dxmt9::util::LogLevel::Info, "dxmt9-mutation-composition",
              "window_presents=%llu mutations=%llu mutation_bytes=%llu "
              "mergeable_range_pairs=%llu candidate_calls=%llu "
              "candidate_bytes=%llu "
              "candidate_cpu_time_saved_ns=%llu "
              "candidate_cpu_ms_per_present=%.6f "
              "wow64_writeback_ns=%llu queue_lock_ns=%llu "
              "backing_rotation_ns=%llu arena_update_ns=%llu "
              "shadow_copy_ns=%llu live_contents_copy_ns=%llu "
              "mergeable_union_bytes=%llu mergeable_overlap_bytes=%llu "
              "zero_use_generations=%llu discard_discard_dead=%llu "
              "barriers=%llu barrier_draw=%llu "
              "barrier_process_vertices=%llu barrier_read_lock=%llu "
              "barrier_query_readback=%llu barrier_update_copy=%llu "
              "barrier_cross_thread=%llu barrier_destroy_reset=%llu "
              "barrier_capture_lease=%llu barrier_failure=%llu "
              "barrier_unknown=%llu "
              "pending=%llu completed=%llu failed=%llu discarded=%llu "
              "overflow=%llu "
              "provisional_rejections=%llu final_rejections=%llu "
              "provisional_completion_rejections=%llu "
              "final_completion_rejections=%llu "
              "provisional_rejection_different_resource=%llu "
              "provisional_rejection_different_generation=%llu "
              "provisional_rejection_render_tape_identity=%llu "
              "provisional_rejection_disposition=%llu "
              "provisional_rejection_range_overlap=%llu "
              "provisional_rejection_barrier=%llu "
              "provisional_rejection_failure=%llu "
              "provisional_rejection_source_order=%llu "
              "provisional_rejection_capacity=%llu "
              "provisional_rejection_invalid=%llu "
              "provisional_rejection_completion=%llu "
              "final_rejection_different_resource=%llu "
              "final_rejection_different_generation=%llu "
              "final_rejection_render_tape_identity=%llu "
              "final_rejection_disposition=%llu "
              "final_rejection_range_overlap=%llu "
              "final_rejection_barrier=%llu "
              "final_rejection_failure=%llu "
              "final_rejection_source_order=%llu "
              "final_rejection_capacity=%llu "
              "final_rejection_invalid=%llu "
              "final_rejection_completion=%llu "
              "first_use_gpu=%llu first_use_cpu=%llu "
              "first_use_distance_total=%llu first_use_distance_max=%llu "
              "invalid_or_dropped=%llu "
              "gate=%s "
              "composition=forbidden reason=semantic-proof-required",
              static_cast<unsigned long long>(observer->windowPresents()),
              static_cast<unsigned long long>(snapshot.mutationCalls),
              static_cast<unsigned long long>(snapshot.mutationBytes),
              static_cast<unsigned long long>(snapshot.mergeableRangePairs),
              static_cast<unsigned long long>(snapshot.candidateCalls),
              static_cast<unsigned long long>(snapshot.candidateBytesSaved),
              static_cast<unsigned long long>(snapshot.candidateCpuTimeSavedNs),
              candidateMs,
              static_cast<unsigned long long>(snapshot.wow64WritebackNs),
              static_cast<unsigned long long>(snapshot.queueLockNs),
              static_cast<unsigned long long>(snapshot.backingRotationNs),
              static_cast<unsigned long long>(snapshot.arenaUpdateNs),
              static_cast<unsigned long long>(snapshot.shadowCopyNs),
              static_cast<unsigned long long>(snapshot.liveContentsCopyNs),
              static_cast<unsigned long long>(snapshot.mergeableUnionBytes),
              static_cast<unsigned long long>(snapshot.mergeableOverlapBytes),
              static_cast<unsigned long long>(snapshot.zeroUseGenerations),
              static_cast<unsigned long long>(
                  snapshot.discardToDiscardDeadChains),
              static_cast<unsigned long long>(barriers),
              static_cast<unsigned long long>(snapshot.barrierCounts[
                  static_cast<std::size_t>(BarrierReason::Draw)]),
              static_cast<unsigned long long>(snapshot.barrierCounts[
                  static_cast<std::size_t>(BarrierReason::ProcessVertices)]),
              static_cast<unsigned long long>(snapshot.barrierCounts[
                  static_cast<std::size_t>(BarrierReason::ReadLock)]),
              static_cast<unsigned long long>(snapshot.barrierCounts[
                  static_cast<std::size_t>(BarrierReason::QueryReadback)]),
              static_cast<unsigned long long>(snapshot.barrierCounts[
                  static_cast<std::size_t>(BarrierReason::UpdateCopy)]),
              static_cast<unsigned long long>(snapshot.barrierCounts[
                  static_cast<std::size_t>(BarrierReason::CrossThread)]),
              static_cast<unsigned long long>(snapshot.barrierCounts[
                  static_cast<std::size_t>(BarrierReason::DestroyReset)]),
              static_cast<unsigned long long>(snapshot.barrierCounts[
                  static_cast<std::size_t>(BarrierReason::CaptureLease)]),
              static_cast<unsigned long long>(snapshot.barrierCounts[
                  static_cast<std::size_t>(BarrierReason::Failure)]),
              static_cast<unsigned long long>(snapshot.barrierCounts[
                  static_cast<std::size_t>(BarrierReason::Unknown)]),
              static_cast<unsigned long long>(snapshot.pendingMutations),
              static_cast<unsigned long long>(snapshot.completedMutations),
              static_cast<unsigned long long>(snapshot.failedMutations),
              static_cast<unsigned long long>(snapshot.discardedMutations),
              static_cast<unsigned long long>(snapshot.overflowEvents),
              static_cast<unsigned long long>(provisionalRejections),
              static_cast<unsigned long long>(finalRejections),
              static_cast<unsigned long long>(rejectionCount(
                  snapshot.rejectionCounts, RejectionReason::Completion)),
              static_cast<unsigned long long>(rejectionCount(
                  snapshot.finalRejectionCounts, RejectionReason::Completion)),
              static_cast<unsigned long long>(rejectionCount(
                  snapshot.rejectionCounts,
                  RejectionReason::DifferentResource)),
              static_cast<unsigned long long>(rejectionCount(
                  snapshot.rejectionCounts,
                  RejectionReason::DifferentGeneration)),
              static_cast<unsigned long long>(rejectionCount(
                  snapshot.rejectionCounts,
                  RejectionReason::RenderTapeIdentity)),
              static_cast<unsigned long long>(rejectionCount(
                  snapshot.rejectionCounts, RejectionReason::Disposition)),
              static_cast<unsigned long long>(rejectionCount(
                  snapshot.rejectionCounts, RejectionReason::RangeOverlap)),
              static_cast<unsigned long long>(rejectionCount(
                  snapshot.rejectionCounts, RejectionReason::Barrier)),
              static_cast<unsigned long long>(rejectionCount(
                  snapshot.rejectionCounts, RejectionReason::Failure)),
              static_cast<unsigned long long>(rejectionCount(
                  snapshot.rejectionCounts, RejectionReason::SourceOrder)),
              static_cast<unsigned long long>(rejectionCount(
                  snapshot.rejectionCounts, RejectionReason::Capacity)),
              static_cast<unsigned long long>(rejectionCount(
                  snapshot.rejectionCounts, RejectionReason::Invalid)),
              static_cast<unsigned long long>(rejectionCount(
                  snapshot.rejectionCounts, RejectionReason::Completion)),
              static_cast<unsigned long long>(rejectionCount(
                  snapshot.finalRejectionCounts,
                  RejectionReason::DifferentResource)),
              static_cast<unsigned long long>(rejectionCount(
                  snapshot.finalRejectionCounts,
                  RejectionReason::DifferentGeneration)),
              static_cast<unsigned long long>(rejectionCount(
                  snapshot.finalRejectionCounts,
                  RejectionReason::RenderTapeIdentity)),
              static_cast<unsigned long long>(rejectionCount(
                  snapshot.finalRejectionCounts, RejectionReason::Disposition)),
              static_cast<unsigned long long>(rejectionCount(
                  snapshot.finalRejectionCounts, RejectionReason::RangeOverlap)),
              static_cast<unsigned long long>(rejectionCount(
                  snapshot.finalRejectionCounts, RejectionReason::Barrier)),
              static_cast<unsigned long long>(rejectionCount(
                  snapshot.finalRejectionCounts, RejectionReason::Failure)),
              static_cast<unsigned long long>(rejectionCount(
                  snapshot.finalRejectionCounts, RejectionReason::SourceOrder)),
              static_cast<unsigned long long>(rejectionCount(
                  snapshot.finalRejectionCounts, RejectionReason::Capacity)),
              static_cast<unsigned long long>(rejectionCount(
                  snapshot.finalRejectionCounts, RejectionReason::Invalid)),
              static_cast<unsigned long long>(rejectionCount(
                  snapshot.finalRejectionCounts, RejectionReason::Completion)),
              static_cast<unsigned long long>(snapshot.firstUseGpuCount),
              static_cast<unsigned long long>(snapshot.firstUseCpuCount),
              static_cast<unsigned long long>(snapshot.firstUseDistanceTotal),
              static_cast<unsigned long long>(snapshot.firstUseDistanceMax),
              static_cast<unsigned long long>(snapshot.invalidOrDroppedEvents),
              gate);
          observer->reset();
        }
      }
      countDurationSince(bridgeCommitStart,
                         dxmt9::perf::countOffloadCommitAppCpuTime);
      return dxmt9::core::D3D_OK;
    }

    // Inline-replay lane, reached when the offload is disabled OR the chunk
    // contains a synchronousReadBoundary record (READBACK). In the second case
    // the offload worker may still hold earlier chunks, and replaying this one
    // here without draining first would run it BEFORE them and CONCURRENTLY
    // with them -- violating R-BACK-2.51(b) (single FIFO worker, never
    // reordered or parallelized) and (d). No-op when the offload is off or the
    // queue is empty, so the non-readback path pays one pointer test.
    dxmt9::d3d9::drainDeferredReplay(d, "dxmt9c_device_commit_chunk_inline");
    // The drain is stop-aware: waitDrained() returns immediately once the queue
    // has been stopped, even with chunks still queued. After a worker fail-stop
    // those chunks were dropped unreplayed, so "drained" here does not mean
    // "caught up" -- replaying now would run against state missing everything
    // the worker discarded and hand back D3D_OK, which for a READBACK chunk is
    // wrong data reported as success. The offload lane already poisons later
    // commits on failed(); this lane has to as well, or it escapes the
    // fail-stop contract the offload's safety story rests on.
    if (d->replayOffload && d->replayOffload->failed()) {
      dxmt9::d3d9::releaseRetainedWrappers(raw);
      dxmt9::perf::countCommandChunkReject();
      return commitChunkFail("chunk-offload-worker-failed-inline");
    }
    if (!d->replayDrainLedger.publishInline(raw)) {
      dxmt9::d3d9::releaseRetainedWrappers(raw);
      dxmt9::perf::countCommandChunkReject();
      return commitChunkFail("chunk-replay-ledger-stopped-inline");
    }
    const int32_t hr = replayPlannedChunk(
        d, raw, /*pacedByPresentOrdinal=*/false,
        /*allowDirectArena=*/false);
    if (!failed(hr)) {
      if (auto* observer = d->mutationCompositionObserver.get()) {
        // Inline replay is the rollback lane for commit offload and for
        // synchronous readback chunks; bind the same snapshot identity and
        // source ordinal used by the worker path.
        for (const auto& snapshot : raw.bufferSnapshots) {
          observer->observeUse(
              {.resource = snapshot.buffer.value,
               .backingGeneration = snapshot.snapshot.contentRevision != 0u
                                        ? snapshot.snapshot.contentRevision
                                        : 1u,
               .sourceOrdinal = raw.replaySeq},
              dxmt9::resources::mutation_observer::ObserverKind::GpuUse);
        }
        if (raw.bufferSnapshots.empty()) {
          for (const auto& entry : raw.resourceEntries) {
            if (entry.kind != dxmt9::core::ChunkHandleKind::Buffer) continue;
            observer->observeUseForResource(
                entry.handle.value, raw.replaySeq,
                dxmt9::resources::mutation_observer::ObserverKind::GpuUse);
          }
        }
      }
      d->replayDrainLedger.publishReplayed(raw);
    } else {
      d->replayDrainLedger.poison();
      if (auto* observer = d->mutationCompositionObserver.get())
        observer->observeGlobalBarrier(
            dxmt9::resources::mutation_observer::BarrierReason::Failure);
    }
    dxmt9::d3d9::releaseRetainedWrappers(raw);
    if (!failed(hr)) {
      const auto elapsed = std::chrono::steady_clock::now() - bridgeCommitStart;
      dxmt9::perf::countBridgeCommitLatencyNs(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
              .count()));
    }
    if (failed(hr)) {
      dxmt9::perf::countCommandChunkReject();
    }
    return hr;
  }
}

extern "C" int32_t dxmt9c_device_commit_chunk(
    D9CDevice* d, const D9CCommandChunk* chunk) {
  try {
    return dxmt9c_device_commit_chunk_impl(
        d, chunk, nullptr, std::chrono::steady_clock::now());
  } catch (const std::bad_alloc&) {
    // Any exception that escapes the implementation may have followed a
    // ledger publication.  Poison conservatively; the local RawCommandChunk
    // guard has already released only the wrapper references it still owns.
    if (d) d->replayDrainLedger.poison();
    dxmt9::perf::countCommandChunkReject();
    return commitChunkFail("chunk-boundary-allocation", 0xffffffffu, 0u,
                           dxmt9::core::E_OUTOFMEMORY);
  } catch (...) {
    if (d) d->replayDrainLedger.poison();
    dxmt9::perf::countCommandChunkReject();
    return commitChunkFail("chunk-boundary-exception");
  }
}

extern "C" int32_t dxmt9c_device_commit_chunk_segmented(
    D9CDevice* d, const D9CCommandChunkSegmentedTransportV1* transport) {
  try {
    const auto bridgeCommitStart = std::chrono::steady_clock::now();
    if (!d || !transport ||
        d->commandChunkTransport != D9C_COMMAND_CHUNK_TRANSPORT_SEGMENTED_V1) {
      return commitChunkFail("segmented-transport-not-negotiated");
    }
    const auto spanFor = [](const D9CWireHandle& handle,
                            std::uint32_t bytes) {
      if (bytes == 0u) return std::span<const std::byte>{};
      const auto address = wireHandleValue(handle);
      if (address > std::numeric_limits<std::uintptr_t>::max() - bytes) {
        return std::span<const std::byte>{};
      }
      const auto* ptr = wireValuePtr<const std::byte>(address);
      return ptr ? std::span<const std::byte>(ptr, bytes)
                 : std::span<const std::byte>{};
    };
    const auto roleTokenMatchesExtent = [](const D9CWireHandle& handle,
                                           std::uint32_t bytes) noexcept {
      return (bytes == 0u) == (wireHandleValue(handle) == 0u);
    };
    if (!roleTokenMatchesExtent(transport->records, transport->recordBytes) ||
        !roleTokenMatchesExtent(transport->handles, transport->handleBytes) ||
        !roleTokenMatchesExtent(transport->payload, transport->payloadBytes)) {
      return commitChunkFail("segmented-transport-role-token");
    }
    const auto records = spanFor(transport->records, transport->recordBytes);
    const auto handles = spanFor(transport->handles, transport->handleBytes);
    const auto payload = spanFor(transport->payload, transport->payloadBytes);
    if ((transport->recordBytes != 0u && records.empty()) ||
        (transport->handleBytes != 0u && handles.empty()) ||
        (transport->payloadBytes != 0u && payload.empty())) {
      return commitChunkFail("segmented-transport-pointer");
    }
    dxmt9::d3d9::RawCommandChunk raw;
    const bool phaseSplit = commitChunkPhaseSplitEnabled();
    if (phaseSplit) {
      dxmt9::perf::countCommitChunkPhaseCall();
    }
    CommitChunkPhaseTimer preparePhase(phaseSplit);
    if (!dxmt9::d3d9::prepareSegmentedOffloadChunk(
            *transport, records, handles, payload, d->commandChunkRegionPool,
            d->wireObjects,
            retainWrapper, raw)) {
      preparePhase.stop(dxmt9::perf::countCommitChunkPhasePrepareCpuTime);
      dxmt9::perf::countCommandChunkReject();
      return commitChunkFail("segmented-chunk-admission");
    }
    preparePhase.stop(dxmt9::perf::countCommitChunkPhasePrepareCpuTime);
    // The common implementation immediately moves the move-only region lease,
    // exact ledger charge, and wrapper batch into its guarded owner.
    return dxmt9c_device_commit_chunk_impl(d, nullptr, &raw,
                                           bridgeCommitStart);
  } catch (const std::bad_alloc&) {
    if (d) d->replayDrainLedger.poison();
    dxmt9::perf::countCommandChunkReject();
    return commitChunkFail("segmented-boundary-allocation", 0xffffffffu, 0u,
                           dxmt9::core::E_OUTOFMEMORY);
  } catch (...) {
    if (d) d->replayDrainLedger.poison();
    dxmt9::perf::countCommandChunkReject();
    return commitChunkFail("segmented-boundary-exception");
  }
}
