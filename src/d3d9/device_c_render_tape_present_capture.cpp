#include "device_c_provider.hpp"

#include "device_c_common.hpp"
#include "device_c_render_tape_capture.hpp"
#include "device_c_replay_offload.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9/dxmt9_presenter.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace {

void cancelPresentCapture(D9CDevice* device) noexcept {
  if (!device || !device->renderTapePresentCapture) {
    return;
  }
  auto& lease = *device->renderTapePresentCapture;
  try {
    if (auto swap = device->dev().swapChain(0u)) {
      if (auto* presenter = swap->presenter()) {
        presenter->cancelPresentMirror(lease.ticket);
      }
    }
  } catch (...) {
    // Cancellation is best-effort at the Presenter boundary, but the lease
    // itself must always be dropped so its mirror texture cannot outlive the
    // owning device during reset or teardown.
  }
  device->renderTapePresentCapture.reset();
}

bool copyTightPixels(const dxmt9::core::ReadbackPixels& pixels,
                    std::uint32_t width, std::uint32_t height,
                    std::uint32_t format, std::vector<std::byte>& out) {
  const auto bytesPerPixel = dxmt9::core::bytesPerPixel(
      static_cast<dxmt9::core::Format>(format));
  if (width == 0u || height == 0u || bytesPerPixel == 0u ||
      width > std::numeric_limits<std::uint32_t>::max() / bytesPerPixel) {
    return false;
  }
  const auto tightPitch = width * bytesPerPixel;
  if (pixels.pitch < tightPitch ||
      pixels.bytes.size() < static_cast<std::size_t>(pixels.pitch) * height) {
    return false;
  }
  try {
    out.resize(static_cast<std::size_t>(tightPitch) * height);
  } catch (...) {
    return false;
  }
  for (std::uint32_t row = 0u; row < height; ++row) {
    std::memcpy(out.data() + static_cast<std::size_t>(row) * tightPitch,
                pixels.bytes.data() + static_cast<std::size_t>(row) * pixels.pitch,
                tightPitch);
  }
  return true;
}

} // namespace

extern "C" int32_t dxmt9c_device_reserve_render_tape_present_capture(
    D9CDevice* device) {
  if (!device || device->renderTapePresentCapture) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  try {
    // The reservation is one-shot at the Presenter boundary.  A prior Present
    // may still be queued by commit-replay offload or by the Metal command
    // queue when the PE side reserves the mirror for the current captured
    // Present.  Drain both stages before publishing the ticket so an older
    // Present cannot consume it and bind the previous frame's pixels to the
    // current Render Tape interval.
    if (!dxmt9::d3d9::drainDeferredReplay(
            device, "render-tape-present-capture-reserve")) {
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    auto swap = device->dev().swapChain(0u);
    auto upper = device->dev().upperDevice();
    if (!swap || !upper || !upper->pool() || !swap->presenter()) {
      return dxmt9::core::D3DERR_NOTAVAILABLE;
    }
    upper->flush();
    const auto source = swap->backBuffer();
    if (!source || !source->valid()) {
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    auto mirrorDesc = source->desc();
    mirrorDesc.multiSampleType = dxmt9::core::MultiSampleType::None;
    mirrorDesc.renderTarget = true;
    mirrorDesc.depthStencil = false;
    mirrorDesc.usage |= dxmt9::core::UsageRenderTarget;
    auto mirror = device->dev().createSurface(mirrorDesc);
    auto* record = mirror ? upper->pool()->findSurface(mirror->handle().value)
                          : nullptr;
    if (!record || !record->texture) {
      return dxmt9::core::D3DERR_NOTAVAILABLE;
    }
    auto ticket = std::make_shared<dxmt9::PresentMirrorTicket>();
    const dxmt9::PresentOutputTarget target{
        .texture = WMT::Texture{record->texture.handle},
        .width = mirrorDesc.width,
        .height = mirrorDesc.height,
    };
    if (!swap->presenter()->reservePresentMirror(target, ticket)) {
      return dxmt9::core::D3DERR_NOTAVAILABLE;
    }
    try {
      device->renderTapePresentCapture.emplace(
          D9CRenderTapePresentCaptureLease{
              .mirror = std::move(mirror),
              .ticket = ticket,
              .width = mirrorDesc.width,
              .height = mirrorDesc.height,
              .coreFormat = static_cast<std::uint32_t>(mirrorDesc.format),
              .d3dFormat = dxmt9::d3d9::devicec::fmtToD3D(mirrorDesc.format),
          });
    } catch (...) {
      swap->presenter()->cancelPresentMirror(ticket);
      return dxmt9::core::D3DERR_NOTAVAILABLE;
    }
    return dxmt9::core::D3D_OK;
  } catch (...) {
    return dxmt9::core::D3DERR_NOTAVAILABLE;
  }
}

extern "C" int32_t dxmt9c_device_finish_render_tape_present_capture(
    D9CDevice* device, D9CRenderTapePresentCaptureResult* out) {
  if (!out) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  *out = {};
  out->status = D9C_RENDER_TAPE_PRESENT_CAPTURE_FAILED;
  if (!device || !device->renderTapePresentCapture) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  try {
    if (!dxmt9::d3d9::drainDeferredReplay(
            device, "render-tape-present-capture-finish") ||
        !device->renderTapePresentCapture->ticket) {
      cancelPresentCapture(device);
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    auto upper = device->dev().upperDevice();
    if (!upper) {
      cancelPresentCapture(device);
      return dxmt9::core::D3DERR_NOTAVAILABLE;
    }
    // Replay-drained only means that the current Present reached the Metal
    // command queue.  Flush that queue before testing the one-shot ticket so
    // the current Present, rather than a later call, must consume it.
    upper->flush();
    if (!device->renderTapePresentCapture->ticket->encoded()) {
      cancelPresentCapture(device);
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    auto lease = std::move(*device->renderTapePresentCapture);
    device->renderTapePresentCapture.reset();
    dxmt9::core::ReadbackPixels pixels;
    std::vector<std::byte> tight;
    if (!lease.mirror || !lease.mirror->valid()) {
      return dxmt9::core::D3DERR_NOTAVAILABLE;
    }
    if (!upper->readbackSurface(
            dxmt9::core::ReadbackDesc{.source = lease.mirror->handle()},
            pixels) ||
        !copyTightPixels(pixels, lease.width, lease.height, lease.coreFormat,
                         tight)) {
      return dxmt9::core::D3DERR_NOTAVAILABLE;
    }
    const auto digest = dxmt9::d3d9::RenderTapeCaptureSession::sha256(tight);
    out->status = D9C_RENDER_TAPE_PRESENT_CAPTURE_COMPLETE;
    out->width = lease.width;
    out->height = lease.height;
    out->format = lease.d3dFormat;
    out->byteCount = tight.size();
    std::memcpy(out->sha256, digest.data(), digest.size());
    return dxmt9::core::D3D_OK;
  } catch (...) {
    cancelPresentCapture(device);
    return dxmt9::core::D3DERR_NOTAVAILABLE;
  }
}

extern "C" void dxmt9c_device_cancel_render_tape_present_capture(
    D9CDevice* device) {
  cancelPresentCapture(device);
}
