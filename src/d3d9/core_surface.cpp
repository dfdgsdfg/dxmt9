#include "core_format_utils.hpp"
#include "core_resources_internal.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9/dxmt9_device.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace dxmt9::core {

// Split from core_resources.cpp: Surface class members and the Device
// surface-oriented entry points (createSurface/registerSurface, fillSurface,
// stretchRect, updateSurface, getRenderTargetData). Helpers shared with
// core_resources.cpp live in core_resources_internal.hpp.

Surface::Surface(std::shared_ptr<Device> owner, SurfaceHandle handle,
                 SurfaceDesc desc)
    : owner_(std::move(owner)), handle_(handle), desc_(desc),
      containerKind_(ContainerKind::Device) {
  if (auto ownerPtr = owner_.lock()) {
    backend_ = ownerPtr->upperDevice();
  }
  // R-FORMAT-12: a D3DFMT_NULL render target is colorless and allocates no
  // GPU color backing (the depth-only render pass omits the color
  // attachment). It IS still lockable, though: Wine
  // (test_surface_format_null) returns a dummy CPU buffer from LockRect with
  // a valid pBits/Pitch (contents meaningless, discarded on Unlock). NullRt
  // carries a 4-byte (BGRA8 placeholder) bpp in the format table, so the
  // standalone scratch below sizes correctly. GetRenderTargetData still keys
  // off isNullRenderTarget() to reject readback.
  if (desc_.width != 0 && desc_.height != 0) {
    standalonePitch_ = formatRowPitch(desc_.format, desc_.width);
    standaloneBytes_.resize(
        formatByteSize(desc_.format, desc_.width, desc_.height), 0);
  }
}

Surface::Surface(std::shared_ptr<Device> owner, SurfaceHandle handle,
                 std::shared_ptr<Texture> texture, u32 level)
    : owner_(std::move(owner)), textureContainer_(std::move(texture)),
      handle_(handle), level_(level), containerKind_(ContainerKind::Texture) {
  if (auto ownerPtr = owner_.lock()) {
    backend_ = ownerPtr->upperDevice();
  }
  if (auto tex = textureContainer_.lock()) {
    const u32 mipLevel = tex->mipLevelForSubresource(level_);
    desc_.width = std::max(1u, tex->desc().width >> mipLevel);
    desc_.height = std::max(1u, tex->desc().height >> mipLevel);
    desc_.format = tex->desc().format;
    desc_.pool = tex->desc().pool;
    desc_.usage = tex->desc().usage;
    desc_.renderTarget = (tex->desc().usage & UsageRenderTarget) != 0;
    desc_.depthStencil = (tex->desc().usage & UsageDepthStencil) != 0;
  }
}

Surface::~Surface() { invalidate(); }

LockedRegion Surface::lockRect(const Rect *rect, u32 flags) {
  if (!valid_) {
    return {};
  }
  // R-FORMAT-12: a NULL render target is lockable — it returns the dummy
  // standalone scratch allocated in the ctor (contents meaningless). Wine
  // test_surface_format_null expects LockRect -> D3D_OK with a valid
  // pBits/Pitch, so NULL falls through to the standalone path below;
  // GetRenderTargetData still rejects readback via isNullRenderTarget().
  if (containerKind_ == ContainerKind::Texture) {
    if (auto tex = textureContainer_.lock()) {
      return tex->lockRect(level_, rect, flags);
    }
    return {};
  }
  if ((flags & UsageDiscard) != 0) {
    standaloneBytes_.assign(
        formatByteSize(desc_.format, desc_.width, desc_.height), 0);
  }
  locked_ = true;
  if (standalonePitch_ == 0 || standaloneBytes_.empty()) {
    return {};
  }
  const u32 left = rect ? std::max(0, rect->left) : 0;
  const u32 top = rect ? std::max(0, rect->top) : 0;
  if (isCompressedFormat(desc_.format)) {
    const u32 blockWidth = formatBlockWidth(desc_.format);
    const u32 blockHeight = formatBlockHeight(desc_.format);
    const u32 blockBytes = formatBlockBytes(desc_.format);
    const u32 blockX = std::min(left, desc_.width - 1u) / blockWidth;
    const u32 blockY = std::min(top, desc_.height - 1u) / blockHeight;
    return {standaloneBytes_.data() +
                static_cast<size_t>(blockY) * standalonePitch_ +
                static_cast<size_t>(blockX) * blockBytes,
            standalonePitch_};
  }
  const u32 bpp = bytesPerPixel(desc_.format);
  return {standaloneBytes_.data() +
              static_cast<size_t>(top) * standalonePitch_ +
              static_cast<size_t>(left) * bpp,
          standalonePitch_};
}

