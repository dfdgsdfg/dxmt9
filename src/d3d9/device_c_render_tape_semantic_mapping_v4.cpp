#include "device_c_render_tape_semantic_mapping_v4.hpp"

#include <algorithm>
#include <array>
#include <tuple>
#include <utility>

namespace dxmt9::d3d9 {
namespace {

using Status = RenderTapeSemanticMappingV4Status;
using Kind = RenderTapeIdentityV3DispositionKind;
using Prefix = RenderTapeIdentityV3PrefixKind;
constexpr auto noIndex = kRenderTapeIdentityV4NoIndex;

bool limitsAdmitted(RenderTapeSemanticMappingV4Limits limits) noexcept {
  for (const auto limit : {limits.maxEvents, limits.maxRecords, limits.maxSources,
          limits.maxCommands, limits.maxDispositions, limits.maxCoordinators,
          limits.maxRaws, limits.maxAttempts, limits.maxCommandRelations,
          limits.maxSourceRelations})
    if (limit > kRenderTapeIdentityV4MaxRows) return false;
  return true;
}

bool extentsAdmitted(const RenderTapeIdentityV4View& identity,
    RenderTapeSemanticMappingV4Limits limits) noexcept {
  return identity.sources.size() <= limits.maxSources &&
      identity.initializers.size() <= limits.maxSources &&
      identity.commands.size() <= limits.maxCommands &&
      identity.dispositions.size() <= limits.maxDispositions &&
      identity.coordinators.size() <= limits.maxCoordinators &&
      identity.settlements.size() <= limits.maxRaws && identity.attempts.size() <= limits.maxAttempts;
}

auto identityKey(D9CWireObjectIdentity identity) noexcept {
  return std::tuple{identity.kind, identity.generation, identity.objectId};
}

bool sameNeutralFact(const RenderTapeIdentityV3RecordFact& original,
    const RenderTapeIdentityV3RecordFact& local) noexcept {
  return original.eventOrdinal == local.eventOrdinal && original.recordIndex == local.recordIndex &&
      original.recordType == local.recordType &&
      original.requiredReleaseAction == local.requiredReleaseAction &&
      original.queryFlags == local.queryFlags && original.queryType == local.queryType;
}

bool localFactsAdmitted(std::span<const RenderTapeIdentityV3RecordFact> original,
    std::span<const RenderTapeIdentityV3RecordFact> local) {
  if (original.size() != local.size()) return false;
  std::vector<std::uint32_t> queries;
  for (std::uint32_t index = 0u; index < original.size(); ++index) {
    const auto& a = original[index];
    const auto& b = local[index];
    if (!sameNeutralFact(a, b)) return false;
    if (a.recordType != D9C_COMMAND_RECORD_QUERY_ISSUE) {
      if (!renderTapeIdentityV3SameIdentity(a.queryIdentity, b.queryIdentity) ||
          a.issueCycle != b.issueCycle || a.issueOrdinal != b.issueOrdinal) return false;
      continue;
    }
    if (b.queryIdentity.kind != D9C_CHUNK_HANDLE_KIND_QUERY || !b.queryIdentity.generation ||
        !b.queryIdentity.objectId || !b.issueCycle || !b.issueOrdinal) return false;
    queries.push_back(index);
  }
  std::sort(queries.begin(), queries.end(), [&](auto left, auto right) {
    return std::tuple{identityKey(original[left].queryIdentity), left} <
        std::tuple{identityKey(original[right].queryIdentity), right};
  });
  std::vector<D9CWireObjectIdentity> localIdentities;
  for (std::size_t index = 0u; index < queries.size(); ++index) {
    const auto current = queries[index];
    if (!index || !renderTapeIdentityV3SameIdentity(original[current].queryIdentity,
            original[queries[index - 1u]].queryIdentity)) {
      localIdentities.push_back(local[current].queryIdentity);
      continue;
    }
    const auto previous = queries[index - 1u];
    const auto& a = original[current];
    const auto& priorA = original[previous];
    const auto& b = local[current];
    const auto& priorB = local[previous];
    // A checkpoint may translate the absolute cycle/ordinal. Subsequent
    // issues preserve the same canonical cycle steps and issue order.
    if (!renderTapeIdentityV3SameIdentity(b.queryIdentity, priorB.queryIdentity) ||
        a.issueCycle < priorA.issueCycle || b.issueCycle < priorB.issueCycle ||
        a.issueCycle - priorA.issueCycle != b.issueCycle - priorB.issueCycle ||
        a.issueOrdinal <= priorA.issueOrdinal || b.issueOrdinal <= priorB.issueOrdinal ||
        a.issueOrdinal - priorA.issueOrdinal != b.issueOrdinal - priorB.issueOrdinal)
      return false;
  }
  std::sort(localIdentities.begin(), localIdentities.end(), [](auto left, auto right) {
    return identityKey(left) < identityKey(right);
  });
  return std::adjacent_find(localIdentities.begin(), localIdentities.end(),
      renderTapeIdentityV3SameIdentity) == localIdentities.end();
}

bool coordinatorSemanticsMatch(const RenderTapeIdentityV4Coordinator& original,
    const RenderTapeIdentityV4Coordinator& candidate) noexcept {
  const auto& a = original.provenance;
  const auto& b = candidate.provenance;
  return original.authority == candidate.authority && a.eventOrdinal == b.eventOrdinal &&
      a.recordIndex == b.recordIndex && a.recordType == b.recordType &&
      a.flags == b.flags && a.queryType == b.queryType && a.dispatchResult == b.dispatchResult &&
      a.releaseAction == b.releaseAction && a.releaseWriter == b.releaseWriter;
}

struct PrefixCoverage {
  std::uint32_t records = 0u;
  std::uint32_t coordinatorCommands = 0u;
  // Canonical flattened record position plus one, not a source/cut cursor.
  std::uint32_t lastRecordEnd = 0u;
};

struct CoverageIndex {
  std::vector<PrefixCoverage> commands;
  std::vector<std::uint32_t> sourceBegins;
};

CoverageIndex coverageIndex(const RenderTapeIdentityV4View& identity,
    std::span<const RenderTapeSemanticMappingV4Record> records, bool captured) {
  CoverageIndex index;
  index.commands.resize(identity.commands.size() + 1u);
  index.sourceBegins.resize(identity.sources.size());
  for (std::uint32_t command = 0u; command < identity.commands.size(); ++command)
    if (!identity.commands[command].physicalCommandIndex)
      index.sourceBegins[identity.commands[command].sourceIndex] = command;
  std::vector<std::uint32_t> coordinatorPositions(identity.coordinators.size(), noIndex);
  for (std::uint32_t position = 0u; position < records.size(); ++position) {
    const auto& record = records[position];
    const auto command = captured ? record.capturedCommandIndex : record.candidateCommandIndex;
    const auto coordinator = captured ? record.capturedCoordinatorIndex : record.candidateCoordinatorIndex;
    if (command != noIndex) {
      auto& coverage = index.commands[command + 1u];
      ++coverage.records;
      coverage.lastRecordEnd = std::max(coverage.lastRecordEnd, position + 1u);
    }
    if (coordinator != noIndex) coordinatorPositions[coordinator] = position;
  }
  for (std::uint32_t command = 0u; command < identity.commands.size(); ++command) {
    const auto coordinator = identity.commands[command].coordinatorIndex;
    if (coordinator != noIndex) {
      auto& coverage = index.commands[command + 1u];
      ++coverage.coordinatorCommands;
      coverage.lastRecordEnd = coordinatorPositions[coordinator] + 1u;
    }
    auto& coverage = index.commands[command + 1u];
    const auto& previous = index.commands[command];
    coverage.records += previous.records;
    coverage.coordinatorCommands += previous.coordinatorCommands;
    coverage.lastRecordEnd = std::max(coverage.lastRecordEnd, previous.lastRecordEnd);
  }
  return index;
}

bool prefixCoverageAdmitted(const RenderTapeIdentityV3Coordinator& coordinator,
    const CoverageIndex& index, std::uint32_t recordPosition,
    std::uint32_t requiredRecords, std::uint32_t requiredCoordinatorCommands) noexcept {
  std::uint32_t end = 0u;
  const auto kind = static_cast<Prefix>(coordinator.prefixKind);
  if (kind == Prefix::RealSource) {
    if (coordinator.sourceIndex >= index.sourceBegins.size()) return false;
    end = index.sourceBegins[coordinator.sourceIndex] + coordinator.commandEnd;
  } else if (kind != Prefix::EmptyBaseline && kind != Prefix::ArmBaseline) return false;
  if (end >= index.commands.size()) return false;
  const auto& coverage = index.commands[end];
  // Each authenticated origin is unique. No younger origin plus equal counts
  // therefore proves the whole immutable pre-END semantic set, even across
  // previous sources and differently grouped DrawRuns.
  return coverage.lastRecordEnd <= recordPosition && coverage.records == requiredRecords &&
      coverage.coordinatorCommands == requiredCoordinatorCommands;
}

} // namespace

RenderTapeSemanticMappingV4::RenderTapeSemanticMappingV4(RenderTapeSemanticMappingV4&& other) noexcept {
  *this = std::move(other);
}

RenderTapeSemanticMappingV4& RenderTapeSemanticMappingV4::operator=(
    RenderTapeSemanticMappingV4&& other) noexcept {
  if (this == &other) return *this;
  status_ = other.status_;
  planDigest_ = other.planDigest_;
  candidateCapture_ = other.candidateCapture_;
  candidateQueue_ = other.candidateQueue_;
  records_ = std::move(other.records_);
  commands_ = std::move(other.commands_);
  sources_ = std::move(other.sources_);
  raws_ = std::move(other.raws_);
  coordinators_ = std::move(other.coordinators_);
  obligations_ = std::move(other.obligations_);
  other.status_ = Status::InvalidPlan;
  other.planDigest_ = {};
  other.candidateCapture_ = {};
  other.candidateQueue_ = 0u;
  return *this;
}

Status buildRenderTapeSemanticMappingV4(const CaptureSourceReplayPlanV4& plan,
    const RenderTapeIdentityV4View& candidate,
    std::span<const RenderTapeIdentityV3EventFact> canonicalEvents,
    std::span<const RenderTapeIdentityV3RecordFact> localRecords,
    std::span<const CaptureSourceReplayPlanV4Action> sourceObligations,
    RenderTapeSemanticMappingV4Limits limits, RenderTapeSemanticMappingV4& out) noexcept {
  out = RenderTapeSemanticMappingV4{};
  const auto reject = [&](Status status) noexcept { out.status_ = status; return status; };
  if (!plan.valid()) return reject(Status::InvalidPlan);
  const auto& original = plan.identity();
  if (!limitsAdmitted(limits) || plan.events().size() > limits.maxEvents ||
      canonicalEvents.size() > limits.maxEvents || plan.records().size() > limits.maxRecords ||
      localRecords.size() > limits.maxRecords || !extentsAdmitted(original, limits) ||
      !extentsAdmitted(candidate, limits) || sourceObligations.size() > limits.maxSources)
    return reject(Status::CapacityExceeded);
  if (canonicalEvents.size() != plan.events().size()) return reject(Status::InvalidFacts);
  for (std::size_t index = 0u; index < canonicalEvents.size(); ++index) {
    const auto& a = plan.events()[index];
    const auto& b = canonicalEvents[index];
    if (a.eventOrdinal != b.ordinal || a.type != b.type || a.recordCount != b.recordCount)
      return reject(Status::InvalidFacts);
  }
  std::size_t obligation = 0u;
  for (const auto& action : plan.actions()) {
    if (action.kind != CaptureSourceReplayPlanV4ActionKind::ExpectSourcePublication) continue;
    if (obligation == sourceObligations.size() || sourceObligations[obligation++] != action)
      return reject(Status::SemanticMismatch);
  }
  if (obligation != sourceObligations.size()) return reject(Status::SemanticMismatch);
  try {
    if (!localFactsAdmitted(plan.records(), localRecords)) return reject(Status::InvalidFacts);
    const auto validation = validateRenderTapeIdentityV4Rows(candidate, canonicalEvents, localRecords, true);
    if (validation == RenderTapeIdentityV4Status::CapacityExceeded) return reject(Status::CapacityExceeded);
    if (validation == RenderTapeIdentityV4Status::AllocationFailed) return reject(Status::AllocationFailed);
    if (validation != RenderTapeIdentityV4Status::Valid) return reject(Status::InvalidCandidate);
    if (!original.initializers.empty() || !candidate.initializers.empty())
      return reject(Status::UnsupportedInitializer);
    if (original.header.presentOrdinal != candidate.header.presentOrdinal ||
        original.header.coordinatorDomain != candidate.header.coordinatorDomain ||
        original.settlements.size() != candidate.settlements.size() ||
        original.coordinators.size() != candidate.coordinators.size())
      return reject(Status::SemanticMismatch);

    RenderTapeSemanticMappingV4 mapping;
    mapping.records_.reserve(plan.records().size());
    mapping.raws_.reserve(original.settlements.size());
    mapping.coordinators_.resize(original.coordinators.size());
    std::vector<std::uint32_t> candidateToCaptured(candidate.coordinators.size(), noIndex);
    std::size_t attemptCursor = 0u;
    for (std::uint32_t raw = 0u; raw < original.settlements.size(); ++raw) {
      const auto& a = original.settlements[raw];
      const auto& b = candidate.settlements[raw];
      if (a.eventOrdinal != b.eventOrdinal || a.recordCount != b.recordCount)
        return reject(Status::SemanticMismatch);
      RenderTapeSemanticMappingV4Raw row{a.eventOrdinal, a.rawOrdinal, b.rawOrdinal, b.replayEdge,
          raw, raw, a.recordCount};
      while (attemptCursor < candidate.attempts.size() &&
          candidate.attempts[attemptCursor].eventOrdinal == b.eventOrdinal) {
        const auto outcome = static_cast<RenderTapeIdentityV4AttemptOutcome>(
            candidate.attempts[attemptCursor++].outcome);
        row.committedAttemptCount += outcome == RenderTapeIdentityV4AttemptOutcome::Committed;
        row.rolledBackAttemptCount += outcome == RenderTapeIdentityV4AttemptOutcome::RolledBack;
      }
      mapping.raws_.push_back(row);
    }
    std::uint32_t originalDisposition = 0u, candidateDisposition = 0u, raw = 0u;
    for (const auto& fact : plan.records()) {
      while (raw < mapping.raws_.size() && mapping.raws_[raw].eventOrdinal < fact.eventOrdinal) ++raw;
      if (raw == mapping.raws_.size() || mapping.raws_[raw].eventOrdinal != fact.eventOrdinal)
        return reject(Status::SemanticMismatch);
      const auto locate = [&](const auto& rows, std::uint32_t& cursor) {
        while (cursor < rows.size() && (rows[cursor].eventOrdinal < fact.eventOrdinal ||
            (rows[cursor].eventOrdinal == fact.eventOrdinal &&
             std::uint64_t{rows[cursor].firstRecord} + rows[cursor].recordCount <= fact.recordIndex))) ++cursor;
        return cursor < rows.size() && rows[cursor].eventOrdinal == fact.eventOrdinal &&
            rows[cursor].firstRecord <= fact.recordIndex;
      };
      if (!locate(original.dispositions, originalDisposition) ||
          !locate(candidate.dispositions, candidateDisposition)) return reject(Status::SemanticMismatch);
      const auto& a = original.dispositions[originalDisposition];
      const auto& b = candidate.dispositions[candidateDisposition];
      if (a.kind != b.kind) return reject(Status::SemanticMismatch);
      RenderTapeSemanticMappingV4Record record{
          .eventOrdinal = fact.eventOrdinal, .capturedRawOrdinal = mapping.raws_[raw].capturedRawOrdinal,
          .candidateRawOrdinal = mapping.raws_[raw].candidateRawOrdinal,
          .recordIndex = fact.recordIndex, .recordType = fact.recordType, .dispositionKind = a.kind,
          .capturedDispositionIndex = originalDisposition, .candidateDispositionIndex = candidateDisposition};
      if (a.kind == static_cast<std::uint32_t>(Kind::GpuCommand)) {
        const auto& x = original.commands[a.commandIndex];
        const auto& y = candidate.commands[b.commandIndex];
        if (x.kind != y.kind || bool(a.subdrawCount) != bool(b.subdrawCount))
          return reject(Status::SemanticMismatch);
        record.capturedSourceIndex = x.sourceIndex; record.candidateSourceIndex = y.sourceIndex;
        record.capturedCommandIndex = a.commandIndex; record.candidateCommandIndex = b.commandIndex;
        record.capturedPhysicalCommandIndex = x.physicalCommandIndex;
        record.candidatePhysicalCommandIndex = y.physicalCommandIndex;
        if (a.subdrawCount) {
          record.capturedSubdraw = a.firstSubdraw + fact.recordIndex - a.firstRecord;
          record.candidateSubdraw = b.firstSubdraw + fact.recordIndex - b.firstRecord;
        }
        mapping.commands_.push_back({a.commandIndex, b.commandIndex, noIndex, noIndex, 1u});
      } else if (a.kind == static_cast<std::uint32_t>(Kind::Coordinator)) {
        const auto& x = original.coordinators[a.coordinatorIndex];
        const auto& y = candidate.coordinators[b.coordinatorIndex];
        if (!coordinatorSemanticsMatch(x, y)) return reject(Status::SemanticMismatch);
        record.capturedCoordinatorIndex = a.coordinatorIndex;
        record.candidateCoordinatorIndex = b.coordinatorIndex;
        candidateToCaptured[b.coordinatorIndex] = a.coordinatorIndex;
        const auto& control = y.provenance;
        mapping.coordinators_[a.coordinatorIndex] = {.capturedIndex = a.coordinatorIndex,
            .candidateIndex = b.coordinatorIndex, .candidateSourceIndex = control.sourceIndex,
            .candidateInitializerIndex = control.initializerIndex, .candidateCommandEnd = control.commandEnd,
            .candidatePrefixKind = control.prefixKind, .candidatePrefixSequence = control.prefixSequence,
            .candidatePrefixInitializer = control.prefixInitializer};
      }
      mapping.records_.push_back(record);
    }

    using Generated = std::pair<std::uint32_t, std::uint32_t>;
    std::vector<Generated> originalGenerated, candidateGenerated;
    for (std::uint32_t command = 0u; command < original.commands.size(); ++command)
      if (original.commands[command].coordinatorIndex != noIndex)
        originalGenerated.emplace_back(original.commands[command].coordinatorIndex, command);
    for (std::uint32_t command = 0u; command < candidate.commands.size(); ++command)
      if (candidate.commands[command].coordinatorIndex != noIndex)
        candidateGenerated.emplace_back(candidateToCaptured[candidate.commands[command].coordinatorIndex], command);
    std::sort(originalGenerated.begin(), originalGenerated.end());
    std::sort(candidateGenerated.begin(), candidateGenerated.end());
    if (originalGenerated.size() != candidateGenerated.size())
      return reject(Status::UnsupportedCoordinatorExpansion);
    std::vector<std::uint32_t> generatedCounts(original.coordinators.size(), 0u);
    for (std::size_t index = 0u; index < originalGenerated.size(); ++index) {
      const auto [coordinator, a] = originalGenerated[index];
      const auto [localOwner, b] = candidateGenerated[index];
      if (coordinator != localOwner || original.commands[a].kind != candidate.commands[b].kind ||
          original.commands[a].drawCount != candidate.commands[b].drawCount)
        return reject(Status::UnsupportedCoordinatorExpansion);
      mapping.commands_.push_back({a, b, coordinator, mapping.coordinators_[coordinator].candidateIndex});
      ++generatedCounts[coordinator];
    }

    // Temporary contributions are bounded by maxRecords + maxCommands, never
    // source-count products. Caps apply to the actual deduplicated relations.
    std::sort(mapping.commands_.begin(), mapping.commands_.end(), [](const auto& a, const auto& b) {
      return std::pair{a.capturedCommandIndex, a.candidateCommandIndex} <
          std::pair{b.capturedCommandIndex, b.candidateCommandIndex};
    });
    std::size_t commandRelations = 0u;
    for (std::size_t index = 0u; index < mapping.commands_.size(); ++index) {
      const auto contribution = mapping.commands_[index];
      if (commandRelations && mapping.commands_[commandRelations - 1u].capturedCommandIndex ==
              contribution.capturedCommandIndex &&
          mapping.commands_[commandRelations - 1u].candidateCommandIndex == contribution.candidateCommandIndex)
        mapping.commands_[commandRelations - 1u].recordCount += contribution.recordCount;
      else {
        if (commandRelations == limits.maxCommandRelations) return reject(Status::CapacityExceeded);
        mapping.commands_[commandRelations++] = contribution;
      }
    }
    mapping.commands_.resize(commandRelations);
    mapping.sources_.reserve(commandRelations);
    for (const auto& command : mapping.commands_) {
      const auto a = original.commands[command.capturedCommandIndex].sourceIndex;
      const auto b = candidate.commands[command.candidateCommandIndex].sourceIndex;
      const auto& lifetime = candidate.sources[b].lifecycle;
      mapping.sources_.push_back({a, b, command.recordCount,
          command.capturedCoordinatorIndex != noIndex ? 1u : 0u, lifetime.seqId, lifetime.publicationEdge});
    }
    std::sort(mapping.sources_.begin(), mapping.sources_.end(), [](const auto& a, const auto& b) {
      return std::pair{a.capturedIndex, a.candidateIndex} < std::pair{b.capturedIndex, b.candidateIndex};
    });
    std::size_t sourceRelations = 0u;
    for (std::size_t index = 0u; index < mapping.sources_.size(); ++index) {
      const auto contribution = mapping.sources_[index];
      if (sourceRelations && mapping.sources_[sourceRelations - 1u].capturedIndex == contribution.capturedIndex &&
          mapping.sources_[sourceRelations - 1u].candidateIndex == contribution.candidateIndex) {
        mapping.sources_[sourceRelations - 1u].recordCount += contribution.recordCount;
        mapping.sources_[sourceRelations - 1u].coordinatorCommandCount += contribution.coordinatorCommandCount;
      } else {
        if (sourceRelations == limits.maxSourceRelations) return reject(Status::CapacityExceeded);
        mapping.sources_[sourceRelations++] = contribution;
      }
    }
    mapping.sources_.resize(sourceRelations);
    for (const auto& action : sourceObligations) {
      const auto first = std::lower_bound(mapping.sources_.begin(), mapping.sources_.end(), action.tableIndex,
          [](const auto& row, auto captured) { return row.capturedIndex < captured; });
      const auto last = std::upper_bound(first, mapping.sources_.end(), action.tableIndex,
          [](auto captured, const auto& row) { return captured < row.capturedIndex; });
      if (first == last) return reject(Status::SemanticMismatch);
      mapping.obligations_.push_back({action.tableIndex,
          static_cast<std::uint32_t>(first - mapping.sources_.begin()),
          static_cast<std::uint32_t>(last - first)});
    }

    const auto capturedCoverage = coverageIndex(original, mapping.records_, true);
    const auto candidateCoverage = coverageIndex(candidate, mapping.records_, false);
    std::uint32_t priorGpuRecords = 0u, priorGeneratedCommands = 0u;
    for (std::uint32_t position = 0u; position < mapping.records_.size(); ++position) {
      const auto& record = mapping.records_[position];
      if (record.capturedCoordinatorIndex != noIndex) {
        const auto originalIndex = record.capturedCoordinatorIndex;
        const auto& a = original.coordinators[originalIndex].provenance;
        const auto& b = candidate.coordinators[record.candidateCoordinatorIndex].provenance;
        if (a.recordType == D9C_COMMAND_RECORD_QUERY_ISSUE && a.flags == 1u) {
          if (!prefixCoverageAdmitted(a, capturedCoverage, position, priorGpuRecords, priorGeneratedCommands) ||
              !prefixCoverageAdmitted(b, candidateCoverage, position, priorGpuRecords, priorGeneratedCommands))
            return reject(Status::InvalidPrefix);
          mapping.coordinators_[originalIndex].prefixRecordCount = priorGpuRecords;
          mapping.coordinators_[originalIndex].prefixCoordinatorCommandCount = priorGeneratedCommands;
        }
        priorGeneratedCommands += generatedCounts[originalIndex];
      }
      priorGpuRecords += record.dispositionKind == static_cast<std::uint32_t>(Kind::GpuCommand);
    }
    mapping.planDigest_ = plan.digest();
    mapping.candidateCapture_ = {candidate.header.captureToken, candidate.header.captureEpoch};
    mapping.candidateQueue_ = candidate.header.queue;
    mapping.status_ = Status::Valid;
    out = std::move(mapping);
    return Status::Valid;
  } catch (...) { return reject(Status::AllocationFailed); }
}

const char* renderTapeSemanticMappingV4StatusName(Status status) noexcept {
  switch (status) {
  case Status::Valid: return "valid";
  case Status::InvalidPlan: return "invalid-plan";
  case Status::InvalidFacts: return "invalid-facts";
  case Status::InvalidCandidate: return "invalid-candidate";
  case Status::SemanticMismatch: return "semantic-mismatch";
  case Status::InvalidPrefix: return "invalid-prefix";
  case Status::UnsupportedInitializer: return "unsupported-initializer";
  case Status::UnsupportedCoordinatorExpansion: return "unsupported-coordinator-expansion";
  case Status::CapacityExceeded: return "capacity-exceeded";
  case Status::AllocationFailed: return "allocation-failed";
  }
  return "unknown";
}

} // namespace dxmt9::d3d9
