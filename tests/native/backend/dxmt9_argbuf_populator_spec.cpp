// R-BACK-12.22..12.26 — Stage 2 argbuf-hybrid populator spec.
//
// CPU-only spec for the per-encoder argbuf populator wired in this
// branch (commit `argbuf-populator`). Focus is on the value-transform
// shape — the actual Metal-side openArgbuf / populateConstantBuffers /
// populateResourceBindings calls require a live MTLDevice and are
// covered by the runtime-integration suite once shader-runner equality
// (R-BACK-12.26) lands. Here we exercise:
//
//   - ArgbufEncoderResource::initForTest sets the encodedLength /
//     alignment shape `openArgbuf` reads from on the hot path.
//   - dirtyBytesEstimate returns 0 when no per-frequency bit is set
//     and matches the four host-struct sizes when each category fires.
//   - dirtyBytesEstimate composes additively across categories so the
//     encoder counter accounts for every dirty bit.
//
// `openArgbuf` itself reaches into CommandQueue::reserveTransientBuffer
// which requires a Metal device; the runtime suite covers that path.

#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "../../../src/dxmt9/dxmt9_argbuf_hybrid.hpp"
#include "../../../src/dxmt9/dxmt9_draw_state.hpp"
#include "../../../src/dxmt9/dxmt9_uniform_dirty.hpp"

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
    out << message << " (left=" << static_cast<std::uint64_t>(left)
        << " right=" << static_cast<std::uint64_t>(right) << ")";
    fail(out.str());
  }
}

// ---------------------------------------------------------------------
// E1 — ArgbufEncoderResource shape

void testEncoderResourceDefaultUninitialized() {
  dxmt9::argbuf_hybrid::ArgbufEncoderResource res;
  check(!res.initialized(),
        "default-constructed ArgbufEncoderResource is uninitialized");
  checkEq(res.encodedLength(), std::uint64_t{0},
          "encodedLength is 0 before init");
  // Default alignment is 16 — matches the constant-block alignment of
  // the four [[id(0..3)]] cbuf entries.
  checkEq(res.alignment(), std::uint64_t{16},
          "alignment defaults to 16 B before init");
}

void testEncoderResourceInitForTestPopulatesShape() {
  dxmt9::argbuf_hybrid::ArgbufEncoderResource res;
  res.initForTest(/*encodedLength=*/512u, /*alignment=*/32u);
  check(res.initialized(), "initForTest flips initialized");
  checkEq(res.encodedLength(), std::uint64_t{512u},
          "initForTest sets encodedLength");
  checkEq(res.alignment(), std::uint64_t{32u},
          "initForTest sets alignment when >=16");
}

void testEncoderResourceAlignmentFloor() {
  // Test fixtures may pass alignment=0 / alignment=4 — the resource
  // floors alignment to 16 so reservations always satisfy the
  // constant-block alignment of the cbuf entries.
  dxmt9::argbuf_hybrid::ArgbufEncoderResource res;
  res.initForTest(/*encodedLength=*/256u, /*alignment=*/0u);
  checkEq(res.alignment(), std::uint64_t{16u},
          "alignment is floored to 16 even when caller passes 0");
}

// ---------------------------------------------------------------------
// E2 — dirtyBytesEstimate

void testDirtyBytesEstimateEmpty() {
  dxmt9::uniform::DirtyState clean{};
  checkEq(dxmt9::argbuf_hybrid::dirtyBytesEstimate(clean),
          std::uint64_t{0},
          "no dirty bits => 0 bytes rewritten");
}

void testDirtyBytesEstimateVsAny() {
  dxmt9::uniform::DirtyState s{};
  dxmt9::uniform::setBit(s, dxmt9::uniform::DirtyBit::VsF);
  checkEq(dxmt9::argbuf_hybrid::dirtyBytesEstimate(s),
          std::uint64_t{sizeof(dxmt9::state::VsConsts)},
          "VsF dirty => sizeof(VsConsts) bytes");
}

void testDirtyBytesEstimatePsAny() {
  dxmt9::uniform::DirtyState s{};
  dxmt9::uniform::setBit(s, dxmt9::uniform::DirtyBit::PsB);
  checkEq(dxmt9::argbuf_hybrid::dirtyBytesEstimate(s),
          std::uint64_t{sizeof(dxmt9::state::PsConsts)},
          "PsB dirty => sizeof(PsConsts) bytes");
}

