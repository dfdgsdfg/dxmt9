#include "framegraph_backend.hpp"

#include "../dxmt9_draw_encoder.hpp"
#include "../dxmt9_perf_counters.hpp"
#include "../framegraph/fg_builder.hpp"
#include "../framegraph/fg_linearizer.hpp"
#include "../framegraph/fg_multi_source_planner.hpp"
#include "util/log/log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

namespace dxmt9::render {

namespace {

// True if the env string carries at least one non-whitespace, non-separator
// token. DXMT9_RENDERER_FEATURES is a comma/space-separated list; at L0 the
// presence of *any* token under the Strict profile is a rejection trigger.
bool hasAnyFeatureToken(const char* env) {
  if (env == nullptr) {
    return false;
  }
  for (const char* c = env; *c != '\0'; ++c) {
    const unsigned char ch = static_cast<unsigned char>(*c);
    if (std::isspace(ch) || ch == ',' || ch == ';') {
      continue;
    }
    return true;
  }
  return false;
}

bool isFeatureSeparator(unsigned char ch) {
  return std::isspace(ch) || ch == ',' || ch == ';';
}

bool activeRenderSeedMerged(const framegraph::FrameGraph& graph) noexcept {
  return std::any_of(graph.passes.begin(), graph.passes.end(),
                     [](const framegraph::PassNode& pass) {
                       return pass.flags.active_render_seed &&
                              pass.commands.count != 0;
                     });
}

framegraph::ActiveRenderPlanningSeed makeActiveRenderPlanningSeed(
    const encoders::ActiveRenderDependencySnapshot& active) noexcept {
  framegraph::ActiveRenderPlanningSeed seed{};
  for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
    seed.targets.color[i] =
        framegraph::TextureHandle{active.colorAttachments[i].value};
    if (active.colorAttachments[i].value != 0) {
      seed.targets.color_count = static_cast<framegraph::u32>(i) + 1u;
    }
  }
  seed.targets.depth = framegraph::TextureHandle{active.depthStencil.value};
  seed.targets.sample_count = active.sampleCount;
  seed.dependency_count = active.dependencyCount;
  seed.complete = active.complete;
  const std::size_t copyCount = std::min<std::size_t>(
      active.dependencyCount, seed.write_dependencies.size());
  for (std::size_t i = 0; i < copyCount; ++i) {
    seed.write_dependencies[i] =
        framegraph::ResourceHandle{active.writeDependencies[i].value};
  }
  return seed;
}

void countReplayFrontierDecision(ReplayFrontierDecision decision) {
  using Outcome = perf::FramegraphActiveRenderSeedOutcome;
  switch (decision) {
    case ReplayFrontierDecision::AcceptedNaturalHead:
      return;
    case ReplayFrontierDecision::AcceptedMovedHead:
      perf::countFramegraphActiveRenderSeedOutcome(Outcome::MovedHeadProved);
      return;
    case ReplayFrontierDecision::FallbackInvalidPlan:
      perf::countFramegraphActiveRenderSeedOutcome(
          Outcome::FallbackInvalidPlan);
      return;
    case ReplayFrontierDecision::FallbackLiveSetMismatch:
      perf::countFramegraphActiveRenderSeedOutcome(
          Outcome::FallbackLiveSetMismatch);
      return;
    case ReplayFrontierDecision::FallbackDuplicateCommand:
      perf::countFramegraphActiveRenderSeedOutcome(
          Outcome::FallbackDuplicateCommand);
      return;
    case ReplayFrontierDecision::FallbackMovedHeadUnproved:
      perf::countFramegraphActiveRenderSeedOutcome(
          Outcome::FallbackMovedHeadUnproved);
      return;
  }
}

std::optional<perf::FramegraphSourceLocalFrontierRollbackReason>
sourceLocalFrontierRollbackReason(ReplayFrontierDecision decision) noexcept {
  using Reason = perf::FramegraphSourceLocalFrontierRollbackReason;
  switch (decision) {
    case ReplayFrontierDecision::FallbackInvalidPlan:
      return Reason::InvalidPlan;
    case ReplayFrontierDecision::FallbackLiveSetMismatch:
      return Reason::LiveSetMismatch;
    case ReplayFrontierDecision::FallbackDuplicateCommand:
      return Reason::DuplicateCommand;
    case ReplayFrontierDecision::FallbackMovedHeadUnproved:
      return Reason::MovedHeadUnproved;
    case ReplayFrontierDecision::AcceptedNaturalHead:
    case ReplayFrontierDecision::AcceptedMovedHead:
      return std::nullopt;
  }
  return std::nullopt;
}

}  // namespace

