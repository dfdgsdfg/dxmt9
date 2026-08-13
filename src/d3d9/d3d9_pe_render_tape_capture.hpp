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
    const dxmt9::d3d9::RenderTapePublicationBundle& bundle);

inline bool dxmt9PeRenderTapeCaptureCallbacksInstalled(
    bool captureEnabled, D3D9PeRenderTapeBootstrapProducer producer,
    D3D9PeRenderTapeArtifactPublisher publisher) noexcept {
  return captureEnabled && producer != nullptr && publisher != nullptr;
}

void dxmt9PeSetRenderTapeBootstrapProducer(
    D3D9PeRenderTapeBootstrapProducer producer) noexcept;
void dxmt9PeSetRenderTapeArtifactPublisher(
    D3D9PeRenderTapeArtifactPublisher publisher) noexcept;
