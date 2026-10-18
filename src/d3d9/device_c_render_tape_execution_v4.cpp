#include "device_c_render_tape_execution_v4.hpp"

#include "device_c_common.hpp"
#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_identity_transfer_v4.hpp"
#include "device_c_render_tape_initial_setup_v4.hpp"
#include "device_c_render_tape_present_preparation.hpp"
#include "device_c_render_tape_runtime_binding_v4.hpp"
#include "device_c_render_tape_runtime_objects.hpp"
#include "dxmt9/dxmt9_device.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace dxmt9::d3d9 {
namespace {

using ActionKind = CaptureSourceReplayPlanV4ActionKind;
using Cut = RenderTapeIdentityV3CutKind;
constexpr auto noIndex = kRenderTapeIdentityV4NoIndex;

std::uint32_t queryWireType(core::QueryType type) noexcept {
  switch (type) {
  case core::QueryType::Event: return 8u;
  case core::QueryType::Occlusion: return 9u;
  case core::QueryType::Timestamp: return 10u;
  case core::QueryType::TimestampDisjoint: return 11u;
  case core::QueryType::TimestampFreq: return 12u;
  }
  return 0u;
}

// A capture key identifies one local arm, not a caller/captured label. Never
// wrap or reuse a key, even after failed arm/cancellation. The synchronous
// provider owns public arm/reset exclusively; this is not a same-key race fix.
std::atomic<std::uint64_t> nextLocalKey{1u};

bool allocateLocalKey(RenderTapeIdentityV4CaptureKey& out) noexcept {
  auto value = nextLocalKey.load(std::memory_order_relaxed);
  while (value && value != UINT64_MAX) {
    if (nextLocalKey.compare_exchange_weak(value, value + 1u,
            std::memory_order_relaxed, std::memory_order_relaxed)) {
      out = {value, value};
      return true;
    }
  }
  return false;
}

bool sameEnvelope(const CommandChunkEnvelope& a, const CommandChunkEnvelope& b) noexcept {
  return a.version == b.version && a.recordCount == b.recordCount &&
      a.handleCount == b.handleCount &&
      !std::memcmp(&a.producerIdentity, &b.producerIdentity, sizeof(a.producerIdentity));
}

bool sameCommand(const RenderTapeIdentityV4PhysicalCommand& a,
    const RenderTapeIdentityV4PhysicalCommand& b) noexcept {
  return a.sourceIndex == b.sourceIndex && a.physicalCommandIndex == b.physicalCommandIndex &&
      a.kind == b.kind && a.drawCount == b.drawCount && a.coordinatorIndex == b.coordinatorIndex;
}

bool sameCut(const RenderTapeIdentityV3Source& a, const RenderTapeIdentityV3Source& b) noexcept {
  return a.commandCount == b.commandCount && a.cutEventOrdinal == b.cutEventOrdinal &&
      a.cutKind == b.cutKind && a.cutRecord == b.cutRecord;
}

bool sameCut(const RenderTapeIdentityV3Initializer& a,
    const RenderTapeIdentityV3Initializer& b) noexcept {
  return a.cutEventOrdinal == b.cutEventOrdinal && a.cutKind == b.cutKind && a.cutRecord == b.cutRecord;
}

bool baselinePrefix(std::uint32_t kind) noexcept {
  return kind == static_cast<std::uint32_t>(RenderTapeIdentityV3PrefixKind::ArmBaseline) ||
      kind == static_cast<std::uint32_t>(RenderTapeIdentityV3PrefixKind::EmptyBaseline);
}

bool sameFinishReceipt(const D9CRenderTapeIdentityCaptureV4Result& a,
    const D9CRenderTapeIdentityCaptureV4Result& b) noexcept {
  return a.status == b.status && a.version == b.version && a.captureToken == b.captureToken &&
      a.captureEpoch == b.captureEpoch && a.queue == b.queue &&
      a.presentOrdinal == b.presentOrdinal && a.byteCount == b.byteCount;
}

} // namespace

RenderTapeExecutionV4::~RenderTapeExecutionV4() { cancel(); }

bool RenderTapeExecutionV4::reject(const char* reason) noexcept {
  bool clear = false;
  if (failed_.compare_exchange_strong(clear, true, std::memory_order_acq_rel))
    failure_.store(reason, std::memory_order_release);
  return false;
}

bool RenderTapeExecutionV4::inputBound() noexcept {
  return !failed_ && preparation_ && preparation_->valid_ && plan_ && evidence_ &&
      preparation_->plan_ == plan_ && preparation_->replay_ == admittedReplay_ &&
      preparation_->digest_ == digest_ && plan_->valid() && plan_->digest() == digest_
      ? true : reject("v4-owner-binding");
}

bool RenderTapeExecutionV4::rootBound(bool allowFrozen) noexcept {
  if (!inputBound()) return false;
  if (!controlledCpu_) return rootBoundActual(allowFrozen);
  struct Context { RenderTapeExecutionV4& owner; bool allowFrozen; } context{*this, allowFrozen};
  const bool bound = controlledCpuLease_.visitBackend(RenderTapeControlledCpuV4Boundary::Replay,
      &context, [](void* opaque, D9CDevice* device, dxmt9::Device& backend) noexcept {
        auto& input = *static_cast<Context*>(opaque);
        return input.owner.device_ == device && input.owner.backend_.get() == &backend &&
            input.owner.rootBoundActual(input.allowFrozen);
      });
  return bound ? true : reject("v4-controlled-cpu-authority");
}

bool RenderTapeExecutionV4::rootBoundActual(bool allowFrozen) noexcept {
  if (!armed_ || !device_ || !backend_ || !root_ || !localQueue_ ||
      acquireRenderTapeRuntimeIdentityV4(device_) != root_ || !root_->matches(localKey_))
    return reject("v4-local-root-binding");
  if (!allowFrozen) {
    bool same = false, healthy = false;
    const auto lease = backend_->queue().acquireTapeRuntimeCapture();
    if (!lease.visit([&](render::TapeRuntimeCaptureAttachment& admitted) noexcept {
          same = &admitted == root_.get();
          // failed() can propagate topology rejection into the root. That
          // diagnostic mutation belongs inside this exact endpoint admission.
          if (same)
            healthy = !root_->frozen() && root_->edges().active() && !root_->failed();
        }) || !same) return reject("v4-local-endpoint-revoked");
    if (!healthy) return reject("v4-local-root-binding");
  } else {
    // Successful finish removes/joins the endpoint before freezing/caching
    // transport. Revoked root clocks never reactivate, so failed() cannot
    // mutate this frozen, permanently inactive root through its active branch.
    if (!root_->frozen() || root_->edges().active() || root_->failed())
      return reject("v4-local-frozen-root-binding");
  }
  std::lock_guard lock(device_->renderTapeRuntimeIdentityV4Mutex);
  const auto& arm = evidence_->localArm;
  if (acquireRenderTapeRuntimeIdentityV4(device_) != root_ ||
      !device_->renderTapeRuntimeBindingV4 ||
      device_->renderTapeRuntimeBindingV4->queue != localQueue_ ||
      arm.status != D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE || arm.version != 4u ||
      arm.captureToken != localKey_.token || arm.captureEpoch != localKey_.epoch ||
      arm.queue != localQueue_) return reject("v4-local-arm-receipt");
  const auto& binding = *device_->renderTapeRuntimeBindingV4;
  if (controlledCpu_ ?
      (binding.domain != RenderTapeRuntimeBindingV4Domain::ControlledCpu ||
       binding.controlledCpuAuthority.lock() != controlledCpuLease_.authority_.lock()) :
      binding.domain != RenderTapeRuntimeBindingV4Domain::Production)
    return reject("v4-local-authority-domain");
  if (allowFrozen) {
    const auto& receipt = device_->renderTapeRuntimeBindingV4->receipt;
    if (!receipt || receipt->status != D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE ||
        receipt->version != 4u || receipt->captureToken != localKey_.token ||
        receipt->captureEpoch != localKey_.epoch || receipt->queue != localQueue_ ||
        receipt->presentOrdinal != plan_->identity().header.presentOrdinal)
      return reject("v4-local-frozen-transport-receipt");
  }
  return true;
}

void RenderTapeExecutionV4::cancel() noexcept {
  if (plan_) preparedAdmissionCancelled_ = true;
  // Never use replacement credentials, or cancel a replacement root. Queue
  // revocation also matches the exact old sink, retaining old-root lifetime
  // across joining any admitted visitor. No renderer fail-stop is requested.
  if (root_) {
    if (controlledCpu_) {
      const bool detached = controlledCpuLease_.visitBackend(RenderTapeControlledCpuV4Boundary::Replay,
          this, [](void* opaque, D9CDevice* device, dxmt9::Device&) noexcept {
            auto& owner = *static_cast<RenderTapeExecutionV4*>(opaque);
            if (owner.device_ != device || acquireRenderTapeRuntimeIdentityV4(device) != owner.root_ ||
                !owner.root_->matches(owner.localKey_)) return false;
            dxmt9c_device_cancel_render_tape_identity_capture_v4(device,
                owner.localKey_.token, owner.localKey_.epoch, owner.localQueue_);
            return true;
          });
      if (!detached && backend_) backend_->queue().revokeTapeRuntimeObserver(root_->sink());
    } else if (device_ && acquireRenderTapeRuntimeIdentityV4(device_) == root_ &&
        root_->matches(localKey_))
      dxmt9c_device_cancel_render_tape_identity_capture_v4(
          device_, localKey_.token, localKey_.epoch, localQueue_);
    else if (backend_)
      backend_->queue().revokeTapeRuntimeObserver(root_->sink());
    else
      root_->edges().revoke();
  }
  armed_ = false;
  device_ = nullptr;
  root_.reset();
  backend_.reset();
  controlledCpu_ = false;
  controlledCpuLease_ = {};
  chunk_ = {};
  chunkBound_ = false;
}

