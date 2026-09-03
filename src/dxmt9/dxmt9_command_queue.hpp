#pragma once

// Upper-runtime CommandQueue — the execution service and a self-hosting
// runtime node. Owns the WMT::CommandQueue handle, the chunk ring
// state, the three worker threads (encode / finish / completion), the
// per-frame scratch allocators, the queue-owned ResourceInitializer
// for deferred texture uploads, the queueLifecycle_ binding, the
// queue-owned transfer paths (mapBuffer + readbackSurface), and the
// encode-context assembly path that feeds encoders::encodeChunk.
//
// Pool, pipeline cache, shader archive, limits, frame scratch, and the
// deferred ResourceInitializer are queue-owned. DeviceImpl only passes
// the WMT device plus finalized limits and carries D3D9-facing policy
// through per-submit descriptors.
//
// CommandQueue does NOT persist the shader archive (that's
// dxmt9::shaders::Archive's dtor) and does NOT run under an
// @autoreleasepool itself (the encode chunk scopes its own).

#include "../winemetal/Metal.hpp"
#include "dxmt9/thread_ownership.hpp"
#include "dxmt9_argbuf_hybrid.hpp"
#include "dxmt9_backend_types.hpp"
#include "dxmt9/copy_materialization_ledger.hpp"
#include "dxmt9_capture.hpp"
#include "dxmt9_direct_continuation.hpp"
#include "dxmt9_queue.hpp"
#include "dxmt9/wsi_surface_protocol.hpp"
#include "dxmt9_hud.hpp"
#include "dxmt9_pipeline_cache.hpp"
#include "dxmt9_presenter.hpp"
#include "dxmt9_resource_pool.hpp"
#include "dxmt9_render_scheduling.hpp"
#include "dxmt9_ring_arena.hpp"
#include "dxmt9_session_release.hpp"
#include "dxmt9_scheduling_progress_watchdog.hpp"
#include "dxmt9_shader_archive.hpp"
#include "dxmt9_transient_resource_arena.hpp"
#include "render/encode_scheduling_progress.hpp"
#include "dxmt9_uniform_dirty.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace dxmt9 {

class Device;
class CommandQueue;
struct CommandQueueArenaLeaseTestAccess;
class Presenter;
class PresentDrawableToken;

namespace encoders {
struct EncodeChunkOptions;
struct EncodeContext;
struct EncodeDrawRecorder;
}
namespace resources { class Initializer; }
// R-BACK-31.7 — the queue owns the render backend via a unique_ptr to this
// interface. A forward declaration (not the full render/backend_factory.hpp
// include) is used deliberately: backend_factory.hpp transitively pulls in
// dxmt9_draw_encoder.hpp, which needs the COMPLETE CommandQueue type
// (PreUploadedDrawData stores CommandQueue::TransientBufferSlice). Including
// it here — before CommandQueue is defined — is a hard include cycle. The
// .cpp includes the full header where the complete IRenderBackend is needed
// (factory call + out-of-line ~CommandQueue destroying the unique_ptr).
namespace render { class IRenderBackend; }

// Chunk-ring size + in-flight cap. Match upstream dxmt's kCommandChunkCount:
// the ring may queue many chunks, while submitPresent() enforces present
// frame-latency through queue-owned present tokens.
inline constexpr size_t kCommandChunkCount = 32;
inline constexpr size_t kMaxQueuedChunks = kCommandChunkCount - 1;

enum class DceChunkLookaheadAction : std::uint8_t {
  UseReady,
  FailOpen,
};

enum class DceChunkLookaheadSourceAction : std::uint8_t {
  Poison,
  EncodeCurrentHoldNext,
  EncodeCurrentExposeLegacyLookahead,
};

// A Direct arena successor is a valid FIFO source, but it cannot be exposed
// through the legacy ChunkSlot-only semantic proof window. Encode the legacy
// current source fail-open and keep the arena successor as the next current.
inline DceChunkLookaheadSourceAction resolveDceChunkLookaheadSourceAction(
    bool currentValid,
    bool currentHasLegacySlot,
    bool hasNext,
    bool nextValid,
    bool nextHasLegacySlot) noexcept {
  if (!currentValid || !currentHasLegacySlot || (hasNext && !nextValid)) {
    return DceChunkLookaheadSourceAction::Poison;
  }
  if (hasNext && nextHasLegacySlot) {
    return DceChunkLookaheadSourceAction::EncodeCurrentExposeLegacyLookahead;
  }
  return DceChunkLookaheadSourceAction::EncodeCurrentHoldNext;
}

// Pure no-wait policy for the one-next-chunk DCE window. A FIFO-ready successor
// is selected opportunistically after prefix encode; otherwise the held source
// is released immediately without a cross-chunk proof. DCE must never create a
// producer-to-encode bubble.
inline DceChunkLookaheadAction resolveDceChunkLookaheadAction(
    bool hasReady) noexcept {
  if (hasReady) {
    return DceChunkLookaheadAction::UseReady;
  }
  return DceChunkLookaheadAction::FailOpen;
}

// R-BACK-2.51 — shared present-latency helpers. Both the inline seqId-based
// present boundary (CommandQueue::presentBoundary /
// CommandQueue::deferPresentBoundary, via presentBoundaryLatency() below)
// and the commit-replay offload's present-ordinal boundary
// (CommandQueue::waitPresentOrdinalBoundary) must honor
// DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS identically, so the cap math lives
// here once instead of being duplicated per boundary mechanism.
inline std::uint32_t backBufferLatencyCap(std::uint32_t backBufferCount) {
  const std::uint32_t normalized = std::max(1u, backBufferCount);
  if (normalized >= core::kMaxFrameLatency) {
    return core::kMaxFrameLatency;
  }
  return normalized + 1u;
}

// Read-once resolver for DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS.
inline bool capFrameLatencyToBackBuffers() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

// Pure combinator taking the cap-enabled bit as an explicit parameter
// (rather than re-resolving the env var) so it is unit-testable without a
// process-env fixture; both boundary mechanisms below call it with
// capFrameLatencyToBackBuffers() as the live value.
inline std::uint32_t cappedFrameLatency(std::uint32_t maxFrameLatency,
                                        std::uint32_t backBufferCount,
                                        bool capEnabled) {
  if (!capEnabled) {
    return maxFrameLatency;
  }
  return std::min(maxFrameLatency, backBufferLatencyCap(backBufferCount));
}

// Immediate presents do not need a multi-frame producer runway to cover a
// display interval. When the application is still using the engine's default
// maximum, keep only one present-bearing frame incomplete. This is a stricter
// scheduling bound within the advertised maximum, not a change to the D3D9Ex
// maximum-frame-latency value. A non-default application/env value remains an
// explicit scheduling window, and synchronized presents retain the
// normal default.
inline std::uint32_t effectivePresentFrameLatency(
    std::uint32_t maxFrameLatency, bool displaySyncEnabled) {
  if (!displaySyncEnabled &&
      maxFrameLatency == core::kDefaultFrameLatency) {
    return 1u;
  }
  return maxFrameLatency;
}

// Pure composition shared by both present-boundary implementations. Keeping
// the Immediate-default rule and optional back-buffer cap in one helper makes
// the seqId and present-ordinal paths order-isomorphic for the same present.
inline std::uint32_t resolvedPresentFrameLatency(
    std::uint32_t maxFrameLatency, std::uint32_t backBufferCount,
    bool displaySyncEnabled, bool capEnabled) {
  return cappedFrameLatency(
      effectivePresentFrameLatency(maxFrameLatency, displaySyncEnabled),
      backBufferCount, capEnabled);
}

inline std::uint32_t presentBoundaryLatency(const core::SwapDesc& desc) {
  return resolvedPresentFrameLatency(
      desc.maxFrameLatency, desc.backBufferCount, desc.displaySyncEnabled,
      capFrameLatencyToBackBuffers());
}

// R-BACK-2.51(g) — per-present decision for which present-boundary branch
// CommandQueue::submitPresent takes. Extracted as a pure truth table (input:
// the specific present's core::SwapDesc::pacedByPresentOrdinal flag plus the
// resolved BoundaryPolicy) so it is unit-testable without a live queue. A
// present paced by the commit-replay offload's present-ordinal boundary
// (dxmt9::Device::waitPresentOrdinalBoundary already ran for it before
// submitPresent was called) always skips the inline boundary, regardless of
// BoundaryPolicy — that ordinal wait already applied the policy itself. Any
// other present (direct COM presents, or presents replayed by the
// synchronous non-offload chunk path) falls through to the same
// policy-driven branches submitPresent used before offload existed.
enum class PresentBoundaryAction {
  SkipPacedByOffloadOrdinal,
  Defer,
  ApplyInline,
  SkipDisabled,
};

inline PresentBoundaryAction resolvePresentBoundaryAction(bool pacedByPresentOrdinal,
                                                           BoundaryPolicy policy) {
  if (pacedByPresentOrdinal) {
    return PresentBoundaryAction::SkipPacedByOffloadOrdinal;
  }
  if (policy == BoundaryPolicy::DeferredPresentCompletion) {
    return PresentBoundaryAction::Defer;
  }
  if (policy != BoundaryPolicy::Disabled) {
    return PresentBoundaryAction::ApplyInline;
  }
  return PresentBoundaryAction::SkipDisabled;
}

// DXMT9_OFFLOAD_COMMIT_REPLAY — the commit-replay offload path paces
// present frame-latency itself via CommandQueue::waitPresentOrdinalBoundary,
// keyed on a present ordinal instead of a queue seqId. R-BACK-2.51(g):
// submitPresent() must not drain/apply the inline seqId-based boundary for a
// present that ordinal wait already paced (core::SwapDesc::pacedByPresentOrdinal,
// set only by the D3D9 chunk-replay path), or the two mechanisms would
// double-wait on the same present token; any other present still
// participates in the inline boundary even while this flag is on elsewhere
// in the process.
// Engine default ON since d45af067 (the "heap corruption" that briefly
// blocked the flip was root-caused to a native-spec harness drain gap, not
// a production race — cad446ce). This copy MUST stay parse-identical to the
// canonical resolver in src/d3d9/device_c_replay_offload.cpp: a drifted
// default here fires the submitPresent pacedByPresentOrdinal assert because
// the d3d9 layer paces presents while this layer believes the offload is
// off (caught by the 2026-07-10 assert-provider GT1 hunt).
inline bool offloadCommitReplayEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("DXMT9_OFFLOAD_COMMIT_REPLAY");
    if (!value || value[0] == '\0') {
      return true;
    }
    return !(value[0] == '0' && value[1] == '\0');
  }();
  return enabled;
}

// App-side present-ordinal frame-latency target for the commit-replay
// offload path (TLA+: PresentFrameLatency ordinal variant). Ordinals count
// present-bearing commits 1,2,3...; the math is the exact shape of the
// boundary seqId target so pacing stays order-isomorphic to the inline
// present boundary.
inline std::uint64_t presentOrdinalBoundaryTarget(std::uint64_t presentOrdinal,
                                                  std::uint32_t maxFrameLatency) {
  if (presentOrdinal == 0) {
    return 0;
  }
  const std::uint64_t latency = std::clamp<std::uint64_t>(
      maxFrameLatency, 1u, kMaxQueuedChunks);
  if (presentOrdinal <= latency) {
    return 0;
  }
  return presentOrdinal - latency;
}

// Pure planning step for waitPresentOrdinalBoundary: given the resolved
// boundary policy, the present ordinal being committed, the effective
// frame latency, and the previously stored deferred target, returns the
// ordinal target to wait on NOW (0 = no wait) and the new stored deferred
// target. Keeps the policy mapping unit-testable without a live queue
// (TLA+: PresentFrameLatency ordinal variant).
struct PresentOrdinalWaitPlan {
  std::uint64_t waitTargetOrdinal = 0;
  std::uint64_t storedDeferredTarget = 0;
};

inline PresentOrdinalWaitPlan planPresentOrdinalWait(
    BoundaryPolicy policy, std::uint64_t presentOrdinal,
    std::uint32_t maxFrameLatency, std::uint64_t storedDeferredTarget) {
  if (policy == BoundaryPolicy::Disabled) {
    return {0, storedDeferredTarget};
  }
  if (policy == BoundaryPolicy::DeferredPresentCompletion) {
    const std::uint64_t nextTarget =
        presentOrdinal == std::numeric_limits<std::uint64_t>::max()
            ? presentOrdinalBoundaryTarget(presentOrdinal, maxFrameLatency)
            : presentOrdinalBoundaryTarget(presentOrdinal + 1, maxFrameLatency);
    return {storedDeferredTarget,
            std::max(storedDeferredTarget, nextTarget)};
  }
  return {presentOrdinalBoundaryTarget(presentOrdinal, maxFrameLatency),
          storedDeferredTarget};
}

// Present-ordinal frame-latency gate for the commit-replay offload path.
// Owns only the ordinal watermark, the sticky abort flag, and the deferred
// target; the caller supplies the mutex/cv it shares with present
// completion publication. Extracted so the wait/abort mechanics are
// unit-testable without a live queue (gap.md offload row).
struct PresentOrdinalGate {
  std::uint64_t completedOrdinal = 0;
  std::uint64_t deferredTarget = 0;
  bool aborted = false;