void Surface::unlockRect() {
  if (containerKind_ == ContainerKind::Texture) {
    if (auto tex = textureContainer_.lock()) {
      tex->unlockRect(level_);
    }
  }
  locked_ = false;
}

void Surface::fillColor(const Rect *rect, ColorRGBA color) {
  if (!valid_) {
    return;
  }
  if (containerKind_ == ContainerKind::Texture) {
    if (auto tex = textureContainer_.lock()) {
      tex->fillColor(level_, rect, color);
    }
    return;
  }
  fillBuffer(standaloneBytes_, standalonePitch_, desc_.width, desc_.height,
             desc_.format, rect, color);
}

void Surface::copyFrom(const Surface &src) {
  if (!valid_ || !src.valid_ || desc_.format != src.desc_.format) {
    return;
  }
  if (containerKind_ == ContainerKind::Texture) {
    if (auto tex = textureContainer_.lock()) {
      if (src.containerKind_ == ContainerKind::Texture) {
        if (auto srcTex = src.textureContainer_.lock()) {
          tex->copyFrom(*srcTex);
        }
      }
    }
    return;
  }
  if (src.containerKind_ == ContainerKind::Texture) {
    if (auto srcTex = src.textureContainer_.lock()) {
      if (!srcTex->levelBytes(src.level_).empty()) {
        const auto bytes = srcTex->levelBytes(src.level_);
        const size_t count = std::min(bytes.size(), standaloneBytes_.size());
        std::copy_n(bytes.begin(), count, standaloneBytes_.begin());
      }
    }
    return;
  }
  const size_t count =
      std::min(standaloneBytes_.size(), src.standaloneBytes_.size());
  std::copy_n(src.standaloneBytes_.begin(), count, standaloneBytes_.begin());
}

void Surface::invalidate() {
  valid_ = false;
  if (backend_ && handle_) {
    backend_->destroySurface(handle_);
  }
  handle_ = {};
}

std::shared_ptr<Surface> Device::createSurface(const SurfaceDesc &desc) {
  auto handle =
      upperDevice_ ? upperDevice_->createSurface(desc) : SurfaceHandle{};
  if (!handle) {
    handle = Handle{nextHandle_++};
  }
  auto surface = std::make_shared<Surface>(shared_from_this(), handle, desc);
  registerSurface(surface);
  return surface;
}

std::shared_ptr<Surface> Device::openSharedSurface(
    const std::shared_ptr<Surface>& source) {
  if (!source || !source->valid()) {
    return {};
  }
  if (source->deviceContainer().get() == this) {
    return source;
  }
  if (!upperDevice_ || !source->backend_ ||
      source->containerKind() != Surface::ContainerKind::Device) {
    return {};
  }
  dxmt9::SharedSurfaceBacking backing;
  if (!source->backend_->exportSharedSurface(source->handle(), backing)) {
    return {};
  }
  const auto handle = upperDevice_->importSharedSurface(source->desc(), backing);
  if (!handle) {
    return {};
  }
  auto surface = std::make_shared<Surface>(shared_from_this(), handle, source->desc());
  registerSurface(surface);
  return surface;
}

void Device::registerSurface(const std::shared_ptr<Surface> &surface) {
  surfaces_.push_back(surface);
}

HResult Device::fillSurface(const std::shared_ptr<Surface> &surface,
                            const Rect *rect, ColorRGBA color) {
  if (!surface || !surface->valid()) {
    return D3DERR_INVALIDCALL;
  }
  if (backend_) {
    ColorFillDesc backendDesc;
    backendDesc.destination = surface->handle();
    if (rect) {
      backendDesc.rect = *rect;
      backendDesc.hasRect = true;
    }
    backendDesc.color = color;
    upperDevice_->submitColorFill(backendDesc);
  }
  surface->fillColor(rect, color);
  return D3D_OK;
}