bool RenderTapeExecutionV4::prepareTemplates(RenderTapeExecutionV4Preparation& out,
    const CaptureSourceReplayPlanV4& plan,
    const render_tape_runtime::GeneralPreflightPlan& replay,
    std::span<const std::byte> tape,
    RenderTapeExecutionV4PublicationMode mode,
    RenderTapeExecutionV4ProducerContract producerContract, const char*& failure) noexcept {
  failure = "none";
  const auto reject = [&](const char* reason) { failure = reason; return false; };
  if (out.valid_) return reject("v4-preparation-repeat");
  if (mode != RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic &&
      mode != RenderTapeExecutionV4PublicationMode::SemanticMapping)
    return reject("v4-publication-mode-unsupported");
  if (producerContract != RenderTapeExecutionV4ProducerContract::None &&
      producerContract != RenderTapeExecutionV4ProducerContract::PePresentReservationV1)
    return reject("v4-producer-contract-unsupported");
  if (!plan.valid() || plan.events().size() != replay.events.size())
    return reject("v4-plan-input");
  try {
    RenderTapeExecutionV4Preparation staged;
    if (!render_tape_runtime::generalReplayPlanMatchesAuthenticatedV4(tape, plan, replay))
      return reject("v4-general-plan-binding");
    if (mode == RenderTapeExecutionV4PublicationMode::SemanticMapping &&
        !plan.identity().initializers.empty())
      return reject("v4-semantic-initializer-unsupported");
    if (!renderTapeInitialSetupV4SnapshotPrefix(replay, staged.snapshotPrefixEvents_))
      return reject("v4-snapshot-prefix");
    const RenderTapeRuntimeIdentityV4Limits limits{};
    std::vector<RenderTapeQueryCycleState> capturedCycles, localCycles;
    for (const auto& event : replay.events) {
      if (event.type != RenderTapeEventType::ObjectDefine) continue;
      const auto& definition = replay.definitions[event.index];
      if (definition.fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_QUERY) continue;
      RenderTapeQueryDescriptorDecoded descriptor{};
      if (!renderTapeLoadQueryDescriptor(definition.descriptor, descriptor) ||
          !renderTapeQueryDescriptorIdentityMatches(descriptor, definition.fixed.identity))
        return reject("v4-query-descriptor-binding");
      const bool checkpoint = descriptor.schemaVersion == kRenderTapeQueryDescriptorVersion3;
      const bool explicitHistory = descriptor.schemaVersion == kRenderTapeQueryDescriptorVersion4;
      if (!checkpoint && !explicitHistory && (descriptor.initialState != static_cast<std::uint32_t>(
              RenderTapeQueryInitialState::Unissued) || descriptor.initialIssueCycle))
        return reject("v4-query-initial-state-unsupported");
      if (staged.queryTemplates_.size() >= limits.provenance.maxCoordinators)
        return reject("v4-query-binding-capacity");
      staged.queryTemplates_.push_back({.capturedIdentity = definition.fixed.identity,
          .descriptor = descriptor, .definitionEvent = event.eventIndex});
      capturedCycles.push_back(renderTapeInitialQueryCycle(descriptor));
      RenderTapeQueryCycleState local{.type = descriptor.type};
      if (checkpoint) {
        // Exactly one actual fresh-query setup END, never the captured history.
        if (!renderTapeAdvanceQueryCycle(local, 1u, false))
          return reject("v4-query-local-baseline-cycle");
        ++staged.queryBaselineExpected_;
      }
      localCycles.push_back(local);
    }
    staged.localRecordSeeds_.assign(plan.records().begin(), plan.records().end());
    for (std::size_t index = 0u; index < plan.records().size(); ++index) {
      const auto& captured = plan.records()[index];
      if (captured.recordType != D9C_COMMAND_RECORD_QUERY_ISSUE) continue;
      const auto query = std::find_if(staged.queryTemplates_.begin(), staged.queryTemplates_.end(), [&](const auto& value) {
        return renderTapeIdentityV3SameIdentity(value.capturedIdentity, captured.queryIdentity);
      });
      if (query == staged.queryTemplates_.end()) return reject("v4-query-record-definition");
      const auto queryIndex = static_cast<std::size_t>(query - staged.queryTemplates_.begin());
      auto& original = capturedCycles[queryIndex];
      auto& local = localCycles[queryIndex];
      if (!renderTapeAdvanceQueryCycle(original, captured.queryFlags, true) ||
          original.type != captured.queryType || original.issueCycle != captured.issueCycle ||
          original.issueOrdinal != captured.issueOrdinal ||
          !renderTapeAdvanceQueryCycle(local, captured.queryFlags, true))
        return reject("v4-query-captured-fact-binding");
      staged.localRecordSeeds_[index].issueCycle = local.issueCycle;
      staged.localRecordSeeds_[index].issueOrdinal = local.issueOrdinal;
    }
    std::uint64_t records = 0u, raws = 0u;
    for (const auto& event : plan.events()) {
      records += event.recordCount;
      raws += event.commandChunkIndex != noIndex;
      staged.eventFacts_.push_back({event.eventOrdinal, event.type, event.recordCount});
    }
    if (records > limits.provenance.maxDispositions || raws > limits.provenance.maxSettlements ||
        raws > limits.maxAttempts || plan.identity().sources.size() > limits.provenance.maxSources ||
        plan.identity().initializers.size() > limits.provenance.maxInitializers ||
        plan.identity().commands.size() > limits.provenance.maxCommands ||
        plan.identity().coordinators.size() > limits.provenance.maxCoordinators)
      return reject("v4-runtime-capacity");
    if (mode == RenderTapeExecutionV4PublicationMode::SemanticMapping) {
      const RenderTapeSemanticMappingV4Limits mappingLimits{};
      if (plan.events().size() > mappingLimits.maxEvents || records > mappingLimits.maxRecords ||
          plan.records().size() > mappingLimits.maxRecords || raws > mappingLimits.maxRaws ||
          plan.identity().sources.size() > mappingLimits.maxSources ||
          plan.identity().commands.size() > mappingLimits.maxCommands ||
          plan.identity().dispositions.size() > mappingLimits.maxDispositions ||
          plan.identity().coordinators.size() > mappingLimits.maxCoordinators ||
          plan.identity().attempts.size() > mappingLimits.maxAttempts)
        return reject("v4-semantic-mapping-capacity");
      staged.sourceObligations_.reserve(plan.identity().sources.size());
      for (const auto& action : plan.actions()) {
        if (action.kind != ActionKind::ExpectSourcePublication) continue;
        if (staged.sourceObligations_.size() >= plan.identity().sources.size())
          return reject("v4-semantic-source-obligation-capacity");
        staged.sourceObligations_.push_back(action);
      }
      if (staged.sourceObligations_.size() != plan.identity().sources.size())
        return reject("v4-semantic-source-obligation-coverage");
      // Check the supported reference semantic shape before any runtime
      // effect. This cold self-correspondence is discarded; it supplies no
      // candidate publication, traversal, query or completion evidence.
      RenderTapeSemanticMappingV4 referenceFeasibility;
      if (buildRenderTapeSemanticMappingV4(plan, plan.identity(), staged.eventFacts_, plan.records(),
              staged.sourceObligations_, mappingLimits, referenceFeasibility) !=
          RenderTapeSemanticMappingV4Status::Valid)
        return reject("v4-semantic-reference-unsupported");
    }
    for (const auto& action : plan.actions())
      if ((action.kind == ActionKind::ExpectSourcePublication ||
           action.kind == ActionKind::ExpectInitializerPublication) &&
          action.eventIndex < staged.snapshotPrefixEvents_)
        return reject("v4-publication-inside-snapshot-prefix");
    staged.commandDigests_.resize(plan.events().size());
    staged.commandEnvelopes_.resize(plan.events().size());
    staged.recordTypes_.resize(plan.events().size());
    for (const auto& event : replay.events) {
      if (event.type != RenderTapeEventType::CommandChunk) continue;
      const auto& admitted = replay.commandChunks[event.index];
      ImportedChunkView imported;
      if (!importPrevalidatedCommandChunk(admitted.bytes, admitted.envelope, imported))
        return reject("v4-record-type-binding");
      staged.commandDigests_[event.eventIndex] = RenderTapeCaptureSession::sha256(admitted.bytes);
      staged.commandEnvelopes_[event.eventIndex] = admitted.envelope;
      for (const auto& record : imported.records)
        staged.recordTypes_[event.eventIndex].push_back(record.type);
    }
    if (producerContract == RenderTapeExecutionV4ProducerContract::PePresentReservationV1) {
      const auto ordinal = plan.identity().header.presentOrdinal;
      const auto present = std::find_if(plan.events().begin(), plan.events().end(),
          [&](const auto& event) { return event.eventOrdinal == ordinal; });
      if (!ordinal || replay.presentOrdinal != ordinal || present == plan.events().end() ||
          present->commandChunkIndex == noIndex)
        return reject("v4-present-preparation-admission");
      const auto index = static_cast<std::uint32_t>(present - plan.events().begin());
      const auto& types = staged.recordTypes_[index];
      // Selection supplies the producer promise. Authenticated General facts
      // supply its exact terminal Present raw and preceding acknowledged raw,
      // never a publication action or captured command-count target.
      if (index <= staged.snapshotPrefixEvents_ || types.empty() ||
          types.back() != D9C_COMMAND_RECORD_PRESENT ||
          std::count(types.begin(), types.end(), D9C_COMMAND_RECORD_PRESENT) != 1 ||
          plan.events()[index - 1u].commandChunkIndex == noIndex)
        return reject("v4-present-preparation-admission");
      staged.presentPreparation_ = {producerContract, plan.digest(), index, ordinal};
    }
    CaptureSourceReplayCursorV4 cursor;
    if (!cursor.begin(plan)) return reject("v4-cursor-begin");
    // Only noexcept publication follows full validation. No partial authority
    // escapes a failed allocation or a rejected input.
    out.eventFacts_.swap(staged.eventFacts_);
    out.localRecordSeeds_.swap(staged.localRecordSeeds_);
    out.queryTemplates_.swap(staged.queryTemplates_);
    out.commandDigests_.swap(staged.commandDigests_);
    out.commandEnvelopes_.swap(staged.commandEnvelopes_);
    out.recordTypes_.swap(staged.recordTypes_);
    out.sourceObligations_.swap(staged.sourceObligations_);
    out.presentPreparation_ = staged.presentPreparation_;
    out.snapshotPrefixEvents_ = staged.snapshotPrefixEvents_;
    out.queryBaselineExpected_ = staged.queryBaselineExpected_;
    out.plan_ = &plan;
    out.replay_ = &replay;
    out.tape_ = tape;
    out.digest_ = plan.digest();
    out.publicationMode_ = mode;
    out.producerContract_ = producerContract;
    out.valid_ = true;
    return true;
  } catch (...) { return reject("v4-admission-allocation"); }
}

