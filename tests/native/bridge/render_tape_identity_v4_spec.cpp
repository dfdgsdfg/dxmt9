#include "render_tape_identity_v4_fixture.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace dxmt9::d3d9;
using Status = RenderTapeIdentityV4Status;
void check(bool value, std::string_view message) {
  if (!value) throw std::runtime_error(std::string(message));
}

void neutralProvenanceRoundTrip() {
  const auto fixture = test::makeIdentityV4Fixture();
  Status status{};
  const auto bytes = buildRenderTapeIdentityV4(fixture.tape, {}, fixture.identity, &status);
  RenderTapeIdentityV4View imported;
  check(status == Status::Valid && !bytes.empty() &&
            validateRenderTapeIdentityV4(fixture.tape, {}, bytes, &imported) == Status::Valid &&
            imported.commands.size() == 3u && imported.commands[0].drawCount == 1u &&
            imported.header.requiredExecutionTopologyVersion == 1u &&
            imported.attempts.size() == 3u && imported.header.headerSize == 264u &&
            imported.header.sources.entrySize == 120u && imported.header.settlements.entrySize == 32u &&
            imported.header.attempts.entrySize == 176u,
        "v4 authenticates neutral extents, distinct raw acknowledgment and mandatory actual transaction attempts");
  check(buildRenderTapeIdentityV4(fixture.tape, {}, imported, &status) == bytes,
        "v4 reserialization preserves exact admitted bytes");
  check(validateRenderTapeIdentityV3(fixture.tape, {}, bytes) == RenderTapeIdentityV3Status::InvalidHeader,
        "legacy v3 validator rejects v4 rather than converting neutral commands to fabricated passes");
}

void malformedProvenanceRejects() {
  const auto fixture = test::makeIdentityV4Fixture();
  Status status{};
  const auto original = buildRenderTapeIdentityV4(fixture.tape, {}, fixture.identity, &status);
  check(status == Status::Valid, "v4 negative fixture starts authenticated");
  for (std::uint32_t mutation = 0u; mutation < 23u; ++mutation) {
    auto bytes = original;
    RenderTapeIdentityV4Header header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    auto expected = Status::InvalidHeader;
    if (mutation == 0u) header.version = 3u;
    if (mutation == 1u) header.requiredExecutionTopologyVersion = 0u;
    if (mutation == 2u) header.reserved1 = 1u;
    if (mutation == 3u) { ++header.commands.offset; expected = Status::InvalidLayout; }
    if (mutation == 4u) { header.sources.count = kRenderTapeIdentityV4MaxRows + 1u; expected = Status::CapacityExceeded; }
    if (mutation == 5u) { header.eventsDigest[0] ^= std::byte{1}; expected = Status::EventsMismatch; }
    if (mutation == 6u) { ++header.eventsBytes; expected = Status::EventsMismatch; }
    if (mutation >= 7u && mutation <= 10u) {
      RenderTapeIdentityV4PhysicalCommand command{};
      std::memcpy(&command, bytes.data() + header.commands.offset, sizeof(command));
      expected = Status::InvalidDisposition;
      if (mutation == 7u) command.sourceIndex = UINT32_MAX;
      if (mutation == 8u) command.physicalCommandIndex = 1u;
      if (mutation == 9u) command.drawCount = 0u;
      if (mutation == 10u) command.coordinatorIndex = 0u;
      std::memcpy(bytes.data() + header.commands.offset, &command, sizeof(command));
    }
    if (mutation == 11u) {
      RenderTapeIdentityV4Source source{};
      std::memcpy(&source, bytes.data() + header.sources.offset, sizeof(source));
      source.lifecycle.submissionEdge = source.lifecycle.completionEdge =
          source.lifecycle.completedSequence = 0u;
      std::memcpy(bytes.data() + header.sources.offset, &source, sizeof(source));
      expected = Status::Incomplete;
    }
    if (mutation == 12u) { bytes.push_back(std::byte{}); expected = Status::InvalidLayout; }
    if (mutation == 13u) { ++header.attempts.offset; expected = Status::InvalidLayout; }
    if (mutation == 14u) { header.attempts.count = kRenderTapeIdentityV4MaxRows + 1u; expected = Status::CapacityExceeded; }
    if (mutation == 15u) { header.attempts.entrySize = 1u; expected = Status::InvalidLayout; }
    if (mutation == 16u) header.reserved3 = 1u;
    if (mutation == 17u) { header.settlements.entrySize = sizeof(RenderTapeIdentityV3ReplaySettlement); expected = Status::InvalidLayout; }
    if (mutation == 18u) { header.sources.entrySize = sizeof(RenderTapeIdentityV3Source); expected = Status::InvalidLayout; }
    if (mutation >= 19u && mutation <= 21u) {
      RenderTapeIdentityV4ReplayAttempt attempt{};
      std::memcpy(&attempt, bytes.data() + header.attempts.offset, sizeof(attempt));
      expected = Status::InvalidTransaction;
      if (mutation == 19u) attempt.terminalEdge = attempt.beginEdge;
      if (mutation == 20u) { attempt.receipt.kind = 0u; expected = Status::InvalidReceipt; }
      if (mutation == 21u) ++attempt.receipt.queueSequence;
      if (mutation == 21u) expected = Status::InvalidReceipt;
      std::memcpy(bytes.data() + header.attempts.offset, &attempt, sizeof(attempt));
    }
    if (mutation == 22u) {
      RenderTapeIdentityV4Source source{};
      std::memcpy(&source, bytes.data() + header.sources.offset, sizeof(source));
      source.reserved0 = 1u;
      std::memcpy(bytes.data() + header.sources.offset, &source, sizeof(source));
      expected = Status::InvalidSource;
    }
    std::memcpy(bytes.data(), &header, sizeof(header));
    auto imported = fixture.identity;
    check(validateRenderTapeIdentityV4(fixture.tape, {}, bytes, &imported) == expected &&
              imported.sources.empty() && imported.commands.empty() && imported.attempts.empty() &&
              imported.settlements.empty(),
          "v4 rejects corrupt seven-table layout, stale extents, false attempt receipts and incomplete lifecycle without partial output");
  }
  auto unused = fixture.identity;
  unused.coordinators.emplace_back();
  check(buildRenderTapeIdentityV4(fixture.tape, {}, unused, &status).empty() &&
            status == Status::InvalidCoordinator,
        "an unreferenced coordinator is rejected by the complete shared provenance grammar");
}

