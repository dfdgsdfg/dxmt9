#include "device_c_render_tape_descriptors.hpp"
#include "device_c_render_tape_snapshot_staging.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace dxmt9::d3d9;
using Family = RenderTapeSnapshotStagingFamily;

void check(bool value, std::string_view message) {
  if (!value) throw std::runtime_error(std::string(message));
}

D9CSurfaceDesc descriptor(std::uint32_t format, std::uint32_t usage,
                         std::uint32_t pool, std::uint32_t type,
                         std::uint32_t width = 63u,
                         std::uint32_t height = 7u) {
  return {.format = format, .resourceType = type, .usage = usage, .pool = pool,
          .width = width, .height = height, .depth = 1u};
}

void checkPlan(Family family, std::uint64_t bytes) {
  RenderTapeSnapshotStagingPlan plan{};
  check(renderTapeDeriveSnapshotStagingPlan(family, bytes, plan),
        "admitted snapshot gets a checked staging plan");
  check(plan.logicalRetainedBytes == bytes &&
            plan.extraHostPayloadBytes == bytes * 2u,
        "logical retention and conservative two-vector host allowance are distinct");
}

void checkDenied(Family family, std::uint64_t bytes) {
  RenderTapeSnapshotStagingPlan plan{99u, 199u};
  check(!renderTapeDeriveSnapshotStagingPlan(family, bytes, plan),
        "invalid/overflow/over-limit staging request is denied");
  check(plan.logicalRetainedBytes == 0u && plan.extraHostPayloadBytes == 0u,
        "denial does not expose an old or partial allocation plan");
}

void testExactLimitsOverflowAndUnknownFamily() {
  static_assert(kRenderTapeSnapshotPerResourceHardLimitBytes == 64ull * 1024ull * 1024ull);
  static_assert(kRenderTapeSnapshotTransientHardLimitBytes == 128ull * 1024ull * 1024ull);
  constexpr auto cap = kRenderTapeSnapshotPerResourceHardLimitBytes;
  const std::array families{Family::Color, Family::D24X8, Family::D24S8};
  for (const auto family : families) {
    for (std::uint64_t bytes = 1u; bytes <= 257u; ++bytes) checkPlan(family, bytes);
    checkPlan(family, cap - 1u);
    checkPlan(family, cap);
    checkDenied(family, 0u);
    checkDenied(family, cap + 1u);
    checkDenied(family, std::numeric_limits<std::uint64_t>::max() / 2u + 1u);
    checkDenied(family, std::numeric_limits<std::uint64_t>::max());
  }
  checkDenied(static_cast<Family>(255u), 1u);
}

void testEverySupportedColorLayoutKeepsTightRows() {
  struct Case {
    D9CSurfaceDesc surface;
    std::uint32_t bytesPerPixel;
  };
  const std::array cases{
      Case{descriptor(22u, 1u, 0u, 1u), 4u}, // standalone/swapchain X8R8G8B8
      Case{descriptor(22u, 1u, 0u, 3u), 4u}, // DEFAULT X8R8G8B8 2D
      Case{descriptor(21u, 1u, 0u, 3u), 4u}, // DEFAULT A8R8G8B8 2D
      Case{descriptor(21u, 0u, 1u, 3u), 4u}, // MANAGED A8R8G8B8 2D
      Case{descriptor(113u, 1u, 0u, 3u), 8u}, // RGBA16Float 2D
      Case{descriptor(114u, 1u, 0u, 3u), 4u}, // R32Float 2D
      Case{descriptor(114u, 1u, 0u, 5u, 63u, 63u), 4u}, // one R32Float cube face
  };
  for (const auto& item : cases) {
    RenderTapeColorSnapshotLayout layout{};
    check(renderTapeDeriveColorSnapshotLayout(item.surface, layout),
          "existing supported color grammar derives its layout");
    check(layout.pitch == item.surface.width * item.bytesPerPixel &&
              layout.byteCount == static_cast<std::uint64_t>(layout.pitch) *
                                      item.surface.height &&
              layout.pitch % 256u != 0u,
          "odd-width color rows are tight, not rounded to 256 bytes");
    checkPlan(Family::Color, layout.byteCount);
  }
  for (const auto bpp : {4u, 8u}) {
    const auto surface = descriptor(bpp == 8u ? 113u : 22u, 1u, 0u, 3u,
                                    4096u, bpp == 8u ? 2048u : 4096u);
    RenderTapeColorSnapshotLayout layout{};
    check(renderTapeDeriveColorSnapshotLayout(surface, layout) &&
              layout.byteCount == kRenderTapeSnapshotPerResourceHardLimitBytes,
          "both color encodings can reach the exact resource cap");
    checkPlan(Family::Color, layout.byteCount);
    auto over = surface;
    ++over.height;
    check(renderTapeDeriveColorSnapshotLayout(over, layout),
          "mathematically valid larger color retains its existing grammar");
    checkDenied(Family::Color, layout.byteCount);
  }
}