bool RenderTapeExecutionV4::initializePrepared(
    const RenderTapeExecutionV4Preparation& preparation,
    const CaptureSourceReplayPlanV4& plan,
    const render_tape_runtime::GeneralPreflightPlan& replay,
    std::span<const std::byte> tape, RenderTapeExecutionV4Evidence& evidence,
    RenderTapeExecutionV4PublicationMode mode,
    RenderTapeExecutionV4ProducerContract producerContract) noexcept {
  if (plan_ || failed_) return reject("v4-admission-repeat");
  evidence = {};
  evidence.requested = true;
  evidence.publicationMode = mode;
  evidence.producerContract = producerContract;
  if (!preparation.valid_ || preparation.plan_ != &plan ||
      preparation.replay_ != &replay || preparation.tape_.data() != tape.data() ||
      preparation.tape_.size() != tape.size() || !plan.valid() ||
      preparation.digest_ != plan.digest() || preparation.publicationMode_ != mode ||
      preparation.producerContract_ != producerContract)
    return reject("v4-preparation-binding");
  try {
    localRecordFacts_ = preparation.localRecordSeeds_;
    queryBindings_.reserve(preparation.queryTemplates_.size());
    for (const auto& query : preparation.queryTemplates_)
      queryBindings_.push_back({.capturedIdentity = query.capturedIdentity,
          .descriptor = query.descriptor, .definitionEvent = query.definitionEvent});
    if (mode == RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic) {
      evidence.sources.reserve(plan.identity().sources.size());
      evidence.initializers.reserve(plan.identity().initializers.size());
      evidence.coordinators.reserve(plan.identity().coordinators.size());
    }
    if (!cursor_.begin(plan)) return reject("v4-cursor-begin");
    eventFacts_ = preparation.eventFacts_;
    commandDigests_ = preparation.commandDigests_;
    commandEnvelopes_ = preparation.commandEnvelopes_;
    recordTypes_ = preparation.recordTypes_;
    sourceObligations_ = preparation.sourceObligations_;
    snapshotPrefixEvents_ = preparation.snapshotPrefixEvents_;
    presentPreparation_ = preparation.presentPreparation_;
    preparation_ = &preparation;
    plan_ = &plan;
    admittedReplay_ = &replay;
    admittedTape_ = tape;
    evidence_ = &evidence;
    digest_ = preparation.digest_;
    publicationMode_ = mode;
    producerContract_ = producerContract;
    evidence.planDigest = digest_;
    evidence.queryBaselineExpected = preparation.queryBaselineExpected_;
    evidence.sourceObligationsExpected = sourceObligations_.size();
    evidence.presentPreparationEventIndex = presentPreparation_.eventIndex;
    evidence.validated = true;
    evidence.admissionCount = 1u;
    return true;
  } catch (...) { return reject("v4-admission-allocation"); }
}

bool RenderTapeExecutionV4::admit(const CaptureSourceReplayPlanV4& plan,
    const render_tape_runtime::GeneralPreflightPlan& replay,
    std::span<const std::byte> tape, RenderTapeExecutionV4Evidence& evidence,
    RenderTapeExecutionV4PublicationMode mode,
    RenderTapeExecutionV4ProducerContract producerContract) noexcept {
  if (plan_ || failed_) return reject("v4-admission-repeat");
  evidence = {};
  evidence.requested = true;
  evidence.publicationMode = mode;
  evidence.producerContract = producerContract;
  const char* failure = "none";
  if (!prepareTemplates(ownedPreparation_, plan, replay, tape, mode, producerContract, failure))
    return reject(failure);
  return initializePrepared(ownedPreparation_, plan, replay, tape, evidence, mode, producerContract);
}

bool RenderTapeExecutionV4PreparedReplayAccess::prepare(
    RenderTapeExecutionV4Preparation& preparation, const CaptureSourceReplayPlanV4& plan,
    const render_tape_runtime::GeneralPreflightPlan& replay,
    std::span<const std::byte> tape, RenderTapeExecutionV4PublicationMode mode,
    RenderTapeExecutionV4ProducerContract producerContract, const char*& failure) noexcept {
  return RenderTapeExecutionV4::prepareTemplates(
      preparation, plan, replay, tape, mode, producerContract, failure);
}

bool RenderTapeExecutionV4PreparedReplayAccess::initialize(
    RenderTapeExecutionV4& execution, const RenderTapeExecutionV4Preparation& preparation,
    const CaptureSourceReplayPlanV4& plan,
    const render_tape_runtime::GeneralPreflightPlan& replay, std::span<const std::byte> tape,
    RenderTapeExecutionV4Evidence& evidence, RenderTapeExecutionV4PublicationMode mode,
    RenderTapeExecutionV4ProducerContract producerContract) noexcept {
  return execution.initializePrepared(preparation, plan, replay, tape, evidence, mode, producerContract);
}

bool RenderTapeExecutionV4::consumePreparedReplayAdmission(
    const CaptureSourceReplayPlanV4& plan,
    const render_tape_runtime::GeneralPreflightPlan& replay,
    std::span<const std::byte> tape, RenderTapeExecutionV4Evidence& evidence,
    RenderTapeExecutionV4PublicationMode mode,
    RenderTapeExecutionV4ProducerContract producerContract) noexcept {
  const RenderTapePreparedAdmissionV4Facts facts{
      .admitted = plan_ && admittedReplay_ && evidence_,
      .failed = failed_.load(std::memory_order_acquire),
      .consumed = preparedAdmissionConsumed_,
      .pristine = !preparedAdmissionCancelled_ && !armed_ && !device_ && !backend_ &&
          !root_ && !localKey_.token && !localKey_.epoch && !localQueue_ &&
          !controlledCpu_ && !chunkBound_ && event_ == noIndex && nextEvent_ == 0u &&
          cursor_.status() == CaptureSourceReplayCursorV4Status::Ready &&
          cursor_.acknowledged() == 0u && !localRaw_ && !registeredSourceObligations_ &&
          !callbackRecordOpen_ && !callbackDispatchStarted_ &&
          presentPreparation_.phase == RenderTapeExecutionV4PresentPreparationPhase::Admitted,
      .samePlanOwner = plan_ == &plan && plan.valid() && plan.digest() == digest_,
      .sameGeneralOwner = admittedReplay_ == &replay,
      .sameEvidenceOwner = evidence_ == &evidence && evidence.validated &&
          evidence.admissionCount == 1u && evidence.planDigest == digest_ &&
          !evidence.preparedAdmissionConsumed,
      .sameTapeStorage = admittedTape_.data() == tape.data() &&
          admittedTape_.size() == tape.size(),
      .samePublicationMode = publicationMode_ == mode && evidence.publicationMode == mode,
      .sameProducerContract = producerContract_ == producerContract &&
          evidence.producerContract == producerContract,
  };
  if (!renderTapePreparedAdmissionV4ClaimAllowed(facts))
    return reject("v4-prepared-admission-handoff");
  preparedAdmissionConsumed_ = true;
  evidence.preparedAdmissionConsumed = true;
  return true;
}

