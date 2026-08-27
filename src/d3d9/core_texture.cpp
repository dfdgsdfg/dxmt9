#include "core_format_utils.hpp"
#include "core_resources_internal.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9/dxmt9_perf_counters.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace dxmt9::core {

// Split from core_resources.cpp: Texture class members, Device texture
// factory/registration/update entry points, and the file-local
// convertTextureUpload helper used at the texture upload boundary.
// Trace/dump helpers shared with core_resources.cpp live in
// core_resources_internal.hpp.

std::vector<u8> convertTextureUpload(Format format, u32 width, u32 height,
                                     std::span<const u8> input) {
  const u32 bpp = bytesPerPixel(format);
  if (bpp == 0) {
    return {};
  }
  std::vector<u8> output(static_cast<size_t>(width) * height * bpp);
  const u32 srcPitch = pitchForFormat(format, width);
  if (input.size() < output.size()) {
    return {};
  }
  if (!copyPixels(output, srcPitch, width, height, format,
                  std::vector<u8>(input.begin(), input.end()), srcPitch, width,
                  height, format)) {
    // Fall back to a raw copy when the format is not color-decodable.
    std::copy_n(input.begin(), std::min(output.size(), input.size()),
                output.begin());
  }
  return output;
}

Texture::Texture(std::shared_ptr<Device> owner, TextureHandle handle,
                 TextureDesc desc)
    : owner_(std::move(owner)), handle_(handle), desc_(desc) {
  if (auto ownerPtr = owner_.lock()) {
    backend_ = ownerPtr->upperDevice();
  }
  const u32 mipLevels = levelCount();
  const u32 faceCount = desc_.type == TextureType::Cube ? 6u : 1u;
  levels_.resize(static_cast<size_t>(mipLevels) * faceCount);
  for (u32 subresource = 0; subresource < levels_.size(); ++subresource) {
    const u32 level = mipLevelForSubresource(subresource);
    LevelStorage storage;
    storage.width = std::max(1u, desc_.width >> level);
    storage.height = std::max(1u, desc_.height >> level);
    storage.depth = desc_.type == TextureType::Volume
                        ? std::max(1u, desc_.depth >> level)
                        : 1u;
    storage.pitch = formatRowPitch(desc_.format, storage.width);
    storage.slicePitch =
        formatByteSize(desc_.format, storage.width, storage.height);
    storage.bytes.resize(static_cast<size_t>(storage.slicePitch) *
                             static_cast<size_t>(storage.depth),
                         0);
    levels_[subresource] = std::move(storage);
  }
}

Texture::~Texture() { invalidate(); }

u32 Texture::levelCount() const noexcept { return std::max(1u, desc_.levels); }

u32 Texture::setLod(u32 lod) {
  const u32 previous = lod_;
  const u32 levels = levelCount();
  lod_ = std::min(lod, levels > 0 ? levels - 1u : 0u);
  if (lod_ != previous) {
    if (auto owner = owner_.lock()) {
      owner->notifyTextureLodChanged(*this);
    }
  }
  return previous;
}

u32 Texture::mipLevelForSubresource(u32 subresource) const noexcept {
  const u32 mipLevels = levelCount();
  if (desc_.type == TextureType::Cube && mipLevels != 0) {
    return subresource % mipLevels;
  }
  return subresource;
}

LockedRegion Texture::lockRect(u32 subresource, const Rect *rect, u32 flags) {
  if (!valid_ || subresource >= levels_.size()) {
    return {};
  }
  LevelStorage &storage = levels_[subresource];
  if ((flags & UsageDiscard) != 0) {
    // state-churn-encode-append-decomposition.24/.26: candidate (a) for
    // dxmt9c_surface_lock_rect's 1.33 ms/call cost. D3DLOCK_DISCARD does not
    // require zeroed contents, so this fill is removable work if it turns
    // out to dominate — timed here rather than removed in this
    // instrumentation-only pass.
    const size_t fillBytes = static_cast<size_t>(storage.slicePitch) *
                             static_cast<size_t>(storage.depth);
    const auto fillStart = std::chrono::steady_clock::now();
    storage.bytes.assign(fillBytes, 0);
    const auto fillNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - fillStart).count());
    dxmt9::perf::countTextureLockDiscardFillCpuTime(fillNs,
                                                     static_cast<std::uint64_t>(fillBytes));
  }
  locked_ = true;
  if (storage.pitch == 0 || storage.bytes.empty()) {
    return {};
  }
  const u32 left = rect ? std::max(0, rect->left) : 0;
  const u32 top = rect ? std::max(0, rect->top) : 0;
  if (isCompressedFormat(desc_.format)) {
    const u32 blockWidth = formatBlockWidth(desc_.format);
    const u32 blockHeight = formatBlockHeight(desc_.format);
    const u32 blockBytes = formatBlockBytes(desc_.format);
    const u32 blockX = std::min(left, storage.width - 1u) / blockWidth;
    const u32 blockY = std::min(top, storage.height - 1u) / blockHeight;
    return {storage.bytes.data() + static_cast<size_t>(blockY) * storage.pitch +
                static_cast<size_t>(blockX) * blockBytes,
            storage.pitch};
  }
  const u32 bpp = bytesPerPixel(desc_.format);
  return {storage.bytes.data() + static_cast<size_t>(top) * storage.pitch +
              static_cast<size_t>(left) * bpp,
          storage.pitch};
}