void externalControlProvenanceRoundTrip() {
  const auto fixture = test::makeIdentityV4Fixture();
  ImportedRenderTapeView imported;
  check(importPrevalidatedRenderTape(fixture.tape, imported), "external-control fixture imports actual tape");
  constexpr D9CWireObjectIdentity query{D9C_CHUNK_HANDLE_KIND_QUERY, 1u, 81u};
  const RenderTapeQueryDescriptorV3 descriptor{
      .initialIssueCycle = 1u,
      .checkpoint = {.checkpointVersion = D9C_RENDER_TAPE_EVENT_QUERY_CHECKPOINT_VERSION_1,
          .status = D9C_RENDER_TAPE_EVENT_QUERY_CHECKPOINT_COMPLETE,
          .queryType = 8u, .identity = query, .issueCycle = 1u,
          .coreQueryIdentity = 1u, .deviceEpoch = 1u, .issueOrdinal = 1u,
          .rawOrdinal = 1u, .queueIdentity = 9u}};
  for (std::uint32_t variant = 0u; variant < 2u; ++variant) {
    RenderTapeBuilder builder;
    const bool queryPoll = variant == 1u;
    for (std::size_t i = 0u; i < imported.events.size(); ++i) {
      const auto event = imported.event(i);
      const auto type = static_cast<RenderTapeEventType>(event.header.type);
      if (i == 4u) {
        if (queryPoll) {
          const RenderTapeQueryGetDataControlV2 poll{.issueCycle = 1u};
          builder.appendOrderedControl({.identity = query,
              .kind = static_cast<std::uint32_t>(RenderTapeControlKind::QueryGetData),
              .disposition = static_cast<std::uint32_t>(RenderTapeControlDisposition::Completed),
              .controlBytes = sizeof(poll), .completionOrdinal = 1u},
              std::as_bytes(std::span(&poll, 1u)));
        } else {
          const std::array<std::byte, kRenderTapeGammaRampBytes> gamma{};
          builder.appendOrderedControl({
              .kind = static_cast<std::uint32_t>(RenderTapeControlKind::GammaRampSet),
              .disposition = static_cast<std::uint32_t>(RenderTapeControlDisposition::Completed),
              .controlBytes = static_cast<std::uint32_t>(gamma.size()), .completionOrdinal = 1u}, gamma);
        }
      }
      if (type == RenderTapeEventType::PresentComplete) {
        auto payload = std::vector<std::byte>(event.payload.begin(), event.payload.end());
        RenderTapePresentCompleteHeader completion{};
        std::memcpy(&completion, payload.data(), sizeof(completion));
        completion.presentOrdinal = queryPoll ? 7u : 6u;
        completion.completionOrdinal = 2u;
        std::memcpy(payload.data(), &completion, sizeof(completion));
        builder.appendRawEvent(type, payload);
      } else builder.appendRawEvent(type, event.payload);
      if (queryPoll && i == 0u)
        builder.appendObjectDefine(query, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Query),
            std::as_bytes(std::span(&descriptor, 1u)), 0u, {});
    }
    const auto tape = builder.seal();
    check(validateRenderTape(tape, {}).valid(),
          "external-control fixture has canonical checkpoint/poll or gamma transcript evidence");
    auto identity = fixture.identity;
    identity.header.presentOrdinal = queryPoll ? 7u : 6u;
    for (std::uint32_t i = 0u; i < 3u; ++i) {
      const std::uint64_t event = i + 3u + (queryPoll ? 1u : 0u) + (i == 2u ? 1u : 0u);
      identity.sources[i].lifecycle.cutEventOrdinal = event;
      identity.dispositions[i].eventOrdinal = event;
      identity.settlements[i].eventOrdinal = identity.settlements[i].rawOrdinal = event;
    }
    test::setWholeRawCompatibilityAttempts(identity);
    Status status{};
    const auto bytes = buildRenderTapeIdentityV4(tape, {}, identity, &status);
    check(status == Status::Valid && !bytes.empty() &&
              validateRenderTapeIdentityV4(tape, {}, bytes) == Status::Valid &&
              identity.coordinators.empty(),
          "gamma and checkpoint-backed old-cycle polls need no fabricated captured QUERY_ISSUE coordinator");
    // This is provenance-only admission. The independent execution gate must
    // establish replay-local checkpoint/poll readiness before observing data.
  }
  const std::array events{RenderTapeIdentityV3EventFact{1u,
      static_cast<std::uint32_t>(RenderTapeEventType::OrderedControl), 0u}};
  RenderTapeIdentityV4View noCommands;
  noCommands.header = fixture.identity.header;
  check(validateRenderTapeIdentityV4Rows(noCommands, events, {}) == Status::Valid,
        "external-control facts do not manufacture command records or source lifecycle rows");
}

