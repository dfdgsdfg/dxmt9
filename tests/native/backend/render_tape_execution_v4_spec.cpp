#include "device_c_render_tape_execution_v4.hpp"
#include "device_c_common.hpp"
#include "device_c_render_tape_present_preparation.hpp"
#include "device_c_render_tape_runtime_binding_v4.hpp"
#include "device_c_render_tape_runtime_objects.hpp"
#include "device_c_render_tape_runtime_replay_v4.hpp"
#include "device_c_replay_projection.hpp"
#include "render_tape_cpu_destroy_fixture.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9/dxmt9_resource_initializer.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dxmt9 {

struct CommandQueueArenaLeaseTestAccess {
  struct Isolation {
    CommandQueue* queue = nullptr;
    bool allowed = true;
    std::size_t probes = 0u;
  };

  static d3d9::RenderTapeControlledCpuV4Capability mint(D9CDevice* device,
      std::shared_ptr<Device> backend, std::weak_ptr<void> lifetime, Isolation& isolation) {
    return d3d9::RenderTapeControlledCpuV4Capability::mint(device, std::move(backend),
        std::move(lifetime), &isolation,
        [](void* opaque, D9CDevice*, const Device&, d3d9::RenderTapeControlledCpuV4Boundary) noexcept {
          auto& state = *static_cast<Isolation*>(opaque);
          ++state.probes;
          if (!state.queue || !state.allowed) return false;
          std::lock_guard lock(state.queue->mutex_);
          return !state.queue->stop_ && !state.queue->encodeThread_.joinable() &&
              !state.queue->finishThread_.joinable() && !state.queue->completionThread_.joinable();
        });
  }
};

struct SchedulingProgressTestAccess {
  static void installPendingInitializer(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    queue.initializer_ = std::make_unique<resources::Initializer>(queue, queue.pool_,
        WMT::Reference<WMT::Device>{});
    // Fault fixture supplies only the pending predicate. These placeholder
    // bytes must never reach publication, encoding or a completion wait.
    queue.initializer_->pendingUploads_.emplace_back();
  }

  static void installFailedInitializer(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    queue.initializer_ = std::make_unique<resources::Initializer>(queue, queue.pool_,
        WMT::Reference<WMT::Device>{});
    queue.initializer_->queryTrackingEnabled_ = true;
    queue.initializer_->queryUploads_.fail();
  }
};

} // namespace dxmt9

namespace dxmt9::d3d9 {

// A cold authored comparison fixture never replaces the genuine observed
// evidence. This seam calls the production projection validator/comparator;
// it does not arm, submit, mutate actual rows, or promote completion flags.
struct RenderTapeExecutionV4TestAccess {
  static const RenderTapeExecutionV4Evidence* boundEvidence(
      const RenderTapeExecutionV4& execution) {
    return execution.evidence_;
  }

  static bool sharedImmutableTables(const RenderTapeExecutionV4& a,
      const RenderTapeExecutionV4& b) {
    return !a.eventFacts_.empty() && !a.commandDigests_.empty() &&
        a.eventFacts_.data() == b.eventFacts_.data() &&
        a.commandDigests_.data() == b.commandDigests_.data() &&
        a.commandEnvelopes_.data() == b.commandEnvelopes_.data() &&
        a.recordTypes_.data() == b.recordTypes_.data() &&
        a.sourceObligations_.data() == b.sourceObligations_.data();
  }

  static bool sameNeutralTables(const RenderTapeExecutionV4& a,
      const RenderTapeExecutionV4& b) {
    return std::equal(a.eventFacts_.begin(), a.eventFacts_.end(),
        b.eventFacts_.begin(), b.eventFacts_.end(), [](const auto& x, const auto& y) {
          return x.ordinal == y.ordinal && x.type == y.type && x.recordCount == y.recordCount;
        }) && std::equal(a.commandDigests_.begin(), a.commandDigests_.end(),
            b.commandDigests_.begin(), b.commandDigests_.end()) &&
        std::equal(a.commandEnvelopes_.begin(), a.commandEnvelopes_.end(),
            b.commandEnvelopes_.begin(), b.commandEnvelopes_.end(), [](const auto& x, const auto& y) {
              return x.version == y.version && x.recordCount == y.recordCount &&
                  x.handleCount == y.handleCount &&
                  x.producerIdentity.firstEventOrdinal == y.producerIdentity.firstEventOrdinal &&
                  x.producerIdentity.lastEventOrdinal == y.producerIdentity.lastEventOrdinal &&
                  x.producerIdentity.firstSourceOrdinal == y.producerIdentity.firstSourceOrdinal &&
                  x.producerIdentity.lastSourceOrdinal == y.producerIdentity.lastSourceOrdinal;
            }) && std::equal(a.recordTypes_.begin(), a.recordTypes_.end(),
            b.recordTypes_.begin(), b.recordTypes_.end()) &&
        std::equal(a.sourceObligations_.begin(), a.sourceObligations_.end(),
            b.sourceObligations_.begin(), b.sourceObligations_.end());
  }

  static bool freshMutableState(const RenderTapeExecutionV4& execution) {
    return !execution.armed_ && !execution.device_ && !execution.backend_ && !execution.root_ &&
        !execution.localKey_.token && !execution.localKey_.epoch && !execution.localQueue_ &&
        execution.event_ == kRenderTapeIdentityV4NoIndex && execution.nextEvent_ == 0u &&
        execution.cursor_.acknowledged() == 0u &&
        execution.cursor_.status() == CaptureSourceReplayCursorV4Status::Ready &&
        execution.presentPreparation_.phase == RenderTapeExecutionV4PresentPreparationPhase::Admitted &&
        execution.registeredSourceObligations_ == 0u && !execution.chunkBound_ &&
        !execution.localRaw_ && !execution.failed() &&
        std::all_of(execution.queryBindings_.begin(), execution.queryBindings_.end(), [](const auto& binding) {
          return !binding.bound && !binding.activated && !binding.localIdentity.objectId &&
              !binding.coreIdentity && !binding.deviceEpoch;
        });
  }

  static bool disjointMutableStorage(const RenderTapeExecutionV4& a,
      const RenderTapeExecutionV4& b) {
    return !a.localRecordFacts_.empty() && !a.queryBindings_.empty() &&
        a.localRecordFacts_.data() != b.localRecordFacts_.data() &&
        a.queryBindings_.data() != b.queryBindings_.data();
  }

  static const RenderTapeIdentityV3RecordFact* localQueryFact(const RenderTapeExecutionV4& execution) {
    const auto found = std::find_if(execution.localRecordFacts_.begin(), execution.localRecordFacts_.end(),
        [](const auto& fact) { return fact.recordType == D9C_COMMAND_RECORD_QUERY_ISSUE; });
    return found == execution.localRecordFacts_.end() ? nullptr : &*found;
  }

  // Pure phase binding only: these callbacks perform no queue/Present work
  // and never write public actual-preparation evidence.
  static bool completePresentPhaseFixture(RenderTapeExecutionV4& execution) {
    auto& phase = execution.presentPreparation_;
    bool prepared = false, closed = false;
    const auto result = renderTapeV4PresentPreparationBeforeClose(phase, execution.digest_,
        phase.eventIndex, phase.eventOrdinal,
        [&]() noexcept { prepared = true; return true; },
        [&]() noexcept { closed = true; return true; });
    return result == RenderTapeExecutionV4PresentPreparationResult::Complete && prepared && closed &&
        phase.phase == RenderTapeExecutionV4PresentPreparationPhase::Completed;
  }

  static RenderTapeIdentityV4Status validateLocalRows(const RenderTapeExecutionV4& execution,
      const RenderTapeIdentityV4View& fixture) {
    return validateRenderTapeIdentityV4Rows(
        fixture, execution.eventFacts_, execution.localRecordFacts_, true);
  }

  static bool qualifyExactFixture(RenderTapeExecutionV4& execution,
      RenderTapeExecutionV4Evidence& fixture) {
    auto* observed = execution.evidence_;
    const auto sources = execution.expectedSources_, initializers = execution.expectedInitializers_;
    execution.evidence_ = &fixture;
    execution.expectedSources_ = static_cast<std::uint32_t>(execution.plan_->identity().sources.size());
    execution.expectedInitializers_ = static_cast<std::uint32_t>(execution.plan_->identity().initializers.size());
    const auto qualified = execution.qualifyExactSources();
    execution.evidence_ = observed;
    execution.expectedSources_ = sources;
    execution.expectedInitializers_ = initializers;
    return qualified;
  }

  static bool registerSemanticFixture(RenderTapeExecutionV4& execution,
      const CaptureSourceReplayPlanV4Action& action) {
    return execution.registerSemanticPublication(action);
  }

  static bool traverseSemanticFixture(RenderTapeExecutionV4& execution) {
    CaptureSourceReplayPlanV4Action action;
    while (execution.cursor_.peek(*execution.plan_, action)) {
      if (action.kind == CaptureSourceReplayPlanV4ActionKind::ExpectSourcePublication &&
          !execution.registerSemanticPublication(action)) return false;
      if (!execution.cursor_.acknowledge(*execution.plan_, action)) return false;
    }
    return execution.cursor_.status() == CaptureSourceReplayCursorV4Status::Complete;
  }

  static bool qualifySemanticFixture(RenderTapeExecutionV4& execution,
      RenderTapeExecutionV4Evidence& fixture) {
    auto* observed = execution.evidence_;
    execution.evidence_ = &fixture;
    const auto qualified = execution.qualifySemanticMapping();
    execution.evidence_ = observed;
    return qualified;
  }
};

} // namespace dxmt9::d3d9