void Texture::unlockRect(u32 subresource) {
  if (subresource < levels_.size()) {
    levels_[subresource].dirty = true;
    syncLevelToBackend(subresource);
  }
  locked_ = false;
}

std::shared_ptr<Surface> Texture::surfaceLevel(u32 subresource) {
  if (subresource >= levels_.size()) {
    return {};
  }
  if (subresource < surfaces_.size()) {
    if (auto surface = surfaces_[subresource].lock()) {
      return surface;
    }
  } else {
    surfaces_.resize(subresource + 1);
  }
  auto owner = owner_.lock();
  if (!owner) {
    return {};
  }
  const u32 level = mipLevelForSubresource(subresource);
  SurfaceDesc surfaceDesc;
  surfaceDesc.width = std::max(1u, desc_.width >> level);
  surfaceDesc.height = std::max(1u, desc_.height >> level);
  surfaceDesc.format = desc_.format;
  surfaceDesc.pool = desc_.pool;
  surfaceDesc.usage = desc_.usage;
  surfaceDesc.renderTarget = (desc_.usage & UsageRenderTarget) != 0;
  surfaceDesc.depthStencil = (desc_.usage & UsageDepthStencil) != 0;
  auto surfaceHandle =
      backend_
          ? backend_->createSurfaceForTexture(handle_, subresource, surfaceDesc)
          : SurfaceHandle{};
  if (!surfaceHandle && backend_) {
    surfaceHandle = backend_->createSurface(surfaceDesc);
  }
  if (!surfaceHandle) {
    surfaceHandle = Handle{owner->nextHandle_++};
  }
  auto surface = std::make_shared<Surface>(owner, surfaceHandle,
                                           shared_from_this(), subresource);
  surfaces_[subresource] = surface;
  return surface;
}

std::span<const u8> Texture::levelBytes(u32 subresource) const {
  if (subresource >= levels_.size()) {
    return {};
  }
  const auto &storage = levels_[subresource];
  return std::span<const u8>(storage.bytes.data(), storage.bytes.size());
}

void Texture::fillColor(const Rect *rect, ColorRGBA color) {
  if (!valid_ || levels_.empty()) {
    return;
  }
  auto &storage = levels_[0];
  fillBuffer(storage.bytes, storage.pitch, storage.width, storage.height,
             desc_.format, rect, color);
  storage.dirty = true;
  syncLevelToBackend(0);
}

void Texture::fillColor(u32 subresource, const Rect *rect, ColorRGBA color) {
  if (!valid_ || subresource >= levels_.size()) {
    return;
  }
  auto &storage = levels_[subresource];
  fillBuffer(storage.bytes, storage.pitch, storage.width, storage.height,
             desc_.format, rect, color);
  storage.dirty = true;
  syncLevelToBackend(subresource);
}

void Texture::copyFrom(const Texture &src) {
  if (!valid_ || !src.valid_ || desc_.format != src.desc_.format) {
    return;
  }
  const size_t levels = std::min(levels_.size(), src.levels_.size());
  for (size_t i = 0; i < levels; ++i) {
    levels_[i].bytes = src.levels_[i].bytes;
    levels_[i].dirty = true;
    syncLevelToBackend(static_cast<u32>(i));
  }
}

