#include "device_c_provider.hpp"

#include <algorithm>

using namespace dxmt9::d3d9::devicec;

namespace {

struct LockFootprint {
  uint32_t rowBytes = 0;
  uint32_t rows = 0;
};

constexpr uint32_t kD3DLockDiscard = 0x00002000u;
constexpr uint32_t kD3DLockNoOverwrite = 0x00001000u;

uint32_t lockFlagsToCore(uint32_t flags) {
  uint32_t out = 0;
  if ((flags & kD3DLockDiscard) != 0) {
    out |= dxmt9::core::UsageDiscard;
  }
  if ((flags & kD3DLockNoOverwrite) != 0) {
    out |= dxmt9::core::UsageNoOverwrite;
  }
  return out;
}

LockFootprint lockFootprint(dxmt9::core::Format format,
                            uint32_t width,
                            uint32_t height,
                            const D9CRect* rect) {
  const uint32_t blockWidth = dxmt9::core::formatBlockWidth(format);
  const uint32_t blockHeight = dxmt9::core::formatBlockHeight(format);
  const uint32_t blockBytes = dxmt9::core::formatBlockBytes(format);
  if (width == 0 || height == 0 || blockBytes == 0) {
    return {};
  }

  const int32_t left = rect ? std::clamp(rect->left, 0, static_cast<int32_t>(width)) : 0;
  const int32_t top = rect ? std::clamp(rect->top, 0, static_cast<int32_t>(height)) : 0;
  const int32_t right =
      rect ? std::clamp(rect->right, left, static_cast<int32_t>(width)) : static_cast<int32_t>(width);
  const int32_t bottom =
      rect ? std::clamp(rect->bottom, top, static_cast<int32_t>(height)) : static_cast<int32_t>(height);
  if (right == left || bottom == top) {
    return {};
  }

  const uint32_t blockLeft = static_cast<uint32_t>(left) / blockWidth;
  const uint32_t blockTop = static_cast<uint32_t>(top) / blockHeight;
  const uint32_t blockRight =
      (static_cast<uint32_t>(right) + blockWidth - 1u) / blockWidth;
  const uint32_t blockBottom =
      (static_cast<uint32_t>(bottom) + blockHeight - 1u) / blockHeight;
  return {(blockRight - blockLeft) * blockBytes, blockBottom - blockTop};
}

bool invalidCompressedTextureDimensions(dxmt9::core::Format format, uint32_t width, uint32_t height) {
  if (!dxmt9::core::isCompressedFormat(format)) {
    return false;
  }
  return width == 0 || height == 0 ||
         (width % dxmt9::core::formatBlockWidth(format)) != 0 ||
         (height % dxmt9::core::formatBlockHeight(format)) != 0;
}

void copyNativeToShadow(ShadowLock& shadow) {
  auto* dst = static_cast<uint8_t*>(shadow.shadow.ptr);
  auto* src = static_cast<const uint8_t*>(shadow.nativePtr);
  for (uint32_t row = 0; row < shadow.rows; ++row) {
    std::memcpy(dst + static_cast<size_t>(row) * shadow.nativePitch,
                src + static_cast<size_t>(row) * shadow.nativePitch,
                shadow.rowBytes);
  }
}

void copyShadowToNative(const ShadowLock& shadow) {
  auto* dst = static_cast<uint8_t*>(shadow.nativePtr);
  auto* src = static_cast<const uint8_t*>(shadow.shadow.ptr);
  for (uint32_t row = 0; row < shadow.rows; ++row) {
    std::memcpy(dst + static_cast<size_t>(row) * shadow.nativePitch,
                src + static_cast<size_t>(row) * shadow.nativePitch,
                shadow.rowBytes);
  }
}

}  // namespace

