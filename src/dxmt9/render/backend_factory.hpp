#pragma once

// backend_factory (Task A2) — env resolution + construction for the
// modern-renderer backend seam. The L0 backends are stateless (default
// ctor, no queue hooks), so the factory is a pure resolver plus a
// construct-by-mode helper with a fail-safe Traditional fallback
// (R-BACK-31.1 / R-BACK-31.6).

#include "backend_interface.hpp"

#include <memory>

namespace dxmt9::render {

// Pure resolver: nullptr / "" / "0" / unknown → Traditional; "framegraph" →
// FrameGraph. Testable without touching the environment.
BackendMode resolveBackendMode(const char* renderModeEnv);

// Pure constructor by mode (testable without env). On any construction
// failure, logs a single warning and falls back to TraditionalBackend.
std::unique_ptr<IRenderBackend> createBackend(BackendMode mode);

// Reads DXMT9_RENDER_MODE once and constructs.
std::unique_ptr<IRenderBackend> createBackendFromEnv();

}  // namespace dxmt9::render
