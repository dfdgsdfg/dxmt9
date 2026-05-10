// Regression test for the wow64 texture-lock shadow allocation upper
// bound.
//
// SFIV crashed 24s into the benchmark with a page-fault one 4KB page
// past the start of a shadow buffer that dxmt9 returned for a BC3
// level-9 lock on a 256x256 texture:
//
//   texture_lock_rect shadow texture=0x6000001bad80 level=9
//     nativeBits=... shadowBits=0x1a4b0000
//     pitch=1024 rowBytes=16 rows=1
//   ...
//   wine: Unhandled page fault on write access to 1A4B1000 ...
//
// `rowBytes * rows = 16` and `pitch * rows = 1024` were both far below
// the 4KB page that allocateLow4GB() actually returned, so any out-of-
// block write fell into unmapped memory.
//
// The helper computeShadowBytesUpperBound() must size the shadow against
// the worst-case texel-row walk of the lock pointer, padded to a full
// block (BC formats require at least blockHeight texels of vertical
// span in the backing storage). See the comment block on
// computeShadowBytesUpperBound() in device_c_common.hpp.

#include "device_c_common.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw TestFailure(std::move(message));
}

void check(bool condition, std::string_view message) {
  if (!condition) {
    fail(std::string(message));
  }
}

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    std::ostringstream out;
    out << message << " (" << left << " vs " << right << ")";
    fail(out.str());
  }
}

using dxmt9::d3d9::devicec::computeShadowBytesUpperBound;

void testSfivBc3Level9RegressionShadowFloorClearsObservedFaultOffsets() {
  // SFIV repro inputs: BC3 256x256 base, level 9 -> 1x1 mip, native
  // pitch reported as 1024 bytes (the parent-level row alignment Metal
  // hands back for the tiny mip's backing buffer), blockHeight = 4.
  const size_t bytes = computeShadowBytesUpperBound(/*nativePitch=*/1024,
                                                    /*rectHeight=*/1,
                                                    /*blockHeight=*/4);
  // Two observed fault offsets — +0x1000 (first run) and +0x2000
  // (second run after the initial padded+slack fix). The floor must
  // strictly exceed both so the post-buffer page stays mapped under
  // either game write pattern.
  check(bytes > 0x1000u,
        "BC3 1x1 shadow must exceed offset 0x1000 (first observed fault)");
  check(bytes > 0x2000u,
        "BC3 1x1 shadow must exceed offset 0x2000 (second observed fault)");
  // The floor formula is `pitch * blockHeight * 4` = 1024 * 4 * 4 = 16384,
  // i.e., four 4KB pages — leaves comfortable headroom past 0x2000.
  checkEq(bytes, size_t{16384},
          "BC3 1x1 shadow floor (pitch * blockHeight * 4)");
}

void testNonCompressedFormatIsPitchTimesHeight() {
  // 256x32 BGRA8 surface, pitch 1024, full lock — uncompressed format
  // has blockHeight == 1 so the natural pitch*height bound (32 KB)
  // exceeds the floor (pitch * 1 * 4 = 4 KB) and is preserved.
  const size_t bytes = computeShadowBytesUpperBound(/*nativePitch=*/1024,
                                                    /*rectHeight=*/32,
                                                    /*blockHeight=*/1);
  checkEq(bytes, size_t{1024} * 32u,
          "uncompressed lock above the floor keeps pitch*height");
}

void testCompressedRectHeightRoundsUpToBlock() {
  // BC3 with locked rect height 5 texels (not block-aligned) rounds up
  // to the next block boundary (8 texel rows = 2 block rows). pitch=512
  // gives padded bytes = 4096; floor = 512 * 4 * 4 = 8192 — the floor
  // wins and protects against the same overrun pattern.
  const size_t bytes = computeShadowBytesUpperBound(/*nativePitch=*/512,
                                                    /*rectHeight=*/5,
                                                    /*blockHeight=*/4);
  checkEq(bytes, size_t{8192},
          "small partial compressed rect is lifted to the floor");
}

void testCompressedLargeMipKeepsNaturalBound() {
  // BC3 256x256 full lock: padded bytes = pitch * 256 = 256 KB which
  // dwarfs the floor — natural bound is preserved.
  const size_t bytes = computeShadowBytesUpperBound(/*nativePitch=*/1024,
                                                    /*rectHeight=*/256,
                                                    /*blockHeight=*/4);
  checkEq(bytes, size_t{1024} * 256u,
          "BC3 256x256 lock keeps pitch*height (well above floor)");
}

void testZeroDimensionsReturnZero() {
  // Defensive: an empty rect or zero pitch returns 0 so the caller's
  // existing zero-byte short circuit still applies.
  checkEq(computeShadowBytesUpperBound(0, 4, 1), size_t{0},
          "zero pitch returns zero shadow bytes");
  checkEq(computeShadowBytesUpperBound(1024, 0, 4), size_t{0},
          "zero rect height returns zero shadow bytes");
}

void testBlockHeightZeroFallsBackToOne() {
  // A misconfigured format should not break the math; blockHeight==0
  // behaves like blockHeight==1. Floor = pitch * 1 * 4 = 256, padded
  // = 64*3 = 192 — floor wins.
  const size_t bytes = computeShadowBytesUpperBound(/*nativePitch=*/64,
                                                    /*rectHeight=*/3,
                                                    /*blockHeight=*/0);
  checkEq(bytes, size_t{256},
          "zero blockHeight is treated as 1 and protected by the floor");
}

}  // namespace

int main() {
  try {
    testSfivBc3Level9RegressionShadowFloorClearsObservedFaultOffsets();
    testNonCompressedFormatIsPitchTimesHeight();
    testCompressedRectHeightRoundsUpToBlock();
    testCompressedLargeMipKeepsNaturalBound();
    testZeroDimensionsReturnZero();
    testBlockHeightZeroFallsBackToOne();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
