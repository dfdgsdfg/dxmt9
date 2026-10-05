#include "device_c_render_tape_execution_v3.hpp"

#include "device_c_common.hpp"
#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_runtime_objects.hpp"
#include "device_c_replay_projection.hpp"
#include "dxmt9/dxmt9_device.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace dxmt9::d3d9 {
namespace {

using ActionKind = CaptureSourceReplayPlanV3ActionKind;
using CutKind = RenderTapeIdentityV3CutKind;

bool coordinatorCut(std::uint32_t type) noexcept {
  return type == D9C_COMMAND_RECORD_QUERY_ISSUE ||
      type == D9C_COMMAND_RECORD_READBACK ||
      type == D9C_COMMAND_RECORD_UPDATE_TEXTURE ||
      type == D9C_COMMAND_RECORD_PRESENT;
}

bool releasesBeforeDispatch(std::uint32_t type) noexcept {
  return type == D9C_COMMAND_RECORD_QUERY_ISSUE ||
      type == D9C_COMMAND_RECORD_READBACK || type == D9C_COMMAND_RECORD_UPDATE_TEXTURE;
}

std::uint32_t identityCommandKind(core::MetalCommandKind kind) noexcept {
  switch (kind) {
  case core::MetalCommandKind::DrawRun: return 1u;
  case core::MetalCommandKind::Clear: return 2u;
  case core::MetalCommandKind::SurfaceCopy: return 3u;
  case core::MetalCommandKind::StretchRect: return 4u;
  case core::MetalCommandKind::Readback: return 5u;
  case core::MetalCommandKind::ColorFill: return 6u;
  case core::MetalCommandKind::DepthResolve: return 7u;
  case core::MetalCommandKind::GenerateMipmaps: return 8u;
  case core::MetalCommandKind::Present: return 9u;
  }
  return 0u;
}

// The snapshot prefix consists only of the bootstrap, definitions and their
// exact promised initial subresource seeds. The first ordinary mutation or
// control ends setup even if no CommandChunk has appeared yet.
bool snapshotPrefix(const render_tape_runtime::GeneralPreflightPlan& replay,
                    std::uint32_t& count) {
  struct Seed {
    D9CWireObjectIdentity identity{};
    std::uint64_t bytes = 0u;
    std::uint32_t count = 0u;
    std::uint64_t recordedBytes = 0u;
    std::vector<std::uint32_t> subresources{};
  };
  std::vector<Seed> seeds;
  count = 0u;
  for (const auto& event : replay.events) {
    if (event.eventIndex != count) return false;
    if (event.type == RenderTapeEventType::BootstrapState) {
      if (count != 0u) return false;
    } else if (event.type == RenderTapeEventType::ObjectDefine) {
      if (event.index >= replay.definitions.size()) return false;
      const auto& definition = replay.definitions[event.index].fixed;
      if (definition.expectedContentCount)
        seeds.push_back({definition.identity, definition.expectedContentBytes,
                         definition.expectedContentCount, 0u, {}});
    } else if (event.type == RenderTapeEventType::ResourceMutation) {
      if (event.index >= replay.mutations.size()) return false;
      const auto& mutation = replay.mutations[event.index];
      auto seed = std::find_if(seeds.begin(), seeds.end(), [&](const auto& value) {
        return renderTapeIdentityV3SameIdentity(value.identity, mutation.identity);
      });
      if (seed == seeds.end() || seed->subresources.size() == seed->count) break;
      if (mutation.byteOffset || mutation.byteSize > seed->bytes - seed->recordedBytes ||
          std::find(seed->subresources.begin(), seed->subresources.end(),
                    mutation.subresource) != seed->subresources.end()) return false;
      seed->subresources.push_back(mutation.subresource);
      seed->recordedBytes += mutation.byteSize;
    } else {
      break;
    }
    ++count;
  }
  return count < replay.events.size() && std::all_of(seeds.begin(), seeds.end(),
      [](const auto& seed) {
        return seed.recordedBytes == seed.bytes && seed.subresources.size() == seed.count;
      });
}

// Process-local queue/source/raw/generation labels differ by construction.
// This comparison binds topology and actual physical commands, preserving
// source ownership, shared DrawRuns, no-publication coordinators and END cuts.
bool sameExecutionTopology(const RenderTapeIdentityV3View& expected,
                           const RenderTapeIdentityV3View& actual) {
  if (expected.sources.size() != actual.sources.size() ||
      expected.initializers.size() != actual.initializers.size() ||
      expected.commands.size() != actual.commands.size() ||
      expected.coordinators.size() != actual.coordinators.size() ||
      expected.settlements.size() != actual.settlements.size()) return false;
  for (std::size_t i = 0u; i < expected.sources.size(); ++i) {
    const auto& a = expected.sources[i];
    const auto& b = actual.sources[i];
    if (a.commandCount != b.commandCount || a.cutEventOrdinal != b.cutEventOrdinal ||
        a.cutKind != b.cutKind || a.cutRecord != b.cutRecord) return false;
  }
  for (std::size_t i = 0u; i < expected.initializers.size(); ++i) {
    const auto& a = expected.initializers[i];
    const auto& b = actual.initializers[i];
    if (a.cutEventOrdinal != b.cutEventOrdinal || a.cutKind != b.cutKind ||
        a.cutRecord != b.cutRecord) return false;
  }
  std::vector<std::pair<std::uint64_t, std::uint64_t>> passes;
  for (std::size_t i = 0u; i < expected.commands.size(); ++i) {
    const auto& a = expected.commands[i];
    const auto& b = actual.commands[i];
    if (a.sourceIndex != b.sourceIndex || a.physicalCommandIndex != b.physicalCommandIndex ||
        a.kind != b.kind || a.drawCount != b.drawCount || a.membership != b.membership ||
        a.passKind != b.passKind || a.coordinatorIndex != b.coordinatorIndex) return false;
    if (a.membership == static_cast<std::uint32_t>(RenderTapeIdentityV3Membership::Pass)) {
      const auto prior = std::find_if(passes.begin(), passes.end(), [&](const auto& value) {
        return value.first == a.logicalPassId || value.second == b.logicalPassId;
      });
      if (prior == passes.end()) passes.push_back({a.logicalPassId, b.logicalPassId});
      else if (prior->first != a.logicalPassId || prior->second != b.logicalPassId) return false;
    }
  }
  std::size_t disposition = 0u;
  for (const auto& a : expected.dispositions) {
    std::uint32_t records = 0u;
    std::uint32_t subdraws = 0u;
    while (records < a.recordCount) {
      if (disposition == actual.dispositions.size()) return false;
      const auto& b = actual.dispositions[disposition++];
      if (a.eventOrdinal != b.eventOrdinal || a.firstRecord + records != b.firstRecord ||
          !b.recordCount || b.recordCount > a.recordCount - records ||
          a.kind != b.kind || a.commandIndex != b.commandIndex ||
          a.coordinatorIndex != b.coordinatorIndex ||
          (a.subdrawCount && a.firstSubdraw + subdraws != b.firstSubdraw) ||
          b.subdrawCount > a.subdrawCount - subdraws) return false;
      records += b.recordCount;
      subdraws += b.subdrawCount;
    }
    if (subdraws != a.subdrawCount) return false;
  }
  if (disposition != actual.dispositions.size()) return false;
  for (std::size_t i = 0u; i < expected.coordinators.size(); ++i) {
    const auto& a = expected.coordinators[i];
    const auto& b = actual.coordinators[i];
    if (a.eventOrdinal != b.eventOrdinal || a.recordIndex != b.recordIndex ||
        a.recordType != b.recordType || a.flags != b.flags || a.queryType != b.queryType ||
        !renderTapeIdentityV3SameIdentity(a.queryIdentity, b.queryIdentity) ||
        a.issueCycle != b.issueCycle || a.dispatchResult != b.dispatchResult ||
        a.releaseAction != b.releaseAction || a.releaseWriter != b.releaseWriter ||
        (a.prefixKind != b.prefixKind &&
         !((a.prefixKind == static_cast<std::uint32_t>(RenderTapeIdentityV3PrefixKind::ArmBaseline) ||
            a.prefixKind == static_cast<std::uint32_t>(RenderTapeIdentityV3PrefixKind::EmptyBaseline)) &&
           (b.prefixKind == static_cast<std::uint32_t>(RenderTapeIdentityV3PrefixKind::ArmBaseline) ||
            b.prefixKind == static_cast<std::uint32_t>(RenderTapeIdentityV3PrefixKind::EmptyBaseline)))) ||
        a.sourceIndex != b.sourceIndex ||
        a.initializerIndex != b.initializerIndex || a.commandEnd != b.commandEnd) return false;
  }
  for (std::size_t i = 0u; i < expected.settlements.size(); ++i) {
    if (expected.settlements[i].eventOrdinal != actual.settlements[i].eventOrdinal ||
        expected.settlements[i].recordCount != actual.settlements[i].recordCount) return false;
  }
  return true;
}

} // namespace