extern "C" D9CTexture* dxmt9c_device_create_texture(D9CDevice* d, uint32_t w, uint32_t h,
                                                    uint32_t levels, uint32_t usage,
                                                    uint32_t fmt, uint32_t pool) {
  dxmt9DebugLog("device_create_texture begin device=%p size=%ux%u levels=%u usage=0x%x fmt=%u(%s) pool=%u",
                static_cast<void*>(d), w, h, levels, usage, fmt,
                dxmt9::core::formatName(fmtFromD3D(fmt)).c_str(), pool);
  dxmt9::core::TextureDesc desc;
  desc.width = w;
  desc.height = h;
  desc.levels = levels ? levels : 1;
  desc.format = fmtFromD3D(fmt);
  desc.pool = poolFromD3D(pool);
  desc.usage = usageFromD3D(usage);
  desc.type = dxmt9::core::TextureType::TwoD;
  if (invalidCompressedTextureDimensions(desc.format, desc.width, desc.height)) {
    dxmt9DebugLog("device_create_texture rejected compressed alignment size=%ux%u fmt=%u(%s)",
                  w, h, fmt, dxmt9::core::formatName(desc.format).c_str());
    return nullptr;
  }
  auto tex = d->iface->CreateTexture(desc);
  if (!tex) {
    dxmt9DebugLog("device_create_texture failed device=%p", static_cast<void*>(d));
    return nullptr;
  }
  dxmt9DebugLog("device_create_texture ok texture=%p levels=%u",
                static_cast<void*>(tex.get()), tex->levelCount());
  return new D9CTexture{tex, d};
}

extern "C" D9CTexture* dxmt9c_device_create_cube_texture(D9CDevice* d, uint32_t size,
                                                         uint32_t levels, uint32_t usage,
                                                         uint32_t fmt, uint32_t pool) {
  dxmt9::core::TextureDesc desc;
  desc.width = size;
  desc.height = size;
  desc.levels = levels ? levels : 1;
  desc.format = fmtFromD3D(fmt);
  desc.pool = poolFromD3D(pool);
  desc.usage = usageFromD3D(usage);
  desc.type = dxmt9::core::TextureType::Cube;
  if (invalidCompressedTextureDimensions(desc.format, desc.width, desc.height)) {
    return nullptr;
  }
  auto tex = d->iface->CreateTexture(desc);
  if (!tex) {
    return nullptr;
  }
  return new D9CTexture{tex, d};
}

extern "C" D9CTexture* dxmt9c_device_create_volume_texture(D9CDevice* d, uint32_t w, uint32_t h,
                                                           uint32_t depth, uint32_t levels,
                                                           uint32_t usage, uint32_t fmt,
                                                           uint32_t pool) {
  dxmt9::core::TextureDesc desc;
  desc.width = w;
  desc.height = h;
  desc.depth = depth;
  desc.levels = levels ? levels : 1;
  desc.format = fmtFromD3D(fmt);
  desc.pool = poolFromD3D(pool);
  desc.usage = usageFromD3D(usage);
  desc.type = dxmt9::core::TextureType::Volume;
  if (invalidCompressedTextureDimensions(desc.format, desc.width, desc.height)) {
    return nullptr;
  }
  auto tex = d->iface->CreateTexture(desc);
  if (!tex) {
    return nullptr;
  }
  return new D9CTexture{tex, d};
}

extern "C" D9CBuffer* dxmt9c_device_create_vertex_buffer(D9CDevice* d, uint32_t len,
                                                         uint32_t usage, uint32_t fvf,
                                                         uint32_t pool) {
  dxmt9::core::BufferDesc desc{len, poolFromD3D(pool),
                               static_cast<uint32_t>(usageFromD3D(usage) |
                                                     dxmt9::core::UsageVertexBuffer)};
  auto buf = d->iface->CreateBuffer(desc);
  if (!buf) {
    return nullptr;
  }
  auto* out = new D9CBuffer{buf};
  out->desc.size = len;
  out->desc.usage = usage;
  out->desc.pool = pool;
  out->desc.fvf = fvf;
  out->desc.format = 0;
  return out;
}

extern "C" D9CBuffer* dxmt9c_device_create_index_buffer(D9CDevice* d, uint32_t len,
                                                        uint32_t usage, uint32_t fmt,
                                                        uint32_t pool) {
  dxmt9::core::BufferDesc desc{len, poolFromD3D(pool),
                               static_cast<uint32_t>(usageFromD3D(usage) |
                                                     dxmt9::core::UsageIndexBuffer)};
  auto buf = d->iface->CreateBuffer(desc);
  if (!buf) {
    return nullptr;
  }
  auto* out = new D9CBuffer{buf};
  out->desc.size = len;
  out->desc.usage = usage;
  out->desc.pool = pool;
  out->desc.fvf = 0;
  out->desc.format = fmt;
  return out;
}

