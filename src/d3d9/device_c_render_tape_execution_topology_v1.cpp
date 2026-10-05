#include "device_c_render_tape_execution_topology_v1.hpp"

#include "device_c_render_tape_capture.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace dxmt9::d3d9 {
namespace {

using Status = RenderTapeExecutionTopologyV1Status;
using Outcome = RenderTapeExecutionTopologyV1Outcome;
using Route = RenderTapeExecutionTopologyV1Route;
constexpr auto noIndex = kRenderTapeExecutionTopologyV1NoIndex;

template <typename T>
bool load(std::span<const std::byte> bytes, std::size_t offset, T& value) noexcept {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) return false;
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return true;
}

bool headerAdmitted(const RenderTapeExecutionTopologyV1Header& h) noexcept {
  return h.magic == kRenderTapeExecutionTopologyV1Magic &&
      h.version == kRenderTapeExecutionTopologyV1Version && h.headerSize == sizeof(h) &&
      h.captureToken && h.captureEpoch && h.queue && !h.reserved0 && !h.reserved1;
}

template <typename T>
bool importTable(std::span<const std::byte> bytes, RenderTapeIdentityV3Table table,
                 std::uint64_t& cursor, std::vector<T>& rows) {
  if (table.offset != cursor || table.entrySize != sizeof(T) || table.count > kRenderTapeIdentityV4MaxRows)
    return false;
  cursor += std::uint64_t{table.count} * sizeof(T);
  if (cursor > bytes.size()) return false;
  rows.resize(table.count);
  if (!rows.empty()) std::memcpy(rows.data(), bytes.data() + table.offset, rows.size() * sizeof(T));
  return true;
}

} // namespace