HResult Texture::generateMipSubLevels() {
  if (!valid_) {
    return D3DERR_INVALIDCALL;
  }

  // AUTOGEN has a deliberately split topology: D3D9 exposes only level 0,
  // while the Metal TextureRecord owns the complete hidden pyramid. Generate
  // that physical chain before consulting the public level count, and do not
  // synthesize inaccessible CPU-shadow levels.
  if ((desc_.usage & UsageAutoGenMipmap) != 0u) {
    if (backend_ && handle_) {
      return backend_->generateTextureMipSublevels(handle_);
    }
    return D3D_OK;
  }

  const u32 mipLevels = levelCount();
  if (mipLevels <= 1) {
    return D3D_OK;
  }

  const u32 bpp = bytesPerPixel(desc_.format);
  if (bpp == 0 || isCompressedFormat(desc_.format)) {
    return D3D_OK;
  }

  HResult backendResult = D3D_OK;
  if (backend_ && handle_) {
    backendResult = backend_->generateTextureMipSublevels(handle_);
  }
  if (backendResult != D3D_OK) {
    return backendResult;
  }

  const u32 faceCount = desc_.type == TextureType::Cube ? 6u : 1u;
  for (u32 face = 0; face < faceCount; ++face) {
    const u32 baseSubresource = face * mipLevels;
    for (u32 level = 1; level < mipLevels; ++level) {
      auto& src = levels_[baseSubresource + level - 1u];
      auto& dst = levels_[baseSubresource + level];
      if (src.bytes.empty() || dst.bytes.empty() || src.width == 0 ||
          src.height == 0 || dst.width == 0 || dst.height == 0) {
        continue;
      }
      for (u32 y = 0; y < dst.height; ++y) {
        for (u32 x = 0; x < dst.width; ++x) {
          const u32 srcX0 = std::min(src.width - 1u, x * 2u);
          const u32 srcY0 = std::min(src.height - 1u, y * 2u);
          for (u32 component = 0; component < bpp; ++component) {
            u32 sum = 0;
            u32 count = 0;
            for (u32 dy = 0; dy < 2; ++dy) {
              for (u32 dx = 0; dx < 2; ++dx) {
                const u32 srcX = std::min(src.width - 1u, srcX0 + dx);
                const u32 srcY = std::min(src.height - 1u, srcY0 + dy);
                sum += src.bytes[static_cast<size_t>(srcY) * src.pitch +
                                 static_cast<size_t>(srcX) * bpp + component];
                ++count;
              }
            }
            dst.bytes[static_cast<size_t>(y) * dst.pitch +
                      static_cast<size_t>(x) * bpp + component] =
                static_cast<u8>((sum + count / 2u) / count);
          }
        }
      }
      dst.dirty = true;
    }
  }
  return D3D_OK;
}

void Texture::syncLevelToBackend(u32 subresource) {
  if (!valid_ || !backend_ || !handle_ || subresource >= levels_.size()) {
    return;
  }
  const auto &storage = levels_[subresource];
  if (storage.bytes.empty() || storage.width == 0 || storage.height == 0 ||
      storage.pitch == 0) {
    return;
  }
  if (const auto wanted = detail::textureDumpHandle();
      wanted && *wanted == handle_.value) {
    const auto dumpDir = std::filesystem::path(detail::textureDumpDir());
    const auto path = (dumpDir /
                       ("dxmt9_tex_" + std::to_string(handle_.value) +
                        "_subresource_" + std::to_string(subresource) + ".bmp"))
                          .string();
    if (writeBmpScreenshot(
            path, desc_.format, storage.width, storage.height, storage.pitch,
            std::span<const u8>(storage.bytes.data(), storage.bytes.size()))) {
      detail::emitRenderTrace(
          "texture dump handle=0x%x subresource=%u path=%s "
          "format=%u size=%ux%u pitch=%u",
          handle_.value, subresource, path.c_str(),
          static_cast<unsigned>(desc_.format), storage.width, storage.height,
          storage.pitch);
    } else {
      detail::emitRenderTrace(
          "texture dump handle=0x%x subresource=%u failed "
          "format=%u size=%ux%u pitch=%u",
          handle_.value, subresource, static_cast<unsigned>(desc_.format),
          storage.width, storage.height, storage.pitch);
    }
    const auto rawPath =
        dumpDir /
        ("dxmt9_tex_" + std::to_string(handle_.value) +
         "_subresource_" + std::to_string(subresource) + "_" +
         std::to_string(storage.width) + "x" +
         std::to_string(storage.height) + "x" +
         std::to_string(storage.depth) + "_pitch" +
         std::to_string(storage.pitch) + "_slice" +
         std::to_string(storage.slicePitch) + ".bin");
    std::error_code ec;
    std::filesystem::create_directories(rawPath.parent_path(), ec);
    std::ofstream raw(rawPath, std::ios::binary | std::ios::trunc);
    raw.write(reinterpret_cast<const char*>(storage.bytes.data()),
              static_cast<std::streamsize>(storage.bytes.size()));
    if (raw.good()) {
      detail::emitRenderTrace(
          "texture raw dump handle=0x%llx subresource=%u path=%s "
          "format=%u size=%ux%ux%u pitch=%u slicePitch=%u bytes=%zu",
          static_cast<unsigned long long>(handle_.value), subresource,
          rawPath.string().c_str(),
          static_cast<unsigned>(desc_.format), storage.width, storage.height,
          storage.depth, storage.pitch, storage.slicePitch,
          storage.bytes.size());
    }
  }
  backend_->uploadTextureLevel(
      handle_, subresource, storage.width, storage.height, storage.depth,
      storage.pitch, storage.slicePitch,
      std::span<const u8>(storage.bytes.data(), storage.bytes.size()));
}

