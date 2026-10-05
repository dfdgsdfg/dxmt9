#include "render_tape_identity_v4_fixture.hpp"
#include "device_c_render_tape_provenance_validation.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace dxmt9::d3d9;
using Status = RenderTapeIdentityV4Status;
using Prefix = RenderTapeIdentityV3PrefixKind;
using Kind = RenderTapeIdentityV3DispositionKind;
using CommandKind = RenderTapeIdentityV3CommandKind;
using Cut = RenderTapeIdentityV3CutKind;
using Observation = RenderTapeIdentityV3Observation;
constexpr D9CWireObjectIdentity kQuery{D9C_CHUNK_HANDLE_KIND_QUERY, 3u, 42u};
constexpr D9CWireObjectIdentity kOutput{D9C_CHUNK_HANDLE_KIND_SURFACE, 1u, 51u};
constexpr auto kSubmit = static_cast<std::uint32_t>(
    dxmt9::core::metalqueue::SessionReleaseAction::SubmitSession);
std::uint64_t checks = 0u;

void check(bool value, std::string_view message) {
  ++checks;
  if (!value) throw std::runtime_error(std::string(message));
}

RenderTapeIdentityV4Header header(bool empty = true) {
  RenderTapeIdentityV4Header value;
  value.frameId = 3u; value.presentOrdinal = 9u;
  value.captureToken = 5u; value.captureEpoch = 6u; value.queue = 1u;
  value.baselineKind = static_cast<std::uint32_t>(
      empty ? Prefix::EmptyBaseline : Prefix::ArmBaseline);
  value.baselineSequence = empty ? 0u : 9u;
  value.baselineCompletedSequence = value.baselineSequence;
  value.initializerEpoch = 7u;
  value.baselineInitializer = value.baselineCompletedInitializer = 2u;
  value.baselineCompletionEdge = 1u;
  return value;
}

TapeProvenanceContext context(const RenderTapeIdentityV4Header& h) {
  return {.captureToken = h.captureToken, .captureEpoch = h.captureEpoch,
      .queue = h.queue, .baselineSequence = h.baselineSequence,
      .baselineInitializer = h.baselineInitializer,
      .baselineCompletedSequence = h.baselineCompletedSequence,
      .initializerEpoch = h.initializerEpoch,
      .baselineCompletedInitializer = h.baselineCompletedInitializer,
      .baselineCompletionEdge = h.baselineCompletionEdge,
      .baselineKind = h.baselineKind,
      .coordinatorDomain = static_cast<TapeCoordinatorAuthority>(h.coordinatorDomain)};
}

RenderTapeIdentityV3Source sourceLifecycle(std::uint32_t commands = 2u) {
  return {.sourceOrdinal = 7u, .seqId = 11u, .writerCaptureId = 1u,
      .writerGeneration = 9u, .storageGeneration = 7u,
      .publicationEdge = 30u, .submissionEdge = 40u,
      .completionEdge = 50u, .completedSequence = 11u,
      .cutEventOrdinal = 2u, .commandCount = commands,
      .cutKind = static_cast<std::uint32_t>(Cut::AfterEvent)};
}

RenderTapeIdentityV4Source source(std::uint32_t commands = 2u) {
  return {.lifecycle = sourceLifecycle(commands)};
}

RenderTapeIdentityV3Coordinator queryEnd(Prefix prefix = Prefix::EmptyBaseline) {
  RenderTapeIdentityV3Coordinator value;
  value.eventOrdinal = 2u; value.rawOrdinal = 20u; value.buildGeneration = 4u;
  value.dispatchEdge = 20u; value.releaseOrdinal = 1u; value.releaseGeneration = 2u;
  value.queryIdentity = kQuery; value.issueCycle = 1u; value.issueOrdinal = 2u;
  value.deviceEpoch = 77u; value.recordType = D9C_COMMAND_RECORD_QUERY_ISSUE;
  value.flags = 1u; value.queryType = 8u;
  value.prefixKind = static_cast<std::uint32_t>(prefix);
  if (prefix == Prefix::ArmBaseline) value.prefixSequence = 9u;
  if (prefix == Prefix::Unbound || prefix == Prefix::RealSource) {
    value.writerCaptureId = 1u; value.writerGeneration = 9u;
    value.storageGeneration = 7u; value.commandEnd = 1u;
  }
  if (prefix == Prefix::RealSource) {
    value.sourceIndex = 0u; value.prefixSequence = 11u;
  }
  return value;
}

RenderTapeIdentityV3CurrentIssue current(const RenderTapeIdentityV3Coordinator& row) {
  return {{5u, 6u}, row.queryIdentity, row.issueCycle,
      row.issueOrdinal, row.deviceEpoch, false};
}

RenderTapeIdentityV4PhysicalCommand draw(std::uint32_t physical,
                                       std::uint32_t count = 1u) {
  return {.sourceIndex = 0u, .physicalCommandIndex = physical,
      .kind = static_cast<std::uint32_t>(CommandKind::DrawRun), .drawCount = count};
}

RenderTapeIdentityV3Disposition gpu(std::uint64_t event, std::uint32_t record,
                                   std::uint32_t command, std::uint32_t subdraw = 0u) {
  return {.eventOrdinal = event, .firstRecord = record, .recordCount = 1u,
      .kind = static_cast<std::uint32_t>(Kind::GpuCommand),
      .commandIndex = command, .firstSubdraw = subdraw, .subdrawCount = 1u};
}

RenderTapeIdentityV3Disposition coordinator(std::uint64_t event,
                                           std::uint32_t record,
                                           std::uint32_t index = 0u) {
  return {.eventOrdinal = event, .firstRecord = record, .recordCount = 1u,
      .kind = static_cast<std::uint32_t>(Kind::Coordinator), .coordinatorIndex = index};
}

RenderTapeIdentityV3RecordFact queryFact(std::uint64_t event, std::uint32_t record) {
  return {.eventOrdinal = event, .recordIndex = record,
      .recordType = D9C_COMMAND_RECORD_QUERY_ISSUE,
      .queryFlags = 1u, .queryIdentity = kQuery, .queryType = 8u,
      .issueCycle = 1u, .issueOrdinal = 2u};
}

