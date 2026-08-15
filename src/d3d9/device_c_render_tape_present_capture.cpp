#include "device_c_provider.hpp"

#include "device_c_common.hpp"
#include "device_c_render_tape_capture.hpp"
#include "device_c_replay_offload.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9/dxmt9_presenter.hpp"

#include <algorithm>
#include <array>
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

void retainSnapshotSurface(std::uint32_t kind, void* object) noexcept {
  if (kind == D9C_CHUNK_HANDLE_KIND_SURFACE && object) {
    dxmt9c_surface_addref(static_cast<D9CSurface*>(object));
  } else if (kind == D9C_CHUNK_HANDLE_KIND_TEXTURE && object) {
    dxmt9c_texture_addref(static_cast<D9CTexture*>(object));
  }
}

bool sameSurfaceDesc(const D9CSurfaceDesc& a,
                     const D9CSurfaceDesc& b) noexcept {
  return a.format == b.format && a.usage == b.usage && a.pool == b.pool &&
         a.multiSampleType == b.multiSampleType &&
         a.multiSampleQuality == b.multiSampleQuality &&
         a.width == b.width && a.height == b.height && a.depth == b.depth &&
         a.resourceType == b.resourceType;
}

bool expectedD24X8Bytes(const D9CSurfaceDesc& desc,
                        std::uint32_t& pitch,
                        std::uint64_t& bytes) noexcept {
  if (desc.format != 77u || desc.usage != 2u || desc.pool != 0u ||
      desc.multiSampleType != 0u || desc.multiSampleQuality != 0u ||
      desc.width == 0u || desc.height == 0u || desc.depth != 1u ||
      desc.resourceType != 1u ||
      desc.width > std::numeric_limits<std::uint32_t>::max() / 4u) {
    return false;
  }
  pitch = desc.width * 4u;
  if (desc.height > std::numeric_limits<std::uint64_t>::max() / pitch) {
    return false;
  }
  bytes = static_cast<std::uint64_t>(pitch) * desc.height;
  return bytes != 0u;
}