ReplayFrontierDecision classifyReplayFrontier(
    const framegraph::ReplayCommandPlan& optimized,
    const framegraph::ReplayCommandPlan& natural,
    encoders::EncodeSessionReplayFrontierState state,
    bool activeRenderSeedProvesMovedHead) noexcept {
  if (!optimized.valid || !natural.valid) {
    return ReplayFrontierDecision::FallbackInvalidPlan;
  }
  if (optimized.command_indices.size() != natural.command_indices.size()) {
    return ReplayFrontierDecision::FallbackLiveSetMismatch;
  }
  for (std::size_t i = 0; i < optimized.command_indices.size(); ++i) {
    const framegraph::u32 optimizedCommand = optimized.command_indices[i];
    const framegraph::u32 naturalCommand = natural.command_indices[i];
    if (std::find(optimized.command_indices.begin(),
                  optimized.command_indices.begin() + i,
                  optimizedCommand) != optimized.command_indices.begin() + i ||
        std::find(natural.command_indices.begin(),
                  natural.command_indices.begin() + i,
                  naturalCommand) != natural.command_indices.begin() + i) {
      return ReplayFrontierDecision::FallbackDuplicateCommand;
    }
    if (std::find(natural.command_indices.begin(),
                  natural.command_indices.end(), optimizedCommand) ==
        natural.command_indices.end()) {
      return ReplayFrontierDecision::FallbackLiveSetMismatch;
    }
  }
  if (optimized.command_indices.empty() ||
      optimized.command_indices.front() == natural.command_indices.front()) {
    return ReplayFrontierDecision::AcceptedNaturalHead;
  }
  if (state == encoders::EncodeSessionReplayFrontierState::
                   CleanClosedEncoderNoPendingClear ||
      (state == encoders::EncodeSessionReplayFrontierState::
                    ActiveRenderComplete &&
       activeRenderSeedProvesMovedHead)) {
    return ReplayFrontierDecision::AcceptedMovedHead;
  }
  return ReplayFrontierDecision::FallbackMovedHeadUnproved;
}

framegraph::MultiSourceReplayPlan
FrameGraphBackend::planMultiSourceSessionReplay(
    const resources::Pool& pool,
    const core::metalqueue::SynchronousSourceBorrowBatch& sources,
    const MultiSourceSessionReplayFrontier& frontier) {
  if (profile_ != RendererCompatProfile::Progressive ||
      !features_.passcoalesce || features_.dce || !frontier.valid() ||
      sources.size() < 2u ||
      sources.size() > framegraph::kMaxMultiSourcePlanningSources) {
    return {};
  }

  std::array<framegraph::MultiSourcePlanningSource,
             framegraph::kMaxMultiSourcePlanningSources>
      planningSources{};
  const bool valid = sources.visit(
      [&](const core::metalqueue::GenerationQualifiedSourceBorrow& source,
          std::size_t i) {
        return source.visitPayload(
            [&](const core::metalqueue::SynchronousSourcePayloadBorrow&
                    payloadBorrow) {
          const auto payload = payloadBorrow.checkedView();
          if (source.commandBegin() != 0u ||
              source.commandCount() != payload.commandCount()) {
            return false;
          }
          planningSources[i].payload = payload;
          return true;
        });
      });
  if (!valid) {
    return {};
  }
  std::optional<framegraph::ActiveRenderPlanningSeed> seed;
  if (frontier.activeRender.has_value()) {
    seed = makeActiveRenderPlanningSeed(*frontier.activeRender);
  }
  return framegraph::planMultiSourcePassCoalesceReplay(
      std::span<const framegraph::MultiSourcePlanningSource>(
          planningSources.data(), sources.size()),
      seed ? &*seed : nullptr, makeResourceAliasResolver(pool),
      frontier.collectActiveSeedMergeWitnesses);
}