RenderTapeExecutionV3::~RenderTapeExecutionV3() {
  cancel();
}

void RenderTapeExecutionV3::cancel() noexcept {
  if (armed_ && backend_) {
    backend_->queue().revokeTapeRuntimeObserver({this, observe});
    armed_ = false;
    if (!evidence_ || !evidence_->complete) backend_->queue().failStopReplaySourceExecution();
  }
  consumer_ = {};
  device_ = nullptr;
  backend_.reset();
  writingContinuation_ = {};
}

bool RenderTapeExecutionV3::reject(const char* reason) noexcept {
  bool clear = false;
  if (failed_.compare_exchange_strong(clear, true, std::memory_order_acq_rel))
    failure_.store(reason, std::memory_order_release);
  return false;
}

bool RenderTapeExecutionV3::admit(const CaptureSourceReplayPlanV3& plan,
    const render_tape_runtime::GeneralPreflightPlan& replay,
    std::span<const std::byte> tape,
    RenderTapeSourceReplayV3Evidence& evidence) noexcept {
  if (plan_ || !plan.valid() || plan.events().size() != replay.events.size())
    return reject("v3-plan-input");
  try {
    const RenderTapeIdentityV3Limits limits{};
    const auto& identity = plan.identity();
    std::uint64_t originalRecords = 0u;
    for (const auto& event : plan.events()) originalRecords += event.recordCount;
    if (identity.sources.size() > limits.maxSources ||
        identity.initializers.size() > limits.maxInitializers ||
        identity.commands.size() > limits.maxCommands ||
        originalRecords > limits.maxDispositions ||
        identity.coordinators.size() > limits.maxCoordinators ||
        identity.settlements.size() > limits.maxSettlements)
      return reject("v3-runtime-identity-capacity");
    ImportedRenderTapeView authenticated;
    if (tape.size() != plan.identity().header.eventsBytes ||
        RenderTapeCaptureSession::sha256(tape) != plan.identity().header.eventsDigest ||
        !importPrevalidatedRenderTape(tape, authenticated) ||
        authenticated.events.size() != replay.tape.events.size() ||
        authenticated.payloadArena.size() != replay.tape.payloadArena.size() ||
        std::memcmp(&authenticated.header, &replay.tape.header, sizeof(RenderTapeHeader)) ||
        !std::equal(authenticated.payloadArena.begin(), authenticated.payloadArena.end(),
                    replay.tape.payloadArena.begin()) ||
        !std::equal(std::as_bytes(authenticated.events).begin(),
                    std::as_bytes(authenticated.events).end(),
                    std::as_bytes(replay.tape.events).begin()))
      return reject("v3-exact-tape-binding");
    if (!snapshotPrefix(replay, snapshotPrefixEvents_))
      return reject("v3-snapshot-prefix");
    for (std::size_t i = 0u; i < plan.events().size(); ++i) {
      const auto& event = plan.events()[i];
      const auto& original = replay.events[i];
      if (original.eventIndex != i || event.type != static_cast<std::uint32_t>(original.type) ||
          event.eventOrdinal != replay.tape.event(i).header.ordinal ||
          (original.type == RenderTapeEventType::CommandChunk &&
           (event.commandChunkIndex != original.index ||
            original.index >= replay.commandChunks.size() ||
            event.recordCount != replay.commandChunks[original.index].envelope.recordCount)))
        return reject("v3-event-binding");
    }
    recordTypes_.resize(plan.events().size());
    commandDigests_.resize(plan.events().size());
    commandEnvelopes_.resize(plan.events().size());
    for (const auto& event : replay.events) {
      if (event.type != RenderTapeEventType::CommandChunk) continue;
      ImportedChunkView imported;
      const auto& chunk = replay.commandChunks[event.index];
      const auto payload = authenticated.event(event.eventIndex).payload;
      if (payload.size() < sizeof(RenderTapeCommandChunkHeader) ||
          payload.size() - sizeof(RenderTapeCommandChunkHeader) != chunk.bytes.size() ||
          !std::equal(payload.begin() + sizeof(RenderTapeCommandChunkHeader), payload.end(),
                      chunk.bytes.begin())) return reject("v3-exact-command-binding");
      if (!importPrevalidatedCommandChunk(chunk.bytes, chunk.envelope, imported))
        return reject("v3-record-type-binding");
      commandDigests_[event.eventIndex] = RenderTapeCaptureSession::sha256(chunk.bytes);
      commandEnvelopes_[event.eventIndex] = chunk.envelope;
      auto& types = recordTypes_[event.eventIndex];
      types.reserve(imported.records.size());
      for (const auto& record : imported.records) types.push_back(record.type);
    }
    for (const auto& action : plan.actions()) {
      if ((action.kind == ActionKind::ExpectSourcePublication ||
           action.kind == ActionKind::ExpectInitializerPublication) &&
          action.eventIndex < snapshotPrefixEvents_)
        return reject("v3-publication-inside-snapshot-prefix");
    }
    if (!render_tape_runtime::generalReplayPlanMatchesAuthenticatedV3(tape, plan, replay))
      return reject("v3-general-plan-binding");
    evidence = {};
    evidence.requested = true;
    evidence.validated = true;
    evidence.planDigest = plan.digest();
    evidence.publications.reserve(plan.identity().sources.size());
    evidence.initializers.reserve(plan.identity().initializers.size());
    evidence.settlements.reserve(plan.identity().settlements.size());
    sourceEdges_.reserve(plan.identity().sources.size());
    initializerEdges_.reserve(plan.identity().initializers.size());
    if (!cursor_.begin(plan)) return reject("v3-cursor-begin");
    plan_ = &plan;
    evidence_ = &evidence;
    localKey_ = {plan.identity().header.captureToken, plan.identity().header.captureEpoch};
    return true;
  } catch (...) {
    return reject("v3-execution-allocation");
  }
}

