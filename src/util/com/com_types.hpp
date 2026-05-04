#pragma once

#include <cstdint>
#include <cstring>

using UINT = unsigned int;

#ifndef WINAPI
#if defined(_WIN32)
#define WINAPI __stdcall
#else
#define WINAPI
#endif
#endif
#ifndef STDMETHODCALLTYPE
#define STDMETHODCALLTYPE WINAPI
#endif

#ifndef _HRESULT_DEFINED
using HRESULT = long;
#define _HRESULT_DEFINED
#endif

#ifndef DXMT9_COM_ULONG_DEFINED
using ULONG = unsigned long;
#define DXMT9_COM_ULONG_DEFINED
#endif

#ifndef GUID_DEFINED
struct _GUID {
  std::uint32_t Data1;
  std::uint16_t Data2;
  std::uint16_t Data3;
  std::uint8_t Data4[8];
};
using GUID = _GUID;
#define GUID_DEFINED
#endif

#ifndef __IID_DEFINED__
using IID = GUID;
using CLSID = GUID;
#define __IID_DEFINED__
#endif

#ifndef _REFGUID_DEFINED
using REFGUID = const GUID&;
#define _REFGUID_DEFINED
#endif

#ifndef _REFIID_DEFINED
using REFIID = const IID&;
#define _REFIID_DEFINED
#endif

#ifndef _REFCLSID_DEFINED
using REFCLSID = const CLSID&;
#define _REFCLSID_DEFINED
#endif

#ifndef __IUnknown_INTERFACE_DEFINED__
struct IUnknown {
  virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) = 0;
  virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
  virtual ULONG STDMETHODCALLTYPE Release() = 0;
};
#define __IUnknown_INTERFACE_DEFINED__
#endif

#ifndef S_OK
constexpr HRESULT S_OK = 0x00000000L;
#endif
#ifndef D3DERR_INVALIDCALL
constexpr HRESULT D3DERR_INVALIDCALL = static_cast<HRESULT>(0x8876086cL);
#endif
#ifndef D3DERR_NOTFOUND
constexpr HRESULT D3DERR_NOTFOUND = static_cast<HRESULT>(0x88760866L);
#endif
#ifndef D3DERR_MOREDATA
constexpr HRESULT D3DERR_MOREDATA = static_cast<HRESULT>(0x88760867L);
#endif

#ifndef _SYS_GUID_OPERATORS_
inline bool InlineIsEqualGUID(REFGUID a, REFGUID b) noexcept {
  return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}
#define _SYS_GUID_OPERATORS_
#endif
