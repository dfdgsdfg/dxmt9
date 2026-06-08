#include "backend_factory.hpp"

#include "framegraph_backend.hpp"
#include "traditional_backend.hpp"

#include "util/log/log.hpp"

#include <cstdlib>
#include <cstring>
#include <exception>

namespace dxmt9::render {

BackendMode resolveBackendMode(const char* renderModeEnv) {
  // Repo env-set convention: set when non-null, non-empty, and not "0".
  const bool set = renderModeEnv && renderModeEnv[0] != '\0' &&
                   std::strcmp(renderModeEnv, "0") != 0;
  if (set && std::strcmp(renderModeEnv, "framegraph") == 0) {
    return BackendMode::FrameGraph;
  }
  // Unset / "" / "0" / unknown strings (including "traditional") → Traditional.
  return BackendMode::Traditional;
}

std::unique_ptr<IRenderBackend> createBackend(BackendMode mode) {
  try {
    switch (mode) {
      case BackendMode::FrameGraph:
        return std::make_unique<FrameGraphBackend>();
      case BackendMode::Traditional:
        return std::make_unique<TraditionalBackend>();
    }
    // Unreachable for the closed enum, but keep a defined fallback.
    return std::make_unique<TraditionalBackend>();
  } catch (const std::exception& e) {
    util::logf(util::LogLevel::Warn, "dxmt9-renderer",
               "backend construction failed (mode=%d): %s; falling back to "
               "Traditional",
               static_cast<int>(mode), e.what());
    return std::make_unique<TraditionalBackend>();
  }
}

std::unique_ptr<IRenderBackend> createBackendFromEnv() {
  return createBackend(resolveBackendMode(std::getenv("DXMT9_RENDER_MODE")));
}

}  // namespace dxmt9::render
