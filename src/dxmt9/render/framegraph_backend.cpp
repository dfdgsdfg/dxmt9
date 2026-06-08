#include "framegraph_backend.hpp"

#include "../dxmt9_draw_encoder.hpp"
#include "../framegraph/fg_builder.hpp"
#include "../framegraph/fg_debug_export.hpp"
#include "../framegraph/fg_optimizer.hpp"
#include "util/log/log.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <ios>
#include <string>

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

}  // namespace

RendererFeatureSet resolveRendererFeatures(const char* env,
                                           RendererCompatProfile profile) {
  // L0: there are no implemented features, so the resolved set is always
  // empty. Under the Strict profile, any token present is rejected with a
  // single warning and then ignored.
  if (profile == RendererCompatProfile::Strict && hasAnyFeatureToken(env)) {
    util::logf(util::LogLevel::Warn, "dxmt9-renderer",
               "DXMT9_RENDERER_FEATURES='%s' rejected: strict compat profile "
               "has no renderer features at this stage; ignoring",
               env);
  }
  return RendererFeatureSet{};
}

FrameGraphBackend::FrameGraphBackend()
    : features_([] {
        // Read the env once via the repo's static-const-lambda pattern (see
        // resolveAcquirePolicyFromEnv / layerDisplaySyncEnabled in
        // dxmt9_presenter.mm). L0 default compat_profile is Strict.
        static const RendererFeatureSet value = resolveRendererFeatures(
            std::getenv("DXMT9_RENDERER_FEATURES"),
            RendererCompatProfile::Strict);
        return value;
      }()) {}

namespace {

// Write `<dir>/dag-frame<frameId>-chunk<seqId>-<stage>.json` from a serialized
// snapshot. Matches framegraph::writeDagDump's file naming (Json format) but
// bypasses the static DXMT9_RENDERER_DUMP_DAG cache so the caller-supplied dir
// is honored. Never throws; an unwritable path is silently skipped.
void writeDagJsonToDir(const framegraph::FrameGraph& fg, std::uint64_t frameId,
                       std::uint64_t seqId, const char* stage,
                       const std::string& dir) {
  std::string path = dir;
  if (!path.empty() && path.back() != '/') {
    path.push_back('/');
  }
  path += "dag-frame";
  path += std::to_string(frameId);
  path += "-chunk";
  path += std::to_string(seqId);
  path += "-";
  path += (stage != nullptr ? stage : "");
  path += ".json";

  const std::string contents = framegraph::serializeDagJson(fg, seqId, stage);
  std::ofstream out(path, std::ios::out | std::ios::trunc | std::ios::binary);
  if (out.is_open()) {
    out << contents;  // best-effort: never fail a render on an unwritable dir.
  }
}

}  // namespace

void FrameGraphBackend::observeAndExportDagToDir(const core::ChunkSlot& slot,
                                                 std::uint64_t frameId,
                                                 const std::string& dumpDir)
    const {
  // Test-seam variant: write JSON to an explicit directory (bypasses the
  // dumpDagDir() static cache). PURE OBSERVATION (R-BACK-39.7 side-effect
  // neutral) — reads only `slot`, writes only dump files; mutates no shared /
  // queue / Metal state, so an enabled dump leaves the Metal stream
  // byte-identical to a disabled run.
  framegraph::FrameGraph fg = framegraph::buildFrameGraph(slot, frameId);
  writeDagJsonToDir(fg, frameId, slot.seqId, "pre-opt", dumpDir);
  framegraph::runOptimizer(fg, options_, /*observations=*/nullptr,
                           /*stats=*/nullptr);
  writeDagJsonToDir(fg, frameId, slot.seqId, "post-opt", dumpDir);
}

void FrameGraphBackend::maybeObserveAndExportDag(const core::ChunkSlot& slot,
                                                 std::uint64_t frameId) const {
  // Zero-overhead default path: skip entirely when there is nothing to observe
  // — no dump directory configured AND no features enabled. (At L1 strict the
  // feature set is always empty, so in practice this is gated purely on the
  // dump dir; the feature check keeps the early-out correct once L2+ enables
  // sidecar-observable features.)
  if (!framegraph::dumpDagDir().has_value() && features_.empty()) {
    return;
  }

  // Production path: build -> dump pre-opt -> optimize -> dump post-opt via the
  // spec's writeDagDump, which honors DXMT9_RENDERER_DUMP_DAG_FORMATS (json /
  // dot / mermaid) and warns at most once on an unwritable dir (R-BACK-39.7).
  // PURE OBSERVATION — reads only `slot`, writes only dump files.
  framegraph::FrameGraph fg = framegraph::buildFrameGraph(slot, frameId);
  framegraph::writeDagDump(fg, frameId, slot.seqId, "pre-opt");

  // Run the resolved optimizer passes (at L1 strict: lifetime + loadstore only,
  // since options_ is all-false → the R-BACK-40.5 parity baseline). No
  // cross-frame memoryless observations are tracked at L1 (memoryless stays
  // gated behind an explicit R-BACK-40.4 relaxation that does not exist yet),
  // so pass nullptr for the observation/stats out-params.
  framegraph::runOptimizer(fg, options_, /*observations=*/nullptr,
                           /*stats=*/nullptr);
  framegraph::writeDagDump(fg, frameId, slot.seqId, "post-opt");
}

std::optional<core::metalqueue::QueueSubmissionRecord>
FrameGraphBackend::onChunkReady(encoders::EncodeContext& ctx,
                                std::size_t slotIndex,
                                const core::ChunkSlot& slot) {
  // Side-effect-neutral observe + DAG export side-channel (R-BACK-39.7). This
  // runs BEFORE the encode but cannot influence it: it only reads `slot` and
  // writes debug dump files. The frame id is sourced from `slot.seqId` — the
  // backend keeps no separate frame counter (onFrameBegin is not overridden),
  // and the chunk seq id is the stable per-chunk identifier the DAG export and
  // its file names key off anyway (dag-frame<seqId>-chunk<seqId>-<stage>.json).
  maybeObserveAndExportDag(slot, slot.seqId);

  // R-BACK-40.5: the Metal command stream stays byte-identical to the
  // traditional path. The DAG above is observation-only.
  //
  // TODO(L1-device): drive encode via fg_linearizer::executeLinearization once
  // on-device parity is validated; until then encode stays on encodeChunk and
  // the DAG is observation-only.
  return encoders::encodeChunk(ctx, slotIndex, slot);
}

}  // namespace dxmt9::render