  // Returns true if the caller must wait on `cv` for `waitTarget` (already
  // computed by planPresentOrdinalWait); encapsulates the abort/satisfied
  // early-outs. Caller holds `lock`.
  bool needsWait(std::uint64_t waitTarget) const {
    return waitTarget != 0 && !aborted && completedOrdinal < waitTarget;
  }
  bool waitDone(std::uint64_t waitTarget, bool stopped) const {
    return ::dxmt9::queue::presentTokenWaitSatisfied(
        completedOrdinal, waitTarget, stopped, aborted);
  }
};

class CommandQueue {
 private:
  struct ArenaBuildContext;
  struct DirectChunkSlotBuildContext;
  static core::DirectReplayDrawDisposition appendDirectChunkSlotDrawBorrowed(
      void* state, const core::DirectReplayDrawInput& input) noexcept;
  static void armDirectReplayDrawAppender(
      core::DirectReplayDrawAppendCapability& destination,
      void* state) noexcept;

 public:
  // Full execution-service constructor. Allocates the WMT::CommandQueue,
  // constructs the queue-owned pool / pipeline cache / shader archive /
  // ResourceInitializer, binds queueLifecycle_ to own state, and spawns
  // the three worker threads. `limits` is the caller's final BackendLimits
  // (DeviceImpl finalizes desc.limits against device caps before passing
  // it in) and is stored verbatim — no post-construction back-channel.
  // A null device or queue-allocation failure leaves the object inert
  // (valid() == false, threadsStarted_ == false) but still safely
  // destructible.
  //
  // Presentation back-channels (maxFrameLatency + notifyPresentationStatus)
  // ride on each core::SwapDesc — DeviceImpl::present() fills them per
  // submission. The queue holds no Device* pointer.
  //
  // dxmt9-specific divergence from upstream's single-arg ctor: dxmt9's
  // encoders consume BackendLimits in the encode path (depth24/sampleN
  // policy, max texture size, etc.), so the queue snapshots it at
  // construction. DeviceImpl also passes the once-sampled CPU-ready Tape
  // activation decision; this constructor never reads that environment
  // gate itself. Upstream dxmt has no BackendLimits.
  CommandQueue(WMT::Device device, core::BackendLimits limits,
               bool cpuReadySessionLaneEnabled,
               bool renderTapePublisherCaptureEnabled = false,
               render::RenderPartitionConfig renderPartitionConfig = {},
               render::EncodeExecutionTopology encodeExecutionTopology =
                   render::kStableOwnedRawSlotTopology);

  // Cold control-plane diagnostic boundary used by the PE device Reset and
  // teardown owners; it never synthesizes per-source reclaim transitions.
  void observePipelineControl(
      ::dxmt9::queue::PipelineControl control,
      ::dxmt9::queue::PipelineDisposition disposition) noexcept;

  // Inert queue for native encoder lifecycle specs. The supplied handle is
  // used only as a validity token: this constructor does not create queue
  // workers, resource initializers, or any other Metal-owned subsystem.
  // Callers must inject command buffers and must not issue Metal commands.
  struct InertTestQueueTag {};
  CommandQueue(InertTestQueueTag,
               WMT::Reference<WMT::CommandQueue> queue,
               core::BackendLimits limits);

  // Native contract fixture for the queue-owned arena transaction. It binds
  // the lifecycle controller but starts no Metal objects or worker threads.
  struct ArenaLeaseTestQueueTag {};
  CommandQueue(ArenaLeaseTestQueueTag, core::BackendLimits limits,
               WMT::Reference<WMT::CommandQueue> queue = {},
               render::RenderPartitionConfig renderPartitionConfig = {});

  // Joins worker threads (if started). Archive persistence is not a
  // queue responsibility — it runs from shaders::Archive's dtor.
  ~CommandQueue();
  CommandQueue(const CommandQueue&) = delete;
  CommandQueue& operator=(const CommandQueue&) = delete;

  // True if the full-service ctor succeeded in spawning the worker
  // threads. Drives DeviceImpl::ready().
  bool started() const noexcept { return threadsStarted_; }

  // True if a WMT::CommandQueue handle was allocated. A non-null
  // handle with !started() means only the test ctor ran.
  bool valid() const noexcept { return static_cast<bool>(queue_); }

  // Queue-owned resource initializer API.
  void uploadTextureLevel(core::TextureHandle handle,
                          std::uint32_t level,
                          std::uint32_t width,
                          std::uint32_t height,
                          std::uint32_t depth,
                          std::uint32_t pitch,
                          std::uint32_t slicePitch,
                          std::span<const std::uint8_t> bytes);
  void initializeTextureZero(core::TextureHandle handle);
  core::HResult generateTextureMipSublevels(core::TextureHandle handle);

  // Flush any pending deferred uploads. Returned (event, value) is what
  // the render command buffer must wait on; value==0 means nothing to
  // wait for. Invoked at the head of each chunk's command buffer by
  // encoders::encodeChunk.
  struct InitializerFlush {
    WMT::Event event{};
    std::uint64_t value = 0;
    bool didFlush = false;
  };
  InitializerFlush flushInitializerUploads();

  // Submission / resource-marking surface. Each call acquires mutex_
  // internally; Pool access goes through pool_ (snapshotted at
  // construction).
  // Compact backend draw-run ingress — pushes one DrawRunCommandRecord
  // (CanonicalDrawState + DrawParam[N]) into the current ChunkSlot under a
  // single mutex acquire. Borrowed spans are copied before this call returns.
  void submitDrawRun(core::CanonicalDrawState state,
                     const core::DrawUniformPayload& uniforms,
                     std::span<const core::DrawParam> draws,
                     std::span<const core::DrawParamPayloadView> payloads = {});
  core::DirectReplayDrawDisposition submitDirectReplayDraw(
      const core::DirectReplayDrawInput& input) noexcept;

  // Cold, capture-only ownership copied from the exact Direct-Arena source
  // before it becomes visible to the encode thread.  Raw-record ranges are
  // expressed in the PE CommandChunk coordinate space; DAG indices and pass
  // kinds come from the event-wide authenticated FrameGraph after the
  // production pass-coalesce proof; segment edges do not create new passes.
  struct CpuReadyCapturePassRange {
    std::uint32_t firstRecord = 0;
    std::uint32_t recordCount = 0;
    std::uint32_t dagPassIndex = 0;
    std::uint32_t passKind = 0;
    std::uint64_t logicalPassId = 0;
  };

  struct CpuReadyCaptureIdentity {
    std::uint64_t sourceOrdinal = 0;
    std::uint64_t seqId = 0;
    std::uint32_t firstRecord = 0;
    std::uint32_t recordCount = 0;
    std::vector<CpuReadyCapturePassRange> ranges{};

    bool valid() const noexcept {
      return sourceOrdinal != 0 && seqId != 0 && recordCount != 0 &&
             !ranges.empty();
    }
  };

  struct CpuReadyCaptureIdentityBatch {
    std::vector<CpuReadyCaptureIdentity> segments{};

    bool valid() const noexcept {
      return segments.size() > 1u &&
             std::all_of(segments.begin(), segments.end(),
                         [](const auto& segment) { return segment.valid(); });
    }
  };

  enum class CpuReadyArenaPublishStatus : std::uint8_t {
    Published,
    RecoverableFailure,
    FailStopped,
  };

  enum class CpuReadyArenaFailureClass : std::uint8_t {
    None,
    Capacity,
    Validation,
    ResourceRetain,
    Publication,
    Completion,
    BuilderInitialization,
    ContextInvalid,
    Append,
    CaptureRanges,
    SegmentSelection,
    ActiveArenaRejected,
    InjectedBuilder,
    InjectedRollback,
    InjectedPostSemanticPublish,
    PayloadSeal,
    Planner,
    CaptureProjection,
    SnapshotValidation,
    Abort,
  };

  struct CpuReadyArenaFailureSnapshot {
    CpuReadyArenaFailureClass failureClass = CpuReadyArenaFailureClass::None;
    std::uint32_t source = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t segment = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t plannedPages = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t actualCommands = std::numeric_limits<std::uint32_t>::max();
  };

  // Move-only capability for one replay-thread-owned direct arena source.
  // Admission fixes and consumes raw/source/seq identities under mutex_; all
  // builder appends then run outside the queue lock. Destruction without a
  // successful publish is a fail-stop two-phase abort, never legacy fallback.
  class CpuReadyArenaBuildLease {
   public:
    CpuReadyArenaBuildLease() = default;
    ~CpuReadyArenaBuildLease();

    CpuReadyArenaBuildLease(const CpuReadyArenaBuildLease&) = delete;
    CpuReadyArenaBuildLease& operator=(
        const CpuReadyArenaBuildLease&) = delete;
    CpuReadyArenaBuildLease(CpuReadyArenaBuildLease&& other) noexcept;
    CpuReadyArenaBuildLease& operator=(
        CpuReadyArenaBuildLease&& other) noexcept;

    explicit operator bool() const noexcept { return queue_ != nullptr; }
    core::CpuReadyPublicationTicket ticket() const noexcept { return ticket_; }
    std::size_t controlIndex() const noexcept { return controlIndex_; }
    std::uint64_t seqId() const noexcept { return ticket_.seqId; }
    bool selectSegment(std::size_t segmentIndex) noexcept;
    // Selects a physical segment in the bounded SegmentSerial batch.  The
    // legacy selectSegment(global) spelling remains valid for EventSerial.
    bool selectSourceSegment(std::size_t sourceIndex,
                             std::size_t segmentIndex) noexcept;
    bool beginCaptureIdentity(std::uint32_t recordCount) noexcept;
    bool captureNextCommandRecord(std::uint32_t recordIndex) noexcept;
    bool captureNextDrawRecords(
        std::span<const std::uint32_t> recordIndices) noexcept;
    bool publish(
        std::span<const core::ChunkHandleEntry> resources = {},
        CpuReadyCaptureIdentity* captureIdentity = nullptr) noexcept;
    bool publishBatch(
        std::span<const core::ChunkHandleEntry> resources,
        CpuReadyCaptureIdentityBatch* captureIdentity) noexcept;
    CpuReadyArenaPublishStatus publishBatchWithStatus(
        std::span<const core::ChunkHandleEntry> resources,
        CpuReadyCaptureIdentityBatch* captureIdentity) noexcept;
    bool setCaptureSourceRanges(
        std::span<const std::uint32_t> firstRecords,
        std::span<const std::uint32_t> recordCounts) noexcept;
    // Explicit pre-effect rollback point for a recoverable capture seam.
    // The caller must invoke this before retrying the raw event.
    // Returns true only when the pre-effect batch rollback restored all
    // reservations. A false result is terminal; the caller must not retry.
    bool abortForFallback() noexcept;

   private:
    friend class CommandQueue;
    CpuReadyArenaBuildLease(
        CommandQueue& queue,
        core::CpuReadyPublicationTicket ticket,
        std::size_t controlIndex, bool batch = false) noexcept
        : queue_(&queue), ticket_(ticket), controlIndex_(controlIndex),
          batch_(batch) {}

    void abort(bool failStop = true) noexcept;

    CommandQueue* queue_ = nullptr;
    core::CpuReadyPublicationTicket ticket_{};
    std::size_t controlIndex_ = std::numeric_limits<std::size_t>::max();
    bool batch_ = false;
  };

  enum class CpuReadyArenaBeginStatus : std::uint8_t {
    Ready,
    TemporaryPressure,
    RecoverableFailure,
    Stopped,
    Corrupt,
    Invalid,
  };

  enum class CpuReadyArenaBeginStopReason : std::uint8_t {
    None,
    QueueAlreadyStopped,
    CompatibilityFlushStopped,
    CpuReadyTapeAlreadyStopped,
  };

  struct CpuReadyArenaBeginDiagnostic {
    core::metalqueue::QueueLifecycleController::PoisonOriginSnapshot
        poisonOrigin{};
  };

  enum class CpuReadyArenaBatchAbortStatus : std::uint8_t {
    RolledBack,
    FailStopped,
    RollbackFailed,
    NoActiveBatch,
  };

  struct CpuReadyArenaBeginResult {
    CpuReadyArenaBeginStatus status = CpuReadyArenaBeginStatus::Invalid;
    // Kept beside status so it occupies the status-alignment padding.  This
    // is part of the returned value, never a queue-global sidecar: callers
    // must retain the reason associated with this exact admission attempt.
    CpuReadyArenaBeginStopReason stopReason =
        CpuReadyArenaBeginStopReason::None;
    std::optional<CpuReadyArenaBuildLease> lease{};

    bool has_value() const noexcept { return lease.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }
    CpuReadyArenaBuildLease* operator->() noexcept { return &*lease; }
    const CpuReadyArenaBuildLease* operator->() const noexcept {
      return &*lease;
    }
    CpuReadyArenaBuildLease& operator*() noexcept { return *lease; }
    const CpuReadyArenaBuildLease& operator*() const noexcept {
      return *lease;
    }
  };

  // Layout baseline for the pre-diagnostic return value.  The reason field
  // above must fit in existing padding and never enlarge this ABI-local
  // native result type.
  struct CpuReadyArenaBeginResultLayoutBaseline {
    CpuReadyArenaBeginStatus status = CpuReadyArenaBeginStatus::Invalid;
    std::optional<CpuReadyArenaBuildLease> lease{};
  };
  static_assert(
      sizeof(CpuReadyArenaBeginResult) ==
          sizeof(CpuReadyArenaBeginResultLayoutBaseline),
      "CpuReadyArenaBeginResult diagnostics must fit existing padding");