void exactEndAndCrossEventWriter() {
  const std::array events{RenderTapeIdentityV3EventFact{2u,
      static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk), 3u}};
  const std::array records{
      RenderTapeIdentityV3RecordFact{2u, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE},
      queryFact(2u, 1u),
      RenderTapeIdentityV3RecordFact{2u, 2u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE}};
  RenderTapeIdentityV4View identity;
  identity.header = header(); identity.sources = {source()};
  identity.commands = {draw(0u), draw(1u)};
  identity.coordinators = {{.provenance = queryEnd(Prefix::RealSource)}};
  identity.coordinators[0].provenance.recordIndex = 1u;
  identity.dispositions = {gpu(2u, 0u, 0u), coordinator(2u, 1u), gpu(2u, 2u, 1u)};
  identity.settlements = {{2u, 20u, 60u, 3u}};
  test::setWholeRawCompatibilityAttempts(identity);
  check(validateRenderTapeIdentityV4Rows(identity, events, records) == Status::Valid,
      "neutral Draw-END-youngerDraw retains one source and the exact immutable END cursor");
  auto deferred = identity;
  deferred.coordinators[0].provenance = queryEnd(Prefix::Unbound);
  deferred.coordinators[0].provenance.recordIndex = 1u;
  deferred.coordinators[0].provenance.observation = static_cast<std::uint32_t>(Observation::Pending);
  check(validateRenderTapeIdentityV4Rows(deferred, events, records, false) == Status::Valid &&
            validateRenderTapeIdentityV4Rows(deferred, events, records) == Status::Incomplete &&
            deferred.coordinators[0].provenance.commandEnd == identity.coordinators[0].provenance.commandEnd &&
            identity.coordinators[0].provenance.commandEnd < identity.sources[0].lifecycle.commandCount,
      "late prefix binding retains the frozen END cursor rather than the final younger-draw source extent");
  for (std::uint32_t cursor : {0u, 2u, 3u}) {
    auto bad = identity;
    bad.coordinators[0].provenance.commandEnd = cursor;
    check(validateRenderTapeIdentityV4Rows(bad, events, records) != Status::Valid,
        "zero, younger-command and out-of-range END cursors reject");
  }
  for (std::uint32_t mutation = 0u; mutation < 10u; ++mutation) {
    auto bad = identity;
    if (mutation == 0u) bad.coordinators.clear();
    if (mutation == 1u) bad.coordinators.push_back(bad.coordinators[0]);
    if (mutation == 2u) ++bad.coordinators[0].provenance.queryIdentity.generation;
    if (mutation == 3u) ++bad.coordinators[0].provenance.issueCycle;
    if (mutation == 4u) ++bad.coordinators[0].provenance.issueOrdinal;
    if (mutation == 5u) ++bad.coordinators[0].provenance.writerGeneration;
    if (mutation == 6u) ++bad.coordinators[0].provenance.storageGeneration;
    if (mutation == 7u) bad.coordinators[0].provenance.releaseAction = kSubmit;
    if (mutation == 8u) bad.coordinators[0].provenance.flags = 2u;
    if (mutation == 9u) bad.coordinators[0].provenance.dispatchResult = -1;
    check(validateRenderTapeIdentityV4Rows(bad, events, records) == Status::InvalidCoordinator,
        "missing, duplicate, stale or failed query coordinator evidence rejects with exact diagnostic");
  }
  for (std::uint32_t mutation = 0u; mutation < 4u; ++mutation) {
    auto bad = identity;
    if (mutation == 0u) ++bad.settlements[0].rawOrdinal;
    if (mutation == 1u) ++bad.attempts[0].attemptGeneration;
    if (mutation == 2u) bad.settlements[0].replayEdge = bad.coordinators[0].provenance.dispatchEdge;
    if (mutation == 3u) --bad.settlements[0].recordCount;
    check(validateRenderTapeIdentityV4Rows(bad, events, records) != Status::Valid,
        "raw settlement and coordinator must join the same actual attempt and follow its terminal edge");
  }
  // A single physical DrawRun may cross pure state, never either query fence.
  for (std::uint32_t flags : {1u, 2u}) {
    auto straddled = identity;
    straddled.sources[0].lifecycle.commandCount = 1u;
    straddled.commands = {draw(0u, 2u)};
    straddled.dispositions[2] = gpu(2u, 2u, 0u, 1u);
    straddled.coordinators[0].provenance = queryEnd(flags == 1u ? Prefix::RealSource : Prefix::None);
    straddled.coordinators[0].provenance.recordIndex = 1u;
    straddled.coordinators[0].provenance.flags = flags;
    straddled.coordinators[0].provenance.queryType = 9u;
    auto facts = records; facts[1].queryFlags = flags; facts[1].queryType = 9u;
    check(validateRenderTapeIdentityV4Rows(straddled, events, facts) == Status::InvalidDisposition,
        "neutral physical extent cannot conceal a BEGIN or END between its subdraws");
  }

  const std::array crossEvents{
      RenderTapeIdentityV3EventFact{2u, static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk), 1u},
      RenderTapeIdentityV3EventFact{3u, static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk), 1u},
      RenderTapeIdentityV3EventFact{4u, static_cast<std::uint32_t>(RenderTapeEventType::OrderedControl), 0u}};
  const std::array crossRecords{
      RenderTapeIdentityV3RecordFact{2u, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE},
      RenderTapeIdentityV3RecordFact{3u, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE}};
  identity.coordinators.clear();
  identity.sources[0].lifecycle.cutEventOrdinal = 4u;
  identity.sources[0].lifecycle.cutKind = static_cast<std::uint32_t>(Cut::BeforeEvent);
  identity.dispositions = {gpu(2u, 0u, 0u), gpu(3u, 0u, 1u)};
  identity.settlements = {{2u, 20u, 20u, 1u}, {3u, 21u, 60u, 1u}};
  test::setWholeRawCompatibilityAttempts(identity);
  check(validateRenderTapeIdentityV4Rows(identity, crossEvents, crossRecords) == Status::Valid &&
            identity.sources.size() == 1u,
      "cross-event writer has one actual publication despite a separate ordered-control trigger");
  auto duplicate = identity; duplicate.sources.push_back(duplicate.sources[0]);
  check(validateRenderTapeIdentityV4Rows(duplicate, crossEvents, crossRecords) == Status::InvalidSource,
      "per-event copies cannot duplicate a shared writer publication");
}

