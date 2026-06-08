#include "framegraph_backend.hpp"

#include "../dxmt9_draw_encoder.hpp"
#include "util/log/log.hpp"

#include <cctype>
#include <cstdlib>

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
                                const core::ChunkSlot& slot) {
  // L0 (R-BACK-40.5): byte-identical to the traditional path.
  return encoders::encodeChunk(ctx, slotIndex, slot);
}

}  // namespace dxmt9::render
