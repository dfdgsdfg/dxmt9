#include "device_c_render_tape_identity_v3.hpp"
#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_descriptors.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using namespace dxmt9::d3d9;
using Prefix = RenderTapeIdentityV3PrefixKind;
using Kind = RenderTapeIdentityV3DispositionKind;
using CommandKind = RenderTapeIdentityV3CommandKind;
using Phase = RenderTapeIdentityV3SourcePhase;
using Observation = RenderTapeIdentityV3Observation;
using Status = RenderTapeIdentityV3Status;
constexpr RenderTapeIdentityV3CaptureKey kKey{5u, 6u};
constexpr D9CWireObjectIdentity kQuery{D9C_CHUNK_HANDLE_KIND_QUERY, 3u, 42u};
constexpr dxmt9::query::WriterIdentity kWriter{1u, 0u, 9u, 7u};
std::uint64_t checks = 0u;

void check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

RenderTapeIdentityV3Header header(bool empty = true) {
  RenderTapeIdentityV3Header value{};
  value.frameId = 3u;
  value.presentOrdinal = 9u;
  value.captureToken = kKey.token;
  value.captureEpoch = kKey.epoch;
  value.queue = 1u;
  value.baselineKind = static_cast<std::uint32_t>(
      empty ? Prefix::EmptyBaseline : Prefix::ArmBaseline);
  value.baselineSequence = empty ? 0u : 9u;
  value.baselineCompletedSequence = value.baselineSequence;
  value.initializerEpoch = 7u;
  value.baselineInitializer = 2u;
  value.baselineCompletedInitializer = 2u;
  value.baselineCompletionEdge = 1u;
  return value;
}

RenderTapeIdentityV3Source source(std::uint32_t commands = 2u) {
  return {.sourceOrdinal = 7u, .seqId = 11u, .writerCaptureId = 1u,
          .writerGeneration = 9u, .storageGeneration = 7u,
          .publicationEdge = 3u, .submissionEdge = 4u,
          .completionEdge = 5u, .completedSequence = 11u,
          .cutEventOrdinal = 2u, .commandCount = commands,
          .cutKind = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterEvent)};
}

RenderTapeIdentityV3Coordinator end(Prefix prefix = Prefix::EmptyBaseline) {
  RenderTapeIdentityV3Coordinator value{};
  value.eventOrdinal = 2u;
  value.rawOrdinal = 20u;
  value.buildGeneration = 4u;
  value.dispatchEdge = 2u;
  value.releaseOrdinal = 1u;
  value.releaseGeneration = 2u;
  value.queryIdentity = kQuery;
  value.issueCycle = 1u;
  value.issueOrdinal = 2u;
  value.deviceEpoch = 77u;
  value.recordType = D9C_COMMAND_RECORD_QUERY_ISSUE;
  value.flags = 1u;
  value.queryType = 8u;
  value.prefixKind = static_cast<std::uint32_t>(prefix);
  if (prefix == Prefix::ArmBaseline) value.prefixSequence = 9u;
  if (prefix == Prefix::Unbound || prefix == Prefix::RealSource) {
    value.writerCaptureId = 1u;
    value.writerGeneration = 9u;
    value.storageGeneration = 7u;
    value.commandEnd = 1u;
  }
  if (prefix == Prefix::RealSource) {
    value.sourceIndex = 0u;
    value.prefixSequence = 11u;
  }
  return value;
}

RenderTapeIdentityV3CurrentIssue current(const RenderTapeIdentityV3Coordinator& row) {
  return {kKey, row.queryIdentity, row.issueCycle, row.issueOrdinal, row.deviceEpoch, false};
}

RenderTapeIdentityV3Disposition gpu(std::uint64_t event, std::uint32_t record,
                                   std::uint32_t command, std::uint32_t subdraw = 0u) {
  return {.eventOrdinal = event,
          .firstRecord = record, .recordCount = 1u,
          .kind = static_cast<std::uint32_t>(Kind::GpuCommand),
          .commandIndex = command, .firstSubdraw = subdraw, .subdrawCount = 1u};
}

RenderTapeIdentityV3Command drawCommand(std::uint32_t sourceIndex,
    std::uint32_t physicalIndex, std::uint32_t draws = 1u) {
  return {.logicalPassId = 17u, .sourceIndex = sourceIndex,
          .physicalCommandIndex = physicalIndex,
          .kind = static_cast<std::uint32_t>(CommandKind::DrawRun), .drawCount = draws,
          .membership = static_cast<std::uint32_t>(RenderTapeIdentityV3Membership::Pass),
          .dagPassIndex = 0u};
}

RenderTapeIdentityV3Disposition state(std::uint64_t event, std::uint32_t record) {
  return {.eventOrdinal = event, .firstRecord = record, .recordCount = 1u,
          .kind = static_cast<std::uint32_t>(Kind::StateOnly)};
}

RenderTapeIdentityV3Disposition coordinator(std::uint64_t event, std::uint32_t record,
                                           std::uint32_t index = 0u) {
  return {.eventOrdinal = event, .firstRecord = record, .recordCount = 1u,
          .kind = static_cast<std::uint32_t>(Kind::Coordinator), .coordinatorIndex = index};
}

RenderTapeIdentityV3RecordFact queryFact(std::uint64_t event, std::uint32_t record,
                                       std::uint64_t issue = 2u) {
  return {.eventOrdinal = event, .recordIndex = record,
          .recordType = D9C_COMMAND_RECORD_QUERY_ISSUE,
          .queryFlags = 1u, .queryIdentity = kQuery, .queryType = 8u,
          .issueCycle = 1u, .issueOrdinal = issue};
}

