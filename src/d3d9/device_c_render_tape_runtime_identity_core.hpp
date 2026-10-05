#pragma once

#include "device_c_render_tape_identity_v3.hpp"
#include "device_c_render_tape_provenance_validation.hpp"
#include "../dxmt9/dxmt9_render_tape_runtime_observer.hpp"
#include "dxmt9/query_poll_snapshot.hpp"

#include <mutex>
#include <source_location>
#include <span>
#include <vector>

namespace dxmt9::d3d9 {

struct RenderTapeRuntimeQueryBaselineV4;

// Cold first-rejection evidence only. The pointers refer to compiler-owned
// source strings; no caller storage, replay payload or wire record is retained.
struct RenderTapeRuntimeIdentityFailure {
  std::source_location location{};
  std::uint64_t captureToken = 0u, captureEpoch = 0u, queue = 0u;
  std::uint64_t eventOrdinal = 0u, rawOrdinal = 0u, attemptGeneration = 0u;
  std::uint32_t cutKind = 0u, cutRecord = 0u, recordIndex = 0u, recordType = 0u;
  bool eventActive = false, recordActive = false;
  bool active = false, failed = false, frozen = false;
};

// Actual capture/lifetime context only: no wire version, byte digest, table
// descriptor, graph/pass membership, replay settlement or edge allocator.
struct RenderTapeRuntimeIdentityContext : TapeProvenanceContext {
  std::uint64_t presentOrdinal = 0u;

  template <typename Header>
  void stamp(Header& header) const noexcept {
    header.frameId = header.captureToken = captureToken;
    header.captureEpoch = captureEpoch;
    header.queue = queue;
    header.presentOrdinal = presentOrdinal;
    header.baselineSequence = baselineSequence;
    header.baselineCompletedSequence = baselineCompletedSequence;
    header.baselineInitializer = baselineInitializer;
    header.baselineCompletedInitializer = baselineCompletedInitializer;
    header.initializerEpoch = initializerEpoch;
    header.baselineCompletionEdge = baselineCompletionEdge;
    header.baselineKind = baselineKind;
    if constexpr (requires { header.coordinatorDomain; })
      header.coordinatorDomain = static_cast<std::uint32_t>(coordinatorDomain);
  }
};

// Shared canonical lifecycle/record/query semantics. The canonical row types
// below are common to V3 and V4; this is not a V3 view later converted to V4.
// Version policies own publication command tables, attempt/settlement rules
// and edge sources. The core never builds a graph or infers a V4 transaction.
class RenderTapeRuntimeIdentityCore {
public:
  virtual ~RenderTapeRuntimeIdentityCore() = default;
  render::TapeRuntimeObserverSink sink() noexcept { return {this, observe}; }
  bool beginRecord(std::uint32_t recordIndex, std::uint32_t recordType) noexcept;
  bool beginDispatch() noexcept;
  bool endRecord(std::int32_t result) noexcept;
  bool setCut(std::uint64_t eventOrdinal, RenderTapeIdentityV3CutKind kind,
      std::uint32_t record = 0u) noexcept;
  void queryIssue(D9CWireObjectIdentity identity, std::uint32_t flags,
      const core::QueryPollSnapshot& snapshot, std::int32_t result,
      std::uint64_t issueCycle = 0u) noexcept;
  void queryPoll(D9CWireObjectIdentity identity,
      const core::QueryPollSnapshot& snapshot, std::int32_t result) noexcept;
  void fail(std::source_location location = std::source_location::current()) noexcept;
  bool failed() const noexcept;
  RenderTapeRuntimeIdentityFailure firstFailure() const noexcept;
  // Current cursor evidence has an empty location, unlike the sticky failure.
  RenderTapeRuntimeIdentityFailure diagnosticState() const noexcept;
  bool matches(RenderTapeIdentityV3CaptureKey key) const noexcept;
  bool freeze(RenderTapeIdentityV3CaptureKey key, std::uint64_t presentOrdinal) noexcept;
  bool frozen() const noexcept;

protected:
  struct Record {
    RenderTapeIdentityV3Disposition disposition{};
    std::uint32_t type = 0u;
    query::WriterIdentity writer{};
    std::uint32_t physicalCommand = kRenderTapeIdentityV3NoIndex;
  };
  struct Writer {
    query::WriterIdentity identity{};
    std::uint64_t captureId = 0u;
    std::uint32_t sourceIndex = kRenderTapeIdentityV3NoIndex;
    std::uint32_t commandCount = 0u;
  };
  struct QueryIssue {
    D9CWireObjectIdentity identity{};
    std::uint64_t ordinal = 0u, epoch = 0u, cycle = 0u;
    std::uint64_t coreIdentity = 0u;
    query::EndReceipt receipt{};
    std::uint32_t queryType = 0u;
    std::uint32_t coordinator = kRenderTapeIdentityV3NoIndex;
    bool active = false, completedBaseline = false;
  };
  struct GeneratedCommand {
    query::WriterIdentity writer{};
    std::uint32_t command = 0u;
    std::uint32_t coordinator = kRenderTapeIdentityV3NoIndex;
    // V4 records this at actual append, before the coordinator can finish.
    // V3 does not expose the source-origin pull and leaves these defaulted.
    std::uint64_t eventOrdinal = 0u;
    std::uint32_t recordIndex = 0u;
  };
  struct RecordCheckpoint {
    std::size_t records = 0u, pendingDraws = 0u, consumedDraws = 0u;
    std::size_t generated = 0u, coordinators = 0u, queries = 0u;
  };
  bool beginLocked(RenderTapeIdentityV3CaptureKey key, render::TapeRuntimeBaseline baseline,
      RenderTapeIdentityV3Limits limits) noexcept;
  bool seedQueryBaselinesLocked(
      std::span<const RenderTapeRuntimeQueryBaselineV4> baselines) noexcept;
  bool beginContextLocked(std::uint64_t token, std::uint64_t eventOrdinal,
      std::uint64_t rawOrdinal, std::uint64_t attemptGeneration,
      std::uint32_t firstRecord, std::uint32_t recordCount) noexcept;
  bool readyLocked(bool requireComplete) const noexcept;
  void copyDispositions(std::vector<RenderTapeIdentityV3Disposition>& out) const;
  Writer* writer(query::WriterIdentity owner);
  virtual bool append(const render::TapeRuntimeObservation& observation);
  std::uint32_t registerPublication(const render::TapeRuntimeObservation& observation,
      std::size_t existingCommands);
  bool finishPublication(const render::TapeRuntimeObservation& observation,
      std::uint32_t sourceIndex, std::size_t commandBase);
  RecordCheckpoint checkpoint() const noexcept;
  bool restoreRecords(RecordCheckpoint checkpoint) noexcept;
  bool reject(std::source_location location = std::source_location::current()) noexcept {
    const bool first = !failed_;
    if (first) {
      firstFailure_ = diagnosticStateLocked(location);
    }
    failed_ = true;
    onFailure();
    if (first) onFirstFailure(firstFailure_);
    return false;
  }
  virtual bool publish(const render::TapeRuntimeObservation&) = 0;
  virtual std::uint32_t sourceIndex(const render::TapeRuntimeObservation&) const = 0;
  virtual std::uint64_t edge() noexcept = 0;
  virtual void onFirstFailure(const RenderTapeRuntimeIdentityFailure&) noexcept {}
  virtual void onFailure() noexcept {}
  virtual void onRecordBegin() noexcept {}
  virtual bool sessionReleaseAdmitted() const noexcept { return true; }
  virtual bool strictQueryContinuity() const noexcept { return false; }
  virtual bool queryMutationAdmitted() const noexcept { return true; }
  virtual bool admitOrdinaryCoordinator(const render::TapeOrdinaryCoordinatorAdmission&) noexcept {
    return reject(); // Legacy V3 has no ordinary authority.
  }
  virtual bool finishCoordinatorAdmission(RenderTapeIdentityV3Coordinator& control);
  virtual TapeCoordinatorAdmissionEvidence coordinatorAdmission(std::uint32_t) const noexcept { return {}; }
  static RenderTapeIdentityV3CommandKind commandKind(core::MetalCommandKind) noexcept;
  RenderTapeRuntimeIdentityFailure diagnosticStateLocked(
      std::source_location location = {}) const noexcept {
    return {.location = location,
        .captureToken = context_.captureToken, .captureEpoch = context_.captureEpoch,
        .queue = context_.queue, .eventOrdinal = eventOrdinal_, .rawOrdinal = rawOrdinal_,
        .attemptGeneration = buildGeneration_, .cutKind = cutKind_, .cutRecord = cutRecord_,
        .recordIndex = nextRecord_,
        .recordType = recordActive_ && currentRecord_ < records_.size()
            ? records_[currentRecord_].type : 0u,
        .eventActive = eventActive_, .recordActive = recordActive_,
        .active = active_, .failed = failed_, .frozen = frozen_};
  }

