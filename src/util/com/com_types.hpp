#pragma once

#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#include <d3d9.h>
#else

using UINT = unsigned int;
using HRESULT = long;
using ULONG = unsigned long;

struct GUID {
  std::uint32_t Data1;
  std::uint16_t Data2;
  std::uint16_t Data3;
  std::uint8_t Data4[8];
};

using IID = GUID;
using CLSID = GUID;
using REFGUID = const GUID&;
using REFIID = const IID&;
using REFCLSID = const CLSID&;

struct IUnknown {
  virtual ~IUnknown() = default;
  virtual ULONG AddRef() = 0;
  virtual ULONG Release() = 0;
};

constexpr HRESULT S_OK = 0x00000000L;
constexpr HRESULT D3DERR_INVALIDCALL = static_cast<HRESULT>(0x8876086cL);
constexpr HRESULT D3DERR_NOTFOUND = static_cast<HRESULT>(0x88760866L);
constexpr HRESULT D3DERR_MOREDATA = static_cast<HRESULT>(0x88760867L);

inline bool InlineIsEqualGUID(REFGUID a, REFGUID b) noexcept {
  return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

#endif
