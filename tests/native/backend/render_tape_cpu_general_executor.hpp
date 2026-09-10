#pragma once

// A deliberately cold, resource-backed executor for the general Render Tape
// grammar.  This is test infrastructure, not a second provider.  Admission is
// performed by buildGeneralReplayPlan(); this type only consumes its immutable
// plan and exercises the same D9C object factory, lock/unlock paths, chunk
// replay, and CpuReady queue publication used by the provider.

#include "device_c_common.hpp"
#include "device_c_chunk_replay.hpp"
#include "device_c_cpu_ready_plan.hpp"
#include "device_c_render_tape_capture_layout.hpp"
#include "device_c_render_tape_provider.hpp"
#include "device_c_render_tape_runtime_objects.hpp"
#include "device_c_render_tape_completion.hpp"
#include "device_c_render_tape_cpu_window.hpp"

#include "render_tape_cpu_semantics.hpp"
#include "render_tape_cpu_snapshot_proof.hpp"
#include "render_tape_cpu_work_boundary.hpp"
#include "render_tape_cpu_mutation_boundary.hpp"
#include "render_tape_cpu_source_oracle.hpp"
#include "render_tape_cpu_source_admission.hpp"

#include "dxmt9/dxmt9_source_payload.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace dxmt9::test {

inline constexpr std::size_t kCpuGeneralMaxEvents = 4096u;
inline constexpr std::size_t kCpuGeneralMaxCommandRecords = 16384u;
inline constexpr std::size_t kCpuGeneralMaxDraws = 4096u;
inline constexpr std::size_t kCpuGeneralMaxObjects = 1024u;
inline constexpr std::size_t kCpuGeneralMaxSources = 4096u;

struct CpuGeneralTapeTrace {
  std::uint32_t event = 0xffffffffu;
  std::uint32_t source = 0u;
  std::uint64_t sourceGeneration = 0u;
  std::uint64_t submission = 0u;
  std::uint32_t storageFirstPage = 0u;
  std::uint32_t storagePageCount = 0u;
  std::uint64_t storageGeneration = 0u;
  bool submitted = false;
  bool completed = false;
  bool reclaimed = false;
  std::uint32_t releasedAtEvent = 0xffffffffu;
  std::vector<D9CWireObjectIdentity> bufferClosure;
  std::uint32_t heldMutationEvent = 0u;
  std::optional<D9CWireObjectIdentity> heldMutationIdentity;
};

struct CpuGeneralTapeDrawObservation {
  std::uint32_t ordinal = 0u;
  std::uint32_t publicationEvent = 0xffffffffu;
  std::uint64_t cumulativeSemanticDigest = 0u;
};

struct CpuGeneralTapeTombstone {
  D9CWireObjectIdentity identity{};
  core::Handle nativeHandle{};
};

struct CpuGeneralTapeDestroyWindow {
  std::uint32_t event = 0u;
  std::vector<std::uint64_t> pendingSubmissions;
  std::size_t ownersChecked = 0u;
  bool retentionValid = false;
};

struct CpuGeneralTapeExecutionResult {
  bool complete = false;
  std::string failure{};
  std::uint32_t failedEvent = 0xffffffffu;
  std::size_t events = 0u;
  std::size_t commands = 0u;
  std::size_t draws = 0u;
  std::size_t mipmapCommands = 0u;
  std::size_t retainedMipmapTexturesAfterDestroy = 0u;
  std::uint64_t digest = 1469598103934665603ull;
  std::size_t resourcesCreated = 0u;
  std::size_t resourcesReleased = 0u;
  std::size_t submitted = 0u;
  std::size_t completed = 0u;
  std::size_t reclaimed = 0u;
  std::size_t pendingPeak = 0u;
  std::size_t deferredReleases = 0u;
  std::size_t destroysWithPending = 0u;
  std::size_t retainedSnapshotsAfterDestroy = 0u;
  std::size_t deferredBufferMutations = 0u;
  std::size_t deferredDiscardMutations = 0u;
  std::size_t retainedBuffersAfterMutation = 0u;
  std::size_t mutationBoundaryReleases = 0u;
  std::vector<CpuGeneralTapeTrace> trace{};
  std::vector<CpuGeneralTapeDrawObservation> drawObservations{};
  // Retain native metadata after wrapper release for qualification only.
  std::vector<CpuGeneralTapeTombstone> tombstones{};
  std::vector<CpuGeneralTapeDestroyWindow> destroyWindows{};
  std::size_t windowPageSize = 0u;
  bool windowOffload = false;
  bool windowArena = false;
  bool windowWorkersDisabled = false;
  std::uint64_t setupNs = 0u;
  std::uint64_t replayNs = 0u;
  std::uint64_t observeNs = 0u;
  std::uint64_t cleanupNs = 0u;
  std::uint64_t completionNs = 0u;
  bool resetOk = false;
  std::size_t observedSnapshots = 0u;
  std::size_t observedLiveBindings = 0u;
  std::size_t sourceOracleCheckedSources = 0u;
  std::size_t observedMutations = 0u;
  // Cheap counts taken from each published source payload.  These remain
  // available when semantic observation is disabled for timing samples.
  std::size_t sourceSummaryCommands = 0u;
  std::size_t sourceSummaryDraws = 0u;
  std::size_t fullOracleCalls = 0u;
  bool logicalResourceOracleComplete = false;
};

namespace detail {

using dxmt9::d3d9::RenderTapeDigest;
using dxmt9::d3d9::RenderTapeProviderBlob;
using dxmt9::d3d9::RenderTapeSurfaceDescriptorV2;
using dxmt9::d3d9::RenderTapeBlockLayoutStatus;
using dxmt9::d3d9::RenderTapeBlockLockLayout;
using dxmt9::d3d9::RenderTapeLinearLayoutStatus;
using dxmt9::d3d9::RenderTapeLinearLockLayout;
using dxmt9::d3d9::renderTapeBlockLockLayout;
using dxmt9::d3d9::renderTapeLinearBytesPerPixel;
using dxmt9::d3d9::renderTapeLinearLockLayout;
using dxmt9::d3d9::writeRenderTapeLinearRows;
using dxmt9::d3d9::renderTapeTextureSubresourceDescriptor;
using dxmt9::d3d9::renderTapeLoadSurfaceDescriptorV2;

inline bool cpuGeneralSameIdentity(const D9CWireObjectIdentity& a,
                                   const D9CWireObjectIdentity& b) noexcept {
  return a.kind == b.kind && a.generation == b.generation &&
         a.objectId == b.objectId;
}

inline bool cpuGeneralSameDigest(const RenderTapeDigest& a,
                                 const RenderTapeDigest& b) noexcept {
  return std::memcmp(a.data(), b.data(), a.size()) == 0;
}

inline const RenderTapeProviderBlob* cpuGeneralFindBlob(
    std::span<const RenderTapeProviderBlob> blobs,
    const RenderTapeDigest& digest) noexcept {
  for (const auto& blob : blobs)
    if (cpuGeneralSameDigest(blob.digest, digest)) return &blob;
  return nullptr;
}

inline bool cpuGeneralWriteRows(const D9CSurfaceDesc& desc,
                                const D9CLockedRect& lock,
                                std::span<const std::byte> bytes) {
  if (renderTapeLinearBytesPerPixel(desc.format) != 0u) {
    RenderTapeLinearLockLayout layout{};
    if (renderTapeLinearLockLayout(desc, lock.pitch, nullptr, layout) !=
            RenderTapeLinearLayoutStatus::Accepted ||
        bytes.size() != layout.tightBytes)
      return false;
    return writeRenderTapeLinearRows(bytes, lock.bits, layout);
  }
  RenderTapeBlockLockLayout layout{};
  if (renderTapeBlockLockLayout(desc, lock.pitch, nullptr, layout) !=
          RenderTapeBlockLayoutStatus::Accepted ||
      bytes.size() != layout.tightBytes || !lock.bits)
    return false;
  auto* dst = static_cast<std::byte*>(lock.bits);
  for (std::uint32_t row = 0u; row < layout.rows; ++row) {
    std::memcpy(dst + static_cast<std::size_t>(row) * layout.pitch,
                bytes.data() + static_cast<std::size_t>(row) * layout.rowBytes,
                layout.rowBytes);
  }
  return true;
}

struct LogicalReceipt {
  D9CWireObjectIdentity identity{};
  std::uint32_t subresource = 0u;
  RenderTapeDigest digest{};
  std::uint64_t revision = 0u;
};

inline LogicalReceipt* cpuGeneralFindReceipt(
    std::vector<LogicalReceipt>& receipts,
    const D9CWireObjectIdentity& identity, std::uint32_t subresource) {
  for (auto& receipt : receipts)
    if (cpuGeneralSameIdentity(receipt.identity, identity) &&
        receipt.subresource == subresource)
      return &receipt;
  return nullptr;
}

inline void cpuGeneralRecordReceipt(
    std::vector<LogicalReceipt>& receipts,
    const D9CWireObjectIdentity& identity, std::uint32_t subresource,
    const RenderTapeDigest& digest) {
  auto* prior = cpuGeneralFindReceipt(receipts, identity, subresource);
  if (prior) {
    prior->digest = digest;
    ++prior->revision;
    return;
  }
  receipts.push_back({.identity = identity,
                      .subresource = subresource,
                      .digest = digest,
                      .revision = 1u});
}

inline void cpuGeneralHashBytes(std::uint64_t& hash,
                                std::span<const std::uint8_t> bytes) noexcept {
  constexpr std::uint64_t prime = 1099511628211ull;
  for (const auto byte : bytes) {
    hash ^= byte;
    hash *= prime;
  }
}

inline void cpuGeneralHashDigest(std::uint64_t& hash,
                                 const RenderTapeDigest& digest) noexcept {
  cpuGeneralHashBytes(hash, std::span<const std::uint8_t>(
                               reinterpret_cast<const std::uint8_t*>(
                                   digest.data()),
                               digest.size()));
}

}  // namespace detail