void coordinatorFailureWitness() {
  using Reason = TapeCoordinatorFailureReason;
  for (auto authority : {TapeCoordinatorAuthority::SessionRelease, TapeCoordinatorAuthority::OrdinarySerial}) {
    const auto original = test::makeIdentityV4CoordinatorFixture(authority);
    for (unsigned mutation = 0u; mutation < 4u; ++mutation) {
      auto fixture = original;
      auto expected = Reason::None;
      if (mutation == 1u) { ++fixture.identity.coordinators[0].provenance.buildGeneration; expected = Reason::AttemptJoin; }
      if (mutation == 2u) { fixture.identity.coordinators[0].provenance.deviceEpoch = 0u; expected = Reason::ShapeOrRecord; }
      if (mutation == 3u) { ++fixture.identity.coordinators[0].provenance.issueOrdinal; expected = Reason::QueryIdentity; }
      Status observed{}, unobserved{};
      TapeCoordinatorFailure failure{Reason::UnusedCoordinator, 123u};
      const auto withWitness = buildRenderTapeIdentityV4(fixture.tape, {}, fixture.identity, &observed, &failure);
      const auto withoutWitness = buildRenderTapeIdentityV4(fixture.tape, {}, fixture.identity, &unobserved);
      check(observed == unobserved && withWitness == withoutWitness && failure.reason == expected,
          "cold failure witness preserves exact admission and clears stale evidence on success");
      if (mutation) check(observed == Status::InvalidCoordinator && failure.coordinatorIndex == 0u,
          "rejected coordinator carries the original row index");
      if (mutation >= 2u) check(failure.fact.eventOrdinal == 4u && failure.fact.recordIndex == 0u &&
          failure.fact.queryType == 9u && failure.fact.queryFlags == 2u && failure.fact.issueOrdinal == 1u,
          "witness keeps the canonical fact, not the corrupt observed row");
    }
  }
}

