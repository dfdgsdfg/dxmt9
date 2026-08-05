#include "framegraph_backend.hpp"

#include "../dxmt9_draw_encoder.hpp"
#include "../dxmt9_perf_counters.hpp"
#include "../framegraph/fg_builder.hpp"
#include "../framegraph/fg_linearizer.hpp"
#include "util/log/log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
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

const core::metalqueue::ResolvedPublishedSource* selectedNextSource(
    std::span<const core::metalqueue::ResolvedPublishedSource> selected,
    std::size_t slotIndex,
    const core::ChunkSlot& slot) noexcept {
  if (selected.size() < 2u) {
    return nullptr;
  }
  const auto& current = selected.front();
  const auto& next = selected[1];
  if (current.slot != &slot || current.slotIndex != slotIndex ||
      current.seqId != slot.seqId || current.commandBegin != 0u ||
      current.commandCount != slot.commandCount() || !next.slot ||
      next.seqId != slot.seqId + 1u || next.commandBegin != 0u ||
      next.commandCount != next.slot->commandCount()) {
    return nullptr;
  }
  return &next;
}

}  // namespace

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
  if (options.skipBackendPlanning) {
    return encoders::encodeChunk(ctx, slotIndex, slot, std::move(options));
  }

  // Side-effect-neutral observe + DAG export side-channel (R-BACK-39.7). This
  // runs BEFORE the encode but cannot influence it: it only reads `slot` and
  // writes debug dump files. The shared render::DagObserver tracks the
  // inter-present frame number itself (encode-thread-local counter) so the file
  // names read dag-frame<observeFrame>-chunk<seqId>-<stage>.json and
  // DXMT9_RENDERER_DUMP_DAG_FRAME can scope the dump to a single frame. The DAG
  // dump is backend-agnostic — the TraditionalBackend owns the same observer.
  const framegraph::ResourceAliasResolver aliasResolver =
      makeResourceAliasResolver(ctx.pool);
  observer_.observeAndExport(slot, aliasResolver);

  if (!features_.empty()) {
    framegraph::FrameGraph graph =
        framegraph::buildFrameGraph(slot, slot.seqId, aliasResolver);
    framegraph::OptimizerOptions activeOptions = options_;
    // A general carried render session may already own an open encoder, so
    // passcoalesce command movement remains source-local-only there. The DCE
    // lookahead suffix is different: its held session contains an exact prefix
    // of this same optimized permutation, validated below, so passcoalesce must
    // remain enabled for the tail.
    if ((options.session || options.hasInjectedCommandBuffer()) &&
        options.replayCommandsAlreadyEncoded.empty()) {
      activeOptions.passcoalesce = false;
    }

    std::vector<framegraph::ResourceHandle> overwriteProof;
    if (features_.dce) {
      if (const auto* next = selectedNextSource(
              options.sessionLookaheadSources, slotIndex, slot)) {
        const framegraph::FrameGraph lookahead =
            framegraph::buildFrameGraph(*next->slot, next->seqId,
                                        aliasResolver);
        overwriteProof =
            framegraph::collectDceLookaheadFullOverwrites(lookahead);
        priorDceLookaheadProof_ = overwriteProof;
      }
    }
    framegraph::OptimizerStats stats{};
    framegraph::runOptimizer(
        graph, activeOptions, /*observations=*/nullptr, &stats,
        framegraph::DceLookaheadProof{overwriteProof});

    std::vector<bool> alreadyEncoded(slot.commandCount(), false);
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
          framegraph::planReplayCommands(graph, slot);
      if (plan.valid) {
        if (plan.dropped) {
          perf::countFramegraphDceReplayCommandsOmitted(
              slot.commandCount() - plan.command_indices.size());
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
            util::logf(util::LogLevel::Error, "dxmt9-renderer",
                       "fresh DCE proof changed the already encoded optimized "
                       "prefix");
            return std::nullopt;
          }
          replayPlan = replayPlan.subspan(encoded.size());
        }

        if (plan.reordered || plan.dropped ||
            !options.replayCommandsAlreadyEncoded.empty()) {
          options.replayCommandPlanActive = true;
          options.replayCommandOrder = replayPlan;
          return encoders::encodeChunk(ctx, slotIndex, slot,
                                       std::move(options));
        }
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
                   "source-order v2 replay");
      });
    }
  }
  if (features_.passcoalesce &&
      (options.session || options.hasInjectedCommandBuffer()) &&
      options.replayCommandsAlreadyEncoded.empty()) {
    static std::once_flag warning;
    std::call_once(warning, [] {
      util::logf(util::LogLevel::Warn, "dxmt9-renderer",
                 "passcoalesce is incompatible with an injected/open encode "
                 "session; falling back to source-order v2 replay");
    });
  }

  return encoders::encodeChunk(ctx, slotIndex, slot, std::move(options));
}

}  // namespace dxmt9::render