extern "C" D9CSurface* dxmt9c_device_create_render_target(D9CDevice* d, uint32_t w, uint32_t h,
                                                          uint32_t fmt, uint32_t msType,
                                                          uint32_t, uint32_t, uint64_t*) {
  dxmt9::core::SurfaceDesc desc;
  desc.width = w;
  desc.height = h;
  desc.format = fmtFromD3D(fmt);
  desc.renderTarget = true;
  desc.multiSampleType = msTypeFromD3D(msType);
  auto surf = d->iface->CreateSurface(desc);
  if (!surf) {
    return nullptr;
  }
  return new D9CSurface{surf};
}

extern "C" D9CSurface* dxmt9c_device_create_depth_stencil(D9CDevice* d, uint32_t w, uint32_t h,
                                                          uint32_t fmt, uint32_t msType,
                                                          uint32_t, uint32_t, uint64_t*) {
  dxmt9::core::SurfaceDesc desc;
  desc.width = w;
  desc.height = h;
  desc.format = fmtFromD3D(fmt);
  desc.depthStencil = true;
  desc.multiSampleType = msTypeFromD3D(msType);
  auto surf = d->iface->CreateSurface(desc);
  if (!surf) {
    return nullptr;
  }
  return new D9CSurface{surf};
}

extern "C" D9CSurface* dxmt9c_device_create_offscreen_surface(D9CDevice* d, uint32_t w,
                                                              uint32_t h, uint32_t fmt,
                                                              uint32_t pool, uint64_t*) {
  dxmt9::core::SurfaceDesc desc;
  desc.width = w;
  desc.height = h;
  desc.format = fmtFromD3D(fmt);
  desc.pool = poolFromD3D(pool);
  auto surf = d->iface->CreateSurface(desc);
  if (!surf) {
    return nullptr;
  }
  return new D9CSurface{surf};
}

extern "C" void dxmt9c_texture_addref(D9CTexture* t) {
  if (t) {
    t->refs.fetch_add(1);
  }
}

extern "C" uint32_t dxmt9c_texture_release(D9CTexture* t) {
  if (!t) {
    return 0;
  }
  const uint32_t refs = t->refs.fetch_sub(1) - 1;
  if (refs == 0) {
    for (auto& [_, lock] : t->wow64Locks) {
      releaseShadowLock(lock);
    }
    delete t;
  }
  return refs;
}