  struct CpuReadyArenaPlanLimits {
    std::size_t pageSize = 0;
    std::size_t maxOrdinaryPagesPerSegment = 64;
    std::size_t maxSegmentsPerSource =
        core::kMaxArenaSourcePayloadSegments;
    std::size_t maxPagesPerSource = 0;
  };

  enum class DirectChunkSlotReplayStatus : std::uint8_t {
    Ready,
    Committed,
    LegacyUnsupported,
    LegacyOversized,
    LegacyPreEffectFailure,
    FailStopped,
  };

  enum class DirectChunkSlotReplayFailureReason : std::uint8_t {
    None,
    InvalidArguments,
    QueueStopped,
    ActiveBuildConflict,
    WritingSlotUnavailable,
    PayloadUnavailable,
    ProducerIdentityMissing,
    SpanWitnessMissing,
    SpanAdmissionRejected,
    ProducerIdentityAppendRejected,
    BuildGenerationUnavailable,
    RotationPublicationRejected,
    StructuralRejected,
    StorageRestoreRejected,
    AssemblerRejected,
  };

  class DirectChunkSlotReplayLease {
   public:
    DirectChunkSlotReplayLease() = default;
    ~DirectChunkSlotReplayLease();
    DirectChunkSlotReplayLease(const DirectChunkSlotReplayLease&) = delete;
    DirectChunkSlotReplayLease& operator=(
        const DirectChunkSlotReplayLease&) = delete;
    DirectChunkSlotReplayLease(
        DirectChunkSlotReplayLease&& other) noexcept;
    DirectChunkSlotReplayLease& operator=(
        DirectChunkSlotReplayLease&& other) noexcept;

    explicit operator bool() const noexcept { return queue_ != nullptr; }
    core::CpuReadyPublicationTicket ticket() const noexcept { return ticket_; }
    std::size_t controlIndex() const noexcept { return controlIndex_; }
    // Borrowed only for the synchronous replay call made while this lease is
    // alive. The capability is lease-owned and is invalidated by every
    // terminal settle operation; callers cannot retain destination storage.
    const core::DirectReplayDrawAppendCapability*
    borrowDirectRangeAppender() const noexcept {
      return directRangeAppendLive_ && directRangeAppender_
                 ? &directRangeAppender_
                 : nullptr;
    }
    void markSemanticEffectsStarted() noexcept { effectsStarted_ = true; }
    // Seal the open DrawRunCommandRecord at an island or coordinator cut, so
    // draws either side of the cut can never merge into one run record. Every
    // borrowed coordinator append below does this implicitly; the explicit
    // call exists for a cut the assembler never sees.
    void closeDirectRun() noexcept {
      if (directRangeAppendLive_ && directAssembler_ &&
          ownerThread_ == std::this_thread::get_id()) {
        directAssembler_->closeDirectRun();
      }
    }
    // Coordinator commands a lease span owns are appended through the
    // queue-owned build context (`borrowedDirectChunkSlotAppend`), not
    // through this lease: the lease is move-constructed out of an optional by
    // its caller, so its address is not stable and must never be published.
    // The lease remains the settlement authority.
    DirectChunkSlotReplayStatus commit(
        std::span<const core::ChunkHandleEntry> resources) noexcept;
    bool rollbackPreEffect() noexcept;

   private:
    friend class CommandQueue;
    DirectChunkSlotReplayLease(
        CommandQueue& queue, core::CpuReadyPublicationTicket ticket,
        std::size_t controlIndex,
        core::TransactionalChunkSlotAssembler* assembler,
        DirectChunkSlotBuildContext* context) noexcept
        : queue_(&queue), ticket_(ticket), controlIndex_(controlIndex),
          directAssembler_(assembler), directContext_(context),
          ownerThread_(std::this_thread::get_id()),
          directRangeAppendLive_(assembler != nullptr && context != nullptr) {
      if (directRangeAppendLive_) {
        CommandQueue::armDirectReplayDrawAppender(
            directRangeAppender_, this);
      }
    }
    void settle() noexcept;

    CommandQueue* queue_ = nullptr;
    core::CpuReadyPublicationTicket ticket_{};
    std::size_t controlIndex_ = std::numeric_limits<std::size_t>::max();
    bool effectsStarted_ = false;
    core::TransactionalChunkSlotAssembler* directAssembler_ = nullptr;
    DirectChunkSlotBuildContext* directContext_ = nullptr;
    core::DirectReplayDrawAppendCapability directRangeAppender_{};
    std::thread::id ownerThread_{};
    bool directRangeAppendLive_ = false;
  };

  struct DirectChunkSlotReplayBeginResult {
    DirectChunkSlotReplayStatus status =
        DirectChunkSlotReplayStatus::LegacyUnsupported;
    DirectChunkSlotReplayFailureReason failureReason =
        DirectChunkSlotReplayFailureReason::None;
    core::CpuReadyPublicationFailureReason publicationFailure =
        core::CpuReadyPublicationFailureReason::None;
    core::CpuReadySpanAdmission spanAdmission =
        core::CpuReadySpanAdmission::CrossRaw;
    std::optional<DirectChunkSlotReplayLease> lease{};

    bool has_value() const noexcept { return lease.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }
  };

  // A raw cut by an ordered control or a compatibility range owns several
  // consecutive lease spans. `spanOrdinal` is that raw's 0-based span index
  // and `finalSpan` marks the last one; together they let the Tape witness
  // admit the immediate successor span of the exact active raw without
  // widening which *other* raw may extend a populated slot. `allowRotation`
  // permits one bounded publish-and-retry when a populated slot cannot hold
  // the span's exact reservation, so a direct span never degrades into
  // draw-by-draw compatibility replay.
  DirectChunkSlotReplayBeginResult beginDirectChunkSlotReplay(
      std::uint64_t rawOrdinal,
      const core::SourcePayloadCapacity& capacity,
      std::size_t plannedBytes,
      core::CpuReadyProducerIdentity producerIdentity = {},
      std::size_t rangeRecordCount = 0,
      std::size_t rangeDrawCount = 0,
      std::uint32_t spanOrdinal = 0,
      bool finalSpan = true,
      bool allowRotation = false) noexcept;
  CpuReadyArenaBeginResult beginCpuReadyArenaSource(
      std::uint64_t rawOrdinal,
      const core::ArenaSourcePayloadLayout& layout,
      std::optional<core::metalqueue::CpuReadySupplyObservationToken>
          supplyAttempt = std::nullopt,
      core::CpuReadyProducerIdentity producerIdentity = {}) noexcept;
  CpuReadyArenaBeginResult beginCpuReadyArenaSources(
      std::uint64_t rawOrdinal,
      std::span<const core::ArenaSourcePayloadLayout> layouts,
      std::optional<core::metalqueue::CpuReadySupplyObservationToken>
          supplyAttempt = std::nullopt,
      core::CpuReadyProducerIdentity producerIdentity = {}) noexcept;
  CpuReadyArenaPlanLimits cpuReadyArenaPlanLimits() const noexcept {
    const auto& values = cpuReadyTape_.config().values();
    return {
        .pageSize = values.pageSize,
        .maxOrdinaryPagesPerSegment = 64,
        .maxSegmentsPerSource = core::kMaxArenaSourcePayloadSegments,
        .maxPagesPerSource = values.maxPagesPerSource,
    };
  }
  bool segmentSerialEnabled() const noexcept {
    return renderPartitionConfig_.sourceIdentity.resolved ==
           render::SourceIdentityMode::SegmentSerial;
  }
  void rejectActiveCpuReadyArenaSource() noexcept;
  bool cpuReadyArenaPoisoned() const noexcept {
    return arenaBuildPoisoned_.load(std::memory_order_acquire);
  }
  CpuReadyArenaFailureSnapshot peekCpuReadyArenaFailure() noexcept;
  CpuReadyArenaFailureSnapshot takeCpuReadyArenaFailure() noexcept;
  CpuReadyArenaBeginDiagnostic cpuReadyArenaBeginDiagnostic() noexcept;
  // A publish/proof failure after replayResolvedChunk has applied semantic
  // effects cannot take the pre-effect EventSerial retry. The caller marks
  // this queue fail-stop before returning the typed chunk failure.
  void failStopCpuReadyArena() noexcept {
    arenaBuildPoisoned_.store(true, std::memory_order_release);
  }
  // Bulk resource retention — chunk importer hands the deduped handle
  // set from D9CCommandChunk.handles[] in one call. Single mutex
  // acquire, dispatches per-kind to pool_.markBufferUse / markTextureUse
  // / markSurfaceUse using the current chunk's nextSeqId. Replaces
  // N×per-record markDrawResources walks once per-record marking is
  // suppressed for chunk-mode draws.
  void markChunkResources(std::span<const core::ChunkHandleEntry> entries);
  core::ChunkBufferBindingCaptureResult
  markChunkResourcesAndCaptureBufferBindings(
      std::span<const core::ChunkHandleEntry> entries,
      std::vector<core::ChunkBufferBindingSnapshot>& snapshots);
  core::ChunkBufferBindingCaptureResult captureChunkBufferBindings(
      std::span<const core::ChunkHandleEntry> entries,
      std::vector<core::ChunkBufferBindingSnapshot>& snapshots);
  bool waitForCpuReadyArenaAdmission(
      const core::ArenaSourcePayloadLayout& layout) noexcept;
  bool waitForCpuReadyArenaAdmission(
      std::span<const core::ArenaSourcePayloadLayout> layouts) noexcept;
  // The replay worker may promise only an already-adopted immediate raw FIFO
  // successor, after the current source has replayed successfully and before
  // it becomes Ready. No Tape storage or Metal/completion ownership moves.
  bool armCpuReadyNextSourceIntent(
      core::CpuReadyPublicationTicket predecessor,
      std::uint64_t nextRawOrdinal,
      bool predecessorHasPresent) noexcept;
  void cancelCpuReadyNextSourceIntent(
      std::uint64_t rawOrdinal) noexcept;

  // Ordered-control synchronization for the CPU-ready session lane. The
  // caller supplies only backend release semantics and the raw-stream fence;
  // this queue publishes any older compatibility slot, fixes the sequence
  // fence, wakes the encode coordinator, and waits until the requested pass
  // or submission action is acknowledged. SubmitAndWait additionally waits
  // for GPU completion of the fixed sequence fence. The default-off lane is a
  // no-op so its existing compatibility ordering remains unchanged.
  bool releaseCpuReadySessionBeforeOrderedControl(
      core::metalqueue::SessionReleaseReason reason,
      core::metalqueue::SessionReleaseAction action,
      std::uint64_t fenceRawOrdinal);

  // Phase 14: chunk importer toggles this around a chunk's record-iter
  // block. While true, submitDrawRun skips per-draw
  // pool_.markDrawResources because chunk.handles[] bulk retention
  // (markChunkResources) has already pinned every resource the chunk
  // touches against the same chunk seqId. Without this guard, every
  // chunk-mode draw double-marks the same (handle, seqId) pair —
  // correct but pure CPU waste (lastUsedSeqId compare always returns
  // the existing value). Default false keeps non-chunk test/core paths
  // pinned by the run's hot resource set.
  void setSkipDrawResourceMarking(bool skip);
  // One cold, nullable effective-replay sink copied into EncodeContext. The
  // draw encoder checks it once after final replay selection and before
  // command encoder effects.
  void setReplayObserver(core::metalqueue::ReplayObserverSink sink) noexcept {
    replayObserver_ = sink;
  }
  void noteCommitChunkEntryForCompletionGap();
  core::metalqueue::CpuReadySupplyObservationToken noteCpuReadySupplyReplayEntry(
      core::CpuReadyTape::PayloadKind sourceClass);
  void cancelCpuReadySupplyReplayEntry(
      core::CpuReadyTape::PayloadKind sourceClass,
      core::metalqueue::CpuReadySupplyObservationToken attemptToken);
  void noteCommitChunkReplayStartForCompletionGap();
  void noteCommitChunkReplayEndForCompletionGap(std::uint64_t replayNanoseconds);
  void noteCommitChunkReplayCpuBeforePublish(std::uint64_t nanoseconds);
  void noteCommitChunkActiveReplayCpuBeforePublish(std::uint64_t nanoseconds);
  void noteCommitChunkRecordShapeForCompletionGap(
      const core::metalqueue::NoEnqueueCommitChunkRecordShape& shape);
  void prefetchCurrentWritingSlotPipelines();
  void submitClear(const core::ClearDesc& desc);
  void submitSurfaceCopy(const core::SurfaceCopyDesc& desc);
  void submitStretchRect(const core::StretchRectDesc& desc);
  void submitReadback(const core::ReadbackDesc& desc);
  void submitColorFill(const core::ColorFillDesc& desc);
  void submitDepthResolve(const core::DepthResolveDesc& desc);
  void submitGenerateMipmaps(const core::GenerateMipmapsDesc& desc);
  // submitPresent / presentBoundary are the present surface today; any
  // future PresenterSlot registry (CommandQueue-owned table mapping a
  // PresentId → Presenter* with slot reuse + per-slot generation counter,
  // mirroring detail::HandleArena in dxmt9_resource_pool.hpp) must
  // preserve the ABA-safety invariants formally proven in
  // specs/verification/tla/PresentIdAba.tla (StaleResolvesNull,
  // NoCrossSlotAlias, GenerationMonotone, EventualReclaim). The TLA+ model
  // documents the assumption that the production generation domain
  // (24-bit in HandleArena, 32-bit in the forward-looking PresenterSlot
  // design) never wraps within the lifetime of any outstanding id.
  std::uint64_t submitPresent(const core::SwapDesc& desc);
  void presentBoundary(std::uint64_t presentSeqId, std::uint32_t maxFrameLatency);
  void deferPresentBoundary(std::uint64_t presentSeqId, std::uint32_t maxFrameLatency);
  void drainDeferredPresentBoundary();
  // Commit-replay offload present-ordinal boundary — mirrors presentBoundary
  // / deferPresentBoundary but paces on presentOrdinalGate_.completedOrdinal
  // (a count of retired presents) instead of a chunk seqId. Used by the
  // PE-side offload path via dxmt9::Device::waitPresentOrdinalBoundary,
  // once per present-bearing commit; the specific present this wait paces
  // skips submitPresent's own inline boundary via
  // core::SwapDesc::pacedByPresentOrdinal (R-BACK-2.51(g)), set only by that
  // same chunk-replay path. `backBufferCount` and `displaySyncEnabled` resolve
  // the effective latency the same way presentBoundaryLatency() resolves the
  // inline boundary (R-BACK-2.51(h), R-BACK-6.10; see
  // resolvedPresentFrameLatency()).
  void waitPresentOrdinalBoundary(std::uint64_t presentOrdinal, std::uint32_t maxFrameLatency,
                                  std::uint32_t backBufferCount, bool displaySyncEnabled);
  // Sticky abort for waitPresentOrdinalBoundary waiters. Set once by
  // ReplayOffloadWorker's fail-stop path (device_c_replay_offload.cpp) when
  // a deferred commit-replay failure means presentOrdinalGate_.completedOrdinal
  // can never advance again -- without this, an app thread already blocked in
  // waitPresentOrdinalBoundary (or one that arrives after the failure) would
  // wait forever in a release build, since DXMT_ASSERT does not abort
  // outside debug builds. Never cleared (there is no path back from a
  // failed offload worker on this device).
  void abortPresentOrdinalWaits();
  void submitFlush();
  // Cold WSI replacement fence. Unlike submitFlush(), this never records a
  // deferred flush against an active CPU-ready arena and then reports success.
  // The drained bridge entry must reach this operation with no active arena;
  // otherwise replacement fails closed and keeps the current Presenter/lease.
  // Complete leaves a short-lived gate armed until endWsiQuiescence().
  // That gate rejects new Present layer users and new CPU-ready arenas while
  // the caller swaps/unregisters the Presenter.
  wsi::QuiescenceDisposition beginWsiQuiescence() noexcept;
  // Terminal release waits out transient replacement/arena ownership and, if
  // the queue has stopped, joins its workers before Presenter destruction.
  wsi::QuiescenceDisposition beginFinalWsiQuiescence() noexcept;
  void endWsiQuiescence() noexcept;
  core::HResult waitForVBlank();

