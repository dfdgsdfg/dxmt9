// Production partial-clear oracle.  This intentionally drives the WMT
// command path through encoders::encodeClearPass, then reads the attachment
// back through the queue-owned synchronous readback helper.  The expected
// image is computed from the clear rectangles, never from the implementation.

#include "../../../src/dxmt9/dxmt9_blit_encoders.hpp"
#include "../../../src/dxmt9/dxmt9_command_queue.hpp"
#include "../../../src/dxmt9/dxmt9_resource_pool.hpp"
#include "../../../src/winemetal/Metal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using dxmt9::core::BackendLimits;
using dxmt9::core::ColorRGBA;
using dxmt9::core::ClearDesc;
using dxmt9::core::Format;
using dxmt9::core::MultiSampleType;
using dxmt9::core::Rect;
using dxmt9::core::SurfaceDesc;

constexpr std::uint32_t kWidth = 16u;
constexpr std::uint32_t kHeight = 16u;
using Pixel = std::array<std::uint8_t, 4>;

extern "C" bool dxmt9_read_depth_stencil(obj_handle_t textureHandle,
                                          std::uint32_t width,
                                          std::uint32_t height,
                                          float* depthOut,
                                          std::uint8_t* stencilOut);

[[noreturn]] void fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
  std::exit(1);
}

void check(bool condition, std::string_view message) {
  if (!condition) fail(message);
}

Pixel packed(ColorRGBA color) {
  const auto channel = [](float value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f);
  };
  // A8R8G8B8 is stored by Metal as BGRA8Unorm.
  return {channel(color.b), channel(color.g), channel(color.r), channel(color.a)};
}

void runClear(dxmt9::CommandQueue& queue, dxmt9::core::SurfaceHandle surface,
              ColorRGBA color, std::span<const Rect> rects,
              std::span<const dxmt9::core::RenderTargetAttachment> extra = {}) {
  auto commandBuffer = queue.newCommandBuffer();
  check(commandBuffer, "queue creates a clear command buffer");
  ClearDesc clear{};
  clear.colorAttachments[0].handle = surface;
  clear.clearColor = true;
  clear.color = color;
  clear.rects.assign(rects.begin(), rects.end());
  for (std::size_t i = 0; i < extra.size() && i + 1u < clear.colorAttachments.size(); ++i) {
    clear.colorAttachments[i + 1u] = extra[i];
  }
  dxmt9::encoders::encodeClearPass(commandBuffer, queue.pool(), queue.pipelineCache(),
                                   queue.device(), queue.limits(), nullptr, nullptr,
                                   clear);
  commandBuffer.commit();
  commandBuffer.waitUntilCompleted();
  check(commandBuffer.status() == WMTCommandBufferStatusCompleted,
        "partial-clear command buffer completes");
}

void runDepthStencilClear(dxmt9::CommandQueue& queue,
                          dxmt9::core::SurfaceHandle surface, bool depth,
                          bool stencil, float depthValue, std::uint32_t stencilValue,
                          std::span<const Rect> rects) {
  auto commandBuffer = queue.newCommandBuffer();
  check(commandBuffer, "queue creates a depth clear command buffer");
  ClearDesc clear{};
  clear.depthStencil.handle = surface;
  clear.clearDepth = depth;
  clear.clearStencil = stencil;
  clear.depth = depthValue;
  clear.stencil = stencilValue;
  clear.rects.assign(rects.begin(), rects.end());
  dxmt9::encoders::encodeClearPass(commandBuffer, queue.pool(), queue.pipelineCache(),
                                   queue.device(), queue.limits(), nullptr, nullptr,
                                   clear);
  commandBuffer.commit();
  commandBuffer.waitUntilCompleted();
  check(commandBuffer.status() == WMTCommandBufferStatusCompleted,
        "depth/stencil clear command buffer completes");
}

std::vector<std::uint8_t> readSurface(dxmt9::CommandQueue& queue,
                                      dxmt9::core::SurfaceHandle surface,
                                      std::uint32_t width = kWidth,
                                      std::uint32_t height = kHeight) {
  dxmt9::core::ReadbackDesc desc{};
  desc.source = surface;
  desc.sourceRect = Rect{0, 0, static_cast<std::int32_t>(width),
                         static_cast<std::int32_t>(height)};
  dxmt9::core::ReadbackPixels pixels{};
  check(dxmt9::encoders::readbackSurface(queue, queue.pool(), queue.device(), queue.limits(),
                                         desc, pixels),
        "surface readback succeeds");
  check(pixels.pitch == width * 4u &&
            pixels.bytes.size() == static_cast<std::size_t>(pixels.pitch) * height,
        "surface readback has tightly packed RGBA8 dimensions");
  return pixels.bytes;
}