bool RenderTapeExecutionV4PreparedReplayAccess::consume(
    RenderTapeExecutionV4& execution, const CaptureSourceReplayPlanV4& plan,
    const render_tape_runtime::GeneralPreflightPlan& replay,
    std::span<const std::byte> tape, RenderTapeExecutionV4Evidence& evidence,
    RenderTapeExecutionV4PublicationMode mode,
    RenderTapeExecutionV4ProducerContract producerContract) noexcept {
  return execution.consumePreparedReplayAdmission(
      plan, replay, tape, evidence, mode, producerContract);
}

bool RenderTapeExecutionV4::bindLocalQuery(QueryBinding& binding, D9CDevice* device,
    D9CQuery* query, const core::QueryPollSnapshot& snapshot) noexcept {
  const auto upper = device ? device->dev().upperDevice() : nullptr;
  const auto ordinal = binding.descriptor.schemaVersion == kRenderTapeQueryDescriptorVersion3 ? 1u : 0u;
  if (!upper || !query || query->device != device || !query->obj ||
      !device->wireObjects.contains(query->wireIdentity, query) ||
      snapshot.owner != &device->dev() || snapshot.backend != upper ||
      !snapshot.identity || snapshot.identity != query->obj->identity() ||
      queryWireType(snapshot.type) != binding.descriptor.type || snapshot.issueOrdinal != ordinal ||
      (ordinal && !snapshot.issueDeviceEpoch) || snapshot.active || snapshot.failed ||
      !device->dev().queryPollSnapshotCurrent(snapshot))
    return reject("v4-query-local-object-binding");
  if (binding.bound && (!renderTapeIdentityV3SameIdentity(binding.localIdentity, query->wireIdentity) ||
      binding.coreIdentity != snapshot.identity || binding.deviceEpoch != snapshot.issueDeviceEpoch))
    return reject("v4-query-local-owner-replaced");
  for (const auto& other : queryBindings_)
    if (&other != &binding && other.bound &&
        !renderTapeIdentityV3SameIdentity(other.capturedIdentity, binding.capturedIdentity) &&
        (renderTapeIdentityV3SameIdentity(other.localIdentity, query->wireIdentity) ||
         other.coreIdentity == snapshot.identity)) return reject("v4-query-local-owner-aliased");
  binding.localIdentity = query->wireIdentity;
  binding.coreIdentity = snapshot.identity;
  binding.deviceEpoch = snapshot.issueDeviceEpoch;
  binding.bound = true;
  return true;
}

bool RenderTapeExecutionV4::bindQueryObject(D9CDevice* device,
    D9CWireObjectIdentity captured, D9CQuery* query) noexcept {
  if (!inputBound() || (armed_ && !rootBound())) return false;
  const auto binding = std::find_if(queryBindings_.begin(), queryBindings_.end(), [&](const auto& value) {
    return renderTapeIdentityV3SameIdentity(value.capturedIdentity, captured);
  });
  if (binding == queryBindings_.end() || binding->activated || event_ != binding->definitionEvent ||
      !query || !query->obj) return reject("v4-query-definition-activation");
  const bool alreadyBound = binding->bound;
  if (!bindLocalQuery(*binding, device, query, device->dev().captureQueryPollSnapshot(query->obj)))
    return false;
  binding->activated = true;
  for (std::size_t index = 0u; index < plan_->records().size(); ++index)
    if (plan_->records()[index].recordType == D9C_COMMAND_RECORD_QUERY_ISSUE &&
        renderTapeIdentityV3SameIdentity(plan_->records()[index].queryIdentity, captured))
      localRecordFacts_[index].queryIdentity = binding->localIdentity;
  if (!alreadyBound) ++evidence_->queryObjectsMapped;
  return true;
}

bool RenderTapeExecutionV4::arm(D9CDevice* device,
    std::span<const RenderTapeExecutionV4QueryBaselineInput> seeds) noexcept {
  if (controlledCpu_) return reject("v4-production-arm-cpu-domain");
  return armActual(device, seeds, nullptr);
}

bool RenderTapeExecutionV4::armControlledCpu(D9CDevice* device,
    const RenderTapeControlledCpuV4Capability& capability,
    std::span<const RenderTapeExecutionV4QueryBaselineInput> seeds) noexcept {
  if (!inputBound()) return false;
  if (publicationMode_ != RenderTapeExecutionV4PublicationMode::SemanticMapping ||
      (presentPreparation_.contract != RenderTapeExecutionV4ProducerContract::None &&
       (!renderTapeV4PresentPreparationBound(presentPreparation_, digest_) ||
        presentPreparation_.phase != RenderTapeExecutionV4PresentPreparationPhase::Admitted)) ||
      armed_ || root_ || controlledCpu_) return reject("v4-controlled-cpu-arm-domain");
  struct Context {
    RenderTapeExecutionV4& owner;
    D9CDevice* expected;
    const RenderTapeControlledCpuV4Capability& capability;
    std::span<const RenderTapeExecutionV4QueryBaselineInput> seeds;
  } context{*this, device, capability, seeds};
  const bool armed = capability.lease().visitBackend(RenderTapeControlledCpuV4Boundary::Replay,
      &context, [](void* opaque, D9CDevice* actual, dxmt9::Device&) noexcept {
        auto& input = *static_cast<Context*>(opaque);
        if (actual != input.expected) return false;
        input.owner.controlledCpu_ = true;
        input.owner.controlledCpuLease_ = input.capability.lease();
        return input.owner.armActual(actual, input.seeds, &input.capability);
      });
  return armed ? true : reject("v4-controlled-cpu-arm-authority");
}

bool RenderTapeExecutionV4::armActual(D9CDevice* device,
    std::span<const RenderTapeExecutionV4QueryBaselineInput> seeds,
    const RenderTapeControlledCpuV4Capability* capability) noexcept {
  if (!inputBound()) return false;
  if (!device || armed_ || root_ || event_ != noIndex || nextEvent_ != snapshotPrefixEvents_)
    return reject("v4-arm-input-or-setup-prefix");
  if (!allocateLocalKey(localKey_)) return reject("v4-local-key-exhausted");
  auto upper = device->dev().upperDevice();
  if (!upper) return reject("v4-local-backend-missing");
  if (seeds.size() != evidence_->queryBaselineExpected) return reject("v4-query-baseline-coverage");
  std::vector<QueryBinding> staged;
  std::vector<RenderTapeReplayQueryBaselineV4> baselines;
  try {
    staged = queryBindings_;
    baselines.reserve(seeds.size());
    std::vector<bool> seen(staged.size(), false);
    for (const auto& seed : seeds) {
      const auto binding = std::find_if(staged.begin(), staged.end(), [&](const auto& value) {
        return renderTapeIdentityV3SameIdentity(value.capturedIdentity, seed.capturedIdentity);
      });
      if (binding == staged.end() || binding->descriptor.schemaVersion != kRenderTapeQueryDescriptorVersion3)
        return reject("v4-query-baseline-descriptor");
      const auto index = static_cast<std::size_t>(binding - staged.begin());
      if (seen[index]) return reject("v4-query-baseline-duplicate");
      seen[index] = true;
      if (!bindLocalQuery(*binding, device, seed.query, seed.snapshot)) return false;
      for (const auto& previous : baselines)
        if (previous.snapshot.identity == seed.snapshot.identity)
          return reject("v4-query-baseline-owner-aliased");
      baselines.push_back({seed.query, seed.snapshot});
    }
  } catch (...) { return reject("v4-query-baseline-allocation"); }
  evidence_->preArm = upper->queue().inspectRenderTapeArmState();
  D9CRenderTapeIdentityCaptureV4ArmResult arm{};
  const auto result = capability ?
      capability->beginCapture(device, localKey_.token, localKey_.epoch, baselines, &arm) :
      beginRenderTapeIdentityCaptureV4ForReplay(
          device, localKey_.token, localKey_.epoch, baselines, &arm);
  // Preserve failed public receipts as well as successes. Observation does
  // not grant this executor ownership of a failed or pre-existing capture.
  evidence_->localArmAttempted = true;
  evidence_->localArmHresult = result;
  evidence_->localArm = arm;
  if (result != core::D3D_OK ||
      arm.status != D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE || arm.version != 4u ||
      arm.captureToken != localKey_.token || arm.captureEpoch != localKey_.epoch || !arm.queue)
    return reject("v4-local-arm-failed");
  for (const auto& binding : staged) {
    if (!binding.bound) continue;
    const auto old = std::find_if(queryBindings_.begin(), queryBindings_.end(), [&](const auto& value) {
      return renderTapeIdentityV3SameIdentity(value.capturedIdentity, binding.capturedIdentity);
    });
    if (old != queryBindings_.end() && !old->bound) ++evidence_->queryObjectsMapped;
    for (std::size_t index = 0u; index < plan_->records().size(); ++index)
      if (plan_->records()[index].recordType == D9C_COMMAND_RECORD_QUERY_ISSUE &&
          renderTapeIdentityV3SameIdentity(plan_->records()[index].queryIdentity, binding.capturedIdentity))
        localRecordFacts_[index].queryIdentity = binding.localIdentity;
  }
  queryBindings_ = std::move(staged);
  evidence_->queryBaselinesJoined = seeds.size();
  device_ = device;
  backend_ = std::move(upper);
  root_ = acquireRenderTapeRuntimeIdentityV4(device);
  localQueue_ = arm.queue;
  armed_ = true;
  if (!rootBound() || !snapshotActual()) { cancel(); return false; }
  if (plan_->identity().header.coordinatorDomain != evidence_->actualIdentity.header.coordinatorDomain) {
    (void)reject("v4-exact-coordinator-domain");
    cancel();
    return false;
  }
  lastPublicationEdge_ = evidence_->actualIdentity.header.baselineCompletionEdge;
  evidence_->armed = true;
  return true;
}