void closedQueryOrdinalLossCounterexample(bool explicitHistory) {
  // H502/r4: a closed occlusion query starts its captured BEGIN at cycle
  // 509 / absolute Issue ordinal 1017. V2 preserves cycle 508, but has no
  // place for ordinal 1016. Do not make admission accept the lost history.
  auto fixture = test::makeIdentityV4CoordinatorFixture(TapeCoordinatorAuthority::OrdinarySerial);
  ImportedRenderTapeView imported;
  check(importPrevalidatedRenderTape(fixture.tape, imported), "closed-query fixture imports");
  RenderTapeBuilder builder;
  for (std::size_t index = 0u; index < imported.events.size(); ++index) {
    const auto event = imported.event(index);
    const auto type = static_cast<RenderTapeEventType>(event.header.type);
    if (type == RenderTapeEventType::ObjectDefine) {
      RenderTapeObjectDefineHeader definition{};
      std::memcpy(&definition, event.payload.data(), sizeof(definition));
      if (definition.identity.kind == D9C_CHUNK_HANDLE_KIND_QUERY) {
        const RenderTapeQueryDescriptorV2 descriptor{.type = 9u, .dataBytes = 4u,
            .initialState = static_cast<std::uint32_t>(
                RenderTapeQueryInitialState::ClosedWithoutReplayResult),
            .initialIssueCycle = 508u};
        const RenderTapeQueryDescriptorV4 complete{.type = descriptor.type,
            .dataBytes = descriptor.dataBytes, .initialState = descriptor.initialState,
            .initialIssueCycle = descriptor.initialIssueCycle, .initialIssueOrdinal = 1016u};
        builder.appendObjectDefine(definition.identity, definition.descriptorKind,
            explicitHistory ? std::as_bytes(std::span(&complete, 1u))
                            : std::as_bytes(std::span(&descriptor, 1u)), 0u, {});
        continue;
      }
    }
    builder.appendRawEvent(type, event.payload);
  }
  fixture.tape = builder.seal();
  for (std::size_t index = 0u; index < fixture.identity.coordinators.size(); ++index) {
    auto& query = fixture.identity.coordinators[index].provenance;
    query.issueCycle = 509u;
    query.issueOrdinal = 1017u + index;
  }
  check(validateRenderTape(fixture.tape, {}).valid(),
      "closed V2 history permits a new captured BEGIN/END, not an old result poll");
  Status status{};
  TapeCoordinatorFailure failure{};
  if (explicitHistory) {
    const auto bytes = buildRenderTapeIdentityV4(fixture.tape, {}, fixture.identity, &status, &failure);
    check(!bytes.empty() && status == Status::Valid &&
        failure.reason == TapeCoordinatorFailureReason::None &&
        validateRenderTapeIdentityV4(fixture.tape, {}, bytes) == Status::Valid,
        "explicit history preserves observed 509/1017 BEGIN and 509/1018 END without loosening equality");
    for (unsigned mutation = 0u; mutation < 3u; ++mutation) {
      auto invalid = fixture.identity;
      auto& row = invalid.coordinators[0].provenance;
      if (mutation == 0u) --row.issueOrdinal;
      if (mutation == 1u) ++row.queryIdentity.generation;
      if (mutation == 2u) ++row.issueCycle;
      check(buildRenderTapeIdentityV4(fixture.tape, {}, invalid, &status, &failure).empty() &&
          status == Status::InvalidCoordinator && failure.reason == TapeCoordinatorFailureReason::QueryIdentity,
          "explicit initial history cannot authorize stale ordinal, generation or cycle");
    }
    return;
  }
  check(buildRenderTapeIdentityV4(fixture.tape, {}, fixture.identity, &status, &failure).empty() &&
      status == Status::InvalidCoordinator &&
      failure.reason == TapeCoordinatorFailureReason::QueryIdentity &&
      failure.coordinatorIndex == 0u && failure.fact.queryFlags == 2u &&
      failure.fact.issueCycle == 509u && failure.fact.issueOrdinal == 1u &&
      fixture.identity.coordinators[0].provenance.issueOrdinal == 1017u,
      "real absolute ordinal must not be rewritten to the V2 capture-local ordinal");
}