void expectSolid(const std::vector<std::uint8_t>& bytes, Pixel expected,
                 std::string_view message) {
  for (std::size_t i = 0; i < bytes.size(); i += 4u) {
    if (!std::equal(expected.begin(), expected.end(), bytes.begin() + i)) fail(message);
  }
}

void expectRectImage(const std::vector<std::uint8_t>& bytes, Pixel base, Pixel fill,
                     std::span<const Rect> rects, std::string_view message,
                     std::uint32_t width = kWidth,
                     std::uint32_t height = kHeight) {
  check(bytes.size() == static_cast<std::size_t>(width) * height * 4u,
        "rect image readback has the expected bounded size");
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      bool covered = false;
      for (const Rect& rect : rects) {
        covered = covered || (static_cast<std::int32_t>(x) >= std::max(0, rect.left) &&
                              static_cast<std::int32_t>(x) < std::min<int>(width, rect.right) &&
                              static_cast<std::int32_t>(y) >= std::max(0, rect.top) &&
                              static_cast<std::int32_t>(y) < std::min<int>(height, rect.bottom));
      }
      const Pixel expected = covered ? fill : base;
      const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4u;
      if (!std::equal(expected.begin(), expected.end(), bytes.begin() + offset)) {
        fail(message);
      }
    }
  }
}

void expectClearSequence(
    const std::vector<std::uint8_t>& bytes, Pixel initial,
    std::span<const std::pair<Rect, Pixel>> operations,
    std::string_view message, std::uint32_t width = kWidth,
    std::uint32_t height = kHeight) {
  check(bytes.size() == static_cast<std::size_t>(width) * height * 4u,
        "composed image readback has the expected bounded size");
  std::vector<Pixel> expected(static_cast<std::size_t>(width) * height, initial);
  for (const auto& [rect, color] : operations) {
    const auto left = std::max(0, rect.left);
    const auto top = std::max(0, rect.top);
    const auto right = std::min(static_cast<int>(width), rect.right);
    const auto bottom = std::min(static_cast<int>(height), rect.bottom);
    for (int y = top; y < bottom; ++y) {
      for (int x = left; x < right; ++x) {
        expected[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)] = color;
      }
    }
  }
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto offset = (static_cast<std::size_t>(y) * width + x) * 4u;
      check(std::equal(expected[static_cast<std::size_t>(y) * width + x].begin(),
                       expected[static_cast<std::size_t>(y) * width + x].end(),
                       bytes.begin() + offset),
            message);
    }
  }
}

SurfaceDesc colorDesc(std::uint32_t width = kWidth, std::uint32_t height = kHeight,
                      MultiSampleType samples = MultiSampleType::None) {
  return SurfaceDesc{.width = width,
                     .height = height,
                     .format = Format::A8R8G8B8,
                     .pool = dxmt9::core::Pool::Default,
                     .usage = dxmt9::core::UsageRenderTarget,
                     .renderTarget = true,
                     .depthStencil = false,
                     .multiSampleType = samples};
}

void testMrtAndMultipleClippedRects(dxmt9::CommandQueue& queue) {
  auto first = queue.pool().createSurface(queue.device(), queue.limits(), colorDesc());
  auto second = queue.pool().createSurface(queue.device(), queue.limits(), colorDesc());
  check(first && second, "MRT surfaces are created");
  const ColorRGBA base{16.0f / 255.0f, 32.0f / 255.0f, 48.0f / 255.0f, 1.0f};
  const ColorRGBA fill{80.0f / 255.0f, 96.0f / 255.0f, 112.0f / 255.0f, 1.0f};
  const auto expectedBase = packed(base);
  const auto expectedFill = packed(fill);
  const std::array<Rect, 0> full{};
  runClear(queue, first, base, full, std::array{dxmt9::core::RenderTargetAttachment{second}});
  const std::array rects{Rect{-4, -3, 10, 10}, Rect{3, 3, 5, 5}, Rect{11, 11, 20, 20}};
  runClear(queue, first, fill, rects, std::array{dxmt9::core::RenderTargetAttachment{second}});
  const auto firstBytes = readSurface(queue, first);
  const auto secondBytes = readSurface(queue, second);
  expectRectImage(firstBytes, expectedBase, expectedFill, rects,
                  "MRT attachment 0 preserves pixels outside all clipped rects");
  expectRectImage(secondBytes, expectedBase, expectedFill, rects,
                  "MRT attachment 1 preserves pixels outside all clipped rects");
}

