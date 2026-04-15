#pragma once

#include "dxmt9/core.hpp"

#include <span>
#include <string>

namespace dxmt9::core::metalcapture {

u64 gpuDumpTextureHandle();
const char* gpuDumpTexturePath();
bool writeTextureBmp(const std::string& path, Format format, u32 width, u32 height, u32 pitch,
                     std::span<const u8> bytes);

}  // namespace dxmt9::core::metalcapture