void exhaustiveRegistryAndPublication() {
  // Finite alphabet: every live registry row x all three dispositions;
  // sequence {1,2}, edge tuple 3x4x5, waterline {0,1,2}, next edge {0..5}.
  for (const auto& record : kRecordTopology) {
    for (std::uint32_t kind = 1u; kind <= 3u; ++kind) {
      const bool expected = record.topology == RecordReplayTopology::DirectStateOnly
          ? kind == 2u : record.topology == RecordReplayTopology::OrderedControl
          ? kind == 3u : kind == 1u;
      check(renderTapeIdentityV3DispositionAdmitted(record.type, static_cast<Kind>(kind)) == expected,
            "closed production topology controls disposition admission");
    }
  }
  for (std::uint32_t type : {0u, 5u, UINT32_MAX})
    for (std::uint32_t kind = 1u; kind <= 3u; ++kind)
      check(!renderTapeIdentityV3DispositionAdmitted(type, static_cast<Kind>(kind)),
            "unknown/retired records cannot be hidden by a disposition");
  for (std::uint64_t seq = 1u; seq <= 2u; ++seq)
    for (std::uint64_t published = 0u; published <= 2u; ++published)
      for (std::uint64_t submitted = 0u; submitted <= 3u; ++submitted)
        for (std::uint64_t completed = 0u; completed <= 4u; ++completed)
          for (std::uint64_t waterline = 0u; waterline <= 2u; ++waterline) {
            auto row = source();
            row.seqId = seq;
            row.publicationEdge = published;
            row.submissionEdge = submitted;
            row.completionEdge = completed;
            row.completedSequence = waterline;
            const auto expected = !published ? Phase::Invalid
                : !submitted ? (!completed && !waterline ? Phase::Published : Phase::Invalid)
                : submitted <= published ? Phase::Invalid
                : !completed ? (!waterline ? Phase::Submitted : Phase::Invalid)
                : completed > submitted && waterline >= seq ? Phase::Completed : Phase::Invalid;
            check(renderTapeIdentityV3SourcePhase(row) == expected,
                  "source progress refines successful publication/submission/completion order");
            RenderTapeIdentityV3Initializer init{
                .sequence = seq, .epoch = 7u, .publicationEdge = published,
                .submissionEdge = submitted, .completionEdge = completed,
                .completedSequence = waterline};
            check(renderTapeIdentityV3InitializerPhase(init) == expected,
                  "initializer has the same transition algebra, not the render waterline");
            for (std::uint64_t edge = 0u; edge <= 5u; ++edge) {
              auto candidate = row;
              auto upload = init;
              const bool submit = expected == Phase::Published && edge > published;
              check(renderTapeIdentityV3SubmitSource(candidate, edge) == submit &&
                        renderTapeIdentityV3SubmitInitializer(upload, edge) == submit,
                    "submission is admitted only once after publication");
              for (std::uint64_t receiptWaterline = 0u; receiptWaterline <= 2u; ++receiptWaterline) {
                candidate = row;
                upload = init;
                const bool complete = expected == Phase::Submitted && edge > submitted && receiptWaterline >= seq;
                check(renderTapeIdentityV3CompleteSource(candidate, edge, receiptWaterline) == complete &&
                          renderTapeIdentityV3CompleteInitializer(upload, edge, receiptWaterline) == complete,
                      "completion is admitted only after actual submission and covered sequence");
              }
            }
          }
}

void exhaustiveBindingAndCurrentIssue() {
  // Bind truth table: two queues/slots/generations/storage generations,
  // END cursors {0..3}, two capture IDs, and actual sequence {0,11}.
  for (std::uint64_t queue = 1u; queue <= 2u; ++queue)
    for (std::uint32_t slot = 0u; slot <= 1u; ++slot)
      for (std::uint64_t generation : {9u, 10u})
        for (std::uint64_t storage : {7u, 8u})
          for (std::uint32_t cursor = 0u; cursor <= 3u; ++cursor)
            for (std::uint64_t id : {1u, 2u})
              for (std::uint64_t sequence : {0u, 11u}) {
                auto row = end(Prefix::Unbound);
                row.commandEnd = cursor;
                row.writerCaptureId = id;
                auto published = source();
                published.seqId = sequence;
                const dxmt9::query::WriterIdentity owner{queue, slot, generation, storage};
                const bool expected = owner == kWriter && cursor && cursor <= 2u && id == 1u && sequence;
                check(renderTapeIdentityV3BindPrefixAdmitted(row, published, kWriter, owner) == expected,
                      "actual publication binds only the exact retained generation and frozen END cursor");
              }
  // Current issue domain: each of token/epoch/wire generation/cycle/ordinal/
  // device epoch/cancellation has two values. Cross all prefix, render phase,
  // independent initializer phase and observation values.
  for (std::uint32_t mask = 0u; mask < 128u; ++mask)
    for (Prefix prefix : {Prefix::Unbound, Prefix::EmptyBaseline, Prefix::ArmBaseline, Prefix::RealSource})
      for (Phase phase : {Phase::Published, Phase::Submitted, Phase::Completed})
        for (bool initializerComplete : {false, true})
          for (Observation observation : {Observation::Pending, Observation::Ready, Observation::Completed}) {
            auto h = header(prefix != Prefix::ArmBaseline);
            auto row = end(prefix);
            row.dispatchEdge = 9u;
            row.prefixInitializer = 5u;
            row.prefixInitializerEpoch = 7u;
            row.initializerIndex = 0u;
            auto s = source();
            if (phase != Phase::Completed) { s.completionEdge = 0u; s.completedSequence = 0u; }
            if (phase == Phase::Published) s.submissionEdge = 0u;
            RenderTapeIdentityV3Initializer initializer{
                .sequence = 5u, .epoch = 7u, .publicationEdge = 6u, .submissionEdge = 7u};
            if (initializerComplete) { initializer.completionEdge = 8u; initializer.completedSequence = 5u; }
            auto issue = current(row);
            if (mask & 1u) ++issue.capture.token;
            if (mask & 2u) ++issue.capture.epoch;
            if (mask & 4u) ++issue.identity.generation;
            if (mask & 8u) ++issue.cycle;
            if (mask & 16u) ++issue.ordinal;
            if (mask & 32u) ++issue.deviceEpoch;
            issue.cancelled = (mask & 64u) != 0u;
            const bool expected = !mask && (observation == Observation::Pending ||
                (prefix != Prefix::Unbound && initializerComplete &&
                 (prefix != Prefix::RealSource || phase == Phase::Completed)));
            check(renderTapeIdentityV3ObservationAdmitted(h, row, issue,
                      std::span(&s, 1u), std::span(&initializer, 1u), observation) == expected,
                  "Ready requires current issue AND independent render/initializer completion");
          }
}