void deferredUnboundAndIndependentInitializer() {
  const std::array events{RenderTapeIdentityV3EventFact{2u,
      static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk), 1u}};
  const std::array records{queryFact(2u, 0u)};
  RenderTapeIdentityV4View identity;
  identity.header = header(); identity.coordinators = {{.provenance = queryEnd(Prefix::Unbound)}};
  identity.coordinators[0].provenance.observation = static_cast<std::uint32_t>(Observation::Pending);
  identity.dispositions = {coordinator(2u, 0u)};
  identity.settlements = {{2u, 20u, 30u, 1u}};
  test::setWholeRawCompatibilityAttempts(identity);
  check(validateRenderTapeIdentityV4Rows(identity, events, records, false) == Status::Valid &&
            validateRenderTapeIdentityV4Rows(identity, events, records) == Status::Incomplete &&
            identity.sources.empty() && identity.commands.empty(),
      "deferred Unbound END remains Pending without inventing source publication or physical command");
  const auto unbound = identity.coordinators[0].provenance;
  check(tapeProvenanceObservationAdmitted(context(identity.header), unbound, current(unbound),
            {}, {}, Observation::Pending) &&
            !tapeProvenanceObservationAdmitted(context(identity.header), unbound, current(unbound),
                {}, {}, Observation::Ready) &&
            !tapeProvenanceObservationAdmitted(context(identity.header), unbound, current(unbound),
                {}, {}, Observation::Completed),
      "unbound current issue admits Pending only, never Ready or Completed");
  identity.coordinators[0].provenance.observation = static_cast<std::uint32_t>(Observation::Ready);
  check(validateRenderTapeIdentityV4Rows(identity, events, records, false) == Status::InvalidCoordinator,
      "an Unbound END cannot forge Ready even in an incomplete snapshot");

  identity.coordinators = {{.provenance = queryEnd()}};
  identity.coordinators[0].provenance.dispatchEdge = 60u;
  identity.coordinators[0].provenance.prefixInitializer = 5u;
  identity.coordinators[0].provenance.prefixInitializerEpoch = 7u;
  identity.coordinators[0].provenance.initializerIndex = 0u;
  identity.coordinators[0].provenance.observation = static_cast<std::uint32_t>(Observation::Pending);
  identity.settlements[0].replayEdge = 80u;
  identity.initializers = {{.sequence = 5u, .epoch = 7u,
      .publicationEdge = 40u, .submissionEdge = 50u, .cutEventOrdinal = 2u,
      .cutKind = static_cast<std::uint32_t>(Cut::AfterRecord), .cutRecord = 1u}};
  test::setWholeRawCompatibilityAttempts(identity);
  check(validateRenderTapeIdentityV4Rows(identity, events, records, false) == Status::Valid &&
            validateRenderTapeIdentityV4Rows(identity, events, records) == Status::Incomplete,
      "post-arm initializer has its own submitted lifecycle, independent of empty render prefix");
  const auto pending = identity.coordinators[0].provenance;
  check(!tapeProvenanceObservationAdmitted(context(identity.header), pending, current(pending),
            {}, identity.initializers, Observation::Ready),
      "render sequence zero does not acknowledge a pending upload initializer");
  identity.coordinators[0].provenance.observation = static_cast<std::uint32_t>(Observation::Ready);
  check(validateRenderTapeIdentityV4Rows(identity, events, records, false) == Status::InvalidPrefix,
      "even an incomplete snapshot cannot claim Ready before independent initializer completion");
  identity.initializers[0].completionEdge = 70u;
  identity.initializers[0].completedSequence = 5u;
  check(validateRenderTapeIdentityV4Rows(identity, events, records) == Status::Valid,
      "actual independent initializer completion admits Ready without a fabricated render source");
  const auto ready = identity.coordinators[0].provenance;
  for (std::uint32_t mask = 0u; mask < 512u; ++mask) {
    auto issue = current(ready);
    if (mask & 1u) ++issue.capture.token;
    if (mask & 2u) ++issue.capture.epoch;
    if (mask & 4u) ++issue.identity.generation;
    if (mask & 8u) ++issue.cycle;
    if (mask & 16u) ++issue.ordinal;
    if (mask & 32u) ++issue.deviceEpoch;
    issue.cancelled = (mask & 64u) != 0u;
    if (mask & 128u) ++issue.identity.objectId;
    if (mask & 256u) issue.identity.kind = D9C_CHUNK_HANDLE_KIND_BUFFER;
    for (Observation observation : {Observation::Pending, Observation::Ready, Observation::Completed})
      check(tapeProvenanceObservationAdmitted(context(identity.header), ready, issue,
                {}, identity.initializers, observation) == (mask == 0u),
          "current issue observation joins capture, exact wire identity, cycle, ordinal, device epoch and cancellation");
  }
  auto future = identity;
  future.initializers[0].publicationEdge = 90u;
  future.initializers[0].submissionEdge = 100u;
  future.initializers[0].completionEdge = 110u;
  check(validateRenderTapeIdentityV4Rows(future, events, records) == Status::InvalidCoordinator,
      "an END cannot depend on a future initializer publication edge");
  const std::array laterEvents{events[0], RenderTapeIdentityV3EventFact{3u,
      static_cast<std::uint32_t>(RenderTapeEventType::OrderedControl), 0u}};
  future = identity; future.initializers[0].cutEventOrdinal = 3u;
  future.initializers[0].cutKind = static_cast<std::uint32_t>(Cut::BeforeEvent);
  future.initializers[0].cutRecord = 0u;
  check(validateRenderTapeIdentityV4Rows(future, laterEvents, records) == Status::InvalidPrefix,
      "forged early edge cannot hide a younger initializer tape cut");
  identity.header = header(false);
  identity.coordinators[0].provenance.prefixKind = static_cast<std::uint32_t>(Prefix::ArmBaseline);
  identity.coordinators[0].provenance.prefixSequence = 9u;
  identity.initializers[0].completionEdge = identity.initializers[0].completedSequence = 0u;
  check(!tapeProvenanceObservationAdmitted(context(identity.header), identity.coordinators[0].provenance,
            current(identity.coordinators[0].provenance), {}, identity.initializers, Observation::Ready),
      "already completed arm render baseline does not complete a newly pending initializer");
  identity.initializers[0].completionEdge = 70u; identity.initializers[0].completedSequence = 5u;
  ++identity.initializers[0].epoch;
  check(!tapeProvenanceObservationAdmitted(context(identity.header), identity.coordinators[0].provenance,
            current(identity.coordinators[0].provenance), {}, identity.initializers, Observation::Ready),
      "stale initializer epoch cannot authorize query readiness");
}

void generatedUpdateTextureCommands() {
  const std::array events{RenderTapeIdentityV3EventFact{2u,
      static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk), 1u}};
  const std::array records{RenderTapeIdentityV3RecordFact{
      .eventOrdinal = 2u, .recordType = D9C_COMMAND_RECORD_UPDATE_TEXTURE,
      .requiredReleaseAction = kSubmit}};
  RenderTapeIdentityV4View identity;
  identity.header = header(); identity.sources = {source()};
  identity.coordinators = {{.provenance = {.eventOrdinal = 2u, .rawOrdinal = 20u,
      .buildGeneration = 4u, .dispatchEdge = 20u,
      .releaseOrdinal = 1u, .releaseGeneration = 2u,
      .recordType = D9C_COMMAND_RECORD_UPDATE_TEXTURE, .releaseAction = kSubmit}}};
  identity.commands = {{.sourceIndex = 0u,
      .kind = static_cast<std::uint32_t>(CommandKind::SurfaceCopy), .coordinatorIndex = 0u},
      {.sourceIndex = 0u, .physicalCommandIndex = 1u,
       .kind = static_cast<std::uint32_t>(CommandKind::GenerateMipmaps), .coordinatorIndex = 0u}};
  identity.dispositions = {coordinator(2u, 0u)};
  identity.settlements = {{2u, 20u, 60u, 1u}};
  test::setWholeRawCompatibilityAttempts(identity);
  check(validateRenderTapeIdentityV4Rows(identity, events, records) == Status::Valid,
      "UpdateTexture owns neutral SurfaceCopy and optional mipmap commands with one source completion");
  for (std::uint32_t mutation = 0u; mutation < 5u; ++mutation) {
    auto bad = identity;
    if (mutation == 0u) bad.commands[1].physicalCommandIndex = 0u;
    if (mutation == 1u) bad.commands[1].coordinatorIndex = kRenderTapeIdentityV4NoIndex;
    if (mutation == 2u) bad.commands[1].coordinatorIndex = 1u;
    if (mutation == 3u) bad.coordinators[0].provenance.releaseAction = 0u;
    if (mutation == 4u) bad.coordinators.push_back(bad.coordinators[0]);
    check(validateRenderTapeIdentityV4Rows(bad, events, records) != Status::Valid,
        "generated extent rejects duplicate location, missing owner, wrong release or duplicate coordinator");
  }
  for (std::uint32_t kind = 0u; kind <= 10u; ++kind) {
    auto candidate = identity; candidate.commands[1].kind = kind;
    candidate.commands[1].drawCount = kind == static_cast<std::uint32_t>(CommandKind::DrawRun) ? 1u : 0u;
    const bool expected = kind == static_cast<std::uint32_t>(CommandKind::SurfaceCopy) ||
        kind == static_cast<std::uint32_t>(CommandKind::GenerateMipmaps);
    check((validateRenderTapeIdentityV4Rows(candidate, events, records) == Status::Valid) == expected,
        "coordinator-generated physical kinds admit only the evidenced UpdateTexture mapping");
  }
}

