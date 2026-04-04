#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>
#include <objbase.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char kLogPath[] = "C:\\dxmt9-anno1701-bypass.log";

typedef struct FakeDirectDraw7 FakeDirectDraw7;
typedef struct FakeSurface7 FakeSurface7;
typedef struct FakeClipper FakeClipper;

struct FakeDirectDraw7 {
  IDirectDraw7 iface;
  LONG ref;
  HWND hwnd;
  DWORD coop_flags;
  DWORD width;
  DWORD height;
  DWORD bpp;
};

struct FakeSurface7 {
  IDirectDrawSurface7 iface;
  LONG ref;
  FakeDirectDraw7* owner;
  DDSURFACEDESC2 desc;
  BYTE* pixels;
  DWORD pitch;
  DWORD priority;
  DWORD lod;
  DWORD uniqueness;
};

struct FakeClipper {
  IDirectDrawClipper iface;
  LONG ref;
  HWND hwnd;
};

static void stub_log(const char* prefix, const char* fmt, ...) {
  HANDLE file = CreateFileA(kLogPath,
                            FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL,
                            OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL,
                            NULL);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD ignored = 0;
  char buffer[768];
  int offset = snprintf(buffer, sizeof(buffer), "[%s] ", prefix);
  if (offset < 0) {
    CloseHandle(file);
    return;
  }
  va_list args;
  va_start(args, fmt);
  int written = vsnprintf(buffer + offset, sizeof(buffer) - (size_t)offset, fmt, args);
  va_end(args);
  if (written < 0) {
    CloseHandle(file);
    return;
  }
  size_t total = strnlen(buffer, sizeof(buffer));
  if (total + 1 < sizeof(buffer)) {
    buffer[total++] = '\n';
  }
  WriteFile(file, buffer, (DWORD)total, &ignored, NULL);
  CloseHandle(file);
}

static DDSURFACEDESC2 default_display_mode(DWORD width, DWORD height, DWORD bpp) {
  DDSURFACEDESC2 desc;
  ZeroMemory(&desc, sizeof(desc));
  desc.dwSize = sizeof(desc);
  desc.dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_PITCH;
  desc.dwWidth = width;
  desc.dwHeight = height;
  desc.lPitch = (LONG)((width * bpp) / 8u);
  desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY;
  desc.ddpfPixelFormat.dwSize = sizeof(desc.ddpfPixelFormat);
  desc.ddpfPixelFormat.dwFlags = DDPF_RGB;
  desc.ddpfPixelFormat.dwRGBBitCount = bpp;
  desc.ddpfPixelFormat.dwRBitMask = 0x00ff0000u;
  desc.ddpfPixelFormat.dwGBitMask = 0x0000ff00u;
  desc.ddpfPixelFormat.dwBBitMask = 0x000000ffu;
  desc.ddpfPixelFormat.dwRGBAlphaBitMask = 0xff000000u;
  return desc;
}

static ULONG WINAPI fake_surface_AddRef(IDirectDrawSurface7* iface);
static ULONG WINAPI fake_ddraw_AddRef(IDirectDraw7* iface);

static FakeDirectDraw7* fake_ddraw_from_iface(IDirectDraw7* iface) {
  return (FakeDirectDraw7*)iface;
}

static FakeSurface7* fake_surface_from_iface(IDirectDrawSurface7* iface) {
  return (FakeSurface7*)iface;
}

static FakeClipper* fake_clipper_from_iface(IDirectDrawClipper* iface) {
  return (FakeClipper*)iface;
}

static HRESULT WINAPI fake_clipper_QueryInterface(IDirectDrawClipper* iface, REFIID riid, void** out) {
  if (!out) {
    return E_POINTER;
  }
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectDrawClipper)) {
    *out = iface;
    IDirectDrawClipper_AddRef(iface);
    return DD_OK;
  }
  *out = NULL;
  return E_NOINTERFACE;
}

static ULONG WINAPI fake_clipper_AddRef(IDirectDrawClipper* iface) {
  return (ULONG)InterlockedIncrement(&fake_clipper_from_iface(iface)->ref);
}

static ULONG WINAPI fake_clipper_Release(IDirectDrawClipper* iface) {
  FakeClipper* clipper = fake_clipper_from_iface(iface);
  ULONG value = (ULONG)InterlockedDecrement(&clipper->ref);
  if (!value) {
    HeapFree(GetProcessHeap(), 0, clipper);
  }
  return value;
}

static HRESULT WINAPI fake_clipper_GetClipList(IDirectDrawClipper* iface, LPRECT rect,
                                               LPRGNDATA clip_list, LPDWORD size) {
  (void)iface;
  (void)rect;
  (void)clip_list;
  if (size) {
    *size = 0;
  }
  return DD_OK;
}