bool RenderTapeExecutionV4::snapshotActual() noexcept {
  if (!rootBound()) return false;
  RenderTapeIdentityV4View identity;
  RenderTapeExecutionTopologyV1View topology;
  bool copied = false;
  const auto lease = backend_->queue().acquireTapeRuntimeCapture();
  if (!lease.visit([&](render::TapeRuntimeCaptureAttachment& attachment) noexcept {
        if (&attachment == root_.get())
          copied = root_->snapshot(localKey_, identity, topology, false);
      }) || !copied ||
      identity.header.queue != localQueue_ || topology.header.queue != localQueue_ ||
      identity.header.captureToken != localKey_.token ||
      identity.header.captureEpoch != localKey_.epoch)
    return reject("v4-local-snapshot");
  evidence_->actualAttemptsObserved |= !identity.attempts.empty();
  evidence_->actualIdentity = std::move(identity);
  evidence_->actualTopology = std::move(topology);
  return true;
}

bool RenderTapeExecutionV4::publishedSources(std::size_t& cursor,
    std::vector<core::metalqueue::ReplaySourcePublicationReceipt>& out) noexcept {
  out.clear();
  if (!rootBound()) return false;
  return root_->copyPublishedSources(localKey_, cursor, out) ? true :
      reject("v4-published-source-pull");
}

bool RenderTapeExecutionV4::ownsWritingContinuation(const WritingContinuation& value) noexcept {
  if (!rootBound() || event_ == noIndex || event_ >= plan_->events().size() || localRaw_ ||
      plan_->events()[event_].commandChunkIndex == noIndex) return false;
  return root_->ownsWritingContinuation(localKey_, value.writer, value.source, value.commandCount);
}

bool RenderTapeExecutionV4::preparePresentReservation() noexcept {
  if (!controlledCpu_) {
    evidence_->presentPreparationAttempted = true;
    evidence_->presentPreparationHresult = prepareRenderTapePresentReservation(device_);
    return evidence_->presentPreparationHresult == core::D3D_OK && rootBound();
  }
  // A prior authority check cannot pin the borrowed wrapper across the real
  // flush/wait. Retain the exact fixture guard for preparation, then validate
  // authority again after any concurrent consumer completion or failure.
  const bool prepared = controlledCpuLease_.visitBackend(RenderTapeControlledCpuV4Boundary::Replay,
      this, [](void* opaque, D9CDevice* actual, dxmt9::Device& backend) noexcept {
        auto& owner = *static_cast<RenderTapeExecutionV4*>(opaque);
        if (owner.device_ != actual || owner.backend_.get() != &backend ||
            !owner.rootBoundActual(false)) return false;
        owner.evidence_->presentPreparationAttempted = true;
        owner.evidence_->presentPreparationHresult = prepareRenderTapePresentReservation(actual);
        return owner.evidence_->presentPreparationHresult == core::D3D_OK &&
            owner.rootBoundActual(false);
      });
  const bool stillBound = rootBound();
  return prepared && stillBound;
}

bool RenderTapeExecutionV4::beginEvent(std::uint32_t index) noexcept {
  if (!inputBound()) return false;
  const auto ordinal = index < plan_->events().size() ? plan_->events()[index].eventOrdinal : 0u;
  if (presentPreparation_.contract == RenderTapeExecutionV4ProducerContract::PePresentReservationV1 &&
      index == presentPreparation_.eventIndex) {
    // rawSettled already authenticated committed coverage and the real ACK.
    // Keep that raw's observer cut intact until preparation and its original
    // AfterEvent comparison have both finished; no new event is entered yet.
    if (!rootBound() || event_ == noIndex || event_ + 1u != index ||
        !eventReplayed_ || !localRaw_ || plan_->events()[event_].commandChunkIndex == noIndex)
      return reject("v4-present-preparation-prior-raw");
  }
  const auto preparation = renderTapeV4PresentPreparationBeforeClose(
      presentPreparation_, digest_, index, ordinal,
      [&]() noexcept { return preparePresentReservation(); },
      [&]() noexcept { return event_ == noIndex || closeEvent(); });
  evidence_->presentPreparationCompleted = presentPreparation_.phase ==
      RenderTapeExecutionV4PresentPreparationPhase::Completed;
  if (preparation == RenderTapeExecutionV4PresentPreparationResult::InvalidBinding)
    return reject("v4-present-preparation-binding");
  if (preparation == RenderTapeExecutionV4PresentPreparationResult::PreparationFailed)
    return reject("v4-present-preparation-failed");
  if (preparation == RenderTapeExecutionV4PresentPreparationResult::CloseFailed) return false;
  if (index != nextEvent_ || index >= plan_->events().size() ||
      (index >= snapshotPrefixEvents_ && !rootBound())) return reject("v4-event-order");
  event_ = index;
  eventReplayed_ = chunkBound_ = false;
  callbackNextRecord_ = 0u;
  callbackRecord_ = noIndex;
  callbackRecordOpen_ = callbackDispatchStarted_ = false;
  localRaw_ = lastRollbackGeneration_ = 0u;
  if (armed_ && (dxmt9c_device_note_render_tape_identity_cut_v4(device_, localKey_.token,
          localKey_.epoch, localQueue_, plan_->events()[index].eventOrdinal,
          static_cast<std::uint32_t>(Cut::BeforeEvent)) != core::D3D_OK || !rootBound()))
    return reject("v4-local-before-event-cut");
  return true;
}

bool RenderTapeExecutionV4::replayedEvent(std::uint32_t index) noexcept {
  if (!inputBound()) return false;
  if (event_ == noIndex || index != event_ || eventReplayed_ ||
      plan_->events()[event_].commandChunkIndex != noIndex)
    return reject("v4-noncommand-event-boundary");
  for (const auto& binding : queryBindings_)
    if (binding.definitionEvent == index && !binding.activated)
      return reject("v4-query-definition-not-activated");
  if (armed_ && (!rootBound() ||
      dxmt9c_device_note_render_tape_identity_cut_v4(device_, localKey_.token,
          localKey_.epoch, localQueue_, plan_->events()[event_].eventOrdinal,
          static_cast<std::uint32_t>(Cut::AfterEvent)) != core::D3D_OK || !rootBound()))
    return reject("v4-local-after-event-cut");
  eventReplayed_ = true;
  // Prefix publication is forbidden at admission, so there is no deferred
  // receipt check. arm is possible only after genuine caller setup completion.
  return event_ < snapshotPrefixEvents_ ? closeEvent() : true;
}

bool RenderTapeExecutionV4::bindCommandChunk(std::span<const std::byte> bytes,
    const CommandChunkEnvelope& envelope, std::span<void* const> objects) noexcept {
  if (!rootBound()) return false;
  if (event_ == noIndex || eventReplayed_ || chunkBound_ || localRaw_ ||
      plan_->events()[event_].commandChunkIndex == noIndex ||
      !sameEnvelope(envelope, commandEnvelopes_[event_]) ||
      RenderTapeCaptureSession::sha256(bytes) != commandDigests_[event_] ||
      !importPrevalidatedCommandChunk(bytes, envelope, chunk_) ||
      chunk_.records.size() != plan_->events()[event_].recordCount ||
      objects.size() != chunk_.handles.size() ||
      std::any_of(objects.begin(), objects.end(), [](const auto* object) { return !object; }))
    return reject("v4-exact-command-binding");
  for (std::size_t index = 0u; index < chunk_.handles.size(); ++index) {
    const auto& handle = chunk_.handles[index];
    if (handle.kind != D9C_CHUNK_HANDLE_KIND_QUERY) continue;
    const D9CWireObjectIdentity captured{handle.kind, handle.generation, handle.objectId};
    const auto binding = std::find_if(queryBindings_.begin(), queryBindings_.end(), [&](const auto& value) {
      return renderTapeIdentityV3SameIdentity(value.capturedIdentity, captured);
    });
    auto* query = static_cast<D9CQuery*>(objects[index]);
    if (binding == queryBindings_.end() || !binding->bound || !binding->activated ||
        query->device != device_ || !query->obj ||
        !renderTapeIdentityV3SameIdentity(binding->localIdentity, query->wireIdentity) ||
        !device_->wireObjects.contains(binding->localIdentity, query) ||
        query->obj->identity() != binding->coreIdentity)
      return reject("v4-query-command-owner-binding");
    const auto snapshot = device_->dev().captureQueryPollSnapshot(query->obj);
    if (!device_->dev().queryPollSnapshotCurrent(snapshot) ||
        queryWireType(snapshot.type) != binding->descriptor.type ||
        (binding->deviceEpoch && snapshot.issueDeviceEpoch != binding->deviceEpoch))
      return reject("v4-query-command-issue-binding");
  }
  chunkBound_ = true;
  return true;
}