bool RenderTapeExecutionV3::arm(D9CDevice* device) noexcept {
  return arm(device, SourceConsumer{});
}

bool RenderTapeExecutionV3::arm(D9CDevice* device, SourceConsumer consumer) noexcept {
  if (failed_ || !plan_ || armed_ || !device) return reject("v3-arm-input");
  auto upper = device->dev().upperDevice();
  if (!upper || !upper->queue().installTapeRuntimeObserver({this, observe}, baseline_))
    return reject("v3-local-baseline-not-quiescent");
  device_ = device;
  backend_ = upper;
  consumer_ = consumer;
  armed_ = true;
  if (!localIdentity_.begin(localKey_, baseline_))
    return reject("v3-local-identity-begin");
  evidence_->executed = true;
  return true;
}

bool RenderTapeExecutionV3::acknowledge(ActionKind kind, std::uint32_t record) noexcept {
  CaptureSourceReplayPlanV3Action action;
  if (failed_ || !plan_ || !cursor_.peek(*plan_, action) || action.kind != kind ||
      action.eventIndex != event_ ||
      (kind == ActionKind::DispatchRecord && action.recordIndex != record) ||
      !cursor_.acknowledge(*plan_, action)) return reject("v3-action-order");
  return true;
}

bool RenderTapeExecutionV3::beginEvent(std::uint32_t eventIndex) noexcept {
  if (failed_ || !plan_ || eventIndex >= plan_->events().size() ||
      (event_ != kRenderTapeIdentityV3NoIndex && eventIndex != event_ + 1u) ||
      (event_ == kRenderTapeIdentityV3NoIndex && eventIndex != 0u) ||
      (eventIndex >= snapshotPrefixEvents_ && !armed_)) return reject("v3-event-order");
  event_ = eventIndex;
  nextRecord_ = 0u;
  if (armed_ && !localIdentity_.setCut(plan_->events()[event_].eventOrdinal,
      CutKind::BeforeEvent, 0u)) return reject("v3-local-before-event-cut");
  if (!recordTypes_[event_].empty() && releasesBeforeDispatch(recordTypes_[event_].front()))
    return true;
  return cut(static_cast<std::uint32_t>(CutKind::BeforeEvent), 0u, true);
}

