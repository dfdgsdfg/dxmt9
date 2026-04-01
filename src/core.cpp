#include "dxmt9/core.hpp"
#include "dxmt9/assert.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <optional>
#include <sstream>

namespace dxmt9::core {

namespace {

constexpr u64 kFnvOffset = 1469598103934665603ull;
constexpr u64 kFnvPrime = 1099511628211ull;

u64 hashCombine(u64 seed, u64 value) {
  seed ^= value;
  seed *= kFnvPrime;
  return seed;
}

template <typename T>
u64 hashTrivial(const T& value) {
  static_assert(std::is_trivially_copyable_v<T>);
  const auto* bytes = reinterpret_cast<const std::byte*>(std::addressof(value));
  u64 hash = kFnvOffset;
  for (size_t i = 0; i < sizeof(T); ++i) {
    hash ^= static_cast<u64>(bytes[i]);
    hash *= kFnvPrime;
  }
  return hash;
}

u32 clampToByte(float value) {
  value = std::clamp(value, 0.0f, 1.0f);
  return static_cast<u32>(std::lround(value * 255.0f)) & 0xffu;
}

std::optional<u32> parseEnvU32(const char* name) {
  const char* value = std::getenv(name);
  if (!value || *value == '\0') {
    return std::nullopt;
  }
  char* end = nullptr;
  const auto parsed = std::strtoul(value, &end, 10);
  if (!end || *end != '\0') {
    return std::nullopt;
  }
  return static_cast<u32>(parsed);
}

std::optional<u32> parseEnvU32Auto(const char* name) {
  const char* value = std::getenv(name);
  if (!value || *value == '\0') {
    return std::nullopt;
  }
  char* end = nullptr;
  const auto parsed = std::strtoull(value, &end, 0);
  if (!end || *end != '\0') {
    return std::nullopt;
  }
  return static_cast<u32>(parsed);
}

std::string getenvString(const char* name) {
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
}

bool renderTraceEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_TRACE_RENDER");
    return env != nullptr && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

void emitRenderTrace(const char* fmt, ...) {
  if (!renderTraceEnabled()) {
    return;
  }
  std::fputs("[dxmt9-render] ", stderr);
  va_list args;
  va_start(args, fmt);
  std::vfprintf(stderr, fmt, args);
  va_end(args);
  std::fputc('\n', stderr);
  std::fflush(stderr);
}

u32 clampToBits(float value, u32 bits) {
  value = std::clamp(value, 0.0f, 1.0f);
  const u32 maxValue = (1u << bits) - 1u;
  return static_cast<u32>(std::lround(value * static_cast<float>(maxValue))) & maxValue;
}

u16 pack565(ColorRGBA c) {
  return static_cast<u16>((clampToBits(c.r, 5) << 11) | (clampToBits(c.g, 6) << 5) |
                          clampToBits(c.b, 5));
}

u16 pack1555(ColorRGBA c, bool forceAlpha) {
  const u16 a = static_cast<u16>(forceAlpha ? 1u : clampToBits(c.a, 1));
  return static_cast<u16>((a << 15) | (clampToBits(c.r, 5) << 10) |
                          (clampToBits(c.g, 5) << 5) | clampToBits(c.b, 5));
}

u16 pack4444(ColorRGBA c) {
  return static_cast<u16>((clampToBits(c.a, 4) << 12) | (clampToBits(c.r, 4) << 8) |
                          (clampToBits(c.g, 4) << 4) | clampToBits(c.b, 4));
}

u32 pack2101010(ColorRGBA c, bool bgraOrder) {
  const u32 a = clampToBits(c.a, 2);
  const u32 r = clampToBits(c.r, 10);
  const u32 g = clampToBits(c.g, 10);
  const u32 b = clampToBits(c.b, 10);
  if (bgraOrder) {
    return (a << 30) | (b << 20) | (g << 10) | r;
  }
  return (a << 30) | (r << 20) | (g << 10) | b;
}

ColorRGBA decodeColor(Format format, const u8* src) {
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

bool writeBmpScreenshot(const std::string& path, Format format, u32 width, u32 height, u32 pitch,
                        std::span<const u8> bytes) {
  if (path.empty() || width == 0 || height == 0 || pitch == 0) {
    return false;
  }
  const u32 srcBytesPerPixel = bytesPerPixel(format);
  if (srcBytesPerPixel == 0) {
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

  if (bytes.size() < static_cast<size_t>(pitch) * height) {
    return false;
  }

  size_t dstOffset = 14 + 40;
  for (u32 y = 0; y < height; ++y) {
    const u8* srcRow = bytes.data() + static_cast<size_t>(y) * pitch;
    for (u32 x = 0; x < width; ++x) {
      const u32 srcOffset = x * srcBytesPerPixel;
      const auto color = decodeColor(format, srcRow + srcOffset);
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

bool encodeColor(Format format, ColorRGBA c, u8* dst) {
  switch (format) {
    case Format::A8R8G8B8:
    case Format::X8R8G8B8:
      dst[0] = static_cast<u8>(clampToByte(c.b));
      dst[1] = static_cast<u8>(clampToByte(c.g));
      dst[2] = static_cast<u8>(clampToByte(c.r));
      dst[3] = format == Format::X8R8G8B8 ? 0xffu : static_cast<u8>(clampToByte(c.a));
      return true;
    case Format::A8B8G8R8:
    case Format::X8B8G8R8:
      dst[0] = static_cast<u8>(clampToByte(c.r));
      dst[1] = static_cast<u8>(clampToByte(c.g));
      dst[2] = static_cast<u8>(clampToByte(c.b));
      dst[3] = format == Format::X8B8G8R8 ? 0xffu : static_cast<u8>(clampToByte(c.a));
      return true;
    case Format::R5G6B5: {
      const u16 v = pack565(c);
      const auto raw = std::bit_cast<std::array<u8, 2>>(v);
      dst[0] = raw[0];
      dst[1] = raw[1];
      return true;
    }
    case Format::A1R5G5B5:
    case Format::X1R5G5B5: {
      const u16 v = pack1555(c, format == Format::X1R5G5B5);
      const auto raw = std::bit_cast<std::array<u8, 2>>(v);
      dst[0] = raw[0];
      dst[1] = raw[1];
      return true;
    }
    case Format::A4R4G4B4: {
      const u16 v = pack4444(c);
      const auto raw = std::bit_cast<std::array<u8, 2>>(v);
      dst[0] = raw[0];
      dst[1] = raw[1];
      return true;
    }
    case Format::A8:
      dst[0] = static_cast<u8>(clampToByte(c.a));
      return true;
    case Format::L8:
      dst[0] = static_cast<u8>(clampToByte((c.r + c.g + c.b) / 3.0f));
      return true;
    case Format::A8L8:
      dst[0] = static_cast<u8>(clampToByte((c.r + c.g + c.b) / 3.0f));
      dst[1] = static_cast<u8>(clampToByte(c.a));
      return true;
    case Format::A2R10G10B10: {
      const u32 v = pack2101010(c, false);
      const auto raw = std::bit_cast<std::array<u8, 4>>(v);
      std::memcpy(dst, raw.data(), 4);
      return true;
    }
    case Format::A2B10G10R10: {
      const u32 v = pack2101010(c, true);
      const auto raw = std::bit_cast<std::array<u8, 4>>(v);
      std::memcpy(dst, raw.data(), 4);
      return true;
    }
    case Format::R32F:
      std::memcpy(dst, &c.r, sizeof(float));
      return true;
    case Format::G32R32F:
    case Format::G16R16:
    case Format::V16U16:
    case Format::Q8W8V8U8:
      std::memcpy(dst, &c.r, std::min<size_t>(sizeof(float) * 2, 4));
      return true;
    default:
      return false;
  }
}

[[maybe_unused]] bool isColorRenderable(Format format) {
  switch (format) {
    case Format::A8R8G8B8:
    case Format::X8R8G8B8:
    case Format::A8B8G8R8:
    case Format::X8B8G8R8:
    case Format::R5G6B5:
    case Format::A1R5G5B5:
    case Format::X1R5G5B5:
    case Format::A4R4G4B4:
    case Format::A8:
    case Format::A16B16G16R16F:
    case Format::A32B32G32R32F:
    case Format::G16R16F:
    case Format::R16F:
    case Format::G32R32F:
    case Format::R32F:
    case Format::A16B16G16R16:
    case Format::G16R16:
    case Format::A2R10G10B10:
    case Format::A2B10G10R10:
    case Format::L8:
    case Format::L16:
    case Format::A8L8:
    case Format::V8U8:
    case Format::Q8W8V8U8:
    case Format::V16U16:
      return true;
    default:
      return false;
  }
}

bool isDepthFormat(Format format) {
  switch (format) {
    case Format::D24S8:
    case Format::D24X8:
    case Format::D16:
    case Format::D32:
    case Format::D32F_LOCKABLE:
    case Format::D16_LOCKABLE:
    case Format::D24FS8:
      return true;
    default:
      return false;
  }
}

bool isCompressedFormat(Format format) {
  switch (format) {
    case Format::DXT1:
    case Format::DXT2:
    case Format::DXT3:
    case Format::DXT4:
    case Format::DXT5:
    case Format::ATI1:
    case Format::BC4:
    case Format::ATI2:
    case Format::BC5:
      return true;
    default:
      return false;
  }
}

struct FormatEntry {
  FormatInfo info;
};

const std::vector<FormatEntry>& formatEntries() {
  static const std::vector<FormatEntry> entries = {
      {{Format::A8R8G8B8, BackendPixelFormat::BGRA8Unorm, FormatClass::Required, 4, true, false,
        false, true}},
      {{Format::X8R8G8B8, BackendPixelFormat::BGRA8Unorm, FormatClass::Required, 4, true, false,
        false, true}},
      {{Format::A8B8G8R8, BackendPixelFormat::RGBA8Unorm, FormatClass::Required, 4, true, false,
        false, true}},
      {{Format::X8B8G8R8, BackendPixelFormat::RGBA8Unorm, FormatClass::Required, 4, true, false,
        false, true}},
      {{Format::R5G6B5, BackendPixelFormat::B5G6R5Unorm, FormatClass::Required, 2, true, false,
        false, true}},
      {{Format::A1R5G5B5, BackendPixelFormat::BGR5A1Unorm, FormatClass::Required, 2, true, false,
        false, true}},
      {{Format::X1R5G5B5, BackendPixelFormat::BGR5A1Unorm, FormatClass::Required, 2, true, false,
        false, true}},
      {{Format::A4R4G4B4, BackendPixelFormat::ABGR4Unorm, FormatClass::Required, 2, true, false,
        false, true}},
      {{Format::A8, BackendPixelFormat::A8Unorm, FormatClass::Required, 1, true, false, false,
        true}},
      {{Format::R8G8B8, BackendPixelFormat::Unknown, FormatClass::Unsupported, 3, false, false,
        false, true}},
      {{Format::A16B16G16R16F, BackendPixelFormat::RGBA16Float, FormatClass::Required, 8, true,
        false, false, true}},
      {{Format::A32B32G32R32F, BackendPixelFormat::RGBA32Float, FormatClass::Required, 16, true,
        false, false, true}},
      {{Format::G16R16F, BackendPixelFormat::RG16Float, FormatClass::Required, 4, true, false,
        false, true}},
      {{Format::R16F, BackendPixelFormat::R16Float, FormatClass::Required, 2, true, false, false,
        true}},
      {{Format::G32R32F, BackendPixelFormat::RG32Float, FormatClass::Required, 8, true, false,
        false, true}},
      {{Format::R32F, BackendPixelFormat::R32Float, FormatClass::Required, 4, true, false, false,
        true}},
      {{Format::A16B16G16R16, BackendPixelFormat::RGBA16Unorm, FormatClass::Required, 8, true,
        false, false, true}},
      {{Format::G16R16, BackendPixelFormat::RG16Unorm, FormatClass::Required, 4, true, false,
        false, true}},
      {{Format::A2R10G10B10, BackendPixelFormat::RGB10A2Unorm, FormatClass::Required, 4, true,
        false, false, true}},
      {{Format::A2B10G10R10, BackendPixelFormat::BGR10A2Unorm, FormatClass::Optional, 4, true,
        false, false, true}},
      {{Format::L8, BackendPixelFormat::R8Unorm, FormatClass::Required, 1, false, false, false,
        true}},
      {{Format::L16, BackendPixelFormat::R16Unorm, FormatClass::Required, 2, false, false, false,
        true}},
      {{Format::A8L8, BackendPixelFormat::RG8Unorm, FormatClass::Required, 2, false, false, false,
        true}},
      {{Format::V8U8, BackendPixelFormat::RG8Snorm, FormatClass::Required, 2, true, false, false,
        true}},
      {{Format::Q8W8V8U8, BackendPixelFormat::RGBA8Snorm, FormatClass::Required, 4, true, false,
        false, true}},
      {{Format::V16U16, BackendPixelFormat::RG16Snorm, FormatClass::Required, 4, true, false,
        false, true}},
      {{Format::CxV8U8, BackendPixelFormat::Unknown, FormatClass::Unsupported, 0, false, false,
        false, true}},
      {{Format::DXT1, BackendPixelFormat::BC1_RGBA, FormatClass::Required, 0, false, false, true,
        true}},
      {{Format::DXT2, BackendPixelFormat::BC2_RGBA, FormatClass::Required, 0, false, false, true,
        true}},
      {{Format::DXT3, BackendPixelFormat::BC2_RGBA, FormatClass::Required, 0, false, false, true,
        true}},
      {{Format::DXT4, BackendPixelFormat::BC3_RGBA, FormatClass::Required, 0, false, false, true,
        true}},
      {{Format::DXT5, BackendPixelFormat::BC3_RGBA, FormatClass::Required, 0, false, false, true,
        true}},
      {{Format::ATI1, BackendPixelFormat::BC4_RUnorm, FormatClass::Required, 0, false, false, true,
        true}},
      {{Format::BC4, BackendPixelFormat::BC4_RUnorm, FormatClass::Required, 0, false, false, true,
        true}},
      {{Format::ATI2, BackendPixelFormat::BC5_RGUnorm, FormatClass::Required, 0, false, false, true,
        true}},
      {{Format::BC5, BackendPixelFormat::BC5_RGUnorm, FormatClass::Required, 0, false, false, true,
        true}},
      {{Format::D24S8, BackendPixelFormat::Depth24Unorm_Stencil8, FormatClass::Required, 4, false,
        true, false, true}},
      {{Format::D24X8, BackendPixelFormat::Depth24Unorm_Stencil8, FormatClass::Required, 4, false,
        true, false, true}},
      {{Format::D16, BackendPixelFormat::Depth16Unorm, FormatClass::Required, 2, false, true,
        false, true}},
      {{Format::D32, BackendPixelFormat::Depth32Float, FormatClass::Required, 4, false, true,
        false, true}},
      {{Format::D32F_LOCKABLE, BackendPixelFormat::Depth32Float, FormatClass::Required, 4, false,
        true, false, true}},
      {{Format::D16_LOCKABLE, BackendPixelFormat::Depth16Unorm, FormatClass::Required, 2, false,
        true, false, true}},
      {{Format::D15S1, BackendPixelFormat::Unknown, FormatClass::Unsupported, 0, false, false,
        false, true}},
      {{Format::D24X4S4, BackendPixelFormat::Unknown, FormatClass::Unsupported, 0, false, false,
        false, true}},
      {{Format::D24FS8, BackendPixelFormat::Depth32Float_Stencil8, FormatClass::Optional, 8, false,
        true, false, true}},
      {{Format::S8_LOCKABLE, BackendPixelFormat::Unknown, FormatClass::Unsupported, 1, false,
        false, false, true}},
      {{Format::INDEX16, BackendPixelFormat::Unknown, FormatClass::Required, 2, false, false,
        false, true}},
      {{Format::INDEX32, BackendPixelFormat::Unknown, FormatClass::Required, 4, false, false,
        false, true}},
  };
  return entries;
}

u64 hashMap(const std::unordered_map<u32, u32>& values) {
  u64 hash = kFnvOffset;
  std::vector<std::pair<u32, u32>> sorted(values.begin(), values.end());
  std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.first < b.first; });
  for (const auto& [key, value] : sorted) {
    hash = hashCombine(hash, key);
    hash = hashCombine(hash, value);
  }
  return hash;
}

u64 hashColor(const ColorRGBA& color) {
  return hashCombine(hashCombine(hashCombine(hashCombine(kFnvOffset, std::bit_cast<u32>(color.r)),
                                             std::bit_cast<u32>(color.g)),
                                 std::bit_cast<u32>(color.b)),
                      std::bit_cast<u32>(color.a));
}

u64 hashMatrix(const Matrix4x4& matrix) {
  u64 hash = kFnvOffset;
  for (auto value : matrix.m) {
    hash = hashCombine(hash, std::bit_cast<u32>(value));
  }
  return hash;
}

Matrix4x4 identityMatrix() {
  Matrix4x4 matrix{};
  matrix.m = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
              0.0f, 0.0f, 0.0f, 1.0f};
  return matrix;
}

Matrix4x4 multiplyMatrix(const Matrix4x4& left, const Matrix4x4& right) {
  Matrix4x4 result{};
  for (size_t row = 0; row < 4; ++row) {
    for (size_t col = 0; col < 4; ++col) {
      float sum = 0.0f;
      for (size_t k = 0; k < 4; ++k) {
        sum += left.m[row * 4 + k] * right.m[k * 4 + col];
      }
      result.m[row * 4 + col] = sum;
    }
  }
  return result;
}

Matrix4x4 transposeMatrix(const Matrix4x4& matrix) {
  Matrix4x4 result{};
  for (size_t row = 0; row < 4; ++row) {
    for (size_t col = 0; col < 4; ++col) {
      result.m[row * 4 + col] = matrix.m[col * 4 + row];
    }
  }
  return result;
}

bool invertMatrix(const Matrix4x4& matrix, Matrix4x4* out) {
  std::array<double, 32> aug{};
  for (size_t row = 0; row < 4; ++row) {
    for (size_t col = 0; col < 4; ++col) {
      aug[row * 8 + col] = matrix.m[row * 4 + col];
      aug[row * 8 + 4 + col] = (row == col) ? 1.0 : 0.0;
    }
  }

  for (size_t col = 0; col < 4; ++col) {
    size_t pivotRow = col;
    double pivot = std::fabs(aug[pivotRow * 8 + col]);
    for (size_t row = col + 1; row < 4; ++row) {
      const double candidate = std::fabs(aug[row * 8 + col]);
      if (candidate > pivot) {
        pivot = candidate;
        pivotRow = row;
      }
    }
    if (pivot < 1.0e-20) {
      return false;
    }
    if (pivotRow != col) {
      for (size_t i = 0; i < 8; ++i) {
        std::swap(aug[col * 8 + i], aug[pivotRow * 8 + i]);
      }
    }
    const double invPivot = 1.0 / aug[col * 8 + col];
    for (size_t i = 0; i < 8; ++i) {
      aug[col * 8 + i] *= invPivot;
    }
    for (size_t row = 0; row < 4; ++row) {
      if (row == col) {
        continue;
      }
      const double factor = aug[row * 8 + col];
      if (factor == 0.0) {
        continue;
      }
      for (size_t i = 0; i < 8; ++i) {
        aug[row * 8 + i] -= factor * aug[col * 8 + i];
      }
    }
  }

  Matrix4x4 inverse{};
  for (size_t row = 0; row < 4; ++row) {
    for (size_t col = 0; col < 4; ++col) {
      inverse.m[row * 4 + col] = static_cast<float>(aug[row * 8 + 4 + col]);
    }
  }
  *out = inverse;
  return true;
}

ClipPlane transformClipPlane(const Matrix4x4& transform, const ClipPlane& plane) {
  Matrix4x4 inverse{};
  if (!invertMatrix(transform, &inverse)) {
    return plane;
  }
  const Matrix4x4 inverseTranspose = transposeMatrix(inverse);
  ClipPlane out{};
  for (size_t row = 0; row < 4; ++row) {
    float sum = 0.0f;
    for (size_t col = 0; col < 4; ++col) {
      sum += inverseTranspose.m[row * 4 + col] * plane[col];
    }
    out[row] = sum;
  }
  return out;
}

Matrix4x4 lookupTransform(const DeviceState& state, u32 key) {
  if (auto it = state.transforms.find(key); it != state.transforms.end()) {
    return it->second;
  }
  return identityMatrix();
}

u64 hashLight(const Light& light) {
  u64 hash = kFnvOffset;
  hash = hashCombine(hash, static_cast<u64>(light.type));
  hash = hashCombine(hash, static_cast<u64>(light.enabled));
  hash = hashCombine(hash, hashColor(light.diffuse));
  hash = hashCombine(hash, hashColor(light.specular));
  hash = hashCombine(hash, hashColor(light.ambient));
  for (float v : light.position) {
    hash = hashCombine(hash, std::bit_cast<u32>(v));
  }
  for (float v : light.direction) {
    hash = hashCombine(hash, std::bit_cast<u32>(v));
  }
  hash = hashCombine(hash, std::bit_cast<u32>(light.range));
  hash = hashCombine(hash, std::bit_cast<u32>(light.falloff));
  hash = hashCombine(hash, std::bit_cast<u32>(light.attenuation0));
  hash = hashCombine(hash, std::bit_cast<u32>(light.attenuation1));
  hash = hashCombine(hash, std::bit_cast<u32>(light.attenuation2));
  hash = hashCombine(hash, std::bit_cast<u32>(light.theta));
  hash = hashCombine(hash, std::bit_cast<u32>(light.phi));
  return hash;
}

u64 hashMaterial(const Material& material) {
  u64 hash = kFnvOffset;
  hash = hashCombine(hash, hashColor(material.emissive));
  hash = hashCombine(hash, hashColor(material.ambient));
  hash = hashCombine(hash, hashColor(material.diffuse));
  hash = hashCombine(hash, hashColor(material.specular));
  hash = hashCombine(hash, std::bit_cast<u32>(material.power));
  return hash;
}

u64 hashFfpVertexKey(const FfpVertexKey& key) {
  u64 hash = kFnvOffset;
  hash = hashCombine(hash, static_cast<u64>(key.lightingEnabled));
  hash = hashCombine(hash, static_cast<u64>(key.specularEnabled));
  hash = hashCombine(hash, static_cast<u64>(key.normalizeNormals));
  for (bool enabled : key.lightEnabled) {
    hash = hashCombine(hash, static_cast<u64>(enabled));
  }
  for (u32 type : key.lightType) {
    hash = hashCombine(hash, type);
  }
  for (u32 mode : key.colorMaterialMode) {
    hash = hashCombine(hash, mode);
  }
  hash = hashCombine(hash, static_cast<u64>(key.fogMode));
  hash = hashCombine(hash, static_cast<u64>(key.fogFromVertex));
  hash = hashCombine(hash, static_cast<u64>(key.rangeFog));
  for (u32 value : key.texCoordGen) {
    hash = hashCombine(hash, value);
  }
  for (u32 value : key.texTransformFlags) {
    hash = hashCombine(hash, value);
  }
  hash = hashCombine(hash, key.vertexBlend);
  hash = hashCombine(hash, static_cast<u64>(key.indexedVertexBlend));
  hash = hashCombine(hash, key.clipPlaneMask);
  return hash;
}

u64 hashFfpPixelKey(const FfpPixelKey& key) {
  u64 hash = kFnvOffset;
  for (const auto& stage : key.stages) {
    hash = hashCombine(hash, stage.colorOp);
    hash = hashCombine(hash, stage.colorArg1);
    hash = hashCombine(hash, stage.colorArg2);
    hash = hashCombine(hash, stage.alphaOp);
    hash = hashCombine(hash, stage.alphaArg1);
    hash = hashCombine(hash, stage.alphaArg2);
    hash = hashCombine(hash, stage.resultArg);
    hash = hashCombine(hash, stage.texType);
    hash = hashCombine(hash, stage.texCoordIndex);
  }
  hash = hashCombine(hash, static_cast<u64>(key.fogMode));
  hash = hashCombine(hash, static_cast<u64>(key.alphaTestEnable));
  hash = hashCombine(hash, key.alphaTestFunc);
  return hash;
}

u64 hashShaderBytecode(const ShaderBytecode& bytecode) {
  if (bytecode.hash != 0) {
    return bytecode.hash;
  }
  return hashBytes(std::as_bytes(std::span<const u8>(bytecode.bytes.data(), bytecode.bytes.size())));
}

u64 hashShaderRef(const ShaderRef& ref) {
  switch (ref.kind) {
    case ShaderRef::Kind::Bytecode:
      return ref.hash != 0 ? ref.hash : hashShaderBytecode(ref.bytecode);
    case ShaderRef::Kind::FixedFunctionVertex:
      return ref.vertexKey ? (ref.vertexKey->hash ? ref.vertexKey->hash : hashFfpVertexKey(*ref.vertexKey))
                           : 0;
    case ShaderRef::Kind::FixedFunctionPixel:
      return ref.pixelKey ? (ref.pixelKey->hash ? ref.pixelKey->hash : hashFfpPixelKey(*ref.pixelKey))
                          : 0;
    case ShaderRef::Kind::None:
      return 0;
  }
  return 0;
}

[[maybe_unused]] u64 hashStateState(const DeviceState& state) {
  u64 hash = kFnvOffset;
  hash = hashCombine(hash, std::bit_cast<u32>(state.viewport.x));
  hash = hashCombine(hash, std::bit_cast<u32>(state.viewport.y));
  hash = hashCombine(hash, std::bit_cast<u32>(state.viewport.width));
  hash = hashCombine(hash, std::bit_cast<u32>(state.viewport.height));
  hash = hashCombine(hash, std::bit_cast<u32>(state.viewport.minZ));
  hash = hashCombine(hash, std::bit_cast<u32>(state.viewport.maxZ));
  hash = hashCombine(hash, static_cast<u64>(state.scissorEnabled));
  hash = hashCombine(hash, state.scissorRect.left);
  hash = hashCombine(hash, state.scissorRect.top);
  hash = hashCombine(hash, state.scissorRect.right);
  hash = hashCombine(hash, state.scissorRect.bottom);
  hash = hashCombine(hash, hashMap(state.renderStates));
  for (const auto& stage : state.textureStageStates) {
    hash = hashCombine(hash, hashMap(stage));
  }
  for (const auto& sampler : state.samplerStates) {
    hash = hashCombine(hash, hashMap(sampler));
  }
  for (const auto& transform : state.transforms) {
    hash = hashCombine(hash, hashMatrix(transform.second));
  }
  for (const auto& clipPlane : state.clipPlanes) {
    for (float value : clipPlane) {
      hash = hashCombine(hash, std::bit_cast<u32>(value));
    }
  }
  for (const auto& light : state.lights) {
    hash = hashCombine(hash, hashLight(light));
  }
  for (bool enabled : state.lightEnabled) {
    hash = hashCombine(hash, static_cast<u64>(enabled));
  }
  hash = hashCombine(hash, hashMaterial(state.material));
  hash = hashCombine(hash, state.fvf);
  hash = hashCombine(hash, hashShaderRef(state.vertexShader));
  hash = hashCombine(hash, hashShaderRef(state.pixelShader));
  hash = hashCombine(hash, state.indexBuffer ? state.indexBuffer->handle().value : 0);
  hash = hashCombine(hash, static_cast<u64>(state.indexType == IndexType::UInt32));
  for (const auto& tex : state.textures) {
    hash = hashCombine(hash, tex ? tex->handle().value : 0);
  }
  for (const auto& rt : state.renderTargets) {
    hash = hashCombine(hash, rt.handle.value);
    hash = hashCombine(hash, rt.level);
  }
  hash = hashCombine(hash, state.depthStencil.handle.value);
  hash = hashCombine(hash, state.depthStencil.level);
  hash = hashCombine(hash, static_cast<u64>(state.inScene));
  return hash;
}

[[maybe_unused]] u32 compareLimitToUsage(const BackendLimits& limits, Format format, u32 usage) {
  (void)limits;
  (void)format;
  (void)usage;
  return 0;
}

[[maybe_unused]] std::shared_ptr<const std::vector<u8>> cloneBytes(std::span<const u8> bytes) {
  return std::make_shared<std::vector<u8>>(bytes.begin(), bytes.end());
}

struct NullBackendDevice final : BackendDevice {
  BufferHandle createBuffer(const BufferDesc&) override {
    return Handle{++next_};
  }

  TextureHandle createTexture(const TextureDesc&) override {
    return Handle{++next_};
  }

  void destroyBuffer(BufferHandle) override {}
  void destroyTexture(TextureHandle) override {}
  void submitDraw(const DrawDesc&) override {}
  void submitClear(const ClearDesc&) override {}
  void present(const SwapDesc&) override {}

 private:
  u64 next_ = 1000;
};

u32 pitchForFormat(Format format, u32 width) {
  const u32 bpp = bytesPerPixel(format);
  return bpp == 0 ? 0 : bpp * width;
}

[[maybe_unused]] bool isSupportedDataFormat(Format format) {
  return bytesPerPixel(format) != 0 && !isCompressedFormat(format) && !isDepthFormat(format);
}

void fillBuffer(std::vector<u8>& bytes, u32 pitch, u32 width, u32 height, Format format,
                const Rect* rect, ColorRGBA color) {
  if (!encodeColor(format, color, bytes.data())) {
    return;
  }

  const i32 left = rect ? std::max(0, rect->left) : 0;
  const i32 top = rect ? std::max(0, rect->top) : 0;
  const i32 right = rect ? std::min<i32>(width, rect->right) : static_cast<i32>(width);
  const i32 bottom = rect ? std::min<i32>(height, rect->bottom) : static_cast<i32>(height);
  const u32 bpp = bytesPerPixel(format);
  std::vector<u8> pixel(bpp);
  if (!encodeColor(format, color, pixel.data())) {
    return;
  }
  for (i32 y = top; y < bottom; ++y) {
    u8* row = bytes.data() + static_cast<size_t>(y) * pitch;
    for (i32 x = left; x < right; ++x) {
      std::memcpy(row + static_cast<size_t>(x) * bpp, pixel.data(), bpp);
    }
  }
}

bool copyPixels(std::vector<u8>& dst, u32 dstPitch, u32 dstWidth, u32 dstHeight, Format dstFormat,
                const std::vector<u8>& src, u32 srcPitch, u32 srcWidth, u32 srcHeight,
                Format srcFormat) {
  const u32 dstBpp = bytesPerPixel(dstFormat);
  const u32 srcBpp = bytesPerPixel(srcFormat);
  if (dstBpp == 0 || srcBpp == 0) {
    return false;
  }
  const u32 width = std::min(dstWidth, srcWidth);
  const u32 height = std::min(dstHeight, srcHeight);
  std::vector<u8> temp(dstBpp);
  for (u32 y = 0; y < height; ++y) {
    const u8* srcRow = src.data() + static_cast<size_t>(y) * srcPitch;
    u8* dstRow = dst.data() + static_cast<size_t>(y) * dstPitch;
    for (u32 x = 0; x < width; ++x) {
      const u8* srcPx = srcRow + static_cast<size_t>(x) * srcBpp;
      ColorRGBA color = decodeColor(srcFormat, srcPx);
      if (!encodeColor(dstFormat, color, temp.data())) {
        return false;
      }
      std::memcpy(dstRow + static_cast<size_t>(x) * dstBpp, temp.data(), dstBpp);
    }
  }
  return true;
}

[[maybe_unused]] bool stretchPixels(std::vector<u8>& dst, u32 dstPitch, u32 dstWidth, u32 dstHeight, Format dstFormat,
                   const std::vector<u8>& src, u32 srcPitch, u32 srcWidth, u32 srcHeight,
                   Format srcFormat) {
  const u32 dstBpp = bytesPerPixel(dstFormat);
  const u32 srcBpp = bytesPerPixel(srcFormat);
  if (dstBpp == 0 || srcBpp == 0) {
    return false;
  }
  std::vector<u8> temp(dstBpp);
  for (u32 y = 0; y < dstHeight; ++y) {
    const u32 srcY = srcHeight == 0 ? 0 : (y * srcHeight) / dstHeight;
    const u8* srcRow = src.data() + static_cast<size_t>(srcY) * srcPitch;
    u8* dstRow = dst.data() + static_cast<size_t>(y) * dstPitch;
    for (u32 x = 0; x < dstWidth; ++x) {
      const u32 srcX = srcWidth == 0 ? 0 : (x * srcWidth) / dstWidth;
      const u8* srcPx = srcRow + static_cast<size_t>(srcX) * srcBpp;
      ColorRGBA color = decodeColor(srcFormat, srcPx);
      if (!encodeColor(dstFormat, color, temp.data())) {
        return false;
      }
      std::memcpy(dstRow + static_cast<size_t>(x) * dstBpp, temp.data(), dstBpp);
    }
  }
  return true;
}

void fillDepthStencil(std::vector<u8>& bytes, u32 pitch, u32 width, u32 height, Format format,
                      const Rect* rect, bool clearDepth, f32 depth, bool clearStencil, u32 stencil) {
  if (width == 0 || height == 0) {
    return;
  }

  const i32 left = rect ? std::max(0, rect->left) : 0;
  const i32 top = rect ? std::max(0, rect->top) : 0;
  const i32 right = rect ? std::min<i32>(width, rect->right) : static_cast<i32>(width);
  const i32 bottom = rect ? std::min<i32>(height, rect->bottom) : static_cast<i32>(height);
  const u32 bpp = bytesPerPixel(format);
  if (bpp == 0) {
    return;
  }

  const auto encodeDepth24 = [](f32 value) -> u32 {
    const auto clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<u32>(std::lround(clamped * 16777215.0f)) & 0x00ffffffu;
  };

  const auto readDepth24 = [](const u8* px) -> f32 {
    const u32 raw = static_cast<u32>(px[0]) | (static_cast<u32>(px[1]) << 8) |
                    (static_cast<u32>(px[2]) << 16);
    return static_cast<f32>(raw) / 16777215.0f;
  };

  for (i32 y = top; y < bottom; ++y) {
    u8* row = bytes.data() + static_cast<size_t>(y) * pitch;
    for (i32 x = left; x < right; ++x) {
      u8* px = row + static_cast<size_t>(x) * bpp;
      f32 currentDepth = 0.0f;
      u32 currentStencil = 0;
      switch (format) {
        case Format::D16:
        case Format::D16_LOCKABLE: {
          const u16 raw = std::bit_cast<u16>(std::array<u8, 2>{px[0], px[1]});
          currentDepth = static_cast<f32>(raw) / 65535.0f;
          break;
        }
        case Format::D32:
        case Format::D32F_LOCKABLE: {
          const u32 raw = std::bit_cast<u32>(std::array<u8, 4>{px[0], px[1], px[2], px[3]});
          currentDepth = std::bit_cast<f32>(raw);
          break;
        }
        case Format::D24S8: {
          currentDepth = readDepth24(px);
          currentStencil = px[3];
          break;
        }
        case Format::D24X8: {
          currentDepth = readDepth24(px);
          break;
        }
        case Format::D24FS8: {
          const u32 raw = std::bit_cast<u32>(std::array<u8, 4>{px[0], px[1], px[2], px[3]});
          currentDepth = std::bit_cast<f32>(raw);
          currentStencil = px[4];
          break;
        }
        case Format::S8_LOCKABLE:
          currentStencil = px[0];
          break;
        default:
          break;
      }

      if (clearDepth) {
        currentDepth = depth;
      }
      if (clearStencil) {
        currentStencil = stencil & 0xffu;
      }

      switch (format) {
        case Format::D16:
        case Format::D16_LOCKABLE: {
          const u16 raw = static_cast<u16>(std::lround(std::clamp(currentDepth, 0.0f, 1.0f) * 65535.0f));
          const auto bytes16 = std::bit_cast<std::array<u8, 2>>(raw);
          px[0] = bytes16[0];
          px[1] = bytes16[1];
          break;
        }
        case Format::D32:
        case Format::D32F_LOCKABLE: {
          const u32 raw = std::bit_cast<u32>(currentDepth);
          const auto bytes32 = std::bit_cast<std::array<u8, 4>>(raw);
          px[0] = bytes32[0];
          px[1] = bytes32[1];
          px[2] = bytes32[2];
          px[3] = bytes32[3];
          break;
        }
        case Format::D24S8: {
          const u32 raw = encodeDepth24(currentDepth);
          px[0] = static_cast<u8>(raw & 0xffu);
          px[1] = static_cast<u8>((raw >> 8) & 0xffu);
          px[2] = static_cast<u8>((raw >> 16) & 0xffu);
          px[3] = static_cast<u8>(currentStencil);
          break;
        }
        case Format::D24X8: {
          const u32 raw = encodeDepth24(currentDepth);
          px[0] = static_cast<u8>(raw & 0xffu);
          px[1] = static_cast<u8>((raw >> 8) & 0xffu);
          px[2] = static_cast<u8>((raw >> 16) & 0xffu);
          px[3] = 0;
          break;
        }
        case Format::D24FS8: {
          const u32 raw = std::bit_cast<u32>(currentDepth);
          const auto bytes32 = std::bit_cast<std::array<u8, 4>>(raw);
          px[0] = bytes32[0];
          px[1] = bytes32[1];
          px[2] = bytes32[2];
          px[3] = bytes32[3];
          px[4] = static_cast<u8>(currentStencil);
          px[5] = 0;
          px[6] = 0;
          px[7] = 0;
          break;
        }
        case Format::S8_LOCKABLE:
          px[0] = static_cast<u8>(currentStencil);
          break;
        default:
          break;
      }
    }
  }
}

}  // namespace

const std::vector<FormatInfo>& formatTable() {
  static const std::vector<FormatInfo> table = [] {
    std::vector<FormatInfo> out;
    out.reserve(formatEntries().size());
    for (const auto& entry : formatEntries()) {
      out.push_back(entry.info);
    }
    return out;
  }();
  return table;
}

const FormatInfo* findFormatInfo(Format format) {
  for (const auto& entry : formatTable()) {
    if (entry.format == format) {
      return &entry;
    }
  }
  return nullptr;
}

FormatClass classifyFormat(Format format) {
  if (const auto* info = findFormatInfo(format)) {
    return info->support;
  }
  return FormatClass::Unsupported;
}

BackendPixelFormat backendPixelFormat(Format format) {
  if (const auto* info = findFormatInfo(format)) {
    return info->backendFormat;
  }
  return BackendPixelFormat::Unknown;
}

u32 bytesPerPixel(Format format) {
  if (const auto* info = findFormatInfo(format)) {
    return info->bytesPerPixel;
  }
  return 0;
}

std::string formatName(Format format) {
  switch (format) {
    case Format::Unknown:
      return "Unknown";
    case Format::A8R8G8B8:
      return "A8R8G8B8";
    case Format::X8R8G8B8:
      return "X8R8G8B8";
    case Format::A8B8G8R8:
      return "A8B8G8R8";
    case Format::X8B8G8R8:
      return "X8B8G8R8";
    case Format::R5G6B5:
      return "R5G6B5";
    case Format::A1R5G5B5:
      return "A1R5G5B5";
    case Format::X1R5G5B5:
      return "X1R5G5B5";
    case Format::A4R4G4B4:
      return "A4R4G4B4";
    case Format::A8:
      return "A8";
    case Format::R8G8B8:
      return "R8G8B8";
    case Format::A16B16G16R16F:
      return "A16B16G16R16F";
    case Format::A32B32G32R32F:
      return "A32B32G32R32F";
    case Format::G16R16F:
      return "G16R16F";
    case Format::R16F:
      return "R16F";
    case Format::G32R32F:
      return "G32R32F";
    case Format::R32F:
      return "R32F";
    case Format::A16B16G16R16:
      return "A16B16G16R16";
    case Format::G16R16:
      return "G16R16";
    case Format::A2R10G10B10:
      return "A2R10G10B10";
    case Format::A2B10G10R10:
      return "A2B10G10R10";
    case Format::L8:
      return "L8";
    case Format::L16:
      return "L16";
    case Format::A8L8:
      return "A8L8";
    case Format::V8U8:
      return "V8U8";
    case Format::Q8W8V8U8:
      return "Q8W8V8U8";
    case Format::V16U16:
      return "V16U16";
    case Format::CxV8U8:
      return "CxV8U8";
    case Format::DXT1:
      return "DXT1";
    case Format::DXT2:
      return "DXT2";
    case Format::DXT3:
      return "DXT3";
    case Format::DXT4:
      return "DXT4";
    case Format::DXT5:
      return "DXT5";
    case Format::ATI1:
      return "ATI1";
    case Format::BC4:
      return "BC4";
    case Format::ATI2:
      return "ATI2";
    case Format::BC5:
      return "BC5";
    case Format::D24S8:
      return "D24S8";
    case Format::D24X8:
      return "D24X8";
    case Format::D16:
      return "D16";
    case Format::D32:
      return "D32";
    case Format::D32F_LOCKABLE:
      return "D32F_LOCKABLE";
    case Format::D16_LOCKABLE:
      return "D16_LOCKABLE";
    case Format::D15S1:
      return "D15S1";
    case Format::D24X4S4:
      return "D24X4S4";
    case Format::D24FS8:
      return "D24FS8";
    case Format::S8_LOCKABLE:
      return "S8_LOCKABLE";
    case Format::INDEX16:
      return "INDEX16";
    case Format::INDEX32:
      return "INDEX32";
  }
  return "Unknown";
}

std::string backendFormatName(BackendPixelFormat format) {
  switch (format) {
    case BackendPixelFormat::Unknown:
      return "Unknown";
    case BackendPixelFormat::BGRA8Unorm:
      return "BGRA8Unorm";
    case BackendPixelFormat::RGBA8Unorm:
      return "RGBA8Unorm";
    case BackendPixelFormat::B5G6R5Unorm:
      return "B5G6R5Unorm";
    case BackendPixelFormat::BGR5A1Unorm:
      return "BGR5A1Unorm";
    case BackendPixelFormat::ABGR4Unorm:
      return "ABGR4Unorm";
    case BackendPixelFormat::A8Unorm:
      return "A8Unorm";
    case BackendPixelFormat::RGBA16Float:
      return "RGBA16Float";
    case BackendPixelFormat::RGBA32Float:
      return "RGBA32Float";
    case BackendPixelFormat::RG16Float:
      return "RG16Float";
    case BackendPixelFormat::R16Float:
      return "R16Float";
    case BackendPixelFormat::RG32Float:
      return "RG32Float";
    case BackendPixelFormat::R32Float:
      return "R32Float";
    case BackendPixelFormat::RGBA16Unorm:
      return "RGBA16Unorm";
    case BackendPixelFormat::RG16Unorm:
      return "RG16Unorm";
    case BackendPixelFormat::RGB10A2Unorm:
      return "RGB10A2Unorm";
    case BackendPixelFormat::BGR10A2Unorm:
      return "BGR10A2Unorm";
    case BackendPixelFormat::R8Unorm:
      return "R8Unorm";
    case BackendPixelFormat::R16Unorm:
      return "R16Unorm";
    case BackendPixelFormat::RG8Unorm:
      return "RG8Unorm";
    case BackendPixelFormat::RG8Snorm:
      return "RG8Snorm";
    case BackendPixelFormat::RGBA8Snorm:
      return "RGBA8Snorm";
    case BackendPixelFormat::RG16Snorm:
      return "RG16Snorm";
    case BackendPixelFormat::BC1_RGBA:
      return "BC1_RGBA";
    case BackendPixelFormat::BC2_RGBA:
      return "BC2_RGBA";
    case BackendPixelFormat::BC3_RGBA:
      return "BC3_RGBA";
    case BackendPixelFormat::BC4_RUnorm:
      return "BC4_RUnorm";
    case BackendPixelFormat::BC5_RGUnorm:
      return "BC5_RGUnorm";
    case BackendPixelFormat::Depth24Unorm_Stencil8:
      return "Depth24Unorm_Stencil8";
    case BackendPixelFormat::Depth32Float:
      return "Depth32Float";
    case BackendPixelFormat::Depth32Float_Stencil8:
      return "Depth32Float_Stencil8";
    case BackendPixelFormat::Depth16Unorm:
      return "Depth16Unorm";
  }
  return "Unknown";
}

bool formatSupportsUsage(Format format, u32 usage, const BackendLimits& limits) {
  const auto* info = findFormatInfo(format);
  if (!info || info->support == FormatClass::Unsupported) {
    return false;
  }

  if (format == Format::A2B10G10R10 && !limits.supportsBgr10A2) {
    return false;
  }

  if ((usage & UsageRenderTarget) != 0) {
    if (!info->renderTarget || info->depthStencil || info->compressed) {
      return false;
    }
  }

  if ((usage & UsageDepthStencil) != 0) {
    if (!info->depthStencil) {
      return format == Format::D24S8 || format == Format::D24X8 || format == Format::D16 ||
             format == Format::D32 || format == Format::D32F_LOCKABLE ||
             format == Format::D16_LOCKABLE || format == Format::D24FS8;
    }
    if (format == Format::D24FS8 && !limits.supportsDepth32FloatStencil8) {
      return false;
    }
  }

  if (info->support == FormatClass::Optional) {
    if (format == Format::A2B10G10R10) {
      return limits.supportsBgr10A2;
    }
    if (format == Format::D24FS8) {
      return limits.supportsDepth32FloatStencil8;
    }
  }

  if (isCompressedFormat(format) && ((usage & UsageRenderTarget) != 0 ||
                                     (usage & UsageDepthStencil) != 0)) {
    return false;
  }

  if (format == Format::R8G8B8 && usage != 0) {
    return false;
  }

  return true;
}

bool isDisplayModeFormat(Format format) {
  const auto* info = findFormatInfo(format);
  return info && info->renderTarget && !info->depthStencil && !info->compressed;
}

std::vector<DisplayMode> makeAdapterModes(Format format, const BackendLimits& limits) {
  if (!isDisplayModeFormat(format) || !formatSupportsUsage(format, UsageRenderTarget, limits)) {
    return {};
  }

  constexpr std::array<std::pair<u32, u32>, 5> kCommonModes = {
      std::pair{640u, 480u},
      std::pair{800u, 600u},
      std::pair{1024u, 768u},
      std::pair{1280u, 720u},
      std::pair{1920u, 1080u},
  };

  std::vector<DisplayMode> modes;
  for (const auto& [width, height] : kCommonModes) {
    if (width > limits.maxTextureSize || height > limits.maxTextureSize) {
      continue;
    }
    modes.push_back({width, height, 60, format});
  }
  return modes;
}

PresentParameters normalizePresentParameters(const AdapterInfo& adapter, PresentParameters params) {
  if (params.backBufferFormat == Format::Unknown) {
    params.backBufferFormat = adapter.displayMode.format;
  }
  if (!params.windowed) {
    if (params.backBufferWidth == 0) {
      params.backBufferWidth = adapter.displayMode.width;
    }
    if (params.backBufferHeight == 0) {
      params.backBufferHeight = adapter.displayMode.height;
    }
  } else {
    params.backBufferWidth = std::max(1u, params.backBufferWidth);
    params.backBufferHeight = std::max(1u, params.backBufferHeight);
  }
  return params;
}

SwapDesc makeSwapDesc(const PresentParameters& params) {
  SwapDesc desc;
  desc.window = params.deviceWindow;
  desc.width = params.backBufferWidth;
  desc.height = params.backBufferHeight;
  desc.format = params.backBufferFormat;
  desc.interval = params.presentationInterval;
  desc.windowed = params.windowed;
  desc.displaySyncEnabled = params.presentationInterval != PresentInterval::Immediate;
  desc.multiSampleType = params.multiSampleType;
  return desc;
}

u64 hashBytes(std::span<const std::byte> bytes) {
  u64 hash = kFnvOffset;
  for (const auto byte : bytes) {
    hash ^= static_cast<u64>(std::to_integer<unsigned char>(byte));
    hash *= kFnvPrime;
  }
  return hash;
}

u64 hashString(std::string_view text) {
  return hashBytes(std::as_bytes(std::span<const char>(text.data(), text.size())));
}

DeviceCaps makeDefaultCaps(const BackendLimits& limits) {
  constexpr u32 kCaps = 0x00000000u;
  constexpr u32 kCaps2 = 0x20000u | 0x40000000u | 0x20000000u;
  constexpr u32 kCaps3 = 0x00000020u | 0x00000100u | 0x00000200u;
  constexpr u32 kCursorCaps = 0x00000001u | 0x00000002u;
  constexpr u32 kPrimitiveMiscCaps = 0x002ecff2u;
  constexpr u32 kRasterCaps = 0x07332191u;
  constexpr u32 kCmpCaps = 0x000000ffu;
  constexpr u32 kShadeCaps = 0x00000008u | 0x00000200u | 0x00004000u | 0x00080000u;
  constexpr u32 kTextureCaps = 0x0001ec85u;
  constexpr u32 kFilterCaps = 0x07030700u;
  constexpr u32 kCubeFilterCaps = 0x07030700u;
  constexpr u32 kVolumeFilterCaps = 0x03030300u;
  constexpr u32 kStretchRectFilterCaps = 0x03000300u;
  constexpr u32 kAddressCaps = 0x0000001fu;
  constexpr u32 kStencilCaps = 0x00000001u | 0x00000002u | 0x00000004u |
                               0x00000008u | 0x00000010u | 0x00000020u |
                               0x00000040u | 0x00000080u | 0x00000100u;
  constexpr u32 kSrcBlendCaps = 0x00003fffu;
  constexpr u32 kDestBlendCaps = 0x000027ffu;
  constexpr u32 kTextureOpCaps = 0x03feffffu;
  constexpr u32 kVertexProcessingCaps = 0x0000013bu;
  constexpr u32 kDeclTypes = 0x0000030fu;
  constexpr u32 kFvfCaps = 0x00100008u;
  constexpr u32 kLineCaps = 0x0000001fu;
  constexpr u32 kDevCaps = 0x0019aff0u;
  constexpr u32 kDevCaps2 = 0x00000001u | 0x00000010u | 0x00000040u;

  DeviceCaps caps;
  caps.maxTextureWidth = std::min(16384u, limits.maxTextureSize);
  caps.maxTextureHeight = std::min(16384u, limits.maxTextureSize);
  caps.maxRenderTargetWidth = caps.maxTextureWidth;
  caps.maxRenderTargetHeight = caps.maxTextureHeight;
  caps.maxAnisotropy = limits.maxAnisotropy;
  caps.numSimultaneousRTs = std::min(kMaxRenderTargets, limits.maxColorAttachments);
  caps.maxVertexShaderConst = kMaxVertexConstants;
  caps.maxSimultaneousTextures = 8;
  caps.maxActiveLights = kMaxLights;
  caps.maxStreams = kMaxStreams;
  caps.vertexShaderVersion = 0xfffe0300u;
  caps.pixelShaderVersion = 0xffff0300u;
  caps.caps = kCaps;
  caps.caps2 = kCaps2;
  caps.caps3 = kCaps3;
  caps.presentationIntervals = 0x80000000u | 0x00000001u;
  caps.cursorCaps = kCursorCaps;
  caps.primitiveMiscCaps = kPrimitiveMiscCaps;
  caps.textureCaps = kTextureCaps;
  caps.textureFilterCaps = kFilterCaps;
  caps.cubetextureFilterCaps = kCubeFilterCaps;
  caps.volumeTextureFilterCaps = kVolumeFilterCaps;
  caps.rasterCaps = kRasterCaps;
  caps.zCmpCaps = kCmpCaps;
  caps.alphaCmpCaps = kCmpCaps;
  caps.shadeCaps = kShadeCaps;
  caps.stencilCaps = kStencilCaps;
  caps.srcBlendCaps = kSrcBlendCaps;
  caps.destBlendCaps = kDestBlendCaps;
  caps.alphaBlendCaps = kCmpCaps;
  caps.textureBlendCaps = kTextureOpCaps;
  caps.textureAddressCaps = kAddressCaps;
  caps.volumeTextureAddressCaps = kAddressCaps;
  caps.lineCaps = kLineCaps;
  caps.fvfCaps = kFvfCaps;
  caps.vertexProcessingCaps = kVertexProcessingCaps;
  caps.devCaps = kDevCaps;
  caps.devCaps2 = kDevCaps2;
  caps.declTypes = kDeclTypes;
  caps.stretchRectFilterCaps = kStretchRectFilterCaps;
  caps.vs20Caps = 0x00000001u;
  caps.ps20Caps = 0x0000001fu;
  caps.maxTextureRepeat = 32768;
  caps.maxTextureAspectRatio = 16384;
  caps.maxUserClipPlanes = 8;
  caps.maxPointSize = 64.0f;
  caps.maxPrimitiveCount = 5592405;
  caps.maxStreamStride = 1024;
  caps.maxVertexBlendMatrixIndex = 0;
  caps.pixelShader1xMaxValue = 1024.0f;
  caps.vertexTextureFilterCaps = 0x01000100u;
  caps.maxVShaderInstructionsExecuted = 65535;
  caps.maxPShaderInstructionsExecuted = 65535;
  caps.maxVertexShader30InstructionSlots = 512;
  caps.maxPixelShader30InstructionSlots = 512;
  return caps;
}

std::array<f32, 2> halfPixelFixup(const Viewport& viewport) {
  if (viewport.width == 0 || viewport.height == 0) {
    return {0.0f, 0.0f};
  }
  return {1.0f / static_cast<f32>(viewport.width), 1.0f / static_cast<f32>(viewport.height)};
}

std::vector<u32> decomposeTriangleFanIndices(std::span<const u32> indices) {
  std::vector<u32> out;
  if (indices.size() < 3) {
    return out;
  }
  out.reserve((indices.size() - 2) * 3);
  for (size_t i = 1; i + 1 < indices.size(); ++i) {
    out.push_back(indices[0]);
    out.push_back(indices[i]);
    out.push_back(indices[i + 1]);
  }
  return out;
}

std::vector<u8> convertTextureUpload(Format format, u32 width, u32 height, std::span<const u8> input) {
  const u32 bpp = bytesPerPixel(format);
  if (bpp == 0) {
    return {};
  }
  std::vector<u8> output(static_cast<size_t>(width) * height * bpp);
  const u32 srcPitch = pitchForFormat(format, width);
  if (input.size() < output.size()) {
    return {};
  }
  if (!copyPixels(output, srcPitch, width, height, format, std::vector<u8>(input.begin(), input.end()),
                  srcPitch, width, height, format)) {
    // Fall back to a raw copy when the format is not color-decodable.
    std::copy_n(input.begin(), std::min(output.size(), input.size()), output.begin());
  }
  return output;
}

void DeviceState::reset() {
  viewport = {};
  scissorRect = {};
  scissorEnabled = false;
  renderStates.clear();
  for (auto& stage : textureStageStates) {
    stage.clear();
  }
  for (auto& sampler : samplerStates) {
    sampler.clear();
  }
  transforms.clear();
  lights = {};
  lightEnabled.fill(false);
  material = {};
  streamBuffers.fill(nullptr);
  streamOffsets.fill(0);
  streamStrides.fill(0);
  indexBuffer.reset();
  indexType = IndexType::UInt16;
  vertexDecl = {};
  fvf = 0;
  vertexShader = {};
  pixelShader = {};
  vsConst = {};
  psConst = {};
  clipPlanes = {};
  textures.fill(nullptr);
  renderTargets = {};
  depthStencil = {};
  inScene = false;

  renderStates[RS_LIGHTING] = 1;
  renderStates[RS_SPECULAR_ENABLE] = 0;
  renderStates[RS_NORMALIZE_NORMALS] = 0;
  renderStates[RS_FOG_TABLE_MODE] = static_cast<u32>(FogMode::None);
  renderStates[RS_FOG_FROM_VERTEX] = 1;
  renderStates[RS_RANGE_FOG] = 0;
  renderStates[RS_ALPHA_TEST_ENABLE] = 0;
  renderStates[RS_ALPHA_FUNC] = static_cast<u32>(CompareFunc::Always);
  renderStates[RS_ALPHA_REF] = 0;
  renderStates[RS_FOG_ENABLE] = 0;
  renderStates[RS_FOG_COLOR] = 0;
  renderStates[RS_FOG_START] = std::bit_cast<u32>(1.0f);
  renderStates[RS_FOG_END] = std::bit_cast<u32>(1.0f);
  renderStates[RS_FOG_DENSITY] = std::bit_cast<u32>(1.0f);
  renderStates[RS_AMBIENT] = 0;
  renderStates[RS_DIFFUSE_MATERIAL_SOURCE] = 1;
  renderStates[RS_SPECULAR_MATERIAL_SOURCE] = 2;
  renderStates[RS_AMBIENT_MATERIAL_SOURCE] = 0;
  renderStates[RS_EMISSIVE_MATERIAL_SOURCE] = 0;
  renderStates[RS_VERTEX_BLEND] = 0;
  renderStates[RS_CLIP_PLANE_ENABLE] = 0;
  renderStates[RS_POINT_SPRITE_ENABLE] = 0;
  renderStates[RS_POINT_SCALE_ENABLE] = 0;
  renderStates[RS_CULL_MODE] = static_cast<u32>(CullMode::Ccw);
  renderStates[RS_Z_WRITE_ENABLE] = 1;
  renderStates[RS_Z_FUNC] = static_cast<u32>(CompareFunc::LessEqual);
  renderStates[RS_SRC_BLEND] = static_cast<u32>(BlendFactor::One);
  renderStates[RS_DEST_BLEND] = static_cast<u32>(BlendFactor::Zero);
  renderStates[RS_BLEND_OP] = static_cast<u32>(BlendOp::Add);
  renderStates[RS_COLOR_WRITE_ENABLE] = 0xf;
  renderStates[RS_Z_ENABLE] = 1;
  renderStates[RS_ALPHABLEND_ENABLE] = 0;
  renderStates[RS_BLEND_FACTOR] = 0xffffffffu;
  renderStates[RS_SEPARATE_ALPHA_BLEND_ENABLE] = 0;
  renderStates[RS_SRC_BLEND_ALPHA] = static_cast<u32>(BlendFactor::One);
  renderStates[RS_DEST_BLEND_ALPHA] = static_cast<u32>(BlendFactor::Zero);
  renderStates[RS_BLEND_OP_ALPHA] = static_cast<u32>(BlendOp::Add);
  renderStates[RS_STENCIL_ENABLE] = 0;
  renderStates[RS_STENCIL_FUNC] = static_cast<u32>(CompareFunc::Always);
  renderStates[RS_STENCIL_FAIL] = static_cast<u32>(StencilOp::Keep);
  renderStates[RS_STENCIL_ZFAIL] = static_cast<u32>(StencilOp::Keep);
  renderStates[RS_STENCIL_PASS] = static_cast<u32>(StencilOp::Keep);
  renderStates[RS_STENCIL_REF] = 0;
  renderStates[RS_STENCIL_MASK] = 0xffu;
  renderStates[RS_STENCIL_WRITEMASK] = 0xffu;
  renderStates[RS_STENCIL_CCW_FUNC] = static_cast<u32>(CompareFunc::Always);
  renderStates[RS_STENCIL_CCW_FAIL] = static_cast<u32>(StencilOp::Keep);
  renderStates[RS_STENCIL_CCW_ZFAIL] = static_cast<u32>(StencilOp::Keep);
  renderStates[RS_STENCIL_CCW_PASS] = static_cast<u32>(StencilOp::Keep);
  renderStates[RS_STENCIL_CCW_REF] = 0;
  renderStates[RS_STENCIL_CCW_MASK] = 0xffu;
  renderStates[RS_STENCIL_CCW_WRITEMASK] = 0xffu;

  for (auto& stage : textureStageStates) {
    stage[TSS_COLOR_OP] = static_cast<u32>(TextureOp::Disable);
    stage[TSS_COLOR_ARG1] = 0;
    stage[TSS_COLOR_ARG2] = 0;
    stage[TSS_ALPHA_OP] = static_cast<u32>(TextureOp::Disable);
    stage[TSS_ALPHA_ARG1] = 0;
    stage[TSS_ALPHA_ARG2] = 0;
    stage[TSS_RESULT_ARG] = 0;
    stage[TSS_TEXCOORD_INDEX] = 0;
    stage[TSS_TEXTURE_TRANSFORM_FLAGS] = 0;
    stage[TSS_TEXTURE_TYPE] = 0;
  }

  for (auto& sampler : samplerStates) {
    sampler[SAMP_MIN_FILTER] = 0;
    sampler[SAMP_MAG_FILTER] = 0;
    sampler[SAMP_MIP_FILTER] = 0;
    sampler[SAMP_ADDRESS_U] = 0;
    sampler[SAMP_ADDRESS_V] = 0;
    sampler[SAMP_ADDRESS_W] = 0;
    sampler[SAMP_MAX_ANISOTROPY] = 1;
    sampler[SAMP_MIPMAP_LOD_BIAS] = 0;
  }

  for (auto& transform : transforms) {
    Matrix4x4 identity{};
    identity.m = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                  0.0f, 0.0f, 0.0f, 1.0f};
    transform.second = identity;
  }
  material.diffuse = {1.0f, 1.0f, 1.0f, 1.0f};
}

Buffer::Buffer(std::shared_ptr<Device> owner, BufferHandle handle, BufferDesc desc)
    : owner_(std::move(owner)), handle_(handle), desc_(desc), storage_(static_cast<size_t>(desc.size)) {
  if (auto ownerPtr = owner_.lock()) {
    backend_ = ownerPtr->backend();
  }
}

Buffer::~Buffer() {
  invalidate();
}

LockedRegion Buffer::lock(u64 offset, u64 size, u32 flags) {
  if (!valid_) {
    return {};
  }
  if ((flags & UsageDiscard) != 0 && (desc_.usage & UsageDynamic) != 0) {
    storage_.assign(static_cast<size_t>(std::max<u64>(size, desc_.size)), 0);
    offset = 0;
  } else if (storage_.size() < offset + size) {
    storage_.resize(static_cast<size_t>(offset + size));
  }
  if (backend_ && handle_) {
    backend_->mapBuffer(handle_, flags);
  }
  locked_ = true;
  return {storage_.data() + offset, static_cast<u32>(size)};
}

void Buffer::unlock() {
  if (backend_ && handle_) {
    backend_->uploadBufferData(handle_, storage_);
    backend_->unmapBuffer(handle_);
  }
  locked_ = false;
}

void Buffer::invalidate() {
  if (!valid_) {
    return;
  }
  valid_ = false;
  if (backend_ && handle_) {
    backend_->destroyBuffer(handle_);
  }
  handle_ = {};
}

Texture::Texture(std::shared_ptr<Device> owner, TextureHandle handle, TextureDesc desc)
    : owner_(std::move(owner)), handle_(handle), desc_(desc) {
  if (auto ownerPtr = owner_.lock()) {
    backend_ = ownerPtr->backend();
  }
  const u32 bpp = bytesPerPixel(desc_.format);
  const u32 levels = std::max(1u, desc_.levels);
  levels_.resize(levels);
  for (u32 level = 0; level < levels; ++level) {
    LevelStorage storage;
    storage.width = std::max(1u, desc_.width >> level);
    storage.height = std::max(1u, desc_.height >> level);
    storage.pitch = bpp * storage.width;
    storage.bytes.resize(static_cast<size_t>(storage.pitch) * storage.height, 0);
    levels_[level] = std::move(storage);
  }
}

Texture::~Texture() {
  invalidate();
}

LockedRegion Texture::lockRect(u32 level, const Rect* rect, u32 flags) {
  if (!valid_ || level >= levels_.size()) {
    return {};
  }
  LevelStorage& storage = levels_[level];
  if ((flags & UsageDiscard) != 0) {
    storage.bytes.assign(static_cast<size_t>(storage.pitch) * storage.height, 0);
  }
  locked_ = true;
  const u32 bpp = bytesPerPixel(desc_.format);
  const u32 left = rect ? std::max(0, rect->left) : 0;
  const u32 top = rect ? std::max(0, rect->top) : 0;
  return {storage.bytes.data() + static_cast<size_t>(top) * storage.pitch + static_cast<size_t>(left) * bpp,
          storage.pitch};
}

void Texture::unlockRect(u32 level) {
  if (level < levels_.size()) {
    levels_[level].dirty = true;
    syncLevelToBackend(level);
  }
  locked_ = false;
}

std::shared_ptr<Surface> Texture::surfaceLevel(u32 level) {
  if (level >= levels_.size()) {
    return {};
  }
  if (level < surfaces_.size()) {
    if (auto surface = surfaces_[level].lock()) {
      return surface;
    }
  } else {
    surfaces_.resize(level + 1);
  }
  auto owner = owner_.lock();
  if (!owner) {
    return {};
  }
  SurfaceDesc surfaceDesc;
  surfaceDesc.width = std::max(1u, desc_.width >> level);
  surfaceDesc.height = std::max(1u, desc_.height >> level);
  surfaceDesc.format = desc_.format;
  surfaceDesc.pool = desc_.pool;
  surfaceDesc.usage = desc_.usage;
  surfaceDesc.renderTarget = (desc_.usage & UsageRenderTarget) != 0;
  surfaceDesc.depthStencil = (desc_.usage & UsageDepthStencil) != 0;
  auto surfaceHandle = backend_ ? backend_->createSurfaceForTexture(handle_, level, surfaceDesc) : SurfaceHandle{};
  if (!surfaceHandle && backend_) {
    surfaceHandle = backend_->createSurface(surfaceDesc);
  }
  if (!surfaceHandle) {
    surfaceHandle = Handle{owner->nextHandle_++};
  }
  auto surface = std::make_shared<Surface>(owner, surfaceHandle, shared_from_this(), level);
  surfaces_[level] = surface;
  return surface;
}

std::span<const u8> Texture::levelBytes(u32 level) const {
  if (level >= levels_.size()) {
    return {};
  }
  const auto& storage = levels_[level];
  return std::span<const u8>(storage.bytes.data(), storage.bytes.size());
}

void Texture::fillColor(const Rect* rect, ColorRGBA color) {
  if (!valid_ || levels_.empty()) {
    return;
  }
  auto& storage = levels_[0];
  fillBuffer(storage.bytes, storage.pitch, storage.width, storage.height, desc_.format, rect, color);
  storage.dirty = true;
  syncLevelToBackend(0);
}

void Texture::fillColor(u32 level, const Rect* rect, ColorRGBA color) {
  if (!valid_ || level >= levels_.size()) {
    return;
  }
  auto& storage = levels_[level];
  fillBuffer(storage.bytes, storage.pitch, storage.width, storage.height, desc_.format, rect, color);
  storage.dirty = true;
  syncLevelToBackend(level);
}

void Texture::copyFrom(const Texture& src) {
  if (!valid_ || !src.valid_ || desc_.format != src.desc_.format) {
    return;
  }
  const size_t levels = std::min(levels_.size(), src.levels_.size());
  for (size_t i = 0; i < levels; ++i) {
    levels_[i].bytes = src.levels_[i].bytes;
    levels_[i].dirty = true;
    syncLevelToBackend(static_cast<u32>(i));
  }
}

void Texture::syncLevelToBackend(u32 level) {
  if (!valid_ || !backend_ || !handle_ || level >= levels_.size()) {
    return;
  }
  const auto& storage = levels_[level];
  if (storage.bytes.empty() || storage.width == 0 || storage.height == 0 || storage.pitch == 0) {
    return;
  }
  backend_->uploadTextureLevel(handle_, level, storage.width, storage.height, storage.pitch,
                               std::span<const u8>(storage.bytes.data(), storage.bytes.size()));
}

void Texture::invalidate() {
  if (!valid_) {
    return;
  }
  valid_ = false;
  if (backend_ && handle_) {
    backend_->destroyTexture(handle_);
  }
  handle_ = {};
}

Surface::Surface(std::shared_ptr<Device> owner, SurfaceHandle handle, SurfaceDesc desc)
    : owner_(std::move(owner)), handle_(handle), desc_(desc), containerKind_(ContainerKind::Device) {
  if (auto ownerPtr = owner_.lock()) {
    backend_ = ownerPtr->backend();
  }
  if (desc_.width != 0 && desc_.height != 0) {
    standalonePitch_ = bytesPerPixel(desc_.format) * desc_.width;
    standaloneBytes_.resize(static_cast<size_t>(standalonePitch_) * desc_.height, 0);
  }
}

Surface::Surface(std::shared_ptr<Device> owner, SurfaceHandle handle, std::shared_ptr<Texture> texture,
                 u32 level)
    : owner_(std::move(owner)), textureContainer_(std::move(texture)), handle_(handle), level_(level),
      containerKind_(ContainerKind::Texture) {
  if (auto ownerPtr = owner_.lock()) {
    backend_ = ownerPtr->backend();
  }
  if (auto tex = textureContainer_.lock()) {
    desc_.width = std::max(1u, tex->desc().width >> level_);
    desc_.height = std::max(1u, tex->desc().height >> level_);
    desc_.format = tex->desc().format;
    desc_.pool = tex->desc().pool;
    desc_.usage = tex->desc().usage;
    desc_.renderTarget = (tex->desc().usage & UsageRenderTarget) != 0;
    desc_.depthStencil = (tex->desc().usage & UsageDepthStencil) != 0;
  }
}

Surface::~Surface() {
  invalidate();
}

LockedRegion Surface::lockRect(const Rect* rect, u32 flags) {
  if (!valid_) {
    return {};
  }
  if (containerKind_ == ContainerKind::Texture) {
    if (auto tex = textureContainer_.lock()) {
      return tex->lockRect(level_, rect, flags);
    }
    return {};
  }
  if ((flags & UsageDiscard) != 0) {
    standaloneBytes_.assign(static_cast<size_t>(standalonePitch_) * desc_.height, 0);
  }
  locked_ = true;
  const u32 bpp = bytesPerPixel(desc_.format);
  const u32 left = rect ? std::max(0, rect->left) : 0;
  const u32 top = rect ? std::max(0, rect->top) : 0;
  return {standaloneBytes_.data() + static_cast<size_t>(top) * standalonePitch_ + static_cast<size_t>(left) * bpp,
          standalonePitch_};
}

void Surface::unlockRect() {
  if (containerKind_ == ContainerKind::Texture) {
    if (auto tex = textureContainer_.lock()) {
      tex->unlockRect(level_);
    }
  }
  locked_ = false;
}

void Surface::fillColor(const Rect* rect, ColorRGBA color) {
  if (!valid_) {
    return;
  }
  if (containerKind_ == ContainerKind::Texture) {
    if (auto tex = textureContainer_.lock()) {
      tex->fillColor(level_, rect, color);
    }
    return;
  }
  fillBuffer(standaloneBytes_, standalonePitch_, desc_.width, desc_.height, desc_.format, rect, color);
}

void Surface::copyFrom(const Surface& src) {
  if (!valid_ || !src.valid_ || desc_.format != src.desc_.format) {
    return;
  }
  if (containerKind_ == ContainerKind::Texture) {
    if (auto tex = textureContainer_.lock()) {
      if (src.containerKind_ == ContainerKind::Texture) {
        if (auto srcTex = src.textureContainer_.lock()) {
          tex->copyFrom(*srcTex);
        }
      }
    }
    return;
  }
  if (src.containerKind_ == ContainerKind::Texture) {
    if (auto srcTex = src.textureContainer_.lock()) {
      if (!srcTex->levelBytes(src.level_).empty()) {
        const auto bytes = srcTex->levelBytes(src.level_);
        const size_t count = std::min(bytes.size(), standaloneBytes_.size());
        std::copy_n(bytes.begin(), count, standaloneBytes_.begin());
      }
    }
    return;
  }
  const size_t count = std::min(standaloneBytes_.size(), src.standaloneBytes_.size());
  std::copy_n(src.standaloneBytes_.begin(), count, standaloneBytes_.begin());
}

void Surface::invalidate() {
  valid_ = false;
  if (backend_ && handle_) {
    backend_->destroySurface(handle_);
  }
  handle_ = {};
}

Query::Query(QueryType type) : type_(type) {
  if (type_ == QueryType::TimestampFreq) {
    resolvedValue_ = 1000000000ull;
  } else if (type_ == QueryType::TimestampDisjoint) {
    resolvedValue_ = 0ull;
  }
}

void Query::begin(u64 sequenceId) {
  active_ = true;
  issuedSequenceId_ = sequenceId;
  resolvedValue_.reset();
}

void Query::end(u64 sequenceId) {
  active_ = false;
  issuedSequenceId_ = sequenceId;
}

void Query::resolve(u64 value) {
  resolvedValue_ = value;
}

HRESULT Query::getData(void* output, size_t size, u32 flags, u64 completedSequenceId) const {
  if (type_ == QueryType::TimestampFreq) {
    if (output && size >= sizeof(u64)) {
      *static_cast<u64*>(output) = 1000000000ull;
    }
    return S_OK;
  }
  if (type_ == QueryType::TimestampDisjoint) {
    if (output && size >= sizeof(u64)) {
      *static_cast<u64*>(output) = 0ull;
    }
    return S_OK;
  }

  if (completedSequenceId < issuedSequenceId_) {
    if ((flags & QUERY_GETDATA_FLUSH) != 0) {
      return S_FALSE;
    }
    return S_FALSE;
  }

  // QueryResolutionSafety: a resolved query is only reported once the GPU has
  // completed the chunk that issued it.
  DXMT_ASSERT(completedSequenceId >= issuedSequenceId_);

  if (type_ == QueryType::Event) {
    return S_OK;
  }

  const u64 value = resolvedValue_.value_or(0ull);
  if (type_ == QueryType::Occlusion) {
    if (!output || size < sizeof(u32)) {
      return D3DERR_INVALIDCALL;
    }
    *static_cast<u32*>(output) = static_cast<u32>(std::min<u64>(value, std::numeric_limits<u32>::max()));
    return S_OK;
  }
  if (type_ == QueryType::Timestamp) {
    if (!output || size < sizeof(u64)) {
      return D3DERR_INVALIDCALL;
    }
    *static_cast<u64*>(output) = value;
    return S_OK;
  }
  return D3DERR_NOTAVAILABLE;
}

void StateBlock::capture(const DeviceState& state) {
  snapshot_ = state;
}

void StateBlock::apply(Device& device) const {
  device.mutableState() = snapshot_;
}

SwapChain::SwapChain(std::shared_ptr<Device> owner, SwapChainHandle handle, PresentParameters params,
                     std::shared_ptr<Surface> backBuffer, std::shared_ptr<Surface> depthStencil)
    : owner_(std::move(owner)), handle_(handle), params_(params), backBuffer_(std::move(backBuffer)),
      depthStencilSurface_(std::move(depthStencil)) {}

SwapChain::~SwapChain() = default;

bool SwapChain::displaySyncEnabled() const noexcept {
  return params_.presentationInterval != PresentInterval::Immediate;
}

void SwapChain::resize(const PresentParameters& params) {
  params_ = params;
  auto owner = owner_.lock();
  if (!owner) {
    return;
  }

  const u32 width = std::max(1u, params_.backBufferWidth);
  const u32 height = std::max(1u, params_.backBufferHeight);
  backBuffer_ = owner->createSurface({width, height, params_.backBufferFormat, Pool::Default,
                                      UsageRenderTarget, true, false, params_.multiSampleType});
  if (params_.enableAutoDepthStencil) {
    depthStencilSurface_ = owner->createSurface({width, height, params_.autoDepthStencilFormat, Pool::Default,
                                                 UsageDepthStencil, false, true, params_.multiSampleType});
  } else {
    depthStencilSurface_.reset();
  }
}

HResult SwapChain::present(std::shared_ptr<BackendDevice> backend, const SwapDesc& desc) {
  if (backend) {
    backend->present(desc);
  }
  return D3D_OK;
}

Device::Device(AdapterInfo adapter, BackendLimits limits, std::shared_ptr<BackendDevice> backend,
               PresentParameters params, u32 behaviorFlags)
    : adapter_(std::move(adapter)), limits_(limits),
      caps_(makeDefaultCaps(limits_)), backend_(backend ? std::move(backend) : makeBackendDevice(limits_)),
      presentParameters_(normalizePresentParameters(adapter_, params)), behaviorFlags_(behaviorFlags) {
  state_.reset();
  const u32 width = std::max(1u, presentParameters_.backBufferWidth);
  const u32 height = std::max(1u, presentParameters_.backBufferHeight);
  state_.viewport = {0, 0, width, height, 0.0f, 1.0f};
  deviceLost_ = false;
  maximumFrameLatency_ = 3;
  if (backend_) {
    backend_->setMaxFrameLatency(maximumFrameLatency_);
  }
  experimentCapture_.path = getenvString("DXMT9_EXPERIMENT_CAPTURE_PATH");
  experimentCapture_.frame = parseEnvU32("DXMT9_EXPERIMENT_CAPTURE_FRAME").value_or(0);
}

Device::~Device() {
  if (backend_) {
    backend_->flush();
  }
  completeUpTo(submittedSequenceId_);
  // SeqIdSafety / drain-before-teardown: pending work is drained before the
  // default-pool resources are invalidated.
  DXMT_ASSERT(completedSequenceId_ == submittedSequenceId_);
  invalidateDefaultPoolResources();
}

HResult Device::testCooperativeLevel() const {
  return deviceLost_ ? D3DERR_DEVICELOST : D3D_OK;
}

HResult Device::checkDeviceState() const {
  if (deviceLost_) {
    return D3DERR_DEVICELOST;
  }
  if (presentOccluded_) {
    return S_PRESENT_OCCLUDED;
  }
  return D3D_OK;
}

std::shared_ptr<Buffer> Device::createBuffer(const BufferDesc& desc) {
  auto handle = backend_->createBuffer(desc);
  if (!handle) {
    handle = Handle{nextHandle_++};
  }
  auto buffer = std::make_shared<Buffer>(shared_from_this(), handle, desc);
  registerBuffer(buffer);
  return buffer;
}

std::shared_ptr<Texture> Device::createTexture(const TextureDesc& desc) {
  auto handle = backend_->createTexture(desc);
  if (!handle) {
    handle = Handle{nextHandle_++};
  }
  auto texture = std::make_shared<Texture>(shared_from_this(), handle, desc);
  registerTexture(texture);
  return texture;
}

std::shared_ptr<Surface> Device::createSurface(const SurfaceDesc& desc) {
  auto handle = backend_->createSurface(desc);
  if (!handle) {
    handle = Handle{nextHandle_++};
  }
  auto surface = std::make_shared<Surface>(shared_from_this(), handle, desc);
  registerSurface(surface);
  return surface;
}

std::shared_ptr<Query> Device::createQuery(QueryType type) {
  auto query = std::make_shared<Query>(type);
  queries_.push_back(query);
  return query;
}

std::shared_ptr<StateBlock> Device::createStateBlock() const {
  auto block = std::make_shared<StateBlock>();
  block->capture(state_);
  return block;
}

std::shared_ptr<StateBlock> Device::captureStateBlock() const {
  return createStateBlock();
}

HResult Device::applyStateBlock(const StateBlock& block) {
  block.apply(*this);
  return D3D_OK;
}

std::shared_ptr<SwapChain> Device::createAdditionalSwapChain(const PresentParameters& params) {
  const auto normalized = normalizePresentParameters(adapter_, params);
  auto backBuffer = createSurface({std::max(1u, normalized.backBufferWidth),
                                   std::max(1u, normalized.backBufferHeight), normalized.backBufferFormat,
                                   Pool::Default, UsageRenderTarget, true, false, normalized.multiSampleType});
  std::shared_ptr<Surface> depth;
  if (normalized.enableAutoDepthStencil) {
    depth = createSurface({std::max(1u, normalized.backBufferWidth),
                           std::max(1u, normalized.backBufferHeight), normalized.autoDepthStencilFormat,
                           Pool::Default, UsageDepthStencil, false, true, normalized.multiSampleType});
  }
  auto swapChain = std::make_shared<SwapChain>(shared_from_this(), Handle{nextHandle_++}, normalized, backBuffer,
                                               depth);
  swapChains_.push_back(swapChain);
  return swapChain;
}

void Device::initializeDefaultSwapChain() {
  if (!swapChains_.empty()) {
    return;
  }
  const u32 width = std::max(1u, presentParameters_.backBufferWidth);
  const u32 height = std::max(1u, presentParameters_.backBufferHeight);
  auto backBuffer = createSurface({width, height, presentParameters_.backBufferFormat, Pool::Default,
                                   UsageRenderTarget, true, false, presentParameters_.multiSampleType});
  std::shared_ptr<Surface> depth;
  if (presentParameters_.enableAutoDepthStencil) {
    depth = createSurface({width, height, presentParameters_.autoDepthStencilFormat, Pool::Default,
                           UsageDepthStencil, false, true, presentParameters_.multiSampleType});
  }
  swapChains_.push_back(std::make_shared<SwapChain>(shared_from_this(), Handle{nextHandle_++},
                                                    presentParameters_, backBuffer, depth));
  state_.renderTargets[0] = backBuffer ? RenderTargetAttachment{backBuffer->handle(), 0, backBuffer->multiSampleCount()}
                                       : RenderTargetAttachment{};
  state_.depthStencil = depth ? RenderTargetAttachment{depth->handle(), 0, depth->multiSampleCount()}
                              : RenderTargetAttachment{};
}

std::shared_ptr<SwapChain> Device::swapChain(size_t index) const {
  if (index >= swapChains_.size()) {
    return {};
  }
  return swapChains_[index];
}

HResult Device::setRenderState(u32 key, u32 value) {
  state_.renderStates[key] = value;
  return D3D_OK;
}

HResult Device::setRenderStateFloat(u32 key, f32 value) {
  state_.renderStates[key] = std::bit_cast<u32>(value);
  return D3D_OK;
}

u32 Device::getRenderState(u32 key) const {
  if (auto it = state_.renderStates.find(key); it != state_.renderStates.end()) {
    return it->second;
  }
  return 0;
}

f32 Device::getRenderStateFloat(u32 key, f32 defaultValue) const {
  if (auto it = state_.renderStates.find(key); it != state_.renderStates.end()) {
    return std::bit_cast<f32>(it->second);
  }
  return defaultValue;
}

HResult Device::setTextureStageState(u32 stage, u32 key, u32 value) {
  if (stage >= kMaxTextureStages) {
    return D3DERR_INVALIDCALL;
  }
  state_.textureStageStates[stage][key] = value;
  return D3D_OK;
}

u32 Device::getTextureStageState(u32 stage, u32 key) const {
  if (stage >= kMaxTextureStages) {
    return 0;
  }
  const auto& map = state_.textureStageStates[stage];
  if (auto it = map.find(key); it != map.end()) {
    return it->second;
  }
  return 0;
}

HResult Device::setSamplerState(u32 sampler, u32 key, u32 value) {
  if (sampler >= kMaxSamplers) {
    return D3DERR_INVALIDCALL;
  }
  state_.samplerStates[sampler][key] = value;
  return D3D_OK;
}

u32 Device::getSamplerState(u32 sampler, u32 key) const {
  if (sampler >= kMaxSamplers) {
    return 0;
  }
  const auto& map = state_.samplerStates[sampler];
  if (auto it = map.find(key); it != map.end()) {
    return it->second;
  }
  return 0;
}

HResult Device::setTransform(u32 key, const Matrix4x4& matrix) {
  state_.transforms[key] = matrix;
  return D3D_OK;
}

HResult Device::setLight(u32 index, const Light& light) {
  if (index >= kMaxLights) {
    return D3DERR_INVALIDCALL;
  }
  state_.lights[index] = light;
  return D3D_OK;
}

HResult Device::lightEnable(u32 index, bool enable) {
  if (index >= kMaxLights) {
    return D3DERR_INVALIDCALL;
  }
  state_.lightEnabled[index] = enable;
  state_.lights[index].enabled = enable;
  return D3D_OK;
}

HResult Device::setMaterial(const Material& material) {
  state_.material = material;
  return D3D_OK;
}

HResult Device::setTexture(u32 stage, std::shared_ptr<Texture> texture) {
  if (stage >= kMaxTextures) {
    return D3DERR_INVALIDCALL;
  }
  state_.textures[stage] = std::move(texture);
  return D3D_OK;
}

HResult Device::setStreamSource(u32 stream, std::shared_ptr<Buffer> buffer, u32 offset, u32 stride) {
  if (stream >= kMaxStreams) {
    return D3DERR_INVALIDCALL;
  }
  state_.streamBuffers[stream] = std::move(buffer);
  state_.streamOffsets[stream] = offset;
  state_.streamStrides[stream] = stride;
  return D3D_OK;
}

HResult Device::setIndices(std::shared_ptr<Buffer> buffer, IndexType indexType) {
  state_.indexBuffer = std::move(buffer);
  state_.indexType = indexType;
  return D3D_OK;
}

HResult Device::setFVF(u32 fvf) {
  state_.fvf = fvf;
  state_.vertexDecl.fvf = fvf;
  return D3D_OK;
}

HResult Device::setVertexDeclaration(std::vector<VertexElement> elements) {
  state_.vertexDecl.elements = std::move(elements);
  return D3D_OK;
}

HResult Device::setVertexShader(const ShaderRef& shader) {
  state_.vertexShader = shader;
  if (state_.vertexShader.hash == 0) {
    state_.vertexShader.hash = hashShaderRef(state_.vertexShader);
  }
  return D3D_OK;
}

HResult Device::setPixelShader(const ShaderRef& shader) {
  state_.pixelShader = shader;
  if (state_.pixelShader.hash == 0) {
    state_.pixelShader.hash = hashShaderRef(state_.pixelShader);
  }
  return D3D_OK;
}

HResult Device::setClipPlane(u32 index, const ClipPlane& plane) {
  if (index >= kMaxClipPlanes) {
    return D3DERR_INVALIDCALL;
  }
  state_.clipPlanes[index] = plane;
  return D3D_OK;
}

HResult Device::setViewport(const Viewport& viewport) {
  if (viewport.width == 0 || viewport.height == 0 || !std::isfinite(viewport.minZ) ||
      !std::isfinite(viewport.maxZ) || viewport.minZ < 0.0f || viewport.maxZ > 1.0f ||
      viewport.minZ > viewport.maxZ) {
    return D3DERR_INVALIDCALL;
  }
  state_.viewport = viewport;
  return D3D_OK;
}

HResult Device::setScissorRect(const Rect& rect) {
  state_.scissorRect = rect;
  state_.scissorEnabled = true;
  return D3D_OK;
}

HResult Device::setRenderTarget(u32 index, std::shared_ptr<Surface> surface) {
  if (index >= kMaxRenderTargets) {
    return D3DERR_INVALIDCALL;
  }
  state_.renderTargets[index] = surface ? RenderTargetAttachment{surface->handle(), surface->level(),
                                                                surface->multiSampleCount()}
                                        : RenderTargetAttachment{};
  return D3D_OK;
}

HResult Device::setDepthStencilSurface(std::shared_ptr<Surface> surface) {
  state_.depthStencil = surface ? RenderTargetAttachment{surface->handle(), surface->level(),
                                                         surface->multiSampleCount()}
                                : RenderTargetAttachment{};
  return D3D_OK;
}

HResult Device::beginScene() {
  if (state_.inScene) {
    return D3DERR_INVALIDCALL;
  }
  state_.inScene = true;
  inScene_ = true;
  return D3D_OK;
}

HResult Device::endScene() {
  if (!state_.inScene) {
    return D3DERR_INVALIDCALL;
  }
  state_.inScene = false;
  inScene_ = false;
  return D3D_OK;
}

ClearDesc Device::snapshotClearDesc(const ClearDesc& desc) const {
  ClearDesc snapshot = desc;
  if (snapshot.clearColor) {
    bool hasExplicitColor = false;
    for (const auto& attachment : snapshot.colorAttachments) {
      if (attachment.handle) {
        hasExplicitColor = true;
        break;
      }
    }
    if (!hasExplicitColor) {
      snapshot.colorAttachments = state_.renderTargets;
    }
  }
  if ((snapshot.clearDepth || snapshot.clearStencil) && !snapshot.depthStencil.handle) {
    snapshot.depthStencil = state_.depthStencil;
  }
  return snapshot;
}

SwapDesc Device::snapshotSwapDesc() const {
  SwapDesc desc;
  desc.window = presentParameters_.deviceWindow;
  desc.width = std::max(1u, presentParameters_.backBufferWidth);
  desc.height = std::max(1u, presentParameters_.backBufferHeight);
  desc.format = presentParameters_.backBufferFormat;
  desc.interval = presentParameters_.presentationInterval;
  desc.windowed = presentParameters_.windowed;
  desc.displaySyncEnabled = presentParameters_.presentationInterval != PresentInterval::Immediate;
  desc.multiSampleType = presentParameters_.multiSampleType;
  return desc;
}

DrawDesc Device::snapshotDrawDesc(PrimitiveType type, u32 primitiveCount, u32 startVertex,
                                  i32 baseVertexIndex, u32 startIndex, IndexType indexType) const {
  DrawDesc desc;
  desc.primitiveType = type == PrimitiveType::TriangleFan ? PrimitiveType::TriangleList : type;
  desc.primitiveCount = primitiveCount;
  desc.startVertex = startVertex;
  desc.baseVertexIndex = baseVertexIndex;
  desc.startIndex = startIndex;
  desc.indexType = indexType;
  desc.indexBuffer = state_.indexBuffer ? state_.indexBuffer->handle() : Handle{};
  desc.vertexDecl = state_.vertexDecl;
  desc.vertexDecl.streams.fill({});
  for (size_t i = 0; i < kMaxStreams; ++i) {
    desc.vertexDecl.streams[i].buffer = state_.streamBuffers[i];
    desc.vertexDecl.streams[i].offset = state_.streamOffsets[i];
    desc.vertexDecl.streams[i].stride = state_.streamStrides[i];
  }
  desc.rs.values = state_.renderStates;
  for (size_t i = 0; i < kMaxTextures; ++i) {
    desc.textures[i].handle = state_.textures[i] ? state_.textures[i]->handle() : Handle{};
    if (i < kMaxTextureStages) {
      desc.textures[i].stageStates = state_.textureStageStates[i];
    } else {
      desc.textures[i].stageStates.clear();
    }
  }
  for (size_t i = 0; i < kMaxSamplers; ++i) {
    desc.samplers[i].states = state_.samplerStates[i];
  }
  desc.rts.color = state_.renderTargets;
  desc.rts.depthStencil = state_.depthStencil;
  desc.viewport.viewport = state_.viewport;
  desc.viewport.scissor = state_.scissorRect;
  desc.viewport.scissorEnabled = state_.scissorEnabled;
  desc.clipPlaneMask = state_.renderStates.contains(RS_CLIP_PLANE_ENABLE)
                           ? state_.renderStates.at(RS_CLIP_PLANE_ENABLE)
                           : 0;
  const Matrix4x4 world = lookupTransform(state_, XFORM_WORLD_BASE);
  const Matrix4x4 view = lookupTransform(state_, XFORM_VIEW);
  const Matrix4x4 proj = lookupTransform(state_, XFORM_PROJECTION);
  const Matrix4x4 worldViewProj = multiplyMatrix(multiplyMatrix(world, view), proj);
  for (size_t i = 0; i < kMaxClipPlanes; ++i) {
    if ((desc.clipPlaneMask & (1u << i)) != 0) {
      desc.clipPlanes[i] = transformClipPlane(worldViewProj, state_.clipPlanes[i]);
    } else {
      desc.clipPlanes[i] = {};
    }
  }

  if (state_.vertexShader.kind == ShaderRef::Kind::Bytecode) {
    desc.vertexShader = state_.vertexShader;
  } else {
    desc.vertexShader.kind = ShaderRef::Kind::FixedFunctionVertex;
    desc.vertexShader.vertexKey = makeFfpVertexKey(state_);
    desc.vertexShader.hash = desc.vertexShader.vertexKey->hash;
  }
  if (state_.pixelShader.kind == ShaderRef::Kind::Bytecode) {
    desc.pixelShader = state_.pixelShader;
  } else {
    desc.pixelShader.kind = ShaderRef::Kind::FixedFunctionPixel;
    desc.pixelShader.pixelKey = makeFfpPixelKey(state_);
    desc.pixelShader.hash = desc.pixelShader.pixelKey->hash;
  }

  desc.vsConst = state_.vsConst;
  desc.psConst = state_.psConst;
  return desc;
}

HResult Device::clear(const ClearDesc& desc) {
  auto snapshot = snapshotClearDesc(desc);
  if (snapshot.clearColor) {
    for (const auto& attachment : snapshot.colorAttachments) {
      if (!attachment.handle) {
        continue;
      }
      for (auto& surface : surfaces_) {
        if (auto sp = surface.lock(); sp && sp->handle() == attachment.handle && sp->valid()) {
          if (snapshot.rects.empty()) {
            sp->fillColor(nullptr, snapshot.color);
          } else {
            for (const auto& rect : snapshot.rects) {
              sp->fillColor(&rect, snapshot.color);
            }
          }
        }
      }
      for (auto& texture : textures_) {
        if (auto tp = texture.lock(); tp && tp->handle() == attachment.handle && tp->valid()) {
          if (snapshot.rects.empty()) {
            tp->fillColor(nullptr, snapshot.color);
          } else {
            for (const auto& rect : snapshot.rects) {
              tp->fillColor(&rect, snapshot.color);
            }
          }
        }
      }
    }
  }

  if (snapshot.clearDepth || snapshot.clearStencil) {
    const auto applyDepthClear = [&](const std::shared_ptr<Surface>& surface) {
      if (!surface || !surface->valid()) {
        return;
      }
      const auto& surfaceDesc = surface->desc();
      if (!surfaceDesc.depthStencil) {
        return;
      }
      if (snapshot.rects.empty()) {
        auto region = surface->lockRect(nullptr, 0);
        if (region.data) {
          std::vector<u8> scratch(static_cast<size_t>(region.pitch) * surfaceDesc.height, 0);
          fillDepthStencil(scratch, region.pitch, surfaceDesc.width, surfaceDesc.height, surfaceDesc.format,
                           nullptr, snapshot.clearDepth, snapshot.depth, snapshot.clearStencil,
                           snapshot.stencil);
          std::memcpy(region.data, scratch.data(), scratch.size());
        }
        surface->unlockRect();
      } else {
        for (const auto& rect : snapshot.rects) {
          auto region = surface->lockRect(&rect, 0);
          if (!region.data) {
            continue;
          }
          auto* bytes = static_cast<u8*>(region.data);
          const u32 rectWidth = static_cast<u32>(std::max(0, rect.right - rect.left));
          const u32 rectHeight = static_cast<u32>(std::max(0, rect.bottom - rect.top));
          std::vector<u8> scratch(static_cast<size_t>(region.pitch) * rectHeight, 0);
          // Fill a temporary region, then copy it into the locked surface area.
          fillDepthStencil(scratch, region.pitch, rectWidth, rectHeight, surfaceDesc.format, nullptr,
                           snapshot.clearDepth, snapshot.depth, snapshot.clearStencil, snapshot.stencil);
          for (u32 y = 0; y < rectHeight; ++y) {
            std::memcpy(bytes + static_cast<size_t>(y) * region.pitch,
                        scratch.data() + static_cast<size_t>(y) * region.pitch,
                        static_cast<size_t>(rectWidth) * bytesPerPixel(surfaceDesc.format));
          }
          surface->unlockRect();
        }
      }
    };

    if (snapshot.depthStencil.handle) {
      for (auto& surface : surfaces_) {
        if (auto sp = surface.lock(); sp && sp->handle() == snapshot.depthStencil.handle) {
          applyDepthClear(sp);
        }
      }
    }
  }
  submitClearInternal(snapshot);
  return D3D_OK;
}

HResult Device::drawPrimitive(PrimitiveType type, u32 primitiveCount, u32 startVertex) {
  auto desc = snapshotDrawDesc(type, primitiveCount, startVertex, 0, 0, state_.indexType);
  submitDrawInternal(desc);
  if (state_.inScene) {
    // No-op; draw submission is immediate in the core harness.
  }
  return D3D_OK;
}

HResult Device::drawIndexedPrimitive(PrimitiveType type, u32 primitiveCount, u32 startVertex,
                                     i32 baseVertexIndex, u32 startIndex, IndexType indexType) {
  auto desc = snapshotDrawDesc(type, primitiveCount, startVertex, baseVertexIndex, startIndex, indexType);
  submitDrawInternal(desc);
  return D3D_OK;
}

HResult Device::drawPrimitiveUP(PrimitiveType type, u32 primitiveCount, std::span<const u8> vertexData) {
  upVertexScratch_.assign(vertexData.begin(), vertexData.end());
  auto desc = snapshotDrawDesc(type, primitiveCount, 0, 0, 0, state_.indexType);
  desc.userVertexData = upVertexScratch_;
  submitDrawInternal(desc);
  return D3D_OK;
}

HResult Device::drawIndexedPrimitiveUP(PrimitiveType type, u32 primitiveCount,
                                       std::span<const u8> vertexData, std::span<const u8> indexData,
                                       IndexType indexType) {
  upVertexScratch_.assign(vertexData.begin(), vertexData.end());
  upIndexScratch_.assign(indexData.begin(), indexData.end());
  if (type == PrimitiveType::TriangleFan) {
    std::vector<u32> indices;
    indices.reserve(indexData.size() / sizeof(u32));
    for (size_t i = 0; i + sizeof(u32) <= indexData.size(); i += sizeof(u32)) {
      u32 value = 0;
      std::memcpy(&value, indexData.data() + i, sizeof(u32));
      indices.push_back(value);
    }
    const auto fan = decomposeTriangleFanIndices(indices);
    upIndexScratch_.resize(fan.size() * sizeof(u32));
    std::memcpy(upIndexScratch_.data(), fan.data(), upIndexScratch_.size());
    type = PrimitiveType::TriangleList;
  }
  auto desc = snapshotDrawDesc(type, primitiveCount, 0, 0, 0, indexType);
  desc.userVertexData = upVertexScratch_;
  desc.userIndexData = upIndexScratch_;
  submitDrawInternal(desc);
  return D3D_OK;
}

HResult Device::present() {
  return presentEx();
}

HResult Device::presentEx(const Rect* sourceRect, const Rect* destRect, Handle destinationWindowOverride,
                          const void* dirtyRegion, u32 flags) {
  (void)sourceRect;
  (void)destRect;
  (void)destinationWindowOverride;
  (void)dirtyRegion;
  (void)flags;
  if (deviceLost_) {
    return D3DERR_DEVICELOST;
  }
  auto desc = snapshotSwapDesc();
  submitPresentInternal(desc);
  if (backend_) {
    backend_->flush();
  }
  completeUpTo(submittedSequenceId_);
  ++presentCount_;
  maybeCaptureExperimentFrame();
  // SeqIdSafety: a completed present must not outrun the submitted sequence.
  DXMT_ASSERT(completedSequenceId_ == submittedSequenceId_);
  return D3D_OK;
}

HResult Device::reset(const PresentParameters& params) {
  return resetEx(params, nullptr);
}

HResult Device::resetEx(const PresentParameters& params, const DisplayModeEx* fullscreenMode) {
  auto adjusted = params;
  if (fullscreenMode) {
    adjusted.windowed = false;
    if (fullscreenMode->width != 0) {
      adjusted.backBufferWidth = fullscreenMode->width;
    }
    if (fullscreenMode->height != 0) {
      adjusted.backBufferHeight = fullscreenMode->height;
    }
    if (fullscreenMode->format != Format::Unknown) {
      adjusted.backBufferFormat = fullscreenMode->format;
    }
  }
  presentParameters_ = normalizePresentParameters(adapter_, adjusted);
  deviceLost_ = false;
  presentOccluded_ = false;
  if (backend_) {
    backend_->flush();
  }
  completeUpTo(submittedSequenceId_);
  // Drain-before-teardown: Reset waits for queued work to drain before
  // invalidating default-pool resources.
  DXMT_ASSERT(completedSequenceId_ == submittedSequenceId_);
  invalidateDefaultPoolResources();
  state_.reset();
  state_.viewport = {0, 0, std::max(1u, presentParameters_.backBufferWidth),
                     std::max(1u, presentParameters_.backBufferHeight), 0.0f,
                     1.0f};
  for (auto& chain : swapChains_) {
    if (chain) {
      chain->resize(presentParameters_);
    }
  }
  if (!swapChains_.empty() && swapChains_.front()) {
    const auto primary = swapChains_.front();
    state_.renderTargets[0] = primary->backBuffer()
                                  ? RenderTargetAttachment{primary->backBuffer()->handle(), 0,
                                                           primary->backBuffer()->multiSampleCount()}
                                  : RenderTargetAttachment{};
    state_.depthStencil = primary->depthStencilSurface()
                              ? RenderTargetAttachment{primary->depthStencilSurface()->handle(), 0,
                                                       primary->depthStencilSurface()->multiSampleCount()}
                              : RenderTargetAttachment{};
  }
  submittedSequenceId_ = 0;
  completedSequenceId_ = 0;
  presentCount_ = 0;
  experimentCapture_.captured = false;
  if (backend_) {
    backend_->setMaxFrameLatency(maximumFrameLatency_);
  }
  return D3D_OK;
}

HResult Device::setMaximumFrameLatency(u32 latency) {
  maximumFrameLatency_ = std::clamp(latency, 1u, 3u);
  if (backend_) {
    backend_->setMaxFrameLatency(maximumFrameLatency_);
  }
  return D3D_OK;
}

HResult Device::waitForVBlank(size_t swapChainIndex) {
  auto chain = swapChain(swapChainIndex);
  if (!chain) {
    return D3DERR_INVALIDCALL;
  }
  if (backend_) {
    return backend_->waitForVBlank(makeSwapDesc(chain->params()));
  }
  return D3D_OK;
}

HResult Device::checkResourceResidency(std::span<void* const> resources) const {
  (void)resources;
  return S_OK;
}

DisplayModeEx Device::getDisplayModeEx(size_t swapChainIndex) const {
  DisplayModeEx mode;
  const auto chain = swapChain(swapChainIndex);
  const auto& params = chain ? chain->params() : presentParameters_;
  mode.width = std::max(1u, params.backBufferWidth);
  mode.height = std::max(1u, params.backBufferHeight);
  mode.refreshRate = 60;
  mode.format = params.backBufferFormat;
  mode.scanLineOrdering = DisplayScanLineOrdering::Progressive;
  return mode;
}

HResult Device::getGPUThreadPriority(i32* priority) const {
  if (priority) {
    *priority = 0;
  }
  return D3D_OK;
}

HResult Device::setGPUThreadPriority(i32 priority) {
  (void)priority;
  return D3D_OK;
}

HResult Device::setConvolutionMonoKernel() {
  return E_NOTIMPL;
}

HResult Device::composeRects() {
  return E_NOTIMPL;
}

HResult Device::checkDeviceMultiSampleType(Format format, MultiSampleType type) const {
  if (type == MultiSampleType::None) {
    return D3D_OK;
  }

  const auto supportsCount = [this](u32 count) {
    switch (count) {
      case 2:
        return limits_.supportsSampleCount2;
      case 4:
        return limits_.supportsSampleCount4;
      case 8:
        return limits_.supportsSampleCount8;
      default:
        return false;
    }
  };

  const u32 count = dxmt9::core::sampleCount(type);
  if (!supportsCount(count)) {
    return D3DERR_NOTAVAILABLE;
  }
  if (!formatSupportsUsage(format, UsageRenderTarget, limits_) &&
      !formatSupportsUsage(format, UsageDepthStencil, limits_)) {
    return D3DERR_NOTAVAILABLE;
  }
  return D3D_OK;
}

HResult Device::issueQuery(const std::shared_ptr<Query>& query, bool begin) {
  if (!query) {
    return D3DERR_INVALIDCALL;
  }
  ++submittedSequenceId_;
  // SeqIdSafety: queries advance the submission sequence but never allow the
  // completed sequence to move ahead of it.
  DXMT_ASSERT(submittedSequenceId_ >= completedSequenceId_);
  if (begin) {
    query->begin(submittedSequenceId_);
    if (query->type() == QueryType::Occlusion) {
      activeOcclusionQuery_ = query;
      activeOcclusionCount_ = 0;
    }
  } else {
    query->end(submittedSequenceId_);
    if (query->type() == QueryType::Occlusion) {
      query->resolve(activeOcclusionCount_);
      activeOcclusionQuery_.reset();
    } else if (query->type() == QueryType::Timestamp) {
      query->resolve(submittedSequenceId_);
    }
  }
  return D3D_OK;
}

HResult Device::getQueryData(const std::shared_ptr<Query>& query, void* output, size_t size,
                             u32 flags) const {
  if (!query) {
    return D3DERR_INVALIDCALL;
  }
  // SeqIdSafety: the completed sequence never exceeds the submitted sequence.
  DXMT_ASSERT(completedSequenceId_ <= submittedSequenceId_);
  return query->getData(output, size, flags, completedSequenceId_);
}

void Device::completeUpTo(u64 sequenceId) {
  DXMT_ASSERT(sequenceId >= completedSequenceId_);
  completedSequenceId_ = std::max(completedSequenceId_, sequenceId);
  DXMT_ASSERT(completedSequenceId_ <= submittedSequenceId_);
}

void Device::registerBuffer(const std::shared_ptr<Buffer>& buffer) {
  buffers_.push_back(buffer);
}

void Device::registerTexture(const std::shared_ptr<Texture>& texture) {
  textures_.push_back(texture);
}

void Device::registerSurface(const std::shared_ptr<Surface>& surface) {
  surfaces_.push_back(surface);
}

void Device::invalidateDefaultPoolResources() {
  auto invalidateWeak = [](auto& list) {
    list.erase(std::remove_if(list.begin(), list.end(), [](const auto& weak) {
                 if (auto ptr = weak.lock()) {
                   if (ptr->desc().pool == Pool::Default) {
                     ptr->invalidate();
                   }
                   return false;
                 }
                 return true;
               }),
               list.end());
  };
  invalidateWeak(buffers_);
  invalidateWeak(textures_);
  invalidateWeak(surfaces_);
}

void Device::submitClearInternal(const ClearDesc& desc) {
  emitRenderTrace("clear seq=%llu color=%d depth=%d stencil=%d color0=0x%llx depthStencil=0x%llx rects=%zu rgba=(%.3f,%.3f,%.3f,%.3f) depthValue=%.3f stencilValue=%u",
                  static_cast<unsigned long long>(submittedSequenceId_ + 1),
                  desc.clearColor ? 1 : 0,
                  desc.clearDepth ? 1 : 0,
                  desc.clearStencil ? 1 : 0,
                  static_cast<unsigned long long>(desc.colorAttachments[0].handle.value),
                  static_cast<unsigned long long>(desc.depthStencil.handle.value),
                  desc.rects.size(),
                  desc.color.r,
                  desc.color.g,
                  desc.color.b,
                  desc.color.a,
                  desc.depth,
                  desc.stencil);
  backend_->submitClear(desc);
  ++submittedSequenceId_;
  // SeqIdSafety: a submission can advance the current sequence, but never
  // below the completed sequence.
  DXMT_ASSERT(submittedSequenceId_ >= completedSequenceId_);
}

void Device::submitDrawInternal(const DrawDesc& desc) {
  emitRenderTrace("draw seq=%llu primType=%u primCount=%u rt0=0x%llx ds=0x%llx tex0=0x%llx vs=%u ps=%u fvf=0x%x lighting=%u alphaTest=%u clipMask=0x%x",
                  static_cast<unsigned long long>(submittedSequenceId_ + 1),
                  static_cast<unsigned>(desc.primitiveType),
                  desc.primitiveCount,
                  static_cast<unsigned long long>(desc.rts.color[0].handle.value),
                  static_cast<unsigned long long>(desc.rts.depthStencil.handle.value),
                  static_cast<unsigned long long>(desc.textures[0].handle.value),
                  static_cast<unsigned>(desc.vertexShader.kind),
                  static_cast<unsigned>(desc.pixelShader.kind),
                  desc.vertexDecl.fvf,
                  desc.rs.values.contains(RS_LIGHTING) ? desc.rs.values.at(RS_LIGHTING) : 0u,
                  desc.rs.values.contains(RS_ALPHA_TEST_ENABLE) ? desc.rs.values.at(RS_ALPHA_TEST_ENABLE) : 0u,
                  desc.clipPlaneMask);
  backend_->submitDraw(desc);
  ++submittedSequenceId_;
  // SeqIdSafety: a submission can advance the current sequence, but never
  // below the completed sequence.
  DXMT_ASSERT(submittedSequenceId_ >= completedSequenceId_);
  if (activeOcclusionQuery_) {
    activeOcclusionCount_ += desc.primitiveCount;
  }
}

void Device::submitPresentInternal(const SwapDesc& desc) {
  emitRenderTrace("present seq=%llu window=0x%llx size=%ux%u fmt=%u windowed=%d interval=%u",
                  static_cast<unsigned long long>(submittedSequenceId_ + 1),
                  static_cast<unsigned long long>(desc.window.value),
                  desc.width,
                  desc.height,
                  static_cast<unsigned>(desc.format),
                  desc.windowed ? 1 : 0,
                  static_cast<unsigned>(desc.interval));
  backend_->present(desc);
  ++submittedSequenceId_;
  // SeqIdSafety: a submission can advance the current sequence, but never
  // below the completed sequence.
  DXMT_ASSERT(submittedSequenceId_ >= completedSequenceId_);
}

void Device::maybeCaptureExperimentFrame() {
  if (experimentCapture_.captured || experimentCapture_.frame == 0 || experimentCapture_.path.empty()) {
    return;
  }
  if (presentCount_ < experimentCapture_.frame) {
    return;
  }
  auto chain = swapChain(0);
  if (!chain) {
    return;
  }
  auto backBuffer = chain->backBuffer();
  if (!backBuffer || !backBuffer->valid()) {
    return;
  }
  const auto& desc = backBuffer->desc();
  const u32 bpp = bytesPerPixel(desc.format);
  if (bpp == 0) {
    return;
  }
  auto scratch = createSurface(
      {desc.width, desc.height, desc.format, Pool::Scratch, 0, false, false, MultiSampleType::None});
  if (!scratch || getRenderTargetData(backBuffer, scratch) != D3D_OK) {
    return;
  }
  auto region = scratch->lockRect(nullptr, 0);
  if (!region.data) {
    return;
  }
  const size_t byteCount = static_cast<size_t>(region.pitch) * desc.height;
  const bool wrote = writeBmpScreenshot(experimentCapture_.path, desc.format, desc.width, desc.height,
                                        region.pitch,
                                        std::span<const u8>(static_cast<const u8*>(region.data), byteCount));
  scratch->unlockRect();
  if (wrote) {
    experimentCapture_.captured = true;
  }
}

HResult Device::fillSurface(const std::shared_ptr<Surface>& surface, const Rect* rect, ColorRGBA color) {
  if (!surface || !surface->valid()) {
    return D3DERR_INVALIDCALL;
  }
  if (backend_) {
    ColorFillDesc backendDesc;
    backendDesc.destination = surface->handle();
    if (rect) {
      backendDesc.rect = *rect;
      backendDesc.hasRect = true;
    }
    backendDesc.color = color;
    backend_->submitColorFill(backendDesc);
  }
  surface->fillColor(rect, color);
  return D3D_OK;
}

HResult Device::stretchRect(const std::shared_ptr<Surface>& src, const Rect* srcRect,
                            const std::shared_ptr<Surface>& dst, const Rect* dstRect, bool linear) {
  if (!src || !dst || !src->valid() || !dst->valid()) {
    return D3DERR_INVALIDCALL;
  }
  if (src->desc().format != dst->desc().format) {
    return D3DERR_NOTAVAILABLE;
  }
  Rect srcArea = srcRect ? *srcRect
                         : Rect{0, 0, static_cast<i32>(src->desc().width), static_cast<i32>(src->desc().height)};
  Rect dstArea = dstRect ? *dstRect
                         : Rect{0, 0, static_cast<i32>(dst->desc().width), static_cast<i32>(dst->desc().height)};
  const i32 srcWidth = std::max(0, srcArea.right - srcArea.left);
  const i32 srcHeight = std::max(0, srcArea.bottom - srcArea.top);
  const i32 dstWidth = std::max(0, dstArea.right - dstArea.left);
  const i32 dstHeight = std::max(0, dstArea.bottom - dstArea.top);
  if (srcWidth == 0 || srcHeight == 0 || dstWidth == 0 || dstHeight == 0) {
    return D3DERR_INVALIDCALL;
  }

  if (backend_) {
    StretchRectDesc backendDesc;
    backendDesc.source = src->handle();
    backendDesc.destination = dst->handle();
    backendDesc.sourceRect = srcArea;
    backendDesc.destinationRect = dstArea;
    backendDesc.linear = linear;
    backend_->submitStretchRect(backendDesc);
  }

  auto extractRegion = [&](const std::shared_ptr<Surface>& surface, const Rect& area) -> std::vector<u8> {
    const u32 bpp = bytesPerPixel(surface->desc().format);
    const u32 width = static_cast<u32>(std::max(0, area.right - area.left));
    const u32 height = static_cast<u32>(std::max(0, area.bottom - area.top));
    auto region = surface->lockRect(&area, 0);
    if (!region.data || bpp == 0) {
      if (region.data) {
        surface->unlockRect();
      }
      return {};
    }
    std::vector<u8> out(static_cast<size_t>(width) * height * bpp);
    const auto* srcBytes = static_cast<const u8*>(region.data);
    for (u32 y = 0; y < height; ++y) {
      std::memcpy(out.data() + static_cast<size_t>(y) * width * bpp,
                  srcBytes + static_cast<size_t>(y) * region.pitch,
                  static_cast<size_t>(width) * bpp);
    }
    surface->unlockRect();
    return out;
  };

  auto blitRegion = [&](const std::shared_ptr<Surface>& surface, const Rect& area, std::span<const u8> bytes,
                        u32 srcWidthPixels, u32 srcHeightPixels) -> HResult {
    const u32 bpp = bytesPerPixel(surface->desc().format);
    if (bpp == 0) {
      return D3DERR_INVALIDCALL;
    }
    auto region = surface->lockRect(&area, 0);
    if (!region.data) {
      return D3DERR_INVALIDCALL;
    }
    const u32 dstW = static_cast<u32>(std::max(0, area.right - area.left));
    const u32 dstH = static_cast<u32>(std::max(0, area.bottom - area.top));
    std::vector<u8> temp;
    if (srcWidthPixels == dstW && srcHeightPixels == dstH) {
      temp.assign(bytes.begin(), bytes.end());
    } else {
      temp.resize(static_cast<size_t>(dstW) * dstH * bpp);
      std::vector<u8> srcCopy(bytes.begin(), bytes.end());
      if (!stretchPixels(temp, dstW * bpp, dstW, dstH, surface->desc().format, srcCopy, srcWidthPixels * bpp,
                         srcWidthPixels, srcHeightPixels, surface->desc().format)) {
        surface->unlockRect();
        return D3DERR_INVALIDCALL;
      }
    }
    const auto* srcBytes = temp.data();
    for (u32 y = 0; y < dstH; ++y) {
      std::memcpy(static_cast<u8*>(region.data) + static_cast<size_t>(y) * region.pitch,
                  srcBytes + static_cast<size_t>(y) * dstW * bpp, static_cast<size_t>(dstW) * bpp);
    }
    surface->unlockRect();
    return D3D_OK;
  };

  const auto srcBytes = extractRegion(src, srcArea);
  if (srcBytes.empty()) {
    return D3DERR_INVALIDCALL;
  }

  const HResult result = blitRegion(dst, dstArea, std::span<const u8>(srcBytes.data(), srcBytes.size()),
                                    static_cast<u32>(srcWidth), static_cast<u32>(srcHeight));
  if (result != D3D_OK) {
    return result;
  }
  return D3D_OK;
}

HResult Device::updateSurface(const std::shared_ptr<Surface>& src, const std::shared_ptr<Surface>& dst) {
  if (!src || !dst || !src->valid() || !dst->valid()) {
    return D3DERR_INVALIDCALL;
  }
  if (src->desc().format != dst->desc().format) {
    return D3DERR_NOTAVAILABLE;
  }

  if (backend_) {
    SurfaceCopyDesc backendDesc;
    backendDesc.source = src->handle();
    backendDesc.destination = dst->handle();
    backendDesc.sourceRect = {0, 0, static_cast<i32>(src->desc().width), static_cast<i32>(src->desc().height)};
    backendDesc.destinationRect = {0, 0, static_cast<i32>(dst->desc().width), static_cast<i32>(dst->desc().height)};
    backend_->submitSurfaceCopy(backendDesc);
  }

  auto srcRegion = src->lockRect(nullptr, 0);
  auto dstRegion = dst->lockRect(nullptr, 0);
  if (!srcRegion.data || !dstRegion.data) {
    if (srcRegion.data) {
      src->unlockRect();
    }
    if (dstRegion.data) {
      dst->unlockRect();
    }
    return D3DERR_INVALIDCALL;
  }

  const u32 bpp = bytesPerPixel(src->desc().format);
  const u32 width = std::min(src->desc().width, dst->desc().width);
  const u32 height = std::min(src->desc().height, dst->desc().height);
  for (u32 y = 0; y < height; ++y) {
    std::memcpy(static_cast<u8*>(dstRegion.data) + static_cast<size_t>(y) * dstRegion.pitch,
                static_cast<const u8*>(srcRegion.data) + static_cast<size_t>(y) * srcRegion.pitch,
                static_cast<size_t>(width) * bpp);
  }
  src->unlockRect();
  dst->unlockRect();
  return D3D_OK;
}

HResult Device::updateTexture(const std::shared_ptr<Texture>& src, const std::shared_ptr<Texture>& dst) {
  if (!src || !dst || !src->valid() || !dst->valid()) {
    return D3DERR_INVALIDCALL;
  }
  if (src->desc().format != dst->desc().format) {
    return D3DERR_NOTAVAILABLE;
  }
  const u32 levels = std::min(src->levelCount(), dst->levelCount());
  for (u32 level = 0; level < levels; ++level) {
    auto srcSurface = src->surfaceLevel(level);
    auto dstSurface = dst->surfaceLevel(level);
    if (!srcSurface || !dstSurface) {
      return D3DERR_INVALIDCALL;
    }
    if (backend_) {
      SurfaceCopyDesc backendDesc;
      backendDesc.source = srcSurface->handle();
      backendDesc.destination = dstSurface->handle();
      backendDesc.sourceLevel = level;
      backendDesc.destinationLevel = level;
      backendDesc.sourceRect = {0, 0, static_cast<i32>(srcSurface->desc().width),
                                static_cast<i32>(srcSurface->desc().height)};
      backendDesc.destinationRect = {0, 0, static_cast<i32>(dstSurface->desc().width),
                                     static_cast<i32>(dstSurface->desc().height)};
      backend_->submitSurfaceCopy(backendDesc);
    }
  }
  dst->copyFrom(*src);
  return D3D_OK;
}

HResult Device::getRenderTargetData(const std::shared_ptr<Surface>& src, const std::shared_ptr<Surface>& dst) {
  if (!src || !dst || !src->valid() || !dst->valid()) {
    return D3DERR_INVALIDCALL;
  }
  if (backend_) {
    ReadbackDesc backendDesc;
    backendDesc.source = src->handle();
    backendDesc.destination = dst->handle();
    backendDesc.sourceRect = {0, 0, static_cast<i32>(src->desc().width), static_cast<i32>(src->desc().height)};
    backend_->submitReadback(backendDesc);
    backend_->flush();
    ReadbackPixels pixels;
    if (backend_->readbackSurface(backendDesc, pixels)) {
      auto dstRegion = dst->lockRect(nullptr, 0);
      if (!dstRegion.data) {
        return D3DERR_INVALIDCALL;
      }
      const u32 bpp = bytesPerPixel(src->desc().format);
      if (bpp == 0) {
        dst->unlockRect();
        return D3DERR_NOTAVAILABLE;
      }
      const u32 width = std::min(src->desc().width, dst->desc().width);
      const u32 height = std::min(src->desc().height, dst->desc().height);
      const size_t rowBytes = static_cast<size_t>(width) * bpp;
      if (pixels.pitch < rowBytes || pixels.bytes.size() < static_cast<size_t>(pixels.pitch) * height) {
        dst->unlockRect();
        return D3DERR_INVALIDCALL;
      }
      for (u32 y = 0; y < height; ++y) {
        std::memcpy(static_cast<u8*>(dstRegion.data) + static_cast<size_t>(y) * dstRegion.pitch,
                    pixels.bytes.data() + static_cast<size_t>(y) * pixels.pitch, rowBytes);
      }
      dst->unlockRect();
      return D3D_OK;
    }
  }
  return updateSurface(src, dst);
}

void Device::resetState() {
  state_.reset();
}

FfpVertexKey makeFfpVertexKey(const DeviceState& state) {
  FfpVertexKey key;
  key.lightingEnabled = state.renderStates.contains(RS_LIGHTING) && state.renderStates.at(RS_LIGHTING) != 0;
  key.specularEnabled = state.renderStates.contains(RS_SPECULAR_ENABLE) &&
                        state.renderStates.at(RS_SPECULAR_ENABLE) != 0;
  key.normalizeNormals = state.renderStates.contains(RS_NORMALIZE_NORMALS) &&
                         state.renderStates.at(RS_NORMALIZE_NORMALS) != 0;
  for (size_t i = 0; i < kMaxLights; ++i) {
    key.lightEnabled[i] = state.lightEnabled[i];
    key.lightType[i] = static_cast<u32>(state.lights[i].type);
  }
  key.colorMaterialMode[0] = state.renderStates.contains(RS_EMISSIVE_MATERIAL_SOURCE)
                                 ? state.renderStates.at(RS_EMISSIVE_MATERIAL_SOURCE)
                                 : 0;
  key.colorMaterialMode[1] = state.renderStates.contains(RS_AMBIENT_MATERIAL_SOURCE)
                                 ? state.renderStates.at(RS_AMBIENT_MATERIAL_SOURCE)
                                 : 0;
  key.colorMaterialMode[2] = state.renderStates.contains(RS_DIFFUSE_MATERIAL_SOURCE)
                                 ? state.renderStates.at(RS_DIFFUSE_MATERIAL_SOURCE)
                                 : 0;
  key.colorMaterialMode[3] = state.renderStates.contains(RS_SPECULAR_MATERIAL_SOURCE)
                                 ? state.renderStates.at(RS_SPECULAR_MATERIAL_SOURCE)
                                 : 0;
  key.fogMode = static_cast<FogMode>(state.renderStates.contains(RS_FOG_TABLE_MODE)
                                         ? state.renderStates.at(RS_FOG_TABLE_MODE)
                                         : 0);
  key.fogFromVertex = state.renderStates.contains(RS_FOG_FROM_VERTEX) &&
                      state.renderStates.at(RS_FOG_FROM_VERTEX) != 0;
  key.rangeFog = state.renderStates.contains(RS_RANGE_FOG) && state.renderStates.at(RS_RANGE_FOG) != 0;
  for (size_t i = 0; i < kMaxTextureStages; ++i) {
    key.texCoordGen[i] = state.textureStageStates[i].contains(TSS_TEXCOORD_INDEX)
                             ? state.textureStageStates[i].at(TSS_TEXCOORD_INDEX)
                             : 0;
    key.texTransformFlags[i] = state.textureStageStates[i].contains(TSS_TEXTURE_TRANSFORM_FLAGS)
                                   ? state.textureStageStates[i].at(TSS_TEXTURE_TRANSFORM_FLAGS)
                                   : 0;
  }
  key.vertexBlend = state.renderStates.contains(RS_VERTEX_BLEND) ? state.renderStates.at(RS_VERTEX_BLEND) : 0;
  key.indexedVertexBlend = key.vertexBlend != 0;
  key.clipPlaneMask = state.renderStates.contains(RS_CLIP_PLANE_ENABLE)
                          ? state.renderStates.at(RS_CLIP_PLANE_ENABLE)
                          : 0;
  key.hash = hashFfpVertexKey(key);
  return key;
}

FfpPixelKey makeFfpPixelKey(const DeviceState& state) {
  FfpPixelKey key;
  for (size_t stage = 0; stage < kMaxTextureStages; ++stage) {
    const auto& map = state.textureStageStates[stage];
    auto& out = key.stages[stage];
    out.colorOp = map.contains(TSS_COLOR_OP) ? map.at(TSS_COLOR_OP) : 0;
    out.colorArg1 = map.contains(TSS_COLOR_ARG1) ? map.at(TSS_COLOR_ARG1) : 0;
    out.colorArg2 = map.contains(TSS_COLOR_ARG2) ? map.at(TSS_COLOR_ARG2) : 0;
    out.alphaOp = map.contains(TSS_ALPHA_OP) ? map.at(TSS_ALPHA_OP) : 0;
    out.alphaArg1 = map.contains(TSS_ALPHA_ARG1) ? map.at(TSS_ALPHA_ARG1) : 0;
    out.alphaArg2 = map.contains(TSS_ALPHA_ARG2) ? map.at(TSS_ALPHA_ARG2) : 0;
    out.resultArg = map.contains(TSS_RESULT_ARG) ? map.at(TSS_RESULT_ARG) : 0;
    out.texType = map.contains(TSS_TEXTURE_TYPE) ? map.at(TSS_TEXTURE_TYPE) : 0;
    out.texCoordIndex = map.contains(TSS_TEXCOORD_INDEX) ? map.at(TSS_TEXCOORD_INDEX) : 0;
  }
  key.fogMode = static_cast<FogMode>(state.renderStates.contains(RS_FOG_TABLE_MODE)
                                         ? state.renderStates.at(RS_FOG_TABLE_MODE)
                                         : 0);
  key.alphaTestEnable = state.renderStates.contains(RS_ALPHA_TEST_ENABLE) &&
                        state.renderStates.at(RS_ALPHA_TEST_ENABLE) != 0;
  key.alphaTestFunc = state.renderStates.contains(RS_ALPHA_FUNC) ? state.renderStates.at(RS_ALPHA_FUNC) : 0;
  key.hash = hashFfpPixelKey(key);
  return key;
}

Factory::Factory(BackendLimits limits, std::shared_ptr<BackendDevice> backend)
    : limits_(limits), backend_(backend ? std::move(backend) : makeBackendDevice(limits_)) {
  AdapterInfo adapter;
  adapter.ordinal = 0;
  adapter.name = getenvString("DXMT9_ADAPTER_NAME");
  if (adapter.name.empty()) {
    adapter.name = "NVIDIA GeForce 6800";
  }
  adapter.registryId = 1;
  adapter.displayId = 1;
  adapter.displayMode = {1920, 1080, 60, Format::A8R8G8B8};
  adapters_.push_back(std::move(adapter));
  adapterCaps_.push_back(makeDefaultCaps(limits_));
}

const AdapterInfo& Factory::adapter(size_t index) const {
  if (index >= adapters_.size()) {
    throw std::out_of_range("adapter index out of range");
  }
  return adapters_[index];
}

const DeviceCaps& Factory::caps(size_t index) const {
  if (index >= adapterCaps_.size()) {
    throw std::out_of_range("caps index out of range");
  }
  return adapterCaps_[index];
}

AdapterIdentifier Factory::getAdapterIdentifier(size_t index) const {
  const auto& info = adapter(index);
  AdapterIdentifier identifier;
  identifier.description = info.name;
  identifier.deviceName = "\\\\.\\DISPLAY1";
  identifier.driver = getenvString("DXMT9_ADAPTER_DRIVER");
  if (identifier.driver.empty()) {
    identifier.driver = "nvd3dum.dll";
  }
  identifier.driverVersion = info.registryId;
  identifier.vendorId = parseEnvU32Auto("DXMT9_ADAPTER_VENDOR_ID").value_or(0x10deu);
  identifier.deviceId = parseEnvU32Auto("DXMT9_ADAPTER_DEVICE_ID").value_or(0x0041u);
  identifier.subSysId = 0;
  identifier.revision = 0;
  identifier.monitor = info.displayId;
  return identifier;
}

std::vector<DisplayMode> Factory::enumAdapterModes(size_t index, Format format) const {
  if (index >= adapters_.size()) {
    return {};
  }
  return makeAdapterModes(format, limits_);
}

DisplayMode Factory::getAdapterDisplayMode(size_t index) const {
  return adapter(index).displayMode;
}

u32 Factory::getAdapterMonitor(size_t index) const {
  return adapter(index).displayId;
}

HRESULT Factory::checkDeviceType(size_t adapterIndex, DeviceType deviceType, Format adapterFormat,
                                 Format backBufferFormat, bool windowed) const {
  if (adapterIndex >= adapters_.size()) {
    return D3DERR_INVALIDCALL;
  }
  if (deviceType != DeviceType::Hal) {
    return D3DERR_NOTAVAILABLE;
  }
  if (windowed) {
    if (!isDisplayModeFormat(adapterFormat) || !isDisplayModeFormat(backBufferFormat)) {
      return D3DERR_NOTAVAILABLE;
    }
  } else {
    if (!isDisplayModeFormat(adapterFormat) || !isDisplayModeFormat(backBufferFormat)) {
      return D3DERR_NOTAVAILABLE;
    }
    if (adapterFormat != backBufferFormat) {
      return D3DERR_NOTAVAILABLE;
    }
    if (enumAdapterModes(adapterIndex, backBufferFormat).empty()) {
      return D3DERR_NOTAVAILABLE;
    }
  }
  if (!formatSupportsUsage(adapterFormat, UsageRenderTarget, limits_) ||
      !formatSupportsUsage(backBufferFormat, UsageRenderTarget, limits_)) {
    return D3DERR_NOTAVAILABLE;
  }
  return D3D_OK;
}

HRESULT Factory::checkDeviceFormat(size_t adapterIndex, Format format, u32 usage) const {
  if (adapterIndex >= adapters_.size()) {
    return D3DERR_INVALIDCALL;
  }
  return formatSupportsUsage(format, usage, limits_) ? D3D_OK : D3DERR_NOTAVAILABLE;
}

HRESULT Factory::checkDeviceMultiSampleType(size_t adapterIndex, Format format, MultiSampleType type) const {
  if (adapterIndex >= adapters_.size()) {
    return D3DERR_INVALIDCALL;
  }

  if (type == MultiSampleType::None) {
    return D3D_OK;
  }

  const auto supportsCount = [this](u32 count) {
    switch (count) {
      case 2:
        return limits_.supportsSampleCount2;
      case 4:
        return limits_.supportsSampleCount4;
      case 8:
        return limits_.supportsSampleCount8;
      default:
        return false;
    }
  };

  const u32 count = dxmt9::core::sampleCount(type);
  if (!supportsCount(count)) {
    return D3DERR_NOTAVAILABLE;
  }
  if (!formatSupportsUsage(format, UsageRenderTarget, limits_) &&
      !formatSupportsUsage(format, UsageDepthStencil, limits_)) {
    return D3DERR_NOTAVAILABLE;
  }
  return D3D_OK;
}

std::shared_ptr<Device> Factory::createDevice(size_t adapterIndex, const PresentParameters& params,
                                              u32 behaviorFlags) {
  if (adapterIndex >= adapters_.size()) {
    return {};
  }
  const auto& adapterInfo = adapters_[adapterIndex];
  const auto normalized = normalizePresentParameters(adapterInfo, params);
  const auto fullscreenAdapterFormat =
      normalized.windowed ? adapterInfo.displayMode.format : normalized.backBufferFormat;
  if (checkDeviceType(adapterIndex, DeviceType::Hal, fullscreenAdapterFormat, normalized.backBufferFormat,
                      normalized.windowed) != D3D_OK) {
    return {};
  }
  if (!normalized.windowed) {
    const auto modes = enumAdapterModes(adapterIndex, normalized.backBufferFormat);
    const auto match = std::find_if(modes.begin(), modes.end(), [&](const DisplayMode& mode) {
      return mode.width == normalized.backBufferWidth && mode.height == normalized.backBufferHeight;
    });
    if (match == modes.end()) {
      return {};
    }
  }
  auto device = std::shared_ptr<Device>(
      new Device(adapterInfo, limits_, backend_, normalized, behaviorFlags));
  device->initializeDefaultSwapChain();
  if (auto backend = device->backend()) {
    std::weak_ptr<Device> weak = device;
    backend->setDeviceLostObserver([weak](bool lost) {
      if (auto locked = weak.lock()) {
        locked->setDeviceLost(lost);
      }
    });
    backend->setPresentationStatusObserver([weak](bool occluded) {
      if (auto locked = weak.lock()) {
        locked->setPresentOccluded(occluded);
      }
    });
    backend->setMaxFrameLatency(device->maximumFrameLatency());
  }
  return device;
}

}  // namespace dxmt9::core