void exactPrefixAndCrossEventWriter() {
  const std::array events{RenderTapeIdentityV3EventFact{2u, 5u, 3u}};
  const std::array records{
      RenderTapeIdentityV3RecordFact{2u, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE},
      queryFact(2u, 1u),
      RenderTapeIdentityV3RecordFact{2u, 2u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE}};
  RenderTapeIdentityV3View identity{};
  identity.header = header();
  identity.sources.push_back(source());
  identity.commands = {drawCommand(0u, 0u), drawCommand(0u, 1u)};
  auto query = end(Prefix::RealSource);
  query.recordIndex = 1u;
  identity.coordinators.push_back(query);
  identity.dispositions = {gpu(2u, 0u, 0u), coordinator(2u, 1u), gpu(2u, 2u, 1u)};
  identity.settlements.push_back({2u, 20u, 4u, 6u, 3u});
  check(validateRenderTapeIdentityV3Rows(identity, events, records) == Status::Valid,
        "Draw-END-youngerDraw shares one real source while END excludes younger command");
  for (std::uint32_t cursor : {0u, 2u, 3u}) {
    auto malformed = identity;
    malformed.coordinators[0].commandEnd = cursor;
    check(validateRenderTapeIdentityV3Rows(malformed, events, records) != Status::Valid,
          "END endpoint must equal exact provenance witness, not an arbitrary in-range cursor");
  }
  auto missing = identity;
  missing.dispositions.pop_back();
  check(validateRenderTapeIdentityV3Rows(missing, events, records) != Status::Valid,
        "missing canonical coverage rejects the entire capture");
  auto overlap = identity;
  overlap.dispositions[2].commandIndex = 0u;
  check(validateRenderTapeIdentityV3Rows(overlap, events, records) != Status::Valid,
        "overlapping physical command witnesses reject");
  auto gap = identity;
  gap.sources[0].commandCount = 3u;
  check(validateRenderTapeIdentityV3Rows(gap, events, records) != Status::Valid,
        "unrepresented internal command gap cannot silently shift an END");
  const std::array emptyEndEvents{RenderTapeIdentityV3EventFact{2u, 5u, 2u}};
  const std::array emptyEndRecords{queryFact(2u, 0u),
      RenderTapeIdentityV3RecordFact{2u, 1u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE}};
  auto futureSource = identity;
  futureSource.sources[0].commandCount = 1u;
  futureSource.commands.resize(1u);
  futureSource.coordinators[0].recordIndex = 0u;
  futureSource.coordinators[0].commandEnd = 0u;
  futureSource.dispositions = {coordinator(2u, 0u), gpu(2u, 1u, 0u)};
  futureSource.settlements[0].recordCount = 2u;
  check(validateRenderTapeIdentityV3Rows(futureSource, emptyEndEvents, emptyEndRecords) != Status::Valid,
        "empty END cannot bind cursor zero to a future younger-draw source");

  const std::array twoEvents{RenderTapeIdentityV3EventFact{2u, 5u, 1u},
                            RenderTapeIdentityV3EventFact{3u, 5u, 1u},
                            RenderTapeIdentityV3EventFact{4u, 6u, 0u}};
  const std::array twoRecords{
      RenderTapeIdentityV3RecordFact{2u, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE},
      RenderTapeIdentityV3RecordFact{3u, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE}};
  identity.coordinators.clear();
  identity.dispositions = {gpu(2u, 0u, 0u), gpu(3u, 0u, 1u)};
  identity.sources[0].cutEventOrdinal = 4u;
  identity.sources[0].cutKind = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::BeforeEvent);
  identity.settlements = {{2u, 20u, 4u, 2u, 1u}, {3u, 21u, 4u, 6u, 1u}};
  check(validateRenderTapeIdentityV3Rows(identity, twoEvents, twoRecords) == Status::Valid &&
            identity.sources.size() == 1u,
        "two events share one actual writer/source and publication trigger is separate ordered-control event");
  auto duplicate = identity;
  duplicate.sources.push_back(duplicate.sources[0]);
  check(validateRenderTapeIdentityV3Rows(duplicate, twoEvents, twoRecords) != Status::Valid,
        "per-event duplicate source/completion rows are not an identity grammar");
  identity.sources[0].cutKind = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterRecord);
  identity.sources[0].cutRecord = 1u;
  check(validateRenderTapeIdentityV3Rows(identity, twoEvents, twoRecords) == Status::InvalidCut,
        "record cuts cannot be fabricated inside a non-command event");
}