bool RenderTapeExecutionV4::rawStarted(std::uint64_t event, std::uint64_t raw,
    std::uint32_t count) noexcept {
  if (!rootBound()) return false;
  if (event_ == noIndex || !chunkBound_ || eventReplayed_ || localRaw_ || !raw ||
      event != plan_->events()[event_].eventOrdinal || count != chunk_.records.size())
    return reject("v4-local-raw-start");
  localRaw_ = raw;
  return true;
}

bool RenderTapeExecutionV4::recoverRolledBackRecord(std::uint32_t index) noexcept {
  // Reentry is authority only after the production root has confirmed actual
  // destination rollback and restored its checkpoint, not after an HRESULT.
  if (!snapshotActual()) return false;
  const auto& attempts = evidence_->actualIdentity.attempts;
  if (attempts.empty()) return reject("v4-record-reentry-without-rollback");
  const auto& rollback = attempts.back();
  if (rollback.eventOrdinal != plan_->events()[event_].eventOrdinal ||
      rollback.rawOrdinal != localRaw_ || rollback.attemptGeneration <= lastRollbackGeneration_ ||
      rollback.outcome != static_cast<std::uint32_t>(RenderTapeIdentityV4AttemptOutcome::RolledBack) ||
      rollback.effectEdge || rollback.firstRecord != index)
    return reject("v4-record-reentry-without-rollback");
  lastRollbackGeneration_ = rollback.attemptGeneration;
  callbackNextRecord_ = index;
  callbackRecordOpen_ = callbackDispatchStarted_ = false;
  return true;
}

bool RenderTapeExecutionV4::beforeRecord(std::uint32_t index, std::uint32_t type) noexcept {
  if (!rootBound()) return false;
  if (event_ == noIndex || !chunkBound_ || !localRaw_ || eventReplayed_ ||
      index >= recordTypes_[event_].size() || recordTypes_[event_][index] != type)
    return reject("v4-record-type-or-raw-binding");
  if ((callbackRecordOpen_ || index != callbackNextRecord_) && !recoverRolledBackRecord(index))
    return false;
  callbackRecord_ = index;
  callbackRecordOpen_ = true;
  callbackDispatchStarted_ = false;
  return true;
}

bool RenderTapeExecutionV4::beforeDispatch(std::uint32_t index, std::uint32_t type) noexcept {
  if (!rootBound()) return false;
  if (!callbackRecordOpen_ || callbackDispatchStarted_ || index != callbackRecord_ ||
      event_ == noIndex || index >= recordTypes_[event_].size() || recordTypes_[event_][index] != type)
    return reject("v4-record-dispatch-phase");
  callbackDispatchStarted_ = true;
  return true;
}

bool RenderTapeExecutionV4::recordDispatched(std::uint32_t index, std::uint32_t type,
    std::int32_t result) noexcept {
  if (!rootBound()) return false;
  if (!callbackRecordOpen_ || !callbackDispatchStarted_ || index != callbackRecord_ ||
      event_ == noIndex || index >= recordTypes_[event_].size() || recordTypes_[event_][index] != type)
    return reject("v4-record-dispatch-result-phase");
  // A private pre-effect failure can be rolled back/retried by its real owner.
  // The failed candidate remains open until authenticated rollback reentry.
  if (result < 0) return true;
  callbackRecordOpen_ = callbackDispatchStarted_ = false;
  callbackNextRecord_ = index + 1u;
  return true;
}

bool RenderTapeExecutionV4::qualifyRawCoverage() noexcept {
  const auto& actual = evidence_->actualIdentity;
  const auto& event = plan_->events()[event_];
  if (actual.settlements.size() != evidence_->acknowledgedRaws + 1u)
    return reject("v4-actual-raw-ack-count");
  const auto& settlement = actual.settlements.back();
  if (settlement.eventOrdinal != event.eventOrdinal || settlement.rawOrdinal != localRaw_ ||
      settlement.recordCount != event.recordCount || !settlement.replayEdge)
    return reject("v4-actual-raw-ack-binding");
  std::uint32_t frontier = 0u;
  bool committed = false;
  for (const auto& attempt : actual.attempts) {
    if (attempt.eventOrdinal != event.eventOrdinal) continue;
    if (attempt.rawOrdinal != localRaw_ || !attempt.attemptGeneration || !attempt.beginEdge ||
        attempt.terminalEdge <= attempt.beginEdge ||
        std::uint64_t{attempt.firstRecord} + attempt.recordCount > event.recordCount)
      return reject("v4-actual-attempt-binding");
    if (attempt.outcome == static_cast<std::uint32_t>(RenderTapeIdentityV4AttemptOutcome::Committed)) {
      if (attempt.firstRecord != frontier) return reject("v4-actual-committed-coverage");
      frontier += attempt.recordCount;
      committed = true;
    } else if (attempt.outcome != static_cast<std::uint32_t>(
                   RenderTapeIdentityV4AttemptOutcome::RolledBack) ||
               attempt.effectEdge || attempt.firstRecord < frontier) {
      return reject("v4-actual-attempt-outcome");
    }
  }
  if (!committed || frontier != event.recordCount) return reject("v4-actual-committed-coverage");
  frontier = 0u;
  for (const auto& disposition : actual.dispositions) {
    if (disposition.eventOrdinal != event.eventOrdinal) continue;
    if (disposition.firstRecord != frontier || !disposition.recordCount ||
        std::uint64_t{frontier} + disposition.recordCount > event.recordCount)
      return reject("v4-actual-record-coverage");
    for (std::uint32_t offset = 0u; offset < disposition.recordCount; ++offset)
      if (!renderTapeIdentityV3DispositionAdmitted(recordTypes_[event_][frontier + offset],
              static_cast<RenderTapeIdentityV3DispositionKind>(disposition.kind)))
        return reject("v4-actual-canonical-record-kind");
    frontier += disposition.recordCount;
  }
  return frontier == event.recordCount ? true : reject("v4-actual-record-coverage");
}

bool RenderTapeExecutionV4::rawSettled(std::uint64_t raw, std::uint32_t count,
    bool succeeded) noexcept {
  if (!rootBound()) return false;
  if (event_ == noIndex || !chunkBound_ || eventReplayed_ || !localRaw_ ||
      raw != localRaw_ || count != chunk_.records.size() || !succeeded ||
      callbackRecordOpen_ || callbackNextRecord_ != count)
    return reject("v4-actual-raw-boundary");
  if (!snapshotActual() || !qualifyRawCoverage()) return false;
  ++evidence_->acknowledgedRaws;
  evidence_->dispatchedRecords += count;
  eventReplayed_ = true;
  chunk_ = {}; // Command bytes are no longer borrowed after synchronous ACK.
  // A real AfterEvent publication may follow the raw replay call. Original
  // cursor actions are consumed at the next boundary, before changing cuts.
  return true;
}

ProviderReplayObservationV4 RenderTapeExecutionV4::observation() noexcept {
  if (!rootBound() || event_ == noIndex || !chunkBound_ || eventReplayed_ || localRaw_) {
    (void)reject("v4-observation-input");
    return {};
  }
  return {.captureToken = localKey_.token, .captureEpoch = localKey_.epoch,
      .queue = localQueue_, .eventOrdinal = plan_->events()[event_].eventOrdinal, .context = this,
      .rawStarted = [](void* p, std::uint64_t event, std::uint64_t raw, std::uint32_t count) noexcept {
        return static_cast<RenderTapeExecutionV4*>(p)->rawStarted(event, raw, count);
      },
      .beforeRecord = [](void* p, std::uint32_t index, std::uint32_t type) noexcept {
        return static_cast<RenderTapeExecutionV4*>(p)->beforeRecord(index, type);
      },
      .beforeDispatch = [](void* p, std::uint32_t index, std::uint32_t type) noexcept {
        return static_cast<RenderTapeExecutionV4*>(p)->beforeDispatch(index, type);
      },
      .recordDispatched = [](void* p, std::uint32_t index, std::uint32_t type,
          std::int32_t result) noexcept {
        return static_cast<RenderTapeExecutionV4*>(p)->recordDispatched(index, type, result);
      },
      .rawSettled = [](void* p, std::uint64_t raw, std::uint32_t count, bool succeeded) noexcept {
        return static_cast<RenderTapeExecutionV4*>(p)->rawSettled(raw, count, succeeded);
      }};
}