void splitAttemptsRollbackRetryAndReceipts() {
  const std::array events{RenderTapeIdentityV3EventFact{2u,
      static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk), 3u}};
  const std::array records{
      RenderTapeIdentityV3RecordFact{2u, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE},
      queryFact(2u, 1u),
      RenderTapeIdentityV3RecordFact{2u, 2u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE}};
  RenderTapeIdentityV4View identity;
  identity.header = header(); identity.sources = {source()};
  identity.commands = {draw(0u), draw(1u)};
  identity.coordinators = {{.provenance = queryEnd(Prefix::RealSource)}};
  identity.coordinators[0].provenance.recordIndex = 1u;
  identity.coordinators[0].provenance.buildGeneration = 5u;
  identity.dispositions = {gpu(2u, 0u, 0u), coordinator(2u, 1u), gpu(2u, 2u, 1u)};
  identity.settlements = {{2u, 20u, 60u, 3u}};
  identity.attempts = {
      test::compatibilityAttempt(2u, 20u, 4u, 0u, 1u, 2u, 10u, 3u),
      test::compatibilityAttempt(2u, 20u, 5u, 1u, 1u, 11u, 25u, 3u),
      test::compatibilityAttempt(2u, 20u, 6u, 2u, 1u, 26u, 59u, 3u)};
  identity.attempts[1].effectEdge = 19u;
  check(validateRenderTapeIdentityV4Rows(identity, events, records) == Status::Valid &&
            identity.settlements.size() == 1u && identity.attempts.size() == 3u &&
            identity.coordinators[0].provenance.buildGeneration == identity.attempts[1].attemptGeneration,
      "one raw settles after exact ordered split commits; END joins its actual middle transaction");

  auto rollback = test::compatibilityAttempt(2u, 20u, 3u, 0u, 1u, 2u, 5u, 3u);
  rollback.outcome = static_cast<std::uint32_t>(RenderTapeIdentityV4AttemptOutcome::RolledBack);
  rollback.receipt = {};
  auto retry = identity;
  retry.attempts[0].beginEdge = 6u;
  retry.attempts.insert(retry.attempts.begin(), rollback);
  check(validateRenderTapeIdentityV4Rows(retry, events, records) == Status::Valid,
      "pre-effect rollback contributes no committed coverage and retry retains a new actual generation");
  retry.attempts[0].recordCount = 0u;
  check(validateRenderTapeIdentityV4Rows(retry, events, records) == Status::Valid,
      "zero-record rollback is retained without inventing a destination receipt or committed span");

  auto suffixRetry = identity;
  rollback.attemptGeneration = 5u; rollback.firstRecord = 1u;
  rollback.beginEdge = 11u; rollback.terminalEdge = 14u;
  suffixRetry.attempts[1].attemptGeneration = 6u; suffixRetry.attempts[1].beginEdge = 15u;
  suffixRetry.attempts[2].attemptGeneration = 7u;
  suffixRetry.attempts.insert(suffixRetry.attempts.begin() + 1u, rollback);
  suffixRetry.coordinators[0].provenance.buildGeneration = 6u;
  check(validateRenderTapeIdentityV4Rows(suffixRetry, events, records) == Status::Valid &&
            suffixRetry.attempts[0].recordCount == 1u &&
            suffixRetry.attempts[1].firstRecord == suffixRetry.attempts[2].firstRecord,
      "local pre-effect suffix rollback does not reroute or revisit an already committed prefix");
  auto prefixRevisit = suffixRetry;
  prefixRevisit.attempts[1].firstRecord = 0u;
  check(validateRenderTapeIdentityV4Rows(prefixRevisit, events, records) == Status::InvalidTransaction,
      "rollback after committed prefix may not revisit that prefix");
  for (std::uint32_t mutation = 0u; mutation < 14u; ++mutation) {
    auto bad = identity;
    auto expected = Status::InvalidTransaction;
    if (mutation == 0u) bad.attempts.clear();
    if (mutation == 1u) bad.attempts[1].firstRecord = 2u;
    if (mutation == 2u) bad.attempts[1].firstRecord = 0u;
    if (mutation == 3u) std::swap(bad.attempts[0], bad.attempts[1]);
    if (mutation == 4u) bad.attempts[1].attemptGeneration = bad.attempts[0].attemptGeneration;
    if (mutation == 5u) bad.settlements[0].replayEdge = bad.attempts.back().terminalEdge;
    if (mutation == 6u) {
      ++bad.attempts[1].identity.source;
      bad.attempts[1].identity.lastSource = bad.attempts[1].identity.source;
      bad.attempts[1].receipt.identity = bad.attempts[1].identity;
    }
    if (mutation == 7u) {
      ++bad.attempts[1].receipt.identity.sequence; expected = Status::InvalidReceipt;
    }
    if (mutation == 8u) {
      bad.attempts[1].receipt.queueSequence = bad.sources[0].lifecycle.seqId;
      expected = Status::InvalidReceipt;
    }
    if (mutation == 9u) { bad.attempts[1].receipt.buildGeneration = 91u; expected = Status::InvalidReceipt; }
    if (mutation == 10u) { ++bad.attempts[1].stagedCommandCount; expected = Status::InvalidReceipt; }
    if (mutation == 11u) { bad.coordinators[0].provenance.buildGeneration = 4u; expected = Status::InvalidCoordinator; }
    if (mutation == 12u) { bad.attempts[1].effectEdge = 0u; expected = Status::InvalidCoordinator; }
    if (mutation == 13u) {
      bad.attempts[1].terminalEdge = 18u; bad.attempts[1].effectEdge = 17u;
      expected = Status::InvalidCoordinator;
    }
    check(validateRenderTapeIdentityV4Rows(bad, events, records) == expected,
        "attempt table rejects gaps, overlaps, order/generation/identity drift, false receipts and END misbinding");
  }
  for (std::uint32_t mutation = 0u; mutation < 3u; ++mutation) {
    auto bad = suffixRetry;
    if (mutation == 0u) bad.attempts[1].effectEdge = 12u;
    if (mutation == 1u) {
      bad.attempts[1].outcome = static_cast<std::uint32_t>(RenderTapeIdentityV4AttemptOutcome::FailedStop);
      bad.attempts[1].effectEdge = 12u;
    }
    if (mutation == 2u)
      bad.attempts[1].outcome = static_cast<std::uint32_t>(RenderTapeIdentityV4AttemptOutcome::FailedStop);
    check(validateRenderTapeIdentityV4Rows(bad, events, records) == Status::InvalidTransaction &&
              validateRenderTapeIdentityV4Rows(bad, events, records, false) == Status::InvalidTransaction,
        "post-effect rollback and any actual FailedStop invalidate capture even when incomplete snapshots are allowed");
  }
  auto falseRollbackReceipt = suffixRetry;
  falseRollbackReceipt.attempts[1].receipt = identity.attempts[0].receipt;
  check(validateRenderTapeIdentityV4Rows(falseRollbackReceipt, events, records) == Status::InvalidReceipt,
      "rolled-back transaction cannot claim a successful destination receipt");

  auto direct = identity;
  direct.sources[0].rawOrdinal = 20u;
  direct.sources[0].publicationBuildGeneration = 100u;
  direct.sources[0].publicationControlIndex = 5u;
  direct.sources[0].lifecycle.publicationEdge = 75u;
  direct.sources[0].lifecycle.submissionEdge = 80u;
  direct.sources[0].lifecycle.completionEdge = 90u;
  for (std::uint32_t index : {0u, 2u}) {
    auto& destination = direct.attempts[index];
    destination.stagedCommandCount = index == 0u ? 6u : 7u;
    destination.receipt.queueSequence = 11u;
    destination.receipt.buildGeneration = index == 0u ? 91u : 92u;
    destination.receipt.sourceGeneration = 9u;
    destination.receipt.storageGeneration = 7u;
    destination.receipt.kind = static_cast<std::uint32_t>(RenderTapeIdentityV4ReceiptKind::DirectChunkSlot);
    destination.receipt.controlIndex = 5u;
    destination.receipt.commandCount = destination.stagedCommandCount;
  }
  const auto& destination = direct.attempts[2];
  check(validateRenderTapeIdentityV4Rows(direct, events, records) == Status::Valid &&
            destination.stagedCommandCount != direct.sources[0].lifecycle.commandCount &&
            destination.identity.source != direct.sources[0].lifecycle.sourceOrdinal &&
            direct.attempts[0].receipt.buildGeneration != destination.receipt.buildGeneration &&
            direct.sources[0].lifecycle.publicationEdge > destination.terminalEdge,
      "fresh Direct span tickets share a stable Writing source that publishes later without aliasing physical extent or producer identity");
  auto independentPublication = direct;
  ++independentPublication.sources[0].rawOrdinal;
  ++independentPublication.sources[0].publicationBuildGeneration;
  check(validateRenderTapeIdentityV4Rows(independentPublication, events, records) == Status::Valid,
      "Direct ticket generation/raw remain distinct from the eventual source publication metadata");
  for (std::uint32_t mutation = 0u; mutation < 7u; ++mutation) {
    auto bad = direct;
    if (mutation == 0u) ++bad.sources[0].publicationControlIndex;
    if (mutation == 1u) ++bad.attempts[2].receipt.sourceGeneration;
    if (mutation == 2u) ++bad.attempts[2].receipt.storageGeneration;
    if (mutation == 3u) ++bad.attempts[2].receipt.queueSequence;
    if (mutation == 4u) bad.sources[0].lifecycle.publicationEdge = 26u;
    if (mutation == 5u) ++bad.attempts[2].receipt.commandCount;
    if (mutation == 6u) bad.attempts[2].receipt.buildGeneration = 0u;
    check(validateRenderTapeIdentityV4Rows(bad, events, records) == Status::InvalidReceipt,
        "Direct receipt fails closed on control/source/storage/sequence/publication ordering/count or absent build generation");
  }
  auto arena = direct;
  arena.attempts = {test::compatibilityAttempt(2u, 20u, 4u, 0u, 3u, 2u, 59u, 3u)};
  arena.attempts[0].effectEdge = 19u;
  arena.attempts[0].receipt = direct.attempts[2].receipt;
  arena.attempts[0].stagedCommandCount = direct.attempts[2].stagedCommandCount;
  arena.attempts[0].receipt.kind = static_cast<std::uint32_t>(RenderTapeIdentityV4ReceiptKind::Arena);
  arena.coordinators[0].provenance.buildGeneration = 4u;
  arena.sources[0].publicationBuildGeneration = 92u;
  arena.sources[0].lifecycle.publicationEdge = 30u;
  arena.sources[0].lifecycle.submissionEdge = 40u;
  arena.sources[0].lifecycle.completionEdge = 50u;
  arena.sources[0].lifecycle.commandCount = 1u;
  arena.sources[0].lifecycle.cutKind = static_cast<std::uint32_t>(Cut::AfterRecord);
  arena.sources[0].lifecycle.cutRecord = 1u;
  auto second = source(1u);
  second.lifecycle.sourceOrdinal = 8u; second.lifecycle.seqId = 12u;
  second.lifecycle.writerCaptureId = 2u; second.lifecycle.writerGeneration = 10u;
  second.lifecycle.storageGeneration = 8u; second.lifecycle.publicationEdge = 31u;
  second.lifecycle.submissionEdge = 41u; second.lifecycle.completionEdge = 51u;
  second.lifecycle.completedSequence = 12u;
  arena.sources.push_back(second);
  arena.commands[1].sourceIndex = 1u; arena.commands[1].physicalCommandIndex = 0u;
  check(validateRenderTapeIdentityV4Rows(arena, events, records) == Status::Valid &&
            arena.sources.size() == 2u && arena.attempts.size() == 1u,
      "Arena receipt anchors the first publication ticket rather than fabricating one transaction per physical source");
  for (std::uint32_t mutation = 0u; mutation < 4u; ++mutation) {
    auto bad = arena;
    if (mutation == 0u) ++bad.sources[0].rawOrdinal;
    if (mutation == 1u) ++bad.sources[0].publicationBuildGeneration;
    if (mutation == 2u) ++bad.sources[0].publicationControlIndex;
    if (mutation == 3u) {
      bad.sources[0].lifecycle.publicationEdge = 59u;
      bad.sources[0].lifecycle.submissionEdge = 80u;
      bad.sources[0].lifecycle.completionEdge = 90u;
      bad.sources[1].lifecycle.publicationEdge = 60u;
      bad.sources[1].lifecycle.submissionEdge = 81u;
      bad.sources[1].lifecycle.completionEdge = 91u;
    }
    check(validateRenderTapeIdentityV4Rows(bad, events, records) == Status::InvalidReceipt,
        "Arena retains strict raw/build/control and publication-before-terminal first-ticket evidence");
  }
  // Producer interval is retained begin/receipt evidence only. The canonical
  // tape has no independently ingress-authenticated producer interval, and a
  // compatibility queueSequence never supplies GPU submission/completion.
}