extern "C" int32_t dxmt9c_texture_lock_rect(D9CTexture* t, uint32_t level, D9CLockedRect* out,
                                            const D9CRect* r, uint32_t flags) {
  if (!out) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  if (t->lockedLevels.contains(level)) {
    dxmt9DebugLog("texture_lock_rect rejected double-lock texture=%p level=%u",
                  static_cast<void*>(t), level);
    out->pitch = 0;
    out->bits = nullptr;
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  if (r) {
    dxmt9DebugLog("texture_lock_rect begin texture=%p level=%u flags=0x%x rect=(%d,%d)-(%d,%d)",
                  static_cast<void*>(t), level, flags, r->left, r->top, r->right, r->bottom);
  } else {
    dxmt9DebugLog("texture_lock_rect begin texture=%p level=%u flags=0x%x rect=<full>",
                  static_cast<void*>(t), level, flags);
  }
  auto* rect = r ? new dxmt9::core::Rect{r->left, r->top, r->right, r->bottom} : nullptr;
  auto lock = t->obj->lockRect(level, rect, lockFlagsToCore(flags));
  delete rect;
  out->pitch = static_cast<int32_t>(lock.pitch);
  out->bits = lock.data;
  if (!lock.data || lock.pitch == 0) {
    dxmt9DebugLog("texture_lock_rect failed texture=%p level=%u pitch=%u bits=%p",
                  static_cast<void*>(t), level, lock.pitch, lock.data);
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  if (lock.data && requiresWow64PointerShadow() && !pointerFits32Bit(lock.data)) {
    const auto& desc = t->obj->desc();
    const uint32_t levelWidth = std::max(1u, desc.width >> std::min(level, 31u));
    const uint32_t levelHeight = std::max(1u, desc.height >> std::min(level, 31u));
    const auto footprint = lockFootprint(desc.format, levelWidth, levelHeight, r);
    const uint32_t rowBytes = footprint.rowBytes;
    const uint32_t rows = footprint.rows;
    // SFIV BC3 level-9 page-fault repro (2026-05-10): for tiny mips
    // (e.g. 1x1 BC3) Metal/the runtime reports `lock.pitch` equal to
    // the BASE-level row pitch (1024 for a 256x256 BC3 base), and the
    // game walks the lock pointer as if it had base-level rows worth
    // of storage — observed writes at offsets 0x1000, 0x2000, 0x4000
    // from the lock pointer. footprint.rows is the LEVEL block-row
    // count (1 for a 1x1 mip), so `lock.pitch * rows` heavily
    // underestimates the worst-case extent. Use the parent texture's
    // full block-padded height as the upper bound (and let
    // computeShadowBytesUpperBound enforce its own floor on top).
    // copyNativeToShadow / copyShadowToNative continue to copy
    // `rowBytes * rows` block-rows — that part is correct; only the
    // allocation upper bound widens.
    const uint32_t rectHeight =
        r ? static_cast<uint32_t>(std::max<int32_t>(
                0, std::clamp(r->bottom, 0, static_cast<int32_t>(levelHeight)) -
                       std::clamp(r->top, 0, static_cast<int32_t>(levelHeight))))
          : levelHeight;
    const uint32_t blockHeight = dxmt9::core::formatBlockHeight(desc.format);
    // When the lock's reported pitch exceeds the strictly-correct
    // row pitch for the level (`> rowBytes`), the game evidently
    // treats the lock as a base-level surface; size against the
    // base-level height. Otherwise the level's own height is fine.
    const uint32_t levelRowPitch =
        dxmt9::core::formatRowPitch(desc.format, levelWidth);
    const uint32_t effectiveHeight =
        (rowBytes != 0 && lock.pitch > levelRowPitch) ? desc.height : rectHeight;
    const size_t shadowBytes =
        computeShadowBytesUpperBound(lock.pitch, effectiveHeight, blockHeight);
    auto& shadow = t->wow64Locks[level];
    if (rowBytes == 0 || rows == 0) {
      dxmt9DebugLog("texture_lock_rect shadow alloc failed texture=%p level=%u nativeBits=%p rowBytes=%u rows=%u",
                    static_cast<void*>(t), level, lock.data, rowBytes, rows);
      out->pitch = 0;
      out->bits = nullptr;
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    if (!shadow.shadow || shadow.shadow.size < shadowBytes) {
      releaseShadowLock(shadow);
      shadow.shadow = allocateLow4GB(shadowBytes);
    }
    if (!shadow.shadow) {
      dxmt9DebugLog("texture_lock_rect shadow alloc failed texture=%p level=%u nativeBits=%p rowBytes=%u rows=%u",
                    static_cast<void*>(t), level, lock.data, rowBytes, rows);
      out->pitch = 0;
      out->bits = nullptr;
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    shadow.nativePtr = lock.data;
    shadow.nativePitch = lock.pitch;
    shadow.rowBytes = rowBytes;
    shadow.rows = rows;
    shadow.active = true;
    copyNativeToShadow(shadow);
    out->pitch = static_cast<int32_t>(shadow.nativePitch);
    out->bits = shadow.shadow.ptr;
    dxmt9DebugLog("texture_lock_rect shadow texture=%p level=%u nativeBits=%p shadowBits=%p pitch=%u rowBytes=%u rows=%u",
                  static_cast<void*>(t), level, shadow.nativePtr, out->bits,
                  shadow.nativePitch, rowBytes, rows);
  }
  t->lockedLevels.insert(level);
  dxmt9DebugLog("texture_lock_rect ok texture=%p level=%u pitch=%d bits=%p",
                static_cast<void*>(t), level, out->pitch, out->bits);
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_texture_unlock_rect(D9CTexture* t, uint32_t level) {
  if (!t->lockedLevels.erase(level)) {
    dxmt9DebugLog("texture_unlock_rect rejected not-locked texture=%p level=%u",
                  static_cast<void*>(t), level);
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  if (auto it = t->wow64Locks.find(level); it != t->wow64Locks.end()) {
    auto& shadow = it->second;
    if (shadow.active) {
      copyShadowToNative(shadow);
      shadow.active = false;
      dxmt9DebugLog("texture_unlock_rect shadow texture=%p level=%u nativeBits=%p shadowBits=%p rowBytes=%u rows=%u",
                    static_cast<void*>(t), level, shadow.nativePtr, shadow.shadow.ptr,
                    shadow.rowBytes, shadow.rows);
    }
  }
  dxmt9DebugLog("texture_unlock_rect texture=%p level=%u", static_cast<void*>(t), level);
  t->obj->unlockRect(level);
  return dxmt9::core::D3D_OK;
}

extern "C" D9CSurface* dxmt9c_texture_get_surface_level(D9CTexture* t, uint32_t level) {
  auto surf = t->obj->surfaceLevel(level);
  if (!surf) {
    return nullptr;
  }
  auto* wrap = new D9CSurface{surf, t, level};
  t->refs.fetch_add(1);
  return wrap;
}

extern "C" uint32_t dxmt9c_texture_get_level_count(D9CTexture* t) {
  return t->obj->levelCount();
}

extern "C" int32_t dxmt9c_texture_get_level_desc(D9CTexture* t, uint32_t level,
                                                 D9CSurfaceDesc* out) {
  if (!out) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  std::memset(out, 0, sizeof(*out));
  if (level >= t->obj->levelCount()) {
    dxmt9DebugLog("texture_get_level_desc rejected texture=%p level=%u levelCount=%u",
                  static_cast<void*>(t), level, t->obj->levelCount());
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  auto& desc = t->obj->desc();
  const uint32_t shift = std::min<uint32_t>(level, 31);
  out->format = fmtToD3D(desc.format);
  out->resourceType = textureTypeToResourceType(desc.type);
  out->usage = usageToD3D(desc.usage);
  out->pool = poolToD3D(desc.pool);
  out->multiSampleType = 0;
  out->multiSampleQuality = 0;
  out->width = std::max(1u, desc.width >> shift);
  out->height = std::max(1u, desc.height >> shift);
  dxmt9DebugLog("texture_get_level_desc texture=%p level=%u fmt=%u(%s) usage=0x%x pool=%u size=%ux%u",
                static_cast<void*>(t), level, out->format,
                dxmt9::core::formatName(desc.format).c_str(), out->usage, out->pool, out->width,
                out->height);
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_texture_generate_mip_sublevels(D9CTexture* t) {
  if (!t) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  return t->obj->generateMipSubLevels();
}

extern "C" uint32_t dxmt9c_texture_set_lod(D9CTexture* t, uint32_t lod) {
  if (!t || !t->obj) {
    return 0;
  }
  return t->obj->setLod(lod);
}

extern "C" void dxmt9c_buffer_addref(D9CBuffer* b) {
  if (b) {
    b->refs.fetch_add(1);
  }
}

extern "C" uint32_t dxmt9c_buffer_release(D9CBuffer* b) {
  if (!b) {
    return 0;
  }
  const uint32_t refs = b->refs.fetch_sub(1) - 1;
  if (refs == 0) {
    releaseShadowLock(b->wow64Lock);
    delete b;
  }
  return refs;
}

extern "C" int32_t dxmt9c_buffer_lock(D9CBuffer* b, uint32_t offset, uint32_t size, void** data,
                                      uint32_t flags) {
  if (!data) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  dxmt9DebugLog("buffer_lock begin buffer=%p offset=%u size=%u flags=0x%x",
                static_cast<void*>(b), offset, size, flags);
  const uint32_t actualSize = size ? size : b->obj->desc().size;
  auto lock = b->obj->lock(offset, actualSize, lockFlagsToCore(flags));
  *data = lock.data;
  if (lock.data && requiresWow64PointerShadow() && !pointerFits32Bit(lock.data)) {
    if (!b->wow64Lock.shadow || b->wow64Lock.shadow.size < actualSize) {
      releaseShadowLock(b->wow64Lock);
      b->wow64Lock.shadow = allocateLow4GB(actualSize);
    }
    if (!b->wow64Lock.shadow) {
      dxmt9DebugLog("buffer_lock shadow alloc failed buffer=%p native=%p size=%u",
                    static_cast<void*>(b), lock.data, actualSize);
      *data = nullptr;
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
    b->wow64Lock.nativePtr = lock.data;
    b->wow64Lock.rowBytes = actualSize;
    b->wow64Lock.rows = 1;
    std::memcpy(b->wow64Lock.shadow.ptr, lock.data, actualSize);
    *data = b->wow64Lock.shadow.ptr;
    dxmt9DebugLog("buffer_lock shadow buffer=%p native=%p shadow=%p size=%u",
                  static_cast<void*>(b), lock.data, *data, actualSize);
  }
  dxmt9DebugLog("buffer_lock ok buffer=%p data=%p", static_cast<void*>(b), *data);
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_buffer_unlock(D9CBuffer* b) {
  if (b->wow64Lock.shadow) {
    std::memcpy(b->wow64Lock.nativePtr, b->wow64Lock.shadow.ptr, b->wow64Lock.rowBytes);
    dxmt9DebugLog("buffer_unlock shadow buffer=%p native=%p shadow=%p size=%u",
                  static_cast<void*>(b), b->wow64Lock.nativePtr, b->wow64Lock.shadow.ptr,
                  b->wow64Lock.rowBytes);
    releaseShadowLock(b->wow64Lock);
  }
  b->obj->unlock();
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_buffer_get_desc(D9CBuffer* b, D9CBufferDesc* out) {
  if (!b || !out) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  *out = b->desc;
  return dxmt9::core::D3D_OK;
}

extern "C" void dxmt9c_surface_addref(D9CSurface* s) {
  if (s) {
    s->refs.fetch_add(1);
  }
}

extern "C" uint32_t dxmt9c_surface_release(D9CSurface* s) {
  if (!s) {
    return 0;
  }
  const uint32_t refs = s->refs.fetch_sub(1) - 1;
  if (refs == 0) {
    releaseShadowLock(s->wow64Lock);
    if (s->ownerTex) {
      dxmt9c_texture_release(s->ownerTex);
    }
    delete s;
  }
  return refs;
}

extern "C" int32_t dxmt9c_surface_lock_rect(D9CSurface* s, D9CLockedRect* out, const D9CRect* r,
                                            uint32_t flags) {
  if (!out) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  if (s->ownerTex) {
    if (s->ownerTex->lockedLevels.contains(s->ownerLevel)) {
      dxmt9DebugLog("surface_lock_rect rejected double-lock surface=%p ownerTexture=%p level=%u",
                    static_cast<void*>(s), static_cast<void*>(s->ownerTex), s->ownerLevel);
      out->pitch = 0;
      out->bits = nullptr;
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
  } else if (s->locked) {
    dxmt9DebugLog("surface_lock_rect rejected double-lock surface=%p",
                  static_cast<void*>(s));
    out->pitch = 0;
    out->bits = nullptr;
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  if (r) {
    dxmt9DebugLog("surface_lock_rect begin surface=%p flags=0x%x rect=(%d,%d)-(%d,%d)",
                  static_cast<void*>(s), flags, r->left, r->top, r->right, r->bottom);
  } else {
    dxmt9DebugLog("surface_lock_rect begin surface=%p flags=0x%x rect=<full>",
                  static_cast<void*>(s), flags);
  }
  auto* rect = r ? new dxmt9::core::Rect{r->left, r->top, r->right, r->bottom} : nullptr;
  auto lock = s->obj->lockRect(rect, lockFlagsToCore(flags));
  delete rect;
  out->pitch = static_cast<int32_t>(lock.pitch);
  out->bits = lock.data;
  if (!lock.data || lock.pitch == 0) {
    dxmt9DebugLog("surface_lock_rect failed surface=%p pitch=%u bits=%p",
                  static_cast<void*>(s), lock.pitch, lock.data);
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  if (lock.data && requiresWow64PointerShadow() && !pointerFits32Bit(lock.data)) {
    const auto& desc = s->obj->desc();
    const uint32_t nativePitch = static_cast<uint32_t>(std::abs(out->pitch));
    const uint32_t rectHeight = static_cast<uint32_t>(std::max<int32_t>(
        0, (r ? std::clamp(r->bottom, 0, static_cast<int32_t>(desc.height))
              : static_cast<int32_t>(desc.height)) -
               (r ? std::clamp(r->top, 0, static_cast<int32_t>(desc.height)) : 0)));
    uint32_t rowBytes = nativePitch;
    uint32_t rows = rectHeight;
    if (dxmt9::core::isCompressedFormat(desc.format)) {
      const auto footprint = lockFootprint(desc.format, desc.width, desc.height, r);
      rowBytes = footprint.rowBytes;
      rows = footprint.rows;
    }
    // See texture_lock_rect note: footprint.rows is the block-row count
    // and `nativePitch` is the native row stride; games may iterate by
    // texel row (or write a full base-level worth on tiny mips), so
    // size the shadow against rectHeight (or the base-level height
    // when the lock's pitch leaks the parent-level row stride).
    const uint32_t blockHeight = dxmt9::core::formatBlockHeight(desc.format);
    const uint32_t levelRowPitch =
        dxmt9::core::formatRowPitch(desc.format, desc.width);
    const uint32_t effectiveHeight =
        (rowBytes != 0 && nativePitch > levelRowPitch) ? desc.height : rectHeight;
    const size_t bytes =
        computeShadowBytesUpperBound(nativePitch, effectiveHeight, blockHeight);
    if (bytes != 0) {
      auto& shadow = s->ownerTex ? s->ownerTex->wow64Locks[s->ownerLevel] : s->wow64Lock;
      if (!shadow.shadow || shadow.shadow.size < bytes) {
        releaseShadowLock(shadow);
        shadow.shadow = allocateLow4GB(bytes);
      }
      if (!shadow.shadow) {
        out->bits = nullptr;
        return dxmt9::core::D3DERR_INVALIDCALL;
      }
      shadow.nativePtr = lock.data;
      shadow.nativePitch = nativePitch;
      shadow.rowBytes = rowBytes;
      shadow.rows = rows;
      shadow.active = true;
      copyNativeToShadow(shadow);
      out->bits = shadow.shadow.ptr;
      dxmt9DebugLog("surface_lock_rect shadow surface=%p native=%p shadow=%p pitch=%u rowBytes=%u rows=%u bytes=%zu",
                    static_cast<void*>(s), lock.data, out->bits, nativePitch,
                    rowBytes, rows, bytes);
    }
  }
  if (s->ownerTex) {
    s->ownerTex->lockedLevels.insert(s->ownerLevel);
  } else {
    s->locked = true;
  }
  dxmt9DebugLog("surface_lock_rect ok surface=%p pitch=%d bits=%p",
                static_cast<void*>(s), out->pitch, out->bits);
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_surface_unlock_rect(D9CSurface* s) {
  if (s->ownerTex) {
    if (!s->ownerTex->lockedLevels.erase(s->ownerLevel)) {
      dxmt9DebugLog("surface_unlock_rect rejected not-locked surface=%p ownerTexture=%p level=%u",
                    static_cast<void*>(s), static_cast<void*>(s->ownerTex), s->ownerLevel);
      return dxmt9::core::D3DERR_INVALIDCALL;
    }
  } else if (!s->locked) {
    dxmt9DebugLog("surface_unlock_rect rejected not-locked surface=%p",
                  static_cast<void*>(s));
    return dxmt9::core::D3DERR_INVALIDCALL;
  } else {
    s->locked = false;
  }

  auto* shadow = s->ownerTex ? [&]() -> ShadowLock* {
    auto it = s->ownerTex->wow64Locks.find(s->ownerLevel);
    return it != s->ownerTex->wow64Locks.end() ? &it->second : nullptr;
  }() : &s->wow64Lock;
  if (shadow && shadow->active) {
    const size_t bytes =
        static_cast<size_t>(shadow->nativePitch) * static_cast<size_t>(shadow->rows);
    if (bytes != 0) {
      copyShadowToNative(*shadow);
      shadow->active = false;
      dxmt9DebugLog("surface_unlock_rect shadow surface=%p native=%p shadow=%p rowBytes=%u rows=%u bytes=%zu",
                    static_cast<void*>(s), shadow->nativePtr, shadow->shadow.ptr,
                    shadow->rowBytes, shadow->rows, bytes);
    }
  }
  dxmt9DebugLog("surface_unlock_rect surface=%p", static_cast<void*>(s));
  s->obj->unlockRect();
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_surface_get_desc(D9CSurface* s, D9CSurfaceDesc* out) {
  if (!out) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  auto& desc = s->obj->desc();
  std::memset(out, 0, sizeof(*out));
  out->format = fmtToD3D(desc.format);
  out->resourceType = 1;
  out->usage = usageToD3D(desc.usage);
  if (desc.renderTarget) {
    out->usage |= 0x00000001u;
  }
  if (desc.depthStencil) {
    out->usage |= 0x00000002u;
  }
  out->pool = poolToD3D(desc.pool);
  out->multiSampleType = msTypeToD3D(desc.multiSampleType);
  out->multiSampleQuality =
      desc.multiSampleType == dxmt9::core::MultiSampleType::None ? 0u : 1u;
  out->width = desc.width;
  out->height = desc.height;
  return dxmt9::core::D3D_OK;
}

extern "C" D9CTexture* dxmt9c_surface_get_container_texture(D9CSurface* s) {
  return s->ownerTex;
}
