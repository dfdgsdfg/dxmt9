// Concrete WMT/Metal oracle for complete automatic mip generation.
//
// The source texture deliberately has a non-uniform RGBA16Float level 0 and
// no authored lower levels.  Generation is issued through the project's WMT
// blit wrapper (the same command encoding used by the backend), then every
// level is read back through Metal and compared with the recursively expected
// 2x2 box reduction.  In particular this rejects a level-2 top-left crop of
// level 1, which was the production failure shape observed in HDR1.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "../../../src/winemetal/Metal.hpp"
#include "../../../src/dxmt9/dxmt9_resource_pool.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint32_t kWidth = 1024u;
constexpr std::uint32_t kHeight = 1024u;
constexpr std::uint32_t kMipCount = 11u;
// Repeated reductions are rounded to binary16 at every level.  The largest
// source values are near 256, where one binary16 ULP is 0.125.
constexpr float kTolerance = 0.2f;

struct Pixel {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 0.0f;
};

[[noreturn]] void fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
  std::exit(1);
}

void check(bool condition, std::string_view message) {
  if (!condition) {
    fail(message);
  }
}

// Decode an IEEE-754 binary16 value without relying on compiler-specific
// half-precision extensions.  The generated mip levels are read as raw
// RGBA16Float bytes from the shared Metal texture.
float halfToFloat(std::uint16_t bits) {
  const std::uint32_t sign = (bits & 0x8000u) << 16u;
  const std::uint32_t exponent = (bits >> 10u) & 0x1fu;
  const std::uint32_t fraction = bits & 0x03ffu;
  std::uint32_t value = sign;
  if (exponent == 0u) {
    if (fraction != 0u) {
      std::uint32_t normalized = fraction;
      std::uint32_t shift = 0u;
      while ((normalized & 0x0400u) == 0u) {
        normalized <<= 1u;
        ++shift;
      }
      const std::uint32_t mantissa = normalized & 0x03ffu;
      value |= (127u - 14u - shift) << 23u;
      value |= mantissa << 13u;
    }
  } else if (exponent == 0x1fu) {
    value |= 0x7f800000u | (fraction << 13u);
  } else {
    value |= (exponent + (127u - 15u)) << 23u;
    value |= fraction << 13u;
  }
  float result = 0.0f;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

Pixel sourcePixel(std::uint32_t x, std::uint32_t y) {
  // Integer-valued channels are exactly representable in binary16 while the
  // distinct x/y periods make a stale crop observably different from a full
  // 2x2 reduction.  Alpha is intentionally non-constant as well.
  return {
      static_cast<float>((3u * x + 5u * y + 17u) % 257u),
      static_cast<float>((7u * x + 11u * y + 31u) % 257u),
      static_cast<float>(((x ^ (3u * y)) + 47u) % 257u),
      static_cast<float>((x + 13u * y) % 257u),
  };
}

std::vector<Pixel> makeSource() {
  std::vector<Pixel> pixels(static_cast<std::size_t>(kWidth) * kHeight);
  for (std::uint32_t y = 0; y < kHeight; ++y) {
    for (std::uint32_t x = 0; x < kWidth; ++x) {
      pixels[static_cast<std::size_t>(y) * kWidth + x] = sourcePixel(x, y);
    }
  }
  return pixels;
}

std::vector<std::uint16_t> encodeSource(const std::vector<Pixel>& pixels) {
  // All source values are integers in the exactly representable binary16
  // range.  Their bit patterns are obtained through a tiny Metal shader-free
  // conversion using NSValue is undesirable here; use a float->half encoder
  // with round-to-nearest-even for the generality of the fixture.
  std::vector<std::uint16_t> encoded;
  encoded.reserve(pixels.size() * 4u);
  for (const Pixel& pixel : pixels) {
    for (const float channel : {pixel.r, pixel.g, pixel.b, pixel.a}) {
      std::uint32_t bits = 0u;
      std::memcpy(&bits, &channel, sizeof(bits));
      const std::uint32_t sign = (bits >> 16u) & 0x8000u;
      const std::int32_t exponent =
          static_cast<std::int32_t>((bits >> 23u) & 0xffu) - 127 + 15;
      const std::uint32_t fraction = bits & 0x7fffffu;
      if (exponent <= 0) {
        encoded.push_back(static_cast<std::uint16_t>(sign));
      } else if (exponent >= 31) {
        encoded.push_back(static_cast<std::uint16_t>(sign | 0x7c00u));
      } else {
        encoded.push_back(static_cast<std::uint16_t>(
            sign | (static_cast<std::uint32_t>(exponent) << 10u) |
            (fraction >> 13u)));
      }
    }
  }
  return encoded;
}

std::vector<Pixel> readLevel(WMT::Texture texture, std::uint32_t level,
                             std::uint32_t width, std::uint32_t height) {
  std::vector<std::uint16_t> bytes(static_cast<std::size_t>(width) * height * 4u);
  id<MTLTexture> metalTexture = (id<MTLTexture>)texture.handle;
  [metalTexture getBytes:bytes.data()
              bytesPerRow:static_cast<NSUInteger>(width) * 8u
            bytesPerImage:static_cast<NSUInteger>(width) * height * 8u
             fromRegion:MTLRegionMake2D(0, 0, width, height)
            mipmapLevel:level
                  slice:0];
  std::vector<Pixel> pixels;
  pixels.reserve(static_cast<std::size_t>(width) * height);
  for (std::size_t index = 0; index < static_cast<std::size_t>(width) * height;
       ++index) {
    pixels.push_back({halfToFloat(bytes[index * 4u]),
                      halfToFloat(bytes[index * 4u + 1u]),
                      halfToFloat(bytes[index * 4u + 2u]),
                      halfToFloat(bytes[index * 4u + 3u])});
  }
  return pixels;
}

std::vector<Pixel> downsample(const std::vector<Pixel>& source,
                              std::uint32_t width, std::uint32_t height) {
  const std::uint32_t nextWidth = std::max(1u, width / 2u);
  const std::uint32_t nextHeight = std::max(1u, height / 2u);
  std::vector<Pixel> result(static_cast<std::size_t>(nextWidth) * nextHeight);
  for (std::uint32_t y = 0; y < nextHeight; ++y) {
    for (std::uint32_t x = 0; x < nextWidth; ++x) {
      Pixel value{};
      for (std::uint32_t dy = 0; dy < 2u; ++dy) {
        for (std::uint32_t dx = 0; dx < 2u; ++dx) {
          const std::uint32_t sx = std::min(width - 1u, x * 2u + dx);
          const std::uint32_t sy = std::min(height - 1u, y * 2u + dy);
          const Pixel& sample = source[static_cast<std::size_t>(sy) * width + sx];
          value.r += sample.r;
          value.g += sample.g;
          value.b += sample.b;
          value.a += sample.a;
        }
      }
      value.r *= 0.25f;
      value.g *= 0.25f;
      value.b *= 0.25f;
      value.a *= 0.25f;
      result[static_cast<std::size_t>(y) * nextWidth + x] = value;
    }
  }
  return result;
}

void compareLevel(const std::vector<Pixel>& actual,
                  const std::vector<Pixel>& expected, std::uint32_t level) {
  check(actual.size() == expected.size(), "mip readback has expected size");
  float maxError = 0.0f;
  for (std::size_t index = 0; index < actual.size(); ++index) {
    const Pixel& a = actual[index];
    const Pixel& e = expected[index];
    maxError = std::max({maxError, std::fabs(a.r - e.r), std::fabs(a.g - e.g),
                         std::fabs(a.b - e.b), std::fabs(a.a - e.a)});
  }
  if (maxError > kTolerance) {
    std::cerr << "mip " << level << " max error " << maxError << '\n';
    fail("WMT generateMipmaps does not produce the expected 2x2 reduction");
  }
}

}  // namespace

