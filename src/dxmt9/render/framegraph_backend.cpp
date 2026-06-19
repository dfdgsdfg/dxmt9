#include "framegraph_backend.hpp"

#include "tail_present_batch.hpp"

#include "../dxmt9_draw_encoder.hpp"
#include "util/log/log.hpp"

#include <cctype>
#include <cstdlib>
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
  observer_.observeAndExport(slot);

  // R-BACK-40.5: the Metal command stream stays byte-identical to the
  // traditional path. The DAG above is observation-only.
  //
  // TODO(L1-device): drive encode via fg_linearizer::executeLinearization once
  // on-device parity is validated; until then encode stays on encodeChunk and
  // the DAG is observation-only.
  return encoders::encodeChunk(ctx, slotIndex, slot, std::move(options));
}

std::optional<core::metalqueue::QueueSubmissionRecord>
FrameGraphBackend::onChunkBatchReady(
    encoders::EncodeContext& ctx,
    std::span<core::metalqueue::ReadySlotSnapshot> sources) {
  if (sources.size() == 1u) {
    const auto& source = sources.front();
    return onChunkReady(ctx, source.slotIndex, source.slot, {});
  }
  return encodeTailPresentBatch(ctx, sources, observer_);
}

}  // namespace dxmt9::render