// Fixture is intentionally duck-typed: it must expose cDevice (unique_ptr
// D9CDevice), routing (the resource-backed RoutingDevice), and device (the
// shared core Device). Access is the test-only CommandQueue access facade.
template <class Fixture, class Access>
class CpuGeneralTapeExecutor {
 public:
  using Plan = dxmt9::d3d9::render_tape_runtime::GeneralPreflightPlan;
  using Blob = dxmt9::d3d9::RenderTapeProviderBlob;
  using Result = CpuGeneralTapeExecutionResult;

  enum class PendingIdentityFault : std::uint8_t {
    None,
    Sequence,
    SourceIndex,
    SourceGeneration,
    StorageFirstPage,
    StoragePageCount,
    StorageGeneration,
  };

  struct Options {
    dxmt9::d3d9::ProviderReplayPolicy route =
        dxmt9::d3d9::ProviderReplayPolicy::ProductionSlot;
    bool beforeNextWork = false;
    std::size_t maxPendingSources = 1u;
    // Native qualification until authenticated CLI schedule metadata exists.
    bool holdSafeBufferMutation = false;
    std::span<const CpuSourceOracleRow> sourceOracle{};
    // Native negative control only; never enabled by the accepted CLI lane.
    bool corruptRetainedBytesForTest = false;
    bool corruptRetainedBytesAfterMutationForTest = false;
    bool deferOneObjectReleaseForResetTest = false;
    bool dropRetiredAliasReceiptForTest = false;
    PendingIdentityFault pendingIdentityFault = PendingIdentityFault::None;
    // Measurement keeps production replay and lifetime work intact while
    // omitting the cold semantic/resource oracle.  It is intentionally
    // constrained to immediate, single-source publication until the caller
    // has a qualified batching policy.
    bool measurement = false;
  };

  CpuGeneralTapeExecutor(Fixture& fixture, const Plan& plan,
                         std::span<const Blob> blobs, Options options = {})
      : fixture_(fixture), plan_(plan), blobs_(blobs), options_(options) {}

  Result run() {
    Result result;
    result.events = plan_.events.size();
    if (options_.measurement &&
        (options_.beforeNextWork || options_.maxPendingSources != 1u ||
         !options_.sourceOracle.empty() || options_.holdSafeBufferMutation ||
         options_.corruptRetainedBytesForTest ||
         options_.corruptRetainedBytesAfterMutationForTest ||
         options_.deferOneObjectReleaseForResetTest ||
         options_.dropRetiredAliasReceiptForTest ||
         options_.pendingIdentityFault != PendingIdentityFault::None)) {
      result.failure = "general-executor-measurement-policy";
      return result;
    }
    if (options_.maxPendingSources < 1u || options_.maxPendingSources > 2u) {
      result.failure = "general-executor-max-pending-sources-bound";
      return result;
    }
    if (options_.maxPendingSources > 1u &&
        (!options_.beforeNextWork || options_.holdSafeBufferMutation)) {
      result.failure = "general-executor-max-pending-sources-policy";
      return result;
    }
    if (options_.holdSafeBufferMutation && !options_.beforeNextWork) {
      result.failure = "general-executor-mutation-policy-needs-delayed-completion";
      return result;
    }
    if (!admit(result)) return result;
    if (!validateSourceOracle(result)) return result;
    if (!fixture_.cDevice || !fixture_.routing || !fixture_.device) {
      result.failure = "general-executor-fixture-unavailable";
      return result;
    }
    if (!admitEmission(result)) return result;
    result.windowPageSize = fixture_.routing->queue_.cpuReadyArenaPlanLimits().pageSize;
    result.windowOffload = bool(fixture_.cDevice->replayOffload);
    result.windowArena = fixture_.routing->supportsCpuReadyArenaReplay();
    result.windowWorkersDisabled = Access::cpuTapeWorkersDisabled(fixture_.routing->queue_);
    // The CLI/native entry establishes zero auto-publication limits BEFORE
    // the first device (cached production policy). These live predicates
    // exclude asynchronous mutation, offload drains and Arena admission waits.
    if (options_.maxPendingSources > 1u &&
        (result.windowOffload || result.windowArena || result.windowPageSize !=
             d3d9::kRenderTapeCpuWindowPageSize ||
         !result.windowWorkersDisabled)) {
      result.failure = "general-executor-window-runtime-not-isolated";
      return result;
    }
    const auto overlapEligible = [&](const auto& chunk, std::uint32_t event) {
      d3d9::ImportedChunkView imported;
      if (!d3d9::importPrevalidatedCommandChunk(chunk.bytes, chunk.envelope, imported) ||
          imported.records.empty() || imported.records.size() > 64u) return false;
      const auto shape = d3d9::planRenderTapeCpuWindowSource(
          imported, static_cast<d3d9::RawOrdinal>(event + 1u),
          fixture_.routing->queue_.cpuReadyArenaPlanLimits().pageSize);
      // One bounded ordinary draw/state source, not UP/Fan/control fallback
      // or an internal rotation. The production aggregate capacity accounts
      // for worst-case uniforms/state, not just command/vertex byte counts.
      // With <=64 records and the zero publication limits, the compatibility
      // path's U32 payload/range capacity cannot require an internal split.
      return shape.eligible();
    };
    std::vector<dxmt9::d3d9::render_tape_runtime::OwnedObject> objects;
    std::vector<detail::LogicalReceipt> receipts;
    objects.reserve(plan_.definitions.size());
    receipts.reserve(plan_.mutations.size());
    result.tombstones.reserve(plan_.definitions.size());
    auto& tombstones = result.tombstones;
    CpuTapeSnapshotProofCache snapshotProofs;
    struct PendingSourceProof {
      struct MipmapTexture {
        D9CWireObjectIdentity identity{};
        core::Handle handle{};
        std::uintptr_t recordAddress = 0u;
      };
      struct Snapshot {
        core::Handle buffer{};
        CpuTapeLiveBufferView view{};
        std::uint64_t digest = 0u;
        std::optional<core::DrawBufferBindingSnapshot> captured;
      };

      d3d9::RenderTapeCompletionSchedule schedule{};
      d3d9::RenderTapeCompletionKey key{};
      std::uint64_t submission = 0u;
      std::uint32_t source = 0u;
      std::uint64_t sourceGeneration = 0u;
      std::uint32_t storageFirstPage = 0u;
      std::uint32_t storagePageCount = 0u;
      std::uint64_t storageGeneration = 0u;
      std::size_t traceOrdinal = 0u;
      std::vector<Snapshot> snapshots;
      std::vector<core::Handle> resourceBuffers;
      std::vector<D9CWireObjectIdentity> bufferIdentities;
      std::vector<MipmapTexture> mipmapTextures;
      bool mutationHeld = false;
      bool windowEligible = false;

      void bind(const core::metalqueue::QueueCompletionSource& completion) noexcept {
        submission = completion.seqId;
        source = completion.source.id.index;
        sourceGeneration = completion.source.id.generation;
        storageFirstPage = completion.source.storage.firstPage;
        storagePageCount = completion.source.storage.pageCount;
        storageGeneration = completion.source.storage.generation;
      }

      bool matches(const core::metalqueue::QueueCompletionSource& completion) const noexcept {
        return submission == completion.seqId &&
               source == completion.source.id.index &&
               sourceGeneration == completion.source.id.generation &&
               storageFirstPage == completion.source.storage.firstPage &&
               storagePageCount == completion.source.storage.pageCount &&
               storageGeneration == completion.source.storage.generation;
      }

      bool matches(const CpuGeneralTapeTrace& trace) const noexcept {
        return submission == trace.submission && source == trace.source &&
               sourceGeneration == trace.sourceGeneration &&
               storageFirstPage == trace.storageFirstPage &&
               storagePageCount == trace.storagePageCount &&
               storageGeneration == trace.storageGeneration;
      }
    };
    std::deque<PendingSourceProof> pendingSources;
    const auto pendingWindowEligible = [&] {
      return std::all_of(pendingSources.begin(), pendingSources.end(),
                         [](const auto& owner) { return owner.windowEligible; });
    };
    const auto expectedBuffer = [&](core::Handle buffer) {
      const D9CBuffer* expected = nullptr;
      for (const auto& object : objects) {
        if (object.identity.kind != D9C_CHUNK_HANDLE_KIND_BUFFER || !object.value) continue;
        const auto* candidate = static_cast<const D9CBuffer*>(object.value);
        if (candidate->obj && candidate->obj->handle() == buffer) { expected = candidate; break; }
      }
      return expected;
    };
    const auto recordProof = [&](PendingSourceProof& owner, core::Handle buffer,
                                 CpuTapeLiveBufferView view,
                                 std::uint64_t content, std::uint64_t& digest,
                                 std::optional<core::DrawBufferBindingSnapshot> captured = std::nullopt) {
      if (options_.beforeNextWork && std::none_of(
              owner.snapshots.begin(), owner.snapshots.end(), [&](const auto& prior) {
                return prior.buffer == buffer &&
                    prior.view.contentsAddress == view.contentsAddress &&
                    prior.view.contentRevision == view.contentRevision;
              })) {
        if (owner.snapshots.size() == CpuTapeSnapshotProofCache::kCapacity) return false;
        owner.snapshots.push_back({buffer, view, content, captured});
      }
      detail::cpuTapeDigestScalar(digest, view.byteSize);
      detail::cpuTapeDigestScalar(digest, content);
      return true;
    };
    const auto observeSnapshot = [&](PendingSourceProof& owner,
                                     core::Handle buffer,
                                     const core::DrawBufferBindingSnapshot& snapshot,
                                     std::uint64_t& digest) {
      auto& pool = fixture_.routing->queue_.pool();
      const auto* expected = expectedBuffer(buffer);
      std::uint64_t content = 0u;
      if (cpuTapeBufferObservationRoute(pool.bufferHasVersionedBacking(buffer.value),
              true, snapshot.valid()) != CpuTapeBufferObservationRoute::Captured ||
          pool.validateCapturedBufferSnapshot(buffer, snapshot) !=
              resources::CapturedBufferSnapshotStatus::Valid ||
          !expected || !snapshotProofs.prove(buffer, snapshot, expected->obj->bytes(), content))
        return false;
      return recordProof(owner, buffer,
                         {snapshot.metalHandle, snapshot.contentsAddress,
                          snapshot.byteSize, snapshot.contentRevision},
                         content, digest, snapshot);
    };
    const auto observeLive = [&](PendingSourceProof& owner, core::Handle buffer,
                                 std::uint64_t& digest,
                                 std::uint64_t& revision) {
      // Cold single-thread executor, inside the queue observer. No mutation
      // or completion can run here; do not reacquire the queue mutex.
      const auto* record = fixture_.routing->queue_.pool().findBuffer(buffer.value);
      const auto* expected = expectedBuffer(buffer);
      if (!record || cpuTapeBufferObservationRoute(record->hasVersionedBacking(),
              false, bool(record->buffer) && record->contents) != CpuTapeBufferObservationRoute::Live)
        return false;
      const CpuTapeLiveBufferView view{record->buffer.handle,
          static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(record->contents)),
          record->desc.size, record->contentRevision};
      std::uint64_t content = 0u;
      if (!expected || !snapshotProofs.proveLive(buffer, view, expected->obj->bytes(), content))
        return false;
      revision = view.contentRevision;
      return recordProof(owner, buffer, view, content, digest);
    };
    core::Handle expectedUploadHandle{};
    std::uint32_t expectedUploadLevel = 0u;
    std::span<const std::byte> expectedUploadBytes;
    bool observedUpload = false;
    fixture_.routing->generalTextureUpload =
        [&](core::TextureHandle handle, std::uint32_t level,
            std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
            std::uint32_t, std::span<const std::uint8_t> bytes) {
          const bool exact = handle == expectedUploadHandle &&
              level == expectedUploadLevel && bytes.size() == expectedUploadBytes.size() &&
              !bytes.empty() &&
              (options_.measurement ||
               std::memcmp(bytes.data(), expectedUploadBytes.data(), bytes.size()) == 0);
          observedUpload |= exact;
          return exact;
        };
    struct UploadCleanup {
      Fixture& fixture;
      ~UploadCleanup() { fixture.routing->generalTextureUpload = {}; }
    } uploadCleanup{fixture_};
    struct ObjectCleanup {
      std::vector<dxmt9::d3d9::render_tape_runtime::OwnedObject>& objects;
      ~ObjectCleanup() {
        for (auto it = objects.rbegin(); it != objects.rend(); ++it)
          dxmt9::d3d9::render_tape_runtime::releaseOwnedObject(*it);
      }
    } objectCleanup{objects};
    bool bootstrapPending = false;
    bool bootstrapDone = false;