void testDirtyBytesEstimateFfpVsAny() {
  dxmt9::uniform::DirtyState s{};
  dxmt9::uniform::setBit(s, dxmt9::uniform::DirtyBit::FfpVsViewport);
  checkEq(dxmt9::argbuf_hybrid::dirtyBytesEstimate(s),
          std::uint64_t{sizeof(dxmt9::state::FfpVsConsts)},
          "FfpVsViewport dirty => sizeof(FfpVsConsts) bytes");
}

void testDirtyBytesEstimateFfpPsAny() {
  dxmt9::uniform::DirtyState s{};
  dxmt9::uniform::setBit(s, dxmt9::uniform::DirtyBit::FfpPsAlpha);
  checkEq(dxmt9::argbuf_hybrid::dirtyBytesEstimate(s),
          std::uint64_t{sizeof(dxmt9::state::FfpPsConsts)},
          "FfpPsAlpha dirty => sizeof(FfpPsConsts) bytes");
}

void testDirtyBytesEstimateAllCategories() {
  // markAllDirty sets every category bit; the byte total must equal
  // the sum of the four host-struct sizes (Stage 1 worst case).
  dxmt9::uniform::DirtyState s{};
  dxmt9::uniform::markAllDirty(s);
  const std::uint64_t expected = sizeof(dxmt9::state::VsConsts) +
                                  sizeof(dxmt9::state::PsConsts) +
                                  sizeof(dxmt9::state::FfpVsConsts) +
                                  sizeof(dxmt9::state::FfpPsConsts);
  checkEq(dxmt9::argbuf_hybrid::dirtyBytesEstimate(s), expected,
          "all-dirty mask => sum of four per-frequency host structs");
}

void testDirtyBytesEstimateAdditive() {
  // Two non-overlapping categories should compose additively. Use VsF +
  // FfpPsTexFactor — they hit disjoint dirty masks.
  dxmt9::uniform::DirtyState a{};
  dxmt9::uniform::setBit(a, dxmt9::uniform::DirtyBit::VsF);
  dxmt9::uniform::DirtyState b{};
  dxmt9::uniform::setBit(b, dxmt9::uniform::DirtyBit::FfpPsTexFactor);
  dxmt9::uniform::DirtyState ab{};
  dxmt9::uniform::setBit(ab, dxmt9::uniform::DirtyBit::VsF);
  dxmt9::uniform::setBit(ab, dxmt9::uniform::DirtyBit::FfpPsTexFactor);
  const auto sum = dxmt9::argbuf_hybrid::dirtyBytesEstimate(a) +
                   dxmt9::argbuf_hybrid::dirtyBytesEstimate(b);
  checkEq(dxmt9::argbuf_hybrid::dirtyBytesEstimate(ab), sum,
          "VsF + FfpPsTexFactor => additive byte total");
}

// ---------------------------------------------------------------------
// E3 — PopulatedArgbuf default + boolean conversion

void testPopulatedArgbufDefaultIsEmpty() {
  dxmt9::argbuf_hybrid::PopulatedArgbuf p{};
  check(!static_cast<bool>(p),
        "default-constructed PopulatedArgbuf is empty (length==0)");
  checkEq(p.length, std::uint64_t{0}, "default length is 0");
  checkEq(p.offset, std::uint64_t{0}, "default offset is 0");
}

}  // namespace

int main() {
  try {
    testEncoderResourceDefaultUninitialized();
    testEncoderResourceInitForTestPopulatesShape();
    testEncoderResourceAlignmentFloor();
    testDirtyBytesEstimateEmpty();
    testDirtyBytesEstimateVsAny();
    testDirtyBytesEstimatePsAny();
    testDirtyBytesEstimateFfpVsAny();
    testDirtyBytesEstimateFfpPsAny();
    testDirtyBytesEstimateAllCategories();
    testDirtyBytesEstimateAdditive();
    testPopulatedArgbufDefaultIsEmpty();
  } catch (const TestFailure& failure) {
    std::cerr << "argbuf_populator_spec failed: " << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "argbuf_populator_spec unexpected exception: " << ex.what() << '\n';
    return 1;
  }
  std::cout << "argbuf_populator_spec passed\n";
  return 0;
}