bool expectedColorSnapshotBytes(const D9CSurfaceDesc& desc,
                                std::uint32_t& pitch,
                                std::uint64_t& bytes) noexcept {
  const bool texture2d = desc.resourceType == 3u && desc.format == 22u;
  const bool cube = desc.resourceType == 5u && desc.format == 114u;
  const bool standalone =
      dxmt9::d3d9::renderTapeArmColorSnapshotStandaloneSurfaceSupported(desc);
  if ((!standalone && !texture2d && !cube) || desc.usage != 1u || desc.pool != 0u ||
      desc.multiSampleType != 0u || desc.multiSampleQuality != 0u ||
      desc.width == 0u || desc.height == 0u || desc.depth != 1u ||
      desc.width > std::numeric_limits<std::uint32_t>::max() / 4u) {
    return false;
  }
  pitch = desc.width * 4u;
  if (desc.height > std::numeric_limits<std::uint64_t>::max() / pitch) {
    return false;
  }
  bytes = static_cast<std::uint64_t>(pitch) * desc.height;
  return bytes != 0u;
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

extern "C" int32_t dxmt9c_device_capture_render_tape_d24x8_snapshot(
    D9CDevice* device, const D9CRenderTapeD24X8SnapshotRequest* request,
    D9CRenderTapeD24X8SnapshotResult* out, void* bytes,
    std::uint64_t capacity) {
  if (!out) return dxmt9::core::D3DERR_INVALIDCALL;
  *out = {};
  out->status = D9C_RENDER_TAPE_D24X8_SNAPSHOT_UNSUPPORTED;
  if (!device || !request || !bytes || request->reserved0 != 0u ||
      request->encodingVersion !=
          D9C_RENDER_TAPE_D24X8_ENCODING_FLOAT32_LE_V1 ||
      request->identity.kind != D9C_CHUNK_HANDLE_KIND_SURFACE ||
      request->identity.generation == 0u || request->identity.objectId == 0u) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  std::uint32_t expectedPitch = 0u;
  std::uint64_t expectedBytes = 0u;
  if (!expectedD24X8Bytes(request->surface, expectedPitch, expectedBytes)) {
    return dxmt9::core::D3DERR_NOTAVAILABLE;
  }
  if (capacity != expectedBytes ||
      capacity > std::numeric_limits<std::size_t>::max()) {
    out->status = D9C_RENDER_TAPE_D24X8_SNAPSHOT_CAPACITY_MISMATCH;
    return dxmt9::core::D3DERR_INVALIDCALL;
  }

  const std::array<D9CCommandChunkWireHandleEntry, 1u> entries{{
      dxmt9::d3d9::wireHandleEntry(request->identity)}};
  std::array<void*, 1u> resolved{};
  if (!device->wireObjects.resolveAndRetain(entries, resolved,
                                            retainSnapshotSurface)) {
    out->status = D9C_RENDER_TAPE_D24X8_SNAPSHOT_STALE_GENERATION;
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  auto* surface = static_cast<D9CSurface*>(resolved[0]);
  bool released = false;
  const auto release = [&] {
    if (surface && !released) {
      dxmt9c_surface_release(surface);
      released = true;
    }
  };
  if (!surface || surface->device != device || !surface->obj ||
      surface->ownerTex) {
    release();
    out->status = D9C_RENDER_TAPE_D24X8_SNAPSHOT_DESCRIPTOR_MISMATCH;
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  D9CSurfaceDesc actual{};
  if (dxmt9c_surface_get_desc(surface, &actual) != dxmt9::core::D3D_OK ||
      !sameSurfaceDesc(actual, request->surface)) {
    release();
    out->status = D9C_RENDER_TAPE_D24X8_SNAPSHOT_DESCRIPTOR_MISMATCH;
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  try {
    if (!dxmt9::d3d9::drainDeferredReplay(
            device, "render-tape-d24x8-snapshot")) {
      release();
      out->status = D9C_RENDER_TAPE_D24X8_SNAPSHOT_READBACK_FAILED;
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    auto upper = device->dev().upperDevice();
    if (!upper) {
      release();
      out->status = D9C_RENDER_TAPE_D24X8_SNAPSHOT_READBACK_FAILED;
      return dxmt9::core::D3DERR_NOTAVAILABLE;
    }
    // This is a capture-only hard boundary: commit and wait only work already
    // submitted before the snapshot. No current command chunk has been bridged.
    upper->flush();
    dxmt9::core::CanonicalD24X8Depth captured;
    const bool capturedOk = upper->captureCanonicalD24X8Depth(
        surface->obj->handle(), captured);
    release();
    if (!capturedOk ||
        captured.version != dxmt9::core::kCanonicalD24X8DepthVersion1 ||
        captured.width != actual.width || captured.height != actual.height ||
        captured.pitch != expectedPitch ||
        captured.bytes.size() != expectedBytes) {
      out->status = D9C_RENDER_TAPE_D24X8_SNAPSHOT_READBACK_FAILED;
      return dxmt9::core::D3DERR_NOTAVAILABLE;
    }
    std::memcpy(bytes, captured.bytes.data(), captured.bytes.size());
    out->status = D9C_RENDER_TAPE_D24X8_SNAPSHOT_COMPLETE;
    out->encodingVersion =
        D9C_RENDER_TAPE_D24X8_ENCODING_FLOAT32_LE_V1;
    out->width = captured.width;
    out->height = captured.height;
    out->pitch = captured.pitch;
    out->physicalFormat = captured.physicalFormat;
    out->byteCount = captured.bytes.size();
    return dxmt9::core::D3D_OK;
  } catch (...) {
    release();
    out->status = D9C_RENDER_TAPE_D24X8_SNAPSHOT_READBACK_FAILED;
    return dxmt9::core::D3DERR_NOTAVAILABLE;
  }
}

extern "C" int32_t dxmt9c_device_capture_render_tape_color_snapshot(
    D9CDevice* device, const D9CRenderTapeColorSnapshotRequest* request,
    D9CRenderTapeColorSnapshotResult* out, void* bytes,
    std::uint64_t capacity) {
  if (!out) return dxmt9::core::D3DERR_INVALIDCALL;
  *out = {};
  out->status = D9C_RENDER_TAPE_COLOR_SNAPSHOT_UNSUPPORTED;
  const bool textureIdentity = request &&
      request->identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE;
  const bool surfaceIdentity = request &&
      request->identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE;
  if (!device || !request || !bytes ||
      request->encodingVersion != D9C_RENDER_TAPE_COLOR_ENCODING_TIGHT_V1 ||
      (!textureIdentity && !surfaceIdentity) ||
      request->identity.generation == 0u || request->identity.objectId == 0u) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  std::uint32_t expectedPitch = 0u;
  std::uint64_t expectedBytes = 0u;
  if (!expectedColorSnapshotBytes(request->surface, expectedPitch,
                                  expectedBytes)) {
    return dxmt9::core::D3DERR_NOTAVAILABLE;
  }
  const std::uint32_t expectedSubresources =
      request->surface.resourceType == 5u ? 6u : 1u;
  if (request->subresource >= expectedSubresources ||
      capacity != expectedBytes ||
      capacity > std::numeric_limits<std::size_t>::max()) {
    out->status = D9C_RENDER_TAPE_COLOR_SNAPSHOT_CAPACITY_MISMATCH;
    return dxmt9::core::D3DERR_INVALIDCALL;
  }

  const std::array<D9CCommandChunkWireHandleEntry, 1u> entries{{
      dxmt9::d3d9::wireHandleEntry(request->identity)}};
  std::array<void*, 1u> resolved{};
  if (!device->wireObjects.resolveAndRetain(entries, resolved,
                                            retainSnapshotSurface)) {
    out->status = D9C_RENDER_TAPE_COLOR_SNAPSHOT_STALE_GENERATION;
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  D9CTexture* texture = nullptr;
  D9CSurface* surface = nullptr;
  const auto releaseResolved = [&] {
    if (surface) {
      dxmt9c_surface_release(surface);
      surface = nullptr;
    }
    if (texture) {
      dxmt9c_texture_release(texture);
      texture = nullptr;
    }
  };
  const auto expectedCoreFormat =
      dxmt9::d3d9::devicec::fmtFromD3D(request->surface.format);
  const D9CSurfaceDesc actual = request->surface;
  if (textureIdentity) {
    texture = static_cast<D9CTexture*>(resolved[0]);
    if (!texture || texture->device != device || !texture->obj) {
      releaseResolved();
      out->status = D9C_RENDER_TAPE_COLOR_SNAPSHOT_DESCRIPTOR_MISMATCH;
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    const auto& textureDesc = texture->obj->desc();
    const bool cube = request->surface.resourceType == 5u;
    if (request->surface.resourceType != (cube ? 5u : 3u) ||
        textureDesc.type != (cube ? dxmt9::core::TextureType::Cube
                                 : dxmt9::core::TextureType::TwoD) ||
        textureDesc.levels != 1u ||
        textureDesc.width != request->surface.width ||
        textureDesc.height != request->surface.height ||
        textureDesc.depth != 1u || textureDesc.format != expectedCoreFormat ||
        textureDesc.pool != dxmt9::core::Pool::Default ||
        textureDesc.usage != dxmt9::core::UsageRenderTarget ||
        texture->d3dFormat != request->surface.format) {
      releaseResolved();
      out->status = D9C_RENDER_TAPE_COLOR_SNAPSHOT_DESCRIPTOR_MISMATCH;
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    surface = dxmt9c_texture_get_surface_level(texture, request->subresource);
    if (!surface || surface->device != device || surface->ownerTex != texture ||
        !surface->obj) {
      releaseResolved();
      out->status = D9C_RENDER_TAPE_COLOR_SNAPSHOT_DESCRIPTOR_MISMATCH;
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
  } else {
    surface = static_cast<D9CSurface*>(resolved[0]);
    if (!surface || surface->device != device || surface->ownerTex ||
        !surface->obj || request->subresource != 0u ||
        !dxmt9::d3d9::renderTapeArmColorSnapshotStandaloneSurfaceSupported(
            request->surface)) {
      releaseResolved();
      out->status = D9C_RENDER_TAPE_COLOR_SNAPSHOT_DESCRIPTOR_MISMATCH;
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    const auto& surfaceDesc = surface->obj->desc();
    if (surfaceDesc.width != request->surface.width ||
        surfaceDesc.height != request->surface.height ||
        surfaceDesc.format != expectedCoreFormat ||
        surfaceDesc.pool != dxmt9::core::Pool::Default ||
        !surfaceDesc.renderTarget || surfaceDesc.depthStencil ||
        surfaceDesc.multiSampleType != dxmt9::core::MultiSampleType::None) {
      releaseResolved();
      out->status = D9C_RENDER_TAPE_COLOR_SNAPSHOT_DESCRIPTOR_MISMATCH;
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
  }
  if (!surface || !surface->obj) {
    releaseResolved();
    out->status = D9C_RENDER_TAPE_COLOR_SNAPSHOT_DESCRIPTOR_MISMATCH;
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  try {
    if (!dxmt9::d3d9::drainDeferredReplay(
            device, "render-tape-color-snapshot")) {
      releaseResolved();
      out->status = D9C_RENDER_TAPE_COLOR_SNAPSHOT_READBACK_FAILED;
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    auto upper = device->dev().upperDevice();
    if (!upper) {
      releaseResolved();
      out->status = D9C_RENDER_TAPE_COLOR_SNAPSHOT_READBACK_FAILED;
      return dxmt9::core::D3DERR_NOTAVAILABLE;
    }
    upper->flush();
    dxmt9::core::ReadbackPixels pixels;
    std::vector<std::byte> tight;
    const auto coreFormat = dxmt9::d3d9::devicec::fmtFromD3D(actual.format);
    const bool captured = coreFormat != dxmt9::core::Format::Unknown &&
        upper->readbackSurface(
            dxmt9::core::ReadbackDesc{.source = surface->obj->handle()},
            pixels) &&
        copyTightPixels(pixels, actual.width, actual.height,
                        static_cast<std::uint32_t>(coreFormat), tight);
    releaseResolved();
    if (!captured || tight.size() != expectedBytes) {
      out->status = D9C_RENDER_TAPE_COLOR_SNAPSHOT_READBACK_FAILED;
      return dxmt9::core::D3DERR_NOTAVAILABLE;
    }
    if (actual.format == 22u) {
      // D3DFMT_X8R8G8B8's high byte is logically unused and Metal is free to
      // discard it. Canonical version 1 normalizes it to one so replayed
      // sampling and byte digests do not depend on physical X-channel bits.
      for (std::size_t offset = 3u; offset < tight.size(); offset += 4u) {
        tight[offset] = std::byte{0xffu};
      }
    }
    std::memcpy(bytes, tight.data(), tight.size());
    out->status = D9C_RENDER_TAPE_COLOR_SNAPSHOT_COMPLETE;
    out->encodingVersion = D9C_RENDER_TAPE_COLOR_ENCODING_TIGHT_V1;
    out->subresource = request->subresource;
    out->width = actual.width;
    out->height = actual.height;
    out->pitch = expectedPitch;
    out->format = actual.format;
    out->byteCount = tight.size();
    return dxmt9::core::D3D_OK;
  } catch (...) {
    releaseResolved();
    out->status = D9C_RENDER_TAPE_COLOR_SNAPSHOT_READBACK_FAILED;
    return dxmt9::core::D3DERR_NOTAVAILABLE;
  }
}
