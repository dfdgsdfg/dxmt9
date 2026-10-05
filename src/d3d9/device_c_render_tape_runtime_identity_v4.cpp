#include "device_c_render_tape_runtime_identity_v4.hpp"
#include "device_c_render_tape_runtime_identity_core.hpp"
#include "device_c_replay_projection.hpp"
#include "dxmt9/dxmt9_queue.hpp"
#include "util/log/log.hpp"

#include <algorithm>
#include <limits>
#include <vector>

namespace dxmt9::d3d9 {
namespace {
using Kind = RenderTapeIdentityV3DispositionKind;
using Command = RenderTapeIdentityV3CommandKind;
using Observation = ReplayTransactionObservationKind;
constexpr auto noIndex = kRenderTapeIdentityV4NoIndex;

RenderTapeIdentityV4ReplaySourceIdentity copyIdentity(ReplaySourceIdentity identity) noexcept {
  return {identity.frame, identity.source, identity.lastSource, identity.sequence};
}

bool sameIdentity(RenderTapeIdentityV4ReplaySourceIdentity left,
    ReplaySourceIdentity right) noexcept {
  return left.frame == right.frame && left.source == right.source &&
      left.lastSource == right.lastSource && left.sequence == right.sequence;
}

RenderTapeIdentityV4ReplayReceipt copyReceipt(const ReplayDestinationReceipt& receipt) noexcept {
  RenderTapeIdentityV4ReceiptKind kind{};
  switch (receipt.kind) {
  case ReplayDestinationKind::Compatibility: kind = RenderTapeIdentityV4ReceiptKind::Compatibility; break;
  case ReplayDestinationKind::DirectChunkSlot: kind = RenderTapeIdentityV4ReceiptKind::DirectChunkSlot; break;
  case ReplayDestinationKind::Arena: kind = RenderTapeIdentityV4ReceiptKind::Arena; break;
  }
  return {.identity = copyIdentity(receipt.identity), .queueSequence = receipt.queueSequence,
      .buildGeneration = receipt.buildGeneration, .sourceGeneration = receipt.sourceGeneration,
      .storageGeneration = receipt.storageGeneration, .kind = static_cast<std::uint32_t>(kind),
      .controlIndex = receipt.controlIndex, .commandCount = receipt.commandCount};
}

bool sameReceipt(const RenderTapeIdentityV4ReplayReceipt& left,
    const ReplayDestinationReceipt& right) noexcept {
  const auto copied = copyReceipt(right);
  return sameIdentity(left.identity, right.identity) && left.queueSequence == copied.queueSequence &&
      left.buildGeneration == copied.buildGeneration && left.sourceGeneration == copied.sourceGeneration &&
      left.storageGeneration == copied.storageGeneration && left.kind == copied.kind &&
      left.controlIndex == copied.controlIndex && left.commandCount == copied.commandCount;
}

bool receiptAdmitted(const ReplayTransactionState& state) noexcept {
  const auto& receipt = state.destination;
  if (!receipt.valid() || receipt.identity != state.identity ||
      receipt.commandCount != state.stagedCommandCount) return false;
  switch (receipt.kind) {
  case ReplayDestinationKind::Compatibility:
    return receipt.queueSequence == state.identity.sequence && !receipt.buildGeneration &&
        !receipt.sourceGeneration && !receipt.storageGeneration && receipt.controlIndex == noIndex;
  case ReplayDestinationKind::DirectChunkSlot: case ReplayDestinationKind::Arena:
    return true;
  }
  return false;
}
} // namespace

class RenderTapeRuntimeIdentityV4::State final : public RenderTapeRuntimeIdentityCore {
public:
  explicit State(RenderTapeRuntimeIdentityV4& owner) noexcept : owner_(owner) {}

  bool begin(RenderTapeIdentityV4CaptureKey key, render::TapeRuntimeBaseline baseline,
      RenderTapeRuntimeIdentityV4Limits limits) noexcept {
    std::lock_guard lock(mutex_);
    if (!owner_.edges().active() || !limits.maxAttempts ||
        limits.maxAttempts > kRenderTapeIdentityV4MaxRows ||
        !beginLocked(key, baseline, limits.provenance)) return reject();
    try {
      maxAttempts_ = limits.maxAttempts;
      context_.coordinatorDomain = baseline.sessionLaneEnabled
          ? TapeCoordinatorAuthority::SessionRelease : TapeCoordinatorAuthority::OrdinarySerial;
      commands_.reserve(limits.provenance.maxCommands);
      publications_.reserve(limits.provenance.maxSources);
      publicationReceipts_.reserve(limits.provenance.maxSources);
      attempts_.reserve(limits.maxAttempts);
      settlements_.reserve(limits.provenance.maxSettlements);
      extents_.reserve(limits.provenance.maxCommands);
      coordinatorAdmissions_.reserve(limits.provenance.maxCoordinators);
      writerCheckpoint_.reserve(std::size_t{limits.provenance.maxSources} +
          limits.provenance.maxCoordinators);
      if (!owner_.topology().begin({key.token, key.epoch, baseline.queue},
              render::TapeRuntimeEdgeClock::baseline(), limits.topology)) return reject();
      return true;
    } catch (...) { return reject(); }
  }

  bool seedQueryBaselines(RenderTapeIdentityV4CaptureKey key,
      std::span<const RenderTapeRuntimeQueryBaselineV4> baselines) noexcept {
    std::lock_guard lock(mutex_);
    // Retirement/freeze must not damage the retained immutable transport.
    if (!owner_.edges().active() || !active_ || failed_ || frozen_) return false;
    if (queryBaselinesSeeded_ || key.token != context_.captureToken ||
        key.epoch != context_.captureEpoch || attemptActive_ || rollbackPending_ ||
        !attempts_.empty() || !settlements_.empty() || !commands_.empty() ||
        !publications_.empty() || !extents_.empty() || owner_.topology().failed()) return reject();
    if (!seedQueryBaselinesLocked(baselines)) return false;
    queryBaselinesSeeded_ = true;
    return true;
  }

