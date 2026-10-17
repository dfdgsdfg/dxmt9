#include "dxmt9/dxmt9_trace_bindings.hpp"
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
#include "device_c_render_tape_runtime_binding_v4.hpp"
#include "device_c_render_tape_runtime_identity_core.hpp"
#include "device_c_render_tape_runtime_replay_v4.hpp"
#include "device_c_query_poll_policy.hpp"
#include "device_c_query_range_route.hpp"
#include "../dxmt9/dxmt9_query_policy.hpp"
#include "device_c_cpu_ready_transfer.hpp"
#include "util/unixcall_marshal.hpp"

#include "../dxmt9/dxmt9_perf_counters.hpp"
#include "dxmt9/copy_materialization_ledger.hpp"
// Need the full dxmt9::Device type to call markChunkResources on the
// upperDevice shared_ptr that the chunk importer's Phase 4-B path
// hands the per-chunk retention list to.
#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9/dxmt9_cpu_supply_trace.hpp"
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

dxmt9::render::TapeRuntimeCaptureLease acquireRuntimeReplayLeaseV4(D9CDevice* device) noexcept {
  const auto upper = device->dev().upperDevice();
  return upper ? upper->queue().acquireTapeRuntimeCapture()
      : dxmt9::render::TapeRuntimeCaptureLease{};
}

bool runtimeReplayV4Selected(D9CDevice* device) noexcept {
  return dxmt9::d3d9::activeRenderTapeRuntimeReplayV4 ||
      (device && device->renderTapeRuntimeV4Selected.load(std::memory_order_acquire));
}

struct ProviderReplayObservationV4State {
  const dxmt9::d3d9::ProviderReplayObservationV4* input = nullptr;
  dxmt9::d3d9::ProviderReplayObservationV4Result* result = nullptr;
  std::shared_ptr<dxmt9::d3d9::RenderTapeRuntimeIdentityV4> owner{};
  dxmt9::render::TapeRuntimeCaptureLease lease{};
  std::optional<dxmt9::d3d9::RenderTapeRuntimeReplayAccessV4> access{};

  void reject() noexcept {
    if (result) result->qualificationSucceeded = false;
    if (access) access->fail();
  }
  void qualify(bool accepted) noexcept {
    if (!access || !access->visit(
        [&](dxmt9::d3d9::RenderTapeRuntimeIdentityV4& admitted) noexcept {
          if (!accepted) {
            result->qualificationSucceeded = false;
            admitted.fail();
          }
          if (admitted.failed()) result->qualificationSucceeded = false;
          return true;
        })) result->qualificationSucceeded = false;
  }
  template <typename Fn>
  void observe(Fn&& callback) noexcept {
    if (!access || !access->visit(
        [](dxmt9::d3d9::RenderTapeRuntimeIdentityV4&) noexcept { return true; })) {
      result->qualificationSucceeded = false;
      return;
    }
    // Arbitrary provider/executor code may cancel this capture or snapshot
    // its root. It borrows outside admission and all root/binding locks; only
    // the fresh qualification visit below can mutate the matching live root.
    qualify(callback());
  }
  void started(std::uint64_t raw, std::uint32_t count) noexcept {
    if (!access || !access->visit(
        [&](dxmt9::d3d9::RenderTapeRuntimeIdentityV4& admitted) noexcept {
          if (!admitted.matches({input->captureToken, input->captureEpoch}) ||
              admitted.frozen()) return false;
          result->captureAccepted = true;
          result->qualificationSucceeded = true;
          return true;
        })) {
      result->qualificationSucceeded = false;
      return;
    }
    qualify(input->eventOrdinal && input->rawStarted &&
        input->rawStarted(input->context, input->eventOrdinal, raw, count));
  }
  void beforeRecord(std::uint32_t index, std::uint32_t type) noexcept {
    observe([&]() noexcept {
      return !input->beforeRecord || input->beforeRecord(input->context, index, type);
    });
  }
  void beforeDispatch(std::uint32_t index, std::uint32_t type) noexcept {
    observe([&]() noexcept {
      return !input->beforeDispatch || input->beforeDispatch(input->context, index, type);
    });
  }
  void recordDispatched(std::uint32_t index, std::uint32_t type,
      std::int32_t actualResult) noexcept {
    observe([&]() noexcept {
      return !input->recordDispatched ||
          input->recordDispatched(input->context, index, type, actualResult);
    });
  }
  void settled(std::uint64_t raw, std::uint32_t count, bool succeeded) noexcept {
    if (!result || result->ledgerSettlementObserved) return;
    result->localRawOrdinal = raw;
    result->ledgerSettlementObserved = true;
    if (!succeeded) reject();
    if (result->captureAccepted) observe([&]() noexcept {
      return !input->rawSettled || input->rawSettled(input->context, raw, count, succeeded);
    });
  }
};

thread_local ProviderReplayObservationV4State* gProviderReplayObservationV4 = nullptr;