struct QueryTapeFixture {
  std::vector<std::byte> tape;
  RenderTapeIdentityV4View identity;
};

QueryTapeFixture queryCycles(std::uint32_t type) {
  QueryTapeFixture fixture;
  auto& identity = fixture.identity; identity.header = header();
  RenderTapeBuilder builder;
  builder.appendBootstrapState(test::fullStateChunk(false));
  const RenderTapeSurfaceDescriptorV2 output{
      .storage = static_cast<std::uint32_t>(RenderTapeSurfaceStorage::SwapchainBackbuffer),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::ProducedPresentOutput),
      .surface = {.format = 21u, .resourceType = 1u, .usage = 1u,
          .width = 4u, .height = 4u, .depth = 1u}};
  builder.appendObjectDefine(kOutput, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&output, 1u)), 0u, {});
  const RenderTapeQueryDescriptorV2 descriptor{.type = type, .dataBytes = 4u};
  builder.appendObjectDefine(kQuery, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Query),
      std::as_bytes(std::span(&descriptor, 1u)), 0u, {});
  std::uint64_t event = 3u, raw = 20u, ordinal = 0u;
  for (std::uint64_t cycle = 1u; cycle <= 2u; ++cycle) {
    for (std::uint32_t flags : {2u, 1u}) {
      if (type == 8u && flags == 2u) continue;
      ++event; ++ordinal;
      const D9CCommandChunkWireQueryIssue issue{.flags = flags};
      const D9CCommandChunkWireHandleEntry handle{
          .kind = kQuery.kind, .generation = kQuery.generation, .objectId = kQuery.objectId};
      builder.appendCommandChunk({.recordCount = 1u, .handleCount = 1u},
          test::singleRecordChunk(D9C_COMMAND_RECORD_QUERY_ISSUE,
              std::as_bytes(std::span(&issue, 1u)), std::span(&handle, 1u)));
      auto control = queryEnd(flags == 2u ? Prefix::None : Prefix::EmptyBaseline);
      control.eventOrdinal = event; control.rawOrdinal = raw;
      control.dispatchEdge = event * 100u + 20u;
      control.releaseOrdinal = ordinal; control.queryType = type;
      control.flags = flags; control.issueCycle = cycle; control.issueOrdinal = ordinal;
      const auto index = static_cast<std::uint32_t>(identity.coordinators.size());
      identity.coordinators.push_back({.provenance = control});
      identity.dispositions.push_back(coordinator(event, 0u, index));
      identity.settlements.push_back({event, raw++, event * 100u + 90u, 1u});
    }
  }
  const RenderTapeQueryGetDataControlV2 poll{.issueCycle = 2u};
  builder.appendOrderedControl({.identity = kQuery,
      .kind = static_cast<std::uint32_t>(RenderTapeControlKind::QueryGetData),
      .disposition = static_cast<std::uint32_t>(RenderTapeControlDisposition::Completed),
      .controlBytes = sizeof(poll), .completionOrdinal = 1u}, std::as_bytes(std::span(&poll, 1u)));
  ++event;
  const auto present = ++event;
  builder.appendCommandChunk({.recordCount = 1u}, test::presentChunk());
  const RenderTapeOracleAttachment oracle{.identity = kOutput,
      .descriptorKind = static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface)};
  builder.appendPresentComplete(present, 2u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  fixture.tape = builder.seal(); identity.header.presentOrdinal = present;
  identity.sources = {source(1u)};
  identity.sources[0].lifecycle.cutEventOrdinal = present;
  identity.sources[0].lifecycle.publicationEdge = present * 100u + 30u;
  identity.sources[0].lifecycle.submissionEdge = present * 100u + 40u;
  identity.sources[0].lifecycle.completionEdge = present * 100u + 50u;
  identity.commands = {{.sourceIndex = 0u,
      .kind = static_cast<std::uint32_t>(CommandKind::Present)}};
  auto presentDisposition = gpu(present, 0u, 0u); presentDisposition.subdrawCount = 0u;
  identity.dispositions.push_back(presentDisposition);
  identity.settlements.push_back({present, raw, present * 100u + 90u, 1u});
  test::setWholeRawCompatibilityAttempts(identity);
  return fixture;
}