void explicitQueryHistoryValidation() {
  const RenderTapeQueryDescriptorV4 original{.type = 9u, .dataBytes = 4u,
      .initialState = static_cast<std::uint32_t>(RenderTapeQueryInitialState::ClosedWithoutReplayResult),
      .initialIssueCycle = 508u, .initialIssueOrdinal = 1016u};
  for (unsigned mutation = 0u; mutation < 8u; ++mutation) {
    auto descriptor = original;
    if (mutation == 1u) descriptor.initialIssueOrdinal = 0u;
    if (mutation == 2u) descriptor.initialIssueOrdinal = 507u;
    if (mutation == 3u) descriptor.initialState = 0u;
    if (mutation == 4u) descriptor.initialState = 2u;
    if (mutation == 5u) descriptor.schemaVersion = 3u;
    if (mutation == 6u) descriptor.dataBytes = 8u;
    if (mutation == 7u) descriptor.initialIssueCycle = 0u;
    RenderTapeQueryDescriptorDecoded decoded{};
    check(renderTapeLoadQueryDescriptor(std::as_bytes(std::span(&descriptor, 1u)), decoded) == (mutation == 0u),
        "explicit query history validates version, type size, state and independent monotone counters");
  }
  RenderTapeQueryDescriptorDecoded decoded{};
  check(renderTapeLoadQueryDescriptor(std::as_bytes(std::span(&original, 1u)), decoded), "closed history decodes");
  auto state = renderTapeInitialQueryCycle(decoded);
  check(state.issueCycle == 508u && state.issueOrdinal == 1016u &&
      !state.active && !state.capturedCycle && !state.completedEventCheckpoint &&
      !renderTapeQueryPollCycleAdmitted(state, 508u, 0) &&
      !renderTapeQueryPollCycleAdmitted(state, 508u, 1) &&
      !renderTapeAdvanceQueryCycle(state, 1u, true),
      "history is not old-result authority and does not supply a missing occlusion BEGIN");
  check(renderTapeAdvanceQueryCycle(state, 2u, true) && state.issueCycle == 509u &&
      state.issueOrdinal == 1017u && !renderTapeQueryPollCycleAdmitted(state, 509u, 0) &&
      renderTapeAdvanceQueryCycle(state, 1u, true) && state.issueOrdinal == 1018u &&
      renderTapeQueryPollCycleAdmitted(state, 509u, 0),
      "captured BEGIN and END alone establish the next result cycle");
  state.issueOrdinal = UINT64_MAX;
  check(!renderTapeAdvanceQueryCycle(state, 2u, true) && state.issueOrdinal == UINT64_MAX,
      "absolute ordinal overflow fails closed without partial mutation");

  // Bounded refinement of the actual pure reducer: independent captured and
  // fresh-local histories see the same six-action Issue/poll trace.
  for (std::uint32_t history = 0u; history < 4096u; ++history) {
    auto captured = renderTapeInitialQueryCycle(decoded);
    RenderTapeQueryCycleState local{.type = 9u};
    auto actions = history;
    for (unsigned step = 0u; step < 6u; ++step, actions /= 4u) {
      const auto action = actions % 4u;
      if (action < 2u) {
        const auto flags = action + 1u;
        check(renderTapeAdvanceQueryCycle(captured, flags, true) ==
            renderTapeAdvanceQueryCycle(local, flags, true),
            "closed-history and fresh-local reducer admit identical Issue transitions");
      } else if (captured.capturedCycle) {
        check(renderTapeQueryPollCycleAdmitted(captured, captured.issueCycle, action - 2u) ==
            renderTapeQueryPollCycleAdmitted(local, local.issueCycle, action - 2u),
            "poll readiness shape refines only after a captured replacement cycle");
      } else {
        check(!renderTapeQueryPollCycleAdmitted(captured, captured.issueCycle, action - 2u),
            "pre-cut closed history never authorizes old Ready or Pending observations");
      }
      check(captured.issueCycle == original.initialIssueCycle + local.issueCycle &&
          captured.issueOrdinal == original.initialIssueOrdinal + local.issueOrdinal &&
          captured.active == local.active && captured.capturedCycle == local.capturedCycle,
          "independent cycle/ordinal offsets and active membership are invariant under every bounded trace");
    }
  }
}