int main() {
  dxmt9::core::TextureDesc autogenDesc{};
  autogenDesc.width = kWidth;
  autogenDesc.height = kHeight;
  autogenDesc.levels = 1u;
  autogenDesc.usage = dxmt9::core::UsageRenderTarget |
                      dxmt9::core::UsageAutoGenMipmap;
  check(dxmt9::resources::physicalTextureMipLevelCount(autogenDesc) ==
            kMipCount,
        "AUTOGEN public level 0 maps to an 11-level physical pyramid");
  autogenDesc.usage = dxmt9::core::UsageRenderTarget;
  check(dxmt9::resources::physicalTextureMipLevelCount(autogenDesc) == 1u,
        "non-AUTOGEN preserves its declared physical level count");

  @autoreleasepool {
    auto devices = WMT::CopyAllDevices();
    if (!devices || devices.count() == 0u) {
      std::cerr << "SKIP: no Metal device\n";
      return 77;
    }
    WMT::Device device = devices.object(0u);
    auto queue = device.newCommandQueue(2u);
    check(static_cast<bool>(queue), "WMT creates a command queue");

    WMTTextureInfo textureInfo{
        .pixel_format = WMTPixelFormatRGBA16Float,
        .width = kWidth,
        .height = kHeight,
        .depth = 1u,
        .array_length = 1u,
        .type = WMTTextureType2D,
        .mipmap_level_count = kMipCount,
        .sample_count = 1u,
        .usage = static_cast<WMTTextureUsage>(WMTTextureUsageShaderRead |
                                              WMTTextureUsageShaderWrite),
        .options = WMTResourceStorageModeShared,
        .reserved = 0u,
        .mach_port = 0u,
        .gpu_resource_id = 0u,
    };
    auto texture = device.newTexture(textureInfo);
    check(static_cast<bool>(texture), "WMT creates the RGBA16Float mip texture");
    check(texture.mipmapLevelCount() == kMipCount,
          "WMT texture has all 11 mip levels");

    const auto expectedSource = makeSource();
    const auto encodedSource = encodeSource(expectedSource);
    texture.replaceRegion(WMTOrigin{0u, 0u, 0u},
                          WMTSize{kWidth, kHeight, 1u}, 0u, 0u,
                          encodedSource.data(),
                          static_cast<std::uint64_t>(kWidth) * 8u, 0u);

    auto commandBuffer = queue.commandBuffer();
    check(static_cast<bool>(commandBuffer), "WMT creates a command buffer");
    auto blit = commandBuffer.blitCommandEncoder();
    check(static_cast<bool>(blit), "WMT creates a blit encoder");
    blit.generateMipmaps(texture);
    blit.endEncoding();
    commandBuffer.commit();
    commandBuffer.waitUntilCompleted();
    check(commandBuffer.status() == WMTCommandBufferStatusCompleted,
          "WMT generateMipmaps command buffer completes");

    auto expected = expectedSource;
    std::uint32_t width = kWidth;
    std::uint32_t height = kHeight;
    Pixel level1First{};
    for (std::uint32_t level = 0; level < kMipCount; ++level) {
      const auto actual = readLevel(texture, level, width, height);
      compareLevel(actual, expected, level);
      if (level == 1u) {
        level1First = actual.front();
      }
      if (level == 2u) {
        // A top-left crop of level 1 would preserve this exact pixel.  The
        // expected recursive reduction is intentionally far away from it.
        const float cropDelta = std::fabs(actual.front().r - level1First.r) +
                                std::fabs(actual.front().g - level1First.g) +
                                std::fabs(actual.front().b - level1First.b) +
                                std::fabs(actual.front().a - level1First.a);
        check(cropDelta > 1.0f,
              "mip2 is not a top-left crop of mip1");
      }
      if (level + 1u < kMipCount) {
        expected = downsample(expected, width, height);
        width = std::max(1u, width / 2u);
        height = std::max(1u, height / 2u);
      }
    }
    std::cout << "wmt_generate_mipmaps_metal_spec passed (RGBA16Float 1024x1024, "
              << kMipCount << " levels)\n";
  }
  return 0;
}
