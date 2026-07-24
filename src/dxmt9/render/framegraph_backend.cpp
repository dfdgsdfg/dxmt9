#include "framegraph_backend.hpp"

#include "../dxmt9_draw_encoder.hpp"
#include "../framegraph/fg_builder.hpp"
#include "../framegraph/fg_linearizer.hpp"
#include "util/log/log.hpp"

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

}  // namespace

RendererCompatProfile resolveRendererCompatProfile(const char* env) {
  if (env && std::strcmp(env, "progressive") == 0) {
    return RendererCompatProfile::Progressive;
  }
  return RendererCompatProfile::Strict;
}

RendererFeatureSet resolveRendererFeatures(const char* env,
                                           RendererCompatProfile profile) {
  if (profile == RendererCompatProfile::Strict && hasAnyFeatureToken(env)) {
    util::logf(util::LogLevel::Warn, "dxmt9-renderer",
               "DXMT9_RENDERER_FEATURES='%s' rejected: strict compat profile "
               "disables optimizer features; ignoring",
               env);
    return RendererFeatureSet{};
  }
  if (!env) {
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
      },
      observer_(options_) {}

std::optional<core::metalqueue::QueueSubmissionRecord>
FrameGraphBackend::onChunkReady(encoders::EncodeContext& ctx,
                                std::size_t slotIndex,
                                const core::ChunkSlot& slot,
                                encoders::EncodeChunkOptions options) {
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

  if (features_.passcoalesce && !options.session &&
      !options.hasInjectedCommandBuffer()) {
    framegraph::FrameGraph graph =
        framegraph::buildFrameGraph(slot, slot.seqId, aliasResolver);
    framegraph::OptimizerStats stats{};
    framegraph::runOptimizer(graph, options_, /*observations=*/nullptr, &stats);
    if (stats.pass_coalesced_count != 0) {
      framegraph::ReplayCommandPlan plan =
          framegraph::planReplayCommands(graph, slot);
      if (plan.valid && plan.reordered) {
        options.replayCommandOrder = plan.command_indices;
        return encoders::encodeChunk(ctx, slotIndex, slot,
                                     std::move(options));
      }
      static std::once_flag warning;
      std::call_once(warning, [] {
        util::logf(util::LogLevel::Warn, "dxmt9-renderer",
                   "passcoalesce replay plan was incomplete; falling back to "
                   "source-order v2 replay");
      });
    }
  } else if (features_.passcoalesce) {
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
