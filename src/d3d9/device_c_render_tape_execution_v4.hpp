#pragma once

#include "device_c_chunk_replay.hpp"
#include "device_c_render_tape_controlled_cpu_v4.hpp"
#include "device_c_render_tape_semantic_mapping_v4.hpp"
#include "device_c_render_tape_source_plan_v4.hpp"
#include "device_c_render_tape_runtime_identity_v4.hpp"
#include "../dxmt9/dxmt9_command_queue.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace dxmt9 { class Device; }

namespace dxmt9::d3d9 {

namespace render_tape_runtime { struct GeneralPreflightPlan; }

enum class RenderTapeExecutionV4PublicationMode : std::uint8_t {
  ExactSourceDiagnostic,
  // Original publication actions register bounded obligations. Only final
  // correspondence with genuine candidate provenance discharges them.
  SemanticMapping,
};

// Explicit caller attestation about the production capture adapter, not
// authority inferred from a Present record or an expected publication row.
enum class RenderTapeExecutionV4ProducerContract : std::uint8_t {
  None,
  PePresentReservationV1,
};

enum class RenderTapeExecutionV4PresentPreparationPhase : std::uint8_t {
  Admitted, Preparing, Completed, Failed,
};

enum class RenderTapeExecutionV4PresentPreparationAction : std::uint8_t {
  Begin, Complete, Fail,
};

struct RenderTapeExecutionV4PresentPreparationState {
  RenderTapeExecutionV4ProducerContract contract = RenderTapeExecutionV4ProducerContract::None;
  RenderTapeDigest planDigest{};
  std::uint32_t eventIndex = kRenderTapeIdentityV4NoIndex;
  std::uint64_t eventOrdinal = 0u;
  RenderTapeExecutionV4PresentPreparationPhase phase =
      RenderTapeExecutionV4PresentPreparationPhase::Admitted;
  friend constexpr bool operator==(const RenderTapeExecutionV4PresentPreparationState&,
      const RenderTapeExecutionV4PresentPreparationState&) = default;
};

inline constexpr bool renderTapeV4PresentPreparationBound(
    const RenderTapeExecutionV4PresentPreparationState& state, const RenderTapeDigest& digest) noexcept {
  bool nonzeroDigest = false;
  for (const auto value : state.planDigest) nonzeroDigest |= value != std::byte{};
  return state.contract == RenderTapeExecutionV4ProducerContract::PePresentReservationV1 &&
      nonzeroDigest && state.planDigest == digest &&
      state.eventIndex != kRenderTapeIdentityV4NoIndex && state.eventOrdinal;
}

// Pure exactly-once grant. Failed or completed preparation can never reenter;
// rejected transitions leave the bound digest/event and phase unchanged.
inline constexpr bool renderTapeV4PresentPreparationAdvance(
    RenderTapeExecutionV4PresentPreparationState& state,
    RenderTapeExecutionV4PresentPreparationAction action, const RenderTapeDigest& digest,
    std::uint32_t eventIndex, std::uint64_t eventOrdinal) noexcept {
  if (!renderTapeV4PresentPreparationBound(state, digest) ||
      state.eventIndex != eventIndex || state.eventOrdinal != eventOrdinal) return false;
  using Phase = RenderTapeExecutionV4PresentPreparationPhase;
  using Action = RenderTapeExecutionV4PresentPreparationAction;
  if (action == Action::Begin && state.phase == Phase::Admitted) state.phase = Phase::Preparing;
  else if (action == Action::Complete && state.phase == Phase::Preparing) state.phase = Phase::Completed;
  else if (action == Action::Fail && state.phase == Phase::Preparing) state.phase = Phase::Failed;
  else return false;
  return true;
}

enum class RenderTapeExecutionV4PresentPreparationResult : std::uint8_t {
  Complete, InvalidBinding, PreparationFailed, CloseFailed,
};

// Cold orchestration shared with fake-boundary tests. The selected preparation
// runs while the previous raw's AfterEvent cut is still active, before its
// close/comparison callback. NONE invokes only the unchanged close callback.
template <typename Prepare, typename Close>
RenderTapeExecutionV4PresentPreparationResult renderTapeV4PresentPreparationBeforeClose(
    RenderTapeExecutionV4PresentPreparationState& state, const RenderTapeDigest& digest,
    std::uint32_t eventIndex, std::uint64_t eventOrdinal,
    Prepare&& prepare, Close&& close) noexcept {
  using Contract = RenderTapeExecutionV4ProducerContract;
  using Phase = RenderTapeExecutionV4PresentPreparationPhase;
  using Action = RenderTapeExecutionV4PresentPreparationAction;
  using Result = RenderTapeExecutionV4PresentPreparationResult;
  if (state.contract == Contract::None) return close() ? Result::Complete : Result::CloseFailed;
  if (!renderTapeV4PresentPreparationBound(state, digest)) return Result::InvalidBinding;
  if (eventIndex != state.eventIndex || eventOrdinal != state.eventOrdinal) {
    const bool before = state.phase == Phase::Admitted &&
        eventIndex < state.eventIndex && eventOrdinal < state.eventOrdinal;
    const bool after = state.phase == Phase::Completed &&
        eventIndex > state.eventIndex && eventOrdinal > state.eventOrdinal;
    return before || after ? (close() ? Result::Complete : Result::CloseFailed) : Result::InvalidBinding;
  }
  if (!renderTapeV4PresentPreparationAdvance(state, Action::Begin, digest, eventIndex, eventOrdinal))
    return Result::InvalidBinding;
  if (!prepare()) {
    (void)renderTapeV4PresentPreparationAdvance(state, Action::Fail, digest, eventIndex, eventOrdinal);
    return Result::PreparationFailed;
  }
  if (!renderTapeV4PresentPreparationAdvance(state, Action::Complete, digest, eventIndex, eventOrdinal))
    return Result::InvalidBinding;
  return close() ? Result::Complete : Result::CloseFailed;
}

// Provider setup completion is confined to the one admitted pre-arm boundary.
// No captured interval event authorizes this setup publication/completion step.
inline constexpr bool renderTapeV4SetupCheckpointRequired(bool exactDiagnosticV4,
    std::uint32_t eventIndex, std::uint32_t snapshotPrefixEvents) noexcept {
  return exactDiagnosticV4 && snapshotPrefixEvents != kRenderTapeIdentityV4NoIndex &&
      eventIndex == snapshotPrefixEvents;
}

struct RenderTapeExecutionV4SourceMapping {
  std::uint32_t capturedIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t localIndex = kRenderTapeIdentityV4NoIndex;
  std::uint64_t localSequence = 0u;
  std::uint64_t localPublicationEdge = 0u;
};

struct RenderTapeExecutionV4InitializerMapping {
  std::uint32_t capturedIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t localIndex = kRenderTapeIdentityV4NoIndex;
  std::uint64_t localSequence = 0u;
  std::uint64_t localPublicationEdge = 0u;
};

struct RenderTapeExecutionV4CoordinatorMapping {
  std::uint32_t capturedIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t localIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t localSourceIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t localInitializerIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t localCommandEnd = 0u;
  std::uint64_t localPrefixSequence = 0u;
  std::uint64_t localPrefixInitializer = 0u;
};

struct RenderTapeExecutionV4QueryBaselineInput {
  D9CWireObjectIdentity capturedIdentity{};
  D9CQuery* query = nullptr; // Borrowed only by synchronous arm.
  core::QueryPollSnapshot snapshot{};
};

struct RenderTapeExecutionV4Evidence {
  RenderTapeExecutionV4PublicationMode publicationMode =
      RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic;
  RenderTapeExecutionV4ProducerContract producerContract = RenderTapeExecutionV4ProducerContract::None;
  bool presentPreparationAttempted = false;
  bool presentPreparationCompleted = false;
  std::int32_t presentPreparationHresult = 0;
  std::uint32_t presentPreparationEventIndex = kRenderTapeIdentityV4NoIndex;
  bool requested = false;
  bool validated = false;
  bool armed = false;
  bool canonicalTraversalComplete = false;
  bool rawAckCoverageComplete = false;
  bool actualAttemptsObserved = false;
  bool actualLifecycleComplete = false;
  bool actualTopologyComplete = false;
  bool exactSourceDiagnosticComplete = false;
  bool semanticMappingComplete = false;
  bool complete = false;
  // Source diagnostics and semantic correspondence do not qualify a paired
  // policy experiment. These flags are never promoted by admission or finish.
  bool qualifierOnly = true;
  bool policyQualified = false;
  bool queryReadinessQualified = false;
  bool pixelsQualified = false;
  bool timingEligible = false;
  std::uint32_t admissionCount = 0u;
  bool preparedAdmissionConsumed = false;
  RenderTapeDigest planDigest{};
  std::optional<CommandQueue::RenderTapeArmState> preArm{};
  bool localArmAttempted = false;
  std::int32_t localArmHresult = 0;
  D9CRenderTapeIdentityCaptureV4ArmResult localArm{};
  D9CRenderTapeIdentityCaptureV4Result localFinish{};
  std::size_t replayedEvents = 0u;
  std::size_t dispatchedRecords = 0u; // Actual committed coverage, not retries.
  std::size_t acknowledgedRaws = 0u;
  std::size_t queryBaselineExpected = 0u;
  std::size_t queryBaselinesJoined = 0u;
  std::size_t queryObjectsMapped = 0u;
  std::size_t sourceObligationsExpected = 0u;
  std::size_t sourceObligationsRegistered = 0u;
  std::size_t sourceObligationsPending = 0u;
  std::size_t sourceObligationsQualified = 0u;
  RenderTapeSemanticMappingV4Status semanticMappingStatus =
      RenderTapeSemanticMappingV4Status::InvalidPlan;
  std::shared_ptr<const RenderTapeSemanticMappingV4> semanticMapping{};
  RenderTapeIdentityV4View actualIdentity{};
  RenderTapeExecutionTopologyV1View actualTopology{};
  // Exact-source mode alone owns these one-to-one receipt mappings. Semantic
  // mode uses semanticMapping's canonical origins and many-to-many relations.
  std::vector<RenderTapeExecutionV4SourceMapping> sources{};
  std::vector<RenderTapeExecutionV4InitializerMapping> initializers{};
  std::vector<RenderTapeExecutionV4CoordinatorMapping> coordinators{};
};

// A prepared replay consumes one already-authenticated, still-pristine local
// admission. Identity comparisons bind its caller-owned evidence and inputs;
// this predicate grants no dispatch, publication, query or completion evidence.
struct RenderTapePreparedAdmissionV4Facts {
  bool admitted = false;
  bool failed = false;
  bool consumed = false;
  bool pristine = false;
  bool samePlanOwner = false;
  bool sameGeneralOwner = false;
  bool sameEvidenceOwner = false;
  bool sameTapeStorage = false;
  bool samePublicationMode = false;
  bool sameProducerContract = false;
};

inline constexpr bool renderTapePreparedAdmissionV4ClaimAllowed(
    const RenderTapePreparedAdmissionV4Facts& facts) noexcept {
  return facts.admitted && !facts.failed && !facts.consumed && facts.pristine &&
      facts.samePlanOwner && facts.sameGeneralOwner && facts.sameEvidenceOwner &&
      facts.sameTapeStorage && facts.samePublicationMode && facts.sameProducerContract;
}

// Stationary, input-only authority. Preparation authenticates all templates
// transactionally; an initialized executor borrows their immutable vectors.
// The provider owns the exact input in its stable PIMPL. Standalone admission
// retains the existing caller-input lifetime requirement.
class RenderTapeExecutionV4Preparation {
 public:
  RenderTapeExecutionV4Preparation() = default;
  RenderTapeExecutionV4Preparation(const RenderTapeExecutionV4Preparation&) = delete;
  RenderTapeExecutionV4Preparation& operator=(const RenderTapeExecutionV4Preparation&) = delete;
  RenderTapeExecutionV4Preparation(RenderTapeExecutionV4Preparation&&) = delete;
  RenderTapeExecutionV4Preparation& operator=(RenderTapeExecutionV4Preparation&&) = delete;