class ScopedProviderReplayObservationV4 {
public:
  explicit ScopedProviderReplayObservationV4(ProviderReplayObservationV4State* state) noexcept
      : previous_(gProviderReplayObservationV4) {
    gProviderReplayObservationV4 = state;
  }
  ~ScopedProviderReplayObservationV4() { gProviderReplayObservationV4 = previous_; }
private:
  ProviderReplayObservationV4State* previous_ = nullptr;
};

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
extern "C" int32_t dxmt9c_device_set_stream_source_freq(D9CDevice* d,
                                                         uint32_t stream,
                                                         uint32_t freq);
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

  // Publish any open compatibility draw-run island. Every cut goes through
  // here: an observable record cut, the end of the replay range, and every
  // early return. Pure ConstantUpload projections are the one exception and
  // may execute while the island remains open. Idempotent, so an unconditional
  // call is correct.
  //
  // The run borrows `bindingSnapshots_` spans, so it must not outlive this
  // sink's caller -- `DrawBatchScope` in replayResolvedChunk enforces that on
  // every exit path, including the failure returns.
  bool flushDrawBatch() noexcept {
    if (directFinalDraws_ || !device_) return true;
    return device_->dev().flushCompatibilityReplayDrawBatch() !=
           dxmt9::core::CompatibilityDrawBatchFlushStatus::Failed;
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
    // PE legacy records use zero for an omitted frequency. Preserve the
    // current D3D9 value; only an explicit nonzero wire value updates it.
    if (!failed(hr) && value.frequency != 0u) {
      const auto frequencyHr = dxmt9c_device_set_stream_source_freq(
          device_, value.slot, value.frequency);
      if (failed(frequencyHr)) return frequencyHr;
    }
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
    if (!call.payload.userVertexData.empty() ||
        !call.payload.userIndexData.empty()) {
      // A UP draw carries its own vertex/index bytes and replays through the
      // legacy sink, so it can never ride a batched run; publish first so the
      // serial order the app issued is preserved exactly. The record walk
      // already cut here for a UP *record*; this covers any other ingress.
      if (!flushDrawBatch()) {
        return dxmt9::core::D3DERR_DEVICELOST;
      }
      if (!directFinalDraws_) {
        dxmt9::perf::countCompatibilityDrawBatchDecision(
            dxmt9::core::CompatibilityDrawBatchAdmission::Unbatchable,
            dxmt9::core::CompatibilityDrawBatchCut::None);
      }
    }
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
    // Batched replay bypasses drawPrimitive[Run], so preserve its state-derived
    // instance count after applying this record's sparse stream frequencies.
    draw.instanceCount =
        dxmt9::core::drawInstanceCountFromState(device_->dev().state());
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
      // Batching does not move that cut: an accepted draw is accounted here,
      // exactly where an unbatched one would have been submitted.
      if (!directFinalDraws_ && !startIrreversibleEffect()) {
        return dxmt9::core::D3DERR_DEVICELOST;
      }
      if (!directFinalDraws_) {
        // Ordinary lane: fold consecutive identical-state draws into one
        // queue acquisition instead of taking one per draw. The run is cut by
        // the record walk below before anything can mutate device state.
        return device_->dev()
            .submitCompatibilityReplayDrawBatched(draw, payload)
            .result;
      }
      const auto result =
          device_->dev().submitDirectReplayDrawFromCurrentState(
              draw, payload, directRangeAppender_);
      if (result.disposition !=
              dxmt9::core::DirectReplayDrawDisposition::Appended &&
          !startIrreversibleEffect()) {
        return dxmt9::core::D3DERR_DEVICELOST;
      }
      return result.result;
    }
    if (!flushDrawBatch()) {
      return dxmt9::core::D3DERR_DEVICELOST;
    }
    if (!directFinalDraws_) {
      // The record walker only sends a non-batchable draw here for
      // TriangleFan (or a state-block recording). Both use the legacy
      // one-draw submission path and must be visible in the residual counter.
      dxmt9::perf::countCompatibilityDrawBatchDecision(
          dxmt9::core::CompatibilityDrawBatchAdmission::Unbatchable,
          dxmt9::core::CompatibilityDrawBatchCut::None);
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

bool rawContainsOrderedControl(
    const dxmt9::d3d9::RawCommandChunk& raw) noexcept {
  dxmt9::d3d9::ImportedChunkView imported;
  if (!importRawChunk(raw, imported)) {
    // This raw was already replayed from the same immutable view. Treat an
    // unexpected second import failure as an ordered boundary so the
    // experiment fails closed without publishing a prefix.
    return true;
  }
  return std::any_of(
      imported.records.begin(), imported.records.end(),
      [](const D9CCommandChunkWireRecordHeader& record) {
        switch (record.type) {
        case D9C_COMMAND_RECORD_QUERY_ISSUE:
        case D9C_COMMAND_RECORD_READBACK:
        case D9C_COMMAND_RECORD_UPDATE_TEXTURE:
          return true;
        default:
          return false;
        }
      });
}

void maybePublishEarlyCpuReadyPrefix(
    D9CDevice* device, const dxmt9::d3d9::RawCommandChunk& raw) noexcept {
  if (!device || raw.hasPresent || !device->dev().upperDevice()) {
    return;
  }
  auto& queue = device->dev().upperDevice()->queue();
  // The stable path pays one cached Boolean only. In particular it does not
  // reconstruct ImportedChunkView or scan records when the experiment is off.
  if (!queue.cpuReadyEarlyPrefixEnabled()) {
    return;
  }
  (void)queue.publishEarlyCpuReadyPrefix(rawContainsOrderedControl(raw));
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
  auto traceScope = dxmt9::tracing::enabled()
      ? dxmt9::tracing::TraceScope(
            static_cast<uint8_t>(dxmt9::tracing::Category::Replay),
            static_cast<uint16_t>(dxmt9::tracing::EventId::ReplayBegin),
            static_cast<uint16_t>(dxmt9::tracing::EventId::ReplayEnd),
            0, raw.replaySeq, dxmt9::tracing::owner_id(device),
            device ? dxmt9::tracing::owner_id(&device->replayDrainLedger)
                   : 0u)
      : dxmt9::tracing::TraceScope{};
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
  const bool v4Selected = runtimeReplayV4Selected(device);
  auto* runtimeIdentity = !v4Selected && raw.renderTapeCaptureToken &&
          raw.renderTapeEventOrdinal && device->renderTapeRuntimeIdentityV3
      ? device->renderTapeRuntimeIdentityV3.get() : nullptr;
  // The raw boundary retains this access facade. Cache its borrowed TLS pointer
  // for the record walk; no per-record shared ownership or route selection.
  auto* runtimeIdentityV4 = dxmt9::d3d9::activeRenderTapeRuntimeReplayV4;
  if (runtimeIdentity && (activeDirectChunkSlotPath || !range.wholeRaw())) {
    // Unsupported provenance is diagnosed before this raw's first effect.
    // The rendering path keeps its selected production route.
    runtimeIdentity->fail();
    runtimeIdentity = nullptr;
  }
  const bool providerRecordConsumer =
      dxmt9::d3d9::providerReplayRecordConsumerEnabled();
  auto* const providerObservationV4 = gProviderReplayObservationV4;
  auto& replayScratch = replayScratchArena();
  ScopedReplayScratchUse replayScratchUse(replayScratch);
  replayScratch.bindingSnapshots.reserve(raw.recordCount);
  const bool legacyCaptureIdentityRequested =
      dxmt9::d3d9::legacyRenderTapeCaptureRouteRequested(
          raw.renderTapeCaptureToken != 0u, v4Selected);
  bool captureIdentity = arenaLease &&
      (dxmt9::tracing::enabled() || legacyCaptureIdentityRequested);
  if (captureIdentity &&
      !arenaLease->beginCaptureIdentity(raw.recordCount)) {
    if (legacyCaptureIdentityRequested)
      return commitChunkFail("chunk-capture-identity-begin");
    captureIdentity = false;
  }
  // A private destination keeps replay rollbackable. The ordinary serial
  // path uses the same borrowed-state ingress but publishes directly into
  // final queue storage instead of materializing an intermediate carrier.
  const bool directFinalDraws = arenaLease != nullptr ||
      activeDirectChunkSlotPath;
  // A previous ordinary replay may have failed while publishing its pending
  // island. That failure was consumed by the replay boundary which returned
  // an error; start the next independent boundary with a clean sticky bit.
  device->dev().clearCompatibilityReplayDrawBatchFailure();
  DeviceReplaySink sink(
      device, pacedByPresentOrdinal, &replayScratch.bindingSnapshots,
      raw.bufferSnapshots,
      raw.bufferSnapshotsCaptured, directFinalDraws, transaction,
      directRangeAppender);
  // A pending compatibility draw-run island holds DrawParamPayloadView spans
  // into `replayScratch`, which dies with this call, and its canonical state
  // borrows the device's binding-agnostic cache. Observable records cut the
  // run before dispatch, but the failure returns below do not, so bind final
  // publication to the scope rather than to each return.
  bool drawBatchFlushFailed = false;
  struct DrawBatchScope {
    DeviceReplaySink* sink;
    bool* failed;
    ~DrawBatchScope() noexcept {
      // Publishing from a destructor must not throw. The sticky device bit is
      // checked by the enclosing replay boundary after this scope unwinds;
      // retaining the local flag documents that this result is intentional,
      // rather than a discarded status from a best-effort destructor.
      if (!sink->flushDrawBatch()) {
        *failed = true;
      }
    }
  } drawBatchScope{&sink, &drawBatchFlushFailed};
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
    if (providerObservationV4)
      providerObservationV4->beforeRecord(static_cast<std::uint32_t>(index),
          record.wire.header.type);
    if (runtimeIdentityV4 && dxmt9::render::activeTapeEmissionObserver)
      dxmt9::render::activeTapeEmissionObserver->recordIndex(static_cast<std::uint32_t>(index));
    if (providerRecordConsumer && !dxmt9::d3d9::observeProviderReplayRecordBefore(
            static_cast<std::uint32_t>(index), record.wire.header.type)) {
      return commitChunkFail("provider-record-observer-before",
          static_cast<std::uint32_t>(index), record.wire.header.type);
    }
    if (runtimeIdentity && !runtimeIdentity->beginRecord(
            static_cast<std::uint32_t>(index), record.wire.header.type))
      runtimeIdentity = nullptr;
    if (runtimeIdentityV4 && !runtimeIdentityV4->beginRecord(
            static_cast<std::uint32_t>(index), record.wire.header.type))
      runtimeIdentityV4 = nullptr;
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
    const auto replayInfo = replayInfoForCommandRecordType(
        record.wire.header.type);
    const bool batchableDraw = recordCanBatchDraw(device, record.wire);
    const auto recordClass =
        dxmt9::core::classifyCompatibilityDrawBatchRecord({
        .batchableDraw = batchableDraw,
        .mutatesDeviceState = replayInfo.mutatesDeviceState,
        .referencesResources = replayInfo.referencesResources,
        .barrier = replayInfo.barrier,
        .stateBlockRecording = device->stateBlockRecording,
    });
    sink.setBatchCurrentDraw(batchableDraw);
    if (dxmt9::core::compatibilityDrawBatchRecordCutsIsland(recordClass)) {
      // Only a pure ConstantUpload projection may remain inside an open
      // compatibility island. StateApply, coordinators, ordered controls,
      // resource/surface operations, UP/TriangleFan and unknown records are
      // observable or resource-bearing and cut before dispatch.
      if (!sink.flushDrawBatch()) {
        return commitChunkFail("chunk-replay-batch-flush",
                               static_cast<std::uint32_t>(index),
                               record.wire.header.type,
                               dxmt9::core::D3DERR_DEVICELOST);
      }
    }
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
      bool gpuOcclusion = false;
      if (dxmt9::query::scopeRangesEnabled() && upper &&
          orderedControl->kind == dxmt9::d3d9::OrderedControlKind::Query) {
        auto* query = static_cast<D9CQuery*>(record.objectForAbsoluteIndex(
            orderedControl->primaryHandleIndex));
        gpuOcclusion = query && query->device == device && query->obj &&
            query->obj->type() == dxmt9::core::QueryType::Occlusion &&
            upper->usesMetalOcclusion();
      }
      const auto writer = dxmt9::d3d9::orderedControlWriter(
          *orderedControl, dxmt9::query::scopeRangesEnabled(), gpuOcclusion);
      const auto reason = orderedControl->kind ==
                                  dxmt9::d3d9::OrderedControlKind::UpdateTexture
                              ? dxmt9::core::metalqueue::
                                    SessionReleaseReason::IndependentSubmission
                              : dxmt9::core::metalqueue::
                                    SessionReleaseReason::DirectObservation;
      if (!orderedControl->valid() || !queue ||
          !queue->releaseCpuReadySessionBeforeOrderedControl(
              reason, orderedControl->requiredReleaseAction,
              orderedControl->rawOrdinal, writer)) {
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
    const bool stateOnly =
        replayInfo.category == ImportedRecordReplayCategory::ConstantUpload ||
        replayInfo.category == ImportedRecordReplayCategory::StateApply;
    if (!stateOnly && !batchableDraw && !dispatchingOrderedControl &&
        !sink.startIrreversibleEffect()) {
      return commitChunkFail("chunk-replay-effect-cut",
                             static_cast<std::uint32_t>(index),
                             record.wire.header.type);
    }
    if (runtimeIdentityV4 && !arenaLease && !activeDirectChunkSlotPath) {
      switch (record.wire.header.type) {
      case D9C_COMMAND_RECORD_QUERY_ISSUE:
      case D9C_COMMAND_RECORD_READBACK:
      case D9C_COMMAND_RECORD_UPDATE_TEXTURE: {
        dxmt9::render::TapeOrdinaryCoordinatorAdmission admission{};
        const bool located = runtimeIdentityV4->visit(
            [&](dxmt9::d3d9::RenderTapeRuntimeIdentityV4& root) noexcept {
              const auto state = root.diagnosticState();
              admission = {.queue = state.queue,
                  .captureToken = state.captureToken, .captureEpoch = state.captureEpoch,
                  .eventOrdinal = state.eventOrdinal, .rawOrdinal = state.rawOrdinal,
                  .attemptGeneration = state.attemptGeneration,
                  .recordIndex = state.recordIndex, .recordType = state.recordType};
              return true;
            });
        const auto upper = located ? device->dev().upperDevice() : nullptr;
        if (upper)
          upper->queue().observeTapeRuntimeOrdinaryCoordinatorAdmission(
              runtimeIdentityV4->captureLease(), admission);
        break;
      }
      default:
        break;
      }
    }
    // Required release/batch flush has already happened. Qualify its actual
    // preceding-record cut before the runtime cursor advances to dispatch.
    if (providerObservationV4)
      providerObservationV4->beforeDispatch(static_cast<std::uint32_t>(index),
          record.wire.header.type);
    if (runtimeIdentity && !runtimeIdentity->beginDispatch()) runtimeIdentity = nullptr;
    if (runtimeIdentityV4 && !runtimeIdentityV4->beginDispatch()) runtimeIdentityV4 = nullptr;
    if (providerRecordConsumer && !dxmt9::d3d9::observeProviderReplayRecordDispatch(
            static_cast<std::uint32_t>(index), record.wire.header.type)) {
      return commitChunkFail("provider-record-observer-dispatch",
          static_cast<std::uint32_t>(index), record.wire.header.type);
    }
    const auto hr = dxmt9::d3d9::isSparseRecord(record.wire.header.type)
                        ? dxmt9::d3d9::replaySparseRecord(record, sink)
                        : dxmt9::d3d9::replayNonDrawRecord(record, sink);
    if (dispatchingOrderedControl) {
      observeOrderedControlReplay(/*AfterDispatch=*/4u, index,
                                  record.wire.header.type, hr);
    }
    if (failed(hr)) {
      if (providerObservationV4)
        providerObservationV4->recordDispatched(static_cast<std::uint32_t>(index),
            record.wire.header.type, hr);
      if (runtimeIdentity) runtimeIdentity->fail();
      // V4 leaves a reversible failed record provisional. Only the actual
      // transaction rollback + destination restoration, or FailedStop,
      // decides capture; diagnostics never change this renderer result.
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
    if (runtimeIdentity && !runtimeIdentity->endRecord(hr)) runtimeIdentity = nullptr;
    if (runtimeIdentityV4 && !runtimeIdentityV4->endRecord(hr)) runtimeIdentityV4 = nullptr;
    if (providerObservationV4)
      providerObservationV4->recordDispatched(static_cast<std::uint32_t>(index),
          record.wire.header.type, hr);
    if (providerRecordConsumer && !dxmt9::d3d9::observeProviderReplayRecordAfter(
            static_cast<std::uint32_t>(index), record.wire.header.type, hr)) {
      return commitChunkFail("provider-record-observer-after",
          static_cast<std::uint32_t>(index), record.wire.header.type);
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
  if (!sink.flushDrawBatch()) {
    return commitChunkFail("chunk-replay-batch-flush-end",
                           static_cast<std::uint32_t>(rangeEnd), 0u,
                           dxmt9::core::D3DERR_DEVICELOST);
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
  // prepareOffloadChunk already reserves this for the normal commit path.
  // Provider-owned inline raws reach this function directly, so reserve here
  // as well: target attachment must not grow after wrapper retention without
  // an owned capacity bound.
  raw.ledgerTargets.reserve(imported.handles.size());
  const auto appendLedgerTarget = [&](dxmt9::d3d9::ReplayDrainTarget* target) {
    if (!target) {
      return;
    }
    bool duplicate;
    if (!scratch.ledgerTargetPresence.overflowed()) {
      duplicate = scratch.ledgerTargetPresence.contains(target);
    } else {
      // Keep the complete vector as the overflow fallback for unusually large
      // handle tables; the normal path remains a bounded presence probe.
      duplicate = std::find(raw.ledgerTargets.begin(), raw.ledgerTargets.end(),
                            target) != raw.ledgerTargets.end();
    }
    if (duplicate) {
      return;
    }
    raw.ledgerTargets.push_back(target);
    if (!scratch.ledgerTargetPresence.overflowed()) {
      scratch.ledgerTargetPresence.insert(target);
    }
  };
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
        appendLedgerTarget(value->replayDrainTarget.get());
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

  // Query handles are also present in the canonical handle table, but that
  // table is shared by records which do not issue a query.  Decode each
  // QUERY_ISSUE payload so BEGIN and END both advance the same target and a
  // receipt for an earlier issue is superseded by a later one.
  {
    for (const auto& recordHeader : imported.records) {
      if (recordHeader.type != D9C_COMMAND_RECORD_QUERY_ISSUE) {
        continue;
      }
      const auto record = imported.record(
          static_cast<std::size_t>(&recordHeader - imported.records.data()));
      if (record.payload.size() < sizeof(D9CCommandChunkWireQueryIssue)) {
        return false;
      }
      D9CCommandChunkWireQueryIssue issue{};
      std::memcpy(&issue, record.payload.data(), sizeof(issue));
      if (issue.queryHandleIndex >= raw.resolvedObjects.size() ||
          issue.queryHandleIndex >= imported.handles.size() ||
          imported.handles[issue.queryHandleIndex].kind !=
              D9C_CHUNK_HANDLE_KIND_QUERY) {
        return false;
      }
      auto* query = static_cast<D9CQuery*>(
          raw.resolvedObjects[issue.queryHandleIndex]);
      if (!query) {
        return false;
      }
      appendLedgerTarget(query->replayDrainTarget.get());
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
    // Ingress owns identity/backing lifetime, not an unreserved GPU ticket.
    // Replay's direct publication or compatibility boundary supplies marks.
    const auto captureResult = upper->captureChunkBufferBindings(
        raw.resourceEntries, raw.bufferSnapshots);
    corePhase.stop(dxmt9::perf::countCommitChunkPhaseMarkCoreCpuTime);
    raw.resourcesMarkedBeforeReplay = false;
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
  if (device && !runtimeReplayV4Selected(device) &&
      raw.renderTapeCaptureToken && raw.renderTapeEventOrdinal &&
      device->renderTapeRuntimeIdentityV3) {
    if (!transaction.state().identity.valid() || firstRecordOrdinal != 0u ||
        !device->renderTapeRuntimeIdentityV3->beginReplayAttempt(
            raw.renderTapeCaptureToken, raw.renderTapeEventOrdinal,
            raw.replaySeq, raw.recordCount))
      device->renderTapeRuntimeIdentityV3->fail();
  }
  if (dxmt9::d3d9::providerReplayRecordConsumerEnabled() &&
      !dxmt9::d3d9::observeProviderReplayTransactionStarted(transaction.state()))
    (void)transaction.failStop();
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

bool rollbackCompatibilityReplayTransaction(D9CDevice* device,
    dxmt9::d3d9::ReplayTransaction& transaction) noexcept {
  const bool restored = rollbackReplayTransaction(device, transaction);
  if (restored) {
    // Compatibility owns no private destination to restore. Ordinary visible
    // emission already crossed the reducer's effect cut, so the successful
    // pre-effect semantic restoration is also this destination-free boundary.
    // Direct/Arena callers must instead confirm their actual lease rollback.
    if (auto* observer = dxmt9::render::activeTapeEmissionObserver)
      observer->discardTentative();
    dxmt9::d3d9::noteDestinationRollback(transaction, true);
  }
  return restored;
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

void traceReplayPublication(D9CDevice* device, std::uint64_t rawReplayId,
                            std::uint64_t queueSequence,
                            std::uint64_t queueIdentity) noexcept {
  if (!dxmt9::tracing::enabled() || !device || rawReplayId == 0u ||
      queueSequence == 0u || queueIdentity == 0u) {
    return;
  }
  // This typed ticket is the only evidence for raw-to-queue identity. Equal
  // numeric values in either sequence domain are not a relation by themselves.
  dxmt9::tracing::observe(
      dxmt9::tracing::Category::Replay,
      dxmt9::tracing::EventId::ReplayPublish,
      dxmt9::tracing::owner_id(device), rawReplayId, queueSequence,
      dxmt9::tracing::Phase::Instant, queueIdentity);
}

void traceReplayPublication(
    D9CDevice* device, std::uint64_t rawReplayId,
    const dxmt9::core::CpuReadyPublicationTicket& ticket,
    std::uint64_t queueIdentity) noexcept {
  if (!ticket.strictIdentityValid() || ticket.rawOrdinal != rawReplayId) {
    return;
  }
  traceReplayPublication(device, rawReplayId, ticket.seqId, queueIdentity);
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
  if (dxmt9::d3d9::providerReplayRecordConsumerEnabled() &&
      !dxmt9::d3d9::observeProviderReplayTransactionCommitted(transaction.state()))
    return false;
  return true;
}

// A replay sink owns the final batch flush in an RAII scope so every early
// return retires borrowed spans.  Destructors cannot change an already
// evaluated return value, therefore the enclosing replay boundary must turn
// the sink's sticky publication failure into a hard replay failure before it
// commits the transaction.
int32_t checkReplayBatchFailure(D9CDevice* device, int32_t hr) noexcept {
  if (failed(hr) || !device ||
      !device->dev().compatibilityReplayDrawBatchFailed()) {
    return hr;
  }
  return commitChunkFail("chunk-replay-batch-flush-scope",
                        0xffffffffu, 0u, dxmt9::core::D3DERR_DEVICELOST);
}

int32_t replayCompatibilityChunk(
    D9CDevice* device, dxmt9::d3d9::RawCommandChunk& raw,
    bool pacedByPresentOrdinal, bool containsOrderedControls = false,
    ReplayRecordRange range = {}) {
  auto& transaction = beginReplayTransaction(
      device, raw, 1u, static_cast<std::uint32_t>(range.firstRecordIndex));
  const auto hr = checkReplayBatchFailure(device, replayResolvedChunk(
      device, raw, pacedByPresentOrdinal, transaction, nullptr, {},
      containsOrderedControls, {}, false, nullptr, range));
  if (failed(hr)) {
    (void)rollbackCompatibilityReplayTransaction(device, transaction);
    return hr;
  }
  if (!commitReplayTransaction(transaction,
                               compatibilityReplayReceipt(transaction))) {
    return commitChunkFail("chunk-replay-transaction-commit");
  }
  if (!runtimeReplayV4Selected(device) &&
      raw.renderTapeCaptureToken && raw.renderTapeEventOrdinal &&
      device->renderTapeRuntimeIdentityV3)
    (void)device->renderTapeRuntimeIdentityV3->transactionCommitted(transaction.state());
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
    dxmt9::perf::DirectChunkSlotReplayOutcome& outcome,
    ReplayRecordRange fallbackRange = {},
    dxmt9::CommandQueue::ReplaySourceBoundaryExpectation*
        terminalWriter = nullptr) {
  if (terminalWriter) *terminalWriter = {};
  outcome = dxmt9::perf::DirectChunkSlotReplayOutcome::NotAttempted;
  // Whole-raw ordered-control preflight, once, before any span executes.
  // Every span call below then skips its own preflight: a span may
  // legitimately contain no control, and repeating the scan per span would
  // let a late malformed control fail after an earlier span's effects.
  if (plan.orderedControlCount != 0 &&
      !fallbackRange.orderedControlPreflightDone) {
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
      const auto hr = checkReplayBatchFailure(device, replayResolvedChunk(
          device, raw, pacedByPresentOrdinal, transaction, nullptr, {},
          spanHasOrderedControl, {}, /*activeDirectChunkSlotPath=*/false,
          nullptr, range));
      if (failed(hr)) {
        outcome = dxmt9::perf::DirectChunkSlotReplayOutcome::ReplayFailed;
        if (separatorEffectsStarted) {
          (void)transaction.failStop();
          return failStopAfterEffects("chunk-span-ordinary",
                                      span.firstRecordIndex);
        }
        (void)rollbackCompatibilityReplayTransaction(device, transaction);
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

    auto controlMode = dxmt9::queue::DirectSourceControlMode::Ordinary;
    if (span.firstSegmentIndex != 0u &&
        span.firstSegmentIndex <= plan.segments.size()) {
      const auto precedingKind =
          plan.segments[span.firstSegmentIndex - 1u].kind;
      controlMode = precedingKind ==
              dxmt9::d3d9::EmissionSegmentKind::OrderedControlLocator
          ? dxmt9::queue::DirectSourceControlMode::OrderedControl
          : dxmt9::queue::DirectSourceControlMode::Separator;
    }
    auto begin = queue.beginDirectChunkSlotReplay(
        raw.replaySeq, span.capacity, span.plannedBytes, producerIdentity,
        span.recordCount, span.drawCount, span.leaseOrdinal,
        span.finalLeaseSpan, /*allowRotation=*/false, controlMode);
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
      return replayCompatibilityChunk(device, raw, pacedByPresentOrdinal,
                                      !fallbackRange.wholeRaw() &&
                                          plan.orderedControlCount != 0u,
                                      fallbackRange);
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
        dxmt9::d3d9::noteExternalEffect(transaction,
            dxmt9::d3d9::ReplayTransactionEffectWitness::DirectLease);
        (void)transaction.failStop();
        return hr;
      }
      const bool stateRolledBack =
          rollbackReplayTransaction(device, transaction);
      const bool destinationRolledBack = lease.rollbackPreEffect();
      if (stateRolledBack && destinationRolledBack &&
          dxmt9::render::activeTapeEmissionObserver)
        dxmt9::render::activeTapeEmissionObserver->discardTentative();
      if (stateRolledBack)
        dxmt9::d3d9::noteDestinationRollback(transaction, destinationRolledBack);
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
      return replayCompatibilityChunk(device, raw, pacedByPresentOrdinal,
                                      !fallbackRange.wholeRaw() &&
                                          plan.orderedControlCount != 0u,
                                      fallbackRange);
    }
    // Explicit cut at the span end. Coordinator appends already seal the open
    // run implicitly, but a span boundary is a cut the assembler never sees,
    // and `Draw, Draw, <cut>, Draw` must produce two run records, not one.
    lease.markSemanticEffectsStarted();
    dxmt9::d3d9::noteExternalEffect(transaction,
        dxmt9::d3d9::ReplayTransactionEffectWitness::DirectLease);
    lease.closeDirectRun();
    const auto destinationTicket = lease.ticket();
    const auto destinationControlIndex = lease.controlIndex();
    const auto destinationReceipt = publishedReplayReceipt(
        transaction, destinationTicket, destinationControlIndex,
        dxmt9::d3d9::ReplayDestinationKind::DirectChunkSlot);
    if (!transaction.receiveDestination(destinationReceipt)) {
      outcome = dxmt9::perf::DirectChunkSlotReplayOutcome::CommitFailed;
      (void)transaction.failStop();
      return commitChunkFail("chunk-span-state-commit", span.firstRecordIndex);
    }
    // Accept the replay receipt while the destination is still Writing. The
    // queue mutex serializes this handoff with the subsequent sealAndPublish;
    // its queue-owned Publish edge is therefore never overtaken by encode.
    lease.observeAcceptedDestination();
    if (lease.commit(raw.resourceEntries) !=
        dxmt9::CommandQueue::DirectChunkSlotReplayStatus::Committed) {
      outcome = dxmt9::perf::DirectChunkSlotReplayOutcome::CommitFailed;
      (void)transaction.failStop();
      return commitChunkFail("chunk-span-commit", span.firstRecordIndex);
    }
    traceReplayPublication(device, raw.replaySeq, destinationTicket,
                           queue.queryQueueIdentity());
    if (!transaction.commit()) {
      outcome = dxmt9::perf::DirectChunkSlotReplayOutcome::CommitFailed;
      return commitChunkFail("chunk-span-state-commit", span.firstRecordIndex);
    }
    if (terminalWriter && !fallbackRange.wholeRaw() &&
        span.presentCount == 0u &&
        span.firstRecordIndex + span.recordCount ==
            fallbackRange.firstRecordIndex + fallbackRange.recordCount) {
      *terminalWriter = {
          .source = {.id = destinationTicket.id,
                     .storage = destinationTicket.storage},
          .seqId = destinationTicket.seqId,
      };
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
  if (fallbackRange.countAdmit) dxmt9::perf::countChunkAdmit();
  return dxmt9::core::D3D_OK;
}

int32_t replayPlannedChunk(D9CDevice* device,
                             dxmt9::d3d9::RawCommandChunk& raw,
                             bool pacedByPresentOrdinal,
                             bool allowDirectArena,
                             bool forceEventSerial = false,
                             bool reportOffloadReplayStage = false,
                             bool allowDirectSlot = true) {
  // V2's 64-page grouping is not queue capacity. V4 tags retain original
  // semantic identity but never select this legacy physical-layout policy,
  // including after the retained capture is failed, cancelled or replaced.
  constexpr std::size_t kCaptureSegmentSerialPagesPerSource = 64u;
  const bool v4Selected = runtimeReplayV4Selected(device);
  const bool captureIdentityRequested =
      dxmt9::d3d9::legacyRenderTapeCaptureRouteRequested(
          raw.renderTapeCaptureToken != 0u && raw.renderTapeEventOrdinal != 0u,
          v4Selected);
  const bool segmentSerialRequested =
      device && !forceEventSerial && device->dev().upperDevice() &&
      device->dev().upperDevice()->queue().segmentSerialEnabled();
  const bool captureSegmentSerialRequested =
      captureIdentityRequested && segmentSerialRequested;
  const bool traceBatchIdentityRequested = dxmt9::tracing::enabled();
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
    // V3 observes the selected production route, including coordinator-only
    // Legacy/Inline work. V2's source-per-event rejection is not a V3 fault.
    if (v4Selected || device->renderTapeRuntimeIdentityV3) return;
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
    if (v4Selected && dxmt9::d3d9::activeRenderTapeRuntimeReplayV4)
      dxmt9::d3d9::activeRenderTapeRuntimeReplayV4->fail();
    failCaptureIdentity("token-event-pair");
  }
  if (!raw.cpuReadyTapePlanningEnabled) {
    // The ordinary execution policy is independent of ingress ownership.
    // Ingress leaves marking to the direct publisher or markLegacyResources
    // at the fallback boundary.
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
    if (!captureIdentityRequested && allowDirectSlot && upper && queue &&
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
      if (!plan.spansExecutable() || plan.leaseOwningSpanCount() == 0 ||
          !plan.rotationFreeProductionEligible()) {
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
        //  * not rotation-free -- a second Direct lease, or an ordinary
        //    prefix before the only lease, could encounter capacity rejection
        //    after an earlier span already had effects. Publishing the
        //    populated prefix to make room caused a reproducible GT1 visual
        //    corruption; until rotation owns a complete incoming first-draw
        //    and resource witness, the serial whole-raw lane is authoritative.
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
  if (queue && dxmt9::d3d9::queryRangeCompatibilityShortcut(
          imported, dxmt9::query::scopeRangesEnabled(),
          dxmt9::d3d9::legacyRenderTapeCaptureRouteRequested(
              raw.renderTapeCaptureToken != 0u || raw.renderTapeEventOrdinal != 0u,
              v4Selected))) {
    // Ranges own compatibility storage for this entire process. No Arena
    // capacity plan, admission lock or failed reservation is needed. Withdraw
    // the speculative successor before applying the original serial effects.
    queue->cancelCpuReadyNextSourceIntent(raw.replaySeq);
    if (reportOffloadReplayStage) {
      dxmt9::perf::recordOffloadReplayStage(
          dxmt9::perf::OffloadReplayStage::Encode);
    }
    markLegacyResources(device, raw);
    return replayCompatibilityChunk(device, raw, pacedByPresentOrdinal);
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
                                allowDirectArena, /*forceEventSerial=*/true,
                                /*reportOffloadReplayStage=*/false, allowDirectSlot);
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
                              allowDirectArena, /*forceEventSerial=*/true,
                              /*reportOffloadReplayStage=*/false, allowDirectSlot);
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
    // An unsupported representation is an intentional pre-effect route, not
    // queue corruption. Keep genuine admission failures visible.
    if (begin.status != dxmt9::CommandQueue::CpuReadyArenaBeginStatus::
                            OcclusionScopeUnsupported) {
      logAdmissionFailure(static_cast<std::uint32_t>(begin.status),
                          begin.stopReason);
    }
    if (begin.status == dxmt9::CommandQueue::CpuReadyArenaBeginStatus::
                            CompatibilityTailOwned) {
      // The prior early publication already reserved the only admissible
      // frame tail. This raw has applied no effects, so route it into that
      // exact compatibility owner rather than publishing or waiting for a
      // separate Arena source.
      supplyReplayEntry.cancel();
      markLegacyResources(device, raw);
      return replayCompatibilityChunk(device, raw, pacedByPresentOrdinal);
    }
    if (begin.status == dxmt9::CommandQueue::CpuReadyArenaBeginStatus::
                            OcclusionScopeUnsupported) {
      // Occlusion membership is frozen on compatibility/direct ChunkSlots;
      // Arena admission was rejected before any effects or Tape reservation.
      // Route the complete raw event through the same bounded compatibility
      // owner, never back through TemporaryPressure.
      supplyReplayEntry.cancel();
      markLegacyResources(device, raw);
      return replayCompatibilityChunk(device, raw, pacedByPresentOrdinal);
    }
    if (segmentSerial && begin.status ==
            dxmt9::CommandQueue::CpuReadyArenaBeginStatus::RecoverableFailure) {
      // Admission and builder construction happen before replay invokes any
      // semantic sink.  The recoverable batch abort restored all Tape and
      // queue cursors, so EventSerial v2 may own this raw event exactly once.
      supplyReplayEntry.cancel();
      return replayPlannedChunk(device, raw, pacedByPresentOrdinal,
                                allowDirectArena, /*forceEventSerial=*/true,
                                /*reportOffloadReplayStage=*/false, allowDirectSlot);
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
                                allowDirectArena, /*forceEventSerial=*/true,
                                /*reportOffloadReplayStage=*/false, allowDirectSlot);
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
      const bool destinationRolledBack = transfer.abortForFallback();
      if (destinationRolledBack && dxmt9::render::activeTapeEmissionObserver)
        dxmt9::render::activeTapeEmissionObserver->discardTentative();
      dxmt9::d3d9::noteDestinationRollback(transaction, destinationRolledBack);
      if (!destinationRolledBack) {
        queue->failStopCpuReadyArena();
        return commitChunkFail("chunk-capture-identity-rollback");
      }
      supplyReplayEntry.cancel();
      transfer.restoreToSource();
      return replayPlannedChunk(device, raw, pacedByPresentOrdinal,
                                /*allowDirectArena=*/false,
                                /*forceEventSerial=*/true,
                                /*reportOffloadReplayStage=*/false,
                                allowDirectSlot);
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
  dxmt9::d3d9::noteExternalEffect(transaction,
      dxmt9::d3d9::ReplayTransactionEffectWitness::ArenaEmitted);
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
            (captureIdentityRequested || traceBatchIdentityRequested)
                ? &captureBatch : nullptr)
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
  if (segmentSerial) {
    if (captureBatch.valid()) {
      for (const auto& segment : captureBatch.segments) {
        traceReplayPublication(device, transferRaw.replaySeq, segment.seqId,
                               queue->queryQueueIdentity());
      }
    }
  } else {
    traceReplayPublication(device, transferRaw.replaySeq, admissionTicket,
                           queue->queryQueueIdentity());
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
  if (!v4Selected && transferRaw.renderTapeCaptureToken &&
      transferRaw.renderTapeEventOrdinal &&
      device->renderTapeRuntimeIdentityV3)
    (void)device->renderTapeRuntimeIdentityV3->transactionCommitted(transaction.state());
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
      if (!device->renderTapeIdentityCapture.appendEventLocalPasses(
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
  std::optional<ScopedRenderTapeRuntimeReplayV4> runtimeReplayV4;
  if (chunk.renderTapeCaptureToken || chunk.renderTapeEventOrdinal) {
    if (auto owner = acquireRenderTapeRuntimeIdentityV4(d))
      runtimeReplayV4.emplace(std::move(owner), ReplayTransactionRawIdentity{
          chunk.renderTapeCaptureToken, chunk.renderTapeEventOrdinal, chunk.replaySeq, chunk.recordCount},
          /*requireLedgerAck=*/false, acquireRuntimeReplayLeaseV4(d));
  }
  if (chunk.wireVersion != D9C_COMMAND_CHUNK_VERSION) {
    dxmt9::perf::countCommandChunkReject();
    return commitChunkFail("offload-unsupported-wire-version");
  }
  dxmt9::d3d9::ScopedQueryReplayOrdinal queryOrdinalScope(chunk.replaySeq);
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
  } else {
    maybePublishEarlyCpuReadyPrefix(d, chunk);
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

int32_t replayProviderRecordRanges(
    D9CDevice* device, dxmt9::d3d9::RawCommandChunk& raw,
    std::span<const dxmt9::d3d9::ProviderReplayRecordRange> ranges,
    std::uint32_t& completedRanges,
    std::span<const std::uint32_t> batchCuts,
    std::uint32_t& completedBatchCuts) {
  completedRanges = 0u;
  completedBatchCuts = 0u;
  markLegacyResources(device, raw);
  std::size_t nextBatchCut = 0u;
  for (const auto& sourceRange : ranges) {
    auto& transaction = beginReplayTransaction(
        device, raw, /*compatibilitySourceCount=*/1u,
        sourceRange.firstRecord);
    struct RangeTransactionScope {
      D9CDevice* device;
      dxmt9::d3d9::ReplayTransaction& transaction;
      bool committed = false;
      ~RangeTransactionScope() noexcept {
        // Also close the journal when allocation throws from the sink.
        // rollbackReplayTransaction fail-stops an irreversible prefix.
        if (!committed) (void)rollbackCompatibilityReplayTransaction(device, transaction);
      }
    } transactionScope{device, transaction};
    const auto sourceEnd = sourceRange.firstRecord + sourceRange.recordCount;
    auto cursor = sourceRange.firstRecord;
    while (cursor < sourceEnd) {
      const bool batchCut = nextBatchCut < batchCuts.size() &&
          batchCuts[nextBatchCut] < sourceEnd;
      const auto end = batchCut ? batchCuts[nextBatchCut] : sourceEnd;
      const ReplayRecordRange range{
          .firstRecordIndex = cursor,
          .recordCount = end - cursor,
          .orderedControlPreflightDone = true,
          .countAdmit = false,
      };
      // A subrange uses the existing sink's terminal batch flush. Keep the
      // source journal and queue publication outside this loop: the cut is
      // not authority to submit a source or end a physical render pass.
      const auto hr = checkReplayBatchFailure(
          device, replayResolvedChunk(device, raw, /*pacedByPresentOrdinal=*/false,
                                      transaction, nullptr, {},
                                      /*containsOrderedControls=*/true, {}, false,
                                      nullptr, range));
      if (failed(hr)) return hr;
      cursor = end;
      if (batchCut) {
        ++nextBatchCut;
        ++completedBatchCuts;
      }
    }
    if (!commitReplayTransaction(transaction,
                                 compatibilityReplayReceipt(transaction))) {
      return commitChunkFail("provider-replay-range-commit",
                             sourceRange.firstRecord);
    }
    transactionScope.committed = true;
    auto upper = device ? device->dev().upperDevice() : nullptr;
    if (!upper) {
      return commitChunkFail("provider-replay-range-flush-device",
                             sourceRange.firstRecord);
    }
    // This cold queue operation requests publication of the pending writer
    // slot without waiting for GPU completion. It rejects an active
    // arena/direct build, so a successful return completes the requested
    // source-boundary publication.
    if (!upper->queue().publishReplayDiagnosticBoundary()) {
      return commitChunkFail("provider-replay-range-flush",
                             sourceRange.firstRecord);
    }
    ++completedRanges;
  }
  dxmt9::perf::countChunkAdmit();
  return dxmt9::core::D3D_OK;
}

bool preflightProviderOrderedControls(
    const dxmt9::d3d9::ImportedChunkView& imported,
    std::uint64_t rawOrdinal) noexcept {
  for (std::size_t index = 0; index < imported.records.size(); ++index) {
    const auto record = imported.record(index);
    switch (record.header.type) {
    case D9C_COMMAND_RECORD_QUERY_ISSUE:
    case D9C_COMMAND_RECORD_READBACK:
    case D9C_COMMAND_RECORD_UPDATE_TEXTURE: {
      const auto disposition = dxmt9::d3d9::makeOrderedControlDisposition(
          record, rawOrdinal, index);
      if (!disposition || !disposition->valid()) {
        return false;
      }
      break;
    }
    default:
      break;
    }
  }
  return true;
}

int32_t replayProviderSourcePolicy(
    D9CDevice* device, dxmt9::d3d9::RawCommandChunk& raw,
    const dxmt9::d3d9::ImportedChunkView& imported,
    dxmt9::d3d9::ProviderReplayPolicy policy,
    std::span<const dxmt9::d3d9::ProviderReplayRecordRange> ranges,
    dxmt9::d3d9::ProviderReplaySourceConsumer consumer) {
  using namespace dxmt9::d3d9;
  auto upper = device ? device->dev().upperDevice() : nullptr;
  if (!upper ||
      (policy == ProviderReplayPolicy::ProductionSlot &&
       !upper->supportsDirectChunkSlotReplay())) {
    return commitChunkFail("provider-source-policy-backend");
  }
  auto& queue = upper->queue();
  const auto limits = queue.cpuReadyArenaPlanLimits();
  // No prefix may execute before the entire raw's plans, control contracts
  // and receipt storage have been admitted. Each plan borrows global payload
  // and handle offsets; private raw ownership never splits or gets repacked.
  std::vector<ReplayEmissionPlan> plans;
  plans.reserve(ranges.size());
  std::size_t maximumRangeRecords = 0u;
  for (const auto& range : ranges) {
    auto plan = planReplayEmissionRange(
        imported, raw.replaySeq, range.firstRecord, range.recordCount,
        limits.pageSize == 0u ? 4096u : limits.pageSize);
    if (!plan.partitioned()) {
      return commitChunkFail("provider-source-policy-plan", range.firstRecord);
    }
    maximumRangeRecords = std::max(maximumRangeRecords,
                                  static_cast<std::size_t>(range.recordCount));
    plans.push_back(std::move(plan));
  }
  if (!preflightProviderOrderedControls(imported, raw.replaySeq)) {
    return commitChunkFail("provider-source-policy-ordered-control");
  }
  // One command can publish a previous writer and its own terminal command.
  // This is bounded evidence storage, not permission to exceed queue capacity.
  if (maximumRangeRecords > (std::numeric_limits<std::size_t>::max() - 1u) / 2u) {
    return commitChunkFail("provider-source-policy-receipt-capacity");
  }
  std::vector<dxmt9::core::metalqueue::ReplaySourcePublicationReceipt> receipts(
      maximumRangeRecords * 2u + 1u);
  const bool observability = dxmt9::perf::enabled();
  bool priorRangeSettled = false;
  struct SourceExecutionScope {
    dxmt9::CommandQueue& queue;
    bool complete = false;
    bool enteredEffects = false;
    ~SourceExecutionScope() noexcept {
      if (!complete && enteredEffects) queue.failStopReplaySourceExecution();
    }
  } execution{queue};

  for (std::size_t index = 0u; index < ranges.size(); ++index) {
    const auto& range = ranges[index];
    const auto& plan = plans[index];
    auto capture = queue.beginReplaySourcePublicationCapture(receipts);
    if (!capture) {
      if (priorRangeSettled) execution.enteredEffects = true;
      return commitChunkFail("provider-source-policy-publication-begin",
                             range.firstRecord);
    }
    const ReplayRecordRange fallbackRange{
        .firstRecordIndex = range.firstRecord,
        .recordCount = range.recordCount,
        .orderedControlPreflightDone = true,
        .countAdmit = false,
    };
    // Each new local source needs final-use marks at its own queue sequence.
    // The full raw's retained wrappers and snapshot bindings stay unchanged.
    raw.resourcesMarkedBeforeReplay = false;
    execution.enteredEffects = true;
    dxmt9::CommandQueue::ReplaySourceBoundaryExpectation terminalWriter{};
    auto outcome = dxmt9::perf::DirectChunkSlotReplayOutcome::NotAttempted;
    const bool direct = policy == ProviderReplayPolicy::ProductionSlot &&
        plan.spansExecutable() && plan.leaseOwningSpanCount() != 0u &&
        plan.rotationFreeProductionEligible();
    int32_t hr;
    if (direct) {
      hr = replayEmissionSpans(device, raw, false, imported, plan, queue,
                               observability, outcome, fallbackRange,
                               &terminalWriter);
    } else {
      markLegacyResources(device, raw);
      hr = replayCompatibilityChunk(device, raw, false,
                                     plan.orderedControlCount != 0u,
                                     fallbackRange);
    }
    if (observability && policy == ProviderReplayPolicy::ProductionSlot) {
      dxmt9::perf::recordDirectChunkSlotReplayDisposition(
          static_cast<dxmt9::perf::DirectChunkSlotReplayDisposition>(
              emissionPlanDisposition(plan, false)),
          outcome, range.recordCount, raw.recordBytes);
    }
    if (failed(hr)) return hr;
    const auto boundary = terminalWriter.empty()
        ? queue.publishReplaySourceBoundary()
        : queue.publishReplaySourceBoundary(terminalWriter);
    if (boundary.status ==
        dxmt9::CommandQueue::ReplaySourceBoundaryStatus::Rejected) {
      return commitChunkFail("provider-source-policy-publication-end",
                             range.firstRecord);
    }
    const auto captured = capture->finish();
    if (!captured.complete || captured.count > receipts.size()) {
      return commitChunkFail("provider-source-policy-publication-receipt",
                             range.firstRecord);
    }
    const auto published = std::span<const dxmt9::core::metalqueue::
        ReplaySourcePublicationReceipt>(receipts.data(), captured.count);
    const auto presentCount = static_cast<std::size_t>(std::count_if(
        imported.records.begin() + range.firstRecord,
        imported.records.begin() + range.firstRecord + range.recordCount,
        [](const auto& record) {
          return record.type == D9C_COMMAND_RECORD_PRESENT;
        }));
    const auto publishedPresents = static_cast<std::size_t>(std::count_if(
        published.begin(), published.end(),
        [](const auto& receipt) { return receipt.hasPresent; }));
    if (publishedPresents != presentCount ||
        (boundary.published() &&
         (published.empty() || published.back().source != boundary.source ||
          published.back().metadata.seqId != boundary.metadata.seqId))) {
      return commitChunkFail("provider-source-policy-settlement-mismatch",
                             range.firstRecord);
    }
    const ProviderReplaySourceSettlement settlement{
        .range = range,
        .rawOrdinal = raw.replaySeq,
        .rangeIndex = static_cast<std::uint32_t>(index),
        .kind = presentCount != 0u
            ? ProviderReplaySourceSettlementKind::PresentPublished
            : (published.empty() ? ProviderReplaySourceSettlementKind::Empty
                                 : ProviderReplaySourceSettlementKind::Published),
        .publications = published,
    };
    // The synchronous callback may drain a no-worker CPU queue, but cannot
    // outlive the borrowed receipt span or mutate/release this raw owner.
    if (consumer.settled && !consumer.settled(consumer.context, settlement)) {
      return commitChunkFail("provider-source-policy-consumer", range.firstRecord);
    }
    priorRangeSettled = true;
  }
  execution.complete = true;
  dxmt9::perf::countChunkAdmit();
  return dxmt9::core::D3D_OK;
}

thread_local const dxmt9::d3d9::ProviderReplayRecordConsumer*
    gProviderReplayRecordConsumer = nullptr;

class ScopedProviderReplayRecordConsumer {
public:
  explicit ScopedProviderReplayRecordConsumer(
      const dxmt9::d3d9::ProviderReplayRecordConsumer* consumer) noexcept
      : previous_(gProviderReplayRecordConsumer) {
    gProviderReplayRecordConsumer = consumer;
  }
  ~ScopedProviderReplayRecordConsumer() {
    gProviderReplayRecordConsumer = previous_;
  }
private:
  const dxmt9::d3d9::ProviderReplayRecordConsumer* previous_;
};

bool observeProviderSlotValidation(const dxmt9::core::ChunkSlot& slot) noexcept {
  using namespace dxmt9;
  diagnostic::ScopedSlotValidationProbe disable(nullptr);
  const auto attempt = supply::nextWaitAttempt();
  // Stored parameter extent is not the number visited (runs may overlap).
  supply::record(supply::Event::SlotValidationExtent, &slot, attempt, slot.drawParams.size());
  supply::record(supply::Event::SlotValidationBegin, &slot, attempt, slot.commandHeaders.size());
  const bool valid = slot.commandPayloadsInRange();
  supply::record(supply::Event::SlotValidationEnd, &slot, attempt, valid);
  return valid;
}

int32_t runProviderReplay(
    D9CDevice* device, std::span<const std::byte> bytes,
    const dxmt9::d3d9::CommandChunkEnvelope& envelope,
    std::span<void* const> resolvedObjects,
    const dxmt9::d3d9::ImportedChunkView& imported,
    dxmt9::d3d9::ProviderReplayPolicy policy,
    std::span<const dxmt9::d3d9::ProviderReplayRecordRange> ranges,
    bool sourceCutDriver, std::uint32_t* completedRanges,
    std::span<const std::uint32_t> batchCuts,
    std::uint32_t* completedBatchCuts,
    const dxmt9::d3d9::ProviderReplaySourceConsumer* sourceConsumer = nullptr,
    const dxmt9::d3d9::ProviderReplayRecordConsumer* recordConsumer = nullptr,
    const dxmt9::d3d9::ProviderReplayObservationV4* observationV4 = nullptr,
    dxmt9::d3d9::ProviderReplayObservationV4Result* observationResultV4 = nullptr) noexcept {
  // Cold observed-provider boundary only. This is not a per-record clock or
  // production offload scope. Rejected/throwing calls deliberately lack the
  // complete phase set and cannot be priced as successful replay.
  const bool traceCpu = observationV4 && dxmt9::supply::enabled();
  const auto cpuPhase = [&](std::uint64_t phase) noexcept {
    if (traceCpu) dxmt9::supply::record(dxmt9::supply::Event::ProviderReplayPhase,
        observationV4->context, observationV4->eventOrdinal, phase);
  };
  cpuPhase(0u);
  const bool v4Selected = observationV4 || runtimeReplayV4Selected(device);
  dxmt9::d3d9::RawCommandChunk raw;
  ScopedRetainedWrapperRelease retainedRelease(raw);
  bool ledgerPublished = false;
  ProviderReplayObservationV4State observationStateV4{
      observationV4, observationResultV4, {}};
  if (completedRanges) {
    *completedRanges = 0u;
  }
  if (completedBatchCuts) *completedBatchCuts = 0u;
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
    // The provider ledger is the sole authority for this raw's replay
    // identity. In particular, source-cut replay must not manufacture a
    // capture/global sequence before publishInline assigns the provider-local
    // ordinal used by ordered-control dispositions and transactions.
    raw.replaySeq = 0u;
    raw.producerIdentity = envelope.producerIdentity;
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

    if (!persistResolvedResourcesAndCaptureBindings(device, raw, imported)) {
      return commitChunkFail("provider-replay-resource-capture");
    }
    if (!dxmt9::d3d9::drainDeferredReplay(device, "provider-render-tape")) {
      return commitChunkFail("provider-replay-drain");
    }
    if (!device->replayDrainLedger.publishInline(raw)) {
      return commitChunkFail("provider-replay-ledger");
    }
    ledgerPublished = true;
    dxmt9::d3d9::ScopedQueryReplayOrdinal queryOrdinalScope(raw.replaySeq);
    std::optional<dxmt9::d3d9::ScopedRenderTapeRuntimeReplayV4> runtimeReplayV4;
    if (observationV4 && observationResultV4) {
      observationResultV4->localRawOrdinal = raw.replaySeq;
      // Separate local capture credentials cannot influence the legacy raw's
      // capture-specific planner/Direct/query route gates; those remain zero.
      {
        std::lock_guard lock(device->renderTapeRuntimeIdentityV4Mutex);
        const auto owner = dxmt9::d3d9::acquireRenderTapeRuntimeIdentityV4(device);
        if (owner && device->renderTapeRuntimeBindingV4 && observationV4->queue &&
            device->renderTapeRuntimeBindingV4->queue == observationV4->queue &&
            owner->matches({observationV4->captureToken, observationV4->captureEpoch})) {
          observationStateV4.owner = owner;
        }
      }
      if (observationStateV4.owner) {
        observationStateV4.lease = acquireRuntimeReplayLeaseV4(device);
        observationStateV4.access.emplace(observationStateV4.owner.get(),
            dxmt9::d3d9::ReplayTransactionRawIdentity{observationV4->captureToken,
                observationV4->eventOrdinal, raw.replaySeq, raw.recordCount},
            observationStateV4.lease);
        observationStateV4.started(raw.replaySeq, raw.recordCount);
        if (observationResultV4->qualificationSucceeded)
          runtimeReplayV4.emplace(observationStateV4.owner,
              dxmt9::d3d9::ReplayTransactionRawIdentity{observationV4->captureToken,
                  observationV4->eventOrdinal, raw.replaySeq, raw.recordCount},
              /*requireLedgerAck=*/true, observationStateV4.lease);
      }
    } else if (raw.renderTapeCaptureToken || raw.renderTapeEventOrdinal) {
      if (auto owner = dxmt9::d3d9::acquireRenderTapeRuntimeIdentityV4(device))
        runtimeReplayV4.emplace(std::move(owner), dxmt9::d3d9::ReplayTransactionRawIdentity{
            raw.renderTapeCaptureToken, raw.renderTapeEventOrdinal, raw.replaySeq, raw.recordCount},
            /*requireLedgerAck=*/true, acquireRuntimeReplayLeaseV4(device));
    }

    if (recordConsumer && recordConsumer->eventStarted &&
        !recordConsumer->eventStarted(recordConsumer->context, raw.replaySeq,
                                      raw.recordCount)) {
      device->replayDrainLedger.poison();
      return commitChunkFail("provider-v3-event-start");
    }

    if ((sourceCutDriver || sourceConsumer || recordConsumer) &&
        !preflightProviderOrderedControls(imported, raw.replaySeq)) {
      const auto hr = commitChunkFail(
          "provider-replay-ordered-control-preflight");
      device->replayDrainLedger.poison();
      if (auto* observer = device->mutationCompositionObserver.get()) {
        observer->observeGlobalBarrier(
            dxmt9::resources::mutation_observer::BarrierReason::Failure);
      }
      return hr;
    }

    std::uint32_t localCompletedRanges = 0u;
    auto& completed = completedRanges ? *completedRanges : localCompletedRanges;
    std::uint32_t localCompletedBatchCuts = 0u;
    ScopedProviderReplayRecordConsumer recordScope(recordConsumer);
    ScopedProviderReplayObservationV4 observationScope(
        observationResultV4 && observationResultV4->captureAccepted ? &observationStateV4 : nullptr);
    if (recordConsumer) markLegacyResources(device, raw);
    cpuPhase(1u);
    int32_t hr;
    {
      dxmt9::diagnostic::ScopedSlotValidationProbe validationProbe(
          traceCpu ? observeProviderSlotValidation : nullptr);
      hr = recordConsumer
          ? replayCompatibilityChunk(device, raw, false,
                 std::any_of(imported.records.begin(), imported.records.end(),
                     [](const auto& record) {
                       return record.type == D9C_COMMAND_RECORD_QUERY_ISSUE ||
                              record.type == D9C_COMMAND_RECORD_READBACK ||
                              record.type == D9C_COMMAND_RECORD_UPDATE_TEXTURE;
                     }),
                 ReplayRecordRange{.orderedControlPreflightDone = true})
          : sourceConsumer
          ? replayProviderSourcePolicy(device, raw, imported, policy, ranges,
                                       *sourceConsumer)
          : sourceCutDriver
          ? replayProviderRecordRanges(device, raw, ranges,
                                       completed, batchCuts,
                                       completedBatchCuts ? *completedBatchCuts
                                                          : localCompletedBatchCuts)
          : replayPlannedChunk(
                device, raw, /*pacedByPresentOrdinal=*/false,
                /*allowDirectArena=*/
                    policy == dxmt9::d3d9::ProviderReplayPolicy::ProductionSlot,
                /*forceEventSerial=*/false,
                /*reportOffloadReplayStage=*/false,
                /*allowDirectSlot=*/
                     policy != dxmt9::d3d9::ProviderReplayPolicy::Compatibility);
    }
    cpuPhase(2u);
    if (!failed(hr)) {
      if (!sourceCutDriver && !sourceConsumer && !recordConsumer) {
        maybePublishEarlyCpuReadyPrefix(device, raw);
      }
      if (auto* observer = device->mutationCompositionObserver.get()) {
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
      device->replayDrainLedger.publishReplayed(raw);
      if (runtimeReplayV4)
        (void)runtimeReplayV4->acknowledge(!device->replayDrainLedger.terminal());
      observationStateV4.settled(raw.replaySeq, raw.recordCount,
          !device->replayDrainLedger.terminal());
      if (!v4Selected && raw.renderTapeCaptureToken)
        dxmt9::d3d9::observeRenderTapeRuntimeReplaySettlementV3(device, raw, true);
      if (recordConsumer && recordConsumer->eventSettled &&
          (device->replayDrainLedger.terminal() ||
           !recordConsumer->eventSettled(recordConsumer->context, raw.replaySeq,
                                         raw.recordCount))) {
        device->replayDrainLedger.poison();
        if (auto upper = device->dev().upperDevice())
          upper->queue().failStopReplaySourceExecution();
        return commitChunkFail("provider-v3-event-settlement");
      }
    } else {
      device->replayDrainLedger.poison();
      if (runtimeReplayV4) (void)runtimeReplayV4->acknowledge(false);
      observationStateV4.settled(raw.replaySeq, raw.recordCount, false);
      if (!v4Selected && raw.renderTapeCaptureToken)
        dxmt9::d3d9::observeRenderTapeRuntimeReplaySettlementV3(device, raw, false);
      if (auto* observer = device->mutationCompositionObserver.get()) {
        observer->observeGlobalBarrier(
            dxmt9::resources::mutation_observer::BarrierReason::Failure);
      }
    }
    if (!failed(hr)) cpuPhase(3u);
    return hr;
  } catch (const std::bad_alloc&) {
    if (ledgerPublished) device->replayDrainLedger.poison();
    if (ledgerPublished) observationStateV4.settled(raw.replaySeq, raw.recordCount, false);
    return commitChunkFail("provider-replay-allocation", 0xffffffffu, 0u,
                           dxmt9::core::E_OUTOFMEMORY);
  } catch (...) {
    if (ledgerPublished) device->replayDrainLedger.poison();
    if (ledgerPublished) observationStateV4.settled(raw.replaySeq, raw.recordCount, false);
    return commitChunkFail("provider-replay-exception");
  }
}
}  // namespace

bool dxmt9::d3d9::providerReplayRecordConsumerEnabled() noexcept {
  return gProviderReplayRecordConsumer != nullptr;
}

bool dxmt9::d3d9::observeProviderReplayTransactionStarted(
    const ReplayTransactionState& state) noexcept {
  const auto* consumer = gProviderReplayRecordConsumer;
  return !consumer || !consumer->transactionStarted ||
      consumer->transactionStarted(consumer->context, state);
}

bool dxmt9::d3d9::observeProviderReplayTransactionCommitted(
    const ReplayTransactionState& state) noexcept {
  const auto* consumer = gProviderReplayRecordConsumer;
  return !consumer || !consumer->transactionCommitted ||
      consumer->transactionCommitted(consumer->context, state);
}

bool dxmt9::d3d9::observeProviderReplayRecordBefore(
    std::uint32_t recordIndex, std::uint32_t recordType) noexcept {
  const auto* consumer = gProviderReplayRecordConsumer;
  return !consumer || !consumer->beforeRecord ||
      consumer->beforeRecord(consumer->context, recordIndex, recordType);
}

bool dxmt9::d3d9::observeProviderReplayRecordAfter(
    std::uint32_t recordIndex, std::uint32_t recordType,
    std::int32_t result) noexcept {
  const auto* consumer = gProviderReplayRecordConsumer;
  return !consumer || !consumer->recordDispatched ||
      consumer->recordDispatched(consumer->context, recordIndex, recordType,
                                 result);
}

bool dxmt9::d3d9::observeProviderReplayRecordDispatch(
    std::uint32_t recordIndex, std::uint32_t recordType) noexcept {
  const auto* consumer = gProviderReplayRecordConsumer;
  return !consumer || !consumer->beforeDispatch ||
      consumer->beforeDispatch(consumer->context, recordIndex, recordType);
}

bool dxmt9::d3d9::validateProviderReplayRecordRanges(
    std::uint32_t totalRecords,
    std::span<const dxmt9::d3d9::ProviderReplayRecordRange> ranges) noexcept {
  if (ranges.empty()) {
    return false;
  }

  std::uint64_t cursor = 0u;
  for (const auto& range : ranges) {
    if (range.recordCount == 0u ||
        range.firstRecord != cursor ||
        static_cast<std::uint64_t>(range.recordCount) >
            static_cast<std::uint64_t>(totalRecords) - cursor) {
      return false;
    }
    cursor += range.recordCount;
  }
  return cursor == totalRecords;
}

bool dxmt9::d3d9::validateProviderReplayBatchCuts(
    std::uint32_t totalRecords,
    std::span<const ProviderReplayRecordRange> ranges,
    std::span<const std::uint32_t> batchCuts) noexcept {
  if (!validateProviderReplayRecordRanges(totalRecords, ranges)) return false;
  std::size_t source = 0u;
  std::uint32_t previous = 0u;
  for (const auto cut : batchCuts) {
    if (cut <= previous || cut >= totalRecords) return false;
    while (source + 1u < ranges.size() &&
           ranges[source + 1u].firstRecord <= cut) ++source;
    if (cut == ranges[source].firstRecord) return false;
    previous = cut;
  }
  return true;
}

int32_t dxmt9::d3d9::replayPrevalidatedResolvedCommandChunk(
    D9CDevice* d, std::span<const std::byte> bytes,
    const dxmt9::d3d9::CommandChunkEnvelope& envelope,
    std::span<void* const> resolvedObjects,
    dxmt9::d3d9::ProviderReplayPolicy policy) noexcept {
  if (!d || bytes.empty() ||
      envelope.version != D9C_COMMAND_CHUNK_VERSION) {
    return commitChunkFail("provider-replay-bad-input");
  }
  if (policy != dxmt9::d3d9::ProviderReplayPolicy::Historical &&
      policy != dxmt9::d3d9::ProviderReplayPolicy::Compatibility &&
      policy != dxmt9::d3d9::ProviderReplayPolicy::ProductionSlot) {
    return commitChunkFail("provider-replay-policy");
  }
  const bool productionSlot =
      policy == dxmt9::d3d9::ProviderReplayPolicy::ProductionSlot;
  if (productionSlot &&
      !commandChunkProducerIdentityImportable(envelope.producerIdentity)) {
    return commitChunkFail("provider-replay-production-slot-identity");
  }

  dxmt9::d3d9::ImportedChunkView imported;
  if (!dxmt9::d3d9::importPrevalidatedCommandChunk(
          bytes, envelope, imported) ||
      resolvedObjects.size() != imported.handles.size()) {
    return commitChunkFail("provider-replay-import");
  }
  return runProviderReplay(
      d, bytes, envelope, resolvedObjects, imported, policy, {}, false,
      nullptr, {}, nullptr);
}

std::int32_t dxmt9::d3d9::replayPrevalidatedResolvedCommandChunkObservedV4(
    D9CDevice* device, std::span<const std::byte> bytes,
    const CommandChunkEnvelope& envelope, std::span<void* const> resolvedObjects,
    ProviderReplayPolicy policy, const ProviderReplayObservationV4& observation,
    ProviderReplayObservationV4Result& observationResult) noexcept {
  observationResult = {};
  // Canonical input/policy admission is exactly the ordinary provider entry.
  // Capture credentials are checked separately after the actual ledger raw
  // exists; a stale observer cannot change whether these semantics execute.
  if (!device || bytes.empty() || envelope.version != D9C_COMMAND_CHUNK_VERSION)
    return commitChunkFail("provider-replay-bad-input");
  if (policy != ProviderReplayPolicy::Historical &&
      policy != ProviderReplayPolicy::Compatibility &&
      policy != ProviderReplayPolicy::ProductionSlot)
    return commitChunkFail("provider-replay-policy");
  if (policy == ProviderReplayPolicy::ProductionSlot &&
      !commandChunkProducerIdentityImportable(envelope.producerIdentity))
    return commitChunkFail("provider-replay-production-slot-identity");
  ImportedChunkView imported;
  if (!importPrevalidatedCommandChunk(bytes, envelope, imported) ||
      resolvedObjects.size() != imported.handles.size())
    return commitChunkFail("provider-replay-import");
  return runProviderReplay(device, bytes, envelope, resolvedObjects, imported,
      policy, {}, false, nullptr, {}, nullptr, nullptr, nullptr,
      &observation, &observationResult);
}

std::int32_t dxmt9::d3d9::replayPrevalidatedResolvedCommandChunkAtRecordBoundaries(
    D9CDevice* device, std::span<const std::byte> bytes,
    const dxmt9::d3d9::CommandChunkEnvelope& envelope,
    std::span<void* const> resolvedObjects,
    std::span<const dxmt9::d3d9::ProviderReplayRecordRange> ranges,
    std::uint32_t& completedRanges,
    std::span<const std::uint32_t> batchCuts,
    std::uint32_t* completedBatchCuts) noexcept {
  completedRanges = 0u;
  if (completedBatchCuts) *completedBatchCuts = 0u;
  if (!device || bytes.empty() ||
      envelope.version != D9C_COMMAND_CHUNK_VERSION ||
      !dxmt9::d3d9::validateProviderReplayBatchCuts(
          envelope.recordCount, ranges, batchCuts)) {
    return commitChunkFail("provider-replay-range-input");
  }

  dxmt9::d3d9::ImportedChunkView imported;
  if (!dxmt9::d3d9::importPrevalidatedCommandChunk(
          bytes, envelope, imported) ||
      resolvedObjects.size() != imported.handles.size()) {
    return commitChunkFail("provider-replay-import");
  }
  return runProviderReplay(
      device, bytes, envelope, resolvedObjects, imported,
      dxmt9::d3d9::ProviderReplayPolicy::Historical, ranges, true,
      &completedRanges, batchCuts, completedBatchCuts);
}

std::int32_t dxmt9::d3d9::replayPrevalidatedResolvedCommandChunkBySources(
    D9CDevice* device, std::span<const std::byte> bytes,
    const CommandChunkEnvelope& envelope,
    std::span<void* const> resolvedObjects,
    ProviderReplayPolicy policy,
    std::span<const ProviderReplayRecordRange> ranges,
    ProviderReplaySourceConsumer consumer) noexcept {
  if (!device || bytes.empty() ||
      envelope.version != D9C_COMMAND_CHUNK_VERSION ||
      envelope.recordCount > kProviderSourceReplayMaxRecords ||
      (policy != ProviderReplayPolicy::Compatibility &&
       policy != ProviderReplayPolicy::ProductionSlot) ||
      !commandChunkProducerIdentityImportable(envelope.producerIdentity) ||
      !validateProviderReplayRecordRanges(envelope.recordCount, ranges)) {
    return commitChunkFail("provider-source-policy-input");
  }
  ImportedChunkView imported;
  if (!importPrevalidatedCommandChunk(bytes, envelope, imported) ||
      resolvedObjects.size() != imported.handles.size()) {
    return commitChunkFail("provider-source-policy-import");
  }
  return runProviderReplay(device, bytes, envelope, resolvedObjects, imported,
                            policy, ranges, false, nullptr, {}, nullptr,
                            &consumer);
}

std::int32_t dxmt9::d3d9::replayPrevalidatedResolvedCommandChunkV3(
    D9CDevice* device, std::span<const std::byte> bytes,
    const CommandChunkEnvelope& envelope,
    std::span<void* const> resolvedObjects, ProviderReplayPolicy policy,
    ProviderReplayRecordConsumer consumer) noexcept {
  if (!device || bytes.empty() ||
      envelope.version != D9C_COMMAND_CHUNK_VERSION ||
      envelope.recordCount > kProviderSourceReplayMaxRecords ||
      policy != ProviderReplayPolicy::Compatibility ||
      !consumer.eventStarted || !consumer.beforeRecord || !consumer.beforeDispatch ||
      !consumer.recordDispatched ||
      !consumer.eventSettled ||
      !commandChunkProducerIdentityImportable(envelope.producerIdentity) ||
      gProviderReplayRecordConsumer) {
    return commitChunkFail("provider-v3-input");
  }
  ImportedChunkView imported;
  if (!importPrevalidatedCommandChunk(bytes, envelope, imported) ||
      resolvedObjects.size() != imported.handles.size())
    return commitChunkFail("provider-v3-import");
  return runProviderReplay(device, bytes, envelope, resolvedObjects, imported,
                            policy, {}, false, nullptr, {}, nullptr,
                            nullptr, &consumer);
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
  auto nativeTraceScope = dxmt9::tracing::enabled()
      ? dxmt9::tracing::TraceScope(
            static_cast<uint8_t>(dxmt9::tracing::Category::Import),
            static_cast<uint16_t>(dxmt9::tracing::EventId::ImportBegin),
            static_cast<uint16_t>(dxmt9::tracing::EventId::ImportEnd),
            0, 0, dxmt9::tracing::owner_id(d))
      : dxmt9::tracing::TraceScope{};
  const bool traceSupply = dxmt9::supply::enabled();
  if (traceSupply)
    dxmt9::supply::record(dxmt9::supply::Event::ImportBegin, d);
  const auto traceImportExit = [d](void*) noexcept {
    dxmt9::supply::record(dxmt9::supply::Event::ImportEnd, d);
  };
  const std::unique_ptr<void, decltype(traceImportExit)> traceImportScope(
      traceSupply ? d : nullptr, traceImportExit);
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
    dxmt9::d3d9::ScopedQueryReplayOrdinal queryOrdinalScope(raw.replaySeq);
    const bool v4Selected = runtimeReplayV4Selected(d);
    std::optional<dxmt9::d3d9::ScopedRenderTapeRuntimeReplayV4> runtimeReplayV4;
    if (raw.renderTapeCaptureToken || raw.renderTapeEventOrdinal) {
      if (auto owner = dxmt9::d3d9::acquireRenderTapeRuntimeIdentityV4(d))
        runtimeReplayV4.emplace(std::move(owner), dxmt9::d3d9::ReplayTransactionRawIdentity{
            raw.renderTapeCaptureToken, raw.renderTapeEventOrdinal, raw.replaySeq, raw.recordCount},
            /*requireLedgerAck=*/true, acquireRuntimeReplayLeaseV4(d));
    }
    const int32_t hr = replayPlannedChunk(
        d, raw, /*pacedByPresentOrdinal=*/false,
        /*allowDirectArena=*/false);
    if (!failed(hr)) {
      maybePublishEarlyCpuReadyPrefix(d, raw);
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
      if (runtimeReplayV4) (void)runtimeReplayV4->acknowledge(!d->replayDrainLedger.terminal());
      if (!v4Selected && raw.renderTapeCaptureToken)
        dxmt9::d3d9::observeRenderTapeRuntimeReplaySettlementV3(d, raw, true);
    } else {
      d->replayDrainLedger.poison();
      if (runtimeReplayV4) (void)runtimeReplayV4->acknowledge(false);
      if (!v4Selected && raw.renderTapeCaptureToken)
        dxmt9::d3d9::observeRenderTapeRuntimeReplaySettlementV3(d, raw, false);
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