namespace {
using namespace dxmt9;
using namespace dxmt9::d3d9;
using render_tape_runtime::GeneralPreflightPlan;
using Outcome = RenderTapeIdentityV4AttemptOutcome;
using ProducerContract = RenderTapeExecutionV4ProducerContract;
using PreparationPhase = RenderTapeExecutionV4PresentPreparationPhase;
using PreparationAction = RenderTapeExecutionV4PresentPreparationAction;
using PreparationResult = RenderTapeExecutionV4PresentPreparationResult;
using PreparationState = RenderTapeExecutionV4PresentPreparationState;
std::uint64_t checks = 0u;

void check(bool value, std::string_view message) {
  ++checks;
  if (!value) throw std::runtime_error(std::string(message));
}

std::vector<std::byte> stateChunk() {
  using test::general_cpu_destroy_fixture_detail::Record;
  const std::array first{1.0f, 2.0f, 3.0f, 4.0f};
  const std::array second{5.0f, 6.0f, 7.0f, 8.0f};
  const D9CCommandChunkWireSetConst fixed{.registerCount = 1u};
  std::array<Record, 2u> records;
  for (std::size_t index = 0u; index < records.size(); ++index) {
    auto& record = records[index];
    record.type = D9C_COMMAND_RECORD_SET_VS_CONST_F;
    record.payload.resize(sizeof(fixed) + sizeof(first));
    std::memcpy(record.payload.data(), &fixed, sizeof(fixed));
    std::memcpy(record.payload.data() + sizeof(fixed),
        index ? second.data() : first.data(), sizeof(first));
  }
  auto bytes = test::general_cpu_destroy_fixture_detail::makeChunk(records);
  ImportedChunkView imported;
  check(importPrevalidatedCommandChunk(bytes, {.recordCount = 2u}, imported) &&
      imported.records.size() == 2u && imported.handles.empty(),
      "genuine two-record handle-free state chunk passes strict production admission");
  return bytes;
}

struct AdmissionFixture {
  std::vector<std::byte> tape{}, identityBytes{}, topologyBytes{};
  std::vector<std::vector<std::byte>> blobs{};
  GeneralPreflightPlan replay{};
  CaptureSourceReplayPlanV4 plan{};
  std::uint32_t prefix = 0u, stateEvent = 0u;
  std::uint32_t prePresentStateEvent = kRenderTapeIdentityV4NoIndex;
  std::uint32_t queryDefinitionEvent = kRenderTapeIdentityV4NoIndex;
  std::uint32_t queryIssueEvent = kRenderTapeIdentityV4NoIndex;
  D9CWireObjectIdentity queryIdentity{D9C_CHUNK_HANDLE_KIND_QUERY, 77u, 901u};
};

enum class QueryFixture { None, CheckpointPrefix, CheckpointLazy, Fresh, ClosedHistory, ClosedOcclusion, ClosedUnsupported,
  LateFreshMissingPrefix };

// General admission needs genuine indexed draws, shader/resource closure and
// one Present. Build V4 provenance and topology directly from that Tape; no
// V3 sidecar conversion or fake General parser is used.
AdmissionFixture makeFixture(bool capturedRollback = false, bool seeded = false,
    bool orderedControl = false, bool missingInitializer = false, bool sessionDomain = false,
    QueryFixture queryShape = QueryFixture::None, bool producerPresentRaw = false,
    std::uint32_t extraDrawSources = 0u) {
  AdmissionFixture out;
  auto original = test::makeGeneralCpuDestroyFixture(false, false,
      test::GeneralCpuMutationFixture::None, test::GeneralCpuResetFixture::A, extraDrawSources);
  ImportedRenderTapeView imported;
  check(importPrevalidatedRenderTape(original.tape, imported), "original strict Tape imports");
  if (seeded) out.blobs = std::move(original.blobs);
  else out.blobs.assign(original.blobs.begin() + 2u, original.blobs.end());
  RenderTapeBuilder builder;
  std::uint32_t eventIndex = 0u;
  std::uint64_t present = 0u;
  bool inserted = false;
  const bool queryCheckpoint = queryShape == QueryFixture::CheckpointPrefix ||
      queryShape == QueryFixture::CheckpointLazy;
  const bool explicitHistory = queryShape == QueryFixture::ClosedHistory ||
      queryShape == QueryFixture::ClosedOcclusion;
  const std::uint32_t queryType = queryShape == QueryFixture::ClosedOcclusion ? 9u : 8u;
  const std::uint32_t queryFlags = queryShape == QueryFixture::ClosedOcclusion ? 2u : 1u;
  const auto appendQueryDefinition = [&] {
    out.queryDefinitionEvent = eventIndex++;
    if (queryCheckpoint) {
      const RenderTapeQueryDescriptorV3 descriptor{.initialIssueCycle = 31u,
          .checkpoint = {.checkpointVersion = D9C_RENDER_TAPE_EVENT_QUERY_CHECKPOINT_VERSION_1,
              .status = D9C_RENDER_TAPE_EVENT_QUERY_CHECKPOINT_COMPLETE, .queryType = 8u,
              .identity = out.queryIdentity, .issueCycle = 31u, .coreQueryIdentity = 902u,
              .deviceEpoch = 903u, .issueOrdinal = 47u, .rawOrdinal = 904u,
              .queueIdentity = 905u}};
      builder.appendObjectDefine(out.queryIdentity,
          static_cast<std::uint32_t>(RenderTapeDescriptorKind::Query),
          std::as_bytes(std::span(&descriptor, 1u)), 0u, {});
    } else if (explicitHistory) {
      const RenderTapeQueryDescriptorV4 descriptor{.type = queryType, .dataBytes = 4u,
          .initialState = static_cast<std::uint32_t>(RenderTapeQueryInitialState::ClosedWithoutReplayResult),
          .initialIssueCycle = 508u, .initialIssueOrdinal = 1016u};
      builder.appendObjectDefine(out.queryIdentity,
          static_cast<std::uint32_t>(RenderTapeDescriptorKind::Query),
          std::as_bytes(std::span(&descriptor, 1u)), 0u, {});
    } else {
      const bool closed = queryShape == QueryFixture::ClosedUnsupported;
      const RenderTapeQueryDescriptorV2 descriptor{.type = 8u, .dataBytes = 4u,
          .initialState = static_cast<std::uint32_t>(closed
              ? RenderTapeQueryInitialState::ClosedWithoutReplayResult
              : RenderTapeQueryInitialState::Unissued), .initialIssueCycle = closed ? 31u : 0u};
      builder.appendObjectDefine(out.queryIdentity,
          static_cast<std::uint32_t>(RenderTapeDescriptorKind::Query),
          std::as_bytes(std::span(&descriptor, 1u)), 0u, {});
    }
  };
  const auto appendQueryIssue = [&] {
    using namespace test::general_cpu_destroy_fixture_detail;
    const std::array records{Record{.type = D9C_COMMAND_RECORD_QUERY_ISSUE,
        .payload = bytesOf(D9CCommandChunkWireQueryIssue{.queryHandleIndex = 0u, .flags = queryFlags}),
        .handles = {{out.queryIdentity.kind, out.queryIdentity.generation, out.queryIdentity.objectId}}}};
    out.queryIssueEvent = eventIndex++;
    builder.appendCommandChunk({.recordCount = 1u, .handleCount = 1u}, makeChunk(records));
  };
  for (std::size_t index = 0u; index < imported.events.size(); ++index) {
    const auto event = imported.event(index);
    const auto type = static_cast<RenderTapeEventType>(event.header.type);
    if (type == RenderTapeEventType::ResourceMutation && !seeded) continue;
    if (type == RenderTapeEventType::CommandChunk && !inserted) {
      if (queryShape == QueryFixture::CheckpointPrefix) appendQueryDefinition();
      out.prefix = eventIndex;
      if (orderedControl) {
        const std::vector<std::byte> gamma(kRenderTapeGammaRampBytes);
        builder.appendOrderedControl({.kind = static_cast<std::uint32_t>(RenderTapeControlKind::GammaRampSet),
            .disposition = static_cast<std::uint32_t>(RenderTapeControlDisposition::Completed),
            .controlBytes = kRenderTapeGammaRampBytes}, gamma);
        ++eventIndex;
      }
      out.stateEvent = eventIndex;
      builder.appendCommandChunk({.recordCount = 2u}, stateChunk());
      ++eventIndex;
      if (queryShape != QueryFixture::None && queryShape != QueryFixture::LateFreshMissingPrefix) {
        if (queryShape != QueryFixture::CheckpointPrefix) appendQueryDefinition();
        appendQueryIssue();
      }
      inserted = true;
    }
    if (type == RenderTapeEventType::ObjectDefine && !seeded) {
      std::vector<std::byte> payload(event.payload.begin(), event.payload.end());
      RenderTapeObjectDefineHeader definition{};
      std::memcpy(&definition, payload.data(), sizeof(definition));
      if (definition.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
        definition.expectedContentBytes = 0u;
        definition.expectedContentCount = 0u;
        std::memcpy(payload.data(), &definition, sizeof(definition));
      }
      builder.appendRawEvent(type, payload);
    } else if (type == RenderTapeEventType::PresentComplete) {
      std::vector<std::byte> payload(event.payload.begin(), event.payload.end());
      RenderTapePresentCompleteHeader completion{};
      std::memcpy(&completion, payload.data(), sizeof(completion));
      completion.presentOrdinal = present;
      std::memcpy(payload.data(), &completion, sizeof(completion));
      builder.appendRawEvent(type, payload);
    } else {
      if (type == RenderTapeEventType::CommandChunk) {
        RenderTapeCommandChunkHeader header{};
        std::memcpy(&header, event.payload.data(), sizeof(header));
        ImportedChunkView chunk;
        check(importPrevalidatedCommandChunk(event.payload.subspan(sizeof(header)),
            {header.wireVersion, header.recordCount, header.handleCount}, chunk), "original chunk imports");
        if (chunk.records[0].type == D9C_COMMAND_RECORD_PRESENT) {
          if (producerPresentRaw) {
            // The inherited destroy fixture intentionally has a non-command
            // tail. Supply a genuine final raw ACK scope for this producer
            // contract without moving or suppressing those original destroys.
            out.prePresentStateEvent = eventIndex++;
            builder.appendCommandChunk({.recordCount = 2u}, stateChunk());
          }
          present = eventIndex + 1u;
        }
      }
      builder.appendRawEvent(type, event.payload);
    }
    ++eventIndex;
    if (type == RenderTapeEventType::CommandChunk &&
        queryShape == QueryFixture::LateFreshMissingPrefix &&
        out.queryDefinitionEvent == kRenderTapeIdentityV4NoIndex) {
      appendQueryDefinition();
      appendQueryIssue();
    }
  }
  out.tape = builder.seal();
  std::vector<RenderTapeProviderBlob> blobs;
  for (const auto& bytes : out.blobs) blobs.push_back({RenderTapeCaptureSession::sha256(bytes), bytes});
  check(render_tape_runtime::buildGeneralReplayPlan(out.tape, blobs, out.replay).complete(),
      "complete production General grammar admits exact indexed/resource Tape");

  RenderTapeIdentityV4View identity;
  identity.header.coordinatorDomain = static_cast<std::uint32_t>(sessionDomain
      ? TapeCoordinatorAuthority::SessionRelease : TapeCoordinatorAuthority::OrdinarySerial);
  identity.header.frameId = 9u;
  identity.header.captureToken = 100u;
  identity.header.captureEpoch = 101u;
  identity.header.presentOrdinal = present;
  identity.header.queue = 102u;
  identity.header.initializerEpoch = 103u;
  identity.header.baselineCompletionEdge = 1u;
  identity.header.baselineKind = static_cast<std::uint32_t>(RenderTapeIdentityV3PrefixKind::EmptyBaseline);
  RenderTapeExecutionTopologyV1View topology;
  topology.header.captureToken = identity.header.captureToken;
  topology.header.captureEpoch = identity.header.captureEpoch;
  topology.header.queue = identity.header.queue;
  std::uint64_t edge = 2u, generation = 1u;
  for (const auto& event : out.replay.events) {
    if (event.type != RenderTapeEventType::CommandChunk) continue;
    const auto ordinal = out.replay.tape.event(event.eventIndex).header.ordinal;
    const auto raw = 500u + ordinal;
    const auto& admitted = out.replay.commandChunks[event.index];
    ImportedChunkView chunk;
    check(importPrevalidatedCommandChunk(admitted.bytes, admitted.envelope, chunk), "strict captured raw imports");
    const bool state = event.eventIndex == out.stateEvent || event.eventIndex == out.prePresentStateEvent;
    const bool query = event.eventIndex == out.queryIssueEvent;
    const bool isPresent = chunk.records[0].type == D9C_COMMAND_RECORD_PRESENT;
    const RenderTapeIdentityV4ReplaySourceIdentity projection{9u, 600u + ordinal, 600u + ordinal, raw};
    if (event.eventIndex == out.stateEvent && capturedRollback) {
      identity.attempts.push_back({.eventOrdinal = ordinal, .rawOrdinal = raw,
          .attemptGeneration = generation++, .beginEdge = edge++, .terminalEdge = edge++,
          .identity = projection, .recordCount = 1u,
          .outcome = static_cast<std::uint32_t>(Outcome::RolledBack)});
    }
    const auto begin = edge++, effect = edge++;
    const auto queryAdmission = query ? edge++ : 0u;
    const auto queryDispatch = query ? edge++ : 0u;
    const auto terminal = edge++;
    identity.attempts.push_back({.eventOrdinal = ordinal, .rawOrdinal = raw,
        .attemptGeneration = generation++, .beginEdge = begin, .terminalEdge = terminal,
        .effectEdge = effect, .identity = projection,
        .receipt = {.identity = projection, .queueSequence = raw,
            .kind = static_cast<std::uint32_t>(RenderTapeIdentityV4ReceiptKind::Compatibility),
            .commandCount = state || query ? 0u : 1u}, .recordCount = admitted.envelope.recordCount,
        .outcome = static_cast<std::uint32_t>(Outcome::Committed), .stagedCommandCount = state || query ? 0u : 1u});
    identity.settlements.push_back({ordinal, raw, edge++, admitted.envelope.recordCount});
    if (state) {
      identity.dispositions.push_back({.eventOrdinal = ordinal, .recordCount = 2u,
          .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::StateOnly)});
      if (event.eventIndex == out.stateEvent && missingInitializer) {
        identity.initializers.push_back({.sequence = 1u, .epoch = 103u,
            .publicationEdge = edge++, .submissionEdge = edge++, .completionEdge = edge++,
            .completedSequence = 1u, .cutEventOrdinal = ordinal,
            .cutKind = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterEvent)});
      }
      continue;
    }
    if (query) {
      const bool closed = queryShape == QueryFixture::ClosedUnsupported;
      identity.dispositions.push_back({.eventOrdinal = ordinal, .recordCount = 1u,
          .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::Coordinator),
          .coordinatorIndex = 0u});
      identity.coordinators.push_back({.provenance = {.eventOrdinal = ordinal, .rawOrdinal = raw,
          .buildGeneration = generation - 1u, .dispatchEdge = queryDispatch,
          .queryIdentity = out.queryIdentity,
          .issueCycle = explicitHistory ? 509u : queryCheckpoint || closed ? 32u : 1u,
          .issueOrdinal = explicitHistory ? 1017u : queryCheckpoint ? 48u : 1u, .deviceEpoch = 903u,
          .recordIndex = 0u, .recordType = D9C_COMMAND_RECORD_QUERY_ISSUE,
          .flags = queryFlags, .queryType = queryType,
          .prefixKind = static_cast<std::uint32_t>(queryFlags == 2u
              ? RenderTapeIdentityV3PrefixKind::None : RenderTapeIdentityV3PrefixKind::EmptyBaseline)},
          .admissionEdge = queryAdmission,
          .authority = static_cast<std::uint32_t>(TapeCoordinatorAuthority::OrdinarySerial)});
      continue;
    }
    const auto source = static_cast<std::uint32_t>(identity.sources.size());
    const auto command = static_cast<std::uint32_t>(identity.commands.size());
    const auto publication = edge++, acceptance = edge++, submission = edge++, completion = edge++;
    identity.sources.push_back({.lifecycle = {.sourceOrdinal = source + 1u, .seqId = source + 1u,
        .writerCaptureId = source + 1u, .writerGeneration = 2u, .storageGeneration = 3u,
        .publicationEdge = publication, .submissionEdge = submission, .completionEdge = completion,
        .completedSequence = source + 1u, .cutEventOrdinal = ordinal, .commandCount = 1u,
        .cutKind = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterEvent)},
        .rawOrdinal = raw, .publicationBuildGeneration = generation - 1u, .publicationControlIndex = source});
    identity.commands.push_back({.sourceIndex = source,
        .kind = static_cast<std::uint32_t>(isPresent ? RenderTapeIdentityV3CommandKind::Present
                                                   : RenderTapeIdentityV3CommandKind::DrawRun),
        .drawCount = isPresent ? 0u : 1u});
    identity.dispositions.push_back({.eventOrdinal = ordinal, .recordCount = 1u,
        .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::GpuCommand),
        .commandIndex = command, .subdrawCount = isPresent ? 0u : 1u});
    topology.plans.push_back({.planId = source + 11u, .acceptanceEdge = acceptance,
        .route = static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1Route::NaturalOrder),
        .firstPlacement = command, .placementCount = 1u, .executedCount = 1u});
    topology.placements.push_back({.planIndex = source, .commandIndex = command,
        .outcome = static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1Outcome::ExecutedWithoutDag),
        .executionOrdinal = 0u});
  }
  RenderTapeIdentityV4Status identityStatus{};
  out.identityBytes = buildRenderTapeIdentityV4(out.tape, out.replay.catalogue, identity, &identityStatus);
  check(identityStatus == RenderTapeIdentityV4Status::Valid, "genuine V4 rows serialize through production validation");
  RenderTapeExecutionTopologyV1Status topologyStatus{};
  out.topologyBytes = buildRenderTapeExecutionTopologyV1(out.tape, out.replay.catalogue,
      out.identityBytes, topology, &topologyStatus);
  check(topologyStatus == RenderTapeExecutionTopologyV1Status::Valid &&
      buildCaptureSourceReplayPlanV4(out.tape, out.replay.catalogue, out.identityBytes,
          out.topologyBytes, out.plan) == CaptureSourceReplayPlanV4Status::Valid,
      "exact V4 pair authenticates before executor admission");
  return out;
}

void narrow(const RenderTapeExecutionV4Evidence& evidence) {
  check(evidence.qualifierOnly && !evidence.policyQualified && !evidence.queryReadinessQualified &&
      !evidence.pixelsQualified && !evidence.timingEligible,
      "admission/ACK/diagnostic evidence never claims policy, query readiness, pixels or timing");
}

void noEffects(const RenderTapeExecutionV4Evidence& evidence) {
  narrow(evidence);
  check(!evidence.armed && !evidence.actualAttemptsObserved && !evidence.complete &&
      !evidence.canonicalTraversalComplete && !evidence.rawAckCoverageComplete &&
      !evidence.actualLifecycleComplete && !evidence.actualTopologyComplete &&
      !evidence.exactSourceDiagnosticComplete && !evidence.semanticMappingComplete &&
      !evidence.semanticMapping && !evidence.sourceObligationsRegistered &&
      !evidence.sourceObligationsPending && !evidence.sourceObligationsQualified &&
      evidence.dispatchedRecords == 0u &&
      evidence.replayedEvents == 0u && evidence.acknowledgedRaws == 0u &&
      !evidence.presentPreparationAttempted && !evidence.presentPreparationCompleted &&
      !evidence.localArm.queue && evidence.actualIdentity.attempts.empty() &&
      evidence.sources.empty() && evidence.initializers.empty() && evidence.coordinators.empty(),
      "pure admission has no candidate arm/dispatch/ACK/publication/lifecycle evidence");
}

void admission() {
  for (std::uint32_t variant = 0u; variant < 4u; ++variant) {
    auto fixture = makeFixture(variant == 1u, variant == 2u, variant == 3u);
    RenderTapeExecutionV4 execution;
    RenderTapeExecutionV4Evidence evidence;
    check(execution.admit(fixture.plan, fixture.replay, fixture.tape, evidence) &&
        evidence.requested && evidence.validated && evidence.planDigest == fixture.plan.digest() &&
        evidence.producerContract == ProducerContract::None &&
        evidence.presentPreparationEventIndex == kRenderTapeIdentityV4NoIndex &&
        execution.snapshotPrefixEvents() == fixture.prefix && !execution.failed(),
        "full exact-source diagnostic admission retains genuine V4 pair/General grammar");
    noEffects(evidence);
    check(!execution.arm(nullptr), "captured key/queue never authorizes a null local device");
    noEffects(evidence);
  }
  auto fixture = makeFixture();
  RenderTapeExecutionV4 execution;
  RenderTapeExecutionV4Evidence evidence;
  check(!execution.admit(fixture.plan, fixture.replay, fixture.tape, evidence,
          static_cast<RenderTapeExecutionV4PublicationMode>(255u)) &&
      std::string_view(execution.failure()) == "v4-publication-mode-unsupported" && !evidence.validated,
      "unknown publication mode rejects explicitly before effects");
  noEffects(evidence);
}

void preparedAdmissionFiniteTransitions() {
  // Exhaust the production predicate's complete Boolean abstraction: admitted,
  // failed, consumed, pristine, then exact plan/General/evidence/tape/mode/
  // producer-contract binding. Only the fresh fully bound state may claim.
  constexpr std::uint32_t axisCount = 10u;
  constexpr std::uint32_t allowedMask = ((1u << axisCount) - 1u) & ~((1u << 1u) | (1u << 2u));
  std::uint32_t allowedStates = 0u;
  for (std::uint32_t mask = 0u; mask < (1u << axisCount); ++mask) {
    const RenderTapePreparedAdmissionV4Facts facts{
        .admitted = bool(mask & (1u << 0u)),
        .failed = bool(mask & (1u << 1u)),
        .consumed = bool(mask & (1u << 2u)),
        .pristine = bool(mask & (1u << 3u)),
        .samePlanOwner = bool(mask & (1u << 4u)),
        .sameGeneralOwner = bool(mask & (1u << 5u)),
        .sameEvidenceOwner = bool(mask & (1u << 6u)),
        .sameTapeStorage = bool(mask & (1u << 7u)),
        .samePublicationMode = bool(mask & (1u << 8u)),
        .sameProducerContract = bool(mask & (1u << 9u))};
    const bool allowed = renderTapePreparedAdmissionV4ClaimAllowed(facts);
    check(allowed == (mask == allowedMask),
        "exhaustive production prepared-claim predicate rejects every missing or stale state axis");
    allowedStates += allowed;
    if (allowed) {
      auto consumed = facts;
      consumed.consumed = true;
      check(!renderTapePreparedAdmissionV4ClaimAllowed(consumed),
          "prepared claim's consumed successor is irrevocably ineligible");
    }
  }
  check(allowedStates == 1u, "finite prepared-admission domain has exactly one allowed state");
}

