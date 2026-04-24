#pragma once

// Metal shader-source helpers shared across pipeline builders. Lifted out of
// backend_metal.mm's anonymous namespace so PipelineCache + Presenter + any
// other consumer can call them without pulling in the full backend TU.

#include "../winemetal/Metal.hpp"

#include <cstdint>
#include <string>

namespace dxmt9::core {
struct ColorRGBA;  // forward — defined in core.hpp
}

namespace dxmt9::shaders {

using u64 = std::uint64_t;

// FNV-1a hash of a shader source string, used for variant keying.
u64 makeHash(const std::string& source);

// Generic fullscreen-triangle vertex shader (no attributes). Used by
// clear/fill pipelines that render a solid color across the target.
std::string makeGenericVertexSource(u64 variantHash);

// Generic solid-color fragment shader. Color is baked into the source.
std::string makeGenericFragmentSource(const core::ColorRGBA& color, u64 variantHash);

// Textured fullscreen-triangle vertex shader — generates UVs for a present
// or stretch blit.
std::string makeTexturedVertexSource(u64 variantHash);

// Textured sampling fragment shader. forceOpaqueAlpha=true clamps alpha to
// 1.0 (used for X8R8G8B8 / X8B8G8R8 present paths).
std::string makeTexturedFragmentSource(u64 variantHash, bool forceOpaqueAlpha = false);

// Compile a source string into a WMT::Library (Metal Shading Language).
// Returns an empty reference on failure.
WMT::Reference<WMT::Library> makeLibrary(WMT::Device& device, const std::string& source);

// Open the on-disk shader binary archive; archiveOut is assigned the result
// (possibly empty on failure).
void initShaderArchive(WMT::Device& device, const std::string& path,
                       WMT::Reference<WMT::BinaryArchive>& archiveOut);

// Value-returning overload for in-place member initialization.
WMT::Reference<WMT::BinaryArchive> initShaderArchive(WMT::Device device, const std::string& path);

// Serialize the archive to disk. No-op if archive is empty or path is empty.
void persistShaderArchive(WMT::BinaryArchive& archive, const std::string& path);

// Shared MSL prelude used by the draw / FFP shader generators: defines
// DrawUniforms, VSOut, and a library of helper inline functions
// (dxmt9_load_*, dxmt9_apply_texture_*, dxmt9_select_*, etc). When
// withClipDistances is true, VSOut gets a clipDistance[6] array.
std::string makeShaderPrelude(bool withClipDistances);

}  // namespace dxmt9::shaders