  bool valid() const noexcept { return valid_; }
  std::uint32_t snapshotPrefixEvents() const noexcept { return snapshotPrefixEvents_; }

 private:
  friend class RenderTapeExecutionV4;
  friend struct RenderTapeExecutionV4PreparedReplayAccess;
  struct QueryTemplate {
    D9CWireObjectIdentity capturedIdentity{};
    RenderTapeQueryDescriptorDecoded descriptor{};
    std::uint32_t definitionEvent = kRenderTapeIdentityV4NoIndex;
  };
  const CaptureSourceReplayPlanV4* plan_ = nullptr;
  const render_tape_runtime::GeneralPreflightPlan* replay_ = nullptr;
  std::span<const std::byte> tape_{};
  RenderTapeDigest digest_{};
  RenderTapeExecutionV4PublicationMode publicationMode_ =
      RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic;
  RenderTapeExecutionV4ProducerContract producerContract_ =
      RenderTapeExecutionV4ProducerContract::None;
  std::vector<RenderTapeIdentityV3EventFact> eventFacts_{};
  std::vector<RenderTapeIdentityV3RecordFact> localRecordSeeds_{};
  std::vector<QueryTemplate> queryTemplates_{};
  std::vector<RenderTapeDigest> commandDigests_{};
  std::vector<CommandChunkEnvelope> commandEnvelopes_{};
  std::vector<std::vector<std::uint32_t>> recordTypes_{};
  std::vector<CaptureSourceReplayPlanV4Action> sourceObligations_{};
  RenderTapeExecutionV4PresentPreparationState presentPreparation_{};
  std::size_t queryBaselineExpected_ = 0u;
  std::uint32_t snapshotPrefixEvents_ = 0u;
  bool valid_ = false;
};

// Cold, synchronous execution qualification. The admitted owner and evidence
// must remain alive/immutable through the session; command bytes and resolved
// wrappers remain alive/immutable through their synchronous replay call. The
// caller exclusively owns device arm and
// reset for the session. Concurrent public replacement, including same-key
// replacement, is unsupported. Every boundary checks the retained actual root.
// No method selects Compatibility, replays a captured failed attempt or forces
// an expected publication. Only the explicit producer contract grants the
// shared production pre-Present preparation; NONE performs no such work.
class RenderTapeExecutionV4 {
public:
  RenderTapeExecutionV4() = default;
  ~RenderTapeExecutionV4();
  RenderTapeExecutionV4(const RenderTapeExecutionV4&) = delete;
  RenderTapeExecutionV4& operator=(const RenderTapeExecutionV4&) = delete;