  // Per-swapchain Presenter registry. Each core::SwapChain registers its
  // Presenter on creation and unregisters on destruction; the queue
  // holds a non-owning observer pointer. The returned core::PresentId is
  // a queue-local opaque handle that survives across the PE/unix wire —
  // it is what travels on core::SwapDesc instead of the raw Presenter
  // pointer + shared_ptr<PresentDrawableToken>. Returning a zero
  // PresentId means the queue refused the binding (null pointer);
  // callers must check before forwarding.
  //
  // lookupPresenter returns nullptr for an invalid / stale (generation
  // mismatch) id; encode-side code must tolerate that path because a
  // SwapChain can be destroyed between submitPresent and the encode
  // worker draining the chunk.
  //
  // Drawable tokens from async / sync acquire-on-submit are stashed
  // against the same PresentId so the encode worker can claim them
  // without the queue carrying a shared_ptr on the PE-visible record.
  core::PresentId registerPresenter(Presenter* presenter) noexcept;
  void unregisterPresenter(core::PresentId id) noexcept;
  Presenter* lookupPresenter(core::PresentId id) const;
  void stashDrawableToken(core::PresentId id,
                          std::shared_ptr<PresentDrawableToken> token);
  std::shared_ptr<PresentDrawableToken> takeDrawableToken(core::PresentId id);

  // Queue-owned transfer paths. mapBuffer orchestrates Pool storage +
  // queue's wait-for-sequence rule under one mutex acquisition;
  // readbackSurface routes through encoders::readbackSurface using the
  // queue's own device + limits + pool refs.
  void* mapBuffer(core::BufferHandle handle, std::uint32_t flags);
  bool readbackSurface(const core::ReadbackDesc& desc, core::ReadbackPixels& pixels);

  // Command-buffer issuance. Callers that need a WMT::CommandBuffer
  // (encoders, transfers, readback) get an owning Reference via
  // newCommandBuffer; raw() exposes the non-owning handle view.
  WMT::Reference<WMT::CommandBuffer> newCommandBuffer();
  WMT::CommandQueue& raw() noexcept { return queueView_; }
  const WMT::CommandQueue& raw() const noexcept { return queueView_; }

  using TransientBufferSlice = transient::BufferSlice;

  TransientBufferSlice uploadTransientBuffer(std::span<const std::byte> bytes,
                                             std::size_t alignment,
                                             std::uint64_t seqId);
  TransientBufferSlice uploadTransientBufferWithCompletedSeqId(
      std::span<const std::byte> bytes,
      std::size_t alignment,
      std::uint64_t seqId,
      std::uint64_t completedSeqId);
  // Batched transient upload — single TransientResourceArena acquire +
  // single completedSeqId_ snapshot for N payloads. Returns one slice
  // per input payload in order. Each payload still gets its own
  // (offset, size) within the shared slab; the wins are amortized
  // mutex traffic + amortized completedSeqId snapshot. Used by the
  // encoder when processing a Kind::DrawRun (UP vertex/index data
  // across N draws batched into one call). Returns empty vector if
  // any allocation failed.
  std::vector<TransientBufferSlice> uploadTransientBufferBatch(
      std::span<const std::span<const std::byte>> payloads,
      std::size_t alignment,
      std::uint64_t seqId);
  std::vector<TransientBufferSlice> uploadTransientBufferBatchWithCompletedSeqId(
      std::span<const std::span<const std::byte>> payloads,
      std::size_t alignment,
      std::uint64_t seqId,
      std::uint64_t completedSeqId);

  // Retain an immutable sampler state until the chunk carrying an argument
  // buffer reference to it has completed. Direct encoder binds are retained by
  // Metal; argbuf descriptor writes only carry the resource id.
  void retainSamplerForSeq(WMT::Reference<WMT::SamplerState> sampler,
                           std::uint64_t seqId);

  // Reserved slab for arena-style writes — single TransientResourceArena
  // acquire reserves `size` bytes of contiguous transient memory and
  // returns both a binding-side slice (buffer + offset + size) and a
  // writable pointer the caller fills in directly. Lifetime tracking
  // is identical to uploadTransientBuffer/uploadTransientBufferBatch:
  // the slab is retained against `seqId` and reclaimed on the finish
  // path once the chunk's command buffer completes. Returns a
  // reservation with `contents == nullptr` on failure (caller falls
  // back to per-draw uploadTransientBuffer).
  using TransientBufferReservation = transient::BufferReservation;
  TransientBufferReservation reserveTransientBuffer(std::size_t size,
                                                    std::size_t alignment,
                                                    std::uint64_t seqId);
  TransientBufferReservation reserveTransientBufferWithCompletedSeqId(
      std::size_t size,
      std::size_t alignment,
      std::uint64_t seqId,
      std::uint64_t completedSeqId);

  resources::ReorderedIndexBufferLookup findReorderedIndexBuffer(
      core::Handle sourceHandle,
      resources::ReorderedIndexBufferCacheKey key,
      std::uint64_t seqId);

  bool rememberRejectedReorderedIndexBuffer(
      core::Handle sourceHandle,
      resources::ReorderedIndexBufferCacheKey key,
      std::uint64_t seqId);

  resources::ReorderedIndexBufferLookup getOrCreateReorderedIndexBuffer(
      core::Handle sourceHandle,
      resources::ReorderedIndexBufferCacheKey key,
      std::span<const std::uint8_t> bytes,
      std::uint64_t seqId);

  // The WMT::Device this queue was built on.
  WMT::Device device() const noexcept { return device_; }

  // Access the queue-owned per-frame scratch allocators. Exposed for
  // encoders::encodeChunk which places per-draw bindings on the
  // argument bump ring.
  scratch::FrameAllocators& allocators() noexcept { return allocators_; }

  // Queue-owned resource registries. DeviceImpl forwards its public
  // accessors to these — pool/pipeline-cache/shader-archive live here
  // in upstream-dxmt style.
  resources::Pool& pool() noexcept { return pool_; }
  pipeline::Cache& pipelineCache() noexcept { return pipelineCache_; }
  shaders::Archive& shaderArchive() noexcept { return shaderArchive_; }
  // R-BACK-12.22 / 12.24 — Stage 2 argbuf-hybrid encoder resource. Built
  // once at queue init from the device + descriptor table; stays
  // uninitialized when the capability gate fails so per-encoder
  // `openArgbuf` short-circuits to an empty handle and Stage 1 binding
  // stays the floor. The encoder is shared across every render pass on
  // this queue — it is stateless w.r.t. the argument-buffer storage; the
  // per-pass `setArgumentBuffer` call retargets it onto the new
  // transient slab.
  argbuf_hybrid::ArgbufEncoderResource& argbufEncoderResource() noexcept {
    return argbufEncoderResource_;
  }

  // R-BACK-12.22..12.26 (resource-array sub-mode) — the queue-owned SECOND
  // MTLArgumentEncoder, built from the extended 20-entry table only when
  // the resource-array opt-in is active (capability gate held AND
  // DXMT9_ARGBUF_RESOURCE_ARRAY set). Stays uninitialized otherwise, so the
  // constants-only encoder and the byte-identical default path are never
  // disturbed. `resourceArrayLaneActive()` is the single bool the encoder
  // reads per pass to choose this encoder + the resource-array PSO bit.
  argbuf_hybrid::ArgbufEncoderResource& resourceArrayEncoderResource() noexcept {
    return resourceArrayEncoderResource_;
  }
  bool resourceArrayLaneActive() const noexcept {
    return resourceArrayLaneActive_;
  }

  // R-BACK-15.4 / 15.5 / 15.6: touched color attachment set API. The
  // encoder's beginRenderPass calls isColorHandleTouched on each color
  // attachment to decide whether the first load action can be DontCare
  // (R-BACK-15.4); endEncoding marks each stored attachment via
  // markColorHandleTouched (R-BACK-15.6); surface-ops that overwrite the
  // handle call invalidateColorHandle (R-BACK-15.5).
  // clearAllTouchedColorHandles is reserved for queue resets / device
  // loss recovery. All four are invoked from the encoder thread only —
  // no mutex needed, matching currentBackBuffer_'s access pattern.
  bool isColorHandleTouched(core::Handle handle) const;
  void markColorHandleTouched(core::Handle handle);
  void invalidateColorHandle(core::Handle handle);
  void clearAllTouchedColorHandles();

  // Render Tape capture and provider replay must use the same deterministic
  // attachment policy. Their event/chunk boundaries are serialization
  // boundaries, not D3D discard points, so preserve first-use contents and
  // every live-out store while exact tape mode is active.
  void setRenderTapeExactAttachmentPreservation(bool enabled) noexcept {
    renderTapeExactAttachmentPreservation_.store(enabled,
                                                  std::memory_order_release);
  }
  bool renderTapeExactAttachmentPreservation() const noexcept {
    return renderTapeExactAttachmentPreservation_.load(
        std::memory_order_acquire);
  }

