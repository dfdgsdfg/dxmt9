// R-BACK-12.22..12.26 — Stage 2 argbuf-hybrid populator spec.
//
// CPU-only spec for the per-encoder argbuf populator. Stage 2 is
// constants-only: the argbuf carries the four per-frequency
// constant-buffer pointers and texture/sampler resources stay on the
// validated direct render-encoder binding lane. The actual Metal-side
// openArgbuf / populateConstantBuffers calls require a live MTLDevice;
// here we exercise the CPU-only value-transform shape:
//
//   - ArgbufEncoderResource::initForTest sets the encodedLength /
//     alignment shape `openArgbuf` reads from on the hot path.
//   - dirtyBytesEstimate returns 0 when no per-frequency bit is set
//     and matches the four host-struct sizes when each category fires.
//   - dirtyBytesEstimate composes additively across categories so the
//     encoder counter accounts for every dirty bit.
//   - the FfpVs repoint recorder seam captures the slot 1 argbuf write
//     ordering without a live Metal device.

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

struct RecordedCommand {
  obj_handle_t handle = 0;
  std::uint64_t offset = 0;
  std::uint32_t index = 0;
};

struct Capture {
  std::vector<RecordedCommand> commands;
};

void recordSetBuffer(void* userdata,
                     WMT::Buffer buffer,
                     std::uint64_t offset,
                     std::uint32_t index) {
  auto& capture = *static_cast<Capture*>(userdata);
  capture.commands.push_back(RecordedCommand{
      .handle = buffer.handle,
      .offset = offset,
      .index = index,
  });
}

dxmt9::argbuf_hybrid::ArgbufRecorder makeRecorder(Capture& capture) {
  dxmt9::argbuf_hybrid::ArgbufRecorder recorder{};
  recorder.userdata = &capture;
  recorder.suppressMetalCalls = true;
  recorder.setBuffer = recordSetBuffer;
  return recorder;
}

const RecordedCommand& commandAt(const Capture& capture,
                                 std::size_t index,
                                 std::string_view message) {
  if (index >= capture.commands.size()) {
    fail(std::string(message));
  }
  return capture.commands[index];
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

void testDirtyBytesEstimateUsesShaderUsageRangeForVsPs() {
  dxmt9::uniform::DirtyState s{};
  dxmt9::uniform::applyConstantSetVsF(s, 2, 2);
  dxmt9::uniform::applyConstantSetPsF(s, 1, 1);

  dxmt9::uniform::ShaderConstantUsageBounds vsUsage{};
  vsUsage.unknown = false;
  vsUsage.floatCount = 6;
  dxmt9::uniform::ShaderConstantUsageBounds psUsage{};
  psUsage.unknown = false;
  psUsage.floatCount = 3;

  const std::uint64_t expected =
      6u * 16u + 3u * 16u;
  checkEq(dxmt9::argbuf_hybrid::dirtyBytesEstimate(s, vsUsage, psUsage),
          expected,
          "known fixed shader usage shrinks Stage 2 VS/PS constant byte estimate");
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

// ---------------------------------------------------------------------
// E4 — Stage 2 argbuf FfpVs repoint seam

void testPointFfpVsAtSliceRecordsArgbufEntry() {
  constexpr obj_handle_t kBuffer = 0x6000000000000030ull;
  constexpr std::uint64_t kOffset = 256u;

  dxmt9::argbuf_hybrid::ArgbufEncoderResource encoderResource;
  encoderResource.initForTest(/*encodedLength=*/512u, /*alignment=*/16u);
  Capture capture;
  auto recorder = makeRecorder(capture);

  WMT::Buffer buffer{};
  buffer.handle = kBuffer;
  dxmt9::argbuf_hybrid::pointFfpVsAtSlice(
      encoderResource, buffer, kOffset, &recorder);

  checkEq(capture.commands.size(), std::size_t{1},
          "FfpVs repoint records one argbuf write");
  const auto& command = commandAt(capture, 0, "missing FfpVs setBuffer write");
  checkEq(command.handle, kBuffer,
          "FfpVs repoint uses the uploaded slice buffer");
  checkEq(command.offset, kOffset,
          "FfpVs repoint uses the uploaded slice offset");
  checkEq(command.index, std::uint32_t{1},
          "FfpVs repoint writes argbuf id 1");
}

}  // namespace

int main() {
  try {
    testEncoderResourceDefaultUninitialized();
    testEncoderResourceInitForTestPopulatesShape();
    testEncoderResourceAlignmentFloor();
    testDirtyBytesEstimateEmpty();
    testDirtyBytesEstimateVsAny();
    testDirtyBytesEstimateUsesShaderUsageRangeForVsPs();
    testDirtyBytesEstimatePsAny();
    testDirtyBytesEstimateFfpVsAny();
    testDirtyBytesEstimateFfpPsAny();
    testDirtyBytesEstimateAllCategories();
    testDirtyBytesEstimateAdditive();
    testPopulatedArgbufDefaultIsEmpty();
    testPointFfpVsAtSliceRecordsArgbufEntry();
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
