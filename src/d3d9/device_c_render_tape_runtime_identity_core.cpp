#include "device_c_render_tape_runtime_identity_core.hpp"
#include "device_c_render_tape_runtime_identity_v4.hpp"
#include "device_c_common.hpp"

#include <algorithm>
#include <limits>

namespace dxmt9::d3d9 {
namespace {
using Kind = RenderTapeIdentityV3DispositionKind;
using Prefix = RenderTapeIdentityV3PrefixKind;
using Phase = RenderTapeIdentityV3SourcePhase;
using Command = RenderTapeIdentityV3CommandKind;
constexpr auto noIndex = kRenderTapeIdentityV3NoIndex;

bool sameFence(query::Fence left, query::Fence right) noexcept {
  return left.queue == right.queue && left.sequence == right.sequence &&
      left.initializer == right.initializer;
}

Command runtimeCommandKind(core::MetalCommandKind kind) noexcept {
  switch (kind) {
  case core::MetalCommandKind::DrawRun: return Command::DrawRun;
  case core::MetalCommandKind::Clear: return Command::Clear;
  case core::MetalCommandKind::SurfaceCopy: return Command::SurfaceCopy;
  case core::MetalCommandKind::StretchRect: return Command::StretchRect;
  case core::MetalCommandKind::Readback: return Command::Readback;
  case core::MetalCommandKind::ColorFill: return Command::ColorFill;
  case core::MetalCommandKind::DepthResolve: return Command::DepthResolve;
  case core::MetalCommandKind::GenerateMipmaps: return Command::GenerateMipmaps;
  case core::MetalCommandKind::Present: return Command::Present;
  }
  return Command::Invalid;
}

std::uint32_t wireQueryType(core::QueryType type) noexcept {
  switch (type) {
  case core::QueryType::Event: return 8u;
  case core::QueryType::Occlusion: return 9u;
  case core::QueryType::Timestamp: return 10u;
  case core::QueryType::TimestampDisjoint: return 11u;
  case core::QueryType::TimestampFreq: return 12u;
  }
  return 0u;
}
} // namespace


RenderTapeIdentityV3CommandKind RenderTapeRuntimeIdentityCore::commandKind(
    core::MetalCommandKind kind) noexcept {
  return runtimeCommandKind(kind);
}

bool RenderTapeRuntimeIdentityCore::beginLocked(RenderTapeIdentityV3CaptureKey key,
    render::TapeRuntimeBaseline baseline, RenderTapeIdentityV3Limits limits) noexcept {
  if (active_ || !key.token || !key.epoch || !baseline.queue ||
      baseline.sequence != baseline.completedSequence ||
      baseline.initializer != baseline.completedInitializer ||
      !renderTapeIdentityV3LimitsAdmitted(limits) ||
      !limits.maxSources || !limits.maxCommands || !limits.maxDispositions ||
      !limits.maxCoordinators || !limits.maxSettlements) return reject();
  try {
    context_ = {};
    static_cast<TapeProvenanceContext&>(context_) = {
        .captureToken = key.token, .captureEpoch = key.epoch, .queue = baseline.queue,
        .baselineSequence = baseline.sequence, .baselineInitializer = baseline.initializer,
        .baselineCompletedSequence = baseline.completedSequence, .initializerEpoch = baseline.queue,
        .baselineCompletedInitializer = baseline.completedInitializer, .baselineCompletionEdge = 1u,
        .baselineKind = static_cast<std::uint32_t>(
            baseline.sequence ? Prefix::ArmBaseline : Prefix::EmptyBaseline)};
    limits_ = limits;
    sources_.reserve(limits.maxSources);
    initializers_.reserve(limits.maxInitializers);
    coordinators_.reserve(limits.maxCoordinators);
    writers_.reserve(limits.maxSources + limits.maxCoordinators);
    records_.reserve(limits.maxDispositions);
    pendingDraws_.reserve(limits.maxDispositions);
    queries_.reserve(limits.maxCoordinators);
    generated_.reserve(limits.maxCommands);
    active_ = true;
    return true;
  } catch (...) { return reject(); }
}

bool RenderTapeRuntimeIdentityCore::seedQueryBaselinesLocked(
    std::span<const RenderTapeRuntimeQueryBaselineV4> baselines) noexcept {
  if (!active_ || failed_ || frozen_ || eventActive_ || recordActive_ || eventOrdinal_ ||
      rawOrdinal_ || buildGeneration_ || !queries_.empty() || !records_.empty() ||
      !coordinators_.empty() || !sources_.empty() || !initializers_.empty() ||
      !writers_.empty() || !generated_.empty() || baselines.size() > limits_.maxCoordinators)
    return reject();
  const query::Progress completed{context_.queue, context_.baselineSequence,
      context_.baselineCompletedSequence, false, context_.baselineInitializer,
      context_.baselineCompletedInitializer};
  try {
    std::vector<QueryIssue> admitted;
    admitted.reserve(baselines.size());
    for (const auto& baseline : baselines) {
      const auto identity = baseline.localIdentity;
      const auto& snapshot = baseline.snapshot;
      // Binding separately proves live wrapper/device/registry ownership.
      // Here a fresh replay query's one actual, completed setup END is the
      // only allowed local starting state, including a genuine empty prefix.
      if (identity.kind != D9C_CHUNK_HANDLE_KIND_QUERY || !identity.generation ||
          !identity.objectId || snapshot.type != core::QueryType::Event ||
          !snapshot.identity || snapshot.issueOrdinal != 1u || !snapshot.issueDeviceEpoch ||
          snapshot.active || snapshot.failed || !snapshot.eligible ||
          query::classifyEnd(snapshot.backendFence, completed) != query::Poll::Complete)
        return reject();
      for (const auto& prior : admitted)
        if (renderTapeIdentityV3SameIdentity(prior.identity, identity) ||
            prior.coreIdentity == snapshot.identity || prior.epoch != snapshot.issueDeviceEpoch)
          return reject();
      admitted.push_back({.identity = identity, .ordinal = snapshot.issueOrdinal,
          .epoch = snapshot.issueDeviceEpoch, .cycle = 1u, .coreIdentity = snapshot.identity,
          .receipt = snapshot.backendFence, .queryType = 8u,
          .completedBaseline = true});
    }
    // No authority is published until the entire batch has passed, and no
    // coordinator, source, edge or actual Issue is manufactured by admission.
    queries_.swap(admitted);
    return true;
  } catch (...) { return reject(); }
}

bool RenderTapeRuntimeIdentityCore::beginContextLocked(std::uint64_t token,
    std::uint64_t eventOrdinal, std::uint64_t rawOrdinal, std::uint64_t attemptGeneration,
    std::uint32_t firstRecord, std::uint32_t recordCount) noexcept {
  if (!active_ || failed_ || frozen_ || eventActive_ || recordActive_ ||
      token != context_.captureToken || !eventOrdinal || eventOrdinal < eventOrdinal_ ||
      !rawOrdinal || !attemptGeneration || attemptGeneration == UINT64_MAX ||
      firstRecord > recordCount) return reject();
  eventOrdinal_ = eventOrdinal;
  rawOrdinal_ = rawOrdinal;
  buildGeneration_ = attemptGeneration;
  recordCount_ = recordCount;
  nextRecord_ = firstRecord;
  cutKind_ = static_cast<std::uint32_t>(firstRecord ?
      RenderTapeIdentityV3CutKind::AfterRecord : RenderTapeIdentityV3CutKind::BeforeEvent);
  cutRecord_ = firstRecord;
  eventActive_ = true;
  return true;
}

std::uint32_t RenderTapeRuntimeIdentityCore::registerPublication(
    const render::TapeRuntimeObservation& value, std::size_t existingCommands) {
  if (!eventOrdinal_ || !cutKind_ || !value.metadata.valid() || !value.payload.valid() ||
      !value.payload.commandCount() || value.payload.commandCount() > UINT32_MAX ||
      existingCommands > limits_.maxCommands ||
      value.payload.commandCount() > limits_.maxCommands - existingCommands ||
      sources_.size() >= limits_.maxSources ||
      value.sequence != value.metadata.seqId ||
      value.sequence <= context_.baselineCompletedSequence ||
      (!sources_.empty() &&
       (value.sequence <= sources_.back().seqId ||
        value.metadata.sourceOrdinal <= sources_.back().sourceOrdinal))) { reject(); return noIndex; }
  auto* owner = writer(value.writer);
  if (!owner || owner->sourceIndex != noIndex) { reject(); return noIndex; }
  const auto index = static_cast<std::uint32_t>(sources_.size());
  owner->sourceIndex = index;
  owner->commandCount = static_cast<std::uint32_t>(value.payload.commandCount());
  sources_.push_back({.sourceOrdinal = value.metadata.sourceOrdinal,
      .seqId = value.sequence, .writerCaptureId = owner->captureId,
      .writerGeneration = value.writer.generation,
      .storageGeneration = value.writer.storageGeneration, .publicationEdge = edge(),
      .cutEventOrdinal = eventOrdinal_, .commandCount = owner->commandCount,
      .cutKind = cutKind_, .cutRecord = cutRecord_});

  return index;
}

bool RenderTapeRuntimeIdentityCore::finishPublication(
    const render::TapeRuntimeObservation& value, std::uint32_t index, std::size_t commandBase) {
  auto* owner = writer(value.writer);
  if (!owner || index >= sources_.size()) return reject();
  for (auto& record : records_)
    if (record.writer == value.writer) {
      if (record.physicalCommand >= owner->commandCount) return reject();
      record.disposition.commandIndex = static_cast<std::uint32_t>(commandBase) + record.physicalCommand;
    }
  for (auto& control : coordinators_) {
    if (control.prefixKind != static_cast<std::uint32_t>(Prefix::Unbound) ||
        control.writerCaptureId != owner->captureId) continue;
    const auto& source = sources_[index];
    if (!renderTapeIdentityV3BindPrefixAdmitted(control, source, value.writer, value.writer))
      return reject();
    control.prefixKind = static_cast<std::uint32_t>(Prefix::RealSource);
    control.sourceIndex = index;
    control.prefixSequence = value.sequence;
  }
  return !failed_;
}


bool RenderTapeRuntimeIdentityCore::readyLocked(bool requireComplete) const noexcept {
  if (!active_ || failed_ || (requireComplete &&
      (eventActive_ || recordActive_ || consumedDraws_ != pendingDraws_.size()))) return false;
  if (!requireComplete) return true;
  for (const auto& source : sources_)
    if (renderTapeIdentityV3SourcePhase(source) != Phase::Completed) return false;
  for (const auto& upload : initializers_)
    if (renderTapeIdentityV3InitializerPhase(upload) != Phase::Completed) return false;
  for (const auto& record : records_)
    if (record.disposition.kind == static_cast<std::uint32_t>(Kind::GpuCommand) &&
        record.disposition.commandIndex == noIndex) return false;
  return true;
}

void RenderTapeRuntimeIdentityCore::copyDispositions(
    std::vector<RenderTapeIdentityV3Disposition>& out) const {
  out.clear();
  out.reserve(records_.size());
  for (const auto& record : records_) out.push_back(record.disposition);
}

RenderTapeRuntimeIdentityCore::RecordCheckpoint RenderTapeRuntimeIdentityCore::checkpoint() const noexcept {
  return {records_.size(), pendingDraws_.size(), consumedDraws_, generated_.size(),
      coordinators_.size(), queries_.size()};
}

bool RenderTapeRuntimeIdentityCore::restoreRecords(RecordCheckpoint saved) noexcept {
  if (saved.records > records_.size() || saved.pendingDraws > pendingDraws_.size() ||
      saved.generated > generated_.size() || saved.coordinators != coordinators_.size() ||
      saved.queries != queries_.size()) return reject();
  records_.resize(saved.records);
  pendingDraws_.resize(saved.pendingDraws);
  consumedDraws_ = saved.consumedDraws;
  generated_.resize(saved.generated);
  recordActive_ = eventActive_ = false;
  return true;
}

bool RenderTapeRuntimeIdentityCore::setCut(std::uint64_t eventOrdinal,
    RenderTapeIdentityV3CutKind kind, std::uint32_t record) noexcept {
  std::lock_guard lock(mutex_);
  if (!active_ || failed_ || frozen_ || recordActive_ || !eventOrdinal ||
      (kind != RenderTapeIdentityV3CutKind::AfterRecord && record) ||
      (kind == RenderTapeIdentityV3CutKind::AfterRecord && !record)) return reject();
  eventOrdinal_ = eventOrdinal;
  cutKind_ = static_cast<std::uint32_t>(kind);
  cutRecord_ = record;
  return true;
}


bool RenderTapeRuntimeIdentityCore::beginRecord(std::uint32_t index,
    std::uint32_t type) noexcept {
  std::lock_guard lock(mutex_);
  if (!active_ || failed_ || frozen_ || !eventActive_ || recordActive_ ||
      index != nextRecord_ || index >= recordCount_ ||
      records_.size() >= limits_.maxDispositions) return reject();
  Kind kind{};
  if (renderTapeIdentityV3DispositionAdmitted(type, Kind::StateOnly)) kind = Kind::StateOnly;
  else if (renderTapeIdentityV3DispositionAdmitted(type, Kind::Coordinator)) kind = Kind::Coordinator;
  else if (renderTapeIdentityV3DispositionAdmitted(type, Kind::GpuCommand)) kind = Kind::GpuCommand;
  else return reject();
  try {
    currentRecord_ = records_.size();
    records_.push_back({.disposition = {.eventOrdinal = eventOrdinal_,
        .firstRecord = index, .recordCount = 1u,
        .kind = static_cast<std::uint32_t>(kind)}, .type = type});
    if (kind == Kind::GpuCommand && renderTapeIdentityV3RecordCommandKind(type) == Command::DrawRun)
      pendingDraws_.push_back(currentRecord_);
    currentCoordinator_ = {};
    currentEndWriter_ = {};
    currentEndSequence_ = currentEndInitializer_ = 0u;
    currentEndCursor_ = 0u;
    release_ = {};
    recordActive_ = true;
    onRecordBegin();
    // Before dispatch, publication triggered by an ordered release belongs
    // to the exact boundary preceding this canonical record.
    cutKind_ = static_cast<std::uint32_t>(index ?
        RenderTapeIdentityV3CutKind::AfterRecord : RenderTapeIdentityV3CutKind::BeforeEvent);
    cutRecord_ = index;
    return true;
  } catch (...) { return reject(); }
}


bool RenderTapeRuntimeIdentityCore::beginDispatch() noexcept {
  std::lock_guard lock(mutex_);
  if (failed_ || !recordActive_) return reject();
  cutKind_ = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterRecord);
  cutRecord_ = nextRecord_ + 1u;
  return true;
}