bool RenderTapeExecutionV3::replayedEvent(std::uint32_t eventIndex) noexcept {
  if (eventIndex != event_ || !acknowledge(ActionKind::ReplayEvent)) return false;
  ++evidence_->replayedEvents;
  if (armed_ && !localIdentity_.setCut(plan_->events()[event_].eventOrdinal,
      CutKind::AfterEvent, 0u)) return reject("v3-local-after-event-cut");
  return cut(static_cast<std::uint32_t>(CutKind::AfterEvent), 0u, true);
}

bool RenderTapeExecutionV3::bindCommandChunk(std::span<const std::byte> bytes,
    const CommandChunkEnvelope& envelope, std::span<void* const> objects) noexcept {
  if (failed_ || !armed_ || !plan_ || event_ >= plan_->events().size() ||
      plan_->events()[event_].type != static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk) ||
      !importPrevalidatedCommandChunk(bytes, envelope, chunk_) ||
      chunk_.records.size() != plan_->events()[event_].recordCount ||
      objects.size() != chunk_.handles.size()) return reject("v3-command-binding");
  const auto& admitted = commandEnvelopes_[event_];
  if (RenderTapeCaptureSession::sha256(bytes) != commandDigests_[event_] ||
      envelope.version != admitted.version || envelope.handleCount != admitted.handleCount ||
      envelope.recordCount != admitted.recordCount ||
      std::memcmp(&envelope.producerIdentity, &admitted.producerIdentity,
                  sizeof(envelope.producerIdentity))) return reject("v3-command-immutable-binding");
  resolved_ = objects;
  return true;
}

