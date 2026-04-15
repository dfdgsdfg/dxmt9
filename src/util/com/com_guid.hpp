#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstddef>
#include <string>

namespace dxmt9::util {

struct GuidHash {
  std::size_t operator()(REFGUID guid) const noexcept;
};

bool guidEqual(REFGUID a, REFGUID b) noexcept;
std::string formatGuid(REFGUID guid);

}  // namespace dxmt9::util