RenderTapeRuntimeIdentityCore::Writer* RenderTapeRuntimeIdentityCore::writer(
    query::WriterIdentity owner) {
  if (!owner.valid() || owner.queue != context_.queue) { reject(); return nullptr; }
  for (auto& value : writers_) if (value.identity == owner) return &value;
  if (writers_.size() >= std::size_t{limits_.maxSources} + limits_.maxCoordinators) {
    reject(); return nullptr;
  }
  writers_.push_back({owner, static_cast<std::uint64_t>(writers_.size()) + 1u});
  return &writers_.back();
}


bool RenderTapeRuntimeIdentityCore::append(const render::TapeRuntimeObservation& value) {
  auto* owner = writer(value.writer);
  if (!owner || owner->sourceIndex != noIndex || !value.payload.valid() ||
      value.commandIndex >= value.payload.commandCount()) return reject();
  const auto command = value.payload.commandAt(value.commandIndex).command;
  const auto kind = commandKind(command.kind);
  if (kind == Command::Invalid) return reject();
  owner->commandCount = static_cast<std::uint32_t>(value.payload.commandCount());
  if (kind == Command::DrawRun) {
    if (!value.subdrawCount || std::uint64_t{value.firstSubdraw} + value.subdrawCount >
        command.drawParams.size() || value.subdrawCount > pendingDraws_.size() - consumedDraws_)
      return reject();
    for (std::uint32_t index = 0u; index < value.subdrawCount; ++index) {
      auto& record = records_[pendingDraws_[consumedDraws_++]];
      if (record.writer.valid() || record.physicalCommand != noIndex) return reject();
      record.writer = value.writer;
      record.physicalCommand = value.commandIndex;
      record.disposition.firstSubdraw = value.firstSubdraw + index;
      record.disposition.subdrawCount = 1u;
    }
    return true;
  }
  if (!recordActive_) return reject();
  auto& record = records_[currentRecord_];
  if (record.disposition.kind == static_cast<std::uint32_t>(Kind::Coordinator)) {
    if (!renderTapeIdentityV3CoordinatorCommandAdmitted(record.type, kind) ||
        generated_.size() >= limits_.maxCommands) return reject();
    generated_.push_back({value.writer, value.commandIndex,
                         static_cast<std::uint32_t>(coordinators_.size())});
    return true;
  }
  if (record.disposition.kind != static_cast<std::uint32_t>(Kind::GpuCommand) ||
      record.writer.valid() || kind != renderTapeIdentityV3RecordCommandKind(record.type))
    return reject();
  record.writer = value.writer;
  record.physicalCommand = value.commandIndex;
  return true;
}