  bool observeReplay(const ReplayTransactionObservation& value) noexcept {
    std::lock_guard lock(mutex_);
    if (!owner_.edges().active()) return false;
    if (!active_ || failed_ || frozen_ || owner_.topology().failed() ||
        !value.transaction || !value.state || value.raw.captureToken != context_.captureToken ||
        !value.raw.eventOrdinal || !value.raw.rawOrdinal ||
        !value.state->identity.valid() || value.state->identity.sequence != value.raw.rawOrdinal ||
        value.state->nextRecordOrdinal > value.raw.totalRawRecords) return reject();
    // Both pointers are synchronous borrows. Only actual value fields below
    // survive this callback; the TLS driver owns address/scope correlation.
    const auto& state = *value.state;
    try {
      if (value.kind == Observation::Begin) return beginAttempt(value.raw, state);
      if (value.raw != raw_ || !sameIdentity(current_.identity, state.identity)) return reject();
      if (value.kind == Observation::DestinationRollbackConfirmed)
        return confirmRollback(state);
      if (!attemptActive_ || rollbackPending_) return reject();
      switch (value.kind) {
      case Observation::FirstEffect:
        if (state.terminal() ||
            (value.effect == ReplayTransactionEffectWitness::Transaction &&
             state.phase != ReplayTransactionPhase::EffectStarted) ||
            (value.effect != ReplayTransactionEffectWitness::Transaction &&
             value.effect != ReplayTransactionEffectWitness::DirectLease &&
             value.effect != ReplayTransactionEffectWitness::ArenaEmitted)) return reject();
        if (!current_.effectEdge) current_.effectEdge = edge();
        return !failed_;
      case Observation::DestinationAccepted:
        if (destinationSeen_ || recordActive_ || state.phase != ReplayTransactionPhase::DestinationReceived ||
            state.nextRecordOrdinal != nextRecord_ || !receiptAdmitted(state)) return reject();
        current_.receipt = copyReceipt(state.destination);
        destinationSeen_ = true;
        return true;
      case Observation::Committed: return commitAttempt(state);
      case Observation::RolledBack: return holdRollback(state);
      case Observation::FailedStop: return reject();
      case Observation::Begin: case Observation::DestinationRollbackConfirmed: break;
      }
      return reject();
    } catch (...) { return reject(); }
  }

  bool bindEmission(query::WriterIdentity identity, std::uint32_t physical,
      core::MetalCommandKind actualKind, std::span<const RenderTapeRuntimeRecordLocator> locators,
      std::uint32_t firstSubdraw) noexcept {
    std::lock_guard lock(mutex_);
    if (!owner_.edges().active()) return false;
    if (!active_ || failed_ || frozen_ || rollbackPending_ || owner_.topology().failed() || locators.empty() ||
        locators.size() > limits_.maxDispositions) return reject();
    const auto kind = commandKind(actualKind);
    if (kind == Command::Invalid ||
        (kind != Command::DrawRun && (locators.size() != 1u || firstSubdraw)) ||
        std::uint64_t{firstSubdraw} + locators.size() > UINT32_MAX ||
        !identity.valid() || identity.queue != context_.queue || physical >= limits_.maxCommands)
      return reject();
    try {
      const auto priorWriter = std::find_if(writers_.begin(), writers_.end(),
          [&](const auto& owner) { return owner.identity == identity; });
      if (priorWriter != writers_.end() && priorWriter->sourceIndex != noIndex) return reject();
      // Validate the complete call before binding any record. Locators are
      // supplied by the actual assembler, never found by matching kind.
      for (std::size_t index = 0u; index < locators.size(); ++index) {
        const auto locator = locators[index];
        const auto found = std::find_if(records_.begin(), records_.end(), [&](const auto& record) {
          return record.disposition.eventOrdinal == locator.eventOrdinal &&
              record.disposition.firstRecord == locator.recordIndex;
        });
        if (found == records_.end()) return reject();
        const auto recordIndex = static_cast<std::size_t>(found - records_.begin());
        if (attemptActive_ && !current_.effectEdge && recordIndex < checkpoint_.records)
          return reject();
        if (kind == Command::DrawRun) {
          if (consumedDraws_ + index >= pendingDraws_.size() ||
              pendingDraws_[consumedDraws_ + index] != recordIndex ||
              found->disposition.kind != static_cast<std::uint32_t>(Kind::GpuCommand) ||
              found->writer.valid() || found->physicalCommand != noIndex ||
              renderTapeIdentityV3RecordCommandKind(found->type) != kind) return reject();
        } else if (found->disposition.kind == static_cast<std::uint32_t>(Kind::Coordinator)) {
          if (!renderTapeIdentityV3CoordinatorCommandAdmitted(found->type, kind) ||
              generated_.size() >= limits_.maxCommands) return reject();
          const auto coordinator = found->disposition.coordinatorIndex == noIndex &&
                  recordActive_ && recordIndex == currentRecord_
              ? static_cast<std::uint32_t>(coordinators_.size()) : found->disposition.coordinatorIndex;
          if (coordinator == noIndex) return reject();
          for (const auto& generated : generated_)
            if (generated.writer == identity && generated.command == physical) return reject();
        } else if (found->disposition.kind != static_cast<std::uint32_t>(Kind::GpuCommand) ||
            found->writer.valid() || found->physicalCommand != noIndex ||
            renderTapeIdentityV3RecordCommandKind(found->type) != kind) return reject();
      }
      auto* owner = writer(identity);
      if (!owner) return false;
      for (std::size_t index = 0u; index < locators.size(); ++index) {
        const auto locator = locators[index];
        auto found = std::find_if(records_.begin(), records_.end(), [&](const auto& record) {
          return record.disposition.eventOrdinal == locator.eventOrdinal &&
              record.disposition.firstRecord == locator.recordIndex;
        });
        if (found->disposition.kind == static_cast<std::uint32_t>(Kind::Coordinator)) {
          const auto coordinator = found->disposition.coordinatorIndex == noIndex
              ? static_cast<std::uint32_t>(coordinators_.size()) : found->disposition.coordinatorIndex;
          generated_.push_back({identity, physical, coordinator,
              locator.eventOrdinal, locator.recordIndex});
        } else {
          found->writer = identity;
          found->physicalCommand = physical;
          if (kind == Command::DrawRun) {
            found->disposition.firstSubdraw = firstSubdraw + static_cast<std::uint32_t>(index);
            found->disposition.subdrawCount = 1u;
          }
        }
      }
      if (kind == Command::DrawRun) consumedDraws_ += locators.size();
      owner->commandCount = std::max(owner->commandCount, physical + 1u);
      return true;
    } catch (...) { return reject(); }
  }