    const auto fail = [&](std::uint32_t event, std::string reason) {
      if (result.failure.empty()) {
        result.failure = std::move(reason);
        result.failedEvent = event;
      }
      return false;
    };

    const auto setupBegin = options_.measurement
        ? std::chrono::steady_clock::time_point{}
        : std::chrono::steady_clock::now();
    const auto create = [&](const auto& definition) {
      if (objects.size() >= kCpuGeneralMaxObjects)
        return static_cast<void*>(nullptr);
      return dxmt9::d3d9::render_tape_runtime::createRenderTapeObject(
          fixture_.cDevice.get(), definition.fixed, definition.descriptor,
          blobs_, objects);
    };
    if (!options_.measurement) result.setupNs += elapsed(setupBegin);

    // Pending sources are retained in submission order. Every potentially
    // blocking work entry remains quiescent; wrapper destruction and
    // explicitly proved no-wait buffer mutations may run before
    // acknowledgment.
    std::size_t sourceOracleRow = 0u;
    bool pendingIdentityFaultInjected = false;
    const auto releaseOne = [&](std::uint32_t event) {
      if (pendingSources.empty()) {
        if (result.completed == result.submitted &&
            Access::pendingCompletionCount(fixture_.routing->queue_) == 0u)
          return true;
        return fail(event, "general-executor-pending-identity-count");
      }
      const auto completionBegin = options_.measurement
          ? std::chrono::steady_clock::time_point{}
          : std::chrono::steady_clock::now();
      struct CompletionTiming {
        Result& result;
        std::chrono::steady_clock::time_point begin;
        bool enabled = true;
        ~CompletionTiming() {
          if (enabled) result.completionNs += elapsed(begin);
        }
      } completionTiming{result, completionBegin, !options_.measurement};
      if (result.submitted < result.completed ||
          result.submitted - result.completed != pendingSources.size() ||
          Access::pendingCompletionCount(fixture_.routing->queue_) !=
              pendingSources.size())
        return fail(event, "general-executor-pending-identity-count");
      auto& owner = pendingSources.front();
      if (owner.traceOrdinal >= result.trace.size() ||
          owner.traceOrdinal != result.completed)
        return fail(event, "general-executor-pending-trace-order");
      auto& trace = result.trace[owner.traceOrdinal];
      if (!pendingIdentityFaultInjected &&
          options_.pendingIdentityFault != PendingIdentityFault::None) {
        switch (options_.pendingIdentityFault) {
        case PendingIdentityFault::Sequence:
          ++owner.submission;
          break;
        case PendingIdentityFault::SourceIndex:
          ++owner.source;
          break;
        case PendingIdentityFault::SourceGeneration:
          ++owner.sourceGeneration;
          break;
        case PendingIdentityFault::StorageFirstPage:
          ++owner.storageFirstPage;
          break;
        case PendingIdentityFault::StoragePageCount:
          ++owner.storagePageCount;
          break;
        case PendingIdentityFault::StorageGeneration:
          ++owner.storageGeneration;
          break;
        case PendingIdentityFault::None:
          break;
        }
        pendingIdentityFaultInjected = true;
      }
      if (!owner.matches(trace))
        return fail(event, "general-executor-release-identity-changed");
      const auto beforeResident = Access::residentCount(fixture_.routing->queue_);
      if (!owner.schedule.release(owner.key, [&] {
            return Access::acknowledgeOne(fixture_.routing->queue_);
          }).accepted())
        return fail(event, "general-executor-acknowledge-failed");
      trace.completed = true;
      trace.releasedAtEvent = event;
      ++result.completed;
      result.deferredReleases += event > trace.event;
      if (!Access::finishQueue(fixture_.routing->queue_))
        return fail(event, "general-executor-finish-failed");
      Access::reclaimCompletedPool(fixture_.routing->queue_);
      trace.reclaimed = Access::residentCount(fixture_.routing->queue_) < beforeResident;
      if (trace.reclaimed) ++result.reclaimed;
      if (!trace.reclaimed || !owner.schedule.finishIteration().accepted())
        return fail(event, "general-executor-scheduled-reclaim-failed");
      pendingSources.pop_front();
      return true;
    };
    const auto releasePending = [&](std::uint32_t event) {
      while (!pendingSources.empty()) {
        if (!releaseOne(event)) return false;
      }
      if (result.completed == result.submitted &&
          Access::pendingCompletionCount(fixture_.routing->queue_) == 0u)
        return true;
      return fail(event, "general-executor-pending-identity-count");
    };
    const auto publishReady = [&](std::uint32_t event, bool windowEligible = false) {
      if (!Access::publishWriting(fixture_.routing->queue_))
        return fail(event, "general-executor-publish-failed");
      while (Access::readyCount(fixture_.routing->queue_) != 0u) {
        if (result.submitted < result.completed ||
            result.submitted - result.completed >= options_.maxPendingSources)
          return fail(event, "general-executor-pending-budget");
        const auto expected = Access::peekReadyCompletion(fixture_.routing->queue_);
        if (!expected.completionIdentityValid())
          return fail(event, "general-executor-invalid-completion-identity");
        PendingSourceProof candidate;
        candidate.windowEligible = windowEligible;
        candidate.bind(expected);
        candidate.key = {1u, candidate.submission, candidate.source, 0u,
                         candidate.sourceGeneration};
        if (!candidate.schedule.append({candidate.key}).accepted() ||
            !candidate.schedule.beginIteration(1u).accepted() ||
            !candidate.schedule.submit(candidate.key).accepted())
          return fail(event, "general-executor-schedule-submit-failed");
        auto submission = Access::submitOneWithheld(
            fixture_.routing->queue_, [&](const auto& payload) {
              const auto observeBegin = options_.measurement
                  ? std::chrono::steady_clock::time_point{}
                  : std::chrono::steady_clock::now();
              {
                for (std::size_t command = 0u; command < payload.commandCount(); ++command) {
                  const auto sourceCommand = payload.commandAt(command);
                  ++result.sourceSummaryCommands;
                  if (sourceCommand.kind() == core::MetalCommandKind::DrawRun)
                    result.sourceSummaryDraws += sourceCommand.command.drawParams.size();
                  if (options_.measurement &&
                      sourceCommand.kind() == core::MetalCommandKind::GenerateMipmaps)
                    ++result.mipmapCommands;
                  if (!options_.measurement &&
                      sourceCommand.kind() == core::MetalCommandKind::GenerateMipmaps &&
                      sourceCommand.command.generateMipmaps) {
                    const auto handle = sourceCommand.command.generateMipmaps->texture;
                    const auto object = std::find_if(objects.begin(), objects.end(), [&](const auto& value) {
                      if (!value.value || value.identity.kind != D9C_CHUNK_HANDLE_KIND_TEXTURE)
                        return false;
                      const auto* texture = static_cast<const D9CTexture*>(value.value);
                      return texture->obj && texture->obj->handle() == handle;
                    });
                    const auto* record = fixture_.routing->queue_.pool().findTexture(handle.value);
                    if (object == objects.end() || !record ||
                        record->lastUsedSeqId < candidate.submission) {
                      fail(event, "general-executor-unqualified-mipmap-texture");
                    } else {
                      candidate.mipmapTextures.push_back({object->identity, handle,
                          reinterpret_cast<std::uintptr_t>(record)});
                    }
                  }
                  if (options_.measurement) continue;
                  core::visitSourceCommandResources(payload.commandAt(command), [&](const auto& ref) {
                    if (ref.entry.kind != core::ChunkHandleKind::Buffer ||
                        std::find(candidate.resourceBuffers.begin(), candidate.resourceBuffers.end(),
                                  ref.entry.handle) != candidate.resourceBuffers.end()) return;
                    if (candidate.resourceBuffers.size() == CpuTapeSnapshotProofCache::kCapacity) {
                      fail(event, "general-executor-mutation-closure-cap");
                      return;
                    }
                    candidate.resourceBuffers.push_back(ref.entry.handle);
                    const auto object = std::find_if(objects.begin(), objects.end(), [&](const auto& candidate) {
                      if (candidate.identity.kind != D9C_CHUNK_HANDLE_KIND_BUFFER || !candidate.value)
                        return false;
                      const auto* buffer = static_cast<const D9CBuffer*>(candidate.value);
                      return buffer->obj && buffer->obj->handle() == ref.entry.handle;
                    });
                    if (object == objects.end()) {
                      fail(event, "general-executor-unqualified-source-buffer");
                      return;
                    }
                    candidate.bufferIdentities.push_back(object->identity);
                  });
                }
                std::sort(candidate.bufferIdentities.begin(), candidate.bufferIdentities.end(),
                    [](const auto& a, const auto& b) {
                      return std::tie(a.kind, a.generation, a.objectId) <
                             std::tie(b.kind, b.generation, b.objectId);
                    });
                if (!options_.sourceOracle.empty()) {
                  if (sourceOracleRow >= options_.sourceOracle.size() ||
                      options_.sourceOracle[sourceOracleRow].event != event + 1u ||
                      !cpuSourceOracleMatches(candidate.bufferIdentities,
                                               options_.sourceOracle[sourceOracleRow])) {
                    fail(event, "general-executor-source-oracle-mismatch");
                    return;
                  }
                  ++sourceOracleRow;
                  ++result.sourceOracleCheckedSources;
                }
              }
              if (!options_.measurement) {
                std::string observeFailure;
                ++result.fullOracleCalls;
                auto resource = [&](std::uint32_t kind, core::Handle handle,
                                    std::uint64_t& hash) {
                  return observeResource(objects, receipts, tombstones,
                                         kind, handle, hash);
                };
                auto draw = [&](std::uint64_t cumulativeHash) {
                  if (result.drawObservations.size() >= kCpuGeneralMaxDraws) {
                    observeFailure = "general-executor-draw-cap";
                    return false;
                  }
                  result.drawObservations.push_back({
                      .ordinal = static_cast<std::uint32_t>(
                          result.drawObservations.size() + 1u),
                      .publicationEvent = event,
                      .cumulativeSemanticDigest = cumulativeHash});
                  return true;
                };
                const auto observeSnapshotForSource =
                    [&](core::Handle buffer,
                        const core::DrawBufferBindingSnapshot& snapshot,
                        std::uint64_t& digest) {
                      return observeSnapshot(candidate, buffer, snapshot, digest);
                    };
                const auto observeLiveForSource =
                    [&](core::Handle buffer, std::uint64_t& digest,
                        std::uint64_t& revision) {
                      return observeLive(candidate, buffer, digest, revision);
                    };
                if (!observeCpuTapePayload(payload, result.digest, resource, draw,
                                           observeFailure, &result.observedSnapshots,
                                           observeSnapshotForSource,
                                           &result.observedLiveBindings,
                                           observeLiveForSource,
                                           &result.mipmapCommands) &&
                    result.failure.empty()) {
                  result.failure = observeFailure.empty()
                                       ? "general-executor-observe-failed"
                                       : observeFailure;
                  result.failedEvent = event;
                }
              }
              if (!options_.measurement)
                result.observeNs += elapsed(observeBegin);
            });
        if (!submission.dequeued || !submission.submitted) {
          return fail(event, "general-executor-submit-failed");
        }
        ++result.submitted;
        const auto& completion = submission.completion;
        if (!candidate.matches(completion))
          return fail(event, "general-executor-submission-identity-changed");
        candidate.traceOrdinal = result.trace.size();
        CpuGeneralTapeTrace trace{
            .event = event,
            .source = completion.source.id.index,
            .sourceGeneration = completion.source.id.generation,
            .submission = completion.seqId,
            .storageFirstPage = completion.source.storage.firstPage,
            .storagePageCount = completion.source.storage.pageCount,
            .storageGeneration = completion.source.storage.generation,
            .submitted = true};
        trace.bufferClosure = candidate.bufferIdentities;
        pendingSources.push_back(std::move(candidate));
        result.trace.push_back(trace);
        result.pendingPeak = std::max(result.pendingPeak, result.submitted - result.completed);
        if (!options_.beforeNextWork && !releasePending(event)) return false;
      }
      return result.failure.empty();
    };