void RenderTapeRuntimeIdentityCore::observe(void* context,
    const render::TapeRuntimeObservation& observation) noexcept {
  auto& runtime = *static_cast<RenderTapeRuntimeIdentityCore*>(context);
  std::lock_guard lock(runtime.mutex_);
  if (!runtime.active_ || runtime.failed_ || runtime.frozen_) return;
  try { runtime.observeLocked(observation); }
  catch (...) { runtime.reject(); }
}


void RenderTapeRuntimeIdentityCore::observeLocked(
    const render::TapeRuntimeObservation& value) {
  using Edge = render::TapeRuntimeEdge;
  switch (value.edge) {
  case Edge::Append: (void)append(value); return;
  case Edge::Publish: (void)publish(value); return;
  case Edge::Submit: case Edge::Complete: {
    const auto index = sourceIndex(value);
    if (index == noIndex) { reject(); return; }
    const bool admitted = value.edge == Edge::Submit
        ? renderTapeIdentityV3SubmitSource(sources_[index], edge())
        : renderTapeIdentityV3CompleteSource(sources_[index], edge(), value.completedSequence);
    if (!admitted) reject();
    return;
  }
  case Edge::InitializerPublish: {
    if (!eventOrdinal_ || !cutKind_ || initializers_.size() >= limits_.maxInitializers ||
        value.sequence <= context_.baselineCompletedInitializer ||
        (!initializers_.empty() && value.sequence <= initializers_.back().sequence)) {
      reject(); return;
    }
    initializers_.push_back({.sequence = value.sequence,
        .epoch = context_.initializerEpoch, .publicationEdge = edge(),
        .cutEventOrdinal = eventOrdinal_, .cutKind = cutKind_, .cutRecord = cutRecord_});
    return;
  }
  case Edge::InitializerSubmit: case Edge::InitializerComplete: {
    if (value.edge == Edge::InitializerComplete &&
        value.completedSequence <= context_.baselineCompletedInitializer) return;
    bool found = false;
    for (auto& initializer : initializers_) {
      if (value.edge == Edge::InitializerSubmit && initializer.sequence != value.sequence) continue;
      if (value.edge == Edge::InitializerComplete && initializer.sequence > value.completedSequence) continue;
      found = true;
      if (value.edge == Edge::InitializerComplete &&
          renderTapeIdentityV3InitializerPhase(initializer) == Phase::Completed) continue;
      const bool admitted = value.edge == Edge::InitializerSubmit
          ? renderTapeIdentityV3SubmitInitializer(initializer, edge())
          : renderTapeIdentityV3CompleteInitializer(initializer, edge(), value.completedSequence);
      if (!admitted) { reject(); return; }
    }
    if (!found) reject();
    return;
  }
  case Edge::OrderedRelease:
    if (!recordActive_ || !value.release.valid() ||
        value.release.event.fenceRawOrdinal != rawOrdinal_ ||
        !sessionReleaseAdmitted()) { reject(); return; }
    release_ = value.release;
    releaseWriter_ = value.releaseWriter;
    return;
  case Edge::OrdinaryCoordinatorAdmission:
    (void)admitOrdinaryCoordinator(value.ordinaryAdmission);
    return;
  case Edge::QueryEnd:
    if (!recordActive_ || records_[currentRecord_].type != D9C_COMMAND_RECORD_QUERY_ISSUE) {
      reject(); return;
    }
    currentEndWriter_ = value.writer;
    currentEndSequence_ = value.sequence;
    currentEndInitializer_ = value.initializer;
    currentEndCursor_ = value.commandIndex;
    return;
  case Edge::Failed: reject(); return;
  }
}


