#pragma once

// Internal helpers shared between core_resources.cpp and the per-class
// resource translation units (core_buffer.cpp, core_texture.cpp,
// core_surface.cpp). These helpers were file-local to core_resources.cpp
// before the per-class split; exposing them here keeps the split a verbatim
// move while preserving a single definition per helper.

#include "dxmt9/core.hpp"

#include <memory>
#include <optional>
#include <string>

namespace dxmt9 {
class Device;
} // namespace dxmt9

namespace dxmt9::core::detail {

bool backendOwnsSurfaceContents(const SurfaceDesc &desc);
bool backendOwnsTextureContents(const TextureDesc &desc);
bool canTrustGpuReadback(const std::shared_ptr<dxmt9::Device> &backend);

bool renderTraceEnabled();
std::optional<u32> textureDumpHandle();
std::string textureDumpDir();
void emitRenderTrace(const char *fmt, ...);

} // namespace dxmt9::core::detail