  mutable std::mutex mutex_{};
  RenderTapeRuntimeIdentityContext context_{};
  RenderTapeIdentityV3Limits limits_{};
  std::vector<RenderTapeIdentityV3Source> sources_{};
  std::vector<RenderTapeIdentityV3Initializer> initializers_{};
  std::vector<RenderTapeIdentityV3Coordinator> coordinators_{};
  std::vector<Writer> writers_{};
  std::vector<Record> records_{};
  std::vector<std::size_t> pendingDraws_{};
  std::size_t consumedDraws_ = 0u;
  std::vector<QueryIssue> queries_{};
  std::vector<GeneratedCommand> generated_{};
  std::uint64_t eventOrdinal_ = 0u, rawOrdinal_ = 0u, buildGeneration_ = 0u;
  std::uint32_t recordCount_ = 0u, nextRecord_ = 0u;
  std::size_t currentRecord_ = 0u;
  std::uint32_t cutKind_ = 0u, cutRecord_ = 0u;
  core::metalqueue::SessionReleaseSnapshot release_{};
  core::metalqueue::SessionReleaseWriter releaseWriter_ =
      core::metalqueue::SessionReleaseWriter::Publish;
  RenderTapeIdentityV3Coordinator currentCoordinator_{};
  query::WriterIdentity currentEndWriter_{};
  std::uint64_t currentEndSequence_ = 0u, currentEndInitializer_ = 0u;
  std::uint32_t currentEndCursor_ = 0u;
  bool recordActive_ = false, eventActive_ = false;
  bool active_ = false, failed_ = false, frozen_ = false;
  RenderTapeRuntimeIdentityFailure firstFailure_{};

private:
  friend struct RenderTapeRuntimeIdentityV4TestAccess;
  static void observe(void*, const render::TapeRuntimeObservation&) noexcept;
  void observeLocked(const render::TapeRuntimeObservation&);
  bool finishRecord(std::int32_t result);
};

} // namespace dxmt9::d3d9
