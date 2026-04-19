#include "device_c_common.hpp"

using namespace dxmt9::d3d9::devicec;

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
  if (r) {
    dxmt9DebugLog("texture_lock_rect begin texture=%p level=%u flags=0x%x rect=(%d,%d)-(%d,%d)",
                  static_cast<void*>(t), level, flags, r->left, r->top, r->right, r->bottom);
  } else {
    dxmt9DebugLog("texture_lock_rect begin texture=%p level=%u flags=0x%x rect=<full>",
                  static_cast<void*>(t), level, flags);
  }
  auto* rect = r ? new dxmt9::core::Rect{r->left, r->top, r->right, r->bottom} : nullptr;
  auto lock = t->obj->lockRect(level, rect, flags);
  delete rect;
  out->pitch = static_cast<int32_t>(lock.pitch);
  out->bits = lock.data;
  if (lock.data && !pointerFits32Bit(lock.data)) {
    const auto& desc = t->obj->desc();
    const uint32_t levelWidth = std::max(1u, desc.width >> std::min(level, 31u));
    const uint32_t levelHeight = std::max(1u, desc.height >> std::min(level, 31u));
    const uint32_t bpp = dxmt9::core::bytesPerPixel(desc.format);
    const int32_t left = r ? std::clamp(r->left, 0, static_cast<int32_t>(levelWidth)) : 0;
    const int32_t top = r ? std::clamp(r->top, 0, static_cast<int32_t>(levelHeight)) : 0;
    const int32_t right =
        r ? std::clamp(r->right, left, static_cast<int32_t>(levelWidth))
          : static_cast<int32_t>(levelWidth);
    const int32_t bottom =
        r ? std::clamp(r->bottom, top, static_cast<int32_t>(levelHeight))
          : static_cast<int32_t>(levelHeight);
    const uint32_t rowBytes = static_cast<uint32_t>(std::max<int32_t>(0, right - left)) * bpp;
    const uint32_t rows = static_cast<uint32_t>(std::max<int32_t>(0, bottom - top));
    const size_t shadowBytes = static_cast<size_t>(rowBytes) * rows;
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
    auto* dst = static_cast<uint8_t*>(shadow.shadow.ptr);
    auto* src = static_cast<const uint8_t*>(shadow.nativePtr);
    for (uint32_t row = 0; row < rows; ++row) {
      std::memcpy(dst + static_cast<size_t>(row) * rowBytes,
                  src + static_cast<size_t>(row) * shadow.nativePitch, rowBytes);
    }
    out->pitch = static_cast<int32_t>(rowBytes);
    out->bits = shadow.shadow.ptr;
    dxmt9DebugLog("texture_lock_rect shadow texture=%p level=%u nativeBits=%p shadowBits=%p rowBytes=%u rows=%u",
                  static_cast<void*>(t), level, shadow.nativePtr, out->bits, rowBytes, rows);
  }
  dxmt9DebugLog("texture_lock_rect ok texture=%p level=%u pitch=%d bits=%p",
                static_cast<void*>(t), level, out->pitch, out->bits);
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_texture_unlock_rect(D9CTexture* t, uint32_t level) {
  if (auto it = t->wow64Locks.find(level); it != t->wow64Locks.end()) {
    auto& shadow = it->second;
    auto* dst = static_cast<uint8_t*>(shadow.nativePtr);
    auto* src = static_cast<const uint8_t*>(shadow.shadow.ptr);
    for (uint32_t row = 0; row < shadow.rows; ++row) {
      std::memcpy(dst + static_cast<size_t>(row) * shadow.nativePitch,
                  src + static_cast<size_t>(row) * shadow.rowBytes, shadow.rowBytes);
    }
    dxmt9DebugLog("texture_unlock_rect shadow texture=%p level=%u nativeBits=%p shadowBits=%p rowBytes=%u rows=%u",
                  static_cast<void*>(t), level, shadow.nativePtr, shadow.shadow.ptr,
                  shadow.rowBytes, shadow.rows);
    releaseShadowLock(shadow);
    t->wow64Locks.erase(it);
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
  auto* wrap = new D9CSurface{surf};
  wrap->ownerTex = t;
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
  auto& desc = t->obj->desc();
  std::memset(out, 0, sizeof(*out));
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

extern "C" int32_t dxmt9c_texture_generate_mip_sublevels(D9CTexture*) {
  return dxmt9::core::D3D_OK;
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
  auto lock = b->obj->lock(offset, actualSize, flags);
  *data = lock.data;
  if (lock.data && !pointerFits32Bit(lock.data)) {
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
  auto* rect = r ? new dxmt9::core::Rect{r->left, r->top, r->right, r->bottom} : nullptr;
  auto lock = s->obj->lockRect(rect, flags);
  delete rect;
  out->pitch = static_cast<int32_t>(lock.pitch);
  out->bits = lock.data;
  if (lock.data && !pointerFits32Bit(lock.data)) {
    const auto& desc = s->obj->desc();
    const int32_t top = r ? r->top : 0;
    const int32_t bottom = r ? r->bottom : static_cast<int32_t>(desc.height);
    const uint32_t rows = bottom > top ? static_cast<uint32_t>(bottom - top) : 0u;
    const uint32_t rowBytes = static_cast<uint32_t>(std::abs(out->pitch));
    const size_t bytes = static_cast<size_t>(rows) * static_cast<size_t>(rowBytes);
    if (bytes != 0) {
      if (!s->wow64Lock.shadow || s->wow64Lock.shadow.size < bytes) {
        releaseShadowLock(s->wow64Lock);
        s->wow64Lock.shadow = allocateLow4GB(bytes);
      }
      if (!s->wow64Lock.shadow) {
        out->bits = nullptr;
        return dxmt9::core::D3DERR_INVALIDCALL;
      }
      s->wow64Lock.nativePtr = lock.data;
      s->wow64Lock.nativePitch = rowBytes;
      s->wow64Lock.rowBytes = rowBytes;
      s->wow64Lock.rows = rows;
      std::memcpy(s->wow64Lock.shadow.ptr, lock.data, bytes);
      out->bits = s->wow64Lock.shadow.ptr;
      dxmt9DebugLog("surface_lock_rect shadow surface=%p native=%p shadow=%p pitch=%u rows=%u bytes=%zu",
                    static_cast<void*>(s), lock.data, out->bits, rowBytes, rows, bytes);
    }
  }
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_surface_unlock_rect(D9CSurface* s) {
  if (s->wow64Lock.shadow) {
    const size_t bytes =
        static_cast<size_t>(s->wow64Lock.rowBytes) * static_cast<size_t>(s->wow64Lock.rows);
    if (bytes != 0) {
      std::memcpy(s->wow64Lock.nativePtr, s->wow64Lock.shadow.ptr, bytes);
      dxmt9DebugLog("surface_unlock_rect shadow surface=%p native=%p shadow=%p bytes=%zu",
                    static_cast<void*>(s), s->wow64Lock.nativePtr, s->wow64Lock.shadow.ptr, bytes);
    }
    releaseShadowLock(s->wow64Lock);
  }
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