  bool acknowledgeRaw(ReplayTransactionRawIdentity raw, bool succeeded) noexcept {
    std::lock_guard lock(mutex_);
    if (!active_ || failed_ || frozen_ || owner_.topology().failed() || !succeeded ||
        raw != raw_ || rawAcknowledged_ || attemptActive_ || rollbackPending_ ||
        eventActive_ || recordActive_ || !haveCommitted_ || committedEnd_ != raw.totalRawRecords ||
        settlements_.size() >= limits_.maxSettlements) return reject();
    try {
      settlements_.push_back({.eventOrdinal = raw.eventOrdinal, .rawOrdinal = raw.rawOrdinal,
          .replayEdge = edge(), .recordCount = raw.totalRawRecords});
      if (failed_) return false;
      rawAcknowledged_ = true;
      cutKind_ = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterEvent);
      cutRecord_ = 0u;
      return true;
    } catch (...) { return reject(); }
  }

  bool snapshot(RenderTapeIdentityV4CaptureKey key, RenderTapeIdentityV4View& out,
      RenderTapeExecutionTopologyV1View& topology, bool requireComplete) noexcept {
    out = {};
    topology = {};
    std::lock_guard lock(mutex_);
    if (owner_.topology().failed()) return reject();
    if (key.token != context_.captureToken || key.epoch != context_.captureEpoch || !readyLocked(requireComplete) ||
        publications_.size() != sources_.size() || (requireComplete &&
        (attemptActive_ || rollbackPending_ || (raw_.rawOrdinal && !rawAcknowledged_)))) return false;
    try {
      render::TapeTopologyRuntimeSnapshot actual;
      if (!owner_.topology().snapshot(actual, requireComplete)) return false;
      if (!copyProvenanceLocked(out)) return false;
      topology.header.captureToken = actual.capture.token;
      topology.header.captureEpoch = actual.capture.epoch;
      topology.header.queue = actual.capture.queue;
      topology.plans = std::move(actual.plans);
      topology.placements = std::move(actual.placements);
      return true;
    } catch (...) { out = {}; topology = {}; return false; }
  }

  bool snapshotControlledCpu(RenderTapeIdentityV4CaptureKey key,
      RenderTapeIdentityV4View& out) noexcept {
    out = {};
    std::lock_guard lock(mutex_);
    if (!frozen_ || owner_.edges().active() || owner_.topology().failed() ||
        key.token != context_.captureToken || key.epoch != context_.captureEpoch ||
        !readyLocked(true) || publications_.size() != sources_.size() ||
        publicationReceipts_.size() != sources_.size() || attemptActive_ || rollbackPending_ ||
        (raw_.rawOrdinal && !rawAcknowledged_)) return false;
    try { return copyProvenanceLocked(out); }
    catch (...) { out = {}; return false; }
  }

  bool copyPublishedSourceOrigins(render::TapeTopologyCaptureIdentity capture,
      core::CpuReadyTape::SourceRef source, std::uint64_t sequence,
      RenderTapeRuntimePublishedSourceOriginsV4& out) noexcept {
    out = {};
    core::metalqueue::ReplaySourcePublicationReceipt publication;
    {
      std::lock_guard lock(mutex_);
      if (!active_ || failed_ || owner_.topology().failed() || !capture.valid() ||
          capture.token != context_.captureToken || capture.epoch != context_.captureEpoch ||
          capture.queue != context_.queue || !source.valid() || !sequence ||
          publicationReceipts_.size() != sources_.size() || publications_.size() != sources_.size() ||
          publicationReceipts_.size() > limits_.maxSources) return false;
      bool found = false;
      for (const auto& receipt : publicationReceipts_) {
        if (receipt.source != source || receipt.metadata.seqId != sequence) continue;
        if (found) return false;
        publication = receipt;
        found = true;
      }
      if (!found) return false;
    }
    // Only the owned value receipt crosses the unlock. Exact projection
    // validation reacquires the root mutex and rechecks capture/generations;
    // no queue mutex, raw ACK or source storage borrow participates.
    return copyPublishedSourceOrigins(capture, publication, out);
  }