void RenderTapeRuntimeIdentityCore::queryIssue(D9CWireObjectIdentity identity,
    std::uint32_t flags, const core::QueryPollSnapshot& snapshot,
    std::int32_t result, std::uint64_t issueCycle) noexcept {
  std::lock_guard lock(mutex_);
  if (!active_ || failed_ || !recordActive_ ||
      records_[currentRecord_].type != D9C_COMMAND_RECORD_QUERY_ISSUE || result < 0 ||
      snapshot.failed || !snapshot.issueOrdinal || !snapshot.issueDeviceEpoch ||
      identity.kind != D9C_CHUNK_HANDLE_KIND_QUERY || !identity.objectId || !identity.generation ||
      !queryMutationAdmitted()) {
    reject(); return;
  }
  try {
    auto issue = std::find_if(queries_.begin(), queries_.end(), [&](const auto& value) {
      return renderTapeIdentityV3SameIdentity(value.identity, identity);
    });
    if (issue == queries_.end()) {
      if (queries_.size() >= limits_.maxCoordinators) { reject(); return; }
      if (strictQueryContinuity() && std::any_of(queries_.begin(), queries_.end(),
          [&](const auto& prior) { return prior.coreIdentity == snapshot.identity; })) {
        reject(); return;
      }
      queries_.push_back({.identity = identity,
          .coreIdentity = strictQueryContinuity() ? snapshot.identity : 0u,
          .queryType = strictQueryContinuity() ? wireQueryType(snapshot.type) : 0u});
      issue = queries_.end() - 1;
    }
    if (strictQueryContinuity()) {
      RenderTapeQueryCycleState next{.type = issue->queryType, .issueCycle = issue->cycle,
          .active = issue->active, .issueOrdinal = issue->ordinal};
      if (!snapshot.identity || snapshot.identity != issue->coreIdentity ||
          wireQueryType(snapshot.type) != issue->queryType ||
          (issue->ordinal && snapshot.issueDeviceEpoch != issue->epoch) ||
          !renderTapeAdvanceQueryCycle(next, flags, false) ||
          snapshot.issueOrdinal != next.issueOrdinal || snapshot.active != next.active ||
          (issueCycle && issueCycle != next.issueCycle)) { reject(); return; }
      issue->cycle = next.issueCycle;
      issue->receipt = snapshot.backendFence;
    } else {
      // Preserve the non-replay V4 and V3 semantics, including their existing
      // explicit captured-cycle path. Only private admission enables strictness.
      if (issueCycle) issue->cycle = issueCycle;
      else {
        if (flags == 2u || !issue->active) ++issue->cycle;
        if (snapshot.issueOrdinal != issue->ordinal + 1u) { reject(); return; }
      }
    }
    issue->ordinal = snapshot.issueOrdinal;
    issue->epoch = snapshot.issueDeviceEpoch;
    issue->active = snapshot.active;
    issue->coordinator = static_cast<std::uint32_t>(coordinators_.size());
    currentCoordinator_.queryIdentity = identity;
    currentCoordinator_.issueCycle = issue->cycle;
    currentCoordinator_.issueOrdinal = snapshot.issueOrdinal;
    currentCoordinator_.deviceEpoch = snapshot.issueDeviceEpoch;
    currentCoordinator_.flags = flags;
    currentCoordinator_.queryType = wireQueryType(snapshot.type);
    if (flags == 2u) {
      currentCoordinator_.prefixKind = static_cast<std::uint32_t>(Prefix::None);
      return;
    }
    const auto fence = snapshot.backendFence.snapshot();
    if (!fence.valid() || fence.queue != context_.queue ||
        fence.sequence != currentEndSequence_ || fence.initializer != currentEndInitializer_)
      { reject(); return; }
    auto& control = currentCoordinator_;
    control.commandEnd = currentEndCursor_;
    control.prefixInitializer = fence.initializer;
    if (fence.initializer) {
      control.prefixInitializerEpoch = context_.initializerEpoch;
      if (fence.initializer > context_.baselineCompletedInitializer) {
        const auto upload = std::find_if(initializers_.begin(), initializers_.end(),
            [&](const auto& value) { return value.sequence == fence.initializer; });
        if (upload == initializers_.end()) { reject(); return; }
        control.initializerIndex = static_cast<std::uint32_t>(upload - initializers_.begin());
      }
    }
    if (snapshot.backendFence.marker && !fence.sequence) {
      const auto owner = snapshot.backendFence.marker->owner();
      if (owner != currentEndWriter_ || snapshot.backendFence.marker->cancelled()) { reject(); return; }
      const auto* value = writer(owner);
      if (!value || !control.commandEnd) { reject(); return; }
      control.prefixKind = static_cast<std::uint32_t>(Prefix::Unbound);
      control.writerCaptureId = value->captureId;
      control.writerGeneration = owner.generation;
      control.storageGeneration = owner.storageGeneration;
      return;
    }
    const auto source = std::find_if(sources_.begin(), sources_.end(),
        [&](const auto& value) { return value.seqId == fence.sequence; });
    if (source == sources_.end()) {
      if (fence.sequence != context_.baselineSequence || control.commandEnd) {
        reject(); return;
      }
      control.prefixKind = context_.baselineKind;
      control.prefixSequence = fence.sequence;
    } else {
      control.prefixKind = static_cast<std::uint32_t>(Prefix::RealSource);
      control.sourceIndex = static_cast<std::uint32_t>(source - sources_.begin());
      control.prefixSequence = fence.sequence;
      control.writerCaptureId = source->writerCaptureId;
      control.writerGeneration = source->writerGeneration;
      control.storageGeneration = source->storageGeneration;
      if (!control.commandEnd) control.commandEnd = source->commandCount;
    }
  } catch (...) { reject(); }
}