  // ─── Mostly-internal: worker-thread bodies + lifecycle binding ─────
  // Exposed so CommandQueue's constructor can wire its own runtime loops.
  // External callers should not use these.
  using EncodeChunkFn =
      std::function<std::optional<core::metalqueue::QueueSubmissionRecord>(
          const core::metalqueue::WorkerOwnedSourceSnapshot& source,
          const core::metalqueue::GenerationQualifiedSourceBorrow& borrow)>;
  using OnSubmittedFn = std::function<void(std::uint64_t completedSeqId)>;
  bool cpuReadySessionCoordinatorSelected() const noexcept;
  void runEncodeLoop(EncodeChunkFn encodeChunk, OnSubmittedFn onSubmitted);
  // Opt-in one-next-source proof window used by FrameGraph DCE. Holds at most
  // one dequeued source, never moves commands across chunks, and immediately
  // releases without proof when no successor is ready after prefix encode.
  void runDceChunkLookaheadEncodeLoop(OnSubmittedFn onSubmitted);
  // Tape-gated session join lane (DXMT9_CPU_READY_TAPE). Source-kind-neutral
  // FIFO session: compatible prefixes admit both Legacy ChunkSlot and Arena
  // Tape sources into one pending EncodeSession/open command buffer. Unlike
  // A parked pending session has no completion-wait or producer-quiescence
  // release; it finalizes only on ordered fences —
  // Present tail, non-appendable/semantic source, session-source cap or
  // preflight failure, producer sequence wait, initializer-wait boundary, and
  // shutdown drain. Admission and writer pressure never post a submission
  // fence. When a denied first lease and live Arena admission form a cycle,
  // the coordinator may execute one exact already-resident ordinary Direct
  // FIFO head serially; another escape requires capacity-generation progress.
  void runCpuReadySessionEncodeLoop(OnSubmittedFn onSubmitted);
  std::optional<core::metalqueue::QueueSubmissionRecord>
  encodeCpuReadySessionSource(
      const core::metalqueue::ResolvedPublishedSource& source,
      encoders::EncodeChunkOptions options);
  void runFinishLoop();
  void runCompletionWatcherLoop();
  void notePresentDequeued(std::uint64_t seqId);
  void noteSchedulingPresentDisposition(std::uint64_t seqId,
                                        bool published) noexcept;
  std::optional<core::metalcapture::MetalCaptureRequest> metalCaptureForPresentChunk(
      std::uint64_t seqId);
  // Multi-chunk capture: chunk-begin opens the session, present-chunk-close
  // returns the request whose `record.metalCapture` triggers stopCapture
  // at commit time. See `dxmt9_capture.hpp` for the controller-side model.
  std::optional<core::metalcapture::MetalCaptureRequest> metalCaptureForChunkBegin(
      std::uint64_t seqId);
  bool metalCaptureEnabled() const noexcept;
  std::optional<core::metalcapture::MetalCaptureRequest> notePresentChunkForCapture(
      std::uint64_t seqId);
  using ResolveSurfaceFlagsFn = std::function<std::uint32_t(core::Handle)>;
  void bindSelfLifecycle(ResolveSurfaceFlagsFn resolveSurfaceFlags);
  void advanceCpuReadyNextSourceIntentLocked(
      std::uint64_t sourceOrdinal,
      std::uint64_t seqId,
      std::size_t publishedSourceCount,
      std::uint64_t nextRawOrdinal,
      bool hasPresent) noexcept;
  void prefetchSlotPipelines(core::ChunkSlot& slot, bool seal = true);
  void startThreads(std::function<void()> encodeLoop,
                    std::function<void()> finishLoop,
                    std::function<void()> completionLoop);
  void stopThreads();

  // --- TLA-backed chunk-ring + sync state ----------------------------
  //
  // TLA+: QueueLifecycleRefinement
  //   nextSeqId_          -> nextSeqId
  //   completedSeqId_     -> completedSeqId
  //   lastCommittedSeqId_ -> lastCommittedSeqId
  //   slots_              -> slotState / slotSeqId / slotHasCommands
  //   writingSlot_        -> writingSlot
  //   writeIndex_         -> writeIndex
  //   inflightCount_      -> inflightCount
  //   cpuReadyTape_ Ready FIFO -> readySlots
  //   completedSeqQueue_  -> completedSeqQueue
  //
  // TLA+: PresentFrameLatency
  //   presentDequeuedSeqId_       -> encode progress diagnostic lane
  //   completedPresentSeqQueue_   -> command-completed present tokens
  //   presentCompletedSeqId_      -> presentCompletedSeqId
  //   presentOrdinalGate_.completedOrdinal -> completedPresentOrdinal (ordinal variant)
  //
  // These are raw-pointer-bound into queueLifecycle_ via bindSelfLifecycle.
  // Callers that need to read completedSeqId_ (e.g., DeviceImpl's
  // mapBuffer wait rule) treat them as read-only data guarded by mutex_.
  // R-BACK-43.4 `owner-published`. PUBLICATION MECHANISM: the queue-mutex
  // holder is the sole writer and publishes with a release store; every other
  // actor reads it lock-free through `markTicketAcquire()`'s acquire load and
  // must pair that read with the frozen-ticket re-stamp protocol
  // (`restampIfTicketAdvancedLocked`) before the mutex-protected step its
  // stamps have to cover. No thread-affinity assert: multi-reader by design,
  // and the argument below is what makes the read sound.
  //
  // Next seq to allocate. Atomic ONLY so the mark ticket can be read without
  // `mutex_` (design T2a/T2a'); every WRITE still happens under `mutex_` and
  // stays where it was — `QueueLifecycleController::commitCurrentChunk`'s
  // publish increment and `beginCpuReadyArenaSource`'s admission increment.
  //
  // Memory-order argument for the lock-free read (`markTicketAcquire()`):
  //   * The writers publish with `release`, so a reader that observes seq N
  //     also observes every slot mutation the publisher of N-1 made before it.
  //     The mark path does not depend on that, but the release keeps the
  //     store from being reordered ahead of the slot bookkeeping it seals.
  //   * The reader loads with `acquire`. A value read outside the mutex may be
  //     STALE (some publisher raised it after the load) but never invented and
  //     never regressed, because the variable is monotonically increasing and
  //     only ever written under one mutex. Staleness in the low direction is
  //     exactly the ticket/slot-seq race, and the re-stamp protocol — re-read
  //     under `mutex_`, re-stamp if it moved — is what closes it.
  //     TLA+: ProducerMarkReclaim!SlotAdvance / !WorkerRestamp, with
  //     `RestampDiscipline = "Removed"` as the executable counterexample.
  //   * A reader holding `mutex_` observes a frozen value, since every writer
  //     needs the same mutex. That is what makes the re-stamp a fixed point
  //     rather than another race.
  std::atomic<std::uint64_t> nextSeqId_{1};
  core::metalqueue::CpuReadyNextSourceIntent nextSourceIntent_{};
  std::uint64_t nextSourceIntentGeneration_ = 0;

  // TLA+: ProducerMarkReclaim — the ticket read in `WorkerBeginBatch` and
  // `BeginMark`. Callable with or without `mutex_`; see the memory-order
  // argument above for what each case guarantees.
  std::uint64_t markTicketAcquire() const noexcept {
    return nextSeqId_.load(std::memory_order_acquire);
  }

  // R-BACK-43.4 `owner-published` (design T2c; was `queue-shared`).
  // PUBLICATION MECHANISM: the completion loop is the sole writer — one site,
  // `QueueLifecycleController::drainCompletedSequence`'s monotone
  // `max(completedSeqId, seqId)` — and it still runs under `mutex_`, now with
  // a release store. Readers that hold `mutex_` load relaxed and get an exact
  // value, because every writer needs the same mutex. The map DISCARD fast
  // path (`CommandQueue::mapBuffer` when `mapWaitSeqId` returned 0) holds no
  // lock and loads acquire.
  //
  // Memory-order argument for the lock-free read
  // (`completedSeqIdAcquire()`), the same shape as `nextSeqId_` above:
  //   * The writer publishes with `release`, so a reader that observes seq N
  //     also observes the writes that PRECEDE the store — the completed-queue
  //     pop. Deliberately not more: the inflight decrement and the present
  //     watermark follow the store inside the same hold and are NOT ordered
  //     by it. That is sound because no lock-free reader reads them; the one
  //     lock-free consumer is `finalizeBufferMap`, which uses the value
  //     alone.
  //   * The reader loads with `acquire`. The value may be STALE — some
  //     completion raised it after the load — but never invented and never
  //     regressed, because the variable is monotone and has one writer. Stale
  //     in the low direction is the SAFE direction for every consumer here:
  //     `finalizeBufferMap` uses it to decide whether a rename-ring entry is
  //     idle, and a too-low watermark can only make it fresh-allocate a
  //     backing it could have reused. TLA+: ProducerMarkReclaim!MapFastRead,
  //     invariant `MapReadSound`.
  //   * It is NOT the reclaim gate's read side. `gcArena` /
  //     `Pool::reclaimCompleted` are driven from the finish loop with `mutex_`
  //     held, so the gate still sees the exact value.
  std::atomic<std::uint64_t> completedSeqId_{0};  // gpu-completed watermark

  // TLA+: ProducerMarkReclaim!MapFastRead — the T2c lock-free watermark read.
  std::uint64_t completedSeqIdAcquire() const noexcept {
    return completedSeqId_.load(std::memory_order_acquire);
  }
  // Exact read for callers that already hold `mutex_`; see the argument above
  // for why `relaxed` loses nothing there.
  std::uint64_t completedSeqIdLocked() const noexcept {
    return completedSeqId_.load(std::memory_order_relaxed);
  }

  std::uint64_t lastCommittedSeqId_ = 0;  // cpu-committed watermark
  std::uint64_t presentDequeuedSeqId_ = 0; // encode worker reached present
  std::uint64_t presentCompletedSeqId_ = 0; // present-bearing command buffer completed
  std::uint64_t deferredPresentBoundaryTargetSeqId_ = 0; // next-Present run-ahead boundary
  // Commit-replay offload present-ordinal frame-latency gate: owns the
  // retired-present watermark, the deferred-policy target, and the sticky
  // abort flag consulted by waitPresentOrdinalBoundary /
  // abortPresentOrdinalWaits (see those methods' docs). Guarded by mutex_
  // like the other present-ordinal state above it.
  PresentOrdinalGate presentOrdinalGate_{};

  core::CpuReadyTape cpuReadyTape_{
      core::CpuReadyTapeConfig::queueCompatibility(kCommandChunkCount)};
  // R-BACK-2.104: retained capacity follows the 64 persistent compatibility
  // payloads, not the 32 reusable control shells. Guarded by `mutex_`.
  core::QueueDirectSlotCapacityLeaseLedger directSlotCapacityLedger_{};
  std::array<core::ChunkSlotControl, kCommandChunkCount> slots_{};
  // Diagnostic-only residency timestamps for the current writing slot.
  // Set on the first command append and consumed when the slot is published;
  // not part of ChunkSlot so encode-side ReadySlotSnapshot refs stay narrow.
  std::array<std::uint64_t, kCommandChunkCount> slotFirstCommandSteadyNs_{};
  std::optional<size_t> writingSlot_{};
  // R-BACK-43.4 `worker-owned` BETWEEN EVENTS, with one documented exception.
  // The thread that (re-)established the writing slot via `ensureWritingSlot*`
  // owns the slot's contents until it is published; in the hot path that is
  // the replay offload worker's direct final-storage ingress. The EXCEPTION is
  // the producer's map-wait force-publish, which reaches the same slot through
  // `commitCurrentChunk` while holding `mutex_` — so the contract is "owner OR
  // holder of `mutex_`", which is why this is a shape-(c) token and why an
  // UNLOCKED append is unsafe without the T2d reserve-copy-commit protocol
  // (design doc §9, model obligation still open; gap.md "T2d"). Rebound at
  // `ensureWritingSlotUnlocked` and asserted at the append site with
  // `lock.owns_lock()` as the witness.
  [[no_unique_address]] core::ThreadOwnershipToken writingSlotOwnership_{
      core::deferThreadOwnership};
  size_t writeIndex_ = 0;
  size_t inflightCount_ = 0;
  std::deque<std::uint64_t> completedSeqQueue_{};
  std::deque<std::uint64_t> completedPresentSeqQueue_{};
  std::condition_variable presentDequeuedCv_{};

  // Last destination handle for a color-write. submitPresent only uses
  // this as a fallback when SwapDesc::sourceSurface is absent; normal
  // D3D9 presents carry the swapchain backbuffer explicitly. Also read
  // by encoders::beginRenderPass to decide whether a post-present
  // Discard load action is safe.
  core::Handle currentBackBuffer_{};
  bool backBufferDiscardAfterPresent_ = false;
  // R-BACK-15.4 / 15.6: queue-local set of color attachment handles that
  // were stored (StoreActionStore or MultisampleResolve) at least once in
  // this queue's lifetime. Encoder consults this on beginRenderPass to
  // decide whether the first load action can be DontCare. Invalidated by
  // surface ops that overwrite the handle (R-BACK-15.5). Sole reader/
  // writer is the encoder thread, so no separate mutex is needed (mirrors
  // currentBackBuffer_ access pattern).
  std::unordered_set<std::uint64_t> touchedColorHandles_{};
  std::atomic<bool> renderTapeExactAttachmentPreservation_{false};
  // Phase 14: see setSkipDrawResourceMarking() doc.
  bool skipDrawResourceMarking_ = false;
  // If a diagnostic record-side split fires while chunk replay has disabled
  // per-draw marking, subsequent draw-runs land under new seqIds that were
  // not covered by the importer's bulk markChunkResources() snapshot.
  bool forceDrawResourceMarkingAfterSplit_ = false;

  core::metalqueue::QueueLifecycleController queueLifecycle_{};
  std::unique_ptr<dxmt9::queue::PipelineLifecycleObserver>
      pipelineLifecycleObserver_{};
  // Resolved once during lifecycle binding so hot source-admission paths do
  // not make an unconditional out-of-line observer call when diagnostics are
  // disabled.
  bool pipelineLifecycleObservationEnabled_ = false;
  // Cold, opt-in diagnostics are held by the binary-local registry. They are
  // intentionally not queue-owned: several live devices must not overwrite
  // or dangle a process-wide installation.
  std::uint64_t copyMaterializationReportPresents_ = 0;
  SchedulingProgressWatchdog schedulingProgressWatchdog_{};
  core::metalhud::SubmissionDiagnosticsController submissionDiagnostics_{};

