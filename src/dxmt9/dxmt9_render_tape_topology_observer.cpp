#include "dxmt9_render_tape_topology_observer.hpp"

#include <algorithm>
#include <limits>

namespace dxmt9::render {
namespace {
using Route = d3d9::RenderTapeExecutionTopologyV1Route;
using Outcome = d3d9::RenderTapeExecutionTopologyV1Outcome;
using Reason = d3d9::RenderTapeExecutionTopologyV1FallbackReason;
using Omission = d3d9::RenderTapeExecutionTopologyV1OmissionReason;
constexpr auto noIndex = d3d9::kRenderTapeExecutionTopologyV1NoIndex;

bool limitsAdmitted(TapeTopologyLimits limits) noexcept {
  constexpr auto maxRows = d3d9::kRenderTapeIdentityV4MaxRows;
  return limits.maxSources && limits.maxSources <= maxRows &&
      limits.maxCommands && limits.maxCommands <= maxRows &&
      limits.maxPlans && limits.maxPlans <= maxRows &&
      limits.maxGraphPasses && limits.maxGraphPasses <= maxRows &&
      limits.maxGraphCommandRefs && limits.maxGraphCommandRefs <= maxRows;
}

bool extentAdmitted(TapeTopologyPhysicalExtent extent) noexcept {
  using Kind = d3d9::RenderTapeIdentityV3CommandKind;
  return extent.kind >= static_cast<std::uint32_t>(Kind::DrawRun) &&
      extent.kind <= static_cast<std::uint32_t>(Kind::Present) &&
      (extent.kind == static_cast<std::uint32_t>(Kind::DrawRun)
          ? extent.drawCount != 0u : extent.drawCount == 0u);
}
} // namespace

bool TapeTopologyCapture::begin(TapeTopologyCaptureIdentity capture,
    std::uint64_t baselineEdge, TapeTopologyLimits limits) noexcept {
  std::lock_guard lock(mutex_);
  if (active_ || failed_ || !capture.valid() || !baselineEdge || !limitsAdmitted(limits))
    return invalidate();
  try {
    sources_.reserve(limits.maxSources);
    commands_.reserve(limits.maxCommands);
    plans_.reserve(limits.maxPlans);
    placements_.reserve(limits.maxCommands);
    pending_.reserve(limits.maxPlans);
    pendingCommands_.reserve(limits.maxCommands);
    staged_.reserve(limits.maxCommands);
    liveOrder_.reserve(limits.maxCommands);
    capture_ = capture;
    limits_ = limits;
    baselineEdge_ = lastAcceptanceEdge_ = baselineEdge;
    active_ = true;
    return true;
  } catch (...) { return invalidate(); }
}

bool TapeTopologyCapture::publish(TapeTopologySourceIdentity source,
    std::span<const TapeTopologyPhysicalExtent> extents) noexcept {
  std::lock_guard lock(mutex_);
  if (!active_ || failed_ || !source.source.valid() || !source.sourceOrdinal ||
      !source.sequence || source.publicationEdge <= baselineEdge_ ||
      source.sourceIndex != sources_.size() || extents.empty() ||
      extents.size() != source.commandCount || sources_.size() >= limits_.maxSources ||
      extents.size() > limits_.maxCommands - commands_.size()) return invalidate();
  if (!sources_.empty() && (source.sequence <= sources_.back().identity.sequence ||
      source.sourceOrdinal <= sources_.back().identity.sourceOrdinal ||
      source.publicationEdge <= sources_.back().identity.publicationEdge)) return invalidate();
  for (const auto& prior : sources_)
    if (prior.identity.source == source.source) return invalidate();
  for (const auto& plan : plans_)
    if (plan.acceptanceEdge == source.publicationEdge) return invalidate();
  for (std::size_t index = 0u; index < extents.size(); ++index)
    if (extents[index].physicalCommandIndex != index || !extentAdmitted(extents[index]))
      return invalidate();
  const auto firstCommand = commands_.size();
  try {
    for (const auto extent : extents) {
      const TapeTopologyCommandRef ref{source,
          static_cast<std::uint32_t>(commands_.size()), extent};
      commands_.push_back({ref, noIndex});
    }
    sources_.push_back({source, static_cast<std::uint32_t>(firstCommand)});
    return true;
  } catch (...) {
    commands_.resize(firstCommand);
    return invalidate();
  }
}

bool TapeTopologyCapture::commandRef(std::uint32_t index,
    TapeTopologyCommandRef& out) const noexcept {
  out = {};
  std::lock_guard lock(mutex_);
  if (!active_ || failed_ || index >= commands_.size()) return false;
  out = commands_[index].ref;
  return true;
}

bool TapeTopologyCapture::commandRef(core::CpuReadyTape::SourceRef source,
    std::uint64_t sequence, std::uint32_t physicalCommandIndex,
    TapeTopologyCommandRef& out) const noexcept {
  out = {};
  std::lock_guard lock(mutex_);
  if (!active_ || failed_ || !source.valid() || !sequence) return false;
  const auto selected = std::find_if(sources_.begin(), sources_.end(), [&](const auto& row) {
    return row.identity.source == source && row.identity.sequence == sequence;
  });
  if (selected == sources_.end() || physicalCommandIndex >= selected->identity.commandCount ||
      selected->firstCommand > commands_.size() ||
      selected->identity.commandCount > commands_.size() - selected->firstCommand) return false;
  const auto& ref = commands_[selected->firstCommand + physicalCommandIndex].ref;
  if (ref.owner != selected->identity || ref.extent.physicalCommandIndex != physicalCommandIndex)
    return false;
  out = ref;
  return true;
}

bool TapeTopologyCapture::sourceIdentity(core::CpuReadyTape::SourceRef source,
    std::uint64_t sequence, TapeTopologySourceIdentity& out) const noexcept {
  out = {};
  std::lock_guard lock(mutex_);
  if (!active_ || failed_ || !source.valid() || !sequence) return false;
  const auto selected = std::find_if(sources_.begin(), sources_.end(), [&](const auto& row) {
    return row.identity.source == source && row.identity.sequence == sequence;
  });
  if (selected == sources_.end()) return false;
  out = selected->identity;
  return true;
}

TapeTopologyCaptureIdentity TapeTopologyCapture::identity() const noexcept {
  std::lock_guard lock(mutex_);
  return active_ && !failed_ ? capture_ : TapeTopologyCaptureIdentity{};
}

bool TapeTopologyCapture::matches(const TapeTopologyCommandRef& ref) const noexcept {
  return ref.commandIndex < commands_.size() && commands_[ref.commandIndex].ref == ref;
}

bool TapeTopologyCapture::prepare(Route route, Reason reason,
    std::span<const TapeTopologyCommandRef> refs, bool permitPrior) {
  if (!active_ || failed_ || refs.empty() ||
      refs.size() > limits_.maxCommands || pending_.size() >= limits_.maxPlans ||
      !d3d9::renderTapeExecutionTopologyV1RouteAdmitted(static_cast<std::uint32_t>(route)) ||
      !d3d9::renderTapeExecutionTopologyV1FallbackReasonAdmitted(static_cast<std::uint32_t>(reason)))
    return invalidate();
  std::size_t newlyOwned = 0u;
  for (std::size_t index = 0u; index < refs.size(); ++index) {
    if (!matches(refs[index]) || (index && refs[index - 1u].commandIndex >= refs[index].commandIndex))
      return invalidate();
    const auto& command = commands_[refs[index].commandIndex];
    std::uint64_t priorAttempt = 0u;
    std::uint32_t priorOrdinal = noIndex;
    if (command.pendingAttempt) {
      if (!permitPrior || !priorExecution(command, priorAttempt, priorOrdinal))
        return invalidate();
    } else {
      ++newlyOwned;
    }
  }
  if (!newlyOwned || newlyOwned > limits_.maxCommands - pendingCommands_.size())
    return invalidate();
  staged_.clear();
  liveOrder_.clear();
  stagedPlan_ = {.route = static_cast<std::uint32_t>(route),
      .fallbackReason = static_cast<std::uint32_t>(reason)};
  for (const auto& ref : refs) {
    StageCommand command{};
    command.ref = ref;
    command.prior = commands_[ref.commandIndex].pendingAttempt != 0u;
    command.placement.commandIndex = ref.commandIndex;
    staged_.push_back(command);
  }
  return true;
}

std::uint64_t TapeTopologyCapture::sealStage() {
  std::size_t count = 0u;
  for (const auto& command : staged_) {
    if (!command.accounted) { invalidate(); return 0u; }
    if (command.prior) continue;
    const d3d9::RenderTapeIdentityV4PhysicalCommand physical{
        .sourceIndex = command.ref.owner.sourceIndex,
        .physicalCommandIndex = command.ref.extent.physicalCommandIndex,
        .kind = command.ref.extent.kind, .drawCount = command.ref.extent.drawCount,
        .coordinatorIndex = command.ref.extent.coordinatorIndex};
    if (!d3d9::renderTapeExecutionTopologyV1CommandPlacementAdmitted(
            physical, stagedPlan_, command.placement)) {
      invalidate(); return 0u;
    }
    ++count;
  }
  if (!count || count > limits_.maxCommands - pendingCommands_.size() ||
      !nextAttempt_ || nextAttempt_ == UINT64_MAX) { invalidate(); return 0u; }
  stagedPlan_.placementCount = static_cast<std::uint32_t>(count);
  stagedPlan_.planId = nextAttempt_++;
  pending_.push_back({stagedPlan_, static_cast<std::uint32_t>(pendingCommands_.size())});
  for (const auto& command : staged_) {
    if (command.prior) continue;
    pendingCommands_.push_back({command.ref, command.placement});
    commands_[command.ref.commandIndex].pendingAttempt = stagedPlan_.planId;
  }
  return stagedPlan_.planId;
}

bool TapeTopologyCapture::priorExecution(const Command& command,
    std::uint64_t& attempt, std::uint32_t& ordinal) const noexcept {
  for (const auto& invocation : pending_) {
    if (invocation.plan.planId != command.pendingAttempt || !invocation.encoded) continue;
    for (std::uint32_t index = 0u; index < invocation.plan.placementCount; ++index) {
      const auto& prior = pendingCommands_[invocation.firstCommand + index];
      if (prior.ref != command.ref || prior.placement.executionOrdinal == noIndex) continue;
      attempt = invocation.plan.planId;
      ordinal = prior.placement.executionOrdinal;
      return true;
    }
  }
  return false;
}

std::uint64_t TapeTopologyCapture::stageWithoutDag(Route route, Reason reason,
    std::span<const TapeTopologyCommandRef> commands,
    std::span<const std::uint32_t> executionOrder) noexcept {
  std::lock_guard lock(mutex_);
  if (route == Route::Optimized || executionOrder.size() != commands.size()) {
    invalidate(); return 0u;
  }
  try {
    if (!prepare(route, reason, commands, false)) return 0u;
    for (std::size_t ordinal = 0u; ordinal < executionOrder.size(); ++ordinal) {
      const auto index = executionOrder[ordinal];
      if (index >= staged_.size() || staged_[index].accounted ||
          (route == Route::NaturalOrder && index != ordinal)) { invalidate(); return 0u; }
      auto& command = staged_[index];
      command.accounted = true;
      command.placement.outcome = static_cast<std::uint32_t>(Outcome::ExecutedWithoutDag);
      command.placement.executionOrdinal = static_cast<std::uint32_t>(ordinal);
    }
    stagedPlan_.executedCount = static_cast<std::uint32_t>(executionOrder.size());
    return sealStage();
  } catch (...) { invalidate(); return 0u; }
}

std::uint64_t TapeTopologyCapture::stageSelectedDag(Reason reason,
    const framegraph::FrameGraph& graph,
    std::span<const TapeTopologyCommandRef> graphCommands,
    std::span<const std::uint32_t> actualOrder,
    std::span<const std::uint32_t> alreadyEncoded) noexcept {
  std::lock_guard lock(mutex_);
  if (graph.passes.size() > limits_.maxGraphPasses ||
      graph.commands.size() > limits_.maxGraphCommandRefs ||
      actualOrder.size() > graphCommands.size() || alreadyEncoded.size() > graphCommands.size()) {
    invalidate(); return 0u;
  }
  try {
    if (!prepare(Route::Optimized, reason, graphCommands, true)) return 0u;
    // Production planReplayCommands flattens this same post-optimizer pass
    // storage order. Still require the supplied actual order below: reading
    // a candidate graph alone never authenticates the selected schedule.
    for (std::size_t passIndex = 0u; passIndex < graph.passes.size(); ++passIndex) {
      const auto& pass = graph.passes[passIndex];
      if (pass.commands.first > graph.commands.size() ||
          pass.commands.count > graph.commands.size() - pass.commands.first ||
          static_cast<std::uint32_t>(pass.kind) > static_cast<std::uint32_t>(framegraph::PassKind::Sync)) {
        invalidate(); return 0u;
      }
      for (std::uint32_t local = 0u; local < pass.commands.count; ++local) {
        const auto ref = graph.commands[pass.commands.first + local];
        if (ref.command_index >= staged_.size()) { invalidate(); return 0u; }
        auto& command = staged_[ref.command_index];
        if (command.accounted || tapeTopologyCommandKind(ref.kind) != command.ref.extent.kind ||
            (pass.flags.dead && command.prior)) { invalidate(); return 0u; }
        command.accounted = true;
        if (pass.flags.dead) {
          command.placement.outcome = static_cast<std::uint32_t>(Outcome::ProvenOmitted);
          command.placement.omissionReason = static_cast<std::uint32_t>(Omission::DeadPass);
        } else {
          command.placement.outcome = static_cast<std::uint32_t>(Outcome::ExecutedDagPass);
          command.placement.dagPassIndex = static_cast<std::uint32_t>(passIndex);
          command.placement.passKind = static_cast<std::uint32_t>(pass.kind) + 1u;
          liveOrder_.push_back(ref.command_index);
        }
      }
    }
    if (alreadyEncoded.size() > liveOrder_.size() ||
        actualOrder.size() != liveOrder_.size() - alreadyEncoded.size() ||
        !std::equal(alreadyEncoded.begin(), alreadyEncoded.end(), liveOrder_.begin()) ||
        !std::equal(actualOrder.begin(), actualOrder.end(), liveOrder_.begin() + alreadyEncoded.size())) {
      invalidate(); return 0u;
    }
    for (std::size_t index = 0u; index < liveOrder_.size(); ++index) {
      auto& command = staged_[liveOrder_[index]];
      if (command.prior != (index < alreadyEncoded.size())) { invalidate(); return 0u; }
      if (command.prior && index) {
        const auto previousCommand = staged_[liveOrder_[index - 1u]].ref.commandIndex;
        std::uint64_t previousAttempt = 0u, currentAttempt = 0u;
        std::uint32_t previousOrdinal = noIndex, currentOrdinal = noIndex;
        if (!priorExecution(commands_[previousCommand], previousAttempt, previousOrdinal) ||
            !priorExecution(commands_[command.ref.commandIndex], currentAttempt, currentOrdinal) ||
            previousAttempt > currentAttempt ||
            (previousAttempt == currentAttempt && previousOrdinal >= currentOrdinal)) {
          invalidate(); return 0u;
        }
      } else if (!command.prior) {
        command.placement.executionOrdinal = static_cast<std::uint32_t>(index - alreadyEncoded.size());
      }
    }
    stagedPlan_.executedCount = static_cast<std::uint32_t>(actualOrder.size());
    return sealStage();
  } catch (...) { invalidate(); return 0u; }
}

bool TapeTopologyCapture::accept(TapeTopologyAcceptanceIdentity acceptance,
    std::span<const TapeTopologySourceIdentity> registeredSources) noexcept {
  const TapeTopologyAcceptedInvocation invocation{acceptance.attempt, acceptance.edge};
  return acceptBatch(acceptance.capture, std::span(&invocation, 1u), registeredSources);
}

bool TapeTopologyCapture::observeEncoded(TapeTopologyEncodedInvocationIdentity invocation,
    std::span<const TapeTopologyCommandRef> executionOrder) noexcept {
  std::lock_guard lock(mutex_);
  if (!active_ || failed_ || invocation.capture != capture_ || !invocation.attempt)
    return invalidate();
  auto selected = std::find_if(pending_.begin(), pending_.end(),
      [](const auto& pending) { return !pending.encoded; });
  if (selected == pending_.end() || selected->plan.planId != invocation.attempt ||
      executionOrder.size() != selected->plan.executedCount) return invalidate();
  for (std::uint32_t index = 0u; index < selected->plan.placementCount; ++index) {
    const auto& command = pendingCommands_[selected->firstCommand + index];
    if (!matches(command.ref) || commands_[command.ref.commandIndex].pendingAttempt != invocation.attempt)
      return invalidate();
    if (command.placement.executionOrdinal == noIndex) continue;
    if (command.placement.executionOrdinal >= executionOrder.size() ||
        executionOrder[command.placement.executionOrdinal] != command.ref) return invalidate();
  }
  selected->encoded = true;
  promoteTerminalPrefix();
  return true;
}

bool TapeTopologyCapture::acceptBatch(TapeTopologyCaptureIdentity capture,
    std::span<const TapeTopologyAcceptedInvocation> invocations,
    std::span<const TapeTopologySourceIdentity> registeredSources) noexcept {
  std::lock_guard lock(mutex_);
  if (!active_ || failed_ || capture != capture_ || invocations.empty() ||
      invocations.size() > pending_.size() || registeredSources.empty() ||
      registeredSources.size() > sources_.size()) return invalidate();
  for (std::size_t index = 0u; index < registeredSources.size(); ++index) {
    const auto& source = registeredSources[index];
    if (source.sourceIndex >= sources_.size() || source != sources_[source.sourceIndex].identity ||
        (index && registeredSources[index - 1u].sourceIndex >= source.sourceIndex)) return invalidate();
  }
  std::uint64_t previousEdge = lastAcceptanceEdge_;
  auto selected = pending_.begin();
  for (const auto receipt : invocations) {
    while (selected != pending_.end() && selected->accepted) ++selected;
    if (selected == pending_.end() || receipt.attempt != selected->plan.planId ||
        receipt.edge <= previousEdge) return invalidate();
    for (const auto& source : sources_)
      if (source.identity.publicationEdge == receipt.edge) return invalidate();
    for (std::uint32_t index = 0u; index < selected->plan.placementCount; ++index) {
      const auto& command = pendingCommands_[selected->firstCommand + index];
      const d3d9::RenderTapeIdentityV4PhysicalCommand physical{
          .sourceIndex = command.ref.owner.sourceIndex,
          .physicalCommandIndex = command.ref.extent.physicalCommandIndex,
          .kind = command.ref.extent.kind, .drawCount = command.ref.extent.drawCount,
          .coordinatorIndex = command.ref.extent.coordinatorIndex};
      if (!matches(command.ref) ||
          commands_[command.ref.commandIndex].pendingAttempt != receipt.attempt ||
          commands_[command.ref.commandIndex].acceptedPlacement != noIndex ||
          command.ref.owner.publicationEdge >= receipt.edge ||
          std::find(registeredSources.begin(), registeredSources.end(),
              command.ref.owner) == registeredSources.end() ||
          !d3d9::renderTapeExecutionTopologyV1CommandPlacementAdmitted(
              physical, selected->plan, command.placement)) return invalidate();
    }
    previousEdge = receipt.edge;
    ++selected;
  }
  // A registration receipt must cover exactly the selected invocations'
  // source union, not merely include every needed source among extras.
  for (const auto& source : registeredSources) {
    bool used = false;
    for (const auto receipt : invocations) {
      const auto pending = std::find_if(pending_.begin(), pending_.end(),
          [&](const auto& value) { return value.plan.planId == receipt.attempt; });
      for (std::uint32_t index = 0u; index < pending->plan.placementCount; ++index)
        used |= pendingCommands_[pending->firstCommand + index].ref.owner == source;
    }
    if (!used) return invalidate();
  }
  selected = pending_.begin();
  for (const auto receipt : invocations) {
    while (selected->accepted) ++selected;
    selected->accepted = true;
    selected->plan.acceptanceEdge = receipt.edge;
    ++selected;
  }
  lastAcceptanceEdge_ = previousEdge;
  promoteTerminalPrefix();
  return true;
}

void TapeTopologyCapture::promoteTerminalPrefix() noexcept {
  // All arenas were reserved at begin, and each physical command is newly
  // owned once. Repeated graph prefixes add no persistent reference storage.
  for (auto& invocation : pending_) {
    if (invocation.publishedPlanIndex != noIndex) continue;
    if (!invocation.accepted || !invocation.encoded) break;
    invocation.publishedPlanIndex = static_cast<std::uint32_t>(plans_.size());
    auto plan = invocation.plan;
    plan.firstPlacement = static_cast<std::uint32_t>(placements_.size());
    plans_.push_back(plan);
    for (std::uint32_t index = 0u; index < plan.placementCount; ++index) {
      const auto& command = pendingCommands_[invocation.firstCommand + index];
      auto placement = command.placement;
      placement.planIndex = invocation.publishedPlanIndex;
      commands_[command.ref.commandIndex].acceptedPlacement = static_cast<std::uint32_t>(placements_.size());
      placements_.push_back(placement);
    }
  }
}

bool TapeTopologyCapture::reject(std::uint64_t attempt) noexcept {
  std::lock_guard lock(mutex_);
  if (!active_ || failed_ || !attempt) return invalidate();
  const auto selected = std::find_if(pending_.begin(), pending_.end(),
      [&](const auto& invocation) { return invocation.plan.planId == attempt; });
  if (selected == pending_.end() || selected->accepted || selected->encoded) return invalidate();
  const auto first = selected->firstCommand;
  const auto count = selected->plan.placementCount;
  for (std::uint32_t index = 0u; index < count; ++index)
    commands_[pendingCommands_[first + index].ref.commandIndex].pendingAttempt = 0u;
  pendingCommands_.erase(pendingCommands_.begin() + first, pendingCommands_.begin() + first + count);
  const auto next = pending_.erase(selected);
  for (auto invocation = next; invocation != pending_.end(); ++invocation)
    invocation->firstCommand -= count;
  staged_.clear();
  liveOrder_.clear();
  stagedPlan_ = {};
  return true;
}

bool TapeTopologyCapture::snapshot(TapeTopologyRuntimeSnapshot& out,
    bool requireComplete) const noexcept {
  out = {};
  std::lock_guard lock(mutex_);
  const bool complete = placements_.size() == commands_.size() &&
      std::all_of(pending_.begin(), pending_.end(),
          [](const auto& invocation) { return invocation.accepted && invocation.encoded; });
  if (!active_ || failed_ || (requireComplete && !complete)) return false;
  try {
    out.capture = capture_;
    out.plans = plans_;
    out.placements = placements_;
    out.complete = complete;
    return true;
  } catch (...) { out = {}; return false; }
}

void TapeTopologyCapture::fail() noexcept {
  std::lock_guard lock(mutex_);
  invalidate();
}
void TapeTopologyCapture::revoke() noexcept {
  std::lock_guard lock(mutex_);
  active_ = false;
  capture_ = {};
  invalidate();
}
bool TapeTopologyCapture::failed() const noexcept {
  std::lock_guard lock(mutex_);
  return failed_;
}

} // namespace dxmt9::render