bool RenderTapeExecutionV3::ownsWritingContinuation(
    const WritingContinuation& queried) const noexcept {
  std::lock_guard lock(observationsMutex_);
  return !failed_ && armed_ && plan_ && evidence_ && evidence_->validated &&
      event_ < plan_->events().size() && !localRawOrdinal_ && !nextRecord_ &&
      plan_->events()[event_].type == static_cast<std::uint32_t>(
          RenderTapeEventType::CommandChunk) &&
      queried.writer.valid() && queried.source.valid() && queried.commandCount &&
      queried.writer.queue == baseline_.queue &&
      queried.writer == writingContinuation_.writer &&
      queried.source == writingContinuation_.source &&
      queried.commandCount == writingContinuation_.commandCount;
}

bool RenderTapeExecutionV3::startEvent(std::uint64_t raw, std::uint32_t count) noexcept {
  if (failed_ || !plan_ || event_ >= plan_->events().size() ||
      count != chunk_.records.size() || !raw || localRawOrdinal_)
    return reject("v3-local-event-start");
  localRawOrdinal_ = raw;
  return true;
}

bool RenderTapeExecutionV3::startTransaction(const ReplayTransactionState& state) noexcept {
  if (failed_ || !plan_ || state.identity.sequence != localRawOrdinal_ ||
      state.phase != ReplayTransactionPhase::Created || state.nextRecordOrdinal ||
      !localIdentity_.beginReplayAttempt(localKey_.token,
          plan_->events()[event_].eventOrdinal, localRawOrdinal_,
          static_cast<std::uint32_t>(chunk_.records.size())))
    return reject("v3-local-transaction-start");
  return true;
}

bool RenderTapeExecutionV3::commitTransaction(const ReplayTransactionState& state) noexcept {
  if (failed_ || state.identity.sequence != localRawOrdinal_ ||
      !localIdentity_.transactionCommitted(state)) return reject("v3-local-transaction-commit");
  return true;
}

bool RenderTapeExecutionV3::beforeRecord(std::uint32_t index, std::uint32_t type) noexcept {
  if (failed_ || index != nextRecord_ || index >= chunk_.records.size() ||
      chunk_.records[index].type != type || !localIdentity_.beginRecord(index, type))
    return reject("v3-record-preflight");
  return true;
}

bool RenderTapeExecutionV3::beforeDispatch(std::uint32_t index, std::uint32_t type) noexcept {
  if (releasesBeforeDispatch(type) &&
      !cut(static_cast<std::uint32_t>(index ? CutKind::AfterRecord : CutKind::BeforeEvent),
           index, false)) return false;
  CaptureSourceReplayPlanV3Action action;
  if (failed_ || !cursor_.peek(*plan_, action) || action.kind != ActionKind::DispatchRecord ||
      action.recordIndex != index || action.eventIndex != event_ ||
      !localIdentity_.beginDispatch()) return reject("v3-record-dispatch-preflight");
  return true;
}

bool RenderTapeExecutionV3::afterRecord(std::uint32_t index, std::uint32_t type,
                                      std::int32_t result) noexcept {
  if (failed_ || result < 0 || index != nextRecord_) return reject("v3-record-dispatch");
  if (type == D9C_COMMAND_RECORD_QUERY_ISSUE) {
    D9CCommandChunkWireQueryIssue issue{};
    const auto record = chunk_.record(index);
    if (record.payload.size() < sizeof(issue)) return reject("v3-query-issue-payload");
    std::memcpy(&issue, record.payload.data(), sizeof(issue));
    if (issue.queryHandleIndex >= resolved_.size()) return reject("v3-query-issue-handle");
    auto* query = static_cast<D9CQuery*>(resolved_[issue.queryHandleIndex]);
    if (!query || !query->obj) return reject("v3-query-issue-owner");
    const auto& handle = chunk_.handles[issue.queryHandleIndex];
    CaptureSourceReplayPlanV3Action action;
    if (!cursor_.peek(*plan_, action) || action.dispositionIndex >= plan_->identity().dispositions.size())
      return reject("v3-query-coordinator-binding");
    const auto coordinator = plan_->identity().dispositions[action.dispositionIndex].coordinatorIndex;
    if (coordinator >= plan_->identity().coordinators.size())
      return reject("v3-query-coordinator-binding");
    localIdentity_.queryIssue({handle.kind, handle.generation, handle.objectId},
        issue.flags, device_->dev().captureQueryPollSnapshot(query->obj), result,
        plan_->identity().coordinators[coordinator].issueCycle);
  }
  if (!localIdentity_.endRecord(result) || !acknowledge(ActionKind::DispatchRecord, index))
    return reject("v3-record-settlement");
  ++nextRecord_;
  ++evidence_->dispatchedRecords;
  if (nextRecord_ < chunk_.records.size() &&
      releasesBeforeDispatch(chunk_.records[nextRecord_].type)) return true;
  return cut(static_cast<std::uint32_t>(CutKind::AfterRecord), nextRecord_,
             !coordinatorCut(type));
}