    const auto replayChunk = [&](std::uint32_t event,
                                 const dxmt9::d3d9::render_tape_runtime::CommandChunkView& chunk) {
      dxmt9::d3d9::ImportedChunkView imported;
      if (!dxmt9::d3d9::importPrevalidatedCommandChunk(chunk.bytes,
                                                        chunk.envelope,
                                                        imported))
        return fail(event, "general-executor-command-import-failed");
      if (result.commands + imported.records.size() >
          kCpuGeneralMaxCommandRecords)
        return fail(event, "general-executor-command-cap");
      const auto replayBegin = options_.measurement
          ? std::chrono::steady_clock::time_point{}
          : std::chrono::steady_clock::now();
      const bool windowEligible = options_.maxPendingSources > 1u && overlapEligible(chunk, event);
      if (Access::readyCount(fixture_.routing->queue_) ||
          Access::writingCommandCount(fixture_.routing->queue_) ||
          Access::pendingCompletionCount(fixture_.routing->queue_) != pendingSources.size() ||
          Access::residentCount(fixture_.routing->queue_) != pendingSources.size() ||
          (!pendingSources.empty() &&
           (!windowEligible || !pendingWindowEligible())))
        return fail(event, "general-executor-nonquiescent-chunk-entry");
      if (options_.maxPendingSources > 1u &&
          !Access::noWaitHeadroom(fixture_.routing->queue_).admitsOne())
        return fail(event, "general-executor-window-source-admission-denied");
      std::vector<void*> resolved(imported.handles.size());
      std::size_t resolvedCount = 0u;
      if (!dxmt9::d3d9::render_tape_runtime::resolveCommandHandles(
              chunk, objects, resolved, resolvedCount) ||
          resolvedCount != imported.handles.size())
        return fail(event, "general-executor-command-handle-failed");
      if (dxmt9::d3d9::replayPrevalidatedResolvedCommandChunk(
              fixture_.cDevice.get(), chunk.bytes, chunk.envelope, resolved,
              options_.route) != core::D3D_OK)
        return fail(event, "general-executor-command-replay-failed");
      if (!options_.measurement) result.replayNs += elapsed(replayBegin);
      result.commands += imported.records.size();
      for (const auto& record : imported.records) {
        if (record.type == D9C_COMMAND_RECORD_DRAW_PRIMITIVE ||
            record.type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE)
          ++result.draws;
      }
      return publishReady(event, windowEligible);
    };