void noPublicationAndIndependentInitializers() {
  const std::array events{RenderTapeIdentityV3EventFact{2u, 5u, 1u}};
  const std::array records{queryFact(2u, 0u)};
  RenderTapeIdentityV3View identity{};
  identity.header = header();
  identity.coordinators = {end()};
  identity.dispositions = {coordinator(2u, 0u)};
  identity.settlements = {{2u, 20u, 4u, 3u, 1u}};
  check(validateRenderTapeIdentityV3Rows(identity, events, records) == Status::Valid &&
            identity.sources.empty(),
        "empty END settles replay exactly once without source, pass, submission or GPU-completion fiction");
  auto fabricated = identity;
  fabricated.dispositions[0].commandIndex = 0u;
  check(validateRenderTapeIdentityV3Rows(fabricated, events, records) != Status::Valid,
        "coordinator disposition cannot fabricate a physical command reference");
  identity.initializers.push_back({.sequence = 5u, .epoch = 7u,
      .publicationEdge = 4u, .submissionEdge = 5u, .cutEventOrdinal = 2u,
      .cutKind = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterRecord), .cutRecord = 1u});
  auto& query = identity.coordinators[0];
  query.dispatchEdge = 6u;
  identity.settlements[0].replayEdge = 8u;
  query.prefixInitializer = 5u;
  query.prefixInitializerEpoch = 7u;
  query.initializerIndex = 0u;
  check(renderTapeIdentityV3ObservationAdmitted(identity.header, query, current(query), {},
            identity.initializers, Observation::Pending) &&
            !renderTapeIdentityV3ObservationAdmitted(identity.header, query, current(query), {},
                identity.initializers, Observation::Ready),
        "initializer-only render sequence zero is Pending until independent upload completion");
  check(renderTapeIdentityV3CompleteInitializer(identity.initializers[0], 7u, 5u) &&
            renderTapeIdentityV3ObservationAdmitted(identity.header, query, current(query), {},
                identity.initializers, Observation::Ready),
        "explicit EmptyBaseline plus completed post-arm initializer admits Ready");
  check(validateRenderTapeIdentityV3Rows(identity, events, records) == Status::Valid,
        "initializer publication precedes successful END dispatch at its actual record cut");
  auto futureInitializer = identity;
  futureInitializer.initializers[0].publicationEdge = 9u;
  futureInitializer.initializers[0].submissionEdge = 10u;
  futureInitializer.initializers[0].completionEdge = 11u;
  check(validateRenderTapeIdentityV3Rows(futureInitializer, events, records) != Status::Valid,
        "END cannot depend on a future initializer publication edge");
  const std::array laterEvents{events[0], RenderTapeIdentityV3EventFact{3u, 6u, 0u}};
  futureInitializer = identity;
  futureInitializer.initializers[0].cutEventOrdinal = 3u;
  futureInitializer.initializers[0].cutKind = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::BeforeEvent);
  futureInitializer.initializers[0].cutRecord = 0u;
  check(validateRenderTapeIdentityV3Rows(futureInitializer, laterEvents, records) != Status::Valid,
        "initializer dependency cannot be published at a younger tape trigger despite forged early edge");
  query.prefixKind = static_cast<std::uint32_t>(Prefix::ArmBaseline);
  query.prefixSequence = 9u;
  identity.header = header(false);
  identity.initializers[0].completionEdge = 0u;
  identity.initializers[0].completedSequence = 0u;
  check(!renderTapeIdentityV3ObservationAdmitted(identity.header, query, current(query), {},
            identity.initializers, Observation::Ready),
        "already completed render S does not complete newly pending initializer U");
  check(renderTapeIdentityV3CompleteInitializer(identity.initializers[0], 7u, 5u),
        "actual initializer completion has separate acknowledgment");
  identity.initializers[0].epoch = 8u;
  check(!renderTapeIdentityV3ObservationAdmitted(identity.header, query, current(query), {},
            identity.initializers, Observation::Ready), "stale initializer owner epoch rejects Ready");
}

void sharedDrawRunAndPhysicalCoverage() {
  const std::array events{RenderTapeIdentityV3EventFact{2u, 5u, 3u}};
  const std::array records{
      RenderTapeIdentityV3RecordFact{2u, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE},
      RenderTapeIdentityV3RecordFact{2u, 1u, D9C_COMMAND_RECORD_SET_VS_CONST_F},
      RenderTapeIdentityV3RecordFact{2u, 2u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE}};
  RenderTapeIdentityV3View identity{};
  identity.header = header();
  identity.sources = {source(1u)};
  identity.commands = {drawCommand(0u, 0u, 2u)};
  identity.dispositions = {gpu(2u, 0u, 0u, 0u), state(2u, 1u), gpu(2u, 2u, 0u, 1u)};
  identity.settlements = {{2u, 20u, 4u, 6u, 3u}};
  check(validateRenderTapeIdentityV3Rows(identity, events, records) == Status::Valid &&
            identity.commands.size() == 1u && identity.sources.size() == 1u,
        "Draw-Const-Draw partitions two actual subdraws in one physical DrawRun without a split");
  // Bind exact subdraw conservation to the production validator, not a
  // second test-only implementation: every bounded slice pair and extent.
  for (std::uint32_t extent = 1u; extent <= 3u; ++extent)
    for (std::uint32_t first = 0u; first <= 3u; ++first)
      for (std::uint32_t firstCount = 0u; firstCount <= 3u; ++firstCount)
        for (std::uint32_t second = 0u; second <= 3u; ++second)
          for (std::uint32_t secondCount = 0u; secondCount <= 3u; ++secondCount) {
            auto candidate = identity;
            candidate.commands[0].drawCount = extent;
            candidate.dispositions[0].firstSubdraw = first;
            candidate.dispositions[0].subdrawCount = firstCount;
            candidate.dispositions[2].firstSubdraw = second;
            candidate.dispositions[2].subdrawCount = secondCount;
            const bool expected = extent == 2u && first == 0u && firstCount == 1u &&
                second == 1u && secondCount == 1u;
            check((validateRenderTapeIdentityV3Rows(candidate, events, records) == Status::Valid) == expected,
                  "subdraw slices admit only exact nonoverlapping ordered cover of actual DrawRun extent");
          }
  for (std::uint32_t mutation = 0u; mutation < 6u; ++mutation) {
    auto candidate = identity;
    if (mutation == 0u) candidate.commands.push_back(candidate.commands[0]);
    if (mutation == 1u) candidate.commands[0].physicalCommandIndex = 1u;
    if (mutation == 2u) candidate.commands[0].kind = UINT32_MAX;
    if (mutation == 3u) candidate.commands[0].sourceIndex = 1u;
    if (mutation == 4u) candidate.sources[0].commandCount = 2u;
    if (mutation == 5u) candidate.dispositions[2].commandIndex = 1u;
    check(validateRenderTapeIdentityV3Rows(candidate, events, records) != Status::Valid,
          "actual command duplicate/gap/kind/source/extent/reference forgery rejects");
  }
  auto opcodeMismatch = records;
  opcodeMismatch[2].recordType = D9C_COMMAND_RECORD_CLEAR;
  check(validateRenderTapeIdentityV3Rows(identity, events, opcodeMismatch) != Status::Valid,
        "canonical GPU opcode must match stable actual command kind");
  auto reversed = identity;
  reversed.sources[0].commandCount = 2u;
  reversed.commands = {drawCommand(0u, 0u), drawCommand(0u, 1u)};
  reversed.dispositions[0].commandIndex = 1u;
  reversed.dispositions[2].firstSubdraw = 0u;
  check(validateRenderTapeIdentityV3Rows(reversed, events, records) != Status::Valid,
        "canonical contributors cannot reverse physical command order");
  auto interleaved = identity;
  interleaved.sources[0].commandCount = 2u;
  auto clear = drawCommand(0u, 1u);
  clear.kind = static_cast<std::uint32_t>(CommandKind::Clear);
  clear.drawCount = 0u;
  interleaved.commands.push_back(clear);
  interleaved.dispositions[1] = gpu(2u, 1u, 1u);
  interleaved.dispositions[1].subdrawCount = 0u;
  auto clearRecords = records;
  clearRecords[1].recordType = D9C_COMMAND_RECORD_CLEAR;
  check(validateRenderTapeIdentityV3Rows(interleaved, events, clearRecords) != Status::Valid,
        "DrawRun cannot straddle a distinct effectful physical command");
  for (std::uint32_t flags : {1u, 2u}) {
    auto straddled = identity;
    auto query = end(flags == 1u ? Prefix::RealSource : Prefix::None);
    query.recordIndex = 1u;
    query.flags = flags;
    straddled.coordinators = {query};
    straddled.dispositions[1] = coordinator(2u, 1u);
    auto queryRecords = records;
    queryRecords[1] = queryFact(2u, 1u);
    queryRecords[1].queryFlags = flags;
    check(validateRenderTapeIdentityV3Rows(straddled, events, queryRecords) != Status::Valid,
          "one actual DrawRun cannot span BEGIN or END even when control generates no command");
  }
}

