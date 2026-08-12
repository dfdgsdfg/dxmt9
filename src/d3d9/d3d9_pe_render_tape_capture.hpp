#pragma once

#include "device_c_render_tape_capture.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

// This is an opt-in PE-local injection seam. It is intentionally not part of
// device_c.h or the generated PE/unix bridge ABI.
using D3D9PeRenderTapeBootstrapProducer = bool (*)(
    dxmt9::d3d9::RenderTapeCaptureBootstrapSeed& seed);
using D3D9PeRenderTapeArtifactPublisher = bool (*)(
    std::span<const std::byte> sealedArtifact);

void dxmt9PeSetRenderTapeBootstrapProducer(
    D3D9PeRenderTapeBootstrapProducer producer) noexcept;
void dxmt9PeSetRenderTapeArtifactPublisher(
    D3D9PeRenderTapeArtifactPublisher publisher) noexcept;