bool RenderTapeRuntimeIdentityCore::finishRecord(std::int32_t result) {
  if (failed_ || !recordActive_ || result < 0) return reject();
  auto& record = records_[currentRecord_];
  if (record.disposition.kind == static_cast<std::uint32_t>(Kind::Coordinator)) {
    if (coordinators_.size() >= limits_.maxCoordinators)
      return reject();
    auto& control = currentCoordinator_;
    control.eventOrdinal = eventOrdinal_;
    control.rawOrdinal = rawOrdinal_;
    control.buildGeneration = buildGeneration_;
    control.recordIndex = record.disposition.firstRecord;
    control.recordType = record.type;
    control.dispatchResult = result;
    control.dispatchEdge = edge();
    if (!finishCoordinatorAdmission(control)) return reject();
    record.disposition.coordinatorIndex = static_cast<std::uint32_t>(coordinators_.size());
    coordinators_.push_back(control);
  }
  recordActive_ = false;
  ++nextRecord_;
  return !failed_;
}

bool RenderTapeRuntimeIdentityCore::finishCoordinatorAdmission(
    RenderTapeIdentityV3Coordinator& control) {
  if (!release_.valid()) return false;
  control.releaseOrdinal = release_.event.ordinal;
  control.releaseGeneration = release_.generation;
  control.releaseAction = static_cast<std::uint32_t>(release_.event.action);
  control.releaseWriter = static_cast<std::uint32_t>(releaseWriter_);
  return true;
}