  bool copyPublishedSourceOrigins(render::TapeTopologyCaptureIdentity capture,
      const core::metalqueue::ReplaySourcePublicationReceipt& publication,
      RenderTapeRuntimePublishedSourceOriginsV4& out) noexcept {
    out = {};
    std::lock_guard lock(mutex_);
    if (!active_ || failed_ || owner_.topology().failed() || !capture.valid() ||
        capture.token != context_.captureToken || capture.epoch != context_.captureEpoch ||
        capture.queue != context_.queue || !publication.source.valid() ||
        !publication.metadata.valid() || publicationReceipts_.size() != sources_.size() ||
        publications_.size() != sources_.size()) return false;
    const auto receipt = std::find_if(publicationReceipts_.begin(), publicationReceipts_.end(),
        [&](const auto& value) {
          return value.source == publication.source && value.metadata == publication.metadata &&
              value.controlIndex == publication.controlIndex && value.hasPresent == publication.hasPresent;
        });
    if (receipt == publicationReceipts_.end()) return false;
    const auto sourceIndex = static_cast<std::uint32_t>(receipt - publicationReceipts_.begin());
    const auto& lifecycle = sources_[sourceIndex];
    const auto owner = std::find_if(writers_.begin(), writers_.end(), [&](const auto& value) {
      return value.sourceIndex == sourceIndex && value.identity.queue == capture.queue &&
          value.identity.index == publication.source.id.index &&
          value.identity.generation == publication.source.id.generation &&
          value.identity.storageGeneration == publication.source.storage.generation &&
          value.captureId == lifecycle.writerCaptureId && value.commandCount == lifecycle.commandCount;
    });
    if (owner == writers_.end() || lifecycle.seqId != publication.metadata.seqId ||
        lifecycle.sourceOrdinal != publication.metadata.sourceOrdinal ||
        lifecycle.writerGeneration != publication.source.id.generation ||
        lifecycle.storageGeneration != publication.source.storage.generation || !lifecycle.commandCount ||
        lifecycle.commandCount > limits_.maxCommands) return false;
    try {
      RenderTapeRuntimePublishedSourceOriginsV4 candidate;
      candidate.capture = capture;
      candidate.source = {receipt->source, lifecycle.sourceOrdinal, lifecycle.seqId,
          lifecycle.publicationEdge, sourceIndex, lifecycle.commandCount};
      candidate.metadata = receipt->metadata;
      candidate.controlIndex = receipt->controlIndex;
      candidate.hasPresent = receipt->hasPresent;
      candidate.commands.reserve(lifecycle.commandCount);
      // Every draw locator is bounded by maxDispositions; generated commands
      // are independently bounded by maxCommands. No payload is consulted.
      const auto originLimit = std::size_t{limits_.maxDispositions} + limits_.maxCommands;
      for (std::size_t index = 0u; index < commands_.size(); ++index) {
        const auto command = commands_[index];
        if (command.sourceIndex != sourceIndex) continue;
        if (command.physicalCommandIndex != candidate.commands.size() ||
            candidate.commands.size() >= lifecycle.commandCount) return false;
        const bool draw = command.kind == static_cast<std::uint32_t>(Command::DrawRun);
        if (draw != (command.drawCount != 0u) || (draw && command.coordinatorIndex != noIndex))
          return false;
        std::uint32_t covered = 0u;
        for (const auto& record : records_) {
          if (record.writer != owner->identity || record.physicalCommand != command.physicalCommandIndex)
            continue;
          const auto& disposition = record.disposition;
          if (command.coordinatorIndex != noIndex || !disposition.eventOrdinal ||
              disposition.recordCount != 1u || disposition.kind != static_cast<std::uint32_t>(Kind::GpuCommand) ||
              disposition.commandIndex != index || command.kind != static_cast<std::uint32_t>(
                  renderTapeIdentityV3RecordCommandKind(record.type)) ||
              (draw ? disposition.firstSubdraw != covered || disposition.subdrawCount != 1u ||
                          covered >= command.drawCount
                    : disposition.firstSubdraw || disposition.subdrawCount || covered) ||
              candidate.origins.size() >= originLimit) return false;
          candidate.origins.push_back({command.physicalCommandIndex, command.kind,
              disposition.firstSubdraw, disposition.subdrawCount,
              {disposition.eventOrdinal, disposition.firstRecord}});
          ++covered;
        }
        for (const auto& generated : generated_) {
          if (generated.writer != owner->identity || generated.command != command.physicalCommandIndex)
            continue;
          if (draw || covered || command.coordinatorIndex == noIndex ||
              command.coordinatorIndex != generated.coordinator || !generated.eventOrdinal ||
              candidate.origins.size() >= originLimit) return false;
          const auto record = std::find_if(records_.begin(), records_.end(), [&](const auto& value) {
            return value.disposition.eventOrdinal == generated.eventOrdinal &&
                value.disposition.firstRecord == generated.recordIndex;
          });
          if (record == records_.end() || record->disposition.recordCount != 1u ||
              record->disposition.kind != static_cast<std::uint32_t>(Kind::Coordinator) ||
              !renderTapeIdentityV3CoordinatorCommandAdmitted(record->type, static_cast<Command>(command.kind)))
            return false;
          candidate.origins.push_back({command.physicalCommandIndex, command.kind, 0u, 0u,
              {generated.eventOrdinal, generated.recordIndex}});
          ++covered;
        }
        if (covered != (draw ? command.drawCount : 1u)) return false;
        candidate.commands.push_back(command);
      }
      if (candidate.commands.size() != lifecycle.commandCount) return false;
      out = std::move(candidate);
      return true;
    } catch (...) { out = {}; return false; }
  }

  bool copyPublishedSources(RenderTapeIdentityV4CaptureKey key, std::size_t& cursor,
      std::vector<core::metalqueue::ReplaySourcePublicationReceipt>& out) noexcept {
    out.clear();
    std::lock_guard lock(mutex_);
    if (!active_ || failed_ || frozen_ || owner_.topology().failed() ||
        key.token != context_.captureToken || key.epoch != context_.captureEpoch ||
        cursor > publicationReceipts_.size()) return false;
    try {
      out.assign(publicationReceipts_.begin() + cursor, publicationReceipts_.end());
      cursor = publicationReceipts_.size();
      return true;
    } catch (...) { out.clear(); return false; }
  }

  bool ownsWritingContinuation(RenderTapeIdentityV4CaptureKey key, query::WriterIdentity identity,
      core::CpuReadyTape::SourceRef source, std::size_t count) noexcept {
    std::lock_guard lock(mutex_);
    if (!owner_.edges().active() || !active_ || failed_ || frozen_ || attemptActive_ ||
        rollbackPending_ || (raw_.rawOrdinal && !rawAcknowledged_) || !count ||
        key.token != context_.captureToken || key.epoch != context_.captureEpoch ||
        !identity.valid() || !source.valid() || identity.queue != context_.queue ||
        identity.index != source.id.index || identity.generation != source.id.generation ||
        identity.storageGeneration != source.storage.generation) return false;
    const auto found = std::find_if(writers_.begin(), writers_.end(), [&](const auto& writer) {
      return writer.identity == identity && writer.sourceIndex == noIndex && writer.commandCount == count;
    });
    return found != writers_.end();
  }