Status validateRenderTapeExecutionTopologyV1Rows(const RenderTapeIdentityV4View& identity,
    const RenderTapeExecutionTopologyV1View& topology) noexcept {
  const auto& h = topology.header;
  if (!headerAdmitted(h)) return Status::InvalidHeader;
  if (identity.header.version != kRenderTapeIdentityV4Version ||
      identity.header.requiredExecutionTopologyVersion != kRenderTapeExecutionTopologyV1Version)
    return Status::InvalidIdentity;
  if (h.captureToken != identity.header.captureToken || h.captureEpoch != identity.header.captureEpoch ||
      h.queue != identity.header.queue) return Status::BindingMismatch;
  for (const auto count : {identity.commands.size(), identity.sources.size(), topology.plans.size(),
                           topology.placements.size()})
    if (count > kRenderTapeIdentityV4MaxRows) return Status::CapacityExceeded;
  if (topology.placements.size() != identity.commands.size()) return Status::Incomplete;
  try {
    std::vector<bool> commands(identity.commands.size(), false);
    std::vector<bool> coveredSources(identity.sources.size(), false);
    std::vector<std::uint64_t> acceptedEdges;
    std::size_t cursor = 0u;
    std::uint64_t priorPlanId = 0u, priorAcceptance = identity.header.baselineCompletionEdge;
    for (std::uint32_t planIndex = 0u; planIndex < topology.plans.size(); ++planIndex) {
      const auto& plan = topology.plans[planIndex];
      if (!plan.planId || plan.planId <= priorPlanId || plan.acceptanceEdge <= priorAcceptance ||
          !renderTapeExecutionTopologyV1RouteAdmitted(plan.route) ||
          !renderTapeExecutionTopologyV1FallbackReasonAdmitted(plan.fallbackReason) || plan.reserved0 ||
          !plan.placementCount || plan.firstPlacement != cursor || cursor > topology.placements.size() ||
          plan.placementCount > topology.placements.size() - cursor || plan.executedCount > plan.placementCount)
        return Status::InvalidPlan;
      priorPlanId = plan.planId; priorAcceptance = plan.acceptanceEdge;
      acceptedEdges.push_back(plan.acceptanceEdge);
      std::vector<std::uint32_t> order(plan.executedCount, noIndex);
      std::vector<std::pair<std::uint32_t, std::uint32_t>> groups;
      for (std::uint32_t local = 0u; local < plan.placementCount; ++local) {
        const auto& placement = topology.placements[cursor++];
        if (placement.planIndex != planIndex || placement.commandIndex >= identity.commands.size() ||
            commands[placement.commandIndex] || !renderTapeExecutionTopologyV1PlacementAdmitted(plan, placement))
          return Status::InvalidPlacement;
        commands[placement.commandIndex] = true;
        const auto& command = identity.commands[placement.commandIndex];
        if (!renderTapeExecutionTopologyV1CommandPlacementAdmitted(command, plan, placement))
          return Status::InvalidPlacement;
        if (command.sourceIndex >= identity.sources.size()) return Status::InvalidIdentity;
        coveredSources[command.sourceIndex] = true;
        const auto& source = identity.sources[command.sourceIndex].lifecycle;
        if (renderTapeIdentityV3SourcePhase(source) != RenderTapeIdentityV3SourcePhase::Completed)
          return Status::UnsupportedLifecycle;
        // This is the accepted production plan, not an arbitrary nonzero
        // candidate label. Acceptance must follow publication and precede
        // actual submission of every source whose commands it selects.
        if (plan.acceptanceEdge <= source.publicationEdge || plan.acceptanceEdge >= source.submissionEdge)
          return Status::InvalidPlan;
        const auto outcome = static_cast<Outcome>(placement.outcome);
        if (outcome != Outcome::ProvenOmitted) {
          if (placement.executionOrdinal >= order.size() || order[placement.executionOrdinal] != noIndex)
            return Status::InvalidPlacement;
          order[placement.executionOrdinal] = placement.commandIndex;
          if (outcome == Outcome::ExecutedDagPass) {
            const auto group = std::find_if(groups.begin(), groups.end(), [&](const auto& value) {
              return value.first == placement.dagPassIndex;
            });
            if (group == groups.end()) groups.push_back({placement.dagPassIndex, placement.passKind});
            else if (group->second != placement.passKind) return Status::InvalidPlacement;
          }
        }
      }
      if (std::find(order.begin(), order.end(), noIndex) != order.end()) return Status::Incomplete;
      // Ordinals are dense and plan-local. The physical table remains in
      // materialization order. Bypass may consume an explicit supplied order
      // without claiming DAG membership; only NaturalOrder promises identity.
      if (plan.route == static_cast<std::uint32_t>(Route::NaturalOrder) &&
          !std::is_sorted(order.begin(), order.end())) return Status::InvalidPlacement;
    }
    if (cursor != topology.placements.size() ||
        std::find(commands.begin(), commands.end(), false) != commands.end()) return Status::Incomplete;
    // A selected DAG may omit every command from a source carried by a real
    // submitted command buffer. Coverage and the Completed phase above still
    // require its actual lifecycle; an inline terminal is not a submission.
    if (std::find(coveredSources.begin(), coveredSources.end(), false) != coveredSources.end())
      return Status::UnsupportedLifecycle;
    // The capture-local edge domain must not alias any lifecycle event.
    const auto collides = [&](std::uint64_t edge) {
      return std::find(acceptedEdges.begin(), acceptedEdges.end(), edge) != acceptedEdges.end();
    };
    if (collides(identity.header.baselineCompletionEdge)) return Status::InvalidPlan;
    for (const auto& source : identity.sources)
      if (collides(source.lifecycle.publicationEdge) || collides(source.lifecycle.submissionEdge) ||
          collides(source.lifecycle.completionEdge))
        return Status::InvalidPlan;
    for (const auto& initializer : identity.initializers)
      if (collides(initializer.publicationEdge) || collides(initializer.submissionEdge) || collides(initializer.completionEdge))
        return Status::InvalidPlan;
    for (const auto& settlement : identity.settlements)
      if (collides(settlement.replayEdge)) return Status::InvalidPlan;
    for (const auto& coordinator : identity.coordinators)
      if (collides(coordinator.provenance.dispatchEdge) ||
          (coordinator.admissionEdge && collides(coordinator.admissionEdge)))
        return Status::InvalidPlan;
    return Status::Valid;
  } catch (...) { return Status::AllocationFailed; }
}