bool RenderTapeRuntimeIdentityCore::endRecord(std::int32_t result) noexcept {
  std::lock_guard lock(mutex_);
  try { return finishRecord(result); }
  catch (...) { return reject(); }
}


void RenderTapeRuntimeIdentityCore::queryPoll(D9CWireObjectIdentity identity,
    const core::QueryPollSnapshot& snapshot, std::int32_t result) noexcept {
  std::lock_guard lock(mutex_);
  if (!active_ || failed_ || frozen_) return;
  if (strictQueryContinuity() && (identity.kind != D9C_CHUNK_HANDLE_KIND_QUERY ||
      !identity.objectId || !identity.generation)) { reject(); return; }
  const auto issue = std::find_if(queries_.begin(), queries_.end(), [&](const auto& value) {
    return renderTapeIdentityV3SameIdentity(value.identity, identity);
  });
  // A pre-arm issue has its authenticated arm descriptor/checkpoint, not a
  // captured coordinator. It must not be attached to a later issue by order.
  if (issue == queries_.end()) {
    if (strictQueryContinuity() && (!snapshot.preIssue() ||
        std::any_of(queries_.begin(), queries_.end(), [&](const auto& prior) {
          return prior.coreIdentity == snapshot.identity;
        }))) reject();
    return;
  }
  if (strictQueryContinuity()) {
    if (snapshot.identity != issue->coreIdentity || snapshot.issueOrdinal != issue->ordinal ||
        snapshot.issueDeviceEpoch != issue->epoch || snapshot.active != issue->active ||
        wireQueryType(snapshot.type) != issue->queryType || snapshot.failed ||
        !sameFence(snapshot.backendFence.prefix, issue->receipt.prefix) ||
        snapshot.backendFence.marker != issue->receipt.marker ||
        !sameFence(snapshot.backendFence.snapshot(), issue->receipt.snapshot()) ||
        (snapshot.backendFence.marker && snapshot.backendFence.marker->cancelled())) {
      reject(); return;
    }
    if (issue->completedBaseline && issue->coordinator == noIndex) {
      if (result != 0 || !snapshot.backendFence.valid()) reject();
      return;
    }
  }
  if (issue->coordinator >= coordinators_.size() || result < 0) { reject(); return; }
  auto& control = coordinators_[issue->coordinator];
  if (control.flags == 2u) return;
  const RenderTapeIdentityV3CurrentIssue current{
      {context_.captureToken, context_.captureEpoch}, identity,
      issue->cycle, snapshot.issueOrdinal, snapshot.issueDeviceEpoch,
      snapshot.backendFence.marker && snapshot.backendFence.marker->cancelled()};
  const auto observation = result == 0 ? RenderTapeIdentityV3Observation::Completed
                                      : RenderTapeIdentityV3Observation::Pending;
  if (!tapeProvenanceObservationAdmitted(context_, control, current,
          sources_, initializers_, observation, coordinatorAdmission(issue->coordinator))) {
    reject(); return;
  }
  control.observation = static_cast<std::uint32_t>(observation);
}