void testMipAliasAndMsaaLoadPreservation(dxmt9::CommandQueue& queue) {
  auto msaa = queue.pool().createSurface(queue.device(), queue.limits(),
                                         colorDesc(kWidth, kHeight, MultiSampleType::Four));
  check(static_cast<bool>(msaa), "MSAA surface is created");
  const ColorRGBA base{16.0f / 255.0f, 32.0f / 255.0f, 48.0f / 255.0f, 1.0f};
  const ColorRGBA fill{80.0f / 255.0f, 96.0f / 255.0f, 112.0f / 255.0f, 1.0f};
  const auto expectedBase = packed(base);
  const auto expectedFill = packed(fill);
  const std::array<Rect, 0> full{};
  runClear(queue, msaa, base, full);
  const std::array firstRect{Rect{4, 4, 12, 12}};
  runClear(queue, msaa, fill, firstRect);
  const std::array secondRect{Rect{8, 8, 14, 14}};
  runClear(queue, msaa, base, secondRect);
  const std::array operations{std::pair{firstRect[0], expectedFill},
                              std::pair{secondRect[0], expectedBase}};
  expectClearSequence(readSurface(queue, msaa), expectedBase, operations,
                      "MSAA resolve preserves the multisample source across a later Load pass");

  dxmt9::core::TextureDesc textureDesc{};
  textureDesc.width = kWidth;
  textureDesc.height = kHeight;
  textureDesc.levels = 2u;
  textureDesc.format = Format::A8R8G8B8;
  textureDesc.usage = dxmt9::core::UsageRenderTarget;
  const auto texture = queue.pool().createTexture(queue.device(), queue.limits(), textureDesc);
  check(static_cast<bool>(texture), "mipped texture is created");
  const auto level0 = queue.pool().createSurfaceForTexture(texture, 0u, colorDesc());
  const auto level1 = queue.pool().createSurfaceForTexture(texture, 1u, colorDesc(8u, 8u));
  check(level0 && level1, "mip-level surface views are created");
  runClear(queue, level0, base, full);
  runClear(queue, level1, fill, full);
  const std::array tiny{Rect{1, 1, 7, 7}};
  runClear(queue, level1, base, tiny);
  expectSolid(readSurface(queue, level0), packed(base),
              "clearing a mip view does not mutate level 0");
  expectRectImage(readSurface(queue, level1, 8u, 8u), packed(fill), packed(base), tiny,
                  "mip-level view clear preserves its untouched texels", 8u, 8u);

  dxmt9::core::TextureDesc cubeDesc{};
  cubeDesc.width = kWidth;
  cubeDesc.height = kHeight;
  cubeDesc.levels = 1u;
  cubeDesc.format = Format::A8R8G8B8;
  cubeDesc.type = dxmt9::core::TextureType::Cube;
  cubeDesc.usage = dxmt9::core::UsageRenderTarget;
  const auto cube = queue.pool().createTexture(queue.device(), queue.limits(), cubeDesc);
  check(static_cast<bool>(cube), "cube texture is created");
  const auto face0 = queue.pool().createSurfaceForTexture(cube, 0u, colorDesc());
  const auto face1 = queue.pool().createSurfaceForTexture(cube, 1u, colorDesc());
  check(face0 && face1, "cube face surface views are created");
  runClear(queue, face0, base, full);
  runClear(queue, face1, fill, full);
  runClear(queue, face1, base, tiny);
  expectSolid(readSurface(queue, face0), packed(base),
              "clearing cube face 1 does not mutate cube face 0");
  expectRectImage(readSurface(queue, face1), packed(fill), packed(base), tiny,
                  "cube face view clear preserves its untouched texels");
}

