#include "fg_multi_source_planner.hpp"

#include "fg_optimizer.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace dxmt9::framegraph {

namespace {

bool validateSources(std::span<const MultiSourcePlanningSource> sources,
                     std::size_t& totalCommands) noexcept {
  totalCommands = 0;
  if (sources.empty() || sources.size() > kMaxMultiSourcePlanningSources) {
    return false;
  }
  for (const MultiSourcePlanningSource& source : sources) {
    const std::size_t commandCount = source.payload.commandCount();
    if (!source.payload.valid() ||
        commandCount > std::numeric_limits<u32>::max() ||
        totalCommands >
            std::numeric_limits<u32>::max() - commandCount) {
      return false;
    }
    totalCommands += commandCount;
  }
  return true;
}

bool accessReads(AccessKind kind) noexcept {
  return kind == AccessKind::Read || kind == AccessKind::ReadWrite;
}

bool accessWrites(AccessKind kind) noexcept {
  return kind == AccessKind::Write || kind == AccessKind::Clear ||
         kind == AccessKind::ReadWrite;
}

void addEdgeOnce(FrameGraph& graph, u32 src, u32 dst,
                 ResourceHandle resource) {
  if (src == dst) {
    return;
  }
  const auto duplicate = std::find_if(
      graph.edges.begin(), graph.edges.end(), [&](const Edge& edge) {
        return edge.src_pass == src && edge.dst_pass == dst &&
               edge.resource == resource;
      });
  if (duplicate == graph.edges.end()) {
    graph.edges.push_back(Edge{
        .src_pass = src,
        .dst_pass = dst,
        .resource = resource,
    });
  }
}

void inferCombinedHazards(FrameGraph& graph) {
  graph.edges.clear();
  for (const ResourceNode& resource : graph.resources) {
    int lastWrite = -1;
    for (std::size_t i = 0; i < resource.accesses.size(); ++i) {
      const AccessLog& access = resource.accesses[i];
      const AccessKind kind = static_cast<AccessKind>(access.access_kind);
      if (accessReads(kind) && lastWrite >= 0) {
        addEdgeOnce(graph,
                    resource.accesses[static_cast<std::size_t>(lastWrite)]
                        .pass_index,
                    access.pass_index, resource.handle);
      }
      if (accessWrites(kind)) {
        if (lastWrite >= 0) {
          addEdgeOnce(graph,
                      resource.accesses[static_cast<std::size_t>(lastWrite)]
                          .pass_index,
                      access.pass_index, resource.handle);
        }
        for (std::size_t read =
                 static_cast<std::size_t>(lastWrite + 1);
             read < i; ++read) {
          const AccessKind prior = static_cast<AccessKind>(
              resource.accesses[read].access_kind);
          if (accessReads(prior)) {
            addEdgeOnce(graph, resource.accesses[read].pass_index,
                        access.pass_index, resource.handle);
          }
        }
        lastWrite = static_cast<int>(i);
      }
    }
  }
}

bool appendSourceGraph(FrameGraph& combined, const FrameGraph& source,
                       u32 commandBase) {
  const std::size_t passBase = combined.passes.size();
  const std::size_t drawBase = combined.draws.size();
  const std::size_t commandRefBase = combined.commands.size();
  if (passBase > std::numeric_limits<u32>::max() ||
      drawBase > std::numeric_limits<u32>::max() ||
      commandRefBase > std::numeric_limits<u32>::max()) {
    return false;
  }

  combined.draws.reserve(drawBase + source.draws.size());
  for (DrawRef draw : source.draws) {
    if (draw.command_index >
        std::numeric_limits<u32>::max() - commandBase) {
      return false;
    }
    draw.command_index += commandBase;
    combined.draws.push_back(draw);
  }

  combined.commands.reserve(commandRefBase + source.commands.size());
  for (CommandRef command : source.commands) {
    if (command.command_index >
        std::numeric_limits<u32>::max() - commandBase) {
      return false;
    }
    command.command_index += commandBase;
    combined.commands.push_back(command);
  }

  combined.passes.reserve(passBase + source.passes.size());
  for (PassNode pass : source.passes) {
    if (pass.draws.first > std::numeric_limits<u32>::max() - drawBase ||
        pass.commands.first >
            std::numeric_limits<u32>::max() - commandRefBase) {
      return false;
    }
    pass.draws.first += static_cast<u32>(drawBase);
    pass.commands.first += static_cast<u32>(commandRefBase);
    combined.passes.push_back(pass);
  }

  for (const ResourceNode& sourceResource : source.resources) {
    std::size_t resourceIndex = findResourceIndex(combined,
                                                  sourceResource.handle);
    if (resourceIndex == combined.resources.size()) {
      ResourceNode resource{};
      resource.handle = sourceResource.handle;
      combined.resources.push_back(std::move(resource));
      resourceIndex = combined.resources.size() - 1u;
    }
    ResourceNode& resource = combined.resources[resourceIndex];
    resource.classifier_flags.lock_seen =
        resource.classifier_flags.lock_seen ||
        sourceResource.classifier_flags.lock_seen;
    resource.classifier_flags.readback_seen =
        resource.classifier_flags.readback_seen ||
        sourceResource.classifier_flags.readback_seen;
    resource.classifier_flags.cross_frame_seen =
        resource.classifier_flags.cross_frame_seen ||
        sourceResource.classifier_flags.cross_frame_seen;
    for (AccessLog access : sourceResource.accesses) {
      if (access.pass_index >
          std::numeric_limits<u32>::max() - passBase) {
        return false;
      }
      access.pass_index += static_cast<u32>(passBase);
      resource.accesses.push_back(access);
    }
  }
  combined.flush_boundary = combined.flush_boundary || source.flush_boundary;
  return true;
}

std::vector<RetainedSourceCommandLocator> naturalPlan(
    std::span<const MultiSourcePlanningSource> sources) {
  std::size_t count = 0;
  for (const MultiSourcePlanningSource& source : sources) {
    count += source.payload.commandCount();
  }
  std::vector<RetainedSourceCommandLocator> natural;
  natural.reserve(count);
  for (std::size_t sourceIndex = 0; sourceIndex < sources.size();
       ++sourceIndex) {
    const std::size_t commandCount =
        sources[sourceIndex].payload.commandCount();
    for (std::size_t commandIndex = 0; commandIndex < commandCount;
         ++commandIndex) {
      natural.push_back(RetainedSourceCommandLocator{
          .retainedSourceIndex = static_cast<u32>(sourceIndex),
          .commandIndex = static_cast<u32>(commandIndex),
      });
    }
  }
  return natural;
}

MultiSourceReplayPlan naturalFallback(
    std::span<const MultiSourcePlanningSource> sources) {
  MultiSourceReplayPlan result{};
  std::size_t totalCommands = 0;
  if (!validateSources(sources, totalCommands)) {
    return result;
  }
  result.commands = naturalPlan(sources);
  result.validation = validateMultiSourceReplayPermutation(sources,
                                                           result.commands);
  result.disposition = result.validation == MultiSourceReplayValidation::Valid
                           ? MultiSourceReplayDisposition::NaturalFifo
                           : MultiSourceReplayDisposition::InvalidInput;
  if (result.disposition == MultiSourceReplayDisposition::InvalidInput) {
    result.commands.clear();
  }
  return result;
}

MultiSourceSeedApplyDiagnostic seedApplyDiagnostic(
    ActiveRenderPlanningSeedResult result) noexcept {
  switch (result) {
  case ActiveRenderPlanningSeedResult::Applied:
    return MultiSourceSeedApplyDiagnostic::Applied;
  case ActiveRenderPlanningSeedResult::Invalid:
    return MultiSourceSeedApplyDiagnostic::Invalid;
  case ActiveRenderPlanningSeedResult::Incomplete:
    return MultiSourceSeedApplyDiagnostic::Incomplete;
  case ActiveRenderPlanningSeedResult::Overflow:
    return MultiSourceSeedApplyDiagnostic::Overflow;
  }
  return MultiSourceSeedApplyDiagnostic::Invalid;
}

void inspectActiveTargetMatch(
    const FrameGraph& graph, MultiSourceReplayDiagnostics& diagnostics) {
  diagnostics.activeTargetMatch =
      MultiSourceActiveTargetMatchDiagnostic::Absent;
  for (std::size_t passIndex = 1; passIndex < graph.passes.size();
       ++passIndex) {
    const PassNode& pass = graph.passes[passIndex];
    if (pass.kind != PassKind::Render || pass.flags.dead ||
        !(pass.targets == graph.passes.front().targets)) {
      continue;
    }
    diagnostics.activeTargetMatch =
        MultiSourceActiveTargetMatchDiagnostic::Present;
    diagnostics.firstMatchingPassDistance = static_cast<u32>(
        std::min<std::size_t>(passIndex,
                              std::numeric_limits<u32>::max()));
    if (pass.commands.count != 0 &&
        pass.commands.first < graph.commands.size() &&
        pass.commands.count <=
            graph.commands.size() - pass.commands.first) {
      diagnostics.firstMatchingCommand =
          graph.commands[pass.commands.first].kind ==
                  core::MetalCommandKind::DrawRun
              ? MultiSourceFirstMatchingCommandDiagnostic::DrawRun
              : MultiSourceFirstMatchingCommandDiagnostic::NonDraw;
    }
    for (std::size_t intervening = 1; intervening < passIndex;
         ++intervening) {
      diagnostics.interveningNonRender =
          diagnostics.interveningNonRender ||
          graph.passes[intervening].kind != PassKind::Render;
    }
    return;
  }
}

bool seedMerged(const FrameGraph& graph) noexcept {
  return std::any_of(graph.passes.begin(), graph.passes.end(),
                     [](const PassNode& pass) {
                       return pass.flags.active_render_seed &&
                           !pass.flags.dead && pass.commands.count != 0;
                     });
}

}  // namespace

