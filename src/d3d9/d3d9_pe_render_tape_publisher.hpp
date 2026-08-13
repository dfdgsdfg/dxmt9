#pragma once

#include "d3d9_pe_render_tape_capture.hpp"

#include <string_view>

// The production callback is installed by the PE device when the explicit
// output-root policy is configured. The value-only helper is also used by the
// native filesystem truth table; it never retains references after returning.
bool dxmt9PePublishRenderTapeBundle(
    const dxmt9::d3d9::RenderTapePublicationBundle& bundle,
    std::string_view outputRoot, std::string_view frameName) noexcept;

bool dxmt9PeRenderTapeOutputConfigured() noexcept;

D3D9PeRenderTapeArtifactPublisher
dxmt9PeDefaultRenderTapeArtifactPublisher() noexcept;