bool RenderTapeExecutionV3::settleEvent(std::uint64_t raw, std::uint32_t count) noexcept {
  if (failed_ || nextRecord_ != count || count != chunk_.records.size() || !raw ||
      raw != localRawOrdinal_ || !localIdentity_.settleReplayAttempt(raw, true))
    return reject("v3-command-event-settlement");
  if (!localIdentity_.setCut(plan_->events()[event_].eventOrdinal,
      CutKind::AfterEvent, 0u)) return reject("v3-local-event-settlement-cut");
  if (!cut(static_cast<std::uint32_t>(CutKind::AfterEvent), 0u, true) ||
      !acknowledge(ActionKind::SettleCommandEvent)) return false;
  evidence_->settlements.push_back({plan_->events()[event_].eventOrdinal, raw, count});
  localRawOrdinal_ = 0u;
  resolved_ = {};
  chunk_ = {};
  return true;
}

ProviderReplayRecordConsumer RenderTapeExecutionV3::recordConsumer() noexcept {
  return {.context = this,
      .eventStarted = [](void* p, std::uint64_t raw, std::uint32_t count) noexcept {
        return static_cast<RenderTapeExecutionV3*>(p)->startEvent(raw, count);
      },
      .transactionStarted = [](void* p, const ReplayTransactionState& state) noexcept {
        return static_cast<RenderTapeExecutionV3*>(p)->startTransaction(state);
      },
      .transactionCommitted = [](void* p, const ReplayTransactionState& state) noexcept {
        return static_cast<RenderTapeExecutionV3*>(p)->commitTransaction(state);
      },
      .beforeRecord = [](void* p, std::uint32_t index, std::uint32_t type) noexcept {
        return static_cast<RenderTapeExecutionV3*>(p)->beforeRecord(index, type);
      },
      .beforeDispatch = [](void* p, std::uint32_t index, std::uint32_t type) noexcept {
        return static_cast<RenderTapeExecutionV3*>(p)->beforeDispatch(index, type);
      },
      .recordDispatched = [](void* p, std::uint32_t index, std::uint32_t type,
                            std::int32_t result) noexcept {
        return static_cast<RenderTapeExecutionV3*>(p)->afterRecord(index, type, result);
      },
      .eventSettled = [](void* p, std::uint64_t raw, std::uint32_t count) noexcept {
        return static_cast<RenderTapeExecutionV3*>(p)->settleEvent(raw, count);
      }};
}

void RenderTapeExecutionV3::observe(void* opaque,
    const render::TapeRuntimeObservation& observation) noexcept {
  static_cast<RenderTapeExecutionV3*>(opaque)->record(observation);
}