  bool admit(const CaptureSourceReplayPlanV4&,
      const render_tape_runtime::GeneralPreflightPlan&,
      std::span<const std::byte> exactTape, RenderTapeExecutionV4Evidence&,
      RenderTapeExecutionV4PublicationMode =
          RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic,
      RenderTapeExecutionV4ProducerContract = RenderTapeExecutionV4ProducerContract::None) noexcept;
  // All snapshot-prefix events and their promised initial seeds must have
  // actually completed before arm. The real queue admits the local baseline.
  bool arm(D9CDevice*, std::span<const RenderTapeExecutionV4QueryBaselineInput> = {}) noexcept;
  bool armControlledCpu(D9CDevice*, const RenderTapeControlledCpuV4Capability&,
      std::span<const RenderTapeExecutionV4QueryBaselineInput> = {}) noexcept;
  // Exact ObjectDefine acquisition/activation. Binds a fresh local wrapper,
  // or verifies the already-seeded hidden checkpoint owner exactly once.
  bool bindQueryObject(D9CDevice*, D9CWireObjectIdentity captured, D9CQuery*) noexcept;
  bool beginEvent(std::uint32_t eventIndex) noexcept;
  bool replayedEvent(std::uint32_t eventIndex) noexcept; // Non-command events only.
  bool bindCommandChunk(std::span<const std::byte>, const CommandChunkEnvelope&,
      std::span<void* const> objects) noexcept;
  ProviderReplayObservationV4 observation() noexcept;
  // finish requires real lifecycle/topology completion, never submits a flush.
  bool finish() noexcept;
  bool finishControlledCpu(const RenderTapeControlledCpuV4Capability&,
      RenderTapeControlledCpuV4Evidence&) noexcept;
  // Outside-lock pull of genuinely observed publications. On success the
  // caller-owned cursor advances once; no captured source index is used.
  bool publishedSources(std::size_t& cursor,
      std::vector<core::metalqueue::ReplaySourcePublicationReceipt>&) noexcept;
  struct WritingContinuation {
    query::WriterIdentity writer{};
    core::CpuReadyTape::SourceRef source{};
    std::size_t commandCount = 0u;
  };
  bool ownsWritingContinuation(const WritingContinuation&) noexcept;
  void cancel() noexcept;
  bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }
  const char* failure() const noexcept { return failure_.load(std::memory_order_acquire); }
  std::uint32_t snapshotPrefixEvents() const noexcept { return snapshotPrefixEvents_; }

private:
  friend struct RenderTapeExecutionV4TestAccess;
  friend struct RenderTapeExecutionV4PreparedReplayAccess;
  static bool prepareTemplates(RenderTapeExecutionV4Preparation&,
      const CaptureSourceReplayPlanV4&, const render_tape_runtime::GeneralPreflightPlan&,
      std::span<const std::byte>, RenderTapeExecutionV4PublicationMode,
      RenderTapeExecutionV4ProducerContract, const char*&) noexcept;
  bool initializePrepared(const RenderTapeExecutionV4Preparation&,
      const CaptureSourceReplayPlanV4&, const render_tape_runtime::GeneralPreflightPlan&,
      std::span<const std::byte>, RenderTapeExecutionV4Evidence&,
      RenderTapeExecutionV4PublicationMode, RenderTapeExecutionV4ProducerContract) noexcept;
  bool consumePreparedReplayAdmission(const CaptureSourceReplayPlanV4&,
      const render_tape_runtime::GeneralPreflightPlan&, std::span<const std::byte>,
      RenderTapeExecutionV4Evidence&, RenderTapeExecutionV4PublicationMode,
      RenderTapeExecutionV4ProducerContract) noexcept;
  bool reject(const char*) noexcept;
  bool inputBound() noexcept;
  bool rootBound(bool allowFrozen = false) noexcept;
  bool rootBoundActual(bool allowFrozen) noexcept;
  bool preparePresentReservation() noexcept;
  bool armActual(D9CDevice*, std::span<const RenderTapeExecutionV4QueryBaselineInput>,
      const RenderTapeControlledCpuV4Capability*) noexcept;
  bool finishTraversal() noexcept;
  bool snapshotActual() noexcept;
  bool closeEvent() noexcept;
  bool qualifyPublication(const CaptureSourceReplayPlanV4Action&) noexcept;
  bool registerSemanticPublication(const CaptureSourceReplayPlanV4Action&) noexcept;
  bool rawStarted(std::uint64_t event, std::uint64_t raw, std::uint32_t count) noexcept;
  bool beforeRecord(std::uint32_t index, std::uint32_t type) noexcept;
  bool beforeDispatch(std::uint32_t index, std::uint32_t type) noexcept;
  bool recordDispatched(std::uint32_t index, std::uint32_t type, std::int32_t result) noexcept;
  bool rawSettled(std::uint64_t raw, std::uint32_t count, bool succeeded) noexcept;
  bool recoverRolledBackRecord(std::uint32_t index) noexcept;
  bool qualifyRawCoverage() noexcept;
  bool qualifyExactSources() noexcept;
  bool qualifySemanticMapping() noexcept;