void coordinatorGeneratedCommands() {
  const std::array events{RenderTapeIdentityV3EventFact{2u, 5u, 1u}};
  constexpr auto submit = static_cast<std::uint32_t>(
      dxmt9::core::metalqueue::SessionReleaseAction::SubmitSession);
  const std::array records{RenderTapeIdentityV3RecordFact{
      .eventOrdinal = 2u, .recordIndex = 0u,
      .recordType = D9C_COMMAND_RECORD_UPDATE_TEXTURE,
      .requiredReleaseAction = submit}};
  RenderTapeIdentityV3View identity{};
  identity.header = header();
  identity.sources = {source(2u)};
  RenderTapeIdentityV3Coordinator control{
      .eventOrdinal = 2u, .rawOrdinal = 20u, .buildGeneration = 4u,
      .dispatchEdge = 2u, .releaseOrdinal = 1u, .releaseGeneration = 2u,
      .recordType = D9C_COMMAND_RECORD_UPDATE_TEXTURE, .releaseAction = submit};
  identity.coordinators = {control};
  RenderTapeIdentityV3Command generated{
      .sourceIndex = 0u, .kind = static_cast<std::uint32_t>(CommandKind::SurfaceCopy),
      .membership = static_cast<std::uint32_t>(RenderTapeIdentityV3Membership::NoPassCommand),
      .coordinatorIndex = 0u};
  identity.commands = {generated, generated};
  identity.commands[1].physicalCommandIndex = 1u;
  identity.dispositions = {coordinator(2u, 0u)};
  identity.settlements = {{2u, 20u, 4u, 6u, 1u}};
  check(validateRenderTapeIdentityV3Rows(identity, events, records) == Status::Valid,
        "coordinator-produced actual commands have one owner each and one source completion, not fake canonical GPU rows");
  auto duplicate = identity;
  duplicate.commands[1].physicalCommandIndex = 0u;
  check(validateRenderTapeIdentityV3Rows(duplicate, events, records) != Status::Valid,
        "generated command physical ownership cannot overlap");
  auto missingOwner = identity;
  missingOwner.commands[1].coordinatorIndex = kRenderTapeIdentityV3NoIndex;
  check(validateRenderTapeIdentityV3Rows(missingOwner, events, records) != Status::Valid,
        "unrepresented generated actual command cannot be silently covered by coordinator release");
  auto autogen = identity;
  autogen.commands[1].kind = static_cast<std::uint32_t>(CommandKind::GenerateMipmaps);
  check(validateRenderTapeIdentityV3Rows(autogen, events, records) == Status::Valid,
        "UpdateTexture optional autogen mipmap uses an evidenced generated command kind");
  for (std::uint32_t record : {D9C_COMMAND_RECORD_QUERY_ISSUE,
                              D9C_COMMAND_RECORD_READBACK,
                              D9C_COMMAND_RECORD_UPDATE_TEXTURE})
    for (std::uint32_t kind = 0u; kind <= 10u; ++kind) {
      const bool expected = record == D9C_COMMAND_RECORD_UPDATE_TEXTURE &&
          (kind == static_cast<std::uint32_t>(CommandKind::SurfaceCopy) ||
           kind == static_cast<std::uint32_t>(CommandKind::GenerateMipmaps));
      check(renderTapeIdentityV3CoordinatorCommandAdmitted(
                record, static_cast<CommandKind>(kind)) == expected,
            "generated command ownership is fail-closed to evidenced coordinator-kind mappings");
      auto fabricated = identity;
      fabricated.coordinators[0].recordType = record;
      fabricated.commands[0].kind = kind;
      fabricated.commands[0].drawCount = kind == static_cast<std::uint32_t>(CommandKind::DrawRun) ? 1u : 0u;
      if (!expected)
        check(validateRenderTapeIdentityV3Rows(fabricated, events, records) != Status::Valid,
              "coordinator cannot invent DrawRun/Clear/unknown/standalone-readback physical commands");
    }
}