void canonicalBeginEndAndReissue() {
  for (std::uint32_t type : {8u, 9u}) {
    const auto fixture = queryCycles(type);
    check(validateRenderTape(fixture.tape, {}).valid(),
        "query cycle fixture authenticates typed descriptor, Issue transitions and external GetData transcript");
    Status status{};
    const auto bytes = buildRenderTapeIdentityV4(fixture.tape, {}, fixture.identity, &status);
    RenderTapeIdentityV4View imported;
    const std::size_t expected = type == 8u ? 2u : 4u;
    check(status == Status::Valid && !bytes.empty() &&
              validateRenderTapeIdentityV4(fixture.tape, {}, bytes, &imported) == Status::Valid &&
              imported.coordinators.size() == expected &&
              imported.coordinators.back().provenance.issueCycle == 2u &&
              imported.coordinators.back().provenance.issueOrdinal == expected &&
              imported.sources.size() == 1u && imported.commands.size() == 1u,
        "EVENT END-only and occlusion BEGIN/END reissue advance canonical cycle and ordinal without fake commands");
    check(buildRenderTapeIdentityV4(fixture.tape, {}, imported, &status) == bytes,
        "query-bearing v4 round trips exact admitted provenance bytes");
    for (std::uint32_t mutation = 0u; mutation < 8u; ++mutation) {
      auto bad = fixture.identity;
      if (mutation == 0u) bad.coordinators.clear();
      if (mutation == 1u) ++bad.coordinators.back().provenance.queryIdentity.generation;
      if (mutation == 2u) --bad.coordinators.back().provenance.issueCycle;
      if (mutation == 3u) --bad.coordinators.back().provenance.issueOrdinal;
      if (mutation == 4u) bad.coordinators.back().provenance.releaseAction = kSubmit;
      if (mutation == 5u) bad.coordinators.push_back(bad.coordinators.back());
      if (mutation == 6u) bad.dispositions[0].kind = static_cast<std::uint32_t>(Kind::StateOnly);
      if (mutation == 7u) ++bad.attempts[0].attemptGeneration;
      check(buildRenderTapeIdentityV4(fixture.tape, {}, bad, &status).empty() && status != Status::Valid,
          "full tape parser rejects omitted captured Issue evidence, stale cycles, wrong releases and settlement attempts");
    }
    // No external poll coordinator is fabricated. These assertions establish
    // provenance only; neither completed GetData nor V4 bytes qualify GPU
    // execution topology or replay-local query receipt readiness.
  }
}

void boundedCanonicalFactsAreTransactional() {
  const auto fixture = queryCycles(8u);
  const RenderTapeIdentityV3EventFact eventSentinel{999u, 888u, 777u};
  const RenderTapeIdentityV3RecordFact recordSentinel{
      .eventOrdinal = 999u, .recordIndex = 888u, .recordType = 777u,
      .requiredReleaseAction = 6u, .queryFlags = 5u,
      .queryIdentity = {D9C_CHUNK_HANDLE_KIND_QUERY, 4u, 333u},
      .queryType = 8u, .issueCycle = 222u, .issueOrdinal = 111u};
  const auto unchanged = [&](const auto& events, const auto& records) {
    return events.size() == 1u && records.size() == 1u &&
        events[0].ordinal == eventSentinel.ordinal && events[0].type == eventSentinel.type &&
        events[0].recordCount == eventSentinel.recordCount &&
        records[0].eventOrdinal == recordSentinel.eventOrdinal &&
        records[0].recordIndex == recordSentinel.recordIndex &&
        records[0].recordType == recordSentinel.recordType &&
        records[0].requiredReleaseAction == recordSentinel.requiredReleaseAction &&
        records[0].queryFlags == recordSentinel.queryFlags &&
        renderTapeIdentityV3SameIdentity(records[0].queryIdentity, recordSentinel.queryIdentity) &&
        records[0].queryType == recordSentinel.queryType &&
        records[0].issueCycle == recordSentinel.issueCycle &&
        records[0].issueOrdinal == recordSentinel.issueOrdinal;
  };
  for (std::uint32_t budget : {1u, 2u}) {
    std::vector events{eventSentinel}; std::vector records{recordSentinel};
    check(buildTapeCanonicalFacts(fixture.tape, {}, fixture.identity.header.presentOrdinal,
              budget, events, records) == TapeProvenanceStatus::CapacityExceeded &&
              unchanged(events, records),
        "canonical-fact maxRows 1/2 fail before admitting max+1 or exposing partial output");
  }
  ImportedRenderTapeView tape;
  check(importPrevalidatedRenderTape(fixture.tape, tape), "bounded fact fixture imports");
  const auto exact = static_cast<std::uint32_t>(tape.events.size());
  std::vector events{eventSentinel}; std::vector records{recordSentinel};
  check(buildTapeCanonicalFacts(fixture.tape, {}, fixture.identity.header.presentOrdinal,
            exact - 1u, events, records) == TapeProvenanceStatus::CapacityExceeded &&
            unchanged(events, records),
      "exact canonical event bound minus one rejects transactionally after prior fact growth");
  check(buildTapeCanonicalFacts(fixture.tape, {}, fixture.identity.header.presentOrdinal,
            exact, events, records) == TapeProvenanceStatus::Valid &&
            events.size() == exact && records.size() == 3u &&
            records[0].issueCycle == 1u && records[0].issueOrdinal == 1u &&
            records[1].issueCycle == 2u && records[1].issueOrdinal == 2u,
      "exact bound replaces sentinels only after complete canonical query fact admission");
  events = {eventSentinel}; records = {recordSentinel};
  check(buildTapeCanonicalFacts(fixture.tape, {}, fixture.identity.header.presentOrdinal + 1u,
            exact, events, records) == TapeProvenanceStatus::InvalidHeader &&
            unchanged(events, records),
      "late canonical Present mismatch also preserves both prior output tables");
}