  struct QueryBinding {
    D9CWireObjectIdentity capturedIdentity{}, localIdentity{};
    RenderTapeQueryDescriptorDecoded descriptor{};
    std::uint64_t coreIdentity = 0u, deviceEpoch = 0u;
    std::uint32_t definitionEvent = kRenderTapeIdentityV4NoIndex;
    bool bound = false, activated = false;
  };
  bool bindLocalQuery(QueryBinding&, D9CDevice*, D9CQuery*,
      const core::QueryPollSnapshot&) noexcept;

  RenderTapeExecutionV4Preparation ownedPreparation_{};
  const RenderTapeExecutionV4Preparation* preparation_ = nullptr;
  const CaptureSourceReplayPlanV4* plan_ = nullptr;
  // Retained only as the identity of the same synchronous prepared call.
  // The caller keeps these inputs and evidence alive until cancellation.
  const render_tape_runtime::GeneralPreflightPlan* admittedReplay_ = nullptr;
  std::span<const std::byte> admittedTape_{};
  RenderTapeExecutionV4Evidence* evidence_ = nullptr;
  CaptureSourceReplayCursorV4 cursor_{};
  RenderTapeDigest digest_{};
  RenderTapeExecutionV4PublicationMode publicationMode_ =
      RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic;
  RenderTapeExecutionV4ProducerContract producerContract_ =
      RenderTapeExecutionV4ProducerContract::None;
  bool preparedAdmissionConsumed_ = false;
  bool preparedAdmissionCancelled_ = false;
  RenderTapeExecutionV4PresentPreparationState presentPreparation_{};
  D9CDevice* device_ = nullptr;
  std::shared_ptr<dxmt9::Device> backend_{};
  std::shared_ptr<RenderTapeRuntimeIdentityV4> root_{};
  RenderTapeIdentityV4CaptureKey localKey_{};
  std::uint64_t localQueue_ = 0u;
  bool controlledCpu_ = false;
  RenderTapeControlledCpuV4Lease controlledCpuLease_{};
  ImportedChunkView chunk_{};
  std::span<const RenderTapeIdentityV3EventFact> eventFacts_{};
  std::vector<RenderTapeIdentityV3RecordFact> localRecordFacts_{};
  std::vector<QueryBinding> queryBindings_{};
  std::span<const RenderTapeDigest> commandDigests_{};
  std::span<const CommandChunkEnvelope> commandEnvelopes_{};
  std::span<const std::vector<std::uint32_t>> recordTypes_{};
  // Admitted actions are owned before effects. Registration exposes only the
  // exactly-once prefix; neither storage nor cursor ACK is a publication.
  std::span<const CaptureSourceReplayPlanV4Action> sourceObligations_{};
  std::size_t registeredSourceObligations_ = 0u;
  std::uint32_t event_ = kRenderTapeIdentityV4NoIndex;
  std::uint32_t nextEvent_ = 0u;
  std::uint32_t snapshotPrefixEvents_ = 0u;
  std::uint32_t callbackNextRecord_ = 0u;
  std::uint32_t callbackRecord_ = kRenderTapeIdentityV4NoIndex;
  std::uint32_t expectedSources_ = 0u, expectedInitializers_ = 0u;
  std::uint64_t localRaw_ = 0u, lastRollbackGeneration_ = 0u;
  std::uint64_t lastPublicationEdge_ = 0u;
  bool armed_ = false, eventReplayed_ = false, chunkBound_ = false;
  bool callbackRecordOpen_ = false, callbackDispatchStarted_ = false;
  std::atomic<bool> failed_{false};
  std::atomic<const char*> failure_{"none"};
};