Status validateRenderTapeExecutionTopologyV1(std::span<const std::byte> tape,
    const RenderTapeBlobCatalogue& catalogue, std::span<const std::byte> identityBytes,
    std::span<const std::byte> bytes, RenderTapeExecutionTopologyV1View* out) noexcept {
  if (out) *out = {};
  RenderTapeExecutionTopologyV1View topology;
  if (!load(bytes, 0u, topology.header) || !headerAdmitted(topology.header)) return Status::InvalidHeader;
  const auto& h = topology.header;
  if (h.plans.count > kRenderTapeIdentityV4MaxRows || h.placements.count > kRenderTapeIdentityV4MaxRows)
    return Status::CapacityExceeded;
  if (h.identityBytes != identityBytes.size() || h.eventsBytes != tape.size() ||
      h.identityDigest != RenderTapeCaptureSession::sha256(identityBytes) ||
      h.eventsDigest != RenderTapeCaptureSession::sha256(tape)) return Status::BindingMismatch;
  try {
    std::uint64_t cursor = sizeof(h);
    if (!importTable(bytes, h.plans, cursor, topology.plans) ||
        !importTable(bytes, h.placements, cursor, topology.placements) || cursor != bytes.size())
      return Status::InvalidLayout;
    RenderTapeIdentityV4View identity;
    if (validateRenderTapeIdentityV4(tape, catalogue, identityBytes, &identity) != RenderTapeIdentityV4Status::Valid)
      return Status::InvalidIdentity;
    const auto status = validateRenderTapeExecutionTopologyV1Rows(identity, topology);
    if (status == Status::Valid && out) *out = std::move(topology);
    return status;
  } catch (...) { return Status::AllocationFailed; }
}

std::vector<std::byte> buildRenderTapeExecutionTopologyV1(std::span<const std::byte> tape,
    const RenderTapeBlobCatalogue& catalogue, std::span<const std::byte> identityBytes,
    const RenderTapeExecutionTopologyV1View& topology, Status* outStatus) {
  auto finish = [&](Status status) { if (outStatus) *outStatus = status; };
  try {
    RenderTapeIdentityV4View identity;
    if (validateRenderTapeIdentityV4(tape, catalogue, identityBytes, &identity) != RenderTapeIdentityV4Status::Valid) {
      finish(Status::InvalidIdentity); return {};
    }
    auto status = validateRenderTapeExecutionTopologyV1Rows(identity, topology);
    if (status != Status::Valid) { finish(status); return {}; }
    auto header = topology.header;
    header.identityBytes = identityBytes.size(); header.eventsBytes = tape.size();
    header.identityDigest = RenderTapeCaptureSession::sha256(identityBytes);
    header.eventsDigest = RenderTapeCaptureSession::sha256(tape);
    std::uint64_t cursor = sizeof(header);
    header.plans = {static_cast<std::uint32_t>(cursor), static_cast<std::uint32_t>(topology.plans.size()),
                    sizeof(RenderTapeExecutionTopologyV1Plan)};
    cursor += topology.plans.size() * std::uint64_t{sizeof(RenderTapeExecutionTopologyV1Plan)};
    if (cursor > UINT32_MAX) { finish(Status::CapacityExceeded); return {}; }
    header.placements = {static_cast<std::uint32_t>(cursor), static_cast<std::uint32_t>(topology.placements.size()),
                         sizeof(RenderTapeExecutionTopologyV1Placement)};
    cursor += topology.placements.size() * std::uint64_t{sizeof(RenderTapeExecutionTopologyV1Placement)};
    if (cursor > UINT32_MAX) { finish(Status::CapacityExceeded); return {}; }
    std::vector<std::byte> bytes(cursor);
    std::memcpy(bytes.data(), &header, sizeof(header));
    if (!topology.plans.empty()) std::memcpy(bytes.data() + header.plans.offset,
        topology.plans.data(), topology.plans.size() * sizeof(topology.plans[0]));
    if (!topology.placements.empty()) std::memcpy(bytes.data() + header.placements.offset,
        topology.placements.data(), topology.placements.size() * sizeof(topology.placements[0]));
    status = validateRenderTapeExecutionTopologyV1(tape, catalogue, identityBytes, bytes);
    finish(status);
    return status == Status::Valid ? std::move(bytes) : std::vector<std::byte>{};
  } catch (...) { finish(Status::AllocationFailed); return {}; }
}

} // namespace dxmt9::d3d9