void v3AndV4ShareProvenanceDiagnostics() {
  const std::array events{RenderTapeIdentityV3EventFact{2u,
      static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk), 3u}};
  const std::array records{
      RenderTapeIdentityV3RecordFact{2u, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE},
      queryFact(2u, 1u),
      RenderTapeIdentityV3RecordFact{2u, 2u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE}};
  RenderTapeIdentityV4View neutral;
  neutral.header = header(); neutral.sources = {source()};
  neutral.commands = {draw(0u), draw(1u)};
  neutral.coordinators = {{.provenance = queryEnd(Prefix::RealSource)}};
  neutral.coordinators[0].provenance.recordIndex = 1u;
  neutral.dispositions = {gpu(2u, 0u, 0u), coordinator(2u, 1u), gpu(2u, 2u, 1u)};
  neutral.settlements = {{2u, 20u, 60u, 3u}};
  test::setWholeRawCompatibilityAttempts(neutral);
  RenderTapeIdentityV3View legacy;
  // V3 authority is supplied independently. In particular, neither V4 bytes
  // nor neutral command extents are converted into invented Pass membership.
  legacy.header.frameId = 3u; legacy.header.presentOrdinal = 9u;
  legacy.header.captureToken = 5u; legacy.header.captureEpoch = 6u; legacy.header.queue = 1u;
  legacy.header.baselineKind = static_cast<std::uint32_t>(Prefix::EmptyBaseline);
  legacy.header.initializerEpoch = 7u;
  legacy.header.baselineInitializer = legacy.header.baselineCompletedInitializer = 2u;
  legacy.header.baselineCompletionEdge = 1u;
  legacy.commands = {{.logicalPassId = 17u, .sourceIndex = 0u,
      .kind = static_cast<std::uint32_t>(CommandKind::DrawRun), .drawCount = 1u,
      .membership = static_cast<std::uint32_t>(RenderTapeIdentityV3Membership::Pass),
      .dagPassIndex = 0u, .passKind = 1u},
      {.logicalPassId = 17u, .sourceIndex = 0u, .physicalCommandIndex = 1u,
       .kind = static_cast<std::uint32_t>(CommandKind::DrawRun), .drawCount = 1u,
       .membership = static_cast<std::uint32_t>(RenderTapeIdentityV3Membership::Pass),
       .dagPassIndex = 0u, .passKind = 1u}};
  legacy.sources = {sourceLifecycle()};
  legacy.coordinators = {queryEnd(Prefix::RealSource)};
  legacy.coordinators[0].recordIndex = 1u;
  legacy.dispositions = {gpu(2u, 0u, 0u), coordinator(2u, 1u), gpu(2u, 2u, 1u)};
  legacy.settlements = {{2u, 20u, 4u, 60u, 3u}};
  for (std::uint32_t mutation = 0u; mutation < 12u; ++mutation) {
    auto candidate = neutral;
    auto v3 = legacy;
    auto expectedV4 = Status::InvalidCoordinator;
    auto expectedV3 = RenderTapeIdentityV3Status::InvalidCoordinator;
    if (mutation == 0u) {
      expectedV4 = Status::Valid; expectedV3 = RenderTapeIdentityV3Status::Valid;
    }
    if (mutation == 1u) { candidate.coordinators.clear(); v3.coordinators.clear(); }
    if (mutation == 2u) {
      candidate.coordinators.push_back(candidate.coordinators[0]);
      v3.coordinators.push_back(v3.coordinators[0]);
    }
    if (mutation == 3u) {
      ++candidate.coordinators[0].provenance.queryIdentity.generation;
      ++v3.coordinators[0].queryIdentity.generation;
    }
    if (mutation == 4u) { candidate.coordinators[0].provenance.releaseAction = kSubmit; v3.coordinators[0].releaseAction = kSubmit; }
    if (mutation == 5u) {
      candidate.coordinators[0].provenance.commandEnd = 2u;
      v3.coordinators[0].commandEnd = 2u;
      expectedV4 = Status::InvalidPrefix; expectedV3 = RenderTapeIdentityV3Status::InvalidPrefix;
    }
    if (mutation == 6u) { ++candidate.coordinators[0].provenance.prefixSequence; ++v3.coordinators[0].prefixSequence; }
    if (mutation == 7u || mutation == 8u) {
      if (mutation == 7u) {
        ++candidate.settlements[0].rawOrdinal; ++v3.settlements[0].rawOrdinal;
        expectedV4 = Status::InvalidTransaction;
      } else {
        ++candidate.coordinators[0].provenance.buildGeneration; ++v3.settlements[0].buildGeneration;
      }
      expectedV3 = RenderTapeIdentityV3Status::InvalidSettlement;
    }
    if (mutation == 9u) { --candidate.coordinators[0].provenance.recordIndex; --v3.coordinators[0].recordIndex; }
    if (mutation == 10u) {
      candidate.sources[0].lifecycle.completionEdge = candidate.sources[0].lifecycle.completedSequence = 0u;
      v3.sources[0].completionEdge = v3.sources[0].completedSequence = 0u;
      expectedV4 = Status::Incomplete; expectedV3 = RenderTapeIdentityV3Status::Incomplete;
    }
    if (mutation == 11u) {
      candidate.coordinators[0].provenance.releaseOrdinal = 0u;
      candidate.coordinators[0].provenance.releaseGeneration = 0u;
      v3.coordinators[0].releaseOrdinal = v3.coordinators[0].releaseGeneration = 0u;
    }
    check(validateRenderTapeIdentityV4Rows(candidate, events, records) == expectedV4 &&
              validateRenderTapeIdentityV3Rows(v3, events, records) == expectedV3,
        "independent v3 oracle preserves its raw-wide generation while v4 separately joins attempts and settlement");
  }
}

