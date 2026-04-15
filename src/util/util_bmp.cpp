#include "util_bmp.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

namespace dxmt9::util {

namespace {

using namespace dxmt9::core;

u32 clampToByte(float value) {
  value = std::clamp(value, 0.0f, 1.0f);
  return static_cast<u32>(std::lround(value * 255.0f)) & 0xffu;
}

u32 bmpBytesPerPixel(Format format) {
  switch (format) {
    case Format::A8R8G8B8:
    case Format::X8R8G8B8:
    case Format::A8B8G8R8:
    case Format::X8B8G8R8:
    case Format::A2R10G10B10:
    case Format::A2B10G10R10:
      return 4;
    case Format::R5G6B5:
    case Format::A1R5G5B5:
    case Format::X1R5G5B5:
    case Format::A4R4G4B4:
    case Format::A8L8:
      return 2;
    case Format::A8:
    case Format::L8:
      return 1;
    default:
      return 0;
  }
}

ColorRGBA decodeBmpColor(Format format, const u8* src) {
  switch (format) {
    case Format::A8R8G8B8:
    case Format::X8R8G8B8:
      return {src[2] / 255.0f, src[1] / 255.0f, src[0] / 255.0f,
              format == Format::X8R8G8B8 ? 1.0f : src[3] / 255.0f};
    case Format::A8B8G8R8:
    case Format::X8B8G8R8:
      return {src[0] / 255.0f, src[1] / 255.0f, src[2] / 255.0f,
              format == Format::X8B8G8R8 ? 1.0f : src[3] / 255.0f};
    case Format::R5G6B5: {
      const u16 v = std::bit_cast<u16>(std::array<u8, 2>{src[0], src[1]});
      return {((v >> 11) & 0x1f) / 31.0f, ((v >> 5) & 0x3f) / 63.0f, (v & 0x1f) / 31.0f, 1.0f};
    }
    case Format::A1R5G5B5:
    case Format::X1R5G5B5: {
      const u16 v = std::bit_cast<u16>(std::array<u8, 2>{src[0], src[1]});
      return {((v >> 10) & 0x1f) / 31.0f, ((v >> 5) & 0x1f) / 31.0f, (v & 0x1f) / 31.0f,
              format == Format::X1R5G5B5 ? 1.0f : ((v >> 15) & 1u) ? 1.0f : 0.0f};
    }
    case Format::A4R4G4B4: {
      const u16 v = std::bit_cast<u16>(std::array<u8, 2>{src[0], src[1]});
      return {((v >> 8) & 0xf) / 15.0f, ((v >> 4) & 0xf) / 15.0f, (v & 0xf) / 15.0f,
              ((v >> 12) & 0xf) / 15.0f};
    }
    case Format::A8:
      return {0.0f, 0.0f, 0.0f, src[0] / 255.0f};
    case Format::L8: {
      const float l = src[0] / 255.0f;
      return {l, l, l, 1.0f};
    }
    case Format::A8L8:
      return {src[0] / 255.0f, src[0] / 255.0f, src[0] / 255.0f, src[1] / 255.0f};
    case Format::A2R10G10B10: {
      const u32 v = std::bit_cast<u32>(std::array<u8, 4>{src[0], src[1], src[2], src[3]});
      return {((v >> 20) & 0x3ff) / 1023.0f, ((v >> 10) & 0x3ff) / 1023.0f,
              (v & 0x3ff) / 1023.0f, ((v >> 30) & 0x3) / 3.0f};
    }
    case Format::A2B10G10R10: {
      const u32 v = std::bit_cast<u32>(std::array<u8, 4>{src[0], src[1], src[2], src[3]});
      return {(v & 0x3ff) / 1023.0f, ((v >> 10) & 0x3ff) / 1023.0f,
              ((v >> 20) & 0x3ff) / 1023.0f, ((v >> 30) & 0x3) / 3.0f};
    }
    default:
      return {};
  }
}

}  // namespace

bool writeBmp(const std::string& path, Format format, u32 width, u32 height, u32 pitch,
              std::span<const u8> bytes) {
  if (path.empty() || width == 0 || height == 0 || pitch == 0) {
    return false;
  }
  const u32 srcBytesPerPixel = bmpBytesPerPixel(format);
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
      out[dstOffset++] = static_cast<u8>(clampToByte(color.b));
      out[dstOffset++] = static_cast<u8>(clampToByte(color.g));
      out[dstOffset++] = static_cast<u8>(clampToByte(color.r));
      out[dstOffset++] = static_cast<u8>(clampToByte(color.a));
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

}  // namespace dxmt9::util