  bool freeze(RenderTapeIdentityV4CaptureKey key, std::uint64_t presentOrdinal) noexcept {
    std::lock_guard lock(mutex_);
    if (owner_.topology().failed()) return reject();
    if (!active_ || failed_ || !presentOrdinal || eventActive_ || recordActive_ ||
        attemptActive_ || rollbackPending_ || (raw_.rawOrdinal && !rawAcknowledged_) ||
        key.token != context_.captureToken || key.epoch != context_.captureEpoch ||
        (frozen_ && context_.presentOrdinal != presentOrdinal)) return false;
    context_.presentOrdinal = presentOrdinal;
    frozen_ = true;
    return true;
  }

private:
  bool copyProvenanceLocked(RenderTapeIdentityV4View& out) {
    if (coordinatorAdmissions_.size() != coordinators_.size()) return reject();
    context_.stamp(out.header);
    out.sources.reserve(sources_.size());
    for (std::size_t index = 0u; index < sources_.size(); ++index) {
      const auto extra = publications_[index];
      out.sources.push_back({sources_[index], extra.rawOrdinal, extra.buildGeneration, extra.controlIndex});
    }
    out.initializers = initializers_;
    out.commands = commands_;
    out.coordinators.reserve(coordinators_.size());
    for (std::size_t index = 0u; index < coordinators_.size(); ++index)
      out.coordinators.push_back({.provenance = coordinators_[index],
          .admissionEdge = coordinatorAdmissions_[index].admissionEdge,
          .authority = static_cast<std::uint32_t>(coordinatorAdmissions_[index].authority)});
    out.settlements = settlements_;
    out.attempts = attempts_;
    copyDispositions(out.dispositions);
    return true;
  }

  bool strictQueryContinuity() const noexcept override { return queryBaselinesSeeded_; }
  bool queryMutationAdmitted() const noexcept override {
    return !queryBaselinesSeeded_ || (attemptActive_ && !rollbackPending_ && current_.effectEdge);
  }

  void onRecordBegin() noexcept override { ordinaryAdmissionEdge_ = 0u; }

  bool sessionReleaseAdmitted() const noexcept override {
    return context_.coordinatorDomain == TapeCoordinatorAuthority::SessionRelease &&
        !release_.valid() && !ordinaryAdmissionEdge_;
  }

  bool admitOrdinaryCoordinator(const render::TapeOrdinaryCoordinatorAdmission& value) noexcept override {
    if (!active_ || failed_ || frozen_ || !attemptActive_ || rollbackPending_ ||
        !recordActive_ || !eventActive_ || ordinaryAdmissionEdge_ || release_.valid() ||
        context_.coordinatorDomain != TapeCoordinatorAuthority::OrdinarySerial ||
        records_[currentRecord_].disposition.kind != static_cast<std::uint32_t>(Kind::Coordinator) ||
        value.queue != context_.queue || value.captureToken != context_.captureToken ||
        value.captureEpoch != context_.captureEpoch || value.eventOrdinal != eventOrdinal_ ||
        value.rawOrdinal != rawOrdinal_ || value.attemptGeneration != buildGeneration_ ||
        value.recordIndex != nextRecord_ || value.recordType != records_[currentRecord_].type ||
        cutRecord_ != nextRecord_ || !current_.effectEdge)
      return reject();
    ordinaryAdmissionEdge_ = edge();
    return !failed_;
  }

  bool finishCoordinatorAdmission(RenderTapeIdentityV3Coordinator& control) override {
    if (context_.coordinatorDomain == TapeCoordinatorAuthority::SessionRelease) {
      if (ordinaryAdmissionEdge_ || !RenderTapeRuntimeIdentityCore::finishCoordinatorAdmission(control))
        return false;
    } else if (context_.coordinatorDomain != TapeCoordinatorAuthority::OrdinarySerial ||
        !ordinaryAdmissionEdge_ || release_.valid() || ordinaryAdmissionEdge_ >= control.dispatchEdge)
      return false;
    coordinatorAdmissions_.push_back({context_.coordinatorDomain, ordinaryAdmissionEdge_});
    return true;
  }

  TapeCoordinatorAdmissionEvidence coordinatorAdmission(std::uint32_t index) const noexcept override {
    return index < coordinatorAdmissions_.size() ? coordinatorAdmissions_[index]
        : TapeCoordinatorAdmissionEvidence{static_cast<TapeCoordinatorAuthority>(0u), 0u};
  }

  struct Publication {
    std::uint64_t rawOrdinal = 0u, buildGeneration = 0u;
    std::uint32_t controlIndex = noIndex;
  };

  bool beginAttempt(ReplayTransactionRawIdentity raw, const ReplayTransactionState& state) {
    if (state.phase != ReplayTransactionPhase::Created || attemptActive_ || rollbackPending_ ||
        eventActive_ || recordActive_ || attempts_.size() >= maxAttempts_ ||
        !nextAttemptGeneration_ || nextAttemptGeneration_ == UINT64_MAX) return reject();
    if (raw_.rawOrdinal && raw != raw_) {
      if (!rawAcknowledged_ || raw.eventOrdinal <= raw_.eventOrdinal ||
          raw.rawOrdinal <= raw_.rawOrdinal) return reject();
    } else if (raw_.rawOrdinal &&
        (rawAcknowledged_ || !sameIdentity(rawIdentity_, state.identity))) return reject();
    if (!raw_.rawOrdinal || raw != raw_) {
      raw_ = raw;
      rawIdentity_ = copyIdentity(state.identity);
      committedEnd_ = 0u;
      haveCommitted_ = rawAcknowledged_ = false;
    }
    // Actual committed spans must cover a raw contiguously. A failed local
    // attempt may start after that frontier, but never rerun the prefix.
    if (state.nextRecordOrdinal < committedEnd_) return reject();
    const auto generation = nextAttemptGeneration_++;
    if (!beginContextLocked(raw.captureToken, raw.eventOrdinal, raw.rawOrdinal, generation,
            state.nextRecordOrdinal, raw.totalRawRecords)) return false;
    checkpoint_ = checkpoint();
    // A tentative append can advance a shared Writing writer's physical
    // count, or register a new candidate writer. Restore those exact prior
    // values on confirmed rollback; this bounded pre-state is not a second
    // lifecycle/provenance ledger.
    writerCheckpoint_ = writers_;
    checkpointSources_ = sources_.size();
    checkpointInitializers_ = initializers_.size();
    current_ = {.eventOrdinal = raw.eventOrdinal, .rawOrdinal = raw.rawOrdinal,
        .attemptGeneration = generation, .beginEdge = edge(), .identity = copyIdentity(state.identity),
        .firstRecord = state.nextRecordOrdinal};
    destinationSeen_ = false;
    attemptActive_ = !failed_;
    return !failed_;
  }

