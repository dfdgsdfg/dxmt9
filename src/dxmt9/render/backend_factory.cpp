#include "backend_factory.hpp"

#include "framegraph_backend.hpp"
#include "traditional_backend.hpp"

#include "util/log/log.hpp"

#include <cstdlib>
#include <cstring>
#include <exception>

namespace dxmt9::render {

BackendMode resolveBackendMode(const char* renderModeEnv) {
  // The proven L1 framegraph lane is the runtime default. Keep every explicit
  // legacy/invalid value conservative so a diagnostic launch always has a
  // simple rollback path.
  if (renderModeEnv == nullptr ||
      std::strcmp(renderModeEnv, "framegraph") == 0) {
    return BackendMode::FrameGraph;
  }
  // "" / "0" / "traditional" / unknown strings → Traditional.
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
  const char* env = std::getenv("DXMT9_RENDER_MODE");
  if (env && env[0] != '\0' && std::strcmp(env, "0") != 0 &&
      std::strcmp(env, "traditional") != 0 &&
      std::strcmp(env, "framegraph") != 0) {
    util::logf(util::LogLevel::Warn, "dxmt9-renderer",
               "DXMT9_RENDER_MODE='%s' is unsupported; falling back to "
               "Traditional",
               env);
  }
  return createBackend(resolveBackendMode(env));
}

}  // namespace dxmt9::render