bool RenderTapeExecutionV4::qualifyPublication(const CaptureSourceReplayPlanV4Action& action) noexcept {
  const auto& expected = plan_->identity();
  const auto& actual = evidence_->actualIdentity;
  std::uint64_t edge = 0u;
  if (action.kind == ActionKind::ExpectSourcePublication) {
    const auto index = action.tableIndex;
    if (index != expectedSources_ || index >= actual.sources.size() ||
        !sameCut(expected.sources[index].lifecycle, actual.sources[index].lifecycle))
      return reject("v4-exact-source-publication");
    const auto& source = actual.sources[index].lifecycle;
    std::size_t count = 0u;
    for (std::size_t command = 0u; command < expected.commands.size(); ++command) {
      if (expected.commands[command].sourceIndex != index) continue;
      if (command >= actual.commands.size() || !sameCommand(expected.commands[command], actual.commands[command]))
        return reject("v4-exact-source-extent");
      ++count;
    }
    if (count != source.commandCount) return reject("v4-exact-source-extent");
    edge = source.publicationEdge;
    evidence_->sources.push_back({index, index, source.seqId, edge});
    ++expectedSources_;
  } else if (action.kind == ActionKind::ExpectInitializerPublication) {
    const auto index = action.tableIndex;
    if (index != expectedInitializers_ || index >= actual.initializers.size() ||
        !sameCut(expected.initializers[index], actual.initializers[index]))
      return reject("v4-exact-initializer-publication");
    const auto& initializer = actual.initializers[index];
    edge = initializer.publicationEdge;
    evidence_->initializers.push_back({index, index, initializer.sequence, edge});
    ++expectedInitializers_;
  } else {
    return reject("v4-publication-action-kind");
  }
  if (edge <= lastPublicationEdge_) return reject("v4-actual-publication-edge-order");
  lastPublicationEdge_ = edge;
  return true;
}

bool RenderTapeExecutionV4::closeEvent() noexcept {
  if (!inputBound()) return false;
  if (event_ == noIndex || !eventReplayed_) return reject("v4-event-not-replayed-or-acked");
  if (armed_ && !snapshotActual()) return false;
  try {
    CaptureSourceReplayPlanV4Action action;
    bool eventAction = false;
    while (cursor_.peek(*plan_, action) && action.eventIndex == event_) {
      if (action.kind == ActionKind::ExpectSourcePublication ||
          action.kind == ActionKind::ExpectInitializerPublication) {
        if (publicationMode_ == RenderTapeExecutionV4PublicationMode::SemanticMapping) {
          if (!armed_ || !registerSemanticPublication(action)) return false;
        } else if (!armed_ || !qualifyPublication(action)) return false;
      } else if (action.kind == ActionKind::ReplayEvent) {
        if (plan_->events()[event_].commandChunkIndex != noIndex || eventAction)
          return reject("v4-canonical-event-action");
        eventAction = true;
      } else if (action.kind == ActionKind::DispatchRecord) {
        if (!localRaw_ || action.recordIndex >= callbackNextRecord_)
          return reject("v4-canonical-dispatch-without-ack");
      } else if (action.kind == ActionKind::AcknowledgeRaw) {
        if (!localRaw_ || !evidence_->acknowledgedRaws || eventAction)
          return reject("v4-canonical-raw-action");
        eventAction = true;
      }
      if (!cursor_.acknowledge(*plan_, action)) return reject("v4-canonical-cursor-order");
    }
    if (!eventAction || cursor_.status() == CaptureSourceReplayCursorV4Status::Rejected)
      return reject("v4-canonical-event-coverage");
    ++evidence_->replayedEvents;
    nextEvent_ = event_ + 1u;
    event_ = noIndex;
    localRaw_ = 0u;
    chunk_ = {};
    chunkBound_ = eventReplayed_ = false;
    return true;
  } catch (...) { return reject("v4-event-evidence-allocation"); }
}

bool RenderTapeExecutionV4::registerSemanticPublication(
    const CaptureSourceReplayPlanV4Action& action) noexcept {
  if (!inputBound()) return false;
  if (publicationMode_ != RenderTapeExecutionV4PublicationMode::SemanticMapping ||
      action.kind != ActionKind::ExpectSourcePublication ||
      registeredSourceObligations_ >= sourceObligations_.size() ||
      action != sourceObligations_[registeredSourceObligations_])
    return reject("v4-semantic-source-obligation-order");
  // No source snapshot, sequence or publication edge is inferred here. The
  // admitted bounded action remains pending until complete candidate mapping.
  ++registeredSourceObligations_;
  evidence_->sourceObligationsRegistered = registeredSourceObligations_;
  evidence_->sourceObligationsPending = registeredSourceObligations_;
  return true;
}

bool RenderTapeExecutionV4::qualifyExactSources() noexcept {
  const auto& a = plan_->identity();
  const auto& b = evidence_->actualIdentity;
  if (a.sources.size() != b.sources.size() || a.initializers.size() != b.initializers.size() ||
      a.commands.size() != b.commands.size() || a.coordinators.size() != b.coordinators.size() ||
      a.settlements.size() != b.settlements.size() || expectedSources_ != a.sources.size() ||
      expectedInitializers_ != a.initializers.size()) return reject("v4-exact-table-extents");
  for (std::size_t index = 0u; index < a.sources.size(); ++index)
    if (!sameCut(a.sources[index].lifecycle, b.sources[index].lifecycle))
      return reject("v4-exact-source-cut");
  for (std::size_t index = 0u; index < a.initializers.size(); ++index)
    if (!sameCut(a.initializers[index], b.initializers[index]))
      return reject("v4-exact-initializer-cut");
  for (std::size_t index = 0u; index < a.commands.size(); ++index)
    if (!sameCommand(a.commands[index], b.commands[index])) return reject("v4-exact-command-extent");
  // Different real attempt segmentation may split or merge neutral disposition
  // rows. Compare each original record/subdraw locator, not row partitions.
  std::size_t ai = 0u, bi = 0u;
  for (const auto& event : plan_->events()) {
    for (std::uint32_t record = 0u; record < event.recordCount; ++record) {
      while (ai < a.dispositions.size() &&
          (a.dispositions[ai].eventOrdinal < event.eventOrdinal ||
           std::uint64_t{a.dispositions[ai].firstRecord} + a.dispositions[ai].recordCount <= record)) ++ai;
      while (bi < b.dispositions.size() &&
          (b.dispositions[bi].eventOrdinal < event.eventOrdinal ||
           std::uint64_t{b.dispositions[bi].firstRecord} + b.dispositions[bi].recordCount <= record)) ++bi;
      if (ai == a.dispositions.size() || bi == b.dispositions.size())
        return reject("v4-exact-record-mapping");
      const auto& x = a.dispositions[ai];
      const auto& y = b.dispositions[bi];
      if (x.eventOrdinal != event.eventOrdinal || y.eventOrdinal != event.eventOrdinal ||
          x.firstRecord > record || y.firstRecord > record || x.kind != y.kind ||
          x.commandIndex != y.commandIndex || x.coordinatorIndex != y.coordinatorIndex ||
          bool(x.subdrawCount) != bool(y.subdrawCount) ||
          (x.subdrawCount && (x.firstSubdraw + record - x.firstRecord !=
                             y.firstSubdraw + record - y.firstRecord)))
        return reject("v4-exact-record-mapping");
    }
  }
  try {
    if (a.header.coordinatorDomain != b.header.coordinatorDomain)
      return reject("v4-exact-coordinator-domain");
    for (std::uint32_t index = 0u; index < a.coordinators.size(); ++index) {
      const auto& captured = a.coordinators[index];
      const auto& actual = b.coordinators[index];
      if (captured.authority != actual.authority)
        return reject("v4-exact-coordinator-authority");
      const auto& x = captured.provenance;
      const auto& y = actual.provenance;
      D9CWireObjectIdentity queryIdentity = x.queryIdentity;
      auto issueCycle = x.issueCycle;
      auto issueOrdinal = x.issueOrdinal;
      if (x.recordType == D9C_COMMAND_RECORD_QUERY_ISSUE) {
        const auto fact = std::find_if(localRecordFacts_.begin(), localRecordFacts_.end(), [&](const auto& value) {
          return value.eventOrdinal == x.eventOrdinal && value.recordIndex == x.recordIndex;
        });
        if (fact == localRecordFacts_.end() || fact->recordType != x.recordType ||
            fact->queryFlags != x.flags || fact->queryType != x.queryType)
          return reject("v4-query-coordinator-fact-binding");
        queryIdentity = fact->queryIdentity;
        issueCycle = fact->issueCycle;
        issueOrdinal = fact->issueOrdinal;
      }
      if (x.eventOrdinal != y.eventOrdinal || x.recordIndex != y.recordIndex || x.recordType != y.recordType ||
          x.flags != y.flags || x.queryType != y.queryType ||
          !renderTapeIdentityV3SameIdentity(queryIdentity, y.queryIdentity) ||
          issueCycle != y.issueCycle || issueOrdinal != y.issueOrdinal || x.dispatchResult != y.dispatchResult ||
          x.releaseAction != y.releaseAction || x.releaseWriter != y.releaseWriter ||
          (x.prefixKind != y.prefixKind && !(baselinePrefix(x.prefixKind) && baselinePrefix(y.prefixKind))) ||
          x.sourceIndex != y.sourceIndex || x.initializerIndex != y.initializerIndex || x.commandEnd != y.commandEnd)
        return reject("v4-exact-coordinator-prefix");
      evidence_->coordinators.push_back({index, index, y.sourceIndex, y.initializerIndex,
          y.commandEnd, y.prefixSequence, y.prefixInitializer});
    }
    return true;
  } catch (...) { return reject("v4-coordinator-mapping-allocation"); }
}