void FrameGraphBackend::observeMultiSourceSessionReplay(
    const resources::Pool& pool,
    const core::metalqueue::SynchronousSourceBorrowBatch& sources) {
  const framegraph::ResourceAliasResolver aliasResolver =
      makeResourceAliasResolver(pool);
  sources.visit(
      [&](const core::metalqueue::GenerationQualifiedSourceBorrow& source,
          std::size_t) {
        return source.visitPayload(
            [&](const core::metalqueue::SynchronousSourcePayloadBorrow&
                    payloadBorrow) {
          const auto payload = payloadBorrow.checkedView();
          observer_.observeAndExport(payload, source.seqId(), aliasResolver);
        });
      });
}

bool resolvedSourceMatchesFrameGraphInput(
    const core::metalqueue::ResolvedPublishedSource& source,
    std::size_t slotIndex, core::SourcePayloadView payload,
    std::uint64_t seqId,
    core::CpuReadyTape::SourceRef expectedSource) noexcept {
  const core::CpuReadyTape::SourceRef represented{
      .id = source.sourceId,
      .storage = source.storage,
  };
  return source.valid() && expectedSource.valid() &&
         source.source == expectedSource && source.source == represented &&
         source.slotIndex == slotIndex && source.payload == payload &&
         source.seqId == seqId && source.commandBegin == 0u &&
         source.commandCount == payload.commandCount();
}

const core::metalqueue::ResolvedPublishedSource* selectFrameGraphLookahead(
    std::span<const core::metalqueue::ResolvedPublishedSource> selected,
    std::size_t slotIndex, core::SourcePayloadView payload,
    std::uint64_t seqId,
    core::CpuReadyTape::SourceRef currentSource) noexcept {
  if (selected.size() < 2u ||
      !resolvedSourceMatchesFrameGraphInput(
          selected.front(), slotIndex, payload, seqId, currentSource)) {
    return nullptr;
  }
  const auto& next = selected[1];
  if (seqId == std::numeric_limits<std::uint64_t>::max() ||
      next.seqId != seqId + 1u ||
      !resolvedSourceMatchesFrameGraphInput(
          next, next.slotIndex, next.payload, next.seqId, next.source)) {
    return nullptr;
  }
  return &next;
}

bool replayPlanPreservesHeadStableFrontier(
    const framegraph::ReplayCommandPlan& optimized,
    const framegraph::ReplayCommandPlan& natural,
    encoders::EncodeSessionReplayFrontierState state,
    bool activeRenderSeedProvesMovedHead) noexcept {
  const ReplayFrontierDecision decision = classifyReplayFrontier(
      optimized, natural, state, activeRenderSeedProvesMovedHead);
  return decision == ReplayFrontierDecision::AcceptedNaturalHead ||
         decision == ReplayFrontierDecision::AcceptedMovedHead;
}

RendererCompatProfile resolveRendererCompatProfile(const char* env) {
  if (env == nullptr || std::strcmp(env, "progressive") == 0) {
    return RendererCompatProfile::Progressive;
  }
  return RendererCompatProfile::Strict;
}