static HRESULT WINAPI fake_clipper_GetHWnd(IDirectDrawClipper* iface, HWND* hwnd) {
  if (!hwnd) {
    return E_POINTER;
  }
  *hwnd = fake_clipper_from_iface(iface)->hwnd;
  return DD_OK;
}

static HRESULT WINAPI fake_clipper_Initialize(IDirectDrawClipper* iface, LPDIRECTDRAW dd, DWORD flags) {
  (void)iface;
  (void)dd;
  (void)flags;
  return DD_OK;
}

static HRESULT WINAPI fake_clipper_IsClipListChanged(IDirectDrawClipper* iface, WINBOOL* changed) {
  (void)iface;
  if (changed) {
    *changed = FALSE;
  }
  return DD_OK;
}

static HRESULT WINAPI fake_clipper_SetClipList(IDirectDrawClipper* iface, LPRGNDATA clip_list,
                                               DWORD flags) {
  (void)iface;
  (void)clip_list;
  (void)flags;
  return DD_OK;
}

static HRESULT WINAPI fake_clipper_SetHWnd(IDirectDrawClipper* iface, DWORD flags, HWND hwnd) {
  (void)flags;
  fake_clipper_from_iface(iface)->hwnd = hwnd;
  return DD_OK;
}

static const IDirectDrawClipperVtbl g_fake_clipper_vtbl = {
  fake_clipper_QueryInterface,
  fake_clipper_AddRef,
  fake_clipper_Release,
  fake_clipper_GetClipList,
  fake_clipper_GetHWnd,
  fake_clipper_Initialize,
  fake_clipper_IsClipListChanged,
  fake_clipper_SetClipList,
  fake_clipper_SetHWnd,
};

static HRESULT WINAPI fake_surface_QueryInterface(IDirectDrawSurface7* iface, REFIID riid, void** out) {
  if (!out) {
    return E_POINTER;
  }
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectDrawSurface7)) {
    *out = iface;
    IDirectDrawSurface7_AddRef(iface);
    return DD_OK;
  }
  *out = NULL;
  return E_NOINTERFACE;
}

static ULONG WINAPI fake_surface_AddRef(IDirectDrawSurface7* iface) {
  return (ULONG)InterlockedIncrement(&fake_surface_from_iface(iface)->ref);
}

static ULONG WINAPI fake_surface_Release(IDirectDrawSurface7* iface) {
  FakeSurface7* surface = fake_surface_from_iface(iface);
  ULONG value = (ULONG)InterlockedDecrement(&surface->ref);
  if (!value) {
    if (surface->owner) {
      IDirectDraw7_Release(&surface->owner->iface);
    }
    HeapFree(GetProcessHeap(), 0, surface->pixels);
    HeapFree(GetProcessHeap(), 0, surface);
  }
  return value;
}