  bool commitAttempt(const ReplayTransactionState& state) {
    if (recordActive_ || state.phase != ReplayTransactionPhase::Committed || !destinationSeen_ ||
        current_.firstRecord != committedEnd_ || state.nextRecordOrdinal != nextRecord_ ||
        !receiptAdmitted(state) || !sameReceipt(current_.receipt, state.destination)) return reject();
    current_.recordCount = state.nextRecordOrdinal - current_.firstRecord;
    current_.stagedCommandCount = state.stagedCommandCount;
    current_.outcome = static_cast<std::uint32_t>(RenderTapeIdentityV4AttemptOutcome::Committed);
    current_.terminalEdge = edge();
    if (failed_) return false;
    attempts_.push_back(current_);
    committedEnd_ = state.nextRecordOrdinal;
    haveCommitted_ = true;
    attemptActive_ = eventActive_ = false;
    return true;
  }

  bool holdRollback(const ReplayTransactionState& state) {
    // A failed record can still be open: project advanced its actual cursor,
    // while dispatch never returned success. This candidate is removed only
    // after separate destination rollback succeeds.
    const auto maximumCursor = std::uint64_t{nextRecord_} + (recordActive_ ? 1u : 0u);
    if (state.phase != ReplayTransactionPhase::RolledBack || current_.effectEdge || destinationSeen_ ||
        sources_.size() != checkpointSources_ || initializers_.size() != checkpointInitializers_ ||
        state.nextRecordOrdinal < nextRecord_ || state.nextRecordOrdinal > maximumCursor ||
        state.nextRecordOrdinal < current_.firstRecord) return reject();
    current_.recordCount = state.nextRecordOrdinal - current_.firstRecord;
    current_.stagedCommandCount = state.stagedCommandCount;
    current_.outcome = static_cast<std::uint32_t>(RenderTapeIdentityV4AttemptOutcome::RolledBack);
    current_.terminalEdge = edge();
    if (failed_) return false;
    attemptActive_ = false;
    rollbackPending_ = true;
    return true;
  }

  bool confirmRollback(const ReplayTransactionState& state) {
    if (!rollbackPending_ || attemptActive_ || state.phase != ReplayTransactionPhase::RolledBack ||
        state.nextRecordOrdinal != std::uint64_t{current_.firstRecord} + current_.recordCount ||
        state.stagedCommandCount != current_.stagedCommandCount || !restoreRecords(checkpoint_))
      return reject();
    writers_ = writerCheckpoint_;
    attempts_.push_back(current_);
    rollbackPending_ = false;
    // Restored state is the serial boundary at which the next real begin can
    // retry; the abandoned candidate does not become a canonical disposition.
    nextRecord_ = current_.firstRecord;
    cutKind_ = static_cast<std::uint32_t>(nextRecord_ ?
        RenderTapeIdentityV3CutKind::AfterRecord : RenderTapeIdentityV3CutKind::BeforeEvent);
    cutRecord_ = nextRecord_;
    return true;
  }

  bool append(const render::TapeRuntimeObservation& value) override {
    if (rollbackPending_ || owner_.topology().failed() || !value.payload.valid() ||
        !value.writer.valid() || value.writer.queue != context_.queue ||
        value.commandIndex >= value.payload.commandCount()) return reject();
    // The physical observer is a verification witness, never a locator
    // allocator. Explicit successful append hooks must already have bound
    // every original record; V3 retains its historical Append policy.
    const auto command = value.payload.commandAt(value.commandIndex).command;
    const auto kind = commandKind(command.kind);
    if (kind == Command::Invalid) return reject();
    if (kind == Command::DrawRun) {
      if (!value.subdrawCount || std::uint64_t{value.firstSubdraw} + value.subdrawCount >
          command.drawParams.size()) return reject();
      for (std::uint32_t index = 0u; index < value.subdrawCount; ++index) {
        const auto record = std::find_if(records_.begin(), records_.end(), [&](const auto& record) {
          return record.writer == value.writer && record.physicalCommand == value.commandIndex &&
              record.disposition.firstSubdraw == value.firstSubdraw + index &&
              record.disposition.subdrawCount == 1u &&
              renderTapeIdentityV3RecordCommandKind(record.type) == Command::DrawRun;
        });
        if (record == records_.end()) return reject();
      }
      return true;
    }
    if (value.subdrawCount || value.firstSubdraw) return reject();
    for (const auto& record : records_)
      if (record.writer == value.writer && record.physicalCommand == value.commandIndex &&
          renderTapeIdentityV3RecordCommandKind(record.type) == kind) return true;
    for (const auto& generated : generated_)
      if (generated.writer == value.writer && generated.command == value.commandIndex) return true;
    return reject();
  }