void writeInspectionFixture(const std::filesystem::path& directory) {
  auto fixture = test::makeIdentityV4Fixture();
  constexpr auto wide = UINT64_C(9007199254740993);
  fixture.identity.header.frameId = wide;
  fixture.identity.header.captureToken = wide + 1u;
  fixture.identity.header.captureEpoch = wide + 2u;
  fixture.identity.header.queue = wide + 3u;
  fixture.identity.header.initializerEpoch = fixture.identity.header.queue;
  for (std::size_t i = 0u; i < fixture.identity.sources.size(); ++i)
    fixture.identity.sources[i].lifecycle.writerCaptureId = wide + 4u + i;
  fixture.topology.header.captureToken = fixture.identity.header.captureToken;
  fixture.topology.header.captureEpoch = fixture.identity.header.captureEpoch;
  fixture.topology.header.queue = fixture.identity.header.queue;
  fixture.topology.plans[0].planId = wide + 8u;
  test::setWholeRawCompatibilityAttempts(fixture.identity);
  const auto writeAt = [&](const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    stream.close();
  };
  const auto write = [&](const char* name, std::span<const std::byte> bytes) {
    writeAt(directory / name, bytes);
  };
  check(std::filesystem::create_directory(directory), "inspection fixture output must be absent");
  write("events.bin", fixture.tape);
  for (const bool alternate : {false, true}) {
    if (alternate) {
      ++fixture.identity.header.frameId;
      test::setWholeRawCompatibilityAttempts(fixture.identity);
    }
    Status identityStatus{};
    const auto identity = buildRenderTapeIdentityV4(fixture.tape, {}, fixture.identity, &identityStatus);
    RenderTapeExecutionTopologyV1Status topologyStatus{};
    const auto topology = buildRenderTapeExecutionTopologyV1(
        fixture.tape, {}, identity, fixture.topology, &topologyStatus);
    check(identityStatus == Status::Valid &&
              topologyStatus == RenderTapeExecutionTopologyV1Status::Valid,
          "inspection exports only genuinely admitted complete pairs");
    write(alternate ? "identity-alternate.bin" : "identity.bin", identity);
    write(alternate ? "execution_topology-alternate.bin" : "execution_topology.bin", topology);
  }
  for (const auto authority : {TapeCoordinatorAuthority::SessionRelease, TapeCoordinatorAuthority::OrdinarySerial}) {
    const auto named = test::makeIdentityV4CoordinatorFixture(authority);
    Status identityStatus{};
    const auto identity = buildRenderTapeIdentityV4(named.tape, {}, named.identity, &identityStatus);
    RenderTapeExecutionTopologyV1Status topologyStatus{};
    const auto topology = buildRenderTapeExecutionTopologyV1(named.tape, {}, identity, named.topology, &topologyStatus);
    check(identityStatus == Status::Valid && topologyStatus == RenderTapeExecutionTopologyV1Status::Valid &&
          named.identity.coordinators.size() == 2u,
        "nonempty coordinator inspection fixtures authenticate their explicit complete domain");
    const auto subdirectory = directory / (authority == TapeCoordinatorAuthority::SessionRelease
        ? "session-coordinator" : "ordinary-coordinator");
    check(std::filesystem::create_directory(subdirectory), "coordinator fixture directory must be absent");
    writeAt(subdirectory / "events.bin", named.tape);
    writeAt(subdirectory / "identity.bin", identity);
    writeAt(subdirectory / "execution_topology.bin", topology);
  }
}

} // namespace
int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string_view(argv[1]) == "--write-inspection-fixture") {
      writeInspectionFixture(argv[2]);
      return 0;
    }
    check(argc == 1, "v4 identity spec accepts only --write-inspection-fixture <absent-directory>");
    neutralProvenanceRoundTrip();
    malformedProvenanceRejects();
    externalControlProvenanceRoundTrip();
    coordinatorFailureWitness();
    closedQueryOrdinalLossCounterexample(false);
    closedQueryOrdinalLossCounterexample(true);
    explicitQueryHistoryValidation();
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "render tape v4 identity spec failed: " << error.what() << '\n'; return 1;
  }
}