void ledgerDeferredAndFailureTraces() {
  const RenderTapeIdentityV3Limits limits{
      .maxSources = 1u, .maxInitializers = 2u, .maxCommands = 8u, .maxDispositions = 8u,
      .maxCoordinators = 2u, .maxSettlements = 2u};
  RenderTapeIdentityV3Ledger ledger;
  check(ledger.begin(header(), limits), "bounded ledger arms");
  auto first = end(Prefix::Unbound);
  first.commandEnd = 3u;
  first.prefixInitializer = 2u;
  first.prefixInitializerEpoch = 7u;
  std::uint32_t firstIndex = UINT32_MAX;
  check(ledger.dispatchCoordinator(kKey, first, kWriter, &firstIndex) &&
            ledger.observeQuery(kKey, firstIndex, current(first), Observation::Pending),
        "pending END stores immutable generation-qualified cursor without publication");
  RenderTapeIdentityV3View snapshot;
  check(ledger.snapshot(kKey, snapshot) && snapshot.sources.empty(),
        "END association does not publish or invent a source");
  RenderTapeIdentityV3Initializer initializer{
      .sequence = 5u, .epoch = 7u, .cutEventOrdinal = 3u,
      .cutKind = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterRecord), .cutRecord = 1u};
  std::uint32_t initializerIndex = UINT32_MAX;
  check(ledger.publishInitializer(kKey, initializer, &initializerIndex) &&
            ledger.submitInitializer(kKey, initializerIndex), "actual new upload prefix is independently submitted");
  auto second = end(Prefix::Unbound);
  second.eventOrdinal = 3u;
  second.rawOrdinal = 21u;
  second.commandEnd = 4u;
  second.issueCycle = 2u;
  second.issueOrdinal = 3u;
  second.prefixInitializer = 5u;
  second.prefixInitializerEpoch = 7u;
  second.initializerIndex = initializerIndex;
  std::uint32_t secondIndex = UINT32_MAX;
  check(ledger.dispatchCoordinator(kKey, second, kWriter, &secondIndex),
        "second END shares writer but retains distinct command/initializer prefix");
  auto published = source(4u);
  published.submissionEdge = published.completionEdge = published.completedSequence = 0u;
  published.cutEventOrdinal = 3u;
  std::uint32_t sourceIndex = UINT32_MAX;
  check(ledger.publishSource(kKey, published, kWriter, &sourceIndex) &&
            ledger.bindQueryPrefix(kKey, firstIndex, sourceIndex) &&
            ledger.bindQueryPrefix(kKey, secondIndex, sourceIndex) &&
            ledger.submitSource(kKey, sourceIndex) && ledger.completeSource(kKey, sourceIndex, 11u),
        "successful publication binds both ENDs once; source submits/completes once");
  check(ledger.snapshot(kKey, snapshot) && snapshot.sources.size() == 1u &&
            snapshot.coordinators[firstIndex].commandEnd == 3u &&
            snapshot.coordinators[secondIndex].commandEnd == 4u,
        "late binding must not replace END cursors with final source extent");
  check(renderTapeIdentityV3ObservationAdmitted(snapshot.header, snapshot.coordinators[firstIndex],
            current(first), snapshot.sources, snapshot.initializers, Observation::Completed) &&
            !renderTapeIdentityV3ObservationAdmitted(snapshot.header, snapshot.coordinators[secondIndex],
                current(second), snapshot.sources, snapshot.initializers, Observation::Completed),
        "same render seq, init2 complete/init5 pending: only first END completes");
  check(ledger.completeInitializer(kKey, initializerIndex, 5u) &&
            ledger.observeQuery(kKey, secondIndex, current(second), Observation::Completed),
        "second END completes only at its actual independent initializer edge");
  check(!ledger.completeSource(kKey, sourceIndex, 11u) && ledger.failed() &&
            !ledger.snapshot(kKey, snapshot) && snapshot.sources.empty(),
        "duplicate completion fails sticky and exposes no partial usable identity");

  for (std::uint32_t failure = 0u; failure < 5u; ++failure) {
    RenderTapeIdentityV3Ledger rejected;
    check(rejected.begin(header(), limits), "negative trace starts fresh cold owner");
    auto row = end(Prefix::Unbound);
    std::uint32_t index = UINT32_MAX;
    check(rejected.dispatchCoordinator(kKey, row, kWriter, &index), "admitted END fixture");
    if (failure == 0u) {
      auto issue = current(row); ++issue.ordinal;
      check(!rejected.observeQuery(kKey, index, issue, Observation::Pending), "reissue invalidates old observation authority");
    } else if (failure == 1u) {
      auto issue = current(row); ++issue.deviceEpoch;
      check(!rejected.observeQuery(kKey, index, issue, Observation::Pending), "reset invalidates device epoch");
    } else if (failure == 2u) {
      check(!rejected.settleReplay({kKey.token, kKey.epoch + 1u}, {2u, 20u, 4u, 0u, 1u}),
            "stale capture epoch rejects settlement");
    } else if (failure == 3u) {
      auto wrongWriter = kWriter; ++wrongWriter.generation;
      auto s = source(); s.submissionEdge = s.completionEdge = s.completedSequence = 0u;
      s.writerGeneration = 0u;
      s.storageGeneration = 0u;
      check(rejected.publishSource(kKey, s, wrongWriter) && !rejected.bindQueryPrefix(kKey, index, 0u),
            "recycled writer generation cannot bind admitted END");
    } else {
      check(!rejected.completeSource(kKey, 0u, 11u), "no-publication replay cannot acknowledge GPU completion");
    }
    check(rejected.failed() && !rejected.snapshot(kKey, snapshot), "authority failure is sticky");
  }
  RenderTapeIdentityV3Ledger capacity;
  auto small = limits; small.maxCoordinators = 1u;
  check(capacity.begin(header(), small) && capacity.dispatchCoordinator(kKey, end()) &&
            !capacity.dispatchCoordinator(kKey, end()) && capacity.failed() &&
            !capacity.snapshot(kKey, snapshot), "bounded capacity rejects capture, never partial coverage");
}

