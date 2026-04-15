#include "dxmt9_capture.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

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

struct BmpColor {
  u8 r = 0;
  u8 g = 0;
  u8 b = 0;
  u8 a = 255;
};

static BmpColor decodeBmpColor(Format format, const u8* src) {
  switch (format) {
    case Format::A8R8G8B8:
    case Format::X8R8G8B8:
      return {src[2], src[1], src[0], static_cast<u8>(format == Format::X8R8G8B8 ? 255u : src[3])};
    case Format::A8B8G8R8:
    case Format::X8B8G8R8:
      return {src[0], src[1], src[2], static_cast<u8>(format == Format::X8B8G8R8 ? 255u : src[3])};
    default:
      return {};
  }
}

bool writeTextureBmp(const std::string& path, Format format, u32 width, u32 height, u32 pitch,
                     std::span<const u8> bytes) {
  if (path.empty() || width == 0 || height == 0 || pitch == 0) {
    return false;
  }
  const u32 srcBytesPerPixel = bytesPerPixel(format);
  if (srcBytesPerPixel == 0 || bytes.size() < static_cast<size_t>(pitch) * height) {
    return false;
  }
  const u32 bytesPerRow = width * 4;
  const u32 imageSize = bytesPerRow * height;
  const u32 fileSize = 14 + 40 + imageSize;
  std::vector<u8> out(fileSize, 0);

  auto writeU16 = [&](size_t offset, u16 value) {
    out[offset + 0] = static_cast<u8>(value & 0xffu);
    out[offset + 1] = static_cast<u8>((value >> 8) & 0xffu);
  };
  auto writeU32 = [&](size_t offset, u32 value) {
    out[offset + 0] = static_cast<u8>(value & 0xffu);
    out[offset + 1] = static_cast<u8>((value >> 8) & 0xffu);
    out[offset + 2] = static_cast<u8>((value >> 16) & 0xffu);
    out[offset + 3] = static_cast<u8>((value >> 24) & 0xffu);
  };
  auto writeI32 = [&](size_t offset, i32 value) { writeU32(offset, static_cast<u32>(value)); };

  out[0] = 'B';
  out[1] = 'M';
  writeU32(2, fileSize);
  writeU32(10, 14 + 40);
  writeU32(14, 40);
  writeI32(18, static_cast<i32>(width));
  writeI32(22, -static_cast<i32>(height));
  writeU16(26, 1);
  writeU16(28, 32);
  writeU32(30, 0);
  writeU32(34, imageSize);
  writeI32(38, 2835);
  writeI32(42, 2835);

  size_t dstOffset = 14 + 40;
  for (u32 y = 0; y < height; ++y) {
    const u8* srcRow = bytes.data() + static_cast<size_t>(y) * pitch;
    for (u32 x = 0; x < width; ++x) {
      const auto color = decodeBmpColor(format, srcRow + x * srcBytesPerPixel);
      out[dstOffset++] = color.b;
      out[dstOffset++] = color.g;
      out[dstOffset++] = color.r;
      out[dstOffset++] = color.a;
    }
  }

  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  std::ofstream stream(path, std::ios::binary);
  if (!stream) {
    return false;
  }
  stream.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
  return stream.good();
}

}  // namespace dxmt9::core::metalcapture