void preparedAdmissionClaim() {
  using Access = RenderTapeExecutionV4PreparedReplayAccess;
  for (const auto mode : {RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic,
                         RenderTapeExecutionV4PublicationMode::SemanticMapping}) {
    for (const auto contract : {ProducerContract::None, ProducerContract::PePresentReservationV1}) {
      auto fixture = makeFixture(false, false, false, false, false, QueryFixture::None,
          contract == ProducerContract::PePresentReservationV1);
      RenderTapeExecutionV4Evidence evidence;
      RenderTapeExecutionV4 execution;
      check(execution.admit(fixture.plan, fixture.replay, fixture.tape, evidence, mode, contract) &&
          evidence.admissionCount == 1u && !evidence.preparedAdmissionConsumed,
          "successful executor admission records one admission before prepared claim");
      const auto* owner = RenderTapeExecutionV4TestAccess::boundEvidence(execution);
      const auto capacities = std::array{evidence.sources.capacity(),
          evidence.initializers.capacity(), evidence.coordinators.capacity()};
      if (mode == RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic)
        check(!fixture.plan.identity().sources.empty() &&
            evidence.sources.capacity() >= fixture.plan.identity().sources.size(),
            "exact-source handoff storage check includes genuinely reserved nonzero admission capacity");
      const auto* sources = evidence.sources.data();
      const auto* initializers = evidence.initializers.data();
      const auto* coordinators = evidence.coordinators.data();
      const auto digest = evidence.planDigest;
      const auto prefix = execution.snapshotPrefixEvents();
      check(Access::consume(execution, fixture.plan, fixture.replay, fixture.tape,
              evidence, mode, contract) && evidence.preparedAdmissionConsumed &&
          evidence.admissionCount == 1u && evidence.planDigest == digest &&
          execution.snapshotPrefixEvents() == prefix && !execution.failed(),
          "prepared replay consumes one existing admitted executor without another admission");
      check(owner == &evidence && RenderTapeExecutionV4TestAccess::boundEvidence(execution) == owner &&
          capacities == std::array{evidence.sources.capacity(), evidence.initializers.capacity(),
              evidence.coordinators.capacity()} && sources == evidence.sources.data() &&
          initializers == evidence.initializers.data() && coordinators == evidence.coordinators.data(),
          "stable prepared claim preserves exact evidence owner, storage and reserved capacities");
      noEffects(evidence);
      check(!Access::consume(execution, fixture.plan, fixture.replay, fixture.tape,
              evidence, mode, contract) && execution.failed() &&
          evidence.admissionCount == 1u && evidence.preparedAdmissionConsumed,
          "same prepared executor cannot be claimed twice or gain a second admission");
      noEffects(evidence);
      execution.cancel();
    }
  }

  auto fixture = makeFixture();
  CaptureSourceReplayPlanV4 otherPlan;
  check(buildCaptureSourceReplayPlanV4(fixture.tape, fixture.replay.catalogue,
          fixture.identityBytes, fixture.topologyBytes, otherPlan) == CaptureSourceReplayPlanV4Status::Valid &&
      otherPlan.digest() == fixture.plan.digest(),
      "wrong-owner negative uses a separately authenticated plan with the exact same digest");
  auto otherGeneral = fixture.replay;
  auto otherTape = fixture.tape;
  for (std::uint32_t fault = 0u; fault < 10u; ++fault) {
    RenderTapeExecutionV4Evidence evidence, foreignEvidence;
    RenderTapeExecutionV4 execution;
    if (fault != 6u)
      check(execution.admit(fixture.plan, fixture.replay, fixture.tape, evidence),
          "prepared negative retains genuine production admission before its isolated fault");
    const auto* plan = &fixture.plan;
    const auto* general = &fixture.replay;
    auto tape = std::span<const std::byte>(fixture.tape);
    auto* receipt = &evidence;
    auto mode = RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic;
    auto contract = ProducerContract::None;
    if (fault == 0u) plan = &otherPlan;
    if (fault == 1u) general = &otherGeneral;
    if (fault == 2u) {
      foreignEvidence = evidence;
      receipt = &foreignEvidence;
    }
    if (fault == 3u) tape = otherTape;
    if (fault == 4u) mode = RenderTapeExecutionV4PublicationMode::SemanticMapping;
    if (fault == 5u) contract = ProducerContract::PePresentReservationV1;
    if (fault == 7u) check(!execution.arm(nullptr), "failed-owner negative rejects actual null arm");
    if (fault == 8u) execution.cancel();
    if (fault == 9u) tape = tape.first(tape.size() - 1u);
    const auto admissionCount = evidence.admissionCount;
    check(!Access::consume(execution, *plan, *general, tape, *receipt, mode, contract) &&
        execution.failed() && !evidence.preparedAdmissionConsumed &&
        evidence.admissionCount == admissionCount &&
        RenderTapeExecutionV4TestAccess::boundEvidence(execution) ==
            (fault == 6u ? nullptr : &evidence),
        "wrong owner/context/mode/contract, unadmitted, failed, cancelled or changed extent rejects before effects");
    noEffects(evidence);
    noEffects(foreignEvidence);
    execution.cancel();
  }
}

void immutablePreparationAdmission() {
  using Access = RenderTapeExecutionV4PreparedReplayAccess;
  for (const auto mode : {RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic,
                         RenderTapeExecutionV4PublicationMode::SemanticMapping}) {
    auto fixture = makeFixture(false, false, false, false, false, QueryFixture::None, true);
    RenderTapeExecutionV4Preparation preparation;
    const char* failure = "none";
    check(Access::prepare(preparation, fixture.plan, fixture.replay, fixture.tape,
            mode, ProducerContract::PePresentReservationV1, failure) && preparation.valid() &&
        preparation.snapshotPrefixEvents() == fixture.prefix,
        "immutable execution factory authenticates complete templates before publishing valid input authority");
    RenderTapeExecutionV4Evidence ordinaryEvidence, firstEvidence, secondEvidence;
    RenderTapeExecutionV4 ordinary, first, second;
    check(ordinary.admit(fixture.plan, fixture.replay, fixture.tape, ordinaryEvidence,
            mode, ProducerContract::PePresentReservationV1) &&
        Access::initialize(first, preparation, fixture.plan, fixture.replay, fixture.tape,
            firstEvidence, mode, ProducerContract::PePresentReservationV1) &&
        Access::initialize(second, preparation, fixture.plan, fixture.replay, fixture.tape,
            secondEvidence, mode, ProducerContract::PePresentReservationV1),
        "ordinary admission and two independent invocation factories admit the same neutral contract");
    check(RenderTapeExecutionV4TestAccess::sameNeutralTables(ordinary, first) &&
        RenderTapeExecutionV4TestAccess::sameNeutralTables(first, second) &&
        RenderTapeExecutionV4TestAccess::sharedImmutableTables(first, second) &&
        firstEvidence.planDigest == ordinaryEvidence.planDigest &&
        secondEvidence.planDigest == ordinaryEvidence.planDigest &&
        firstEvidence.publicationMode == ordinaryEvidence.publicationMode &&
        firstEvidence.producerContract == ordinaryEvidence.producerContract &&
        firstEvidence.presentPreparationEventIndex == ordinaryEvidence.presentPreparationEventIndex &&
        firstEvidence.queryBaselineExpected == ordinaryEvidence.queryBaselineExpected &&
        firstEvidence.sourceObligationsExpected == ordinaryEvidence.sourceObligationsExpected &&
        firstEvidence.admissionCount == 1u && secondEvidence.admissionCount == 1u &&
        RenderTapeExecutionV4TestAccess::freshMutableState(first) &&
        RenderTapeExecutionV4TestAccess::freshMutableState(second),
        "immutable spans are borrowed once while neutral input facts and fresh mutable state match ordinary admission");
    noEffects(ordinaryEvidence);
    noEffects(firstEvidence);
    noEffects(secondEvidence);
    check(!Access::prepare(preparation, fixture.plan, fixture.replay, fixture.tape,
            mode, ProducerContract::PePresentReservationV1, failure) &&
        std::string_view(failure) == "v4-preparation-repeat" && preparation.valid() &&
        RenderTapeExecutionV4TestAccess::sharedImmutableTables(first, second),
        "repreparing stationary live templates rejects without invalidating any initialized borrow");
    check(Access::consume(first, fixture.plan, fixture.replay, fixture.tape, firstEvidence,
            mode, ProducerContract::PePresentReservationV1) &&
        Access::consume(second, fixture.plan, fixture.replay, fixture.tape, secondEvidence,
            mode, ProducerContract::PePresentReservationV1),
        "immutable owner grants separate irreversible invocation claims, not one shared mutable cursor");
    check(RenderTapeExecutionV4TestAccess::completePresentPhaseFixture(first) &&
        RenderTapeExecutionV4TestAccess::freshMutableState(second) &&
        !firstEvidence.presentPreparationAttempted && !firstEvidence.presentPreparationCompleted,
        "pure production Present-phase mutation belongs to one executor and grants no actual preparation evidence");
    ordinary.cancel();
    first.cancel();
    second.cancel();
    RenderTapeExecutionV4Evidence nextEvidence;
    RenderTapeExecutionV4 next;
    check(Access::initialize(next, preparation, fixture.plan, fixture.replay, fixture.tape,
            nextEvidence, mode, ProducerContract::PePresentReservationV1) &&
        RenderTapeExecutionV4TestAccess::freshMutableState(next) &&
        RenderTapeExecutionV4TestAccess::sharedImmutableTables(first, next),
        "cancellation after changed Present phase cannot seed the next initializer from immutable templates");
    noEffects(nextEvidence);
    next.cancel();
  }

  auto fixture = makeFixture();
  RenderTapeExecutionV4Preparation preparation;
  const char* failure = "none";
  check(Access::prepare(preparation, fixture.plan, fixture.replay, fixture.tape,
          RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic, ProducerContract::None, failure),
      "factory binding negatives start with one complete immutable authority");
  CaptureSourceReplayPlanV4 otherPlan;
  check(buildCaptureSourceReplayPlanV4(fixture.tape, fixture.replay.catalogue,
          fixture.identityBytes, fixture.topologyBytes, otherPlan) == CaptureSourceReplayPlanV4Status::Valid &&
      otherPlan.digest() == fixture.plan.digest(), "factory wrong owner preserves the same authenticated digest");
  auto otherGeneral = fixture.replay;
  auto otherTape = fixture.tape;
  RenderTapeExecutionV4Preparation empty;
  for (std::uint32_t fault = 0u; fault < 7u; ++fault) {
    RenderTapeExecutionV4Evidence evidence;
    RenderTapeExecutionV4 execution;
    check(!Access::initialize(execution, fault == 6u ? empty : preparation,
            fault == 0u ? otherPlan : fixture.plan, fault == 1u ? otherGeneral : fixture.replay,
            fault == 2u ? std::span<const std::byte>(otherTape) :
                fault == 3u ? std::span<const std::byte>(fixture.tape).first(fixture.tape.size() - 1u) :
                              std::span<const std::byte>(fixture.tape), evidence,
            fault == 4u ? RenderTapeExecutionV4PublicationMode::SemanticMapping :
                          RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic,
            fault == 5u ? ProducerContract::PePresentReservationV1 : ProducerContract::None) &&
        !evidence.validated && evidence.admissionCount == 0u && !evidence.preparedAdmissionConsumed,
        "fresh invocation factory rejects wrong exact owner/storage/extent/policy or missing authority without partial admission");
    noEffects(evidence);
    execution.cancel();
  }
  auto unsupported = makeFixture(false, false, false, false, false, QueryFixture::ClosedUnsupported);
  RenderTapeExecutionV4Preparation rejected;
  check(!Access::prepare(rejected, unsupported.plan, unsupported.replay, unsupported.tape,
          RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic, ProducerContract::None, failure) &&
      !rejected.valid(), "authenticated unsupported query input publishes no partial immutable preparation");
}

void semanticAdmission() {
  for (std::uint32_t variant = 0u; variant < 4u; ++variant) {
    auto fixture = makeFixture(variant == 1u, variant == 2u, variant == 3u);
    RenderTapeExecutionV4 execution;
    RenderTapeExecutionV4Evidence evidence;
    check(execution.admit(fixture.plan, fixture.replay, fixture.tape, evidence,
            RenderTapeExecutionV4PublicationMode::SemanticMapping) && evidence.validated &&
        evidence.publicationMode == RenderTapeExecutionV4PublicationMode::SemanticMapping &&
        evidence.sourceObligationsExpected == fixture.plan.identity().sources.size() &&
        evidence.planDigest == fixture.plan.digest(),
        "semantic admission bounds original source obligations without publication or traversal authority");
    noEffects(evidence);
  }
  auto fixture = makeFixture(false, false, false, true);
  check(!fixture.plan.identity().initializers.empty(),
      "initializer rejection uses an authenticated nonzero interval initializer table");
  RenderTapeExecutionV4 execution;
  RenderTapeExecutionV4Evidence evidence;
  check(!execution.admit(fixture.plan, fixture.replay, fixture.tape, evidence,
          RenderTapeExecutionV4PublicationMode::SemanticMapping) && !evidence.validated &&
      std::string_view(execution.failure()) == "v4-semantic-initializer-unsupported",
      "unsupported interval initializer mapping fails before local arm or replay effects");
  noEffects(evidence);

  auto missingPrefix = makeFixture(false, false, false, false, false,
      QueryFixture::LateFreshMissingPrefix);
  check(missingPrefix.plan.identity().coordinators.size() == 1u &&
      missingPrefix.plan.identity().sources[0].lifecycle.cutEventOrdinal <
          missingPrefix.plan.identity().coordinators[0].provenance.eventOrdinal,
      "captured END fixture is authenticated but claims baseline after an original GPU draw");
  RenderTapeExecutionV4 missingPrefixExecution;
  RenderTapeExecutionV4Evidence missingPrefixEvidence;
  check(!missingPrefixExecution.admit(missingPrefix.plan, missingPrefix.replay, missingPrefix.tape,
          missingPrefixEvidence, RenderTapeExecutionV4PublicationMode::SemanticMapping) &&
      !missingPrefixEvidence.validated &&
      std::string_view(missingPrefixExecution.failure()) == "v4-semantic-reference-unsupported",
      "cold reference feasibility rejects missing pre-END source coverage before effects");
  noEffects(missingPrefixEvidence);
}

void mergeAuthoredDraws(RenderTapeIdentityV4View& identity,
    RenderTapeExecutionTopologyV1View& topology) {
  check(identity.sources.size() == 3u && identity.commands.size() == 3u &&
      identity.coordinators.empty() && identity.commands[0].drawCount == 1u &&
      identity.commands[1].drawCount == 1u,
      "authored split/merge fixture has two canonical draws and one terminal Present");
  identity.sources.erase(identity.sources.begin());
  identity.commands[0].drawCount = 2u;
  identity.commands.erase(identity.commands.begin() + 1u);
  identity.commands[1].sourceIndex = 1u;
  for (auto& disposition : identity.dispositions) {
    if (disposition.commandIndex == kRenderTapeIdentityV4NoIndex) continue;
    if (disposition.commandIndex == 1u) {
      disposition.commandIndex = 0u;
      disposition.firstSubdraw = 1u;
    } else if (disposition.commandIndex > 1u) {
      --disposition.commandIndex;
    }
  }
  topology.plans.erase(topology.plans.begin());
  topology.placements.erase(topology.placements.begin());
  for (std::uint32_t index = 0u; index < topology.plans.size(); ++index) {
    topology.plans[index].firstPlacement = index;
    topology.placements[index].planIndex = index;
    topology.placements[index].commandIndex = index;
  }
}

