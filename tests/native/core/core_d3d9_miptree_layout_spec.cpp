// Wine behavioral oracle (LGPL — observable contract only, not source):
//   dlls/d3d9/tests/device.c :: test_miptree_layout()
//   wine commit 6e073d28dee3af7f4c965daec94644e0f9f92727
//
// Verifies that dxmt9's per-level miptree dimension + pitch + byte-size
// calculation matches the D3D9 NPOT mip chain for typical 2D textures.
// Pure value-level: no Wine, no Metal, no device construction.
//
// Mip count rule: levels = 1 + floor(log2(max(width, height))) for full
// chain (`Levels = 0`). For block-compressed formats the per-level
// dimension is rounded UP to the block size when computing the row
// pitch (dxmt9::core::formatRowPitch already encodes this).

#include "core_spec_fixtures.hpp"

using namespace dxmt9::core;
using namespace dxmt9::core::spec;

namespace {

// Mirrors dxmt9::d3d9::devicec::fullMipLevelCount (anonymous namespace
// in src/d3d9/device_c_resources.cpp). Inlined so the spec can call it
// without invoking the C device entry point.
u32 fullMipLevelCount(u32 width, u32 height, u32 depth) {
  u32 dimension = std::max({width, height, depth});
  u32 levels = 1;
  while (dimension > 1u) {
    dimension >>= 1u;
    ++levels;
  }
  return levels;
}

// floor(dimension / 2^level) clamped to 1 (matches D3D9 / Metal).
u32 levelExtent(u32 base, u32 level) {
  return std::max(1u, base >> std::min(level, 31u));
}

struct LevelDims {
  u32 width;
  u32 height;
};

void testFullChainA8R8G8B8_256x256() {
  const Format format = Format::A8R8G8B8;
  const u32 baseWidth = 256;
  const u32 baseHeight = 256;
  const u32 levels = fullMipLevelCount(baseWidth, baseHeight, 1u);
  checkEq(levels, 9u, "A8R8G8B8 256x256 produces 9 mip levels");

  // Expected dims per level: 256, 128, 64, 32, 16, 8, 4, 2, 1.
  const std::array<LevelDims, 9> expected{{
      {256, 256}, {128, 128}, {64, 64}, {32, 32}, {16, 16},
      {8, 8}, {4, 4}, {2, 2}, {1, 1},
  }};

  // Each level is 4 bytes per pixel; verify pitch + byte size match
  // the D3D9 observable contract.
  for (u32 i = 0; i < levels; ++i) {
    const u32 w = levelExtent(baseWidth, i);
    const u32 h = levelExtent(baseHeight, i);
    checkEq(w, expected[i].width, "A8R8G8B8 mip width");
    checkEq(h, expected[i].height, "A8R8G8B8 mip height");
    checkEq(formatRowPitch(format, w), w * 4u, "A8R8G8B8 mip row pitch");
    checkEq(formatByteSize(format, w, h), std::size_t{w} * h * 4u,
            "A8R8G8B8 mip byte size");
  }
}

void testFullChainA8R8G8B8_257x129() {
  const Format format = Format::A8R8G8B8;
  const u32 baseWidth = 257;
  const u32 baseHeight = 129;
  const u32 levels = fullMipLevelCount(baseWidth, baseHeight, 1u);
  // max(257, 129) = 257, floor(log2(257)) = 8, so 9 levels.
  checkEq(levels, 9u, "A8R8G8B8 257x129 produces 9 mip levels");

  // Level 0 dimensions are the base; the last level always shrinks to 1.
  checkEq(levelExtent(baseWidth, 0), 257u, "A8R8G8B8 NPOT level0 width");
  checkEq(levelExtent(baseHeight, 0), 129u, "A8R8G8B8 NPOT level0 height");
  const u32 lastLevel = levels - 1u;
  checkEq(levelExtent(baseWidth, lastLevel), 1u, "A8R8G8B8 NPOT last level width");
  checkEq(levelExtent(baseHeight, lastLevel), 1u, "A8R8G8B8 NPOT last level height");

  // Spot-check level 1: width=128 (257>>1=128), height=64 (129>>1=64).
  checkEq(levelExtent(baseWidth, 1), 128u, "A8R8G8B8 NPOT level1 width");
  checkEq(levelExtent(baseHeight, 1), 64u, "A8R8G8B8 NPOT level1 height");
  // Pitch tracks the truncated level extent — no block-rounding for
  // uncompressed formats.
  checkEq(formatRowPitch(format, 128u), 128u * 4u,
          "A8R8G8B8 NPOT level1 row pitch");
}

void testFullChainDXT1_64x64() {
  const Format format = Format::DXT1;
  check(isCompressedFormat(format), "DXT1 is a block-compressed format");
  checkEq(formatBlockWidth(format), 4u, "DXT1 block width");
  checkEq(formatBlockHeight(format), 4u, "DXT1 block height");
  checkEq(formatBlockBytes(format), 8u, "DXT1 block bytes");

  const u32 baseWidth = 64;
  const u32 baseHeight = 64;
  const u32 levels = fullMipLevelCount(baseWidth, baseHeight, 1u);
  // log2(64) = 6, so 7 mip levels.
  checkEq(levels, 7u, "DXT1 64x64 produces 7 mip levels");

  // Level extents: 64, 32, 16, 8, 4, 2, 1.
  // For block-compressed formats: even when the level extent falls
  // below the 4x4 block size (levels 5 and 6 here), formatRowPitch /
  // formatByteSize round UP to one block. That is the D3D9 observable
  // pitch contract — a 2x2 DXT1 surface still has an 8-byte row pitch
  // covering one 4x4 block.
  const std::array<u32, 7> expectedExtents{{64u, 32u, 16u, 8u, 4u, 2u, 1u}};
  const std::array<u32, 7> expectedPitch{{
      // 64/4*8, 32/4*8, 16/4*8, 8/4*8, 4/4*8, ceil(2/4)*8, ceil(1/4)*8.
      128u, 64u, 32u, 16u, 8u, 8u, 8u,
  }};
  const std::array<std::size_t, 7> expectedBytes{{
      // rowCount * rowPitch. For levels 5 and 6 the row count rounds up
      // to one block as well, so the byte size is one 8-byte block.
      128u * 16u, 64u * 8u, 32u * 4u, 16u * 2u, 8u * 1u, 8u, 8u,
  }};

  for (u32 i = 0; i < levels; ++i) {
    const u32 w = levelExtent(baseWidth, i);
    const u32 h = levelExtent(baseHeight, i);
    checkEq(w, expectedExtents[i], "DXT1 mip extent (width)");
    checkEq(h, expectedExtents[i], "DXT1 mip extent (height)");
    checkEq(formatRowPitch(format, w), expectedPitch[i],
            "DXT1 mip row pitch (block-aligned)");
    checkEq(formatByteSize(format, w, h), expectedBytes[i],
            "DXT1 mip byte size (block-aligned)");
  }

  // Level 5 (extent 2): block-aligned dims are 4x4, byte size = 8.
  checkEq(formatByteSize(format, 2u, 2u), std::size_t{8u},
          "DXT1 sub-block level still costs one full 8-byte block");
}

void testFullChainDXT1_4x4() {
  // 4x4 is exactly one DXT1 block at the base level. Wine's
  // wined3d_log2i(4) + 1 = 3 yields a 3-level chain (4x4, 2x2, 1x1).
  // dxmt9 matches this: the mip-count formula does not stop at the
  // compressed block size — it walks until max(dim) == 1. The
  // sub-block levels still report a single-block row pitch / byte
  // size, which is the D3D9 lock-rect contract for those tiny mips.
  const Format format = Format::DXT1;
  const u32 baseWidth = 4;
  const u32 baseHeight = 4;
  const u32 levels = fullMipLevelCount(baseWidth, baseHeight, 1u);
  checkEq(levels, 3u, "DXT1 4x4 produces 3 mip levels (matches Wine wined3d_log2i+1)");

  checkEq(levelExtent(baseWidth, 0), 4u, "DXT1 4x4 level0 extent");
  checkEq(levelExtent(baseWidth, 1), 2u, "DXT1 4x4 level1 extent");
  checkEq(levelExtent(baseWidth, 2), 1u, "DXT1 4x4 level2 extent");

  // Every level — even the sub-block 2x2 and 1x1 — bills as one 8-byte
  // block; the lock-rect pitch the runtime returns is 8.
  for (u32 i = 0; i < levels; ++i) {
    const u32 w = levelExtent(baseWidth, i);
    const u32 h = levelExtent(baseHeight, i);
    checkEq(formatRowPitch(format, w), 8u,
            "DXT1 4x4 mip row pitch is one block per row");
    checkEq(formatByteSize(format, w, h), std::size_t{8u},
            "DXT1 4x4 mip byte size is one block");
  }
}

void testMipCountNonSquareLogPick() {
  // The mip-count derivation picks the log of max(width, height,
  // depth) — confirm a few corner cases.
  checkEq(fullMipLevelCount(1u, 1u, 1u), 1u, "1x1 has exactly 1 level");
  checkEq(fullMipLevelCount(2u, 1u, 1u), 2u, "2x1 has 2 levels");
  checkEq(fullMipLevelCount(1024u, 1u, 1u), 11u, "1024x1 has 11 levels");
  checkEq(fullMipLevelCount(1u, 1024u, 1u), 11u, "1x1024 has 11 levels");
  checkEq(fullMipLevelCount(8u, 8u, 64u), 7u, "volume picks depth");
}

}  // namespace

int main() {
  try {
    testFullChainA8R8G8B8_256x256();
    testFullChainA8R8G8B8_257x129();
    testFullChainDXT1_64x64();
    testFullChainDXT1_4x4();
    testMipCountNonSquareLogPick();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