  // Thread-coordination primitives. Worker threads owned by *this
  // (see private section); encoders and DeviceImpl acquire mutex_
  // directly when they need to read chunk-ring state.
  std::mutex mutex_{};
  std::condition_variable writeCv_{};
  std::condition_variable encodeCv_{};
  std::condition_variable finishCv_{};
  std::condition_variable presentCompletedCv_{};
  std::condition_variable sessionReleaseCv_{};
  core::metalqueue::SessionReleaseState sessionReleaseState_{};
  std::uint64_t sessionReleaseCoveredRawOrdinal_ = 0;
  std::uint64_t sessionReleaseCoveredSeqId_ = 0;
  std::size_t cpuReadyCapacityWaiterCount_ = 0;
  // Native-only deterministic wait-entry observation. Production never sets
  // the gate; the counters let the production-loop fixture synchronize on the
  // two condition-variable boundaries without sleep or polling.
  bool testOnlySchedulingWaitObservationEnabled_ = false;
  std::uint64_t testOnlyArenaAdmissionWaitEntries_ = 0;
  std::uint64_t testOnlyArenaAdmissionPredicateEvaluations_ = 0;
  std::uint64_t testOnlyFirstLeaseWaitEntries_ = 0;
  std::uint64_t testOnlyIdleSessionWaitEntries_ = 0;
  bool testOnlyPauseAfterFirstLeaseRetry_ = false;
  bool testOnlyPausedAfterFirstLeaseRetry_ = false;
  bool stop_ = true;

 private:
  friend class resources::Initializer;
  friend struct CommandQueueArenaLeaseTestAccess;
  friend struct SchedulingProgressTestAccess;

  void noteInitializerPendingUploads() noexcept;
  void emitCopyMaterializationReport() const noexcept;
  void noteCopyMaterializationPublishedPresent() noexcept;
  void requestSchedulingStopLocked() noexcept;
  bool cpuReadyArenaControlSlotsFreeLocked(
      std::size_t requiredSlots) const noexcept;
  void notifySchedulingTerminalWaiters(
      render::SchedulingTerminalDisposition disposition) noexcept;

  struct ArenaBuildContext {
    ArenaBuildContext(core::CpuReadyTape::Reservation value,
                      std::size_t selectedControlIndex,
                      const core::ArenaSourcePayloadLayout& sourceLayout,
                      core::CpuReadyTape& tape,
                      core::Handle initialBackBuffer) noexcept
        : reservation(value),
          controlIndex(selectedControlIndex),
          layout(sourceLayout),
          initialBackBuffer(initialBackBuffer),
          ownerThread(std::this_thread::get_id()) {
      if (!initializeBuilders(tape)) {
        noteFailure(CpuReadyArenaFailureClass::BuilderInitialization);
        failed.store(true, std::memory_order_relaxed);
      }
    }

    ArenaBuildContext(
        const core::CpuReadyTape::ArenaBatchReservation& batch,
        std::span<const std::size_t> selectedControlIndices,
        std::span<const core::ArenaSourcePayloadLayout> sourceLayouts,
        core::CpuReadyTape& tape, core::Handle initialBackBuffer) noexcept
        : reservation(batch.reservations[0]),
          controlIndex(selectedControlIndices[0]),
          layout(sourceLayouts[0]),
          batchMode(true),
          sourceCount(batch.count),
          batchRawHighWaterBefore(batch.rawHighWaterBefore),
          batchSourceHighWaterBefore(batch.sourceHighWaterBefore),
          batchSeqHighWaterBefore(batch.seqHighWaterBefore),
          batchSourceTailBefore(batch.sourceTailBefore),
          batchPageTailBefore(batch.pageTailBefore),
          batchResidentCountBefore(batch.residentCountBefore),
          batchOccupiedPagesBefore(batch.occupiedPagesBefore),
          batchReadyCountBefore(batch.readyCountBefore),
          batchReadyReservationsBefore(batch.readyReservationsBefore),
          initialBackBuffer(initialBackBuffer),
          ownerThread(std::this_thread::get_id()) {
      for (std::size_t source = 0; source < sourceCount; ++source) {
        batchReservations[source] = batch.reservations[source];
        batchControlIndices[source] = selectedControlIndices[source];
        batchLayouts[source] = sourceLayouts[source];
      }
      if (!initializeBatchBuilders(tape)) {
        noteFailure(CpuReadyArenaFailureClass::BuilderInitialization);
        failed.store(true, std::memory_order_relaxed);
      }
    }

    void noteFailure(CpuReadyArenaFailureClass failureClass,
                     std::size_t source = std::numeric_limits<std::size_t>::max(),
                     std::size_t segment = std::numeric_limits<std::size_t>::max(),
                     std::size_t plannedPages = std::numeric_limits<std::size_t>::max(),
                     std::size_t actualCommands = std::numeric_limits<std::size_t>::max()) noexcept {
      bool expected = false;
      if (!firstFailureClaimed.compare_exchange_strong(
              expected, true, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        return;
      }
      firstFailureSource.store(static_cast<std::uint32_t>(source),
                               std::memory_order_relaxed);
      firstFailureSegment.store(static_cast<std::uint32_t>(segment),
                                std::memory_order_relaxed);
      firstFailurePlannedPages.store(static_cast<std::uint32_t>(plannedPages),
                                     std::memory_order_relaxed);
      firstFailureActualCommands.store(
          static_cast<std::uint32_t>(actualCommands),
          std::memory_order_relaxed);
      // Readers acquire the class only after all coordinates are complete.
      firstFailureClass.store(static_cast<std::uint8_t>(failureClass),
                              std::memory_order_release);
    }

    CpuReadyArenaFailureSnapshot firstFailure() const noexcept {
      return {
          .failureClass = static_cast<CpuReadyArenaFailureClass>(
              firstFailureClass.load(std::memory_order_acquire)),
          .source = firstFailureSource.load(std::memory_order_acquire),
          .segment = firstFailureSegment.load(std::memory_order_acquire),
          .plannedPages =
              firstFailurePlannedPages.load(std::memory_order_acquire),
          .actualCommands =
              firstFailureActualCommands.load(std::memory_order_acquire),
      };
    }

    std::size_t activeSourceIndex() const noexcept {
      return batchMode ? activeSource : 0u;
    }

    std::size_t activeSegmentIndex() const noexcept { return activeSegment; }

    std::size_t plannedActivePages() const noexcept {
      if (batchMode) {
        return activeSource < sourceCount &&
                activeSegment < batchLayouts[activeSource].segmentCount
            ? batchLayouts[activeSource].segments[activeSegment].layout.pageCount
            : std::numeric_limits<std::size_t>::max();
      }
      return activeSegment < layout.segmentCount
          ? layout.segments[activeSegment].layout.pageCount
          : std::numeric_limits<std::size_t>::max();
    }

    std::size_t actualActiveCommands() const noexcept {
      const auto* assembler = activeAssembler();
      return assembler ? assembler->commandCount()
                       : std::numeric_limits<std::size_t>::max();
    }

    bool initializeBuilders(core::CpuReadyTape& tape) noexcept {
      if (!layout.valid() ||
          reservation.arenaPayloadCount != layout.segmentCount) {
        return false;
      }
      for (std::size_t i = 0; i < layout.segmentCount; ++i) {
        auto memory = tape.writableArenaSegment(reservation.ticket, i);
        if (!reservation.arenaPayloads[i] ||
            memory.size() != layout.segments[i].layout.usedBytes) {
          return false;
        }
        builders[i].emplace(*reservation.arenaPayloads[i],
                            layout.segments[i].layout, memory);
        if (!builders[i]->good()) {
          return false;
        }
        assemblers[i].emplace(*builders[i]);
        const auto& ticket = reservation.ticket;
        if (!assemblers[i]->bindOuter(
                core::TransactionalChunkSlotAssembler::OuterBinding{
                    .rawOrdinal = ticket.rawOrdinal,
                    .sourceOrdinal = ticket.sourceOrdinal,
                    .seqId = ticket.seqId,
                    .buildGeneration = ticket.buildGeneration,
                    .sourceGeneration = ticket.id.generation,
                    .storageGeneration = ticket.storage.generation,
                    .controlIndex = static_cast<std::uint32_t>(controlIndex),
                    .firstPage = ticket.storage.firstPage,
                    .pageCount = ticket.storage.pageCount,
                    .segmentIndex = static_cast<std::uint32_t>(i),
                    .segmentCount = static_cast<std::uint32_t>(layout.segmentCount),
                    .plannedBytes = layout.segments[i].layout.usedBytes,
                })) {
          return false;
        }
      }
      return true;
    }

    core::ArenaSourcePayloadAssembler* activeAssembler() noexcept {
      if (batchMode) {
        return activeSource < sourceCount &&
                activeSegment < batchLayouts[activeSource].segmentCount &&
                batchAssemblers[activeSource][activeSegment]
            ? &*batchAssemblers[activeSource][activeSegment]
            : nullptr;
      }
      return activeSegment < layout.segmentCount && assemblers[activeSegment]
          ? &*assemblers[activeSegment]
          : nullptr;
    }

    const core::ArenaSourcePayloadAssembler* activeAssembler() const noexcept {
      if (batchMode) {
        return activeSource < sourceCount &&
                activeSegment < batchLayouts[activeSource].segmentCount &&
                batchAssemblers[activeSource][activeSegment]
            ? &*batchAssemblers[activeSource][activeSegment]
            : nullptr;
      }
      return activeSegment < layout.segmentCount && assemblers[activeSegment]
          ? &*assemblers[activeSegment]
          : nullptr;
    }

    bool initializeBatchBuilders(core::CpuReadyTape& tape) noexcept {
      if (sourceCount == 0 || sourceCount > batchReservations.size()) {
        return false;
      }
      for (std::size_t source = 0; source < sourceCount; ++source) {
        const auto& sourceLayout = batchLayouts[source];
        const auto& sourceReservation = batchReservations[source];
        if (!sourceLayout.valid() ||
            sourceReservation.arenaPayloadCount != sourceLayout.segmentCount) {
          return false;
        }
        for (std::size_t segment = 0; segment < sourceLayout.segmentCount;
             ++segment) {
          auto memory = tape.writableArenaSegment(sourceReservation.ticket,
                                                  segment);
          if (!sourceReservation.arenaPayloads[segment] ||
              memory.size() != sourceLayout.segments[segment].layout.usedBytes) {
            return false;
          }
          batchBuilders[source][segment].emplace(
              *sourceReservation.arenaPayloads[segment],
              sourceLayout.segments[segment].layout, memory);
          if (!batchBuilders[source][segment]->good()) {
            return false;
          }
          batchAssemblers[source][segment].emplace(
              *batchBuilders[source][segment]);
          const auto& ticket = sourceReservation.ticket;
          if (!batchAssemblers[source][segment]->bindOuter(
                  core::TransactionalChunkSlotAssembler::OuterBinding{
                      .rawOrdinal = ticket.rawOrdinal,
                      .sourceOrdinal = ticket.sourceOrdinal,
                      .seqId = ticket.seqId,
                      .buildGeneration = ticket.buildGeneration,
                      .sourceGeneration = ticket.id.generation,
                      .storageGeneration = ticket.storage.generation,
                      .controlIndex = static_cast<std::uint32_t>(
                          batchControlIndices[source]),
                      .firstPage = ticket.storage.firstPage,
                      .pageCount = ticket.storage.pageCount,
                      .segmentIndex = static_cast<std::uint32_t>(segment),
                      .segmentCount = static_cast<std::uint32_t>(
                          sourceLayout.segmentCount),
                      .plannedBytes =
                          sourceLayout.segments[segment].layout.usedBytes,
                  })) {
            return false;
          }
        }
      }
      return true;
    }

    core::ArenaSourcePayloadBuilder* activeBuilder() noexcept {
      if (batchMode) {
        return activeSource < sourceCount &&
                activeSegment < batchLayouts[activeSource].segmentCount &&
                batchBuilders[activeSource][activeSegment]
            ? &*batchBuilders[activeSource][activeSegment]
            : nullptr;
      }
      return activeSegment < layout.segmentCount && builders[activeSegment]
          ? &*builders[activeSegment]
          : nullptr;
    }

    core::CpuReadyTape::Reservation reservation{};
    std::size_t controlIndex = std::numeric_limits<std::size_t>::max();
    core::ArenaSourcePayloadLayout layout{};
    std::array<std::optional<core::ArenaSourcePayloadBuilder>,
               core::kMaxArenaSourcePayloadSegments> builders{};
    std::array<std::optional<core::ArenaSourcePayloadAssembler>,
               core::kMaxArenaSourcePayloadSegments> assemblers{};
    std::size_t activeSegment = 0;
    bool hasSelectedSegment = false;
    bool batchMode = false;
    std::size_t sourceCount = 1;
    std::size_t activeSource = 0;
    std::uint64_t batchRawHighWaterBefore = 0;
    std::uint64_t batchSourceHighWaterBefore = 0;
    std::uint64_t batchSeqHighWaterBefore = 0;
    std::size_t batchSourceTailBefore = 0;
    std::size_t batchPageTailBefore = 0;
    std::size_t batchResidentCountBefore = 0;
    std::size_t batchOccupiedPagesBefore = 0;
    std::size_t batchReadyCountBefore = 0;
    std::size_t batchReadyReservationsBefore = 0;
    std::array<core::CpuReadyTape::Reservation,
               core::CpuReadyTape::kMaxArenaBatchSources>
        batchReservations{};
    std::array<std::size_t, core::CpuReadyTape::kMaxArenaBatchSources>
        batchControlIndices{};
    std::array<core::ArenaSourcePayloadLayout,
               core::CpuReadyTape::kMaxArenaBatchSources>
        batchLayouts{};
    std::array<
        std::array<std::optional<core::ArenaSourcePayloadBuilder>,
                   core::kMaxArenaSourcePayloadSegments>,
        core::CpuReadyTape::kMaxArenaBatchSources>
        batchBuilders{};
    std::array<
        std::array<std::optional<core::ArenaSourcePayloadAssembler>,
                   core::kMaxArenaSourcePayloadSegments>,
        core::CpuReadyTape::kMaxArenaBatchSources>
        batchAssemblers{};
    core::Handle initialBackBuffer{};
    std::thread::id ownerThread{};
    std::atomic<bool> failed{false};
    std::atomic<bool> publishing{false};
    std::atomic<bool> firstFailureClaimed{false};
    std::atomic<std::uint8_t> firstFailureClass{
        static_cast<std::uint8_t>(CpuReadyArenaFailureClass::None)};
    std::atomic<std::uint32_t> firstFailureSource{
        std::numeric_limits<std::uint32_t>::max()};
    std::atomic<std::uint32_t> firstFailureSegment{
        std::numeric_limits<std::uint32_t>::max()};
    std::atomic<std::uint32_t> firstFailurePlannedPages{
        std::numeric_limits<std::uint32_t>::max()};
    std::atomic<std::uint32_t> firstFailureActualCommands{
        std::numeric_limits<std::uint32_t>::max()};
    core::Handle pendingBackBuffer{};
    bool updatesBackBuffer = false;
    // Before Ready publication the Presenter registry owns any stashed token,
    // while this context owns the sole obligation to remove it on abort.
    // Successful publication transfers that cleanup obligation to encoded
    // Present consumption; abortCpuReadyArenaSource is the only failure-side
    // takeDrawableToken site.
    core::PresentId pendingPresentId{};
    core::SwapDesc pendingPresentDesc{};
    BoundaryPolicy pendingPresentBoundaryPolicy = BoundaryPolicy::Disabled;
    bool presentAppended = false;
    bool presentTokenStashed = false;
    bool flushAfterPublication = false;
    // Staged only after successful semantic replay. Publication turns this
    // already-adopted replay-FIFO fact into the globally visible intent in
    // the same scheduling-lock transaction that makes this source Ready.
    std::uint64_t nextQueuedRawOrdinalHint = 0;
    struct CaptureCommandAnchor {
      std::uint32_t firstRecord = 0;
      std::uint32_t lastRecord = 0;
    };
    std::uint32_t captureRecordCount = 0;
    bool captureIdentityBegun = false;
    std::size_t captureSourceRangeCount = 0;
    std::array<std::uint32_t, core::CpuReadyTape::kMaxArenaBatchSources>
        captureSourceFirstRecords{};
    std::array<std::uint32_t, core::CpuReadyTape::kMaxArenaBatchSources>
        captureSourceRecordCounts{};
    std::vector<std::uint32_t> captureNextRawRecords{};
    std::vector<CaptureCommandAnchor> captureCommandAnchors{};

    bool captureEnabled() const noexcept { return captureRecordCount != 0; }
    bool setCaptureNextRawRecords(
        std::span<const std::uint32_t> records) noexcept;
    bool captureSingleCommand() noexcept;
    bool captureDrawCommand(std::size_t first,
                            std::size_t count) noexcept;
    bool captureDirectDraw(bool appendedCommand) noexcept;
  };