void delayAuthoredPublication(RenderTapeIdentityV4View& identity,
    RenderTapeExecutionTopologyV1View& topology) {
  check(identity.sources.size() == 3u && identity.coordinators.empty(),
      "authored delayed-publication fixture has no query or initializer dependency");
  identity.header.baselineCompletionEdge *= 10u;
  for (auto& source : identity.sources) {
    source.lifecycle.publicationEdge *= 10u;
    source.lifecycle.submissionEdge *= 10u;
    source.lifecycle.completionEdge *= 10u;
  }
  for (auto& attempt : identity.attempts) {
    attempt.beginEdge *= 10u;
    attempt.terminalEdge *= 10u;
    attempt.effectEdge *= 10u;
  }
  for (auto& settlement : identity.settlements) settlement.replayEdge *= 10u;
  for (auto& plan : topology.plans) plan.acceptanceEdge *= 10u;
  const auto cut = identity.sources[1].lifecycle.cutEventOrdinal;
  const auto settlement = std::find_if(identity.settlements.begin(), identity.settlements.end(),
      [&](const auto& value) { return value.eventOrdinal == cut; });
  check(settlement != identity.settlements.end(), "later original raw has its own declared settlement");
  auto& first = identity.sources[0];
  first.lifecycle.cutEventOrdinal = cut;
  first.lifecycle.publicationEdge = settlement->replayEdge + 1u;
  first.lifecycle.submissionEdge = settlement->replayEdge + 3u;
  first.lifecycle.completionEdge = settlement->replayEdge + 4u;
  first.rawOrdinal = identity.sources[1].rawOrdinal;
  first.publicationBuildGeneration = identity.sources[1].publicationBuildGeneration;
  topology.plans[0].acceptanceEdge = settlement->replayEdge + 2u;
}

void rebuildAuthoredReference(AdmissionFixture& fixture,
    const RenderTapeIdentityV4View& identity, const RenderTapeExecutionTopologyV1View& topology) {
  RenderTapeIdentityV4Status identityStatus{};
  fixture.identityBytes = buildRenderTapeIdentityV4(
      fixture.tape, fixture.replay.catalogue, identity, &identityStatus);
  check(identityStatus == RenderTapeIdentityV4Status::Valid,
      "authored merged reference passes complete production identity validation");
  RenderTapeExecutionTopologyV1Status topologyStatus{};
  fixture.topologyBytes = buildRenderTapeExecutionTopologyV1(fixture.tape,
      fixture.replay.catalogue, fixture.identityBytes, topology, &topologyStatus);
  check(topologyStatus == RenderTapeExecutionTopologyV1Status::Valid &&
      buildCaptureSourceReplayPlanV4(fixture.tape, fixture.replay.catalogue, fixture.identityBytes,
          fixture.topologyBytes, fixture.plan) == CaptureSourceReplayPlanV4Status::Valid,
      "authored merged reference authenticates its independent topology companion");
}

void semanticCorrespondenceFixtures() {
  for (std::uint32_t variant = 0u; variant < 3u; ++variant) {
    auto fixture = makeFixture(false, false, false, false, false, QueryFixture::None, false, 1u);
    auto candidate = fixture.plan.identity();
    auto candidateTopology = fixture.plan.topology();
    if (variant == 0u) {
      mergeAuthoredDraws(candidate, candidateTopology);
    } else if (variant == 1u) {
      auto reference = fixture.plan.identity();
      auto referenceTopology = fixture.plan.topology();
      mergeAuthoredDraws(reference, referenceTopology);
      rebuildAuthoredReference(fixture, reference, referenceTopology);
    } else {
      delayAuthoredPublication(candidate, candidateTopology);
    }
    RenderTapeExecutionV4 execution;
    RenderTapeExecutionV4Evidence observed;
    check(execution.admit(fixture.plan, fixture.replay, fixture.tape, observed,
            RenderTapeExecutionV4PublicationMode::SemanticMapping),
        "authored semantic comparison admits the unchanged complete General Tape");
    noEffects(observed);
    // This seam traverses only the value cursor and deferred registration.
    // It supplies no replay/ACK, observed source, arm or completion authority.
    check(RenderTapeExecutionV4TestAccess::traverseSemanticFixture(execution) &&
        observed.sourceObligationsRegistered == observed.sourceObligationsExpected &&
        observed.sourceObligationsPending == observed.sourceObligationsExpected &&
        !observed.sourceObligationsQualified && !observed.semanticMappingComplete &&
        !observed.complete && !observed.canonicalTraversalComplete && !observed.rawAckCoverageComplete &&
        observed.sources.empty() && observed.actualIdentity.sources.empty(),
        "completed value traversal leaves every source obligation pending with no fabricated receipt");
    RenderTapeExecutionV4Evidence comparison = observed;
    comparison.actualIdentity = std::move(candidate);
    comparison.actualTopology = std::move(candidateTopology);
    check(RenderTapeExecutionV4TestAccess::validateLocalRows(execution, comparison.actualIdentity) ==
            RenderTapeIdentityV4Status::Valid &&
        validateRenderTapeExecutionTopologyV1Rows(comparison.actualIdentity, comparison.actualTopology) ==
            RenderTapeExecutionTopologyV1Status::Valid,
        "authored candidate split/merge/delayed graphs independently validate complete lifecycle and topology");
    check(RenderTapeExecutionV4TestAccess::qualifySemanticFixture(execution, comparison) &&
        comparison.semanticMappingStatus == RenderTapeSemanticMappingV4Status::Valid &&
        comparison.semanticMappingComplete && comparison.semanticMapping &&
        comparison.semanticMapping->valid() && !comparison.sourceObligationsPending &&
        comparison.sourceObligationsQualified == observed.sourceObligationsExpected &&
        comparison.semanticMapping->records().size() == fixture.plan.records().size() &&
        comparison.semanticMapping->obligations().size() == observed.sourceObligationsExpected &&
        comparison.sources.empty() && comparison.initializers.empty() && comparison.coordinators.empty() &&
        !comparison.exactSourceDiagnosticComplete && !comparison.complete &&
        !comparison.actualLifecycleComplete && !comparison.actualTopologyComplete,
        "production semantic comparator discharges real-origin relations without promoting authored execution evidence");
    if (variant == 0u)
      check(comparison.semanticMapping->sources()[0].candidateIndex ==
              comparison.semanticMapping->sources()[1].candidateIndex,
          "two original sources map to the same declared candidate DrawRun source");
    if (variant == 1u)
      check(comparison.semanticMapping->obligations()[0].sourceRelationCount == 2u,
          "one original merged source obligation maps to two candidate source memberships");
    const auto copied = comparison;
    check(copied.semanticMapping == comparison.semanticMapping &&
        copied.semanticMapping->planDigest() == fixture.plan.digest() &&
        observed.sourceObligationsPending == observed.sourceObligationsExpected &&
        !observed.semanticMapping && !observed.semanticMappingComplete,
        "receipt copies retain immutable owned mapping while the comparison seam preserves observed evidence");
    narrow(comparison);
    narrow(observed);
  }
}

void semanticObligationNegatives() {
  for (std::uint32_t variant = 0u; variant < 5u; ++variant) {
    auto fixture = makeFixture();
    std::vector<CaptureSourceReplayPlanV4Action> sources;
    for (const auto& action : fixture.plan.actions())
      if (action.kind == CaptureSourceReplayPlanV4ActionKind::ExpectSourcePublication)
        sources.push_back(action);
    check(sources.size() >= 2u, "negative fixture owns distinct Draw and Present publication obligations");
    RenderTapeExecutionV4 execution;
    RenderTapeExecutionV4Evidence evidence;
    check(execution.admit(fixture.plan, fixture.replay, fixture.tape, evidence,
            RenderTapeExecutionV4PublicationMode::SemanticMapping), "obligation negative admits before effects");
    if (variant == 0u) {
      check(!RenderTapeExecutionV4TestAccess::registerSemanticFixture(execution, sources[1]) &&
          !evidence.sourceObligationsRegistered,
          "out-of-order source obligation cannot grant traversal registration");
    } else if (variant == 1u) {
      auto forged = sources[0];
      ++forged.evidenceEdge;
      check(!RenderTapeExecutionV4TestAccess::registerSemanticFixture(execution, forged) &&
          !evidence.sourceObligationsRegistered,
          "changed publication evidence edge cannot register an authenticated obligation");
    } else if (variant == 2u) {
      check(RenderTapeExecutionV4TestAccess::registerSemanticFixture(execution, sources[0]) &&
          !RenderTapeExecutionV4TestAccess::registerSemanticFixture(execution, sources[0]) &&
          evidence.sourceObligationsRegistered == 1u && evidence.sourceObligationsPending == 1u,
          "duplicate obligation registration leaves the exactly-once pending count unchanged");
    } else {
      if (variant == 3u)
        check(RenderTapeExecutionV4TestAccess::registerSemanticFixture(execution, sources[0]),
            "one authenticated obligation registers without creating a receipt");
      else
        check(RenderTapeExecutionV4TestAccess::traverseSemanticFixture(execution),
            "all authenticated obligations register before missing candidate evidence is evaluated");
      RenderTapeExecutionV4Evidence comparison = evidence;
      comparison.actualIdentity = fixture.plan.identity();
      if (variant == 4u) comparison.actualIdentity.sources.pop_back();
      check(!RenderTapeExecutionV4TestAccess::qualifySemanticFixture(execution, comparison) &&
          !comparison.semanticMapping && !comparison.semanticMappingComplete && !comparison.complete &&
          !comparison.sourceObligationsQualified &&
          comparison.sourceObligationsPending == comparison.sourceObligationsRegistered &&
          std::string_view(execution.failure()) == (variant == 3u
              ? "v4-semantic-source-obligations-incomplete" : "v4-semantic-mapping-invalid"),
          "unregistered or unresolved candidate source obligations cannot promote value traversal to qualification");
      narrow(comparison);
    }
    check(!evidence.complete && !evidence.semanticMappingComplete && !evidence.semanticMapping &&
        !evidence.sourceObligationsQualified && evidence.sources.empty() &&
        evidence.actualIdentity.sources.empty(),
        "all obligation failures preserve genuine absence of publication and completion evidence");
    narrow(evidence);
  }
}

void producerContractAdmission() {
  check(prepareRenderTapePresentReservation(nullptr) != core::D3D_OK,
      "shared actual producer preparation rejects a null device without effects");
  auto fixture = makeFixture(false, false, false, false, false, QueryFixture::None, true);
  const auto digest = fixture.plan.digest();
  const auto actions = std::vector<CaptureSourceReplayPlanV4Action>(
      fixture.plan.actions().begin(), fixture.plan.actions().end());
  const auto present = std::find_if(fixture.plan.events().begin(), fixture.plan.events().end(),
      [&](const auto& event) { return event.eventOrdinal == fixture.replay.presentOrdinal; });
  check(present != fixture.plan.events().end(), "producer fixture has an authenticated original Present raw");
  const auto presentIndex = static_cast<std::uint32_t>(present - fixture.plan.events().begin());
  check(fixture.prePresentStateEvent != kRenderTapeIdentityV4NoIndex &&
      fixture.prePresentStateEvent + 1u == presentIndex &&
      fixture.plan.events()[fixture.prePresentStateEvent].commandChunkIndex != kRenderTapeIdentityV4NoIndex &&
      fixture.plan.identity().settlements.size() >= 2u &&
      fixture.plan.identity().settlements.back().eventOrdinal == present->eventOrdinal &&
      fixture.plan.identity().settlements[fixture.plan.identity().settlements.size() - 2u].eventOrdinal ==
          fixture.plan.events()[fixture.prePresentStateEvent].eventOrdinal,
      "producer fixture retains genuine final state raw attempt/ACK immediately before original Present");
  for (auto contract : {ProducerContract::None, ProducerContract::PePresentReservationV1}) {
    RenderTapeExecutionV4 execution;
    RenderTapeExecutionV4Evidence evidence;
    check(execution.admit(fixture.plan, fixture.replay, fixture.tape, evidence,
            RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic, contract) &&
        evidence.producerContract == contract && evidence.planDigest == digest &&
        evidence.presentPreparationEventIndex == (contract == ProducerContract::None
            ? kRenderTapeIdentityV4NoIndex : presentIndex),
        "explicit producer selection binds only the admitted digest and original General Present raw");
    noEffects(evidence);
    if (contract == ProducerContract::PePresentReservationV1) {
      check(!execution.beginEvent(presentIndex) && execution.failed() &&
          !evidence.presentPreparationAttempted && !evidence.presentPreparationCompleted,
          "selection and a Present opcode do not grant preparation without an armed preceding actual raw ACK");
    }
  }
  for (std::uint32_t mutation = 0u; mutation < 3u; ++mutation) {
    auto forged = fixture.replay;
    auto tape = std::span<const std::byte>(fixture.tape);
    auto changed = fixture.tape;
    if (mutation == 0u) ++forged.presentOrdinal;
    if (mutation == 1u) ++forged.commandChunks[present->commandChunkIndex].envelope.recordCount;
    if (mutation == 2u) { changed.back() ^= std::byte{1u}; tape = changed; }
    RenderTapeExecutionV4 execution;
    RenderTapeExecutionV4Evidence evidence;
    check(!execution.admit(fixture.plan, forged, tape, evidence,
            RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic,
            ProducerContract::PePresentReservationV1) && !evidence.validated,
        "wrong original Present evidence or exact Tape digest cannot admit the producer adapter");
    noEffects(evidence);
  }
  RenderTapeExecutionV4 unsupported;
  RenderTapeExecutionV4Evidence evidence;
  check(!unsupported.admit(fixture.plan, fixture.replay, fixture.tape, evidence,
          RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic,
          static_cast<ProducerContract>(255u)) &&
      std::string_view(unsupported.failure()) == "v4-producer-contract-unsupported",
      "unknown producer contracts are rejected before any preparation or interval effect");
  noEffects(evidence);
  auto noncommandTail = makeFixture();
  RenderTapeExecutionV4 missingPriorRaw;
  check(!missingPriorRaw.admit(noncommandTail.plan, noncommandTail.replay, noncommandTail.tape, evidence,
          RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic,
          ProducerContract::PePresentReservationV1) &&
      std::string_view(missingPriorRaw.failure()) == "v4-present-preparation-admission" && !evidence.validated,
      "generic destroy-to-Present tail cannot grant a producer boundary with no preceding command raw ACK scope");
  noEffects(evidence);
  check(fixture.plan.digest() == digest &&
      std::equal(actions.begin(), actions.end(), fixture.plan.actions().begin()),
      "producer admission never adds a flush action or rewrites captured cuts, extents or plan digest");
}

void presentPreparationTransitions() {
  RenderTapeDigest digest{};
  digest[0] = std::byte{1u};
  const PreparationState admitted{ProducerContract::PePresentReservationV1, digest, 7u, 8u};
  for (auto contract : {ProducerContract::None, ProducerContract::PePresentReservationV1,
          static_cast<ProducerContract>(255u)}) {
    for (auto phase : {PreparationPhase::Admitted, PreparationPhase::Preparing,
            PreparationPhase::Completed, PreparationPhase::Failed,
            static_cast<PreparationPhase>(255u)}) {
      for (auto action : {PreparationAction::Begin, PreparationAction::Complete,
              PreparationAction::Fail, static_cast<PreparationAction>(255u)}) {
        for (std::uint32_t mutation = 0u; mutation < 6u; ++mutation) {
          auto state = admitted;
          state.contract = contract;
          state.phase = phase;
          auto suppliedDigest = digest;
          auto event = state.eventIndex;
          auto ordinal = state.eventOrdinal;
          if (mutation == 1u) suppliedDigest[0] ^= std::byte{1u};
          if (mutation == 2u) ++event;
          if (mutation == 3u) ++ordinal;
          if (mutation == 4u) state.eventIndex = kRenderTapeIdentityV4NoIndex;
          if (mutation == 5u) state.planDigest = suppliedDigest = {};
          const auto prior = state;
          const bool expected = contract == ProducerContract::PePresentReservationV1 && mutation == 0u &&
              ((action == PreparationAction::Begin && phase == PreparationPhase::Admitted) ||
               ((action == PreparationAction::Complete || action == PreparationAction::Fail) &&
                phase == PreparationPhase::Preparing));
          check(renderTapeV4PresentPreparationAdvance(state, action, suppliedDigest, event, ordinal) == expected,
              "shared pure producer reducer requires exact contract/digest/event and one allowed phase transition");
          check(expected ? state.contract == prior.contract && state.planDigest == prior.planDigest &&
                  state.eventIndex == prior.eventIndex && state.eventOrdinal == prior.eventOrdinal &&
                  state.phase == (action == PreparationAction::Begin ? PreparationPhase::Preparing
                      : action == PreparationAction::Complete ? PreparationPhase::Completed : PreparationPhase::Failed)
              : state == prior, "accepted preparation changes only phase; rejection leaves all authority unchanged");
        }
      }
    }
  }
}