  bool publish(const render::TapeRuntimeObservation& value) override {
    if (owner_.topology().failed() || !value.source.valid() || value.controlIndex > UINT32_MAX ||
        value.writer.index != value.source.id.index || value.writer.generation != value.source.id.generation ||
        value.writer.storageGeneration != value.source.storage.generation || !value.payload.valid() ||
        value.payload.commandCount() > limits_.maxCommands - commands_.size()) return reject();
    extents_.clear();
    for (std::uint32_t physical = 0u; physical < value.payload.commandCount(); ++physical) {
      const auto actual = value.payload.commandAt(physical).command;
      const auto kind = commandKind(actual.kind);
      if (kind == Command::Invalid || actual.drawParams.size() > UINT32_MAX) return reject();
      render::TapeTopologyPhysicalExtent extent{physical, static_cast<std::uint32_t>(kind),
          kind == Command::DrawRun ? static_cast<std::uint32_t>(actual.drawParams.size()) : 0u};
      for (const auto& generated : generated_)
        if (generated.writer == value.writer && generated.command == physical) {
          if (extent.coordinatorIndex != noIndex) return reject();
          extent.coordinatorIndex = generated.coordinator;
        }
      std::uint32_t covered = 0u;
      for (const auto& record : records_) {
        if (record.writer != value.writer || record.physicalCommand != physical) continue;
        if (extent.coordinatorIndex != noIndex ||
            record.disposition.kind != static_cast<std::uint32_t>(Kind::GpuCommand) ||
            renderTapeIdentityV3RecordCommandKind(record.type) != kind) return reject();
        if (kind == Command::DrawRun) {
          if (record.disposition.firstSubdraw != covered || record.disposition.subdrawCount != 1u ||
              covered >= extent.drawCount) return reject();
          ++covered;
        } else if (covered++) return reject();
      }
      if (extent.coordinatorIndex == noIndex &&
          covered != (kind == Command::DrawRun ? extent.drawCount : 1u)) return reject();
      extents_.push_back(extent);
    }
    const auto commandBase = commands_.size();
    const auto index = registerPublication(value, commandBase);
    if (index == noIndex || failed_) return false;
    publications_.push_back({value.metadata.rawOrdinal, value.metadata.buildGeneration,
                            static_cast<std::uint32_t>(value.controlIndex)});
    for (const auto extent : extents_)
      commands_.push_back({index, extent.physicalCommandIndex, extent.kind, extent.drawCount,
                          extent.coordinatorIndex});
    if (!finishPublication(value, index, commandBase)) return false;
    const auto& lifecycle = sources_[index];
    if (!owner_.topology().publish({value.source, lifecycle.sourceOrdinal, lifecycle.seqId,
            lifecycle.publicationEdge, index, lifecycle.commandCount}, extents_)) return reject();
    publicationReceipts_.push_back({value.source, value.metadata, value.controlIndex,
        value.payload.presentRecordCount() != 0u});
    return true;
  }

  std::uint32_t sourceIndex(const render::TapeRuntimeObservation& value) const override {
    render::TapeTopologySourceIdentity exact;
    if (!owner_.topology().sourceIdentity(value.source, value.sequence, exact) ||
        exact.sourceIndex >= sources_.size()) return noIndex;
    const auto& lifecycle = sources_[exact.sourceIndex];
    return lifecycle.seqId == exact.sequence && lifecycle.sourceOrdinal == exact.sourceOrdinal &&
            lifecycle.publicationEdge == exact.publicationEdge && lifecycle.commandCount == exact.commandCount
        ? exact.sourceIndex : noIndex;
  }

  std::uint64_t edge() noexcept override {
    const auto result = owner_.edges().next();
    if (!result) reject();
    return result;
  }
  void onFirstFailure(const RenderTapeRuntimeIdentityFailure& failure) noexcept override {
    try {
      util::logf(util::LogLevel::Info, "dxmt9-render-tape-v4",
          "first_failure phase=root_reject token=%llu epoch=%llu queue=%llu "
          "event=%llu raw=%llu attempt=%llu cut=%u/%u record=%u type=%u "
          "event_active=%u record_active=%u file=%s line=%u function=%s",
          static_cast<unsigned long long>(failure.captureToken),
          static_cast<unsigned long long>(failure.captureEpoch),
          static_cast<unsigned long long>(failure.queue),
          static_cast<unsigned long long>(failure.eventOrdinal),
          static_cast<unsigned long long>(failure.rawOrdinal),
          static_cast<unsigned long long>(failure.attemptGeneration),
          failure.cutKind, failure.cutRecord, failure.recordIndex, failure.recordType,
          static_cast<unsigned>(failure.eventActive), static_cast<unsigned>(failure.recordActive),
          failure.location.file_name(), static_cast<unsigned>(failure.location.line()),
          failure.location.function_name());
    } catch (...) {} // Diagnostic allocation cannot change fail-stop behavior.
  }
  void onFailure() noexcept override {
    owner_.edges().revoke();
    owner_.topology().fail();
  }

  RenderTapeRuntimeIdentityV4& owner_; // Non-owning parent; no lease or self-cycle.
  std::vector<RenderTapeIdentityV4PhysicalCommand> commands_{};
  std::vector<Publication> publications_{};
  std::vector<core::metalqueue::ReplaySourcePublicationReceipt> publicationReceipts_{};
  std::vector<RenderTapeIdentityV4ReplayAttempt> attempts_{};
  std::vector<RenderTapeIdentityV4RawSettlement> settlements_{};
  std::vector<render::TapeTopologyPhysicalExtent> extents_{};
  std::vector<Writer> writerCheckpoint_{};
  std::vector<TapeCoordinatorAdmissionEvidence> coordinatorAdmissions_{};
  std::uint64_t ordinaryAdmissionEdge_ = 0u;
  ReplayTransactionRawIdentity raw_{};
  RenderTapeIdentityV4ReplaySourceIdentity rawIdentity_{};
  RenderTapeIdentityV4ReplayAttempt current_{};
  RecordCheckpoint checkpoint_{};
  std::size_t checkpointSources_ = 0u, checkpointInitializers_ = 0u;
  std::uint64_t nextAttemptGeneration_ = 1u;
  std::uint32_t maxAttempts_ = 0u, committedEnd_ = 0u;
  bool attemptActive_ = false, rollbackPending_ = false, destinationSeen_ = false;
  bool queryBaselinesSeeded_ = false;
  bool haveCommitted_ = false, rawAcknowledged_ = false;
};

RenderTapeRuntimeIdentityV4::RenderTapeRuntimeIdentityV4() : state_(std::make_unique<State>(*this)) {}
RenderTapeRuntimeIdentityV4::~RenderTapeRuntimeIdentityV4() = default;