  struct DirectChunkSlotBuildContext {
    std::thread::id ownerThread{};
    core::CpuReadyPublicationTicket ticket{};
    std::size_t controlIndex = std::numeric_limits<std::size_t>::max();
    std::optional<core::TransactionalChunkSlotAssembler> assembler{};
    core::Handle initialBackBuffer{};
    core::Handle pendingBackBuffer{};
    bool updatesBackBuffer = false;
    bool failed = false;
    bool effectsStarted = false;
    // True only when admission proved that this source can append to an
    // already populated slot without relocating or rebuilding its prefix.
    bool continuation = false;
    // A nonzero value binds the complete immutable imported range to this
    // lease. replayResolvedChunk returns only after consuming that complete
    // range; prepare/commit verify the bound once, rather than synchronizing
    // with the queue for every record.
    std::size_t expectedRangeRecordCount = 0;
    // Lease-span identity of the raw owning this transaction. `spanOrdinal`
    // is 0-based within the raw and `finalSpan` marks its last span; commit
    // presents both to the Tape witness so only the immediate successor span
    // of the exact active raw may extend a populated slot.
    std::uint32_t spanOrdinal = 0;
    bool finalSpan = true;
    // Destination command-header capacity observed at admission. R-BACK-2.86
    // forbids reallocating an earlier final extent, so a different capacity at
    // commit means an append grew reserved storage inside the transaction.
    std::size_t reservedCommandHeaderCapacity = 0;
    // Present is staged by submitPresent, but remains coordinator-owned until
    // direct assembler evidence has committed the same final ChunkSlot.
    core::PresentId pendingPresentId{};
    core::SwapDesc pendingPresentDesc{};
    core::Handle pendingPresentSource{};
    BoundaryPolicy pendingPresentBoundaryPolicy = BoundaryPolicy::Disabled;
    bool presentAppended = false;
    bool presentTokenStashed = false;
  };

  enum class ActiveArenaAppendResult {
    Inactive,
    Appended,
    Failed,
  };

  template <typename Append>
  ActiveArenaAppendResult appendActiveArena(Append&& append) noexcept {
    if (!arenaAdmissionActive_.load(std::memory_order_acquire)) {
      return ActiveArenaAppendResult::Inactive;
    }
    auto* context = activeArenaBuild_.load(std::memory_order_acquire);
    // Structural admission corruption cannot be recovered by the batch
    // capability: a missing context, wrong owner, or concurrent publication
    // poisons the queue and fails closed even when a batch was requested.
    if (!context || context->ownerThread != std::this_thread::get_id() ||
        context->publishing.load(std::memory_order_acquire)) {
      if (context) {
        context->noteFailure(
            CpuReadyArenaFailureClass::ContextInvalid,
            context->activeSourceIndex(), context->activeSegmentIndex(),
            context->plannedActivePages(), context->actualActiveCommands());
        context->failed.store(true, std::memory_order_release);
      }
      arenaBuildPoisoned_.store(true, std::memory_order_release);
      return ActiveArenaAppendResult::Failed;
    }
    const bool batch = context->batchMode;
    if (context->failed.load(std::memory_order_acquire) ||
        !append(*context)) {
      context->noteFailure(
          CpuReadyArenaFailureClass::Append, context->activeSourceIndex(),
          context->activeSegmentIndex(), context->plannedActivePages(),
          context->actualActiveCommands());
      context->failed.store(true, std::memory_order_release);
      // SegmentSerial owns a whole-event rollback capability.  Keep the
      // queue healthy until publishCpuReadyArenaBatch performs the guarded
      // rollback; poisoning here would turn a pre-effect append rejection
      // into an unrecoverable device loss and prevent EventSerial retry.
      if (!batch) {
        arenaBuildPoisoned_.store(true, std::memory_order_release);
      }
      return ActiveArenaAppendResult::Failed;
    }
    return ActiveArenaAppendResult::Appended;
  }

  template <typename Append>
  ActiveArenaAppendResult appendActiveDirectChunkSlot(
      Append&& append) noexcept {
    auto* context = activeDirectChunkSlotBuild_.load(
        std::memory_order_acquire);
    if (!context) {
      return ActiveArenaAppendResult::Inactive;
    }
    std::lock_guard lock(mutex_);
    if (!directChunkSlotBuildContext_ ||
        &*directChunkSlotBuildContext_ != context ||
        context->ownerThread != std::this_thread::get_id() ||
        context->failed || context->controlIndex >= slots_.size() ||
        !writingSlot_ || *writingSlot_ != context->controlIndex) {
      if (directChunkSlotBuildContext_ &&
          &*directChunkSlotBuildContext_ == context) {
        context->failed = true;
      }
      return ActiveArenaAppendResult::Failed;
    }
    auto& control = slots_[context->controlIndex];
    auto* payload = cpuReadyTape_.resolveForWrite(
        core::CpuReadyPublicationTicket{
            .id = control.sourceId,
            .storage = control.storage,
        });
    if (control.state != core::ChunkSlot::State::Writing ||
        control.sourceId != context->ticket.id ||
        control.storage != context->ticket.storage ||
        payload != control.payload || !context->assembler ||
        !append(*context, *context->assembler)) {
      context->failed = true;
      return ActiveArenaAppendResult::Failed;
    }
    return ActiveArenaAppendResult::Appended;
  }

  ActiveArenaAppendResult appendActiveArenaDrawRun(
      core::CanonicalDrawState& state,
      const core::DrawUniformPayload& uniforms,
      std::span<const core::DrawParam> draws,
      std::span<const core::DrawParamPayloadView> payloads) noexcept;
  ActiveArenaAppendResult appendActiveArenaDirectReplayDraw(
      const core::DirectReplayDrawInput& input) noexcept;
  ActiveArenaAppendResult appendActiveArenaClear(
      const core::ClearDesc& value) noexcept;
  ActiveArenaAppendResult appendActiveArenaSurfaceCopy(
      const core::SurfaceCopyDesc& value) noexcept;
  ActiveArenaAppendResult appendActiveArenaStretchRect(
      const core::StretchRectDesc& value) noexcept;
  ActiveArenaAppendResult appendActiveArenaColorFill(
      const core::ColorFillDesc& value) noexcept;
  ActiveArenaAppendResult appendActiveArenaDepthResolve(
      const core::DepthResolveDesc& value) noexcept;
  ActiveArenaAppendResult appendActiveArenaGenerateMipmaps(
      const core::GenerateMipmapsDesc& value) noexcept;
  ActiveArenaAppendResult appendActiveArenaPresent(
      core::SwapDesc value, BoundaryPolicy boundaryPolicy,
      bool tokenStashed) noexcept;
  ActiveArenaAppendResult deferActiveArenaFlush() noexcept;
  ActiveArenaAppendResult rejectIfActiveArena() noexcept;
  // Thread-pinned borrowed coordinator append over the *queue-owned* build
  // context. The RAII lease is move-constructed out of an optional by its
  // caller, so its address is not stable and must never be published; the
  // context in `directChunkSlotBuildContext_` is queue-owned, non-moving for
  // the whole transaction, and already published through
  // `activeDirectChunkSlotBuild_`. The lease stays the settlement authority
  // (commit / rollback / fail-stop); this is only the append seam.
  struct BorrowedDirectChunkSlotAppend {
    DirectChunkSlotBuildContext* context = nullptr;
    core::TransactionalChunkSlotAssembler* assembler = nullptr;

    explicit operator bool() const noexcept {
      return context != nullptr && assembler != nullptr;
    }
    // Poison the transaction after a rejected append, mirroring the borrowed
    // draw appender. Returns false so callers can `return poison();`.
    bool poison() const noexcept {
      if (context) context->failed = true;
      return false;
    }
  };

  // Non-null only on the transaction's owning thread, which is its sole
  // writer. That is what lets this skip the queue mutex and the slot
  // revalidation the generic append below performs: no other thread may
  // change the writing slot while a direct build is active, and the
  // destination is pre-reserved so no append can grow it.
  BorrowedDirectChunkSlotAppend borrowedDirectChunkSlotAppend() noexcept {
    auto* context =
        activeDirectChunkSlotBuild_.load(std::memory_order_acquire);
    if (!context || context->ownerThread != std::this_thread::get_id() ||
        context->failed || !context->assembler ||
        !context->assembler->good()) {
      return {};
    }
    return {context, &*context->assembler};
  }
  core::DirectReplayDrawDisposition appendActiveDirectChunkSlotDraw(
      const core::DirectReplayDrawInput& input) noexcept;
  ActiveArenaAppendResult appendActiveDirectChunkSlotClear(
      const core::ClearDesc& value) noexcept;
  ActiveArenaAppendResult appendActiveDirectChunkSlotSurfaceCopy(
      const core::SurfaceCopyDesc& value) noexcept;
  ActiveArenaAppendResult appendActiveDirectChunkSlotStretchRect(
      const core::StretchRectDesc& value) noexcept;
  ActiveArenaAppendResult appendActiveDirectChunkSlotColorFill(
      const core::ColorFillDesc& value) noexcept;
  ActiveArenaAppendResult appendActiveDirectChunkSlotDepthResolve(
      const core::DepthResolveDesc& value) noexcept;
  ActiveArenaAppendResult appendActiveDirectChunkSlotGenerateMipmaps(
      const core::GenerateMipmapsDesc& value) noexcept;
  ActiveArenaAppendResult appendActiveDirectChunkSlotPresent(
      core::SwapDesc value, BoundaryPolicy boundaryPolicy,
      bool tokenStashed) noexcept;
  std::optional<std::uint64_t>
  validateActiveDirectChunkSlotPresent() noexcept;
  DirectChunkSlotReplayStatus commitDirectChunkSlotReplay(
      core::CpuReadyPublicationTicket ticket, std::size_t controlIndex,
      std::span<const core::ChunkHandleEntry> resources) noexcept;
  bool abortDirectChunkSlotReplay(
      core::CpuReadyPublicationTicket ticket, std::size_t controlIndex,
      bool failStop) noexcept;
  bool waitEnterWsiPresentUse() noexcept;
  void leaveWsiPresentUse() noexcept;
  bool selectCpuReadyArenaSegment(
      core::CpuReadyPublicationTicket ticket,
      std::size_t controlIndex,
      std::size_t segmentIndex) noexcept;
  bool selectCpuReadyArenaSourceSegment(
      core::CpuReadyPublicationTicket ticket,
      std::size_t controlIndex, std::size_t sourceIndex,
      std::size_t segmentIndex) noexcept;
  CpuReadyArenaPublishStatus publishCpuReadyArenaBatch(
      core::CpuReadyPublicationTicket ticket,
      std::span<const core::ChunkHandleEntry> resources,
      CpuReadyCaptureIdentityBatch* captureIdentity = nullptr) noexcept;
 public:
  // Capture-only fence: returns only after the exact raw/generation group
  // tail has been consumed by QueueLifecycle's completion settlement ledger.
  bool waitForCpuReadyEventSettlement(
      std::uint64_t rawOrdinal, std::uint64_t buildGeneration,
      std::uint64_t firstSourceOrdinal, std::uint64_t tailSeqId,
      std::uint32_t sourceCount) noexcept;
 private:
  CpuReadyArenaBatchAbortStatus abortCpuReadyArenaBatch(
      core::CpuReadyPublicationTicket ticket,
      bool failStop = true) noexcept;
  bool publishCpuReadyArenaSource(
      core::CpuReadyPublicationTicket ticket,
      std::size_t controlIndex,
      std::span<const core::ChunkHandleEntry> resources,
      CpuReadyCaptureIdentity* captureIdentity) noexcept;
  void abortCpuReadyArenaSource(
      core::CpuReadyPublicationTicket ticket,
      std::size_t controlIndex) noexcept;