MultiSourceReplayValidation validateMultiSourceReplayPermutation(
    std::span<const MultiSourcePlanningSource> sources,
    std::span<const RetainedSourceCommandLocator> commands) {
  std::size_t totalCommands = 0;
  if (!validateSources(sources, totalCommands)) {
    return MultiSourceReplayValidation::InvalidSource;
  }
  if (commands.size() < totalCommands) {
    return MultiSourceReplayValidation::Missing;
  }
  if (commands.size() > totalCommands) {
    return MultiSourceReplayValidation::Duplicate;
  }

  std::vector<std::size_t> sourceBases(sources.size(), 0);
  std::size_t base = 0;
  for (std::size_t i = 0; i < sources.size(); ++i) {
    sourceBases[i] = base;
    base += sources[i].payload.commandCount();
  }
  std::vector<bool> seen(totalCommands, false);
  std::vector<std::size_t> naturalIndices;
  naturalIndices.reserve(commands.size());
  for (const RetainedSourceCommandLocator locator : commands) {
    if (locator.retainedSourceIndex >= sources.size()) {
      return MultiSourceReplayValidation::InvalidSource;
    }
    const core::SourcePayloadView payload =
        sources[locator.retainedSourceIndex].payload;
    if (locator.commandIndex >= payload.commandCount()) {
      return MultiSourceReplayValidation::InvalidCommand;
    }
    const std::size_t naturalIndex =
        sourceBases[locator.retainedSourceIndex] + locator.commandIndex;
    if (seen[naturalIndex]) {
      return MultiSourceReplayValidation::Duplicate;
    }
    seen[naturalIndex] = true;
    naturalIndices.push_back(naturalIndex);
  }
  if (std::find(seen.begin(), seen.end(), false) != seen.end()) {
    return MultiSourceReplayValidation::Missing;
  }

  constexpr std::size_t kMissingNaturalIndex =
      std::numeric_limits<std::size_t>::max();
  std::array<std::size_t, kMaxMultiSourcePlanningSources> prefixMax{};
  std::array<bool, kMaxMultiSourcePlanningSources> prefixPresent{};
  for (std::size_t i = 0; i < commands.size(); ++i) {
    const u32 sourceIndex = commands[i].retainedSourceIndex;
    const auto kind = sources[sourceIndex]
                          .payload.commandAt(commands[i].commandIndex)
                          .kind();
    if (kind != core::MetalCommandKind::DrawRun) {
      for (std::size_t other = 0; other < sources.size(); ++other) {
        if (other != sourceIndex && prefixPresent[other] &&
            prefixMax[other] > naturalIndices[i]) {
          return MultiSourceReplayValidation::NonDrawCrossSourceMovement;
        }
      }
    }
    prefixPresent[sourceIndex] = true;
    prefixMax[sourceIndex] =
        std::max(prefixMax[sourceIndex], naturalIndices[i]);
  }

  std::array<std::size_t, kMaxMultiSourcePlanningSources> suffixMin{};
  suffixMin.fill(kMissingNaturalIndex);
  for (std::size_t i = commands.size(); i-- > 0;) {
    const u32 sourceIndex = commands[i].retainedSourceIndex;
    const auto kind = sources[sourceIndex]
                          .payload.commandAt(commands[i].commandIndex)
                          .kind();
    if (kind != core::MetalCommandKind::DrawRun) {
      for (std::size_t other = 0; other < sources.size(); ++other) {
        if (other != sourceIndex &&
            suffixMin[other] < naturalIndices[i]) {
          return MultiSourceReplayValidation::NonDrawCrossSourceMovement;
        }
      }
    }
    suffixMin[sourceIndex] =
        std::min(suffixMin[sourceIndex], naturalIndices[i]);
  }
  return MultiSourceReplayValidation::Valid;
}

