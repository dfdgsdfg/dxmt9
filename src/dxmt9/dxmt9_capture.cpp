#include "dxmt9_capture.hpp"

#include "util/util_bmp.hpp"

#include <cstdlib>
#include <cstring>

namespace dxmt9::core::metalcapture {

u64 gpuDumpTextureHandle() {
  static const u64 handle = [] {
    const char* env = std::getenv("DXMT_DUMP_GPU_TEXTURE_HANDLE");
    if (!env || env[0] == '\0') {
      return 0ull;
    }
    return static_cast<u64>(std::strtoull(env, nullptr, 0));
  }();
  return handle;
}

const char* gpuDumpTexturePath() {
  static const char* path = std::getenv("DXMT_DUMP_GPU_TEXTURE_PATH");
  return path && path[0] != '\0' ? path : nullptr;
}

bool writeTextureBmp(const std::string& path, Format format, u32 width, u32 height, u32 pitch,
                     std::span<const u8> bytes) {
  return dxmt9::util::writeBmp(path, format, width, height, pitch, bytes);
}

}  // namespace dxmt9::core::metalcapture