void ordinaryBaselineAndInitializerReadiness() {
  const std::array events{RenderTapeIdentityV3EventFact{
      2u, static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk), 1u}};
  const std::array facts{queryFact(2u, 0u)};
  for (const bool empty : {false, true}) {
    RenderTapeIdentityV4View identity;
    identity.header = header(empty);
    identity.header.coordinatorDomain = static_cast<std::uint32_t>(TapeCoordinatorAuthority::OrdinarySerial);
    auto end = queryEnd(empty ? Prefix::EmptyBaseline : Prefix::ArmBaseline);
    end.releaseOrdinal = end.releaseGeneration = 0u;
    end.observation = static_cast<std::uint32_t>(Observation::Ready);
    identity.coordinators = {{.provenance = end, .admissionEdge = 19u,
        .authority = identity.header.coordinatorDomain}};
    identity.dispositions = {coordinator(2u, 0u)};
    identity.settlements = {{2u, 20u, 30u, 1u}};
    test::setWholeRawCompatibilityAttempts(identity);
    const TapeCoordinatorAdmissionEvidence admission{TapeCoordinatorAuthority::OrdinarySerial, 19u};
    check(validateRenderTapeIdentityV4Rows(identity, events, facts) == Status::Valid &&
        tapeProvenanceObservationAdmitted(context(identity.header), end, current(end),
            {}, {}, Observation::Ready, admission) && identity.sources.empty(),
        "positive ordinary admission preserves completed arm/empty baseline END readiness without a render source");
    auto wrongBaseline = identity;
    ++wrongBaseline.coordinators[0].provenance.prefixSequence;
    check(validateRenderTapeIdentityV4Rows(wrongBaseline, events, facts) != Status::Valid,
        "ordinary authority cannot replace the exact retained arm baseline sequence");

    auto& pending = identity.coordinators[0];
    pending.admissionEdge = 59u;
    pending.provenance.dispatchEdge = 60u;
    pending.provenance.prefixInitializer = 5u;
    pending.provenance.prefixInitializerEpoch = 7u;
    pending.provenance.initializerIndex = 0u;
    pending.provenance.observation = static_cast<std::uint32_t>(Observation::Pending);
    identity.initializers = {{.sequence = 5u, .epoch = 7u,
        .publicationEdge = 40u, .submissionEdge = 50u, .cutEventOrdinal = 2u,
        .cutKind = static_cast<std::uint32_t>(Cut::AfterRecord), .cutRecord = 1u}};
    identity.settlements[0].replayEdge = 80u;
    test::setWholeRawCompatibilityAttempts(identity);
    const TapeCoordinatorAdmissionEvidence initializerAdmission{TapeCoordinatorAuthority::OrdinarySerial, 59u};
    check(validateRenderTapeIdentityV4Rows(identity, events, facts, false) == Status::Valid &&
        validateRenderTapeIdentityV4Rows(identity, events, facts) == Status::Incomplete &&
        !tapeProvenanceObservationAdmitted(context(identity.header), pending.provenance,
            current(pending.provenance), {}, identity.initializers, Observation::Ready, initializerAdmission),
        "ordinary arm/empty baseline does not acknowledge a separately pending initializer-only END");
    pending.provenance.observation = static_cast<std::uint32_t>(Observation::Ready);
    check(validateRenderTapeIdentityV4Rows(identity, events, facts, false) == Status::InvalidPrefix,
        "ordinary initializer-only END cannot forge Ready from render baseline completion");
    identity.initializers[0].completionEdge = 70u;
    identity.initializers[0].completedSequence = 5u;
    check(validateRenderTapeIdentityV4Rows(identity, events, facts) == Status::Valid &&
        tapeProvenanceObservationAdmitted(context(identity.header), pending.provenance,
            current(pending.provenance), {}, identity.initializers, Observation::Ready, initializerAdmission) &&
        identity.sources.empty(),
        "actual initializer completion admits ordinary END readiness independently of absent render publication");
    ++identity.initializers[0].epoch;
    check(validateRenderTapeIdentityV4Rows(identity, events, facts) != Status::Valid &&
        !tapeProvenanceObservationAdmitted(context(identity.header), pending.provenance,
            current(pending.provenance), {}, identity.initializers, Observation::Ready, initializerAdmission),
        "stale initializer epoch remains rejected under positive ordinary admission");
  }
}

void explicitCoordinatorDomainsAndEdges() {
  for (const auto authority : {TapeCoordinatorAuthority::SessionRelease, TapeCoordinatorAuthority::OrdinarySerial}) {
    const auto fixture = test::makeIdentityV4CoordinatorFixture(authority);
    Status status{};
    const auto bytes = buildRenderTapeIdentityV4(fixture.tape, {}, fixture.identity, &status);
    check(status == Status::Valid && !bytes.empty() &&
          validateRenderTapeIdentityV4(fixture.tape, {}, bytes) == Status::Valid,
        "complete BEGIN/END captures retain explicit session or ordinary admission domains");
    for (std::uint32_t mutation = 0u; mutation < 13u; ++mutation) {
      auto bad = fixture.identity;
      auto& row = bad.coordinators[0];
      if (mutation == 0u) bad.header.coordinatorDomain = 0u;
      if (mutation == 1u) bad.header.reserved4 = 1u;
      if (mutation == 2u) bad.header.headerSize = 256u;
      if (mutation == 3u) row.authority = 0u;
      if (mutation == 4u) row.authority = authority == TapeCoordinatorAuthority::SessionRelease ? 2u : 1u;
      if (mutation == 5u) row.reserved0 = 1u;
      if (mutation == 6u) row.admissionEdge = authority == TapeCoordinatorAuthority::SessionRelease ? 19u : 0u;
      if (mutation == 7u) row.admissionEdge = row.provenance.dispatchEdge;
      if (mutation == 8u) row.admissionEdge = bad.attempts[0].beginEdge;
      if (mutation == 9u) row.provenance.releaseOrdinal = authority == TapeCoordinatorAuthority::SessionRelease ? 0u : 1u;
      if (mutation == 10u) ++row.provenance.buildGeneration;
      if (mutation == 11u) ++row.provenance.recordIndex;
      if (mutation == 12u) row.provenance.dispatchResult = -1;
      std::vector<RenderTapeIdentityV3EventFact> events;
      std::vector<RenderTapeIdentityV3RecordFact> records;
      check(buildTapeCanonicalFacts(fixture.tape, {}, bad.header.presentOrdinal, 4096u, events, records) ==
            TapeProvenanceStatus::Valid && validateRenderTapeIdentityV4Rows(bad, events, records) != Status::Valid,
        "unknown/mixed authority, old header, missing receipt/admission and out-of-attempt edges reject");
    }
  }

  const std::array events{RenderTapeIdentityV3EventFact{
      2u, static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk), 3u}};
  const std::array facts{RenderTapeIdentityV3RecordFact{2u, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE},
      queryFact(2u, 1u), RenderTapeIdentityV3RecordFact{2u, 2u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE}};
  RenderTapeIdentityV4View ordinary;
  ordinary.header = header();
  ordinary.header.coordinatorDomain = static_cast<std::uint32_t>(TapeCoordinatorAuthority::OrdinarySerial);
  ordinary.sources = {source()}; ordinary.commands = {draw(0u), draw(1u)};
  auto control = queryEnd(Prefix::RealSource);
  control.recordIndex = 1u; control.releaseOrdinal = control.releaseGeneration = 0u;
  ordinary.coordinators = {{.provenance = control, .admissionEdge = 19u,
      .authority = ordinary.header.coordinatorDomain}};
  ordinary.dispositions = {gpu(2u, 0u, 0u), coordinator(2u, 1u), gpu(2u, 2u, 1u)};
  ordinary.settlements = {{2u, 20u, 60u, 3u}};
  test::setWholeRawCompatibilityAttempts(ordinary);
  check(validateRenderTapeIdentityV4Rows(ordinary, events, facts) == Status::Valid,
      "ordinary END retains its real nonempty source prefix and exact immutable cursor");
  for (std::uint32_t mutation = 0u; mutation < 4u; ++mutation) {
    auto bad = ordinary;
    auto& end = bad.coordinators[0].provenance;
    if (mutation == 0u) ++end.prefixSequence;
    if (mutation == 1u) ++end.writerGeneration;
    if (mutation == 2u) ++end.storageGeneration;
    if (mutation == 3u) ++end.commandEnd;
    check(validateRenderTapeIdentityV4Rows(bad, events, facts) != Status::Valid,
        "ordinary admission cannot authorize wrong source, stale writer/storage or younger END cursor");
  }
}

} // namespace

int main() {
  try {
    exactEndAndCrossEventWriter();
    deferredUnboundAndIndependentInitializer();
    generatedUpdateTextureCommands();
    splitAttemptsRollbackRetryAndReceipts();
    canonicalBeginEndAndReissue();
    boundedCanonicalFactsAreTransactional();
    v3AndV4ShareProvenanceDiagnostics();
    explicitCoordinatorDomainsAndEdges();
    ordinaryBaselineAndInitializerReadiness();
    std::cout << "render tape v4 query provenance spec: " << checks << " checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "render tape v4 query provenance spec failed: " << error.what() << '\n';
    return 1;
  }
}