void presentPreparationOrdering() {
  RenderTapeDigest digest{};
  digest[0] = std::byte{1u};
  const PreparationState admitted{ProducerContract::PePresentReservationV1, digest, 7u, 8u};
  for (std::uint32_t outcome = 0u; outcome < 3u; ++outcome) {
    auto state = admitted;
    std::array<std::uint32_t, 2u> order{};
    std::uint32_t calls = 0u, prepares = 0u, closes = 0u;
    // This is a fake boundary, not reported actual GPU/source evidence. The
    // production orchestrator must preserve its previous ACK cut until close.
    const std::uint64_t previousOrdinal = 7u;
    const auto previousCut = RenderTapeIdentityV3CutKind::AfterEvent;
    bool previousRawOpen = true, published = false, prepareSawPrior = false, closeSawPublication = false;
    const auto prepare = [&]() noexcept {
      ++prepares;
      order[calls++] = 1u;
      prepareSawPrior = previousRawOpen && previousOrdinal == 7u &&
          previousCut == RenderTapeIdentityV3CutKind::AfterEvent && state.phase == PreparationPhase::Preparing;
      published = outcome != 1u;
      return published;
    };
    const auto close = [&]() noexcept {
      ++closes;
      order[calls++] = 2u;
      closeSawPublication = published && previousRawOpen && state.phase == PreparationPhase::Completed;
      if (outcome != 2u) previousRawOpen = false;
      return outcome != 2u;
    };
    const auto result = renderTapeV4PresentPreparationBeforeClose(state, digest, 7u, 8u, prepare, close);
    check(result == (outcome == 0u ? PreparationResult::Complete
        : outcome == 1u ? PreparationResult::PreparationFailed : PreparationResult::CloseFailed) &&
        prepares == 1u && order[0] == 1u && prepareSawPrior,
        "shared production orchestration enters preparation exactly once under the preceding ACK AfterEvent scope");
    check(outcome == 1u ? closes == 0u && calls == 1u && previousRawOpen && state.phase == PreparationPhase::Failed
        : closes == 1u && calls == 2u && order[1] == 2u && closeSawPublication &&
          state.phase == PreparationPhase::Completed && previousRawOpen == (outcome == 2u),
        "preparation must finish before previous event comparison; preparation failure leaves it open and forbids close");
    const auto prior = state;
    check(renderTapeV4PresentPreparationBeforeClose(state, digest, 7u, 8u, prepare, close) ==
          PreparationResult::InvalidBinding && state == prior && prepares == 1u && closes == (outcome == 1u ? 0u : 1u),
        "successful, failed, or close-rejected preparation cannot be repeated after its side-effect entry");
  }
  for (std::uint32_t mutation = 0u; mutation < 6u; ++mutation) {
    auto state = admitted;
    auto suppliedDigest = digest;
    auto event = state.eventIndex;
    auto ordinal = state.eventOrdinal;
    if (mutation == 0u) suppliedDigest[0] ^= std::byte{1u};
    if (mutation == 1u) ++event;
    if (mutation == 2u) ++ordinal;
    if (mutation == 3u) { ++event; ++ordinal; }
    if (mutation == 4u) state.contract = static_cast<ProducerContract>(255u);
    if (mutation == 5u) state.planDigest = suppliedDigest = {};
    const auto prior = state;
    std::uint32_t prepares = 0u, closes = 0u;
    check(renderTapeV4PresentPreparationBeforeClose(state, suppliedDigest, event, ordinal,
            [&]() noexcept { ++prepares; return true; }, [&]() noexcept { ++closes; return true; }) ==
          PreparationResult::InvalidBinding && state == prior && prepares == 0u && closes == 0u,
        "wrong digest/event, skipped Present, or unsupported contract cannot authorize either effect or previous close");
  }
  for (auto contract : {ProducerContract::None, ProducerContract::PePresentReservationV1}) {
    auto state = admitted;
    state.contract = contract;
    const auto prior = state;
    std::uint32_t prepares = 0u, closes = 0u;
    check(renderTapeV4PresentPreparationBeforeClose(state, digest, 6u, 7u,
            [&]() noexcept { ++prepares; return true; }, [&]() noexcept { ++closes; return true; }) ==
          PreparationResult::Complete && state == prior && prepares == 0u && closes == 1u,
        "unselected and pre-Present boundaries preserve the original event-only close path without preparation");
    if (contract == ProducerContract::None) {
      check(renderTapeV4PresentPreparationBeforeClose(state, digest, 7u, 8u,
              [&]() noexcept { ++prepares; return true; }, [&]() noexcept { ++closes; return true; }) ==
            PreparationResult::Complete && state == prior && prepares == 0u && closes == 2u,
          "generic validated Present remains unselected and never acquires producer preparation authority");
    }
  }
}

void exactGeneralBinding() {
  for (std::uint32_t mutation = 0u; mutation < 15u; ++mutation) {
    auto fixture = makeFixture(false, false, true);
    CaptureSourceReplayPlanV4 invalid, moved;
    auto* plan = &fixture.plan;
    auto tape = std::span<const std::byte>(fixture.tape);
    std::vector<std::byte> changed;
    if (mutation == 0u) plan = &invalid;
    if (mutation == 1u) moved = std::move(fixture.plan);
    if (mutation == 2u) { changed = fixture.tape; changed.back() ^= std::byte{1u}; tape = changed; }
    if (mutation == 3u) ++fixture.replay.events.back().aux;
    if (mutation == 4u) ++fixture.replay.commandChunks[0].envelope.recordCount;
    if (mutation == 5u) fixture.replay.catalogue.blobs.pop_back();
    if (mutation == 6u) fixture.replay.catalogue.blobs[0].verified = 0u;
    if (mutation == 7u) ++fixture.replay.definitions[0].fixed.expectedContentCount;
    if (mutation == 8u) ++fixture.replay.bootstrapChunks[0].envelope.handleCount;
    if (mutation == 9u) ++fixture.replay.outputDesc.width;
    if (mutation == 10u) ++fixture.replay.presentOrdinal;
    if (mutation == 11u) ++fixture.replay.completionOrdinal;
    if (mutation == 12u) fixture.replay.expectedDigest[0] ^= std::byte{1u};
    if (mutation == 13u) ++fixture.replay.controls[0].controlBytes;
    if (mutation == 14u) fixture.replay.controlBytes[0][0] ^= std::byte{1u};
    RenderTapeExecutionV4 execution;
    RenderTapeExecutionV4Evidence evidence;
    check(!execution.admit(*plan, fixture.replay, tape, evidence) && execution.failed() && !evidence.validated,
        "invalid/moved owner or any exact Tape/derived General field mismatch rejects before effects");
    noEffects(evidence);
  }
}

struct Upper final : Device {
  Upper() : queue_(CommandQueue::ArenaLeaseTestQueueTag{}, limits_) {}
  WMT::Device wmtDevice() override { return WMT::Device{NULL_OBJECT_HANDLE}; }
  CommandQueue& queue() override { return queue_; }
  const core::BackendLimits& limits() const override { return limits_; }
  std::shared_ptr<core::BackendDevice> backend() override { return {}; }
  bool usesAsyncQueryFences() const noexcept override { return true; }
  query::EndReceipt publishQueryEnd() noexcept override { return queue_.publishQueryEnd(); }
  query::Poll pollQueryFence(const query::EndReceipt& receipt, bool flush) noexcept override {
    return queue_.pollQueryFence(receipt, flush);
  }
  query::Progress queryProgress() noexcept override { return queue_.queryProgress(); }
  core::BackendLimits limits_{};
  CommandQueue queue_;
};

struct LocalDevice {
  LocalDevice() {
    auto upper = std::make_unique<Upper>();
    queue = &upper->queue();
    factory = com::Direct3DCreate9Ex(com::D3D_SDK_VERSION, std::move(upper));
    check(factory != nullptr, "inert real frontend factory exists");
    core::PresentParameters params{};
    params.backBufferWidth = params.backBufferHeight = 8u;
    params.backBufferFormat = core::Format::A8R8G8B8;
    params.windowed = true;
    params.deviceWindow = core::Handle{1u};
    params.presentationInterval = core::PresentInterval::Immediate;
    iface = factory->CreateDeviceEx(0u, params, nullptr);
    check(iface != nullptr, "inert real frontend device exists");
    iface->AddRef();
    device = std::make_unique<D9CDevice>(iface);
  }
  ~LocalDevice() {
    lifetime.reset(); // All test consumers are synchronous and already joined.
    device.reset();
    if (iface) iface->Release();
    if (factory) factory->Release();
  }
  CommandQueue* queue = nullptr;
  com::IDirect3D9Ex* factory = nullptr;
  com::IDirect3DDevice9Ex* iface = nullptr;
  std::unique_ptr<D9CDevice> device{};
  std::shared_ptr<void> lifetime = std::make_shared<int>(0);
};

void setup(RenderTapeExecutionV4&, const AdmissionFixture&);

void controlledCpuFiniteTransitions() {
  using Phase = RenderTapeControlledCpuV4Phase;
  using Action = RenderTapeControlledCpuV4Action;
  for (std::uint32_t phaseIndex = 0u; phaseIndex < 4u; ++phaseIndex) {
    for (std::uint32_t actionIndex = 0u; actionIndex < 3u; ++actionIndex) {
      for (std::uint32_t mask = 0u; mask < 32u; ++mask) {
        const auto original = static_cast<Phase>(phaseIndex);
        auto phase = original;
        const auto action = static_cast<Action>(actionIndex);
        const RenderTapeControlledCpuV4Facts facts{
            bool(mask & 1u), bool(mask & 2u), bool(mask & 4u), bool(mask & 8u), bool(mask & 16u)};
        const bool expected = action == Action::Revoke ? original != Phase::Revoked :
            (action == Action::Bind ? original == Phase::Available && (mask & 15u) == 15u :
                                     original == Phase::Bound && mask == 31u);
        check(renderTapeControlledCpuV4Advance(phase, action, facts) == expected &&
            phase == (expected ? (action == Action::Bind ? Phase::Bound :
                action == Action::Finish ? Phase::Finalized : Phase::Revoked) : original),
            "exhaustive shared authority transition preserves rejected state and every finite gate");
        if (expected && action != Action::Revoke)
          check(!renderTapeControlledCpuV4Advance(phase, action, facts),
              "successful bind/finish is exactly once, not a reusable grant");
      }
    }
  }
}

bool liveControlledLease(const RenderTapeControlledCpuV4Lease& lease) {
  return lease.visitBackend(RenderTapeControlledCpuV4Boundary::Replay, nullptr,
      [](void*, D9CDevice*, Device&) noexcept { return true; });
}

void controlledCpuCapabilityNegatives() {
  using Access = CommandQueueArenaLeaseTestAccess;
  LocalDevice local;
  LocalDevice foreign;
  Access::Isolation isolation{local.queue};
  auto mint = [&]() {
    return Access::mint(local.device.get(), local.device->dev().upperDevice(), local.lifetime, isolation);
  };
  auto capability = mint();
  check(liveControlledLease(capability.lease()), "trusted inert fixture mints a live backend-pinned weak lease");
  const auto retainedLease = capability.lease();
  auto moved = std::move(capability);
  check(!liveControlledLease(capability.lease()) && liveControlledLease(moved.lease()) &&
      liveControlledLease(retainedLease), "moved-from cap has no authority while existing weak consumers follow moved owner");
  moved.revoke();
  check(!liveControlledLease(moved.lease()) && !liveControlledLease(retainedLease),
      "explicit revocation invalidates capability and all retained weak consumers");

  isolation.allowed = false;
  auto disallowed = mint();
  check(!liveControlledLease(disallowed.lease()), "fixture isolation rejection cannot mint queue authority");
  isolation.allowed = true;
  local.device->replayOffload = std::make_unique<ReplayOffloadWorker>();
  auto offloaded = mint();
  check(!liveControlledLease(offloaded.lease()), "a live offload owner rejects mint even when worker probe accepts");
  local.device->replayOffload.reset();

  std::weak_ptr<void> expired;
  const auto probes = isolation.probes;
  auto stale = Access::mint(reinterpret_cast<D9CDevice*>(std::uintptr_t{1u}),
      local.device->dev().upperDevice(), expired, isolation);
  check(!liveControlledLease(stale.lease()) && isolation.probes == probes,
      "expired fixture rejects before borrowed wrapper or isolation context access");

  for (std::uint32_t variant = 0u; variant < 5u; ++variant) {
    auto fixture = makeFixture();
    RenderTapeExecutionV4 execution;
    RenderTapeExecutionV4Evidence evidence;
    check(execution.admit(fixture.plan, fixture.replay, fixture.tape, evidence,
        RenderTapeExecutionV4PublicationMode::SemanticMapping), "controlled negative authenticates full V4 input pair");
    setup(execution, fixture);
    auto localCapability = mint();
    RenderTapeControlledCpuV4Capability owner;
    if (variant == 0u) localCapability.revoke();
    if (variant == 1u) owner = std::move(localCapability);
    if (variant == 2u) local.device->replayOffload = std::make_unique<ReplayOffloadWorker>();
    if (variant == 3u) isolation.allowed = false;
    check(!execution.armControlledCpu(variant == 4u ? foreign.device.get() : local.device.get(),
            localCapability) && !evidence.armed && !evidence.localArmAttempted &&
        !acquireRenderTapeRuntimeIdentityV4(local.device.get()) && !local.queue->acquireTapeRuntimeCapture(),
        "revoked/moved/offload/isolation/foreign authority rejects before actual arm or interval effects");
    local.device->replayOffload.reset();
    isolation.allowed = true;
    narrow(evidence);
  }
}

void setup(RenderTapeExecutionV4& execution, const AdmissionFixture& fixture) {
  for (std::uint32_t index = 0u; index < fixture.prefix; ++index)
    check(execution.beginEvent(index) && execution.replayedEvent(index),
        "caller acknowledges setup-only semantic prefix before real queue arm");
}

void preparedAdmissionProgressedState() {
  LocalDevice local;
  for (std::uint32_t fault = 0u; fault < 3u; ++fault) {
    auto fixture = makeFixture();
    RenderTapeExecutionV4Evidence evidence;
    RenderTapeExecutionV4 execution;
    check(fixture.prefix != 0u &&
        execution.admit(fixture.plan, fixture.replay, fixture.tape, evidence),
        "progressed prepared-claim negatives begin with genuine admitted setup prefix");
    if (fault == 0u)
      check(execution.beginEvent(0u), "open-prefix negative advances the actual executor event state");
    else
      setup(execution, fixture);
    if (fault == 2u)
      check(execution.arm(local.device.get()) && evidence.armed,
          "armed prepared-claim negative owns a genuine inert queue capture, not an authored armed bit");
    const auto root = acquireRenderTapeRuntimeIdentityV4(local.device.get());
    const auto replayedEvents = evidence.replayedEvents;
    const auto progress = local.queue->queryProgress();
    check(!RenderTapeExecutionV4PreparedReplayAccess::consume(execution,
            fixture.plan, fixture.replay, fixture.tape, evidence,
            RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic, ProducerContract::None) &&
        execution.failed() && evidence.admissionCount == 1u && !evidence.preparedAdmissionConsumed &&
        evidence.replayedEvents == replayedEvents && evidence.dispatchedRecords == 0u &&
        evidence.acknowledgedRaws == 0u && !evidence.complete &&
        evidence.actualIdentity.attempts.empty() &&
        acquireRenderTapeRuntimeIdentityV4(local.device.get()) == root &&
        local.queue->queryProgress().published == progress.published &&
        local.queue->queryProgress().completed == progress.completed,
        "open/completed prefix or real arm cannot be claimed, replayed, published or detached by handoff rejection");
    narrow(evidence);
    execution.cancel();
    check(!acquireRenderTapeRuntimeIdentityV4(local.device.get()) && (!root || !root->edges().active()),
        "explicit recovery cancellation still retires only the retained genuine arm after claim rejection");
  }
}

void beginState(RenderTapeExecutionV4& execution, const AdmissionFixture& fixture) {
  check(fixture.stateEvent == fixture.prefix && execution.beginEvent(fixture.stateEvent),
      "state-only event begins at first actual original boundary");
  const auto& chunk = fixture.replay.commandChunks[fixture.plan.events()[fixture.stateEvent].commandChunkIndex];
  check(execution.bindCommandChunk(chunk.bytes, chunk.envelope, {}), "exact state-only chunk binds without wrapper borrows");
}