  std::optional<ArenaBuildContext> arenaBuildContext_{};
  CpuReadyArenaFailureSnapshot lastCpuReadyArenaFailure_{};
  std::atomic<ArenaBuildContext*> activeArenaBuild_{nullptr};
  std::atomic<bool> arenaAdmissionActive_{false};
  std::atomic<bool> arenaBuildPoisoned_{false};
  std::optional<DirectChunkSlotBuildContext> directChunkSlotBuildContext_{};
  std::atomic<DirectChunkSlotBuildContext*> activeDirectChunkSlotBuild_{
      nullptr};
  std::uint64_t nextDirectChunkSlotBuildGeneration_ = 1;
  std::atomic<std::uint32_t> arenaAdmissionWaiterCount_{0};
  std::uint64_t nextArenaBuildGeneration_ = 1;
  std::atomic<bool> wsiQuiescenceActive_{false};
  std::atomic<std::uint32_t> wsiPresentUsers_{0};
  std::atomic<bool> wsiAdmissionStopped_{true};
  std::mutex wsiQuiescenceMutex_{};
  std::condition_variable wsiQuiescenceCv_{};
  // Native coordinator fault seam: fail one post-reservation semantic
  // preflight, restore the exact tentative prefix, then return from the
  // manually-driven encode loop. Never set by production.
  bool testOnlyRestoreNextCpuReadySessionPreflight_ = false;
  // Native observability pin: make the compatibility-writing-slot flush
  // return a typed Stopped result before arena reserve. Never set by
  // production callers.
  bool testOnlyStopCpuReadyArenaAfterCompatibilityFlush_ = false;
  // Native rollback pin: force the pre-effect builder rejection seam and
  // perturb nextSeqId_ so the guarded abort must fail-stop, never recover.
  bool testOnlyForceNextCpuReadyArenaRollbackFailure_ = false;
  // Native routing pin: fail one batch builder before any semantic replay so
  // the production caller must take the complete EventSerial retry exactly
  // once. Never set by production.
  bool testOnlyForceNextCpuReadyArenaBuilderFailure_ = false;
  // Native routing pin: make beginCaptureIdentity fail after ranges are
  // predeclared, before replay effects. Never set by production.
  bool testOnlyForceNextCpuReadyArenaCaptureIdentityBeginFailure_ = false;
  // Native routing pin: make batch publication recoverably fail after
  // replayResolvedChunk has run, proving the caller fail-stops instead of
  // recursively replaying semantic effects. Never set by production.
  bool testOnlyForceNextCpuReadyArenaPostSemanticPublishFailure_ = false;
  // Native-only deterministic fault seams for the complete transactional
  // arena lifecycle. These are never armed by production callers.
  bool testOnlyForceNextCpuReadyArenaCapacityFailure_ = false;
  bool testOnlyForceNextCpuReadyArenaValidationFailure_ = false;
  bool testOnlyForceNextCpuReadyArenaResourceRetainFailure_ = false;
  bool testOnlyForceNextCpuReadyArenaPublicationFailure_ = false;
  bool testOnlyForceNextDirectChunkSlotCommitFailure_ = false;
  // Native planner seam: tests may observe one event-wide planner result or
  // force that result invalid before any Ready/resource effect. These fields
  // are never armed by production callers.
  bool testOnlyObserveNextCpuReadyArenaPlanner_ = false;
  std::uint32_t testOnlyCpuReadyArenaPlannerInvocationCount_ = 0;
  bool testOnlyCpuReadyArenaPlannerValid_ = false;
  bool testOnlyForceNextCpuReadyArenaPlannerInvalid_ = false;
  bool testOnlyPauseAfterStaleMultiSourcePlannerRestore_ = false;
  bool testOnlyPausedAfterStaleMultiSourcePlannerRestore_ = false;
  bool testOnlyOverrideLiveActiveRenderInstance_ = false;
  std::uint64_t testOnlyLiveActiveRenderSeqId_ = 0;
  std::uint64_t testOnlyLiveActiveRenderEncoderIndex_ = 0;
  bool testOnlyPauseAfterNextSessionReleaseAck_ = false;
  bool testOnlyPausedAfterSessionReleaseAck_ = false;
  // Native lifecycle specs may keep recorder-suppressed fake encoder handles
  // alive across coordinator-created EncodeContexts, including finalization.
  encoders::EncodeDrawRecorder* testOnlyDrawRecorder_ = nullptr;

  // Assemble the EncodeContext handed to encoders::encodeChunk. Uses
  // queue-owned state only (device_, limits_, allocators_, pool_,
  // pipelineCache_, shaderArchive_, *this). No upper-Device pointer —
  // presentation back-channels ride on SwapDesc per submission.
  encoders::EncodeContext makeEncodeContext();
  void applyPublishedPresentBoundary(std::uint64_t presentSeqId,
                                     const core::SwapDesc& desc,
                                     BoundaryPolicy policy);

  WMT::Device device_{};
  bool cpuReadySessionLaneEnabled_ = false;
  render::RenderPartitionConfig renderPartitionConfig_{};
  render::EncodeExecutionTopology encodeExecutionTopology_ =
      render::kStableOwnedRawSlotTopology;
  WMT::Reference<WMT::CommandQueue> queue_{};
  WMT::CommandQueue queueView_{};  // non-owning view of queue_

  // R-BACK-31.7 — CommandQueue owns the render backend. Declared before the
  // worker-thread members so it is constructed first (the ctor assigns it
  // before startThreads()) and, by reverse declaration order, destroyed after
  // them. The encode lambda calls backend_->onChunkReady(...), so backend_
  // MUST outlive the encode thread; the dtor joins threads via stopThreads()
  // in its body, before any member destruction runs, so the lifetime is safe
  // either way and this ordering documents the intent.
  std::unique_ptr<render::IRenderBackend> backend_;
  core::metalqueue::ReplayObserverSink replayObserver_{};

  std::thread encodeThread_{};
  std::thread finishThread_{};
  std::thread completionThread_{};
  bool threadsStarted_ = false;

  // Queue-owned runtime node state. Pool / cache / archive / limits
  // all live HERE (matches upstream dxmt's CommandQueue). Order matters:
  // pool_ must be constructed before initializer_ (which borrows it).
  // All queue-owned state outlives the worker threads because threads
  // are joined first via stopThreads() in ~CommandQueue.
  core::BackendLimits limits_{};
  scratch::FrameAllocators allocators_{};
  resources::Pool pool_{};
  pipeline::Cache pipelineCache_{};
  shaders::Archive shaderArchive_{};
  // R-BACK-12.22 / 12.24 — see argbufEncoderResource() accessor above.
  // Lifetime: WMT::Reference inside owns the encoder; default ctor leaves
  // it uninitialized (any sentinel-null device skips init() in the queue
  // ctor). Order matters — must outlive the worker threads that may
  // observe it via encoders::EncodeContext.
  argbuf_hybrid::ArgbufEncoderResource argbufEncoderResource_{};
  // R-BACK-12.22..12.26 (resource-array sub-mode) — second encoder + lane
  // flag. resourceArrayEncoderResource_ is built from the 20-entry table
  // only when resourceArrayLaneActive_ holds (set in the ctor from the
  // capability gate AND DXMT9_ARGBUF_RESOURCE_ARRAY). Both default to a
  // disabled/uninitialized state so the constants-only path is untouched.
  argbuf_hybrid::ArgbufEncoderResource resourceArrayEncoderResource_{};
  bool resourceArrayLaneActive_ = false;
  std::unique_ptr<resources::Initializer> initializer_;
  core::metalcapture::MetalCaptureController metalCapture_{};

  // SeqId-scoped transient resource owner: shared slabs, dedicated transient
  // buffers, argbuf reservations, and argbuf sampler retention all reclaim
  // through ResourceArena::reclaim(completedSeqId).
  transient::ResourceArena transientArena_{};

  // C1 chunk-record-import dirty accumulator. The d3d9 chunk-record
  // dispatcher (device_c_device_state_draw.cpp commit_chunk) calls
  // applyDirty* below as it processes records; makeEncodeContext()
  // snapshots and resets this field into the freshly-built
  // EncodeContext::dirty so C2 (encoder consumption) reads exactly the
  // bits accumulated since the last encode call. Guarded by
  // dirtyMutex_ — the chunk dispatcher and the encode worker can
  // race during steady-state operation.
  std::mutex dirtyMutex_{};
  uniform::DirtyState pendingDirty_{};

  // Per-swapchain Presenter registry. SwapChain owns the Presenter
  // (unique_ptr); the queue holds a non-owning observer pointer plus a
  // generation counter so a stale PresentId from a torn-down swapchain
  // resolves to nullptr (the encoder then skips the present cleanly).
  // The encoded id is `(generation << 32) | (slot + 1)` so a zero value
  // is never a real binding. Free slots form a stack via
  // PresenterSlot::nextFree. Drawable tokens from acquire-on-submit are
  // stashed here keyed by id so the encode worker reclaims them without
  // a shared_ptr field on the PE-visible SwapDesc.
  struct PresenterSlot {
    Presenter* presenter = nullptr;
    std::uint32_t generation = 0;
    std::int32_t nextFree = -1;
    detail::SingleUseTokenSlot<PresentDrawableToken> pendingToken{};
  };
  static constexpr std::uint64_t encodePresentId(std::uint32_t slotIndex,
                                                 std::uint32_t generation) noexcept {
    return (static_cast<std::uint64_t>(generation) << 32) |
           (static_cast<std::uint64_t>(slotIndex) + 1ull);
  }
  static constexpr std::uint32_t decodePresentIdSlot(core::PresentId id) noexcept {
    return static_cast<std::uint32_t>((id.value & 0xffffffffull) - 1ull);
  }
  static constexpr std::uint32_t decodePresentIdGeneration(core::PresentId id) noexcept {
    return static_cast<std::uint32_t>(id.value >> 32);
  }
  mutable std::mutex presenterRegistryMutex_{};
  std::vector<PresenterSlot> presenterSlots_{};
  std::int32_t presenterFreeHead_ = -1;
  // Native cold-boundary fault seam: fail one registry allocation without
  // mutating the current Presenter binding.
  bool testOnlyFailNextPresenterRegistration_ = false;

 public:
  // Limits accessor — value member, not a borrowed pointer.
  const core::BackendLimits& limits() const noexcept { return limits_; }

  // C1 dirty-marking surface — used by the chunk-record import path in
  // device_c_device_state_draw.cpp. Each method ORs the matching bit
  // and bumps the high-water counter (for the constant-set variants).
  // Mutex-guarded so the importer thread can call these concurrently
  // with the encode worker draining pendingDirty_ via makeEncodeContext.
  void applyDirtyConstantSetVsF(std::uint32_t startReg, std::uint32_t count);
  void applyDirtyConstantSetVsI(std::uint32_t startReg, std::uint32_t count);
  void applyDirtyConstantSetVsB(std::uint32_t startReg, std::uint32_t count);
  void applyDirtyConstantSetPsF(std::uint32_t startReg, std::uint32_t count);
  void applyDirtyConstantSetPsI(std::uint32_t startReg, std::uint32_t count);
  void applyDirtyConstantSetPsB(std::uint32_t startReg, std::uint32_t count);
  void applyDirtyTransformChange();
  void applyDirtyClipPlaneChange();
  void applyDirtyViewportChange();
  void applyDirtyRenderStateFog();
  void applyDirtyRenderStateAlpha();
  void applyDirtyRenderStateTexFactor();
  void applyDirtyTextureStageConstant();

  // Mark every per-frequency uniform DirtyBit on pendingDirty_. Used by
  // the d3d9 stateblock-apply path: a state-block apply is bulk state
  // mutation that bypasses per-record dirty marking, so the next
  // EncodeContext must observe "everything could have changed since the
  // last encode call" and re-upload all per-frequency UBOs.
  void markPendingDirtyAll();

  // Snapshot + reset pendingDirty_. Called by makeEncodeContext (and
  // tests) to atomically move the accumulated dirty state into a new
  // encoder. Public so the d2 unit tests can drive end-to-end coverage
  // without reaching into private state.
  uniform::DirtyState consumePendingDirty();
};

}  // namespace dxmt9