void testBothDepthLayoutsAndTheirNearCapBoundaries() {
  const auto d24x8 = descriptor(77u, 2u, 0u, 1u);
  check(renderTapeSnapshotStandaloneD24X8Supported(d24x8),
        "existing D24X8 grammar admits the tight float32 checkpoint");
  const auto d24x8Bytes = static_cast<std::uint64_t>(d24x8.width) *
      d24x8.height * 4u;
  checkPlan(Family::D24X8, d24x8Bytes);
  checkPlan(Family::D24X8, 4096ull * 4096ull * 4u);
  checkDenied(Family::D24X8, 4096ull * 4097ull * 4u);

  auto d24s8 = descriptor(75u, 2u, 0u, 1u);
  RenderTapeD24S8SnapshotLayout layout{};
  check(renderTapeDeriveD24S8SnapshotLayout(d24s8, layout),
        "existing D24S8 grammar derives both planes");
  const auto pixels = static_cast<std::uint64_t>(d24s8.width) * d24s8.height;
  check(layout.depthPitch == d24s8.width * 4u &&
            layout.stencilPitch == d24s8.width &&
            layout.stencilOffset == pixels * 4u && layout.byteCount == pixels * 5u,
        "D24S8 has tight float32 depth followed by tight uint8 stencil");
  checkPlan(Family::D24S8, layout.byteCount);
  d24s8.width = 1u;
  d24s8.height = static_cast<std::uint32_t>(
      kRenderTapeSnapshotPerResourceHardLimitBytes / 5u);
  check(renderTapeDeriveD24S8SnapshotLayout(d24s8, layout),
        "last whole dual-plane pixel below cap has a valid layout");
  checkPlan(Family::D24S8, layout.byteCount);
  ++d24s8.height;
  check(renderTapeDeriveD24S8SnapshotLayout(d24s8, layout),
        "first whole dual-plane pixel above cap retains mathematical validity");
  checkDenied(Family::D24S8, layout.byteCount);
}

std::string_view functionBody(std::string_view source, std::string_view name) {
  const auto begin = source.find(name);
  check(begin != std::string_view::npos, "production snapshot definition is present");
  const auto end = source.find("\nextern \"C\"", begin + name.size());
  return source.substr(begin, end == std::string_view::npos ? end : end - begin);
}

void checkSourceGuard(std::string_view body, std::string_view family,
                      std::string_view logicalBytes,
                      std::initializer_list<std::string_view> effects) {
  constexpr std::string_view call = "renderTapeDeriveSnapshotStagingPlan(";
  const auto guard = body.find("if (!dxmt9::d3d9::renderTapeDeriveSnapshotStagingPlan(");
  check(guard != std::string_view::npos, "production actually branches on the shared plan");
  const auto end = body.find("\n  }", guard);
  check(end != std::string_view::npos, "production plan-denial branch is bounded");
  const auto denial = body.substr(guard, end - guard);
  check(denial.find(family) != std::string_view::npos &&
            denial.find(logicalBytes) != std::string_view::npos &&
            denial.find("return dxmt9::core::D3DERR_NOTAVAILABLE;") != std::string_view::npos,
        "production uses the right family/logical extent and returns on denial");
  const auto firstCall = body.find(call);
  check(body.find(call, firstCall + call.size()) == std::string_view::npos,
        "each snapshot has one authoritative preallocation plan guard");
  for (const auto effect : effects) {
    const auto position = body.find(effect);
    check(position != std::string_view::npos && position > end,
          "denial precedes registry materialization/provider drain/readback allocation");
  }
}

void testProductionBindings(const char* sourcePath) {
  std::ifstream file(sourcePath);
  check(file.good(), "production source supplied by the test runner is readable");
  const std::string source{std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>()};
  checkSourceGuard(functionBody(source,
      "extern \"C\" int32_t dxmt9c_device_capture_render_tape_color_snapshot("),
      "RenderTapeSnapshotStagingFamily::Color", "layout.byteCount",
      {"resolveAndRetain", "dxmt9c_texture_get_surface_level", "drainDeferredReplay",
       "upper->flush()", "captureRenderTapeColorTexture", "upper->readbackSurface("});
  checkSourceGuard(functionBody(source,
      "extern \"C\" int32_t dxmt9c_device_capture_render_tape_d24x8_snapshot("),
      "RenderTapeSnapshotStagingFamily::D24X8", "expectedBytes",
      {"resolveAndRetain", "drainDeferredReplay", "upper->flush()",
       "captureCanonicalD24X8Depth("});
  checkSourceGuard(functionBody(source,
      "extern \"C\" int32_t dxmt9c_device_capture_render_tape_d24s8_snapshot("),
      "RenderTapeSnapshotStagingFamily::D24S8", "layout.byteCount",
      {"resolveAndRetain", "drainDeferredReplay", "captureCanonicalD24S8DepthStencil("});
}

} // namespace

int main(int argc, char** argv) {
  try {
    check(argc == 2, "usage: render_tape_snapshot_staging_spec PROVIDER_CPP_PATH");
    testExactLimitsOverflowAndUnknownFamily();
    testEverySupportedColorLayoutKeepsTightRows();
    testBothDepthLayoutsAndTheirNearCapBoundaries();
    testProductionBindings(argv[1]);
    std::cout << "Render Tape snapshot staging spec passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "Render Tape snapshot staging spec failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