RendererFeatureSet resolveRendererFeatures(const char* env,
                                           RendererCompatProfile profile) {
  if (profile == RendererCompatProfile::Strict) {
    if (hasAnyFeatureToken(env)) {
      util::logf(util::LogLevel::Warn, "dxmt9-renderer",
                 "DXMT9_RENDERER_FEATURES='%s' rejected: strict compat "
                 "profile disables optimizer features; ignoring",
                 env);
    }
    return RendererFeatureSet{};
  }
  if (!env) {
    // passcoalesce is the sole promoted production optimizer. All other
    // framegraph features remain token-gated and disabled.
    return RendererFeatureSet{
        .passcoalesce = true,
    };
  }
  if (env[0] == '\0' || std::strcmp(env, "0") == 0) {
    return RendererFeatureSet{};
  }

  RendererFeatureSet features{};
  bool rejected = false;
  const char* cursor = env;
  while (*cursor != '\0') {
    while (*cursor != '\0' &&
           isFeatureSeparator(static_cast<unsigned char>(*cursor))) {
      ++cursor;
    }
    const char* begin = cursor;
    while (*cursor != '\0' &&
           !isFeatureSeparator(static_cast<unsigned char>(*cursor))) {
      ++cursor;
    }
    if (begin == cursor) {
      continue;
    }
    const std::string token(begin, cursor);
    if (token == "passcoalesce") {
      features.passcoalesce = true;
    } else if (token == "dce") {
      features.dce = true;
    } else {
      rejected = true;
    }
  }
  if (rejected) {
    util::logf(util::LogLevel::Warn, "dxmt9-renderer",
               "DXMT9_RENDERER_FEATURES='%s' contains unsupported feature "
               "tokens; ignoring unsupported tokens",
               env);
  }
  return features;
}

FrameGraphBackend::FrameGraphBackend()
    : profile_(resolveRendererCompatProfile(
          std::getenv("DXMT9_RENDERER_COMPAT_PROFILE"))),
      features_(resolveRendererFeatures(std::getenv("DXMT9_RENDERER_FEATURES"),
                                        profile_)),
      options_{
          .passcoalesce = features_.passcoalesce,
          .dce = features_.dce,
      },
      observer_(options_) {}

std::vector<std::uint32_t> FrameGraphBackend::dceLookaheadReplayPrefix(
    encoders::EncodeContext& ctx,
    std::size_t slotIndex,
    const core::ChunkSlot& slot) {
  if (!features_.dce) {
    return {};
  }
  static_cast<void>(slotIndex);
  const framegraph::ResourceAliasResolver aliasResolver =
      makeResourceAliasResolver(ctx.pool);
  const framegraph::FrameGraph graph =
      framegraph::buildFrameGraph(slot, slot.seqId, aliasResolver);

  // Bootstrap the scheduling hint without waiting for a first successful
  // successor window. Treat every current resource as a hypothetical
  // overwrite only to locate the earliest pass that could depend on a future
  // proof. planDceLookaheadReplayPrefix never authorizes omission, and the
  // final onChunkReady call still requires a freshly selected successor.
  std::vector<framegraph::ResourceHandle> schedulingProof =
      priorDceLookaheadProof_;
  if (schedulingProof.empty()) {
    schedulingProof.reserve(graph.resources.size());
    for (const framegraph::ResourceNode& resource : graph.resources) {
      if (resource.handle.value != 0) {
        schedulingProof.push_back(resource.handle);
      }
    }
  }
  return framegraph::planDceLookaheadReplayPrefix(
      graph, slot.commandCount(), options_,
      framegraph::DceLookaheadProof{schedulingProof});
}

std::optional<core::metalqueue::QueueSubmissionRecord>
FrameGraphBackend::onChunkReady(encoders::EncodeContext& ctx,
                                std::size_t slotIndex,
                                const core::ChunkSlot& slot,
                                encoders::EncodeChunkOptions options) {
  return onSourceReady(ctx, slotIndex, core::SourcePayloadView(slot),
                       slot.seqId, std::move(options));
}