    const auto replayBootstrap = [&](std::uint32_t event) {
      if (bootstrapDone) return true;
      bootstrapDone = true;
      for (const auto& chunk : plan_.bootstrapChunks)
        if (!replayChunk(event, chunk)) return false;
      return true;
    };

    const auto mutation = [&](std::uint32_t event,
                              const auto& fixed) {
      // Cached byte proofs never survive a possible CPU mutation, including
      // DISCARD that legally reuses an address after immediate completion.
      snapshotProofs.invalidate();
      const auto* blob = detail::cpuGeneralFindBlob(blobs_, fixed.digest);
      void* value = dxmt9::d3d9::render_tape_runtime::findExactObject(
          objects, fixed.identity);
      if (!blob || !value || blob->bytes.size() != fixed.byteSize)
        return fail(event, "general-executor-mutation-object-or-blob");
      bool ok = false;
      if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
        dxmt9::d3d9::RenderTapeBufferMutationReplayPlan replayPlan{};
        void* bits = nullptr;
        auto* buffer = static_cast<D9CBuffer*>(value);
        ok = dxmt9::d3d9::renderTapeBufferMutationReplayPlan(fixed,
                                                              replayPlan) &&
             dxmt9c_buffer_lock(buffer, replayPlan.byteOffset,
                                replayPlan.byteSize, &bits,
                                replayPlan.lockFlags) == core::D3D_OK &&
             bits != nullptr;
        if (ok) {
          std::memcpy(bits, blob->bytes.data(), blob->bytes.size());
          ok = dxmt9c_buffer_unlock(buffer) == core::D3D_OK;
        }
      } else if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
        const auto definition = std::find_if(
            plan_.definitions.begin(), plan_.definitions.end(),
            [&](const auto& candidate) {
              return detail::cpuGeneralSameIdentity(candidate.fixed.identity,
                                                     fixed.identity);
            });
        D9CSurfaceDesc desc{};
        D9CLockedRect lock{};
        auto* texture = static_cast<D9CTexture*>(value);
        expectedUploadHandle = texture->obj->handle();
        expectedUploadLevel = fixed.subresource;
        expectedUploadBytes = blob->bytes;
        observedUpload = false;
        ok = definition != plan_.definitions.end() && fixed.byteOffset == 0u &&
             dxmt9::d3d9::renderTapeTextureSubresourceDescriptor(
                 definition->descriptor, fixed.subresource, desc) &&
             dxmt9c_texture_lock_rect(texture, fixed.subresource, &lock,
                                      nullptr, 0u) == core::D3D_OK &&
             lock.bits != nullptr;
        const bool copied = ok && detail::cpuGeneralWriteRows(
                                        desc, lock, blob->bytes);
        const bool unlocked = ok &&
            dxmt9c_texture_unlock_rect(texture, fixed.subresource) ==
            core::D3D_OK;
        ok = copied && unlocked && observedUpload && !fixture_.routing->textureUploadFailed;
        expectedUploadBytes = {};
      } else if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
        const auto definition = std::find_if(
            plan_.definitions.begin(), plan_.definitions.end(),
            [&](const auto& candidate) {
              return detail::cpuGeneralSameIdentity(candidate.fixed.identity,
                                                     fixed.identity);
            });
        dxmt9::d3d9::RenderTapeSurfaceDescriptorV2 surface{};
        auto* wrapped = static_cast<D9CSurface*>(value);
        ok = definition != plan_.definitions.end() && fixed.subresource == 0u &&
             fixed.byteOffset == 0u &&
             dxmt9::d3d9::renderTapeLoadSurfaceDescriptorV2(definition->descriptor, surface);
        const auto disposition = ok
            ? static_cast<dxmt9::d3d9::RenderTapeInitialContentDisposition>(
                  surface.initialContentDisposition)
            : dxmt9::d3d9::RenderTapeInitialContentDisposition::Unavailable;
        if (ok && disposition ==
                      dxmt9::d3d9::RenderTapeInitialContentDisposition::
                          CompleteDepthFloat32V1) {
          const auto expected = static_cast<std::uint64_t>(surface.surface.width) *
                                surface.surface.height * 4u;
          // D24X8's tape bytes are canonical logical float depth, not the
          // backend's packed/physical representation.  The CPU executor
          // records this receipt for semantic observation; it deliberately
          // does not invoke a GPU seed/blit or claim physical depth bytes.
          ok = wrapped && wrapped->obj && blob->bytes.size() == expected;
        } else if (ok && disposition ==
                             dxmt9::d3d9::RenderTapeInitialContentDisposition::
                                 CompleteSeed) {
          D9CLockedRect lock{};
          ok = wrapped &&
               dxmt9c_surface_lock_rect(wrapped, &lock, nullptr, 0u) ==
                   core::D3D_OK &&
               lock.bits != nullptr;
          const bool copied = ok &&
              detail::cpuGeneralWriteRows(surface.surface, lock, blob->bytes);
          const bool unlocked = ok && dxmt9c_surface_unlock_rect(wrapped) ==
              core::D3D_OK;
          ok = copied && unlocked;
        } else {
          ok = false;
        }
      }
      if (!ok) return fail(event, "general-executor-mutation-failed");
      detail::cpuGeneralRecordReceipt(receipts, fixed.identity,
                                       fixed.subresource, fixed.digest);
      ++result.observedMutations;
      return true;
    };

    struct MutationBoundary {
      CpuTapeMutationResult decision = CpuTapeMutationResult::ReleaseBeforeMutation;
      core::Handle target{};
      D9CWireObjectIdentity identity{};
      std::uint64_t priorRevision = 0u;
    };
    const auto mutationBoundary = [&](const auto& fixed) {
      MutationBoundary boundary;
      if (!options_.holdSafeBufferMutation || result.submitted == result.completed ||
          pendingSources.size() != 1u ||
          Access::pendingCompletionCount(fixture_.routing->queue_) != 1u ||
          fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_BUFFER) return boundary;
      const auto* pendingSource = &pendingSources.front();
      const auto disposition = static_cast<d3d9::RenderTapeBufferMutationDisposition>(
          fixed.bufferDisposition);
      // NoOverwrite and managed rotation need separate witnesses. Never
      // classify an arbitrary non-DISCARD operation as Plain.
      if (disposition != d3d9::RenderTapeBufferMutationDisposition::Plain &&
          disposition != d3d9::RenderTapeBufferMutationDisposition::Discard) return boundary;
      auto* buffer = static_cast<D9CBuffer*>(
          d3d9::render_tape_runtime::findExactObject(objects, fixed.identity));
      d3d9::RenderTapeBufferMutationReplayPlan replayPlan{};
      if (!buffer || !buffer->obj || !d3d9::renderTapeBufferMutationReplayPlan(fixed, replayPlan))
        return boundary;
      boundary.target = buffer->obj->handle();
      boundary.identity = fixed.identity;
      auto& pool = fixture_.routing->queue_.pool();
      const auto* record = pool.findBuffer(boundary.target.value);
      if (!record) return boundary;
      boundary.priorRevision = record->contentRevision;
      const bool discard = disposition == d3d9::RenderTapeBufferMutationDisposition::Discard;
      const bool referenced = pendingSource &&
          std::find(pendingSource->resourceBuffers.begin(),
                    pendingSource->resourceBuffers.end(), boundary.target) !=
              pendingSource->resourceBuffers.end();
      bool hasCaptured = false, validCaptured = true;
      if (pendingSource) for (const auto& retained : pendingSource->snapshots) {
        if (retained.buffer != boundary.target) continue;
        hasCaptured |= retained.captured.has_value();
        validCaptured &= retained.captured &&
            pool.validateCapturedBufferSnapshot(boundary.target, *retained.captured) ==
                resources::CapturedBufferSnapshotStatus::Valid;
      }
      boundary.decision = reduceCpuTapeMutationAdmission({
          .exactlyOnePending = result.submitted - result.completed == 1u &&
              Access::pendingCompletionCount(fixture_.routing->queue_) == 1u,
          .alreadyDeferredMutation = pendingSource && pendingSource->mutationHeld,
          .bufferResolved = true,
          .defaultPool = buffer->desc.pool == 0u,
          .referencedByPending = referenced,
          .versioned = record->hasVersionedBacking(),
          .discard = discard,
          .noWait = pool.mapWaitSeqId(boundary.target, discard ? core::UsageDiscard : 0u) == 0u,
          .capturedBackingValid = referenced && hasCaptured && validCaptured});
      return boundary;
    };
    const auto verifyDeferredMutation = [&](std::uint32_t event, const MutationBoundary& boundary) {
      auto& pool = fixture_.routing->queue_.pool();
      const auto* after = pool.findBuffer(boundary.target.value);
      const auto* expected = expectedBuffer(boundary.target);
      if (!after || !expected || after->contentRevision <= boundary.priorRevision)
        return fail(event, "general-executor-deferred-mutation-revision");
      const CpuTapeLiveBufferView view{after->buffer.handle,
          static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(after->contents)),
          after->desc.size, after->contentRevision};
      std::uint64_t content = 0u;
      if (!snapshotProofs.proveLive(boundary.target, view, expected->obj->bytes(), content))
        return fail(event, "general-executor-deferred-mutation-upload");
      if (pendingSources.size() != 1u)
        return fail(event, "general-executor-deferred-mutation-without-source");
      auto& pendingSource = pendingSources.front();
      for (const auto& retained : pendingSource.snapshots) {
        if (!Access::bufferAlive(fixture_.routing->queue_, retained.buffer))
          return fail(event, "general-executor-mutation-premature-retirement");
        if (retained.buffer == boundary.target &&
            boundary.decision == CpuTapeMutationResult::DeferCapturedDiscard &&
            retained.view.metalHandle == after->buffer.handle)
          return fail(event, "general-executor-discard-reused-pending-backing");
        if (options_.corruptRetainedBytesAfterMutationForTest) {
          auto* bytes = reinterpret_cast<std::uint8_t*>(
              static_cast<std::uintptr_t>(retained.view.contentsAddress));
          bytes[0] ^= 1u;
        }
        std::uint64_t digest = 1469598103934665603ull;
        detail::cpuGeneralHashBytes(digest, {
            reinterpret_cast<const std::uint8_t*>(
                static_cast<std::uintptr_t>(retained.view.contentsAddress)),
            static_cast<std::size_t>(retained.view.byteSize)});
        if (digest != retained.digest)
          return fail(event, "general-executor-mutation-changed-pending-bytes");
        ++result.retainedBuffersAfterMutation;
      }
      ++result.deferredBufferMutations;
      result.deferredDiscardMutations +=
          boundary.decision == CpuTapeMutationResult::DeferCapturedDiscard;
      pendingSource.mutationHeld = true;
      if (pendingSource.traceOrdinal >= result.trace.size())
        return fail(event, "general-executor-held-mutation-without-source");
      result.trace[pendingSource.traceOrdinal].heldMutationEvent = event + 1u;
      result.trace[pendingSource.traceOrdinal].heldMutationIdentity = boundary.identity;
      return true;
    };

    const auto executeEvents = [&]() -> bool {
      for (const auto& replayEvent : plan_.events) {
        const auto event = replayEvent.eventIndex;
        MutationBoundary boundary;
        const bool pendingMutation = replayEvent.type == d3d9::RenderTapeEventType::ResourceMutation &&
            result.submitted != result.completed;
        if (pendingMutation && replayEvent.index < plan_.mutations.size())
          boundary = mutationBoundary(plan_.mutations[replayEvent.index]);
        const bool holdMutation = boundary.decision != CpuTapeMutationResult::ReleaseBeforeMutation;
        result.mutationBoundaryReleases += options_.holdSafeBufferMutation && pendingMutation && !holdMutation;
        if (!holdMutation) {
          const bool eligible = options_.maxPendingSources > 1u &&
              replayEvent.type == d3d9::RenderTapeEventType::CommandChunk &&
              replayEvent.index < plan_.commandChunks.size() &&
              overlapEligible(plan_.commandChunks[replayEvent.index], event);
          const auto action = cpuTapeWindowAction(pendingSources.size(),
              options_.maxPendingSources,
              releaseBeforeCpuTapeWork(replayEvent.type, options_.beforeNextWork),
              eligible, pendingWindowEligible());
          if (action == CpuTapeWindowAction::Reject)
            return fail(event, "general-executor-window-invalid-state");
          if (action == CpuTapeWindowAction::ReleaseOldest && !releaseOne(event)) return false;
          if (action == CpuTapeWindowAction::DrainAll && !releasePending(event)) return false;
        }
        const auto eventBegin = options_.measurement
            ? std::chrono::steady_clock::time_point{}
            : std::chrono::steady_clock::now();
        switch (replayEvent.type) {
        case dxmt9::d3d9::RenderTapeEventType::BootstrapState:
          bootstrapPending = true;
          for (const auto& gamma : plan_.bootstrapGamma) {
            if (gamma.empty()) continue;
            if (gamma.size() != dxmt9::d3d9::kRenderTapeGammaRampBytes)
            {
              fail(event, "general-executor-bootstrap-gamma-size");
              return false;
            }
            dxmt9c_device_set_gamma_ramp(
                fixture_.cDevice.get(),
                reinterpret_cast<const std::uint16_t*>(gamma.data()));
          }
          break;
        case dxmt9::d3d9::RenderTapeEventType::ObjectDefine: {
          snapshotProofs.invalidate();
          if (replayEvent.index >= plan_.definitions.size())
          {
            fail(event, "general-executor-definition-index");
            return false;
          }
          const auto& definition = plan_.definitions[replayEvent.index];
          void* value = create(definition);
          if (!value) {
            fail(event, "general-executor-object-create-failed");
            return false;
          }
          objects.push_back({.identity = definition.fixed.identity, .value = value});
          ++result.resourcesCreated;
          break;
        }
        case dxmt9::d3d9::RenderTapeEventType::ResourceMutation:
          if (replayEvent.index >= plan_.mutations.size() ||
              !mutation(event, plan_.mutations[replayEvent.index]))
            return false;
          if (holdMutation && !verifyDeferredMutation(event, boundary)) return false;
          break;
        case dxmt9::d3d9::RenderTapeEventType::CommandChunk:
          if (bootstrapPending && !replayBootstrap(event)) return false;
          if (replayEvent.index >= plan_.commandChunks.size() ||
              !replayChunk(event, plan_.commandChunks[replayEvent.index]))
            return false;
          break;
        case dxmt9::d3d9::RenderTapeEventType::ObjectDestroy: {
          snapshotProofs.invalidate();
          result.destroysWithPending += result.submitted != result.completed;
          if (bootstrapPending && !replayBootstrap(event)) return false;
          if (replayEvent.index >= plan_.destroys.size())
          {
            fail(event, "general-executor-destroy-index");
            return false;
          }
          const auto& destroy = plan_.destroys[replayEvent.index];
          auto object = std::find_if(
              objects.begin(), objects.end(), [&](const auto& candidate) {
                return candidate.value && detail::cpuGeneralSameIdentity(
                    candidate.identity, destroy.identity);
              });
          if (object == objects.end())
          {
            fail(event, "general-executor-stale-destroy");
            return false;
          }
          if (destroy.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
            auto* value = static_cast<D9CBuffer*>(object->value);
            if (value && value->obj)
              tombstones.push_back({destroy.identity, value->obj->handle()});
          } else if (destroy.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
            auto* value = static_cast<D9CTexture*>(object->value);
            if (value && value->obj)
              tombstones.push_back({destroy.identity, value->obj->handle()});
          } else if (destroy.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
            auto* value = static_cast<D9CSurface*>(object->value);
            if (value && value->obj)
              tombstones.push_back({destroy.identity, value->obj->handle()});
          }
          dxmt9::d3d9::render_tape_runtime::releaseOwnedObject(*object);
          ++result.resourcesReleased;
          if (result.submitted != result.completed) {
            const auto retentionBegin = options_.measurement
                ? std::chrono::steady_clock::time_point{}
                : std::chrono::steady_clock::now();
            CpuGeneralTapeDestroyWindow window{.event = event + 1u};
            // No mutation/work is allowed in this pending epoch. A wrapper
            // release must not destroy a submitted draw's concrete bytes.
            if (pendingSources.empty())
              return fail(event, "general-executor-destroy-without-source-proof");
            // Exercise real retirement at the actual completion waterline;
            // merely finding an entry before reclaim would not prove retention.
            Access::reclaimCompletedPool(fixture_.routing->queue_);
            for (const auto& owner : pendingSources) {
              if (owner.traceOrdinal >= result.trace.size() ||
                  !owner.matches(result.trace[owner.traceOrdinal]))
                return fail(event, "general-executor-destroy-owner-identity");
              window.pendingSubmissions.push_back(owner.submission);
              for (const auto& retained : owner.mipmapTextures) {
                const auto* record = fixture_.routing->queue_.pool().findTexture(retained.handle.value);
                if (!record || reinterpret_cast<std::uintptr_t>(record) != retained.recordAddress ||
                    record->lastUsedSeqId < owner.submission)
                  return fail(event, "general-executor-premature-mipmap-retirement");
                ++result.retainedMipmapTexturesAfterDestroy;
              }
              for (const auto& retained : owner.snapshots) {
                if (!Access::bufferAlive(fixture_.routing->queue_, retained.buffer))
                  return fail(event, "general-executor-premature-buffer-retirement");
                if (options_.corruptRetainedBytesForTest) {
                  auto* bytes = reinterpret_cast<std::uint8_t*>(
                      static_cast<std::uintptr_t>(retained.view.contentsAddress));
                  bytes[0] ^= 1u;
                }
                std::uint64_t digest = 1469598103934665603ull;
                detail::cpuGeneralHashBytes(digest, {
                    reinterpret_cast<const std::uint8_t*>(
                        static_cast<std::uintptr_t>(retained.view.contentsAddress)),
                    static_cast<std::size_t>(retained.view.byteSize)});
                if (digest != retained.digest)
                  return fail(event, "general-executor-retained-buffer-bytes-changed");
                ++result.retainedSnapshotsAfterDestroy;
              }
              ++window.ownersChecked;
            }
            window.retentionValid = true;
            result.destroyWindows.push_back(std::move(window));
            if (!options_.measurement) result.observeNs += elapsed(retentionBegin);
          }
          break;
        }
        case dxmt9::d3d9::RenderTapeEventType::OrderedControl:
          fail(event, "general-executor-unsupported-control");
          return false;
        case dxmt9::d3d9::RenderTapeEventType::PresentComplete:
          break;
        }
        if (replayEvent.type == d3d9::RenderTapeEventType::ObjectDefine ||
            replayEvent.type == d3d9::RenderTapeEventType::ResourceMutation) {
          if (!options_.measurement) result.setupNs += elapsed(eventBegin);
        }
      }
      if (bootstrapPending && !bootstrapDone && !replayBootstrap(
                                      plan_.events.empty()
                                          ? 0xffffffffu
                                          : plan_.events.back().eventIndex))
        return false;
      if (!options_.sourceOracle.empty() &&
          sourceOracleRow != options_.sourceOracle.size())
        return fail(plan_.events.empty() ? 0xffffffffu
                                         : plan_.events.back().eventIndex,
                    "general-executor-source-oracle-missing-row");
      if (result.completed != result.submitted)
        return fail(plan_.events.back().eventIndex, "general-executor-unresolved-schedule");
      return result.failure.empty();
    };
    try {
    result.complete = executeEvents();
    } catch (...) {
      if (result.failure.empty()) {
        result.failure = "general-executor-exception";
        result.failedEvent = plan_.events.empty() ? 0xffffffffu
                                                  : plan_.events.back().eventIndex;
      }
    }

    const auto cleanupBegin = options_.measurement
        ? std::chrono::steady_clock::time_point{}
        : std::chrono::steady_clock::now();
    // Failed execution is not an accepted sample, but it must still settle
    // every actually published no-GPU source before dropping retained owners.
    if (!result.complete) {
      (void)publishReady(result.failedEvent);
      for (std::size_t pending = 0u;
           pending < kCpuGeneralMaxSources &&
           Access::pendingCompletionCount(fixture_.routing->queue_) != 0u;
           ++pending) {
        if (!Access::acknowledgeOne(fixture_.routing->queue_) ||
            !Access::finishQueue(fixture_.routing->queue_)) break;
      }
    }
    dxmt9::d3d9::render_tape_runtime::OwnedObject* deferredResetObject = nullptr;
    for (auto it = objects.rbegin(); it != objects.rend(); ++it) {
      if (!it->value) continue;
      if (it->identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
        auto* value = static_cast<D9CBuffer*>(it->value);
        if (value && value->obj)
          tombstones.push_back({it->identity, value->obj->handle()});
      } else if (it->identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
        auto* value = static_cast<D9CTexture*>(it->value);
        if (value && value->obj)
          tombstones.push_back({it->identity, value->obj->handle()});
      } else if (it->identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
        auto* value = static_cast<D9CSurface*>(it->value);
        if (value && value->obj)
          tombstones.push_back({it->identity, value->obj->handle()});
      }
      if (options_.deferOneObjectReleaseForResetTest &&
          deferredResetObject == nullptr) {
        deferredResetObject = &*it;
        continue;
      }
      dxmt9::d3d9::render_tape_runtime::releaseOwnedObject(*it);
      ++result.resourcesReleased;
    }
    // Only Tape-owned wrappers must be gone here. Core bindings/caches may
    // legitimately retain Pool entries until releaseCoreForInspection below.
    // Sample real outstanding ownership before recovery, not the fault flag.
    const bool resetOutstandingObjectObserved = std::any_of(
        objects.begin(), objects.end(), [](const auto& object) {
          return object.value != nullptr;
        });
    if (deferredResetObject != nullptr) {
      dxmt9::d3d9::render_tape_runtime::releaseOwnedObject(*deferredResetObject);
      ++result.resourcesReleased;
    }
    fixture_.releaseCoreForInspection();
    Access::reclaimCompletedPool(fixture_.routing->queue_);
    bool resourcesGone = true;
    for (const auto& object : tombstones) {
      const auto& pool = fixture_.routing->queue_.pool();
      if (object.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER)
        resourcesGone &= !pool.findBuffer(object.nativeHandle.value);
      else if (object.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE)
        resourcesGone &= !pool.findTexture(object.nativeHandle.value);
      else if (object.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE)
        resourcesGone &= !pool.findSurface(object.nativeHandle.value);
    }
    result.resetOk = !resetOutstandingObjectObserved && resourcesGone &&
        Access::pendingCompletionCount(fixture_.routing->queue_) == 0u &&
        Access::readyCount(fixture_.routing->queue_) == 0u &&
        Access::writingCommandCount(fixture_.routing->queue_) == 0u &&
        Access::residentCount(fixture_.routing->queue_) == 0u;
    if (!options_.measurement) result.cleanupNs = elapsed(cleanupBegin);
    if (options_.measurement) {
      // A successful lifecycle/reset sample is not a semantic or pixel
      // oracle; the omitted observer counters must remain zero.
      if (result.sourceSummaryDraws != result.draws && result.failure.empty()) {
        result.failure = "general-executor-measurement-draw-conservation";
        result.complete = false;
      }
      result.digest = 0u;
      result.logicalResourceOracleComplete = false;
    } else {
      result.logicalResourceOracleComplete = result.failure.empty() &&
          result.observedMutations == plan_.mutations.size() &&
          result.drawObservations.size() == result.draws;
    }
    result.complete = result.complete && result.resetOk;
    if (!result.resetOk && result.failure.empty())
      result.failure = resetOutstandingObjectObserved
          ? "general-executor-reset-object-release-incomplete"
          : "general-executor-reset-ambiguous";
    return result;
  }

 private:
  static std::uint64_t elapsed(std::chrono::steady_clock::time_point begin) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin)
                                            .count());
  }

  bool validateSourceOracle(Result& result) const {
    if (options_.sourceOracle.empty()) return true;
    if (options_.sourceOracle.size() > kCpuSourceOracleMaxRows) {
      result.failure = "general-executor-source-oracle-row-cap";
      return false;
    }
    std::size_t row = 0u;
    for (const auto& replayEvent : plan_.events) {
      if (replayEvent.type != d3d9::RenderTapeEventType::CommandChunk) continue;
      if (row >= options_.sourceOracle.size() ||
          options_.sourceOracle[row].event != replayEvent.eventIndex + 1u) {
        result.failure = "general-executor-source-oracle-command-events";
        result.failedEvent = replayEvent.eventIndex;
        return false;
      }
      ++row;
    }
    if (row != options_.sourceOracle.size()) {
      result.failure = "general-executor-source-oracle-extra-row";
      return false;
    }
    return true;
  }

  bool admit(Result& result) const {
    if (plan_.events.size() > kCpuGeneralMaxEvents ||
        plan_.definitions.size() > kCpuGeneralMaxObjects ||
        plan_.events.size() > std::numeric_limits<std::uint32_t>::max()) {
      result.failure = "general-executor-plan-bound";
      return false;
    }
    std::size_t records = 0u;
    std::size_t draws = 0u;
    for (const auto& chunk : plan_.commandChunks) {
      dxmt9::d3d9::ImportedChunkView imported;
      if (!dxmt9::d3d9::importPrevalidatedCommandChunk(chunk.bytes,
                                                        chunk.envelope,
                                                        imported)) {
        result.failure = "general-executor-plan-chunk-invalid";
        return false;
      }
      records += imported.records.size();
      for (const auto& record : imported.records)
        draws += (record.type == D9C_COMMAND_RECORD_DRAW_PRIMITIVE ||
                  record.type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE);
    }
    for (const auto& chunk : plan_.bootstrapChunks) {
      dxmt9::d3d9::ImportedChunkView imported;
      if (!dxmt9::d3d9::importPrevalidatedCommandChunk(chunk.bytes,
                                                        chunk.envelope,
                                                        imported)) {
        result.failure = "general-executor-bootstrap-invalid";
        return false;
      }
      records += imported.records.size();
    }
    if (records > kCpuGeneralMaxCommandRecords || draws > kCpuGeneralMaxDraws ||
        plan_.events.size() > kCpuGeneralMaxSources) {
      result.failure = "general-executor-plan-cap";
      return false;
    }
    return true;
  }

  bool admitEmission(Result& result) const {
    const auto pageSize = fixture_.routing->queue_.cpuReadyArenaPlanLimits().pageSize;
    if (pageSize == 0u) {
      result.failure = "general-executor-queue-page-size";
      return false;
    }
    const auto inspect = [&](const auto& chunk, std::uint32_t event) {
      dxmt9::d3d9::ImportedChunkView imported;
      if (!dxmt9::d3d9::importPrevalidatedCommandChunk(chunk.bytes,
                                                        chunk.envelope,
                                                        imported)) {
        result.failure = "general-executor-emission-import";
        result.failedEvent = event;
        return false;
      }
      const auto emission = dxmt9::d3d9::planReplayEmission(
          imported, static_cast<dxmt9::d3d9::RawOrdinal>(event + 1u), pageSize);
      if (!emission.partitioned()) {
        result.failure = "general-executor-emission-plan";
        result.failedEvent = event;
        return false;
      }
      // A whole-raw compatibility fallback cannot split internally under
      // this fixture's pinned zero-limit policy. If production will actually
      // enter the direct span driver, require one exact whole-raw lease.
      if (options_.route == d3d9::ProviderReplayPolicy::ProductionSlot &&
          emission.rotationFreeProductionEligible() &&
          (!emission.leaseEligible() || emission.leaseSpans.size() != 1u ||
           !emission.leaseSpans.front().ownsLease ||
           emission.leaseSpans.front().firstRecordIndex != 0u ||
           emission.leaseSpans.front().recordCount != imported.records.size() ||
           !emission.leaseSpans.front().finalLeaseSpan)) {
        result.failure = "general-executor-emission-source-shape";
        result.failedEvent = event;
        return false;
      }
      return true;
    };
    for (const auto& replayEvent : plan_.events) {
      if (replayEvent.type != dxmt9::d3d9::RenderTapeEventType::CommandChunk)
        continue;
      if (replayEvent.index >= plan_.commandChunks.size() ||
          !inspect(plan_.commandChunks[replayEvent.index], replayEvent.eventIndex))
        return false;
    }
    return true;
  }

  bool observePayload(const core::SourcePayloadView& payload,
                      std::uint64_t& hash, auto&& resource, auto&& draw,
                      std::string& failure) {
    return observeCpuTapePayload(payload, hash,
                                 std::forward<decltype(resource)>(resource),
                                 std::forward<decltype(draw)>(draw), failure);
  }

  bool observeResource(
      const std::vector<d3d9::render_tape_runtime::OwnedObject>& objects,
      const std::vector<detail::LogicalReceipt>& receipts,
      std::span<const CpuGeneralTapeTombstone> tombstones, std::uint32_t kind,
      core::Handle handle, std::uint64_t& hash) const {
    for (const auto& object : objects) {
      if (!object.value || object.identity.kind != kind) continue;
      core::Handle actual{};
      D9CWireObjectIdentity contentIdentity = object.identity;
      std::optional<std::uint32_t> aliasLevel;
      if (kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
        const auto* value = static_cast<const D9CBuffer*>(object.value);
        if (value->obj) actual = value->obj->handle();
      } else if (kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
        const auto* value = static_cast<const D9CTexture*>(object.value);
        if (value->obj) actual = value->obj->handle();
      } else if (kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
        const auto* value = static_cast<const D9CSurface*>(object.value);
        if (value->obj) actual = value->obj->handle();
        if (actual == handle && value->ownerTex) {
          // A surface owns its parent wrapper independently of the Tape's
          // live-object registry. Parent ObjectDestroy (or later ABA reuse)
          // must not drop or reattribute the surviving alias's seed receipts.
          const auto definition = std::find_if(
              plan_.definitions.begin(), plan_.definitions.end(),
              [&](const auto& candidate) {
                return detail::cpuGeneralSameIdentity(candidate.fixed.identity,
                                                       object.identity);
              });
          d3d9::RenderTapeSurfaceDescriptorV2 alias{};
          if (definition == plan_.definitions.end() ||
              !d3d9::renderTapeLoadSurfaceDescriptorV2(
                  definition->descriptor, alias) ||
              alias.storage != static_cast<std::uint32_t>(
                  d3d9::RenderTapeSurfaceStorage::TextureSubresource) ||
              alias.subresource != value->ownerLevel)
            return false;
          contentIdentity = alias.parentTexture;
          aliasLevel = alias.subresource;
          if (!value->ownerTex->obj) return false;
          const auto actualParent = value->ownerTex->obj->handle();
          const bool liveParentMatches = std::any_of(
              objects.begin(), objects.end(), [&](const auto& parent) {
                if (!parent.value || !detail::cpuGeneralSameIdentity(
                        parent.identity, contentIdentity)) return false;
                const auto* texture = static_cast<const D9CTexture*>(parent.value);
                return texture->obj && texture->obj->handle() == actualParent;
              });
          const bool retiredParentMatches = std::any_of(
              tombstones.begin(), tombstones.end(), [&](const auto& parent) {
                return detail::cpuGeneralSameIdentity(
                           parent.identity, contentIdentity) &&
                       parent.nativeHandle == actualParent;
              });
          if (!liveParentMatches && !retiredParentMatches) return false;
          if (options_.dropRetiredAliasReceiptForTest && !liveParentMatches) {
            // Reproduce the old live-registry-only attribution, without
            // changing actual resource ownership or the reference digest.
            contentIdentity = object.identity;
            aliasLevel.reset();
          }
        }
      }
      if (actual != handle) continue;
      detail::cpuTapeDigestScalar(hash, object.identity.kind);
      detail::cpuTapeDigestScalar(hash, object.identity.generation);
      detail::cpuTapeDigestScalar(hash, object.identity.objectId);
      std::uint32_t count = 0u;
      for (const auto& receipt : receipts) {
        if (!detail::cpuGeneralSameIdentity(receipt.identity, contentIdentity) ||
            (aliasLevel && receipt.subresource != *aliasLevel)) continue;
        ++count;
        detail::cpuTapeDigestScalar(hash, receipt.subresource);
        detail::cpuGeneralHashDigest(hash, receipt.digest);
        detail::cpuTapeDigestScalar(hash, receipt.revision);
      }
      // A resource produced by captured GPU commands has no CPU pixel bytes.
      // Its qualified identity, seed receipts (if any), and the complete
      // producer command stream form the logical oracle, not a fake readback.
      detail::cpuTapeDigestScalar(hash, count);
      return true;
    }
    return false;
  }
  Fixture& fixture_;
  const Plan& plan_;
  std::span<const Blob> blobs_;
  Options options_{};
};

}  // namespace dxmt9::test