ProviderReplayObservationV4Result replayState(RenderTapeExecutionV4& execution,
    const AdmissionFixture& fixture, D9CDevice* device) {
  beginState(execution, fixture);
  const auto observation = execution.observation();
  const auto& chunk = fixture.replay.commandChunks[fixture.plan.events()[fixture.stateEvent].commandChunkIndex];
  ProviderReplayObservationV4Result result;
  check(replayPrevalidatedResolvedCommandChunkObservedV4(device, chunk.bytes, chunk.envelope,
      {}, ProviderReplayPolicy::Compatibility, observation, result) == core::D3D_OK &&
      result.captureAccepted && result.qualificationSucceeded && result.ledgerSettlementObserved &&
      result.localRawOrdinal && !execution.failed(), "genuine production state replay supplies actual commit and raw ACK");
  return result;
}

void emptyControlledOutput(const RenderTapeControlledCpuV4Evidence& evidence) {
  check(evidence.domain == RenderTapeControlledCpuV4Evidence::Domain::None &&
      !evidence.controlledCpuComplete && !evidence.canonicalTraversalComplete &&
      !evidence.rawAckCoverageComplete && !evidence.semanticMappingComplete &&
      !evidence.semanticMapping && evidence.actualIdentity.sources.empty() &&
      evidence.actualIdentity.attempts.empty() && evidence.actualIdentity.dispositions.empty() &&
      evidence.actionWitness == RenderTapeActionWitnessV1{},
      "failed typed CPU output is empty, without partial domain/mapping/success authority");
}

void controlledCpuArmAndDomainGuards() {
  using Access = CommandQueueArenaLeaseTestAccess;
  for (std::uint32_t variant = 0u; variant < 6u; ++variant) {
    LocalDevice local;
    LocalDevice foreign;
    Access::Isolation isolation{local.queue}, foreignIsolation{foreign.queue};
    auto capability = Access::mint(local.device.get(), local.device->dev().upperDevice(),
        local.lifetime, isolation);
    auto other = Access::mint(foreign.device.get(), foreign.device->dev().upperDevice(),
        foreign.lifetime, foreignIsolation);
    auto fixture = makeFixture();
    RenderTapeExecutionV4 execution;
    RenderTapeExecutionV4Evidence evidence;
    check(execution.admit(fixture.plan, fixture.replay, fixture.tape, evidence,
        RenderTapeExecutionV4PublicationMode::SemanticMapping), "controlled arm authenticates unchanged full V4 components");
    setup(execution, fixture);
    check(execution.armControlledCpu(local.device.get(), capability) && evidence.armed &&
        local.device->renderTapeRuntimeBindingV4->domain == RenderTapeRuntimeBindingV4Domain::ControlledCpu,
        "typed arm binds real queue baseline to explicit controlled domain and exact live fixture");
    const auto root = acquireRenderTapeRuntimeIdentityV4(local.device.get());
    const auto& arm = evidence.localArm;
    D9CRenderTapeIdentityCaptureV4Result receipt{};
    check(dxmt9c_device_finish_render_tape_identity_capture_v4(local.device.get(),
            arm.captureToken, arm.captureEpoch, arm.queue, fixture.plan.identity().header.presentOrdinal,
            &receipt, nullptr, 0u) == core::D3DERR_NOTAVAILABLE &&
        receipt.status == D9C_RENDER_TAPE_IDENTITY_CAPTURE_FAILED && !receipt.byteCount &&
        !root->frozen() && root->edges().active() && !local.device->renderTapeRuntimeBindingV4->actionWitnessFrozen &&
        !local.device->renderTapeRuntimeBindingV4->receipt && local.device->renderTapeRuntimeBindingV4->transfer.empty(),
        "public full finish rejects CPU domain before endpoint retirement/freeze/paired transport, even empty topology");
    RenderTapeControlledCpuV4Evidence output;
    output.domain = RenderTapeControlledCpuV4Evidence::Domain::ControlledCpu;
    output.controlledCpuComplete = true; // Prior caller value must be erased on rejection.
    if (variant == 0u) {
      check(!execution.finish() && !root->frozen(), "production executor finish never relaxes topology for CPU authority");
      check(!execution.finishControlledCpu(capability, output), "failed production cross-domain attempt grants no CPU success");
    } else if (variant == 1u) {
      check(!execution.finishControlledCpu(other, output) && !root->frozen(),
          "foreign capability cannot finalize another queue/root binding");
    } else if (variant == 2u) {
      capability.revoke();
      check(!execution.beginEvent(fixture.stateEvent) && !execution.finishControlledCpu(capability, output),
          "revoked capability cannot cross another canonical or finalization boundary");
    } else if (variant == 3u) {
      local.lifetime.reset();
      local.device.reset();
      check(!execution.beginEvent(fixture.stateEvent) && !execution.finishControlledCpu(capability, output),
          "expired fixture is checked before a previously borrowed destroyed device wrapper");
    } else if (variant == 4u) {
      replayState(execution, fixture, local.device.get());
      std::size_t cursor = 0u;
      std::vector<core::metalqueue::ReplaySourcePublicationReceipt> publications;
      check(execution.publishedSources(cursor, publications) && !cursor && publications.empty() &&
          !execution.finishControlledCpu(capability, output),
          "real state ACK invents no publication; missing later canonical/GPU work cannot finalize provenance");
    } else {
      dxmt9c_device_cancel_render_tape_identity_capture_v4(local.device.get(),
          arm.captureToken, arm.captureEpoch, arm.queue);
      D9CRenderTapeIdentityCaptureV4ArmResult replacement{};
      check(dxmt9c_device_begin_render_tape_identity_capture_v4(local.device.get(),
          arm.captureToken, arm.captureEpoch, &replacement) == core::D3D_OK,
          "replacement trace installs a distinct production root with the same labels");
      check(!execution.beginEvent(fixture.stateEvent) && !execution.finishControlledCpu(capability, output),
          "same-key replacement invalidates exact CPU root authority");
      execution.cancel();
      check(acquireRenderTapeRuntimeIdentityV4(local.device.get()) != root &&
          bool(local.queue->acquireTapeRuntimeCapture()), "stale CPU cancellation cannot detach replacement production endpoint");
      dxmt9c_device_cancel_render_tape_identity_capture_v4(local.device.get(),
          replacement.captureToken, replacement.captureEpoch, replacement.queue);
    }
    emptyControlledOutput(output);
    execution.cancel();
    check(!evidence.complete && !evidence.actualLifecycleComplete && !evidence.actualTopologyComplete &&
        !evidence.semanticMappingComplete && !evidence.semanticMapping,
        "controlled path never promotes existing production completion/topology/semantic flags");
    narrow(evidence);
  }
}

using OwnedQuery = std::unique_ptr<D9CQuery, decltype(&dxmt9c_query_release)>;

OwnedQuery localEvent(LocalDevice& local, bool setupEnd, std::uint32_t type = 8u) {
  OwnedQuery query(dxmt9c_device_create_query(local.device.get(), type), dxmt9c_query_release);
  check(bool(query), "query replay maps a real registry-owned frontend EVENT");
  if (setupEnd)
    check(dxmt9c_query_issue(query.get(), 1u) == core::D3D_OK,
        "completed checkpoint restoration performs one actual pre-arm END");
  return query;
}

void querySetup(RenderTapeExecutionV4& execution, const AdmissionFixture& fixture,
    LocalDevice& local, D9CQuery* query) {
  for (std::uint32_t index = 0u; index < fixture.prefix; ++index) {
    check(execution.beginEvent(index), "query setup preserves every authenticated prefix event");
    if (index == fixture.queryDefinitionEvent)
      check(execution.bindQueryObject(local.device.get(), fixture.queryIdentity, query),
          "prefix ObjectDefine activates the exact completed local wrapper once");
    check(execution.replayedEvent(index), "query prefix event completes only after required activation");
  }
}

void immutablePreparationQueryIsolation() {
  using Access = RenderTapeExecutionV4PreparedReplayAccess;
  LocalDevice local;
  auto fixture = makeFixture(false, false, false, false, false,
      QueryFixture::CheckpointLazy, true);
  RenderTapeExecutionV4Preparation preparation;
  const char* failure = "none";
  check(Access::prepare(preparation, fixture.plan, fixture.replay, fixture.tape,
          RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic,
          ProducerContract::PePresentReservationV1, failure),
      "one immutable query/Present template prepares before any local binding");
  const auto capturedDigest = fixture.plan.digest();
  std::array<RenderTapeExecutionV4Evidence, 3u> evidence;
  std::array<RenderTapeExecutionV4, 3u> executions;
  D9CWireObjectIdentity priorLocalQuery{};
  std::uint64_t priorArmToken = 0u;
  for (std::size_t invocation = 0u; invocation < executions.size(); ++invocation) {
    auto& execution = executions[invocation];
    auto& observed = evidence[invocation];
    check(Access::initialize(execution, preparation, fixture.plan, fixture.replay,
            fixture.tape, observed, RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic,
            ProducerContract::PePresentReservationV1) &&
        RenderTapeExecutionV4TestAccess::freshMutableState(execution) &&
        observed.queryBaselineExpected == 1u && !observed.queryObjectsMapped &&
        !observed.queryBaselinesJoined && !observed.localArmAttempted &&
        !observed.presentPreparationAttempted && observed.admissionCount == 1u,
        "each invocation starts with fresh cursor/query/Present/root/actual evidence after earlier failure or cancellation");
    const auto* initial = RenderTapeExecutionV4TestAccess::localQueryFact(execution);
    check(initial && renderTapeIdentityV3SameIdentity(initial->queryIdentity, fixture.queryIdentity) &&
        initial->issueCycle == 2u && initial->issueOrdinal == 2u,
        "fresh local record template retains unbound captured identity and baseline-relative cycle, not prior actual labels");
    if (invocation != 0u)
      check(RenderTapeExecutionV4TestAccess::sharedImmutableTables(executions[0], execution) &&
          RenderTapeExecutionV4TestAccess::disjointMutableStorage(executions[0], execution),
          "later invocation borrows identical immutable storage but never shares mutable query or translated record vectors");
    noEffects(observed);
    check(Access::consume(execution, fixture.plan, fixture.replay, fixture.tape, observed,
            RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic,
            ProducerContract::PePresentReservationV1), "each fresh invocation owns its separate one-shot claim");
    if (invocation == 1u) {
      check(!execution.arm(nullptr) && execution.failed(),
          "middle invocation fails before setup/arm effects without damaging immutable templates");
      noEffects(observed);
      execution.cancel();
      continue;
    }
    auto query = localEvent(local, true);
    const auto setupSnapshot = local.device->dev().captureQueryPollSnapshot(query->obj);
    querySetup(execution, fixture, local, query.get());
    const std::array seeds{RenderTapeExecutionV4QueryBaselineInput{
        fixture.queryIdentity, query.get(), setupSnapshot}};
    check(execution.arm(local.device.get(), seeds) && observed.queryBaselinesJoined == 1u &&
        observed.queryObjectsMapped == 1u && observed.localArm.captureToken > priorArmToken,
        "fresh actual checkpoint binding and arm use this invocation's query wrapper and independent root key");
    priorArmToken = observed.localArm.captureToken;
    replayState(execution, fixture, local.device.get());
    check(execution.beginEvent(fixture.queryDefinitionEvent) &&
        execution.bindQueryObject(local.device.get(), fixture.queryIdentity, query.get()) &&
        execution.replayedEvent(fixture.queryDefinitionEvent) &&
        execution.beginEvent(fixture.queryIssueEvent),
        "immutable-template invocation activates genuine local checkpoint at original definition/Issue order");
    const auto& chunk = fixture.replay.commandChunks[
        fixture.plan.events()[fixture.queryIssueEvent].commandChunkIndex];
    const std::array<void*, 1u> objects{query.get()};
    ProviderReplayObservationV4Result observation;
    check(execution.bindCommandChunk(chunk.bytes, chunk.envelope, objects) &&
        replayPrevalidatedResolvedCommandChunkObservedV4(local.device.get(), chunk.bytes,
            chunk.envelope, objects, ProviderReplayPolicy::Compatibility,
            execution.observation(), observation) == core::D3D_OK &&
        observation.qualificationSucceeded && observed.actualIdentity.coordinators.size() == 1u,
        "real local END mutates this invocation's binding and record projection, not an authored test bit");
    const auto* translated = RenderTapeExecutionV4TestAccess::localQueryFact(execution);
    check(translated && renderTapeIdentityV3SameIdentity(translated->queryIdentity, query->wireIdentity) &&
        !renderTapeIdentityV3SameIdentity(translated->queryIdentity, fixture.queryIdentity) &&
        translated->issueCycle == 2u && translated->issueOrdinal == 2u &&
        (invocation == 0u || !renderTapeIdentityV3SameIdentity(query->wireIdentity, priorLocalQuery)) &&
        fixture.plan.digest() == capturedDigest,
        "actual translated identity changes locally while reusable captured query templates and plan digest remain immutable");
    priorLocalQuery = query->wireIdentity;
    check(!execution.finish() && !observed.complete,
        "missing later genuine traversal fails finalization without manufacturing completion or Present preparation");
    execution.cancel();
    check(!acquireRenderTapeRuntimeIdentityV4(local.device.get()) && preparation.valid(),
        "failed actual invocation cancels its own root without invalidating reusable preparation");
    narrow(observed);
  }
}