// Preparation authenticates transactionally. Initialization and consumption
// only check exact immutable ownership; they never replace evidence pointers,
// authenticate again, reset an executor or reuse it across invocations.
struct RenderTapeExecutionV4PreparedReplayAccess {
  // Failure leaves a fresh destination invalid; a published preparation
  // cannot be replaced, including while executors borrow its storage.
  static bool prepare(RenderTapeExecutionV4Preparation&, const CaptureSourceReplayPlanV4&,
      const render_tape_runtime::GeneralPreflightPlan&, std::span<const std::byte>,
      RenderTapeExecutionV4PublicationMode, RenderTapeExecutionV4ProducerContract,
      const char*& failure) noexcept;
  static bool initialize(RenderTapeExecutionV4&, const RenderTapeExecutionV4Preparation&,
      const CaptureSourceReplayPlanV4&, const render_tape_runtime::GeneralPreflightPlan&,
      std::span<const std::byte>, RenderTapeExecutionV4Evidence&,
      RenderTapeExecutionV4PublicationMode, RenderTapeExecutionV4ProducerContract) noexcept;
  static bool consume(RenderTapeExecutionV4&, const CaptureSourceReplayPlanV4&,
      const render_tape_runtime::GeneralPreflightPlan&, std::span<const std::byte>,
      RenderTapeExecutionV4Evidence&, RenderTapeExecutionV4PublicationMode,
      RenderTapeExecutionV4ProducerContract) noexcept;
};

} // namespace dxmt9::d3d9