MultiSourceReplayPlan planMultiSourcePassCoalesceReplay(
    std::span<const MultiSourcePlanningSource> sources,
    const ActiveRenderPlanningSeed* activeRenderSeed,
    ResourceAliasResolver aliasResolver,
    bool collectActiveSeedMergeWitnesses) {
  MultiSourceReplayPlan fallback = naturalFallback(sources);
  if (!fallback.valid()) {
    return fallback;
  }

  FrameGraph combined{};
  u32 commandBase = 0;
  for (std::size_t sourceIndex = 0; sourceIndex < sources.size();
       ++sourceIndex) {
    const core::SourcePayloadView payload = sources[sourceIndex].payload;
    const FrameGraph source = buildFrameGraph(
        payload, static_cast<u64>(sourceIndex), aliasResolver);
    if (!appendSourceGraph(combined, source, commandBase)) {
      fallback.diagnostics.outcome =
          MultiSourcePlannerOutcome::InvalidInput;
      return fallback;
    }
    commandBase += static_cast<u32>(payload.commandCount());
  }
  inferCombinedHazards(combined);

  if (activeRenderSeed) {
    const auto seedResult = applyActiveRenderPlanningSeed(
        combined, *activeRenderSeed, aliasResolver);
    fallback.diagnostics.seedApply = seedApplyDiagnostic(seedResult);
    if (seedResult != ActiveRenderPlanningSeedResult::Applied) {
      fallback.diagnostics.outcome =
          MultiSourcePlannerOutcome::SeedRejected;
      return fallback;
    }
    inspectActiveTargetMatch(combined, fallback.diagnostics);
  }

  OptimizerOptions options{};
  options.passcoalesce = true;
  OptimizerStats stats{};
  std::vector<encoders::ActiveSeedMergeCommandWitness>
      activeSeedMergeCommandStorage;
  std::optional<ActiveSeedMergeWitnessSink> activeSeedMergeWitnessSink;
  if (collectActiveSeedMergeWitnesses && activeRenderSeed) {
    activeSeedMergeCommandStorage.resize(
        combined.passes.empty() ? 0u : combined.passes.size() - 1u);
    activeSeedMergeWitnessSink.emplace(ActiveSeedMergeWitnessSink{
        .storage = activeSeedMergeCommandStorage,
    });
  }
  runOptimizer(combined, options, nullptr, &stats, {},
               activeSeedMergeWitnessSink ? &*activeSeedMergeWitnessSink
                                          : nullptr);
  fallback.diagnostics.seedSecondNonDraw =
      stats.pass_coalesce_second_non_draw != 0;
  fallback.diagnostics.seedBlockedCycle =
      stats.pass_coalesce_blocked_cycle != 0;
  fallback.diagnostics.optimizerMergeCount = stats.pass_coalesced_count;
  fallback.diagnostics.activeSeedMergeCount =
      stats.pass_coalesce_active_seed_merge_count;
  fallback.diagnostics.activeSeedMergeDistanceTotal =
      stats.pass_coalesce_active_seed_merge_distance_total;
  fallback.diagnostics.activeSeedMergeDistanceMax =
      stats.pass_coalesce_active_seed_merge_distance_max;
  fallback.diagnostics.activeSeedCommandBefore =
      stats.pass_coalesce_active_seed_command_before;
  fallback.diagnostics.activeSeedCommandAfter =
      stats.pass_coalesce_active_seed_command_after;
  fallback.diagnostics.activeSeedEmptyIntervening =
      stats.pass_coalesce_active_seed_empty_intervening;
  if (activeSeedMergeWitnessSink) {
    auto& diagnostics = fallback.diagnostics;
    diagnostics.activeSeedMergeWitnessOverflow =
        activeSeedMergeWitnessSink->overflow;
    if (!diagnostics.activeSeedMergeWitnessOverflow) {
      const auto publishableWitnesses =
          activeSeedMergeWitnessSink->publishable();
      diagnostics.activeSeedMergeWitnesses.reserve(
          publishableWitnesses.size());
      for (const auto& witness : publishableWitnesses) {
        u32 sourceIndex = 0;
        u32 localIndex = witness.flattenedCommandIndex;
        while (sourceIndex < sources.size()) {
          const u32 sourceCommandCount = static_cast<u32>(
              sources[sourceIndex].payload.commandCount());
          if (localIndex < sourceCommandCount) {
            break;
          }
          localIndex -= sourceCommandCount;
          ++sourceIndex;
        }
        if (!witness.valid() || sourceIndex >= sources.size() ||
            sources[sourceIndex].payload.commandAt(localIndex).kind() !=
                core::MetalCommandKind::DrawRun) {
          diagnostics.activeSeedMergeWitnessMismatch = true;
          break;
        }
        diagnostics.activeSeedMergeWitnesses.push_back(
            encoders::ActiveSeedMergeTargetWitness{
                .retainedSourceIndex = sourceIndex,
                .commandIndex = localIndex,
                .mergeOrdinal = witness.mergeOrdinal,
                .mergeDistance = witness.mergeDistance,
            });
      }
      diagnostics.activeSeedMergeWitnessMismatch =
          diagnostics.activeSeedMergeWitnessMismatch ||
          diagnostics.activeSeedMergeWitnesses.size() !=
              stats.pass_coalesce_active_seed_merge_count;
      std::sort(diagnostics.activeSeedMergeWitnesses.begin(),
                diagnostics.activeSeedMergeWitnesses.end(),
                [](const auto& left, const auto& right) {
                  if (left.retainedSourceIndex != right.retainedSourceIndex) {
                    return left.retainedSourceIndex < right.retainedSourceIndex;
                  }
                  if (left.commandIndex != right.commandIndex) {
                    return left.commandIndex < right.commandIndex;
                  }
                  return left.mergeOrdinal < right.mergeOrdinal;
                });
      for (std::size_t i = 1u;
           i < diagnostics.activeSeedMergeWitnesses.size(); ++i) {
        const auto& previous = diagnostics.activeSeedMergeWitnesses[i - 1u];
        const auto& current = diagnostics.activeSeedMergeWitnesses[i];
        if (previous.retainedSourceIndex == current.retainedSourceIndex &&
            previous.commandIndex == current.commandIndex) {
          diagnostics.activeSeedMergeWitnessMismatch = true;
          break;
        }
      }
    }
    if (diagnostics.activeSeedMergeWitnessOverflow ||
        diagnostics.activeSeedMergeWitnessMismatch) {
      diagnostics.activeSeedMergeWitnesses.clear();
    }
  }
  fallback.diagnostics.merge =
      stats.pass_coalesced_count == 0
      ? MultiSourceMergeDiagnostic::None
      : seedMerged(combined)
          ? MultiSourceMergeDiagnostic::SeedMerged
          : MultiSourceMergeDiagnostic::NonSeedOnly;
  if (fallback.diagnostics.merge == MultiSourceMergeDiagnostic::SeedMerged) {
    const u64 intervening = fallback.diagnostics.activeSeedCommandBefore +
        fallback.diagnostics.activeSeedCommandAfter +
        fallback.diagnostics.activeSeedEmptyIntervening;
    fallback.diagnostics.activeSeedMergeAttributionMissing =
        fallback.diagnostics.activeSeedMergeCount == 0u ||
        fallback.diagnostics.optimizerMergeCount <
            fallback.diagnostics.activeSeedMergeCount ||
        fallback.diagnostics.activeSeedMergeDistanceMax == 0u ||
        fallback.diagnostics.activeSeedMergeDistanceTotal <
            fallback.diagnostics.activeSeedMergeCount ||
        fallback.diagnostics.activeSeedMergeDistanceMax >
            fallback.diagnostics.activeSeedMergeDistanceTotal ||
        intervening != fallback.diagnostics.activeSeedMergeDistanceTotal -
            fallback.diagnostics.activeSeedMergeCount;
  }
  if (stats.pass_coalesced_count == 0) {
    fallback.diagnostics.outcome =
        fallback.diagnostics.activeTargetMatch ==
                MultiSourceActiveTargetMatchDiagnostic::Absent
            ? MultiSourcePlannerOutcome::NoActiveTargetMatch
            : MultiSourcePlannerOutcome::NoMerge;
    return fallback;
  }

  MultiSourceReplayPlan result{};
  result.diagnostics = fallback.diagnostics;
  result.commands.reserve(commandBase);
  std::vector<bool> seen(commandBase, false);
  for (const PassNode& pass : combined.passes) {
    if (pass.flags.dead || pass.commands.first > combined.commands.size() ||
        pass.commands.count >
            combined.commands.size() - pass.commands.first) {
      fallback.diagnostics = result.diagnostics;
      fallback.diagnostics.outcome =
          MultiSourcePlannerOutcome::PermutationRejected;
      return fallback;
    }
    for (u32 i = 0; i < pass.commands.count; ++i) {
      const CommandRef& command =
          combined.commands[pass.commands.first + i];
      if (command.command_index >= commandBase ||
          seen[command.command_index]) {
        fallback.diagnostics = result.diagnostics;
        fallback.diagnostics.outcome =
            MultiSourcePlannerOutcome::PermutationRejected;
        return fallback;
      }
      seen[command.command_index] = true;
      u32 sourceIndex = 0;
      u32 localIndex = command.command_index;
      while (sourceIndex < sources.size()) {
        const u32 sourceCount =
            static_cast<u32>(sources[sourceIndex].payload.commandCount());
        if (localIndex < sourceCount) {
          break;
        }
        localIndex -= sourceCount;
        ++sourceIndex;
      }
      if (sourceIndex >= sources.size() ||
          sources[sourceIndex].payload.commandAt(localIndex).kind() !=
              command.kind) {
        fallback.diagnostics = result.diagnostics;
        fallback.diagnostics.outcome =
            MultiSourcePlannerOutcome::PermutationRejected;
        return fallback;
      }
      result.commands.push_back(RetainedSourceCommandLocator{
          .retainedSourceIndex = sourceIndex,
          .commandIndex = localIndex,
      });
    }
  }
  if (std::find(seen.begin(), seen.end(), false) != seen.end()) {
    fallback.diagnostics = result.diagnostics;
    fallback.diagnostics.outcome =
        MultiSourcePlannerOutcome::PermutationRejected;
    return fallback;
  }

  result.validation = validateMultiSourceReplayPermutation(sources,
                                                            result.commands);
  if (result.validation != MultiSourceReplayValidation::Valid) {
    fallback.diagnostics = result.diagnostics;
    fallback.diagnostics.outcome =
        MultiSourcePlannerOutcome::PermutationRejected;
    return fallback;
  }
  if (result.commands == fallback.commands) {
    fallback.diagnostics = result.diagnostics;
    fallback.diagnostics.outcome =
        MultiSourcePlannerOutcome::NaturalAfterMerge;
    return fallback;
  }
  if (activeRenderSeed &&
      result.commands.front() != fallback.commands.front()) {
    u32 flattenedHead = result.commands.front().commandIndex;
    for (u32 sourceIndex = 0;
         sourceIndex < result.commands.front().retainedSourceIndex;
         ++sourceIndex) {
      flattenedHead +=
          static_cast<u32>(sources[sourceIndex].payload.commandCount());
    }
    if (!activeRenderPlanningSeedProvesReplayHead(combined,
                                                  flattenedHead)) {
      fallback.diagnostics.outcome =
          MultiSourcePlannerOutcome::MovedHeadUnproved;
      return fallback;
    }
  }
  result.disposition = MultiSourceReplayDisposition::Planned;
  result.diagnostics.outcome = MultiSourcePlannerOutcome::Planned;
  return result;
}