std::optional<core::metalqueue::QueueSubmissionRecord>
FrameGraphBackend::onSourceReady(encoders::EncodeContext& ctx,
                                 std::size_t slotIndex,
                                 core::SourcePayloadView payload,
                                 std::uint64_t seqId,
                                 encoders::EncodeChunkOptions options) {
  if (options.skipBackendPlanning) {
    return encoders::encodeChunk(ctx, slotIndex, payload, seqId,
                                 std::move(options));
  }

  // Side-effect-neutral observe + DAG export side-channel (R-BACK-39.7). This
  // runs BEFORE the encode but cannot influence it: it only reads `payload` and
  // writes debug dump files. The shared render::DagObserver tracks the
  // inter-present frame number itself (encode-thread-local counter) so the file
  // names read dag-frame<observeFrame>-chunk<seqId>-<stage>.json and
  // DXMT9_RENDERER_DUMP_DAG_FRAME can scope the dump to a single frame. The DAG
  // dump is backend-agnostic — the TraditionalBackend owns the same observer.
  const framegraph::ResourceAliasResolver aliasResolver =
      makeResourceAliasResolver(ctx.pool);
  observer_.observeAndExport(payload, seqId, aliasResolver);

  if (!features_.empty()) {
    framegraph::FrameGraph graph =
        framegraph::buildFrameGraph(payload, seqId, aliasResolver);
    framegraph::OptimizerOptions activeOptions = options_;
    activeOptions.collect_passcoalesce_return_diagnostics =
        activeOptions.passcoalesce && perf::enabled();

    std::vector<framegraph::ResourceHandle> overwriteProof;
    if (features_.dce) {
      if (const auto* next = selectFrameGraphLookahead(
              options.sessionLookaheadSources, slotIndex, payload, seqId,
              options.partitionSource)) {
        const framegraph::FrameGraph lookahead =
            framegraph::buildFrameGraph(next->payload, next->seqId,
                                        aliasResolver);
        overwriteProof =
            framegraph::collectDceLookaheadFullOverwrites(lookahead);
        priorDceLookaheadProof_ = overwriteProof;
      }
    }
    const bool requiresHeadStableFrontier =
        activeOptions.passcoalesce &&
        (options.session || options.hasInjectedCommandBuffer()) &&
        options.replayCommandsAlreadyEncoded.empty();
    framegraph::FrameGraph naturalGraph;
    framegraph::OptimizerStats naturalStats{};
    framegraph::ReplayCommandPlan naturalPlan{};
    bool activeRenderSeedApplied = false;
    if (requiresHeadStableFrontier) {
      naturalGraph =
          framegraph::buildFrameGraph(payload, seqId, aliasResolver);
      framegraph::OptimizerOptions naturalOptions = activeOptions;
      naturalOptions.passcoalesce = false;
      framegraph::runOptimizer(
          naturalGraph, naturalOptions, /*observations=*/nullptr,
          &naturalStats, framegraph::DceLookaheadProof{overwriteProof});
      naturalPlan = framegraph::planReplayCommands(naturalGraph, payload);

      if (options.session) {
        const auto active =
            encoders::encodeChunkSessionActiveRenderDependencySnapshot(
                *options.session);
        if (active) {
          if (!active->complete) {
            perf::countFramegraphActiveRenderSeedOutcome(
                perf::FramegraphActiveRenderSeedOutcome::SnapshotIncomplete);
          }
          framegraph::ActiveRenderPlanningSeed seed{};
          for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
            seed.targets.color[i] =
                framegraph::TextureHandle{active->colorAttachments[i].value};
            if (active->colorAttachments[i].value != 0) {
              seed.targets.color_count =
                  static_cast<framegraph::u32>(i) + 1u;
            }
          }
          seed.targets.depth =
              framegraph::TextureHandle{active->depthStencil.value};
          seed.targets.sample_count = active->sampleCount;
          seed.dependency_count = active->dependencyCount;
          seed.complete = active->complete;
          const std::size_t copyCount = std::min<std::size_t>(
              active->dependencyCount, seed.write_dependencies.size());
          for (std::size_t i = 0; i < copyCount; ++i) {
            seed.write_dependencies[i] = framegraph::ResourceHandle{
                active->writeDependencies[i].value};
          }
          const auto applyResult = framegraph::applyActiveRenderPlanningSeed(
              graph, seed, aliasResolver);
          using Outcome = perf::FramegraphActiveRenderSeedOutcome;
          switch (applyResult) {
            case framegraph::ActiveRenderPlanningSeedResult::Applied:
              activeRenderSeedApplied = true;
              perf::countFramegraphActiveRenderSeedOutcome(
                  Outcome::ApplyApplied);
              break;
            case framegraph::ActiveRenderPlanningSeedResult::Invalid:
              perf::countFramegraphActiveRenderSeedOutcome(
                  Outcome::ApplyInvalid);
              break;
            case framegraph::ActiveRenderPlanningSeedResult::Incomplete:
              perf::countFramegraphActiveRenderSeedOutcome(
                  Outcome::ApplyIncomplete);
              break;
            case framegraph::ActiveRenderPlanningSeedResult::Overflow:
              perf::countFramegraphActiveRenderSeedOutcome(
                  Outcome::ApplyOverflow);
              break;
          }
        } else {
          perf::countFramegraphActiveRenderSeedOutcome(
              perf::FramegraphActiveRenderSeedOutcome::SnapshotAbsent);
        }
      } else {
        perf::countFramegraphActiveRenderSeedOutcome(
            perf::FramegraphActiveRenderSeedOutcome::SnapshotAbsent);
      }
    }

    framegraph::OptimizerStats stats{};
    framegraph::runOptimizer(
        graph, activeOptions, /*observations=*/nullptr, &stats,
        framegraph::DceLookaheadProof{overwriteProof});
    const perf::FramegraphSourceLocalPassCoalesceDiagnostic
        sourceLocalReturnDiagnostic{
            .candidates = stats.pass_coalesce_return_candidates,
            .merged = stats.pass_coalesce_return_merged,
            .blockedCycle = stats.pass_coalesce_return_blocked_cycle,
            .secondNonDraw = stats.pass_coalesce_return_second_non_draw,
            .nonRenderIntervener =
                stats.pass_coalesce_return_non_render_intervener,
            .missingInvariant =
                stats.pass_coalesce_return_missing_invariant,
            .dependencyKept = stats.pass_coalesce_return_dependency_kept,
            .moveBefore = stats.pass_coalesce_return_move_before,
            .moveAfter = stats.pass_coalesce_return_move_after,
            .nonDrawIntervener =
                stats.pass_coalesce_return_non_draw_intervener,
            .semanticIntervener =
                stats.pass_coalesce_return_semantic_intervener,
            .commandlessIntervener =
                stats.pass_coalesce_return_commandless_intervener,
            .commandlessReturn = stats.pass_coalesce_return_commandless,
        };
    const perf::FramegraphSourceProvenance sourceLocalProvenance =
        payload.isArena()
        ? perf::FramegraphSourceProvenance::Arena
        : payload.isLegacy()
            ? perf::FramegraphSourceProvenance::Legacy
            : perf::FramegraphSourceProvenance::Unknown;
    bool sourceLocalReturnOutcomeRecorded = false;
    const auto recordSourceLocalReturnDiagnostic = [&]() {
      if (sourceLocalReturnOutcomeRecorded ||
          sourceLocalReturnDiagnostic.candidates == 0) {
        return false;
      }
      perf::recordFramegraphSourceLocalPassCoalesce(
          sourceLocalProvenance, options.partitionSource.valid(),
          sourceLocalReturnDiagnostic);
      sourceLocalReturnOutcomeRecorded = true;
      return true;
    };
    const auto recordSourceLocalReturnOutcome =
        [&](perf::FramegraphSourceLocalReplayOutcome outcome) {
          if (!recordSourceLocalReturnDiagnostic()) {
            return;
          }
          perf::recordFramegraphSourceLocalReplayOutcome(
              outcome, sourceLocalReturnDiagnostic.candidates,
              sourceLocalReturnDiagnostic.merged);
        };
    const auto recordSourceLocalFrontierRollback =
        [&](perf::FramegraphSourceLocalFrontierRollbackReason reason) {
          if (!recordSourceLocalReturnDiagnostic()) {
            return;
          }
          perf::recordFramegraphSourceLocalFrontierRollback(
              reason, sourceLocalReturnDiagnostic.candidates,
              sourceLocalReturnDiagnostic.merged);
        };
    if (activeRenderSeedApplied) {
      perf::countFramegraphActiveRenderSeedOutcome(
          perf::FramegraphActiveRenderSeedOutcome::PassCoalesceBlockedCycle,
          stats.pass_coalesce_blocked_cycle);
      perf::countFramegraphActiveRenderSeedOutcome(
          perf::FramegraphActiveRenderSeedOutcome::PassCoalesceSecondNonDraw,
          stats.pass_coalesce_second_non_draw);
      if (!activeRenderSeedMerged(graph)) {
        perf::countFramegraphActiveRenderSeedOutcome(
            perf::FramegraphActiveRenderSeedOutcome::AppliedButUnmerged);
      }
    }

    std::vector<bool> alreadyEncoded(payload.commandCount(), false);
    bool alreadyEncodedValid = true;
    for (const std::uint32_t commandIndex :
         options.replayCommandsAlreadyEncoded) {
      if (commandIndex >= alreadyEncoded.size() ||
          alreadyEncoded[commandIndex]) {
        alreadyEncodedValid = false;
        break;
      }
      alreadyEncoded[commandIndex] = true;
    }
    if (!alreadyEncodedValid) {
      recordSourceLocalReturnOutcome(
          perf::FramegraphSourceLocalReplayOutcome::FinalInvalid);
      util::logf(util::LogLevel::Error, "dxmt9-renderer",
                 "DCE lookahead supplied an invalid encoded replay prefix");
      return std::nullopt;
    }

    if (!options.replayCommandsAlreadyEncoded.empty()) {
      for (framegraph::PassNode& pass : graph.passes) {
        if (!pass.flags.dead) {
          continue;
        }
        bool touchesEncodedPrefix = false;
        for (framegraph::u32 i = 0; i < pass.commands.count; ++i) {
          const std::size_t commandOffset =
              static_cast<std::size_t>(pass.commands.first) + i;
          if (commandOffset >= graph.commands.size()) {
            touchesEncodedPrefix = true;
            break;
          }
          const framegraph::u32 commandIndex =
              graph.commands[commandOffset].command_index;
          if (commandIndex >= alreadyEncoded.size() ||
              alreadyEncoded[commandIndex]) {
            touchesEncodedPrefix = true;
            break;
          }
        }
        if (touchesEncodedPrefix) {
          pass.flags.dead = false;
          if (stats.dce_dropped != 0) {
            --stats.dce_dropped;
          }
          ++stats.dce_preserved_unprovable;
        }
      }
    }
    bool activeRenderSeedReplayAccepted = false;
    if (requiresHeadStableFrontier) {
      const encoders::EncodeSessionReplayFrontierState replayFrontierState =
          options.session
              ? encoders::encodeChunkSessionReplayFrontierState(
                    *options.session)
              : encoders::EncodeSessionReplayFrontierState::InjectedUnknown;
      const framegraph::ReplayCommandPlan optimizedPlan =
          framegraph::planReplayCommands(graph, payload);
      const bool activeRenderSeedProvesMovedHead =
          activeRenderSeedApplied && optimizedPlan.valid &&
          !optimizedPlan.command_indices.empty() &&
          framegraph::activeRenderPlanningSeedProvesReplayHead(
              graph, optimizedPlan.command_indices.front());
      const ReplayFrontierDecision frontierDecision = classifyReplayFrontier(
          optimizedPlan, naturalPlan, replayFrontierState,
          activeRenderSeedProvesMovedHead);
      if (activeRenderSeedApplied) {
        countReplayFrontierDecision(frontierDecision);
      }
      activeRenderSeedReplayAccepted =
          activeRenderSeedApplied && activeRenderSeedMerged(graph) &&
          frontierDecision == ReplayFrontierDecision::AcceptedMovedHead;
      if (frontierDecision != ReplayFrontierDecision::AcceptedNaturalHead &&
          frontierDecision != ReplayFrontierDecision::AcceptedMovedHead) {
        const auto rollbackReason =
            sourceLocalFrontierRollbackReason(frontierDecision);
        if (rollbackReason) {
          recordSourceLocalFrontierRollback(*rollbackReason);
        }
        graph = std::move(naturalGraph);
        stats = naturalStats;
      }
    }
    if (features_.dce) {
      perf::countFramegraphDceDropped(stats.dce_dropped);
      perf::countFramegraphDcePreservedUnprovable(
          stats.dce_preserved_unprovable);
      perf::countFramegraphDceCrossChunkProofResources(
          overwriteProof.size());
    }

    if (stats.pass_coalesced_count != 0 || stats.dce_dropped != 0 ||
        !options.replayCommandsAlreadyEncoded.empty()) {
      framegraph::ReplayCommandPlan plan =
          framegraph::planReplayCommands(graph, payload);
      if (plan.valid) {
        if (plan.dropped) {
          perf::countFramegraphDceReplayCommandsOmitted(
              payload.commandCount() - plan.command_indices.size());
        }

        std::span<const framegraph::u32> replayPlan(
            plan.command_indices);
        if (!options.replayCommandsAlreadyEncoded.empty()) {
          const auto encoded = options.replayCommandsAlreadyEncoded;
          const bool exactPrefix =
              encoded.size() <= plan.command_indices.size() &&
              std::equal(encoded.begin(), encoded.end(),
                         plan.command_indices.begin());
          if (!exactPrefix) {
            recordSourceLocalReturnOutcome(
                perf::FramegraphSourceLocalReplayOutcome::FinalInvalid);
            util::logf(util::LogLevel::Error, "dxmt9-renderer",
                       "fresh DCE proof changed the already encoded optimized "
                       "prefix");
            return std::nullopt;
          }
          replayPlan = replayPlan.subspan(encoded.size());
        }

        if (plan.reordered || plan.dropped ||
            !options.replayCommandsAlreadyEncoded.empty()) {
          if (activeRenderSeedReplayAccepted) {
            perf::countFramegraphActiveRenderSeedOutcome(
                perf::FramegraphActiveRenderSeedOutcome::ReplayActivated);
          }
          options.replayCommandPlanActive = true;
          options.replayCommandOrder = replayPlan;
          recordSourceLocalReturnOutcome(
              plan.reordered
                  ? perf::FramegraphSourceLocalReplayOutcome::
                        FinalReorderedActivated
                  : perf::FramegraphSourceLocalReplayOutcome::
                        FinalNaturalOrder);
          return encoders::encodeChunk(ctx, slotIndex, payload, seqId,
                                       std::move(options));
        }
        recordSourceLocalReturnOutcome(
            perf::FramegraphSourceLocalReplayOutcome::FinalNaturalOrder);
      } else {
        recordSourceLocalReturnOutcome(
            perf::FramegraphSourceLocalReplayOutcome::FinalInvalid);
      }
      if (!options.replayCommandsAlreadyEncoded.empty()) {
        util::logf(util::LogLevel::Error, "dxmt9-renderer",
                   "DCE lookahead could not validate the already encoded "
                   "optimized prefix");
        return std::nullopt;
      }
      static std::once_flag warning;
      std::call_once(warning, [] {
        util::logf(util::LogLevel::Warn, "dxmt9-renderer",
                   "framegraph replay plan was incomplete; falling back to "
                   "source-order canonical replay");
      });
    }
    recordSourceLocalReturnOutcome(
        perf::FramegraphSourceLocalReplayOutcome::FinalNaturalOrder);
  }
  return encoders::encodeChunk(ctx, slotIndex, payload, seqId,
                               std::move(options));
}

}  // namespace dxmt9::render