void queryJoinsAndLocalFacts() {
  for (auto shape : {QueryFixture::CheckpointPrefix, QueryFixture::CheckpointLazy, QueryFixture::Fresh,
                    QueryFixture::ClosedHistory, QueryFixture::ClosedOcclusion}) {
    LocalDevice local;
    auto fixture = makeFixture(false, false, false, false, false, shape);
    const bool checkpoint = shape == QueryFixture::CheckpointPrefix || shape == QueryFixture::CheckpointLazy;
    const std::uint32_t type = shape == QueryFixture::ClosedOcclusion ? 9u : 8u;
    const std::uint32_t flags = shape == QueryFixture::ClosedOcclusion ? 2u : 1u;
    auto query = localEvent(local, checkpoint, type);
    const auto setupSnapshot = local.device->dev().captureQueryPollSnapshot(query->obj);
    const auto digest = fixture.plan.digest();
    const auto capturedFact = std::find_if(fixture.plan.records().begin(), fixture.plan.records().end(),
        [](const auto& fact) { return fact.recordType == D9C_COMMAND_RECORD_QUERY_ISSUE; });
    check(capturedFact != fixture.plan.records().end(), "authenticated Tape owns the captured query fact");
    const auto capturedCycle = capturedFact->issueCycle, capturedOrdinal = capturedFact->issueOrdinal;
    RenderTapeExecutionV4 execution;
    RenderTapeExecutionV4Evidence evidence;
    check(execution.admit(fixture.plan, fixture.replay, fixture.tape, evidence) &&
        evidence.queryBaselineExpected == (checkpoint ? 1u : 0u),
        "admission counts authenticated completed checkpoints, not all query definitions");
    querySetup(execution, fixture, local, query.get());
    const std::array seeds{RenderTapeExecutionV4QueryBaselineInput{
        fixture.queryIdentity, query.get(), setupSnapshot}};
    check(execution.arm(local.device.get(), checkpoint
            ? std::span<const RenderTapeExecutionV4QueryBaselineInput>(seeds)
            : std::span<const RenderTapeExecutionV4QueryBaselineInput>{}) &&
        evidence.queryBaselinesJoined == (checkpoint ? 1u : 0u) &&
        evidence.queryObjectsMapped == (checkpoint ? 1u : 0u),
        "all checkpoint owners join before atomic arm, including hidden lazy definitions");
    replayState(execution, fixture, local.device.get());
    if (fixture.queryDefinitionEvent >= fixture.prefix) {
      check(execution.beginEvent(fixture.queryDefinitionEvent) &&
          execution.bindQueryObject(local.device.get(), fixture.queryIdentity, query.get()) &&
          execution.replayedEvent(fixture.queryDefinitionEvent),
          "authentic lazy ObjectDefine activates seeded checkpoint or maps fresh query without an extra END");
    }
    check(evidence.queryObjectsMapped == 1u &&
        local.device->dev().captureQueryPollSnapshot(query->obj).issueOrdinal == (checkpoint ? 1u : 0u),
        "activation counts each query once and does not mutate its real issue history");
    check(execution.beginEvent(fixture.queryIssueEvent), "first authenticated query Issue follows activation");
    const auto& chunk = fixture.replay.commandChunks[
        fixture.plan.events()[fixture.queryIssueEvent].commandChunkIndex];
    const std::array<void*, 1u> objects{query.get()};
    check(execution.bindCommandChunk(chunk.bytes, chunk.envelope, objects),
        "captured query handle resolves only through the admitted physical local mapping");
    ProviderReplayObservationV4Result observed;
    check(replayPrevalidatedResolvedCommandChunkObservedV4(local.device.get(), chunk.bytes,
        chunk.envelope, objects, ProviderReplayPolicy::Compatibility, execution.observation(), observed) ==
            core::D3D_OK && observed.qualificationSucceeded && !execution.failed(),
        "actual query END replay observes adjacent local ordinal instead of borrowed captured labels");
    check(evidence.actualIdentity.coordinators.size() == 1u,
        "only the interval Issue creates a coordinator, never the checkpoint join or definition");
    const auto& actual = evidence.actualIdentity.coordinators[0].provenance;
    RenderTapeQueryCycleState cycle{.type = type};
    if (checkpoint) check(renderTapeAdvanceQueryCycle(cycle, 1u, false), "pure local setup transition succeeds");
    check(renderTapeAdvanceQueryCycle(cycle, flags, true) && actual.issueCycle == cycle.issueCycle &&
        actual.issueOrdinal == cycle.issueOrdinal &&
        renderTapeIdentityV3SameIdentity(actual.queryIdentity, query->wireIdentity) &&
        !renderTapeIdentityV3SameIdentity(actual.queryIdentity, fixture.queryIdentity) &&
        evidence.actualIdentity.sources.empty() && evidence.actualIdentity.initializers.empty(),
        "local provenance preserves actual wire identity, pure cycle/ordinal, and strict empty END extent");
    check(fixture.plan.digest() == digest && capturedFact->issueCycle == capturedCycle &&
        capturedFact->issueOrdinal == capturedOrdinal &&
        renderTapeIdentityV3SameIdentity(capturedFact->queryIdentity, fixture.queryIdentity) &&
        (!checkpoint || (capturedCycle == 32u && capturedOrdinal == 48u && actual.issueCycle == 2u &&
                        actual.issueOrdinal == 2u)),
        "local projection never overwrites immutable authenticated captured facts or actual evidence");
    if (shape == QueryFixture::ClosedHistory || shape == QueryFixture::ClosedOcclusion)
      check(capturedCycle == 509u && capturedOrdinal == 1017u &&
          actual.issueCycle == 1u && actual.issueOrdinal == 1u && !evidence.queryBaselinesJoined,
          "closed history maps to a fresh actual query with no synthetic setup Issue or old completion authority");
    // Source rows below are an explicitly authored neutral comparison graph,
    // not reported actual replay evidence. Only its query labels are derived
    // from the genuine observed local END; all physical extents stay exact.
    const auto genuineActual = evidence.actualIdentity;
    for (std::uint32_t mutation = 0u; mutation < 8u; ++mutation) {
      RenderTapeExecutionV4Evidence comparison;
      comparison.actualIdentity = fixture.plan.identity();
      auto& row = comparison.actualIdentity.coordinators[0].provenance;
      row.queryIdentity = actual.queryIdentity;
      row.issueCycle = actual.issueCycle;
      row.issueOrdinal = actual.issueOrdinal;
      row.deviceEpoch = actual.deviceEpoch;
      if (mutation == 1u) ++row.issueOrdinal;
      if (mutation == 2u) ++row.issueCycle;
      if (mutation == 3u) ++row.queryIdentity.generation;
      if (mutation == 4u) ++comparison.actualIdentity.sources[0].lifecycle.cutEventOrdinal;
      if (mutation == 5u) row.sourceIndex = 0u;
      if (mutation == 6u) ++comparison.actualIdentity.commands[0].drawCount;
      if (mutation == 7u) ++row.commandEnd;
      const auto valid = RenderTapeExecutionV4TestAccess::validateLocalRows(execution, comparison.actualIdentity);
      if (mutation == 0u)
        check(valid == RenderTapeIdentityV4Status::Valid,
            "actual-row validator accepts production-derived local facts for captured32/48 to local2/2 mapping");
      if (mutation >= 1u && mutation <= 3u)
        check(valid == RenderTapeIdentityV4Status::InvalidCoordinator,
            "actual-row validator rejects wrong mapped local ordinal, cycle or wire identity");
      check(RenderTapeExecutionV4TestAccess::qualifyExactFixture(execution, comparison) == (mutation == 0u),
          "production exact comparator admits only mapped query labels and retains strict source/prefix/END/physical extents");
      check(!comparison.complete && !comparison.armed && !comparison.exactSourceDiagnosticComplete,
          "cold comparison fixture never promotes actual replay completion evidence");
    }
    check(evidence.actualIdentity.sources.size() == genuineActual.sources.size() &&
        evidence.actualIdentity.coordinators.size() == genuineActual.coordinators.size() &&
        evidence.actualIdentity.coordinators[0].provenance.issueOrdinal == actual.issueOrdinal &&
        evidence.actualIdentity.coordinators[0].provenance.issueCycle == actual.issueCycle &&
        fixture.plan.digest() == digest && capturedFact->issueOrdinal == capturedOrdinal,
        "comparator seam preserves genuine observed evidence and authenticated capture ownership");
    execution.cancel();
    narrow(evidence);
  }
}

void queryMappingRejections() {
  for (std::uint32_t variant = 0u; variant < 8u; ++variant) {
    LocalDevice local;
    auto fixture = makeFixture(false, false, false, false, false, QueryFixture::CheckpointLazy);
    auto query = localEvent(local, true);
    auto other = localEvent(local, true);
    const auto snapshot = local.device->dev().captureQueryPollSnapshot(query->obj);
    RenderTapeExecutionV4 execution;
    RenderTapeExecutionV4Evidence evidence;
    check(execution.admit(fixture.plan, fixture.replay, fixture.tape, evidence), "query rejection pair authenticates");
    querySetup(execution, fixture, local, query.get());
    std::vector<RenderTapeExecutionV4QueryBaselineInput> seeds{{fixture.queryIdentity, query.get(), snapshot}};
    if (variant == 0u) seeds.clear();
    if (variant == 1u) seeds.push_back(seeds[0]);
    if (variant == 2u) ++seeds[0].capturedIdentity.generation;
    if (variant == 3u) seeds[0].query = other.get();
    if (variant == 4u)
      check(dxmt9c_query_issue(query.get(), 1u) == core::D3D_OK, "stale join has a genuine intervening reissue");
    if (variant <= 4u) {
      check(!execution.arm(local.device.get(), seeds) && !evidence.armed &&
          !evidence.queryBaselinesJoined && !evidence.actualAttemptsObserved &&
          !local.queue->acquireTapeRuntimeCapture() && !acquireRenderTapeRuntimeIdentityV4(local.device.get()),
          "missing/duplicate/unauthenticated/wrong-owner/stale seed rejects before interval effects");
      continue;
    }
    check(execution.arm(local.device.get(), seeds), "negative lazy mapping independently seeds its root");
    replayState(execution, fixture, local.device.get());
    check(execution.beginEvent(fixture.queryDefinitionEvent), "negative trace reaches authentic lazy definition");
    if (variant == 5u) {
      check(!execution.replayedEvent(fixture.queryDefinitionEvent) &&
          std::string_view(execution.failure()) == "v4-query-definition-not-activated",
          "canonical ObjectDefine cannot be acknowledged without exact physical query activation");
    } else if (variant == 6u) {
      check(!execution.bindQueryObject(local.device.get(), fixture.queryIdentity, other.get()),
          "same-type replacement wrapper cannot activate the authenticated seeded query");
    } else {
      check(execution.bindQueryObject(local.device.get(), fixture.queryIdentity, query.get()) &&
          !execution.bindQueryObject(local.device.get(), fixture.queryIdentity, query.get()) &&
          evidence.queryObjectsMapped == 1u,
          "duplicate lazy activation fails without double counting checkpoint mapping");
    }
    execution.cancel();
  }

  for (bool wrongGeneration : {false, true}) {
    LocalDevice local;
    auto fixture = makeFixture(false, false, false, false, false, QueryFixture::Fresh);
    auto query = localEvent(local, false);
    auto other = localEvent(local, false);
    RenderTapeExecutionV4 execution;
    RenderTapeExecutionV4Evidence evidence;
    check(execution.admit(fixture.plan, fixture.replay, fixture.tape, evidence), "fresh query Tape authenticates");
    setup(execution, fixture);
    check(execution.arm(local.device.get()), "empty checkpoint batch enables strict fresh-query replay");
    replayState(execution, fixture, local.device.get());
    check(execution.beginEvent(fixture.queryDefinitionEvent) &&
        execution.bindQueryObject(local.device.get(), fixture.queryIdentity, query.get()) &&
        execution.replayedEvent(fixture.queryDefinitionEvent) && execution.beginEvent(fixture.queryIssueEvent),
        "fresh query maps at original definition before its first effect");
    const auto savedIdentity = query->wireIdentity;
    if (wrongGeneration) ++query->wireIdentity.generation;
    const std::array<void*, 1u> objects{wrongGeneration ? query.get() : other.get()};
    const auto& chunk = fixture.replay.commandChunks[fixture.plan.events()[fixture.queryIssueEvent].commandChunkIndex];
    check(!execution.bindCommandChunk(chunk.bytes, chunk.envelope, objects) &&
        evidence.acknowledgedRaws == 1u && evidence.actualIdentity.coordinators.empty() &&
        local.device->dev().captureQueryPollSnapshot(query->obj).issueOrdinal == 0u,
        "wrong-generation/replacement command owner rejects before first fresh Issue effect");
    query->wireIdentity = savedIdentity;
    execution.cancel();
  }

  auto unsupported = makeFixture(false, false, false, false, false, QueryFixture::ClosedUnsupported);
  RenderTapeExecutionV4 execution;
  RenderTapeExecutionV4Evidence evidence;
  check(!execution.admit(unsupported.plan, unsupported.replay, unsupported.tape, evidence) &&
      std::string_view(execution.failure()) == "v4-query-initial-state-unsupported",
      "unsupported old closed-cycle query mapping rejects authentic input before any effect");
  noEffects(evidence);
}

void actualArmAndAck() {
  LocalDevice local;
  std::uint64_t previous = 0u;
  for (std::uint32_t repetition = 0u; repetition < 2u; ++repetition) {
    auto fixture = makeFixture(true);
    RenderTapeExecutionV4 execution;
    RenderTapeExecutionV4Evidence evidence;
    check(execution.admit(fixture.plan, fixture.replay, fixture.tape, evidence), "exact plan admits");
    if (repetition == 1u)
      check(RenderTapeExecutionV4PreparedReplayAccess::consume(execution, fixture.plan,
              fixture.replay, fixture.tape, evidence,
              RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic, ProducerContract::None) &&
          evidence.admissionCount == 1u && evidence.preparedAdmissionConsumed,
          "one-shot prepared claim preserves the real setup/arm/raw-callback path without re-admission");
    setup(execution, fixture);
    check(execution.arm(local.device.get()) && evidence.localArm.queue &&
        evidence.localArm.captureToken > previous && evidence.localArm.captureEpoch == evidence.localArm.captureToken,
        "executor allocates a never-reused local arm key and retains real queue receipt");
    previous = evidence.localArm.captureToken;
    const auto retained = acquireRenderTapeRuntimeIdentityV4(local.device.get());
    check(retained && retained->matches({evidence.localArm.captureToken, evidence.localArm.captureEpoch}),
        "actual retained root matches independent local arm rather than captured authority");
    replayState(execution, fixture, local.device.get());
    check(evidence.acknowledgedRaws == 1u && evidence.dispatchedRecords == 2u &&
        evidence.actualIdentity.attempts.size() == 1u && evidence.actualIdentity.settlements.size() == 1u &&
        evidence.actualIdentity.dispositions.size() == 2u && evidence.actualIdentity.sources.empty() &&
        local.device->dev().state().vsConst.float4[0u][0u] == 5.0f,
        "candidate real attempt does not repeat captured rollback or manufacture physical publication");
    check(execution.beginEvent(fixture.stateEvent + 1u) && evidence.replayedEvents == fixture.prefix + 1u,
        "canonical event cursor advances only after actual committed coverage and ledger ACK");
    narrow(evidence);
    check(!execution.finish() && !evidence.complete && local.queue->hasCompletedPendingWork() &&
        local.device->dev().state().vsConst.float4[0u][0u] == 5.0f,
        "missing later source/topology/completion fails without submitting or replaying missing work");
    execution.cancel();
    check(!acquireRenderTapeRuntimeIdentityV4(local.device.get()) && !retained->edges().active(),
        "cancellation retires only the retained arm");
  }
}

void failedArmEvidence() {
  LocalDevice local;
  const auto before = local.queue->inspectRenderTapeArmState();
  check(before && before->checkpoint.buildQuiescent && before->checkpoint.writingEmpty &&
      !before->initializerPresent && before->initializerProgressAvailable,
      "inert queue arm inspection is available without an initializer");
  SchedulingProgressTestAccess::installPendingInitializer(*local.queue);
  const auto pending = local.queue->inspectRenderTapeArmState();
  const auto repeated = local.queue->inspectRenderTapeArmState();
  check(pending && repeated && pending->initializerPresent && !pending->initializerProgressAvailable &&
      pending->checkpoint.initializerPending && repeated->checkpoint.initializerPending &&
      pending->checkpoint.nextSequence == before->checkpoint.nextSequence &&
      repeated->checkpoint.nextSequence == before->checkpoint.nextSequence &&
      pending->checkpoint.publishedSequence == before->checkpoint.publishedSequence &&
      repeated->checkpoint.publishedSequence == before->checkpoint.publishedSequence &&
      pending->checkpoint.completedSequence == before->checkpoint.completedSequence &&
      repeated->checkpoint.completedSequence == before->checkpoint.completedSequence &&
      repeated->checkpoint.inflightCount == before->checkpoint.inflightCount &&
      !local.queue->acquireTapeRuntimeCapture(),
      "cold arm inspection distinguishes pending uploads without publication, wait, reap or capture");
  auto fixture = makeFixture();
  RenderTapeExecutionV4 execution;
  RenderTapeExecutionV4Evidence evidence;
  check(execution.admit(fixture.plan, fixture.replay, fixture.tape, evidence),
      "failed arm fixture admits before setup");
  setup(execution, fixture);
  check(!execution.arm(local.device.get()) && execution.failed() && !evidence.armed &&
      std::string_view(execution.failure()) == "v4-local-arm-failed" &&
      evidence.localArmAttempted && evidence.localArmHresult == core::D3DERR_NOTAVAILABLE &&
      evidence.localArm.status == D9C_RENDER_TAPE_IDENTITY_CAPTURE_FAILED &&
      !evidence.localArm.version && !evidence.localArm.queue &&
      evidence.preArm && evidence.preArm->checkpoint.initializerPending &&
      !evidence.preArm->initializerProgressAvailable &&
      !acquireRenderTapeRuntimeIdentityV4(local.device.get()),
      "failed real arm preserves HRESULT/result and exact pending-upload evidence without owning a root");
  execution.cancel();
  narrow(evidence);
  check(evidence.replayedEvents == fixture.prefix && !evidence.dispatchedRecords &&
      !evidence.acknowledgedRaws && evidence.actualIdentity.attempts.empty() &&
      evidence.preArm && evidence.preArm->checkpoint.initializerPending &&
      evidence.localArmHresult == core::D3DERR_NOTAVAILABLE,
      "failed-arm cancellation preserves setup and diagnostic evidence without replay effects");
}