void testDepthStencilAspectPreservation(dxmt9::CommandQueue& queue) {
  SurfaceDesc desc{.width = kWidth,
                   .height = kHeight,
                   .format = Format::D24S8,
                   .pool = dxmt9::core::Pool::Default,
                   .usage = dxmt9::core::UsageDepthStencil,
                   .renderTarget = false,
                   .depthStencil = true,
                   .multiSampleType = MultiSampleType::None};
  const auto surface = queue.pool().createSurface(queue.device(), queue.limits(), desc);
  check(static_cast<bool>(surface), "D24S8 depth/stencil surface is created");
  auto* record = queue.pool().findSurface(surface.value);
  check(record && record->texture, "D24S8 surface exposes a Metal depth/stencil texture");
  const std::array<Rect, 0> full{};
  runDepthStencilClear(queue, surface, true, true, 0.25f, 0x12u, full);
  std::vector<float> seededDepth(kWidth * kHeight);
  std::vector<std::uint8_t> seededStencil(kWidth * kHeight);
  check(dxmt9_read_depth_stencil(record->texture.handle, kWidth, kHeight,
                                 seededDepth.data(), seededStencil.data()),
        "D24S8 depth/stencil readback succeeds");
  for (std::size_t i = 0; i < seededDepth.size(); ++i) {
    check(seededDepth[i] == 0.25f && seededStencil[i] == 0x12u,
          "full clear establishes exact independent depth and stencil values");
  }

  const std::array depthRect{Rect{4, 4, 12, 12}};
  runDepthStencilClear(queue, surface, true, false, 0.75f, 0u, depthRect);
  std::vector<float> depthOnly(kWidth * kHeight);
  std::vector<std::uint8_t> depthOnlyStencil(kWidth * kHeight);
  check(dxmt9_read_depth_stencil(record->texture.handle, kWidth, kHeight,
                                 depthOnly.data(), depthOnlyStencil.data()),
        "depth-only D24S8 readback succeeds");
  for (std::uint32_t y = 0; y < kHeight; ++y) {
    for (std::uint32_t x = 0; x < kWidth; ++x) {
      const bool inside = x >= 4u && x < 12u && y >= 4u && y < 12u;
      const std::size_t offset = static_cast<std::size_t>(y) * kWidth + x;
      check(depthOnly[offset] == (inside ? 0.75f : 0.25f),
            "depth-only clear writes the exact requested depth and preserves depth outside");
      check(depthOnlyStencil[offset] == 0x12u,
            "depth-only clear preserves the exact stencil aspect at every pixel");
    }
  }

  const std::array stencilRect{Rect{8, 8, 16, 16}};
  runDepthStencilClear(queue, surface, false, true, 0.0f, 0x1a5u, stencilRect);
  std::vector<float> stencilOnlyDepth(kWidth * kHeight);
  std::vector<std::uint8_t> stencilOnly(kWidth * kHeight);
  check(dxmt9_read_depth_stencil(record->texture.handle, kWidth, kHeight,
                                 stencilOnlyDepth.data(), stencilOnly.data()),
        "stencil-only D24S8 readback succeeds");
  for (std::uint32_t y = 0; y < kHeight; ++y) {
    for (std::uint32_t x = 0; x < kWidth; ++x) {
      const bool inside = x >= 8u && y >= 8u;
      const std::size_t offset = static_cast<std::size_t>(y) * kWidth + x;
      const bool depthWasPatched = x >= 4u && x < 12u && y >= 4u && y < 12u;
      check(stencilOnlyDepth[offset] == (depthWasPatched ? 0.75f : 0.25f),
            "stencil-only clear preserves the exact depth aspect at every pixel");
      check(stencilOnly[offset] == (inside ? 0xa5u : 0x12u),
            "stencil-only clear writes the low eight bits and preserves stencil outside");
    }
  }
}

}  // namespace

int main() {
  auto devices = WMT::CopyAllDevices();
  if (!devices || devices.count() == 0u) {
    std::cerr << "SKIP: no Metal device\n";
    return 77;
  }
  WMT::Device device = devices.object(0u);
  BackendLimits limits{};
  // Exercise the production float32/stencil8 fallback on Apple Silicon,
  // which does not expose the desktop-only packed depth24/stencil8 format.
  limits.supportsDepth24Stencil8 = false;
  dxmt9::CommandQueue queue(device, limits, false);
  check(queue.valid() && queue.started(), "native queue starts on the Metal device");
  testMrtAndMultipleClippedRects(queue);
  testMipAliasAndMsaaLoadPreservation(queue);
  testDepthStencilAspectPreservation(queue);
  std::cout << "partial_clear_spec passed (MRT, clipped, mip-view, and MSAA readback)\n";
  return 0;
}