MultiSourceReplayRuns buildMultiSourceReplayRuns(
    std::span<const MultiSourcePlanningSource> sources,
    const MultiSourceReplayPlan& plan) {
  MultiSourceReplayRuns result{};
  if (!plan.valid() ||
      validateMultiSourceReplayPermutation(sources, plan.commands) !=
          MultiSourceReplayValidation::Valid) {
    return result;
  }

  result.runs.reserve(plan.commands.size());
  for (const RetainedSourceCommandLocator command : plan.commands) {
    if (!result.runs.empty()) {
      MultiSourceReplayRun& tail = result.runs.back();
      const u64 tailEnd = static_cast<u64>(tail.commandBegin) +
                          tail.commandCount;
      if (tail.retainedSourceIndex == command.retainedSourceIndex &&
          tailEnd == command.commandIndex) {
        ++tail.commandCount;
        continue;
      }
    }
    result.runs.push_back(MultiSourceReplayRun{
        .retainedSourceIndex = command.retainedSourceIndex,
        .commandBegin = command.commandIndex,
        .commandCount = 1u,
    });
  }
  std::array<u32, kMaxMultiSourcePlanningSources> sourceFragmentCounts{};
  for (const MultiSourceReplayRun& run : result.runs) {
    ++sourceFragmentCounts[run.retainedSourceIndex];
  }
  std::array<u32, kMaxMultiSourcePlanningSources> sourceFragmentOrdinals{};
  for (std::size_t runIndex = 0; runIndex < result.runs.size(); ++runIndex) {
    MultiSourceReplayRun& run = result.runs[runIndex];
    run.sourceFragmentOrdinal =
        sourceFragmentOrdinals[run.retainedSourceIndex]++;
    run.sourceFragmentCount =
        sourceFragmentCounts[run.retainedSourceIndex];
    run.transactionFragmentOrdinal = static_cast<u32>(runIndex);
    run.transactionFragmentCount = static_cast<u32>(result.runs.size());
  }
  result.validation = MultiSourceReplayRunValidation::Valid;
  return result;
}

}  // namespace dxmt9::framegraph