void setupCheckpointBoundary() {
  for (const bool exact : {false, true}) {
    for (const std::uint32_t prefix : {0u, 1u, 11u}) {
      std::size_t checkpoints = 0u;
      for (std::uint32_t event = 0u; event < prefix + 8u; ++event) {
        const auto selected = renderTapeV4SetupCheckpointRequired(exact, event, prefix);
        check(selected == (exact && event == prefix),
            "shared provider boundary admits one V4 setup checkpoint and no interval flush");
        checkpoints += selected ? 1u : 0u;
      }
      check(checkpoints == (exact ? 1u : 0u),
          "V3/non-V4 mode never selects the new setup completion step");
    }
    check(!renderTapeV4SetupCheckpointRequired(exact, kRenderTapeIdentityV4NoIndex,
        kRenderTapeIdentityV4NoIndex), "missing admitted prefix cannot select a checkpoint");
  }

  LocalDevice local;
  auto fixture = makeFixture();
  RenderTapeExecutionV4 execution;
  RenderTapeExecutionV4Evidence evidence;
  check(execution.admit(fixture.plan, fixture.replay, fixture.tape, evidence),
      "setup checkpoint fixture admits");
  setup(execution, fixture);
  const auto before = local.queue->inspectRenderTapeArmState();
  {
    const auto checkpoint = local.queue->beginRenderTapeCheckpoint();
    check(checkpoint && checkpoint->owns_lock(),
        "real inert setup checkpoint retains the queue lock through setup completion");
  }
  check(execution.arm(local.device.get()) && evidence.armed,
      "scoped setup checkpoint releases its lock before actual V4 arm");
  replayState(execution, fixture, local.device.get());
  const auto after = local.queue->inspectRenderTapeArmState();
  check(before && after && after->checkpoint.publishedSequence == before->checkpoint.publishedSequence &&
      after->checkpoint.completedSequence == before->checkpoint.completedSequence &&
      after->checkpoint.nextSequence == before->checkpoint.nextSequence &&
      evidence.actualIdentity.sources.empty() && evidence.actualIdentity.initializers.empty(),
      "setup completion and state-only interval replay invent no source or initializer publication");
  execution.cancel();
  narrow(evidence);
}

void setupCheckpointFailureGuards() {
  RenderTapeCheckpointCompletionState settled{
      .nextSequence = 1u, .initializerPublished = 2u, .initializerCompleted = 2u,
      .buildQuiescent = true, .writingEmpty = true, .initializerTrackingAvailable = true};
  check(renderTapeCheckpointCompletionAllowed(settled),
      "successful setup requires the current initializer completion prefix");
  auto stale = settled;
  stale.initializerCompleted = 1u;
  check(!renderTapeCheckpointCompletionAllowed(stale),
      "older initializer completion cannot settle a newer setup prefix");
  auto failed = settled;
  failed.initializerFailed = true;
  check(!renderTapeCheckpointCompletionAllowed(failed),
      "failed initializer cannot settle setup even when its signal matches publication");
  auto pending = settled;
  pending.initializerPending = true;
  check(!renderTapeCheckpointCompletionAllowed(pending),
      "unpublished setup uploads cannot hide behind previously completed initializer progress");

  LocalDevice local;
  SchedulingProgressTestAccess::installFailedInitializer(*local.queue);
  const auto before = local.queue->inspectRenderTapeArmState();
  check(!local.queue->beginRenderTapeCheckpoint(),
      "actual checkpoint rejects an already failed initializer before any completion fence");
  const auto after = local.queue->inspectRenderTapeArmState();
  check(before && after && after->checkpoint.nextSequence == before->checkpoint.nextSequence &&
      after->checkpoint.publishedSequence == before->checkpoint.publishedSequence &&
      after->checkpoint.completedSequence == before->checkpoint.completedSequence &&
      !acquireRenderTapeRuntimeIdentityV4(local.device.get()) && !local.queue->acquireTapeRuntimeCapture(),
      "failed setup checkpoint creates no source, completion or capture root");
}

void mismatchedZeroCoordinatorDomain() {
  LocalDevice local;
  auto fixture = makeFixture(false, false, false, false, true);
  check(fixture.plan.identity().coordinators.empty(), "domain mismatch fixture has no coordinator rows");
  RenderTapeExecutionV4 execution;
  RenderTapeExecutionV4Evidence evidence;
  check(execution.admit(fixture.plan, fixture.replay, fixture.tape, evidence),
      "captured session domain is independently valid without coordinator rows");
  setup(execution, fixture);
  check(!execution.arm(local.device.get()) && execution.failed() && !evidence.armed &&
      std::string_view(execution.failure()) == "v4-exact-coordinator-domain" &&
      evidence.actualIdentity.header.coordinatorDomain ==
          static_cast<std::uint32_t>(TapeCoordinatorAuthority::OrdinarySerial) &&
      evidence.actualIdentity.attempts.empty() && !evidence.dispatchedRecords && !evidence.acknowledgedRaws,
      "known incompatible exact domain rejects at actual arm before any candidate replay effect");
  execution.cancel();
  check(!acquireRenderTapeRuntimeIdentityV4(local.device.get()),
      "rejected exact-domain arm can retire only its own retained local capture");
  narrow(evidence);
}

void setupAndMutationNegatives() {
  for (std::uint32_t variant = 0u; variant < 7u; ++variant) {
    LocalDevice local;
    auto fixture = makeFixture();
    RenderTapeExecutionV4 execution;
    RenderTapeExecutionV4Evidence evidence;
    check(execution.admit(fixture.plan, fixture.replay, fixture.tape, evidence), "negative fixture admits");
    if (variant == 0u) {
      check(!execution.arm(local.device.get()) && !acquireRenderTapeRuntimeIdentityV4(local.device.get()),
          "real arm cannot skip original setup prefix");
      continue;
    }
    setup(execution, fixture);
    check(execution.arm(local.device.get()), "negative fixture independently arms");
    if (variant == 1u) {
      check(!execution.arm(local.device.get()), "duplicate executor arm fails closed");
    } else if (variant == 2u) {
      CaptureSourceReplayPlanV4 moved = std::move(fixture.plan);
      check(!execution.beginEvent(fixture.stateEvent), "moving admitted owner invalidates bound execution");
    } else if (variant == 3u) {
      check(!execution.beginEvent(fixture.stateEvent + 1u), "wrong original event order rejects");
    } else if (variant == 4u) {
      check(execution.beginEvent(fixture.stateEvent), "current event begins");
      const auto& chunk = fixture.replay.commandChunks[0];
      auto bytes = std::vector<std::byte>(chunk.bytes.begin(), chunk.bytes.end());
      bytes.back() ^= std::byte{1u};
      check(!execution.bindCommandChunk(bytes, chunk.envelope, {}), "changed canonical chunk is not admitted through borrowed views");
    } else {
      beginState(execution, fixture);
      const auto observation = execution.observation();
      if (variant == 5u)
        check(!observation.rawStarted(observation.context, observation.eventOrdinal + 1u, 1u, 2u),
            "wrong original raw event cannot authenticate callback traversal");
      else {
        check(observation.rawStarted(observation.context, observation.eventOrdinal, 1u, 2u) &&
            !observation.beforeRecord(observation.context, 0u, D9C_COMMAND_RECORD_SET_PS_CONST_F),
            "wrong canonical record type fails before candidate diagnostic dispatch");
      }
    }
    narrow(evidence);
  }
}

void missingPublicationAndDuplicateAck() {
  for (bool missing : {false, true}) {
    LocalDevice local;
    auto fixture = makeFixture(false, false, false, missing);
    RenderTapeExecutionV4 execution;
    RenderTapeExecutionV4Evidence evidence;
    check(execution.admit(fixture.plan, fixture.replay, fixture.tape, evidence), "publication negative admits");
    setup(execution, fixture);
    check(execution.arm(local.device.get()), "publication negative arms");
    beginState(execution, fixture);
    const auto observation = execution.observation();
    const auto& chunk = fixture.replay.commandChunks[0];
    ProviderReplayObservationV4Result result;
    check(replayPrevalidatedResolvedCommandChunkObservedV4(local.device.get(), chunk.bytes, chunk.envelope,
        {}, ProviderReplayPolicy::Compatibility, observation, result) == core::D3D_OK &&
        result.qualificationSucceeded && evidence.acknowledgedRaws == 1u, "real raw ACK precedes receipt expectations");
    if (missing)
      check(!execution.beginEvent(fixture.stateEvent + 1u) &&
          std::string_view(execution.failure()) == "v4-exact-initializer-publication" &&
          evidence.actualIdentity.initializers.empty() && evidence.initializers.empty(),
          "missing captured initializer receipt is rejected, never synthesized/published by executor");
    else
      check(!observation.rawSettled(observation.context, result.localRawOrdinal, 2u, true) &&
          evidence.acknowledgedRaws == 1u, "duplicate raw ACK cannot consume canonical coverage twice");
    narrow(evidence);
  }
}

void replacementRoot() {
  LocalDevice local;
  auto fixture = makeFixture();
  RenderTapeExecutionV4 execution;
  RenderTapeExecutionV4Evidence evidence;
  check(execution.admit(fixture.plan, fixture.replay, fixture.tape, evidence), "replacement fixture admits");
  setup(execution, fixture);
  check(execution.arm(local.device.get()), "old local root arms");
  const auto old = acquireRenderTapeRuntimeIdentityV4(local.device.get());
  const auto lease = local.queue->acquireTapeRuntimeCapture();
  const auto arm = evidence.localArm;
  dxmt9c_device_cancel_render_tape_identity_capture_v4(local.device.get(), arm.captureToken, arm.captureEpoch, arm.queue);
  D9CRenderTapeIdentityCaptureV4ArmResult replacement{};
  check(dxmt9c_device_begin_render_tape_identity_capture_v4(local.device.get(), 9001u, 9002u,
      &replacement) == core::D3D_OK, "independent public replacement has different local key/root");
  const auto fresh = acquireRenderTapeRuntimeIdentityV4(local.device.get());
  check(!execution.beginEvent(fixture.stateEvent) && old != fresh && !lease.active(),
      "executor refuses retired retained root instead of silently adopting replacement");
  execution.cancel();
  check(acquireRenderTapeRuntimeIdentityV4(local.device.get()) == fresh && fresh->edges().active() &&
      fresh->matches({9001u, 9002u}) && !fresh->failed(), "executor cancellation never cancels/fails replacement capture");
  dxmt9c_device_cancel_render_tape_identity_capture_v4(local.device.get(), 9001u, 9002u, replacement.queue);
}

void actualRollbackRetryAndSeparateAck(bool acknowledge) {
  LocalDevice local;
  auto fixture = makeFixture();
  RenderTapeExecutionV4 execution;
  RenderTapeExecutionV4Evidence evidence;
  check(execution.admit(fixture.plan, fixture.replay, fixture.tape, evidence), "retry fixture admits");
  setup(execution, fixture);
  check(execution.arm(local.device.get()), "retry fixture arms");
  beginState(execution, fixture);
  const auto observation = execution.observation();
  const auto root = acquireRenderTapeRuntimeIdentityV4(local.device.get());
  RawCommandChunk raw;
  raw.recordCount = 2u;
  check(local.device->replayDrainLedger.publishInline(raw) && raw.replaySeq &&
      observation.rawStarted(observation.context, observation.eventOrdinal, raw.replaySeq, 2u),
      "test seam starts only after genuine raw ledger admission");
  const ReplayTransactionRawIdentity identity{evidence.localArm.captureToken,
      observation.eventOrdinal, raw.replaySeq, 2u};
  {
    ScopedRenderTapeRuntimeReplayV4 scope(root, identity, true, local.queue->acquireTapeRuntimeCapture());
    const ReplaySourceIdentity projection{19u, 21u, 21u, raw.replaySeq};
    ReplayTransaction transaction;
    transaction.begin(projection, &local.device->dev());
    check(observation.beforeRecord(observation.context, 0u, D9C_COMMAND_RECORD_SET_VS_CONST_F) &&
        activeRenderTapeRuntimeReplayV4->beginRecord(0u, D9C_COMMAND_RECORD_SET_VS_CONST_F) &&
        transaction.project({.stateGeneration = 7u, .source = projection.source, .recordOrdinal = 0u}) &&
        observation.beforeDispatch(observation.context, 0u, D9C_COMMAND_RECORD_SET_VS_CONST_F) &&
        activeRenderTapeRuntimeReplayV4->beginDispatch() &&
        observation.recordDispatched(observation.context, 0u, D9C_COMMAND_RECORD_SET_VS_CONST_F, core::D3DERR_NOTAVAILABLE),
        "private pre-effect failed dispatch leaves reversible candidate, not invented failed-stop evidence");
    check(transaction.rollback(local.device->dev()), "real transaction owner rolls back reversible candidate");
    noteDestinationRollback(transaction, true);
    transaction.begin(projection, &local.device->dev());
    for (std::uint32_t index = 0u; index < 2u; ++index) {
      check(observation.beforeRecord(observation.context, index, D9C_COMMAND_RECORD_SET_VS_CONST_F) &&
          activeRenderTapeRuntimeReplayV4->beginRecord(index, D9C_COMMAND_RECORD_SET_VS_CONST_F) &&
          transaction.project({.stateGeneration = 8u, .source = projection.source, .recordOrdinal = index}) &&
          observation.beforeDispatch(observation.context, index, D9C_COMMAND_RECORD_SET_VS_CONST_F) &&
          activeRenderTapeRuntimeReplayV4->beginDispatch() &&
          activeRenderTapeRuntimeReplayV4->endRecord(core::D3D_OK) &&
          observation.recordDispatched(observation.context, index, D9C_COMMAND_RECORD_SET_VS_CONST_F, core::D3D_OK),
          "confirmed production rollback permits retry without rewinding canonical cursor");
    }
    check(transaction.receiveDestination({.kind = ReplayDestinationKind::Compatibility,
        .identity = projection, .queueSequence = raw.replaySeq, .controlIndex = UINT32_MAX}) &&
        transaction.commit(), "actual successful retry commits a real destination receipt");
    check(evidence.acknowledgedRaws == 0u && evidence.dispatchedRecords == 0u,
        "actual commit alone is not raw ACK or committed canonical cursor authority");
    if (acknowledge) {
      local.device->replayDrainLedger.publishReplayed(raw);
      check(!local.device->replayDrainLedger.terminal() && scope.acknowledge(true) &&
          observation.rawSettled(observation.context, raw.replaySeq, 2u, true),
          "separate real ledger ACK authenticates candidate rollback + committed retry coverage");
      check(evidence.actualIdentity.attempts.size() == 2u && evidence.acknowledgedRaws == 1u &&
          evidence.actualIdentity.attempts[0].outcome == static_cast<std::uint32_t>(Outcome::RolledBack) &&
          evidence.actualIdentity.attempts[1].outcome == static_cast<std::uint32_t>(Outcome::Committed),
          "candidate actual attempt history differs legally from captured successful-only history");
    } else {
      check(!observation.rawSettled(observation.context, raw.replaySeq, 2u, true) &&
          evidence.acknowledgedRaws == 0u && evidence.dispatchedRecords == 0u,
          "boolean success without actual root ledger ACK cannot authenticate a commit");
      local.device->replayDrainLedger.publishReplayed(raw);
      check(scope.acknowledge(true), "actual test raw subsequently settles independently of rejected qualification");
    }
  }
  if (acknowledge)
    check(execution.beginEvent(fixture.stateEvent + 1u), "actual rolled-back/retried raw consumes original actions once");
  narrow(evidence);
}

} // namespace

int main() {
  try {
    admission();
    preparedAdmissionFiniteTransitions();
    preparedAdmissionClaim();
    immutablePreparationAdmission();
    preparedAdmissionProgressedState();
    semanticAdmission();
    semanticCorrespondenceFixtures();
    semanticObligationNegatives();
    controlledCpuFiniteTransitions();
    controlledCpuCapabilityNegatives();
    controlledCpuArmAndDomainGuards();
    producerContractAdmission();
    presentPreparationTransitions();
    presentPreparationOrdering();
    exactGeneralBinding();
    actualArmAndAck();
    queryJoinsAndLocalFacts();
    immutablePreparationQueryIsolation();
    queryMappingRejections();
    failedArmEvidence();
    setupCheckpointBoundary();
    setupCheckpointFailureGuards();
    mismatchedZeroCoordinatorDomain();
    setupAndMutationNegatives();
    missingPublicationAndDuplicateAck();
    replacementRoot();
    actualRollbackRetryAndSeparateAck(true);
    actualRollbackRetryAndSeparateAck(false);
    std::cout << "render tape execution v4: " << checks << " checks\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