bool RenderTapeRuntimeIdentityV4::begin(RenderTapeIdentityV4CaptureKey key,
    render::TapeRuntimeBaseline baseline, RenderTapeRuntimeIdentityV4Limits limits) noexcept {
  return state_->begin(key, baseline, limits);
}
bool RenderTapeRuntimeIdentityV4::seedQueryBaselines(RenderTapeIdentityV4CaptureKey key,
    std::span<const RenderTapeRuntimeQueryBaselineV4> baselines) noexcept {
  return state_->seedQueryBaselines(key, baselines);
}
render::TapeRuntimeObserverSink RenderTapeRuntimeIdentityV4::sink() noexcept { return state_->sink(); }
ReplayTransactionObserverSink RenderTapeRuntimeIdentityV4::replaySink() noexcept {
  return {this, observeReplay, replayFailed};
}
bool RenderTapeRuntimeIdentityV4::observeReplay(void* context,
    const ReplayTransactionObservation& observation) noexcept {
  return static_cast<RenderTapeRuntimeIdentityV4*>(context)->state_->observeReplay(observation);
}
void RenderTapeRuntimeIdentityV4::replayFailed(void* context,
    ReplayTransactionObserverFailure) noexcept {
  static_cast<RenderTapeRuntimeIdentityV4*>(context)->fail();
}
bool RenderTapeRuntimeIdentityV4::beginRecord(std::uint32_t index, std::uint32_t type) noexcept {
  return edges().active() && state_->beginRecord(index, type);
}
bool RenderTapeRuntimeIdentityV4::beginDispatch() noexcept {
  return edges().active() && state_->beginDispatch();
}
bool RenderTapeRuntimeIdentityV4::endRecord(std::int32_t result) noexcept {
  return edges().active() && state_->endRecord(result);
}
bool RenderTapeRuntimeIdentityV4::bindEmission(query::WriterIdentity writer,
    std::uint32_t physical, core::MetalCommandKind kind,
    std::span<const RenderTapeRuntimeRecordLocator> records, std::uint32_t firstSubdraw) noexcept {
  return state_->bindEmission(writer, physical, kind, records, firstSubdraw);
}
bool RenderTapeRuntimeIdentityV4::acknowledgeRaw(ReplayTransactionRawIdentity raw, bool succeeded) noexcept {
  return edges().active() && state_->acknowledgeRaw(raw, succeeded);
}
bool RenderTapeRuntimeIdentityV4::setCut(std::uint64_t event, RenderTapeIdentityV3CutKind kind,
    std::uint32_t record) noexcept {
  return edges().active() && state_->setCut(event, kind, record);
}
void RenderTapeRuntimeIdentityV4::queryIssue(D9CWireObjectIdentity identity, std::uint32_t flags,
    const core::QueryPollSnapshot& snapshot, std::int32_t result, std::uint64_t cycle) noexcept {
  if (edges().active()) state_->queryIssue(identity, flags, snapshot, result, cycle);
}
void RenderTapeRuntimeIdentityV4::queryPoll(D9CWireObjectIdentity identity,
    const core::QueryPollSnapshot& snapshot, std::int32_t result) noexcept {
  if (edges().active()) state_->queryPoll(identity, snapshot, result);
}
bool RenderTapeRuntimeIdentityV4::snapshot(RenderTapeIdentityV4CaptureKey key,
    RenderTapeIdentityV4View& identity, RenderTapeExecutionTopologyV1View& topology,
    bool requireComplete) noexcept { return state_->snapshot(key, identity, topology, requireComplete); }
bool RenderTapeRuntimeIdentityV4::snapshotControlledCpu(RenderTapeIdentityV4CaptureKey key,
    RenderTapeIdentityV4View& identity) noexcept { return state_->snapshotControlledCpu(key, identity); }
bool RenderTapeRuntimeIdentityV4::copyPublishedSourceOrigins(render::TapeTopologyCaptureIdentity capture,
    core::CpuReadyTape::SourceRef source, std::uint64_t sequence,
    RenderTapeRuntimePublishedSourceOriginsV4& out) noexcept {
  return state_->copyPublishedSourceOrigins(capture, source, sequence, out);
}
bool RenderTapeRuntimeIdentityV4::copyPublishedSourceOrigins(render::TapeTopologyCaptureIdentity capture,
    const core::metalqueue::ReplaySourcePublicationReceipt& publication,
    RenderTapeRuntimePublishedSourceOriginsV4& out) noexcept {
  return state_->copyPublishedSourceOrigins(capture, publication, out);
}
bool RenderTapeRuntimeIdentityV4::copyPublishedSources(RenderTapeIdentityV4CaptureKey key,
    std::size_t& cursor, std::vector<core::metalqueue::ReplaySourcePublicationReceipt>& out) noexcept {
  return state_->copyPublishedSources(key, cursor, out);
}
bool RenderTapeRuntimeIdentityV4::ownsWritingContinuation(RenderTapeIdentityV4CaptureKey key,
    query::WriterIdentity writer, core::CpuReadyTape::SourceRef source, std::size_t count) noexcept {
  return state_->ownsWritingContinuation(key, writer, source, count);
}
void RenderTapeRuntimeIdentityV4::fail(std::source_location location) noexcept {
  if (edges().active()) state_->fail(location);
}
RenderTapeRuntimeIdentityFailure RenderTapeRuntimeIdentityV4::firstFailure() const noexcept {
  return state_->firstFailure();
}
RenderTapeRuntimeIdentityFailure RenderTapeRuntimeIdentityV4::diagnosticState() const noexcept {
  return state_->diagnosticState();
}
bool RenderTapeRuntimeIdentityV4::failed() const noexcept {
  if (topology_.failed()) {
    // A backend-local topology rejection is a failure of this same retained
    // root, not permission to keep allocating provenance/acceptance edges.
    if (const_cast<RenderTapeRuntimeIdentityV4*>(this)->edges().active()) state_->fail();
    return true;
  }
  return state_->failed();
}
bool RenderTapeRuntimeIdentityV4::matches(RenderTapeIdentityV4CaptureKey key) const noexcept {
  return state_->matches(key);
}
bool RenderTapeRuntimeIdentityV4::freeze(RenderTapeIdentityV4CaptureKey key,
    std::uint64_t presentOrdinal) noexcept { return state_->freeze(key, presentOrdinal); }
bool RenderTapeRuntimeIdentityV4::frozen() const noexcept { return state_->frozen(); }

} // namespace dxmt9::d3d9