void RenderTapeExecutionV3::record(const render::TapeRuntimeObservation& observation) noexcept {
  std::lock_guard lock(observationsMutex_);
  if (failed_ || !evidence_) return;
  const auto sink = localIdentity_.sink();
  sink.fn(sink.context, observation);
  if (localIdentity_.failed()) { (void)reject("v3-runtime-identity"); return; }
  auto& sources = evidence_->publications;
  auto& initializers = evidence_->initializers;
  if (observation.edge == render::TapeRuntimeEdge::Append) {
    writingContinuation_ = {observation.writer, observation.source,
                           observation.payload.commandCount()};
  } else if (observation.edge == render::TapeRuntimeEdge::Publish) {
    if (observation.writer == writingContinuation_.writer &&
        observation.source == writingContinuation_.source)
      writingContinuation_ = {};
    if (sources.size() == sources.capacity() || sources.size() >= plan_->identity().sources.size()) {
      evidence_->storageOverflow = true;
      (void)reject("v3-source-publication-capacity");
      return;
    }
    const auto index = static_cast<std::uint32_t>(sources.size());
    const auto& expected = plan_->identity().sources[index];
    if (!observation.source.valid() || !observation.metadata.valid() ||
        observation.payload.commandCount() != expected.commandCount) {
      (void)reject("v3-source-publication-shape");
      return;
    }
    std::size_t commandIndex = 0u;
    for (const auto& command : plan_->identity().commands) {
      if (command.sourceIndex != index) continue;
      if (commandIndex >= observation.payload.commandCount()) {
        (void)reject("v3-source-command-count"); return;
      }
      const auto actual = observation.payload.commandAt(commandIndex++);
      if (command.kind != identityCommandKind(actual.kind()) ||
          command.drawCount != (actual.kind() == core::MetalCommandKind::DrawRun
              ? actual.command.drawParams.size() : 0u)) {
        (void)reject("v3-source-command-shape"); return;
      }
    }
    if (commandIndex != observation.payload.commandCount()) {
      (void)reject("v3-source-command-count"); return;
    }
    sources.push_back({.eventOrdinal = plan_->events()[event_].eventOrdinal,
        .localSequence = observation.metadata.seqId, .source = observation.source,
        .metadata = observation.metadata,
        .controlIndex = observation.controlIndex,
        .commandCount = static_cast<std::uint32_t>(observation.payload.commandCount()),
        .hasPresent = observation.payload.presentRecordCount() != 0u});
    sourceEdges_.push_back(++observedPublicationEdge_);
  } else if (observation.edge == render::TapeRuntimeEdge::Submit ||
             observation.edge == render::TapeRuntimeEdge::Complete) {
    auto source = std::find_if(sources.begin(), sources.end(), [&](const auto& value) {
      return value.source == observation.source && value.localSequence == observation.sequence;
    });
    if (source == sources.end()) { (void)reject("v3-source-lifecycle-identity"); return; }
    if (observation.edge == render::TapeRuntimeEdge::Submit) {
      if (source->submitted) { (void)reject("v3-source-double-submit"); return; }
      source->submitted = true;
    } else {
      if (!source->submitted || source->completed ||
          observation.completedSequence < source->localSequence) {
        (void)reject("v3-source-completion-order"); return;
      }
      source->completed = true;
    }
  } else if (observation.edge == render::TapeRuntimeEdge::InitializerPublish) {
    if (initializers.size() == initializers.capacity() ||
        !observation.sequence || observation.sequence <= baseline_.initializer) {
      evidence_->storageOverflow = true;
      (void)reject("v3-initializer-publication-capacity"); return;
    }
    initializers.push_back({.localSequence = observation.sequence});
    initializerEdges_.push_back(++observedPublicationEdge_);
  } else if (observation.edge == render::TapeRuntimeEdge::InitializerSubmit ||
             observation.edge == render::TapeRuntimeEdge::InitializerComplete) {
    auto initializer = std::find_if(initializers.begin(), initializers.end(), [&](const auto& value) {
      return value.localSequence == observation.sequence;
    });
    if (initializer == initializers.end()) { (void)reject("v3-initializer-lifecycle-identity"); return; }
    if (observation.edge == render::TapeRuntimeEdge::InitializerSubmit) {
      if (initializer->submitted) { (void)reject("v3-initializer-double-submit"); return; }
      initializer->submitted = true;
    } else {
      if (!initializer->submitted || initializer->completed ||
          observation.completedSequence < initializer->localSequence) {
        (void)reject("v3-initializer-completion-order"); return;
      }
      initializer->completed = true;
    }
  } else if (observation.edge == render::TapeRuntimeEdge::Failed) {
    (void)reject("v3-backend-failed");
  }
}

