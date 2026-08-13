#pragma once

#include "device_c_render_tape_capture.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

// These are opt-in PE-local test/publication seams. Production bootstrap state
// comes from the device-owned PE shadow; a non-null producer explicitly
// overrides that owner for native/PE tests. Neither callback is part of
// device_c.h or the generated PE/unix bridge ABI.
using D3D9PeRenderTapeBootstrapProducer = bool (*)(
    dxmt9::d3d9::RenderTapeCaptureBootstrapSeed& seed);
using D3D9PeRenderTapeArtifactPublisher = bool (*)(
    const dxmt9::d3d9::RenderTapePublicationBundle& bundle);

inline bool dxmt9PeRenderTapeCaptureCallbacksInstalled(
    bool captureEnabled, D3D9PeRenderTapeBootstrapProducer producer,
    D3D9PeRenderTapeArtifactPublisher publisher) noexcept {
  (void)producer;
  return captureEnabled && publisher != nullptr;
}

void dxmt9PeSetRenderTapeBootstrapProducer(
    D3D9PeRenderTapeBootstrapProducer producer) noexcept;
void dxmt9PeSetRenderTapeArtifactPublisher(
    D3D9PeRenderTapeArtifactPublisher publisher) noexcept;