void ledgerActualCommandTraces() {
  const RenderTapeIdentityV3Limits limits{
      .maxSources = 1u, .maxInitializers = 0u, .maxCommands = 1u,
      .maxDispositions = 3u, .maxCoordinators = 0u, .maxSettlements = 1u};
  RenderTapeIdentityV3Ledger ledger;
  auto published = source(1u);
  published.submissionEdge = published.completionEdge = published.completedSequence = 0u;
  check(ledger.begin(header(), limits) && ledger.publishSource(kKey, published, kWriter) &&
            ledger.appendCommand(kKey, drawCommand(0u, 0u, 2u)) &&
            ledger.appendDisposition(kKey, gpu(2u, 0u, 0u, 0u), D9C_COMMAND_RECORD_DRAW_PRIMITIVE) &&
            ledger.appendDisposition(kKey, state(2u, 1u), D9C_COMMAND_RECORD_SET_VS_CONST_F) &&
            ledger.appendDisposition(kKey, gpu(2u, 2u, 0u, 1u), D9C_COMMAND_RECORD_DRAW_PRIMITIVE) &&
            ledger.settleReplay(kKey, {2u, 20u, 4u, 0u, 3u}) &&
            ledger.submitSource(kKey, 0u) && ledger.completeSource(kKey, 0u, 11u),
        "cold bounded ledger records two canonical draw slices with one actual command/source completion");
  RenderTapeIdentityV3View snapshot;
  const std::array events{RenderTapeIdentityV3EventFact{2u, 5u, 3u}};
  const std::array records{
      RenderTapeIdentityV3RecordFact{2u, 0u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE},
      RenderTapeIdentityV3RecordFact{2u, 1u, D9C_COMMAND_RECORD_SET_VS_CONST_F},
      RenderTapeIdentityV3RecordFact{2u, 2u, D9C_COMMAND_RECORD_DRAW_PRIMITIVE}};
  check(ledger.snapshot(kKey, snapshot) && snapshot.commands.size() == 1u &&
            validateRenderTapeIdentityV3Rows(snapshot, events, records) == Status::Valid,
        "ledger snapshot owns command rows and passes exact final conservation validator");
  check(!ledger.appendCommand(kKey, drawCommand(0u, 1u)) && ledger.failed() &&
            !ledger.snapshot(kKey, snapshot),
        "actual command capacity/extent overrun fails sticky without partial usable capture");
}