bool RenderTapeExecutionV3::cut(std::uint32_t kind, std::uint32_t recordIndex,
                              bool externalAllowed) noexcept {
  if (failed_ || !plan_) return false;
  CaptureSourceReplayPlanV3Action action;
  if (!cursor_.peek(*plan_, action))
    return cursor_.status() == CaptureSourceReplayCursorV3Status::Complete;
  const auto sameCut = [&](const auto& value) {
    return value.eventIndex == event_ && value.cutKind == kind && value.cutRecord == recordIndex &&
        (value.kind == ActionKind::ExpectSourcePublication ||
         value.kind == ActionKind::ExpectInitializerPublication);
  };
  if (!sameCut(action)) return true;
  if (!armed_) return reject("v3-cut-before-local-arm");
  while (sameCut(action)) {
    // This is the separately admitted external cut instruction. Coordinator
    // dispatch may already have published, so check each actual receipt first.
    // An initializer can precede a source at the same cut; never manufacture
    // initializer work, and never hold the observation lock while publishing.
    bool sourceMissing = false;
    {
      std::lock_guard lock(observationsMutex_);
      sourceMissing = action.kind == ActionKind::ExpectSourcePublication &&
          expectedSources_ == evidence_->publications.size();
    }
    if (sourceMissing) {
      if (!externalAllowed) return reject("v3-missing-coordinator-publication");
      if (device_->dev().flushCompatibilityReplayDrawBatch() ==
          core::CompatibilityDrawBatchFlushStatus::Failed)
        return reject("v3-cut-draw-batch-flush");
      auto upper = device_->dev().upperDevice();
      if (!upper || !upper->queue().publishReplaySourceBoundary().published())
        return reject("v3-external-cut-publication");
      ++evidence_->externalPublicationCuts;
    }
    std::optional<core::metalqueue::ReplaySourcePublicationReceipt> receipt;
    {
    std::lock_guard lock(observationsMutex_);
    if (action.kind == ActionKind::ExpectSourcePublication) {
      if (expectedSources_ >= evidence_->publications.size() || action.tableIndex != expectedSources_)
        return reject("v3-source-expectation");
      auto& source = evidence_->publications[expectedSources_];
      const auto nextInitializer = expectedInitializers_ < initializerEdges_.size()
          ? initializerEdges_[expectedInitializers_] : UINT64_MAX;
      if (source.expected || sourceEdges_[expectedSources_] > nextInitializer)
        return reject("v3-source-publication-edge-order");
      source.capturedIndex = expectedSources_++;
      source.expected = true;
      if (consumer_.published) {
        // SourceRef and metadata are copied from the actual publication;
        // captured queue sequences never become local completion authority.
        receipt = core::metalqueue::ReplaySourcePublicationReceipt{
            .source = source.source, .metadata = source.metadata,
            .controlIndex = source.controlIndex, .hasPresent = source.hasPresent};
      }
    } else {
      if (expectedInitializers_ >= evidence_->initializers.size() ||
          action.tableIndex != expectedInitializers_) return reject("v3-initializer-expectation");
      auto& initializer = evidence_->initializers[expectedInitializers_];
      const auto nextSource = expectedSources_ < sourceEdges_.size()
          ? sourceEdges_[expectedSources_] : UINT64_MAX;
      if (initializer.expected || initializerEdges_[expectedInitializers_] > nextSource)
        return reject("v3-initializer-publication-edge-order");
      initializer.capturedIndex = expectedInitializers_++;
      initializer.expected = true;
    }
    }
    if (receipt && !consumer_.published(consumer_.context, *receipt))
      return reject("v3-source-consumer");
    if (!cursor_.acknowledge(*plan_, action)) return reject("v3-publication-acknowledge");
    if (!cursor_.peek(*plan_, action))
      return cursor_.status() == CaptureSourceReplayCursorV3Status::Complete;
  }
  return true;
}

bool RenderTapeExecutionV3::lifecycleComplete() const noexcept {
  return std::all_of(evidence_->publications.begin(), evidence_->publications.end(),
             [](const auto& row) { return row.expected && row.submitted && row.completed; }) &&
      std::all_of(evidence_->initializers.begin(), evidence_->initializers.end(),
             [](const auto& row) { return row.expected && row.submitted && row.completed; });
}

bool RenderTapeExecutionV3::finish() noexcept {
  if (failed_ || !armed_ || !backend_ ||
      !backend_->queue().refreshTapeRuntimeCompletions({this, observe}))
    return reject("v3-observer-final-refresh");
  std::unique_lock lock(observationsMutex_);
  if (failed_ || !armed_ || cursor_.status() != CaptureSourceReplayCursorV3Status::Complete ||
      expectedSources_ != plan_->identity().sources.size() ||
      expectedInitializers_ != plan_->identity().initializers.size() ||
      !lifecycleComplete()) return reject("v3-execution-incomplete");
  try {
    RenderTapeIdentityV3View actual;
    if (!localIdentity_.freeze(localKey_, plan_->identity().header.presentOrdinal) ||
        !localIdentity_.snapshot(localKey_, actual, true) ||
        !sameExecutionTopology(plan_->identity(), actual)) return reject("v3-execution-topology");
  } catch (...) {
    return reject("v3-execution-topology-allocation");
  }
  lock.unlock();
  if (!backend_->queue().removeTapeRuntimeObserver({this, observe}))
    return reject("v3-observer-remove");
  armed_ = false;
  consumer_ = {};
  device_ = nullptr;
  backend_.reset();
  writingContinuation_ = {};
  evidence_->lifecycleComplete = true;
  evidence_->complete = true;
  return true;
}

} // namespace dxmt9::d3d9