void RenderTapeRuntimeIdentityCore::fail(std::source_location location) noexcept {
  std::lock_guard lock(mutex_);
  reject(location);
}


RenderTapeRuntimeIdentityFailure RenderTapeRuntimeIdentityCore::firstFailure() const noexcept {
  std::lock_guard lock(mutex_);
  return firstFailure_;
}


RenderTapeRuntimeIdentityFailure RenderTapeRuntimeIdentityCore::diagnosticState() const noexcept {
  std::lock_guard lock(mutex_);
  return diagnosticStateLocked();
}


bool RenderTapeRuntimeIdentityCore::failed() const noexcept {
  std::lock_guard lock(mutex_);
  return failed_;
}


bool RenderTapeRuntimeIdentityCore::matches(RenderTapeIdentityV3CaptureKey key) const noexcept {
  std::lock_guard lock(mutex_);
  return active_ && key.token == context_.captureToken && key.epoch == context_.captureEpoch;
}


bool RenderTapeRuntimeIdentityCore::freeze(RenderTapeIdentityV3CaptureKey key,
    std::uint64_t presentOrdinal) noexcept {
  std::lock_guard lock(mutex_);
  if (!active_ || failed_ || !presentOrdinal || eventActive_ || recordActive_ ||
      key.token != context_.captureToken || key.epoch != context_.captureEpoch ||
      (frozen_ && context_.presentOrdinal != presentOrdinal)) return false;
  context_.presentOrdinal = presentOrdinal;
  frozen_ = true;
  return true;
}


bool RenderTapeRuntimeIdentityCore::frozen() const noexcept {
  std::lock_guard lock(mutex_);
  return frozen_;
}


} // namespace dxmt9::d3d9