std::size_t alignUp(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

std::vector<std::byte> oneRecord(std::uint32_t type, std::span<const std::byte> payload) {
  const auto recordsOffset = sizeof(D9CCommandChunkWireHeader);
  const auto handlesOffset = alignUp(recordsOffset + sizeof(D9CCommandChunkWireRecordHeader), 8u);
  const D9CCommandChunkWireHeader h{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = static_cast<std::uint32_t>(recordsOffset), .recordCount = 1u,
      .handleTableOffset = static_cast<std::uint32_t>(handlesOffset),
      .payloadArenaOffset = static_cast<std::uint32_t>(handlesOffset),
      .payloadArenaSize = static_cast<std::uint32_t>(payload.size())};
  const D9CCommandChunkWireRecordHeader record{.type = type,
      .payloadSize = static_cast<std::uint32_t>(payload.size())};
  std::vector<std::byte> bytes(handlesOffset + payload.size());
  std::memcpy(bytes.data(), &h, sizeof(h));
  std::memcpy(bytes.data() + recordsOffset, &record, sizeof(record));
  std::memcpy(bytes.data() + handlesOffset, payload.data(), payload.size());
  return bytes;
}

std::vector<std::byte> bootstrap() {
  std::array<D9CCommandChunkWireTextureBinding, D9C_DRAW_PACKET_MAX_TEXTURES> textures{};
  std::array<D9CCommandChunkWireStreamBinding, D9C_DRAW_PACKET_MAX_STREAMS> streams{};
  for (std::uint32_t i = 0u; i < textures.size(); ++i)
    textures[i] = {.slot = i, .valid = 1u, .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX};
  for (std::uint32_t i = 0u; i < streams.size(); ++i)
    streams[i] = {.slot = i, .valid = 1u, .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX};
  const auto table = sizeof(D9CCommandChunkWireDrawHeader);
  const auto data = alignUp(table + 2u * sizeof(D9CCommandChunkWireSectionDesc), 4u);
  const auto streamsOffset = data + sizeof(textures);
  const D9CCommandChunkWireDrawHeader state{
      .flags = D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT, .sectionCount = 2u,
      .sectionTableOffset = static_cast<std::uint32_t>(table),
      .sectionPayloadOffset = static_cast<std::uint32_t>(data)};
  const std::array sections{
      D9CCommandChunkWireSectionDesc{.kind = D9C_COMMAND_CHUNK_SECTION_TEXTURE,
          .elementSize = sizeof(D9CCommandChunkWireTextureBinding),
          .count = D9C_DRAW_PACKET_MAX_TEXTURES, .payloadOffset = static_cast<std::uint32_t>(data),
          .byteSize = sizeof(textures)},
      D9CCommandChunkWireSectionDesc{.kind = D9C_COMMAND_CHUNK_SECTION_STREAM,
          .elementSize = sizeof(D9CCommandChunkWireStreamBinding),
          .count = D9C_DRAW_PACKET_MAX_STREAMS, .payloadOffset = static_cast<std::uint32_t>(streamsOffset),
          .byteSize = sizeof(streams)}};
  std::vector<std::byte> bytes(streamsOffset + sizeof(streams));
  std::memcpy(bytes.data(), &state, sizeof(state));
  std::memcpy(bytes.data() + table, sections.data(), sizeof(sections));
  std::memcpy(bytes.data() + data, textures.data(), sizeof(textures));
  std::memcpy(bytes.data() + streamsOffset, streams.data(), sizeof(streams));
  return oneRecord(D9C_COMMAND_RECORD_APPLY_STATE, bytes);
}

void writeFixtureFile(const std::filesystem::path& path,
                      std::span<const std::byte> bytes) {
  std::ofstream stream(path, std::ios::binary);
  check(static_cast<bool>(stream), "inspection fixture opens a new artifact");
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  stream.close();
  check(static_cast<bool>(stream), "inspection fixture writes complete validated bytes");
}

void serializationAndV2Isolation(const std::filesystem::path& fixtureDirectory) {
  RenderTapeBuilder builder;
  builder.appendBootstrapState(bootstrap());
  const D9CWireObjectIdentity output{D9C_CHUNK_HANDLE_KIND_SURFACE, 1u, 51u};
  const RenderTapeSurfaceDescriptorV2 outputDescriptor{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(RenderTapeSurfaceStorage::SwapchainBackbuffer),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::ProducedPresentOutput),
      .surface = {.format = 21u, .resourceType = 1u, .usage = 1u,
                  .width = 4u, .height = 4u, .depth = 1u}};
  builder.appendObjectDefine(output,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&outputDescriptor, 1u)), 0u, {});
  const D9CCommandChunkWirePresent present{};
  builder.appendCommandChunk({.recordCount = 1u},
      oneRecord(D9C_COMMAND_RECORD_PRESENT, std::as_bytes(std::span(&present, 1u))));
  const RenderTapeOracleAttachment oracle{.identity = output,
      .descriptorKind = static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface)};
  builder.appendPresentComplete(3u, 30u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  const auto tape = builder.seal();
  const auto tapeValidation = validateRenderTape(tape, {});
  if (!tapeValidation.valid())
    std::cerr << "wire fixture status=" << renderTapeValidationStatusName(tapeValidation.status)
              << " event=" << tapeValidation.failedEventIndex
              << " chunk=" << static_cast<std::uint32_t>(tapeValidation.chunkStatus) << '\n';
  check(tapeValidation.valid(), "wire fixture uses production full-tape validation");
  RenderTapeIdentityV3View identity;
  identity.header = header();
  identity.header.frameId = 9007199254740993ull;
  identity.header.presentOrdinal = 3u;
  identity.sources = {source(1u)};
  identity.sources[0].sourceOrdinal = 9007199254740995ull;
  identity.sources[0].cutEventOrdinal = 3u;
  identity.commands = {{.sourceIndex = 0u,
      .kind = static_cast<std::uint32_t>(CommandKind::Present),
      .membership = static_cast<std::uint32_t>(RenderTapeIdentityV3Membership::NoPassCommand)}};
  auto presentIdentity = gpu(3u, 0u, 0u);
  presentIdentity.subdrawCount = 0u;
  identity.dispositions = {presentIdentity};
  identity.settlements = {{3u, 20u, 4u, 6u, 1u}};
  Status status{};
  auto bytes = buildRenderTapeIdentityV3(tape, {}, identity, &status);
  check(status == Status::Valid && !bytes.empty(), "v3 builder authenticates actual tape and bounded row grammar");
  RenderTapeIdentityV3View imported;
  check(validateRenderTapeIdentityV3(tape, {}, bytes, &imported) == Status::Valid &&
            imported.sources.size() == 1u && imported.sources[0].seqId == 11u &&
            imported.commands.size() == 1u && imported.commands[0].kind ==
                static_cast<std::uint32_t>(CommandKind::Present),
        "versioned little-endian identity round trips without pointers");
  for (std::uint32_t mutation = 0u; mutation < 6u; ++mutation) {
    auto corrupt = bytes;
    RenderTapeIdentityV3Header h{};
    std::memcpy(&h, corrupt.data(), sizeof(h));
    if (mutation == 0u) h.sources.offset = UINT32_MAX;
    if (mutation == 1u) h.initializers.count = UINT32_MAX;
    if (mutation == 2u) h.version = 2u;
    if (mutation == 3u) ++h.eventsBytes;
    if (mutation == 4u) h.commands.entrySize = 0u;
    if (mutation == 5u) h.reserved3 = 1u;
    std::memcpy(corrupt.data(), &h, sizeof(h));
    check(validateRenderTapeIdentityV3(tape, {}, corrupt, &imported) != Status::Valid &&
              imported.sources.empty(), "corrupt offset/count/version/digest context exposes no partial view");
  }
  const RenderTapeIdentitySource oldSource{.eventOrdinal = 3u, .sourceOrdinal = 7u,
      .seqId = 11u, .captureToken = 5u, .recordCount = 1u, .rangeCount = 1u};
  const RenderTapeIdentityRange oldRange{.eventOrdinal = 3u, .sourceOrdinal = 7u,
      .seqId = 11u, .logicalPassId = 17u, .recordCount = 1u, .passKind = 3u};
  const auto v2 = buildRenderTapeIdentity(tape, {}, 3u, 3u, 5u,
      RenderTapeIdentityAuthority::Capture, std::span(&oldSource, 1u), std::span(&oldRange, 1u));
  RenderTapeIdentityView oldView;
  check(!v2.empty() && validateRenderTapeIdentity(tape, {}, v2, &oldView).valid() &&
            oldView.header.version == 2u && oldView.sources[0].sourceOrdinal == 7u,
        "v2 bytes and original source/pass interpretation remain unchanged");
  check(!validateRenderTapeIdentity(tape, {}, bytes).valid() &&
            validateRenderTapeIdentityV3(tape, {}, v2) != Status::Valid,
        "neither reader silently reinterprets the other grammar");
  if (!fixtureDirectory.empty()) {
    check(std::filesystem::create_directories(fixtureDirectory),
          "inspection fixture output directory must be new; existing artifacts are never overwritten");
    writeFixtureFile(fixtureDirectory / "events.bin", tape);
    writeFixtureFile(fixtureDirectory / "identity.bin", bytes);
    writeFixtureFile(fixtureDirectory / "identity-v2.bin", v2);
  }
}

} // namespace

int main(int argc, char** argv) {
  try {
    std::filesystem::path fixtureDirectory;
    if (argc != 1) {
      check(argc == 3 && std::string_view(argv[1]) == "--write-inspection-fixture",
            "usage: render-tape-identity-v3-spec [--write-inspection-fixture <new-directory>]");
      fixtureDirectory = argv[2];
      check(!fixtureDirectory.empty(), "inspection fixture path must not be empty");
    }
    exhaustiveRegistryAndPublication();
    exhaustiveBindingAndCurrentIssue();
    exactPrefixAndCrossEventWriter();
    noPublicationAndIndependentInitializers();
    sharedDrawRunAndPhysicalCoverage();
    coordinatorGeneratedCommands();
    ledgerDeferredAndFailureTraces();
    ledgerActualCommandTraces();
    serializationAndV2Isolation(fixtureDirectory);
    // Exclusions: live observer scheduling, queue races/liveness, source-plan
    // consumption, Metal/resource bytes, query result bytes, pixels and wild
    // qualification. The finite checker binds only these production predicates.
    std::cout << "render-tape-identity-v3-spec: " << checks << " checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "render-tape-identity-v3-spec: " << error.what() << '\n';
    return 1;
  }
}
