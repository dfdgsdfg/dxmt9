#pragma once

#include "dxmt9/core.hpp"

#include <span>
#include <string>

namespace dxmt9::util {

bool writeBmp(const std::string& path,
              dxmt9::core::Format format,
              dxmt9::core::u32 width,
              dxmt9::core::u32 height,
              dxmt9::core::u32 pitch,
              std::span<const dxmt9::core::u8> bytes);

}  // namespace dxmt9::util