bool RenderTapeExecutionV4::qualifySemanticMapping() noexcept {
  if (!inputBound()) return false;
  if (publicationMode_ != RenderTapeExecutionV4PublicationMode::SemanticMapping ||
      registeredSourceObligations_ != sourceObligations_.size())
    return reject("v4-semantic-source-obligations-incomplete");
  RenderTapeSemanticMappingV4 mapping;
  evidence_->semanticMappingStatus = buildRenderTapeSemanticMappingV4(*plan_,
      evidence_->actualIdentity, eventFacts_, localRecordFacts_,
      std::span(sourceObligations_).first(registeredSourceObligations_), {}, mapping);
  if (evidence_->semanticMappingStatus != RenderTapeSemanticMappingV4Status::Valid)
    return reject("v4-semantic-mapping-invalid");
  if (!mapping.valid() || mapping.planDigest() != digest_ ||
      mapping.obligations().size() != registeredSourceObligations_) {
    evidence_->semanticMappingStatus = RenderTapeSemanticMappingV4Status::SemanticMismatch;
    return reject("v4-semantic-mapping-invalid");
  }
  for (std::size_t index = 0u; index < mapping.obligations().size(); ++index) {
    const auto& obligation = mapping.obligations()[index];
    if (obligation.capturedIndex != sourceObligations_[index].tableIndex ||
        !obligation.sourceRelationCount ||
        std::uint64_t{obligation.firstSourceRelation} + obligation.sourceRelationCount >
            mapping.sources().size()) {
      evidence_->semanticMappingStatus = RenderTapeSemanticMappingV4Status::SemanticMismatch;
      return reject("v4-semantic-source-obligation-undischarged");
    }
    for (const auto& relation : mapping.sources().subspan(
             obligation.firstSourceRelation, obligation.sourceRelationCount))
      if (relation.capturedIndex != obligation.capturedIndex ||
          relation.candidateIndex >= evidence_->actualIdentity.sources.size() ||
          (!relation.recordCount && !relation.coordinatorCommandCount)) {
        evidence_->semanticMappingStatus = RenderTapeSemanticMappingV4Status::SemanticMismatch;
        return reject("v4-semantic-source-obligation-undischarged");
      }
  }
  try {
    evidence_->semanticMapping = std::make_shared<const RenderTapeSemanticMappingV4>(std::move(mapping));
  } catch (...) {
    evidence_->semanticMappingStatus = RenderTapeSemanticMappingV4Status::AllocationFailed;
    return reject("v4-semantic-mapping-allocation");
  }
  evidence_->sourceObligationsQualified = registeredSourceObligations_;
  evidence_->sourceObligationsPending = 0u;
  evidence_->semanticMappingComplete = true;
  return true;
}

bool RenderTapeExecutionV4::finishTraversal() noexcept {
  if (!rootBound()) return false;
  if (event_ != noIndex && !closeEvent()) return false;
  if (nextEvent_ != plan_->events().size() ||
      cursor_.status() != CaptureSourceReplayCursorV4Status::Complete ||
      evidence_->acknowledgedRaws != plan_->identity().settlements.size())
    return reject("v4-execution-incomplete");
  if (presentPreparation_.contract == RenderTapeExecutionV4ProducerContract::PePresentReservationV1 &&
      presentPreparation_.phase != RenderTapeExecutionV4PresentPreparationPhase::Completed)
    return reject("v4-present-preparation-incomplete");
  if (evidence_->queryBaselinesJoined != evidence_->queryBaselineExpected ||
      evidence_->queryObjectsMapped != queryBindings_.size() ||
      std::any_of(queryBindings_.begin(), queryBindings_.end(), [](const auto& binding) {
        return !binding.bound || !binding.activated;
      })) return reject("v4-query-mapping-incomplete");
  evidence_->canonicalTraversalComplete = true;
  evidence_->rawAckCoverageComplete = true;
  return true;
}

bool RenderTapeExecutionV4::finish() noexcept {
  if (controlledCpu_) return reject("v4-production-finish-cpu-domain");
  if (!finishTraversal()) return false;
  try {
    D9CRenderTapeIdentityCaptureV4Result queried{}, copied{};
    const auto present = plan_->identity().header.presentOrdinal;
    if (dxmt9c_device_finish_render_tape_identity_capture_v4(device_, localKey_.token,
            localKey_.epoch, localQueue_, present, &queried, nullptr, 0u) != core::D3D_OK ||
        queried.status != D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE || queried.version != 4u ||
        queried.captureToken != localKey_.token || queried.captureEpoch != localKey_.epoch ||
        queried.queue != localQueue_ || queried.presentOrdinal != present ||
        queried.byteCount < sizeof(RenderTapeIdentityV4TransferHeader) ||
        queried.byteCount > std::numeric_limits<std::size_t>::max())
      return reject("v4-actual-finish-incomplete");
    if (!rootBound(true)) return false;
    std::vector<std::byte> bytes(static_cast<std::size_t>(queried.byteCount));
    if (dxmt9c_device_finish_render_tape_identity_capture_v4(device_, localKey_.token,
            localKey_.epoch, localQueue_, present, &copied, bytes.data(), bytes.size()) != core::D3D_OK ||
        !sameFinishReceipt(queried, copied) || !rootBound(true))
      return reject("v4-actual-finish-receipt");
    RenderTapeIdentityV4View identity;
    RenderTapeExecutionTopologyV1View topology;
    if (decodeRenderTapeIdentityV4SnapshotTransfer(bytes, localKey_, localQueue_, identity, topology) !=
            RenderTapeIdentityV4TransferStatus::Valid ||
        validateRenderTapeIdentityV4Rows(identity, eventFacts_, localRecordFacts_, true) !=
            RenderTapeIdentityV4Status::Valid ||
        validateRenderTapeExecutionTopologyV1Rows(identity, topology) != RenderTapeExecutionTopologyV1Status::Valid)
      return reject("v4-actual-provenance-or-topology");
    evidence_->localFinish = copied;
    evidence_->actualIdentity = std::move(identity);
    evidence_->actualTopology = std::move(topology);
    evidence_->actualLifecycleComplete = true;
    evidence_->actualTopologyComplete = true;
    // Topology and attempts were independently validated above. Captured plan
    // partitions, pass groups, execution ordinals and attempt counts are NOT
    // candidate equality criteria, even in exact-source diagnostic mode.
    if (publicationMode_ == RenderTapeExecutionV4PublicationMode::SemanticMapping) {
      if (!qualifySemanticMapping()) return false;
    } else {
      if (!qualifyExactSources()) return false;
      evidence_->exactSourceDiagnosticComplete = true;
    }
    evidence_->complete = true;
    cancel();
    return true;
  } catch (...) { return reject("v4-finish-allocation"); }
}

bool RenderTapeExecutionV4::finishControlledCpu(const RenderTapeControlledCpuV4Capability& capability,
    RenderTapeControlledCpuV4Evidence& out) noexcept {
  out = {};
  if (!controlledCpu_ || publicationMode_ != RenderTapeExecutionV4PublicationMode::SemanticMapping ||
      !capability.authority_ || controlledCpuLease_.authority_.lock() != capability.authority_)
    return reject("v4-controlled-cpu-finish-domain");
  if (!finishTraversal()) return false;
  try {
    RenderTapeIdentityV4View identity;
    RenderTapeActionWitnessV1 witness;
    if (!capability.finishCapture(device_, localKey_, localQueue_,
            plan_->identity().header.presentOrdinal, identity, witness) ||
        validateRenderTapeIdentityV4Rows(identity, eventFacts_, localRecordFacts_, true) !=
            RenderTapeIdentityV4Status::Valid)
      return reject("v4-controlled-cpu-provenance-incomplete");
    // Qualification is staged. Failed CPU output is empty, and the existing
    // production lifecycle/topology/complete flags never enter this domain.
    RenderTapeExecutionV4Evidence staged = *evidence_;
    staged.actualIdentity = std::move(identity);
    auto* observed = evidence_;
    evidence_ = &staged;
    const bool mapped = qualifySemanticMapping();
    evidence_ = observed;
    if (!mapped) return false;
    RenderTapeControlledCpuV4Evidence candidate;
    candidate.domain = RenderTapeControlledCpuV4Evidence::Domain::ControlledCpu;
    candidate.controlledCpuComplete = true;
    candidate.canonicalTraversalComplete = true;
    candidate.rawAckCoverageComplete = true;
    candidate.semanticMappingComplete = true;
    candidate.planDigest = digest_;
    candidate.sourceObligationsRegistered = staged.sourceObligationsRegistered;
    candidate.sourceObligationsQualified = staged.sourceObligationsQualified;
    candidate.semanticMapping = std::move(staged.semanticMapping);
    candidate.actualIdentity = std::move(staged.actualIdentity);
    candidate.actionWitness = witness;
    out = std::move(candidate);
    cancel();
    return true;
  } catch (...) { return reject("v4-controlled-cpu-finish-allocation"); }
}

} // namespace dxmt9::d3d9