static HRESULT WINAPI fake_surface_AddAttachedSurface(IDirectDrawSurface7* iface,
                                                      LPDIRECTDRAWSURFACE7 attached) {
  (void)iface;
  (void)attached;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_AddOverlayDirtyRect(IDirectDrawSurface7* iface, LPRECT rect) {
  (void)iface;
  (void)rect;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_Blt(IDirectDrawSurface7* iface, LPRECT dst_rect,
                                       LPDIRECTDRAWSURFACE7 src_iface, LPRECT src_rect,
                                       DWORD flags, LPDDBLTFX fx) {
  FakeSurface7* dst = fake_surface_from_iface(iface);
  (void)dst_rect;
  (void)src_rect;
  (void)flags;
  (void)fx;
  if (src_iface) {
    FakeSurface7* src = fake_surface_from_iface(src_iface);
    size_t bytes = (size_t)min(dst->pitch * dst->desc.dwHeight, src->pitch * src->desc.dwHeight);
    memcpy(dst->pixels, src->pixels, bytes);
  } else {
    memset(dst->pixels, 0, (size_t)dst->pitch * dst->desc.dwHeight);
  }
  return DD_OK;
}

static HRESULT WINAPI fake_surface_BltBatch(IDirectDrawSurface7* iface, LPDDBLTBATCH batch,
                                            DWORD count, DWORD flags) {
  (void)iface;
  (void)batch;
  (void)count;
  (void)flags;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_BltFast(IDirectDrawSurface7* iface, DWORD x, DWORD y,
                                           LPDIRECTDRAWSURFACE7 src, LPRECT rect, DWORD trans) {
  (void)x;
  (void)y;
  (void)rect;
  (void)trans;
  return fake_surface_Blt(iface, NULL, src, NULL, 0, NULL);
}

static HRESULT WINAPI fake_surface_DeleteAttachedSurface(IDirectDrawSurface7* iface, DWORD flags,
                                                         LPDIRECTDRAWSURFACE7 attached) {
  (void)iface;
  (void)flags;
  (void)attached;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_EnumAttachedSurfaces(IDirectDrawSurface7* iface, LPVOID ctx,
                                                        LPDDENUMSURFACESCALLBACK7 cb) {
  (void)iface;
  (void)ctx;
  (void)cb;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_EnumOverlayZOrders(IDirectDrawSurface7* iface, DWORD flags,
                                                      LPVOID ctx, LPDDENUMSURFACESCALLBACK7 cb) {
  (void)iface;
  (void)flags;
  (void)ctx;
  (void)cb;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_Flip(IDirectDrawSurface7* iface,
                                        LPDIRECTDRAWSURFACE7 target_override, DWORD flags) {
  (void)iface;
  (void)target_override;
  (void)flags;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_GetAttachedSurface(IDirectDrawSurface7* iface, LPDDSCAPS2 caps,
                                                      LPDIRECTDRAWSURFACE7* out) {
  (void)iface;
  (void)caps;
  if (out) {
    *out = NULL;
  }
  return DDERR_NOTFOUND;
}

static HRESULT WINAPI fake_surface_GetBltStatus(IDirectDrawSurface7* iface, DWORD flags) {
  (void)iface;
  (void)flags;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_GetCaps(IDirectDrawSurface7* iface, LPDDSCAPS2 caps) {
  if (!caps) {
    return E_POINTER;
  }
  *caps = fake_surface_from_iface(iface)->desc.ddsCaps;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_GetClipper(IDirectDrawSurface7* iface,
                                              LPDIRECTDRAWCLIPPER* out) {
  (void)iface;
  if (out) {
    *out = NULL;
  }
  return DDERR_NOCLIPPERATTACHED;
}

static HRESULT WINAPI fake_surface_GetColorKey(IDirectDrawSurface7* iface, DWORD flags,
                                               LPDDCOLORKEY key) {
  (void)iface;
  (void)flags;
  if (key) {
    ZeroMemory(key, sizeof(*key));
  }
  return DD_OK;
}

static HRESULT WINAPI fake_surface_GetDC(IDirectDrawSurface7* iface, HDC* dc) {
  (void)iface;
  if (dc) {
    *dc = NULL;
  }
  return DDERR_GENERIC;
}

static HRESULT WINAPI fake_surface_GetFlipStatus(IDirectDrawSurface7* iface, DWORD flags) {
  (void)iface;
  (void)flags;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_GetOverlayPosition(IDirectDrawSurface7* iface, LPLONG x, LPLONG y) {
  (void)iface;
  if (x) {
    *x = 0;
  }
  if (y) {
    *y = 0;
  }
  return DD_OK;
}

static HRESULT WINAPI fake_surface_GetPalette(IDirectDrawSurface7* iface,
                                              LPDIRECTDRAWPALETTE* out) {
  (void)iface;
  if (out) {
    *out = NULL;
  }
  return DDERR_NOPALETTEATTACHED;
}

static HRESULT WINAPI fake_surface_GetPixelFormat(IDirectDrawSurface7* iface,
                                                  LPDDPIXELFORMAT format) {
  if (!format) {
    return E_POINTER;
  }
  *format = fake_surface_from_iface(iface)->desc.ddpfPixelFormat;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_GetSurfaceDesc(IDirectDrawSurface7* iface,
                                                  LPDDSURFACEDESC2 desc) {
  FakeSurface7* surface = fake_surface_from_iface(iface);
  if (!desc) {
    return E_POINTER;
  }
  *desc = surface->desc;
  desc->lpSurface = surface->pixels;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_Initialize(IDirectDrawSurface7* iface, LPDIRECTDRAW dd,
                                              LPDDSURFACEDESC2 desc) {
  (void)iface;
  (void)dd;
  (void)desc;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_IsLost(IDirectDrawSurface7* iface) {
  (void)iface;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_Lock(IDirectDrawSurface7* iface, LPRECT rect,
                                        LPDDSURFACEDESC2 desc, DWORD flags, HANDLE event) {
  FakeSurface7* surface = fake_surface_from_iface(iface);
  (void)rect;
  (void)flags;
  (void)event;
  if (!desc) {
    return E_POINTER;
  }
  *desc = surface->desc;
  desc->lpSurface = surface->pixels;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_ReleaseDC(IDirectDrawSurface7* iface, HDC dc) {
  (void)iface;
  (void)dc;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_Restore(IDirectDrawSurface7* iface) {
  (void)iface;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_SetClipper(IDirectDrawSurface7* iface,
                                              LPDIRECTDRAWCLIPPER clipper) {
  (void)iface;
  (void)clipper;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_SetColorKey(IDirectDrawSurface7* iface, DWORD flags,
                                               LPDDCOLORKEY key) {
  (void)iface;
  (void)flags;
  (void)key;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_SetOverlayPosition(IDirectDrawSurface7* iface, LONG x, LONG y) {
  (void)iface;
  (void)x;
  (void)y;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_SetPalette(IDirectDrawSurface7* iface,
                                              LPDIRECTDRAWPALETTE palette) {
  (void)iface;
  (void)palette;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_Unlock(IDirectDrawSurface7* iface, LPRECT rect) {
  (void)iface;
  (void)rect;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_UpdateOverlay(IDirectDrawSurface7* iface, LPRECT src_rect,
                                                 LPDIRECTDRAWSURFACE7 dst, LPRECT dst_rect,
                                                 DWORD flags, LPDDOVERLAYFX fx) {
  (void)iface;
  (void)src_rect;
  (void)dst;
  (void)dst_rect;
  (void)flags;
  (void)fx;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_UpdateOverlayDisplay(IDirectDrawSurface7* iface, DWORD flags) {
  (void)iface;
  (void)flags;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_UpdateOverlayZOrder(IDirectDrawSurface7* iface, DWORD flags,
                                                       LPDIRECTDRAWSURFACE7 ref) {
  (void)iface;
  (void)flags;
  (void)ref;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_GetDDInterface(IDirectDrawSurface7* iface, LPVOID* out) {
  FakeSurface7* surface = fake_surface_from_iface(iface);
  if (!out) {
    return E_POINTER;
  }
  *out = &surface->owner->iface;
  IDirectDraw7_AddRef(&surface->owner->iface);
  return DD_OK;
}

static HRESULT WINAPI fake_surface_PageLock(IDirectDrawSurface7* iface, DWORD flags) {
  (void)iface;
  (void)flags;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_PageUnlock(IDirectDrawSurface7* iface, DWORD flags) {
  (void)iface;
  (void)flags;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_SetSurfaceDesc(IDirectDrawSurface7* iface,
                                                  LPDDSURFACEDESC2 desc, DWORD flags) {
  FakeSurface7* surface = fake_surface_from_iface(iface);
  (void)flags;
  if (!desc) {
    return E_POINTER;
  }
  surface->desc = *desc;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_SetPrivateData(IDirectDrawSurface7* iface, REFGUID tag,
                                                  LPVOID data, DWORD size, DWORD flags) {
  (void)iface;
  (void)tag;
  (void)data;
  (void)size;
  (void)flags;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_GetPrivateData(IDirectDrawSurface7* iface, REFGUID tag,
                                                  LPVOID buffer, LPDWORD size) {
  (void)iface;
  (void)tag;
  (void)buffer;
  if (size) {
    *size = 0;
  }
  return DDERR_NOTFOUND;
}

static HRESULT WINAPI fake_surface_FreePrivateData(IDirectDrawSurface7* iface, REFGUID tag) {
  (void)iface;
  (void)tag;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_GetUniquenessValue(IDirectDrawSurface7* iface, LPDWORD value) {
  FakeSurface7* surface = fake_surface_from_iface(iface);
  if (!value) {
    return E_POINTER;
  }
  *value = surface->uniqueness;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_ChangeUniquenessValue(IDirectDrawSurface7* iface) {
  FakeSurface7* surface = fake_surface_from_iface(iface);
  ++surface->uniqueness;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_SetPriority(IDirectDrawSurface7* iface, DWORD prio) {
  fake_surface_from_iface(iface)->priority = prio;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_GetPriority(IDirectDrawSurface7* iface, LPDWORD prio) {
  if (!prio) {
    return E_POINTER;
  }
  *prio = fake_surface_from_iface(iface)->priority;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_SetLOD(IDirectDrawSurface7* iface, DWORD lod) {
  fake_surface_from_iface(iface)->lod = lod;
  return DD_OK;
}

static HRESULT WINAPI fake_surface_GetLOD(IDirectDrawSurface7* iface, LPDWORD lod) {
  if (!lod) {
    return E_POINTER;
  }
  *lod = fake_surface_from_iface(iface)->lod;
  return DD_OK;
}

static const IDirectDrawSurface7Vtbl g_fake_surface_vtbl = {
  fake_surface_QueryInterface,
  fake_surface_AddRef,
  fake_surface_Release,
  fake_surface_AddAttachedSurface,
  fake_surface_AddOverlayDirtyRect,
  fake_surface_Blt,
  fake_surface_BltBatch,
  fake_surface_BltFast,
  fake_surface_DeleteAttachedSurface,
  fake_surface_EnumAttachedSurfaces,
  fake_surface_EnumOverlayZOrders,
  fake_surface_Flip,
  fake_surface_GetAttachedSurface,
  fake_surface_GetBltStatus,
  fake_surface_GetCaps,
  fake_surface_GetClipper,
  fake_surface_GetColorKey,
  fake_surface_GetDC,
  fake_surface_GetFlipStatus,
  fake_surface_GetOverlayPosition,
  fake_surface_GetPalette,
  fake_surface_GetPixelFormat,
  fake_surface_GetSurfaceDesc,
  fake_surface_Initialize,
  fake_surface_IsLost,
  fake_surface_Lock,
  fake_surface_ReleaseDC,
  fake_surface_Restore,
  fake_surface_SetClipper,
  fake_surface_SetColorKey,
  fake_surface_SetOverlayPosition,
  fake_surface_SetPalette,
  fake_surface_Unlock,
  fake_surface_UpdateOverlay,
  fake_surface_UpdateOverlayDisplay,
  fake_surface_UpdateOverlayZOrder,
  fake_surface_GetDDInterface,
  fake_surface_PageLock,
  fake_surface_PageUnlock,
  fake_surface_SetSurfaceDesc,
  fake_surface_SetPrivateData,
  fake_surface_GetPrivateData,
  fake_surface_FreePrivateData,
  fake_surface_GetUniquenessValue,
  fake_surface_ChangeUniquenessValue,
  fake_surface_SetPriority,
  fake_surface_GetPriority,
  fake_surface_SetLOD,
  fake_surface_GetLOD,
};

static HRESULT fake_surface_create(FakeDirectDraw7* owner, LPDDSURFACEDESC2 requested,
                                   LPDIRECTDRAWSURFACE7* out) {
  FakeSurface7* surface;
  DWORD width = owner->width ? owner->width : 1024;
  DWORD height = owner->height ? owner->height : 768;
  DWORD bpp = owner->bpp ? owner->bpp : 32;
  if (!out) {
    return E_POINTER;
  }
  *out = NULL;
  surface = (FakeSurface7*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*surface));
  if (!surface) {
    return E_OUTOFMEMORY;
  }
  surface->iface.lpVtbl = &g_fake_surface_vtbl;
  surface->ref = 1;
  surface->owner = owner;
  IDirectDraw7_AddRef(&owner->iface);
  surface->desc = requested ? *requested : default_display_mode(width, height, bpp);
  if (!surface->desc.dwSize) {
    surface->desc.dwSize = sizeof(surface->desc);
  }
  if (!(surface->desc.dwFlags & DDSD_WIDTH)) {
    surface->desc.dwWidth = width;
    surface->desc.dwFlags |= DDSD_WIDTH;
  }
  if (!(surface->desc.dwFlags & DDSD_HEIGHT)) {
    surface->desc.dwHeight = height;
    surface->desc.dwFlags |= DDSD_HEIGHT;
  }
  if (!(surface->desc.dwFlags & DDSD_PIXELFORMAT)) {
    surface->desc.ddpfPixelFormat = default_display_mode(width, height, bpp).ddpfPixelFormat;
    surface->desc.dwFlags |= DDSD_PIXELFORMAT;
  }
  if (!(surface->desc.dwFlags & DDSD_CAPS)) {
    surface->desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    surface->desc.dwFlags |= DDSD_CAPS;
  }
  surface->pitch = (surface->desc.dwWidth * max(surface->desc.ddpfPixelFormat.dwRGBBitCount, 32u)) / 8u;
  surface->desc.lPitch = (LONG)surface->pitch;
  surface->desc.dwFlags |= DDSD_PITCH;
  surface->pixels = (BYTE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                     (SIZE_T)surface->pitch * surface->desc.dwHeight);
  if (!surface->pixels) {
    IDirectDraw7_Release(&owner->iface);
    HeapFree(GetProcessHeap(), 0, surface);
    return E_OUTOFMEMORY;
  }
  surface->uniqueness = 1;
  *out = &surface->iface;
  stub_log("ddraw", "CreateSurface size=%lux%lu caps=0x%lx -> %p",
           surface->desc.dwWidth, surface->desc.dwHeight, surface->desc.ddsCaps.dwCaps, surface);
  return DD_OK;
}

static HRESULT WINAPI fake_ddraw_QueryInterface(IDirectDraw7* iface, REFIID riid, void** out) {
  if (!out) {
    return E_POINTER;
  }
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectDraw7)) {
    *out = iface;
    IDirectDraw7_AddRef(iface);
    return DD_OK;
  }
  *out = NULL;
  stub_log("ddraw", "QueryInterface unsupported iid");
  return E_NOINTERFACE;
}

static ULONG WINAPI fake_ddraw_AddRef(IDirectDraw7* iface) {
  return (ULONG)InterlockedIncrement(&fake_ddraw_from_iface(iface)->ref);
}

static ULONG WINAPI fake_ddraw_Release(IDirectDraw7* iface) {
  FakeDirectDraw7* ddraw = fake_ddraw_from_iface(iface);
  ULONG value = (ULONG)InterlockedDecrement(&ddraw->ref);
  stub_log("ddraw", "Release ref=%lu", value);
  if (!value) {
    HeapFree(GetProcessHeap(), 0, ddraw);
  }
  return value;
}

static HRESULT WINAPI fake_ddraw_Compact(IDirectDraw7* iface) {
  (void)iface;
  return DD_OK;
}

static HRESULT WINAPI fake_ddraw_CreateClipper(IDirectDraw7* iface, DWORD flags,
                                               LPDIRECTDRAWCLIPPER* out, IUnknown* outer) {
  FakeClipper* clipper;
  FakeDirectDraw7* ddraw = fake_ddraw_from_iface(iface);
  (void)flags;
  (void)outer;
  if (!out) {
    return E_POINTER;
  }
  clipper = (FakeClipper*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*clipper));
  if (!clipper) {
    return E_OUTOFMEMORY;
  }
  clipper->iface.lpVtbl = &g_fake_clipper_vtbl;
  clipper->ref = 1;
  clipper->hwnd = ddraw->hwnd;
  *out = &clipper->iface;
  stub_log("ddraw", "CreateClipper -> %p", clipper);
  return DD_OK;
}

static HRESULT WINAPI fake_ddraw_CreatePalette(IDirectDraw7* iface, DWORD flags,
                                               LPPALETTEENTRY table, LPDIRECTDRAWPALETTE* out,
                                               IUnknown* outer) {
  (void)iface;
  (void)flags;
  (void)table;
  (void)outer;
  if (out) {
    *out = NULL;
  }
  stub_log("ddraw", "CreatePalette unsupported");
  return DDERR_UNSUPPORTED;
}

static HRESULT WINAPI fake_ddraw_CreateSurface(IDirectDraw7* iface, LPDDSURFACEDESC2 desc,
                                               LPDIRECTDRAWSURFACE7* out, IUnknown* outer) {
  (void)outer;
  return fake_surface_create(fake_ddraw_from_iface(iface), desc, out);
}

static HRESULT WINAPI fake_ddraw_DuplicateSurface(IDirectDraw7* iface, LPDIRECTDRAWSURFACE7 src,
                                                  LPDIRECTDRAWSURFACE7* out) {
  FakeSurface7* source = src ? fake_surface_from_iface(src) : NULL;
  return fake_surface_create(fake_ddraw_from_iface(iface), source ? &source->desc : NULL, out);
}

static HRESULT WINAPI fake_ddraw_EnumDisplayModes(IDirectDraw7* iface, DWORD flags,
                                                  LPDDSURFACEDESC2 filter, LPVOID ctx,
                                                  LPDDENUMMODESCALLBACK2 cb) {
  DDSURFACEDESC2 desc;
  FakeDirectDraw7* ddraw = fake_ddraw_from_iface(iface);
  (void)flags;
  (void)filter;
  if (!cb) {
    return DD_OK;
  }
  desc = default_display_mode(ddraw->width, ddraw->height, ddraw->bpp);
  cb(&desc, ctx);
  return DD_OK;
}

static HRESULT WINAPI fake_ddraw_EnumSurfaces(IDirectDraw7* iface, DWORD flags,
                                              LPDDSURFACEDESC2 desc, LPVOID ctx,
                                              LPDDENUMSURFACESCALLBACK7 cb) {
  (void)iface;
  (void)flags;
  (void)desc;
  (void)ctx;
  (void)cb;
  return DD_OK;
}

static HRESULT WINAPI fake_ddraw_FlipToGDISurface(IDirectDraw7* iface) {
  (void)iface;
  return DD_OK;
}

static HRESULT WINAPI fake_ddraw_GetCaps(IDirectDraw7* iface, LPDDCAPS driver, LPDDCAPS hel) {
  (void)iface;
  if (driver) {
    ZeroMemory(driver, sizeof(*driver));
    driver->dwSize = sizeof(*driver);
    driver->dwCaps = DDCAPS_3D | DDCAPS_BLT | DDCAPS_BLTQUEUE | DDCAPS_GDI;
    driver->ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_FLIP |
                             DDSCAPS_COMPLEX | DDSCAPS_3DDEVICE | DDSCAPS_TEXTURE;
    driver->dwVidMemTotal = 256u * 1024u * 1024u;
    driver->dwVidMemFree = driver->dwVidMemTotal;
  }
  if (hel) {
    ZeroMemory(hel, sizeof(*hel));
    hel->dwSize = sizeof(*hel);
    hel->dwCaps = DDCAPS_3D | DDCAPS_BLT | DDCAPS_GDI;
    hel->ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY | DDSCAPS_TEXTURE;
    hel->dwVidMemTotal = 128u * 1024u * 1024u;
    hel->dwVidMemFree = hel->dwVidMemTotal;
  }
  return DD_OK;
}

static HRESULT WINAPI fake_ddraw_GetDisplayMode(IDirectDraw7* iface, LPDDSURFACEDESC2 desc) {
  FakeDirectDraw7* ddraw = fake_ddraw_from_iface(iface);
  if (!desc) {
    return E_POINTER;
  }
  *desc = default_display_mode(ddraw->width, ddraw->height, ddraw->bpp);
  return DD_OK;
}

static HRESULT WINAPI fake_ddraw_GetFourCCCodes(IDirectDraw7* iface, LPDWORD num, LPDWORD codes) {
  (void)iface;
  if (num) {
    *num = 0;
  }
  (void)codes;
  return DD_OK;
}

static HRESULT WINAPI fake_ddraw_GetGDISurface(IDirectDraw7* iface, LPDIRECTDRAWSURFACE7* out) {
  return fake_surface_create(fake_ddraw_from_iface(iface), NULL, out);
}

static HRESULT WINAPI fake_ddraw_GetMonitorFrequency(IDirectDraw7* iface, LPDWORD frequency) {
  (void)iface;
  if (!frequency) {
    return E_POINTER;
  }
  *frequency = 60;
  return DD_OK;
}

static HRESULT WINAPI fake_ddraw_GetScanLine(IDirectDraw7* iface, LPDWORD scanline) {
  (void)iface;
  if (!scanline) {
    return E_POINTER;
  }
  *scanline = 0;
  return DD_OK;
}

static HRESULT WINAPI fake_ddraw_GetVerticalBlankStatus(IDirectDraw7* iface, WINBOOL* in_vblank) {
  (void)iface;
  if (!in_vblank) {
    return E_POINTER;
  }
  *in_vblank = FALSE;
  return DD_OK;
}

static HRESULT WINAPI fake_ddraw_Initialize(IDirectDraw7* iface, GUID* guid) {
  (void)iface;
  (void)guid;
  return DD_OK;
}

static HRESULT WINAPI fake_ddraw_RestoreDisplayMode(IDirectDraw7* iface) {
  (void)iface;
  return DD_OK;
}

static HRESULT WINAPI fake_ddraw_SetCooperativeLevel(IDirectDraw7* iface, HWND hwnd, DWORD flags) {
  FakeDirectDraw7* ddraw = fake_ddraw_from_iface(iface);
  ddraw->hwnd = hwnd;
  ddraw->coop_flags = flags;
  stub_log("ddraw", "SetCooperativeLevel hwnd=%p flags=0x%lx", hwnd, flags);
  return DD_OK;
}

static HRESULT WINAPI fake_ddraw_SetDisplayMode(IDirectDraw7* iface, DWORD width, DWORD height,
                                                DWORD bpp, DWORD refresh, DWORD flags) {
  FakeDirectDraw7* ddraw = fake_ddraw_from_iface(iface);
  (void)refresh;
  (void)flags;
  ddraw->width = width;
  ddraw->height = height;
  ddraw->bpp = bpp ? bpp : 32;
  stub_log("ddraw", "SetDisplayMode %lux%lu@%lu", width, height, bpp);
  return DD_OK;
}

static HRESULT WINAPI fake_ddraw_WaitForVerticalBlank(IDirectDraw7* iface, DWORD flags, HANDLE event) {
  (void)iface;
  (void)flags;
  if (event) {
    SetEvent(event);
  }
  return DD_OK;
}

static HRESULT WINAPI fake_ddraw_GetAvailableVidMem(IDirectDraw7* iface, LPDDSCAPS2 caps,
                                                    LPDWORD total, LPDWORD free_mem) {
  (void)iface;
  (void)caps;
  if (total) {
    *total = 256u * 1024u * 1024u;
  }
  if (free_mem) {
    *free_mem = 256u * 1024u * 1024u;
  }
  return DD_OK;
}

static HRESULT WINAPI fake_ddraw_GetSurfaceFromDC(IDirectDraw7* iface, HDC dc,
                                                  LPDIRECTDRAWSURFACE7* out) {
  (void)dc;
  return fake_surface_create(fake_ddraw_from_iface(iface), NULL, out);
}

static HRESULT WINAPI fake_ddraw_RestoreAllSurfaces(IDirectDraw7* iface) {
  (void)iface;
  return DD_OK;
}

static HRESULT WINAPI fake_ddraw_TestCooperativeLevel(IDirectDraw7* iface) {
  (void)iface;
  return DD_OK;
}

static HRESULT WINAPI fake_ddraw_GetDeviceIdentifier(IDirectDraw7* iface,
                                                     LPDDDEVICEIDENTIFIER2 ident, DWORD flags) {
  (void)iface;
  (void)flags;
  if (!ident) {
    return E_POINTER;
  }
  ZeroMemory(ident, sizeof(*ident));
  strcpy(ident->szDriver, "dxmt9ddraw");
  strcpy(ident->szDescription, "dxmt9 ddraw startup stub");
  ident->liDriverVersion.HighPart = 0;
  ident->liDriverVersion.LowPart = 1;
  ident->dwVendorId = 0x10de;
  ident->dwDeviceId = 0x0045;
  ident->dwSubSysId = 0;
  ident->dwRevision = 0;
  return DD_OK;
}

static HRESULT WINAPI fake_ddraw_StartModeTest(IDirectDraw7* iface, LPSIZE modes, DWORD count,
                                               DWORD flags) {
  (void)iface;
  (void)modes;
  (void)count;
  (void)flags;
  return DD_OK;
}

static HRESULT WINAPI fake_ddraw_EvaluateMode(IDirectDraw7* iface, DWORD flags, DWORD* timeout) {
  (void)iface;
  (void)flags;
  if (timeout) {
    *timeout = 0;
  }
  return DD_OK;
}

static const IDirectDraw7Vtbl g_fake_ddraw_vtbl = {
  fake_ddraw_QueryInterface,
  fake_ddraw_AddRef,
  fake_ddraw_Release,
  fake_ddraw_Compact,
  fake_ddraw_CreateClipper,
  fake_ddraw_CreatePalette,
  fake_ddraw_CreateSurface,
  fake_ddraw_DuplicateSurface,
  fake_ddraw_EnumDisplayModes,
  fake_ddraw_EnumSurfaces,
  fake_ddraw_FlipToGDISurface,
  fake_ddraw_GetCaps,
  fake_ddraw_GetDisplayMode,
  fake_ddraw_GetFourCCCodes,
  fake_ddraw_GetGDISurface,
  fake_ddraw_GetMonitorFrequency,
  fake_ddraw_GetScanLine,
  fake_ddraw_GetVerticalBlankStatus,
  fake_ddraw_Initialize,
  fake_ddraw_RestoreDisplayMode,
  fake_ddraw_SetCooperativeLevel,
  fake_ddraw_SetDisplayMode,
  fake_ddraw_WaitForVerticalBlank,
  fake_ddraw_GetAvailableVidMem,
  fake_ddraw_GetSurfaceFromDC,
  fake_ddraw_RestoreAllSurfaces,
  fake_ddraw_TestCooperativeLevel,
  fake_ddraw_GetDeviceIdentifier,
  fake_ddraw_StartModeTest,
  fake_ddraw_EvaluateMode,
};

static HRESULT create_ddraw7_instance(void** out) {
  FakeDirectDraw7* ddraw;
  if (!out) {
    return E_POINTER;
  }
  *out = NULL;
  ddraw = (FakeDirectDraw7*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*ddraw));
  if (!ddraw) {
    return E_OUTOFMEMORY;
  }
  ddraw->iface.lpVtbl = &g_fake_ddraw_vtbl;
  ddraw->ref = 1;
  ddraw->width = 1024;
  ddraw->height = 768;
  ddraw->bpp = 32;
  *out = &ddraw->iface;
  stub_log("ddraw", "create fake IDirectDraw7 %p", ddraw);
  return DD_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

__declspec(dllexport) HRESULT WINAPI DirectDrawCreate(GUID* guid, LPDIRECTDRAW* out, IUnknown* outer) {
  (void)guid;
  (void)outer;
  if (out) {
    *out = NULL;
  }
  stub_log("ddraw", "DirectDrawCreate unsupported");
  return DDERR_NODIRECTDRAWSUPPORT;
}

__declspec(dllexport) HRESULT WINAPI DirectDrawCreateEx(GUID* guid, void** out, REFIID iid, IUnknown* outer) {
  LPOLESTR iid_str = NULL;
  HRESULT hr;
  (void)guid;
  (void)outer;
  StringFromIID(iid, &iid_str);
  stub_log("ddraw", "DirectDrawCreateEx guid=%p out=%p iid=%ls outer=%p",
           guid, out, iid_str ? iid_str : L"(null)", outer);
  if (iid_str) {
    CoTaskMemFree(iid_str);
  }
  if (!iid || (!IsEqualIID(iid, &IID_IDirectDraw7) && !IsEqualIID(iid, &IID_IUnknown))) {
    if (out) {
      *out = NULL;
    }
    stub_log("ddraw", "DirectDrawCreateEx unsupported iid");
    return E_NOINTERFACE;
  }
  hr = create_ddraw7_instance(out);
  stub_log("ddraw", "DirectDrawCreateEx -> 0x%08lx object=%p", hr, out ? *out : NULL);
  return hr;
}

__declspec(dllexport) HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACKA callback, void* context) {
  stub_log("ddraw", "DirectDrawEnumerateA callback=%p context=%p", callback, context);
  if (callback) {
    callback(NULL, "dxmt9 ddraw stub", "dxmt9 ddraw stub", context);
  }
  return DD_OK;
}

__declspec(dllexport) HRESULT WINAPI DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA callback,
                                                            void* context,
                                                            DWORD flags) {
  stub_log("ddraw", "DirectDrawEnumerateExA callback=%p context=%p flags=0x%lx",
           callback, context, flags);
  if (callback) {
    callback(NULL, "dxmt9 ddraw stub", "dxmt9 ddraw stub", context, NULL);
  }
  return DD_OK;
}