HResult Device::stretchRect(const std::shared_ptr<Surface> &src,
                            const Rect *srcRect,
                            const std::shared_ptr<Surface> &dst,
                            const Rect *dstRect, bool linear) {
  if (!src || !dst || !src->valid() || !dst->valid()) {
    return D3DERR_INVALIDCALL;
  }
  if (src->desc().format != dst->desc().format) {
    return D3DERR_NOTAVAILABLE;
  }
  Rect srcArea = srcRect ? *srcRect
                         : Rect{0, 0, static_cast<i32>(src->desc().width),
                                static_cast<i32>(src->desc().height)};
  Rect dstArea = dstRect ? *dstRect
                         : Rect{0, 0, static_cast<i32>(dst->desc().width),
                                static_cast<i32>(dst->desc().height)};
  const i32 srcWidth = std::max(0, srcArea.right - srcArea.left);
  const i32 srcHeight = std::max(0, srcArea.bottom - srcArea.top);
  const i32 dstWidth = std::max(0, dstArea.right - dstArea.left);
  const i32 dstHeight = std::max(0, dstArea.bottom - dstArea.top);
  if (srcWidth == 0 || srcHeight == 0 || dstWidth == 0 || dstHeight == 0) {
    return D3DERR_INVALIDCALL;
  }

  if (backend_) {
    StretchRectDesc backendDesc;
    backendDesc.source = src->handle();
    backendDesc.destination = dst->handle();
    backendDesc.sourceRect = srcArea;
    backendDesc.destinationRect = dstArea;
    backendDesc.linear = linear;
    upperDevice_->submitStretchRect(backendDesc);
    if (detail::canTrustGpuReadback(backend_) &&
        (detail::backendOwnsSurfaceContents(src->desc()) ||
         detail::backendOwnsSurfaceContents(dst->desc()))) {
      return D3D_OK;
    }
  }

  auto extractRegion = [&](const std::shared_ptr<Surface> &surface,
                           const Rect &area) -> std::vector<u8> {
    const u32 bpp = bytesPerPixel(surface->desc().format);
    const u32 width = static_cast<u32>(std::max(0, area.right - area.left));
    const u32 height = static_cast<u32>(std::max(0, area.bottom - area.top));
    auto region = surface->lockRect(&area, 0);
    if (!region.data || bpp == 0) {
      if (region.data) {
        surface->unlockRect();
      }
      return {};
    }
    std::vector<u8> out(static_cast<size_t>(width) * height * bpp);
    const auto *srcBytes = static_cast<const u8 *>(region.data);
    for (u32 y = 0; y < height; ++y) {
      std::memcpy(out.data() + static_cast<size_t>(y) * width * bpp,
                  srcBytes + static_cast<size_t>(y) * region.pitch,
                  static_cast<size_t>(width) * bpp);
    }
    surface->unlockRect();
    return out;
  };

  auto blitRegion = [&](const std::shared_ptr<Surface> &surface,
                        const Rect &area, std::span<const u8> bytes,
                        u32 srcWidthPixels, u32 srcHeightPixels) -> HResult {
    const u32 bpp = bytesPerPixel(surface->desc().format);
    if (bpp == 0) {
      return D3DERR_INVALIDCALL;
    }
    auto region = surface->lockRect(&area, 0);
    if (!region.data) {
      return D3DERR_INVALIDCALL;
    }
    const u32 dstW = static_cast<u32>(std::max(0, area.right - area.left));
    const u32 dstH = static_cast<u32>(std::max(0, area.bottom - area.top));
    std::vector<u8> temp;
    if (srcWidthPixels == dstW && srcHeightPixels == dstH) {
      temp.assign(bytes.begin(), bytes.end());
    } else {
      temp.resize(static_cast<size_t>(dstW) * dstH * bpp);
      std::vector<u8> srcCopy(bytes.begin(), bytes.end());
      if (!stretchPixels(temp, dstW * bpp, dstW, dstH, surface->desc().format,
                         srcCopy, srcWidthPixels * bpp, srcWidthPixels,
                         srcHeightPixels, surface->desc().format)) {
        surface->unlockRect();
        return D3DERR_INVALIDCALL;
      }
    }
    const auto *srcBytes = temp.data();
    for (u32 y = 0; y < dstH; ++y) {
      std::memcpy(static_cast<u8 *>(region.data) +
                      static_cast<size_t>(y) * region.pitch,
                  srcBytes + static_cast<size_t>(y) * dstW * bpp,
                  static_cast<size_t>(dstW) * bpp);
    }
    surface->unlockRect();
    return D3D_OK;
  };

  const auto srcBytes = extractRegion(src, srcArea);
  if (srcBytes.empty()) {
    return D3DERR_INVALIDCALL;
  }

  const HResult result = blitRegion(
      dst, dstArea, std::span<const u8>(srcBytes.data(), srcBytes.size()),
      static_cast<u32>(srcWidth), static_cast<u32>(srcHeight));
  if (result != D3D_OK) {
    return result;
  }
  return D3D_OK;
}