void Texture::invalidate() {
  if (!valid_) {
    return;
  }
  valid_ = false;
  if (backend_ && handle_) {
    backend_->destroyTexture(handle_);
  }
  handle_ = {};
}

std::shared_ptr<Texture> Device::createTexture(const TextureDesc &desc) {
  auto handle =
      upperDevice_ ? upperDevice_->createTexture(desc) : TextureHandle{};
  if (!handle) {
    handle = Handle{nextHandle_++};
  }
  auto texture = std::make_shared<Texture>(shared_from_this(), handle, desc);
  registerTexture(texture);
  return texture;
}

std::shared_ptr<Texture> Device::openSharedTexture(
    const std::shared_ptr<Texture>& source) {
  if (!source || !source->valid()) {
    return {};
  }
  if (source->device().get() == this) {
    return source;
  }
  if (!upperDevice_ || !source->backend_) {
    return {};
  }
  dxmt9::SharedTextureBacking backing;
  if (!source->backend_->exportSharedTexture(source->handle(), backing)) {
    return {};
  }
  const auto handle = upperDevice_->importSharedTexture(source->desc(), backing);
  if (!handle) {
    return {};
  }
  auto texture = std::make_shared<Texture>(shared_from_this(), handle, source->desc());
  registerTexture(texture);
  return texture;
}

void Device::registerTexture(const std::shared_ptr<Texture> &texture) {
  textures_.push_back(texture);
}

HResult Device::updateTexture(const std::shared_ptr<Texture> &src,
                              const std::shared_ptr<Texture> &dst) {
  if (!src || !dst || !src->valid() || !dst->valid()) {
    return D3DERR_INVALIDCALL;
  }
  if (src->desc().format != dst->desc().format) {
    return D3DERR_NOTAVAILABLE;
  }
  const u32 subresources =
      std::min(src->subresourceCount(), dst->subresourceCount());
  for (u32 subresource = 0; subresource < subresources; ++subresource) {
    auto srcSurface = src->surfaceLevel(subresource);
    auto dstSurface = dst->surfaceLevel(subresource);
    if (!srcSurface || !dstSurface) {
      return D3DERR_INVALIDCALL;
    }
    if (backend_) {
      SurfaceCopyDesc backendDesc;
      backendDesc.source = srcSurface->handle();
      backendDesc.destination = dstSurface->handle();
      backendDesc.sourceLevel = 0;
      backendDesc.destinationLevel = 0;
      backendDesc.sourceRect = {0, 0,
                                static_cast<i32>(srcSurface->desc().width),
                                static_cast<i32>(srcSurface->desc().height)};
      backendDesc.destinationRect = {
          0, 0, static_cast<i32>(dstSurface->desc().width),
          static_cast<i32>(dstSurface->desc().height)};
      upperDevice_->submitSurfaceCopy(backendDesc);
    }
  }
  dst->copyFrom(*src);
  if ((dst->desc().usage & UsageAutoGenMipmap) != 0) {
    return dst->generateMipSubLevels();
  }
  return D3D_OK;
}

} // namespace dxmt9::core