HResult Device::updateSurface(const std::shared_ptr<Surface> &src,
                              const std::shared_ptr<Surface> &dst) {
  if (!src || !dst || !src->valid() || !dst->valid()) {
    return D3DERR_INVALIDCALL;
  }
  if (src->desc().format != dst->desc().format) {
    return D3DERR_NOTAVAILABLE;
  }

  if (backend_) {
    SurfaceCopyDesc backendDesc;
    backendDesc.source = src->handle();
    backendDesc.destination = dst->handle();
    backendDesc.sourceRect = {0, 0, static_cast<i32>(src->desc().width),
                              static_cast<i32>(src->desc().height)};
    backendDesc.destinationRect = {0, 0, static_cast<i32>(dst->desc().width),
                                   static_cast<i32>(dst->desc().height)};
    upperDevice_->submitSurfaceCopy(backendDesc);
  }

  auto srcRegion = src->lockRect(nullptr, 0);
  auto dstRegion = dst->lockRect(nullptr, 0);
  if (!srcRegion.data || !dstRegion.data) {
    if (srcRegion.data) {
      src->unlockRect();
    }
    if (dstRegion.data) {
      dst->unlockRect();
    }
    return D3DERR_INVALIDCALL;
  }

  const u32 width = std::min(src->desc().width, dst->desc().width);
  const u32 height = std::min(src->desc().height, dst->desc().height);
  const u32 rowBytes = formatRowPitch(src->desc().format, width);
  const u32 rows = formatRowCount(src->desc().format, height);
  if (rowBytes == 0 || rows == 0 || srcRegion.pitch < rowBytes ||
      dstRegion.pitch < rowBytes) {
    src->unlockRect();
    dst->unlockRect();
    return D3DERR_INVALIDCALL;
  }
  for (u32 y = 0; y < rows; ++y) {
    std::memcpy(static_cast<u8 *>(dstRegion.data) +
                    static_cast<size_t>(y) * dstRegion.pitch,
                static_cast<const u8 *>(srcRegion.data) +
                    static_cast<size_t>(y) * srcRegion.pitch,
                rowBytes);
  }
  src->unlockRect();
  dst->unlockRect();
  return D3D_OK;
}

HResult Device::getRenderTargetData(const std::shared_ptr<Surface> &src,
                                    const std::shared_ptr<Surface> &dst) {
  if (!src || !dst || !src->valid() || !dst->valid()) {
    return D3DERR_INVALIDCALL;
  }
  // R-FORMAT-12: a NULL render target has no color storage to read back.
  // Readback from (or into) a NULL surface is an invalid call.
  if (src->isNullRenderTarget() || dst->isNullRenderTarget()) {
    return D3DERR_INVALIDCALL;
  }
  if (backend_) {
    ReadbackDesc backendDesc;
    backendDesc.source = src->handle();
    backendDesc.destination = dst->handle();
    backendDesc.sourceRect = {0, 0, static_cast<i32>(src->desc().width),
                              static_cast<i32>(src->desc().height)};
    upperDevice_->submitReadback(backendDesc);
    upperDevice_->flush();
    ReadbackPixels pixels;
    if (backend_->readbackSurface(backendDesc, pixels)) {
      auto dstRegion = dst->lockRect(nullptr, 0);
      if (!dstRegion.data) {
        return D3DERR_INVALIDCALL;
      }
      const u32 bpp = bytesPerPixel(src->desc().format);
      if (bpp == 0) {
        dst->unlockRect();
        return D3DERR_NOTAVAILABLE;
      }
      const u32 width = std::min(src->desc().width, dst->desc().width);
      const u32 height = std::min(src->desc().height, dst->desc().height);
      const size_t rowBytes = static_cast<size_t>(width) * bpp;
      if (pixels.pitch < rowBytes ||
          pixels.bytes.size() < static_cast<size_t>(pixels.pitch) * height) {
        dst->unlockRect();
        return D3DERR_INVALIDCALL;
      }
      for (u32 y = 0; y < height; ++y) {
        std::memcpy(static_cast<u8 *>(dstRegion.data) +
                        static_cast<size_t>(y) * dstRegion.pitch,
                    pixels.bytes.data() + static_cast<size_t>(y) * pixels.pitch,
                    rowBytes);
      }
      dst->unlockRect();
      return D3D_OK;
    }
  }
  return updateSurface(src, dst);
}

HResult Device::reszDepthResolve(const std::shared_ptr<Surface> &msaaDepth,
                                 const std::shared_ptr<Texture> &intzDest) {
  // R-FORMAT-11 — RESZ is fire-and-forget on real hardware: a missing or
  // invalid binding is a benign no-op rather than an error (mirrors the PE
  // emit's no-op guard in requestReszDepthResolve).
  if (!msaaDepth || !intzDest || !msaaDepth->valid()) {
    return D3D_OK;
  }
  // The INTZ destination is the stage-0 texture; resolve writes into its
  // level-0 surface, whose handle resolves through the same surface table the
  // encoder reads (encodeDepthResolve calls findSurface on both endpoints).
  auto intzSurface = intzDest->surfaceLevel(0);
  if (!intzSurface || !intzSurface->valid()) {
    return D3D_OK;
  }
  if (backend_) {
    DepthResolveDesc backendDesc;
    backendDesc.msaaDepth = msaaDepth->handle();
    backendDesc.intzDest = intzSurface->handle();
    upperDevice_->submitDepthResolve(backendDesc);
  }
  return D3D_OK;
}

} // namespace dxmt9::core
