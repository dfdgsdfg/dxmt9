// Parity harness for the modern-renderer backend transition (Task A8,
// R-BACK-39.1).
//
// GOAL
//   Prove the FrameGraph (empty-feature) backend produces output identical to
//   the Traditional backend. At L0 both backends' onChunkReady bodies are
//   literally `return encoders::encodeChunk(ctx, slotIndex, slot);` (see
//   src/dxmt9/render/traditional_backend.cpp and framegraph_backend.cpp), so
//   byte-identical is true by construction. The harness's heavy value lands at
//   L1, when FrameGraph begins to reinterpret the chunk and divergence becomes
//   possible.
//
// DEVICE FEASIBILITY (investigated 2026-06-08)
//   The native test host is a `nowine` host with NO Metal device, so the full
//   encode path cannot run headless:
//     * encoders::encodeChunk lives in dxmt9_draw_encoder.mm and immediately
//       short-circuits to std::nullopt when `!ctx.device || !ctx.queue.valid()`
//       (dxmt9_draw_encoder.mm:13304). A default-constructed WMT::Device{} has
//       handle 0, so the body never runs.
//     * Even with a faked nonzero device handle it then calls
//       ctx.queue.newCommandBuffer() and WMT::Device{...}.supportsCounterSampling
//       — real Metal/Wine-bridge operations that have no nowine implementation.
//     * The existing EncodeDrawRecorder seam (EncodeContext::drawRecorder,
//       exercised by tests/native/backend/encode_draw_recorder_spec.cpp) only
//       intercepts at the per-DRAW issue level *inside* an already-open render
//       pass; it does not let beginRenderPass / encodeChunk run without a live
//       command buffer. So there is no headless full-chunk encode path today.
//
//   Therefore this is the **Option B** deliverable from the task brief: we do
//   NOT fake a Metal device. Instead we implement and unit-test the harness's
//   reusable comparison CORE as a device-free pure function over a small POD
//   `Trace`, plus device-free structural backend assertions. The full
//   byte-identical replay is exercised by the wine+device conformance leg
//   (scripts/run_suites/run_d3d9_conformance_render_modes.sh, Task A11) and
//   matures in L1 — see the DEVICE-GATED note below.
//
// WHAT THIS FILE ASSERTS
//   Device-free (runs here, on the nowine native host):
//     * compareEncodeTraces() reports equality for identical traces and pins
//       the divergence index/kind for dropped / reordered / extra / mutated
//       entries. This is the logic L1 will feed REAL recorded encode traces
//       into once a headless or capture-backed trace source exists.
//     * TraditionalBackend and FrameGraphBackend both report their BackendMode
//       and are usable polymorphically through IRenderBackend& (no encodeChunk
//       call).
//   Device-gated (DEFERRED — NOT run here):
//     * The actual capture of each backend's encode decision sequence by
//       replaying one core::ChunkSlot through encodeChunk, and the element-wise
//       equality of those two captured sequences. This requires a Metal device
//       and is covered by the conformance leg above.

#include "../../../src/dxmt9/render/framegraph_backend.hpp"
#include "../../../src/dxmt9/render/traditional_backend.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using dxmt9::render::BackendMode;
using dxmt9::render::FrameGraphBackend;
using dxmt9::render::IRenderBackend;
using dxmt9::render::TraditionalBackend;

// ---------------------------------------------------------------------------
// Homegrown test harness (matches the repo pattern: TestFailure, check(),
// main() returning 0/1 — see encode_draw_recorder_spec.cpp).
// ---------------------------------------------------------------------------

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

// ===========================================================================
// Reusable harness CORE (device-free).
//
// A `Trace` is the ordered sequence of encode decisions one backend made while
// replaying a single chunk. At L0 the production capture of these traces needs
// a Metal device (see file header), so here `Trace` is a small POD that the L1
// trace source — and the unit tests below — can populate directly. The
// comparison logic is identical regardless of where the trace bytes come from,
// which is the whole point of factoring it out as a pure value transform
// (codebase_conventions: "prefer pure value transforms ... unit-testable
// without Wine, Metal, or GPU timing").
// ===========================================================================

// One recorded encode decision. Kept intentionally small and POD: an opcode
// tag plus a handful of scalar operands covers the encode-decision shape the
// EncodeDrawRecorder seam already records (pipeline / depth / draw / bind
// handles, offsets, indices). L1 can widen this without changing the
// comparison contract below.
enum class TraceOp : std::uint16_t {
  SetRenderPipelineState,
  SetDepthStencilState,
  SetViewport,
  SetScissorRect,
  SetVertexBuffer,
  SetVertexBytes,
  DrawPrimitives,
  DrawIndexedPrimitives,
};

struct TraceEntry {
  TraceOp op = TraceOp::DrawPrimitives;
  std::uint64_t handle = 0;
  std::uint64_t a = 0;
  std::uint64_t b = 0;

  bool operator==(const TraceEntry& other) const {
    return op == other.op && handle == other.handle && a == other.a &&
           b == other.b;
  }
  bool operator!=(const TraceEntry& other) const { return !(*this == other); }
};

using Trace = std::vector<TraceEntry>;

// Result of comparing two encode traces. `equal` is the headline; on
// divergence `divergenceIndex` pins the first differing position so L1 triage
// can jump straight to the offending encode decision. `reason` is a short
// human-readable tag (length mismatch vs element mismatch).
struct ParityResult {
  bool equal = false;
  std::size_t divergenceIndex = 0;
  std::string reason;
};

// Pure, device-free comparison core. Identical traces compare equal; any
// dropped, extra, reordered, or mutated entry is reported with the first
// divergent index. This is the function L1 feeds two real recorded traces
// (Traditional vs FrameGraph) into.
ParityResult compareEncodeTraces(const Trace& a, const Trace& b) {
  const std::size_t common = a.size() < b.size() ? a.size() : b.size();
  for (std::size_t i = 0; i < common; ++i) {
    if (a[i] != b[i]) {
      return ParityResult{/*equal=*/false, i, "entry-mismatch"};
    }
  }
  if (a.size() != b.size()) {
    // First missing/extra entry is at the end of the shorter trace.
    return ParityResult{/*equal=*/false, common, "length-mismatch"};
  }
  return ParityResult{/*equal=*/true, 0, "equal"};
}

// Small builder so the tests read like the encode sequence they model.
Trace makeReferenceTrace() {
  return Trace{
      TraceEntry{TraceOp::SetRenderPipelineState, 0xAA01, 0, 0},
      TraceEntry{TraceOp::SetDepthStencilState, 0xAA02, /*stencilRef=*/3, 0},
      TraceEntry{TraceOp::SetViewport, 0, /*origin=*/0, /*extent=*/0},
      TraceEntry{TraceOp::SetVertexBuffer, 0xBB01, /*offset=*/64, /*slot=*/1},
      TraceEntry{TraceOp::SetVertexBytes, 0, /*length=*/32, /*slot=*/5},
      TraceEntry{TraceOp::DrawIndexedPrimitives, 0xCC01, /*indexCount=*/36,
                 /*offset=*/0},
  };
}

// ---------------------------------------------------------------------------
// Device-free tests of the comparison core.
// ---------------------------------------------------------------------------

void testIdenticalTracesAreEqual() {
  const Trace a = makeReferenceTrace();
  const Trace b = makeReferenceTrace();
  const ParityResult result = compareEncodeTraces(a, b);
  check(result.equal, "identical traces compare equal");
}

void testEmptyTracesAreEqual() {
  const ParityResult result = compareEncodeTraces(Trace{}, Trace{});
  check(result.equal, "two empty traces compare equal");
}

void testDroppedDrawDiverges() {
  const Trace a = makeReferenceTrace();
  Trace b = makeReferenceTrace();
  b.pop_back();  // FrameGraph "dropped" the final indexed draw.
  const ParityResult result = compareEncodeTraces(a, b);
  check(!result.equal, "dropping a draw is detected as divergence");
  checkEq(result.divergenceIndex, b.size(),
          "dropped-draw divergence index points at the missing tail entry");
  checkEq(result.reason, std::string("length-mismatch"),
          "dropped draw reported as a length mismatch");
}

void testExtraDrawDiverges() {
  const Trace a = makeReferenceTrace();
  Trace b = makeReferenceTrace();
  b.push_back(TraceEntry{TraceOp::DrawPrimitives, 0xDEAD, 3, 0});  // spurious.
  const ParityResult result = compareEncodeTraces(a, b);
  check(!result.equal, "an extra draw is detected as divergence");
  checkEq(result.divergenceIndex, a.size(),
          "extra-draw divergence index points just past the shorter trace");
  checkEq(result.reason, std::string("length-mismatch"),
          "extra draw reported as a length mismatch");
}

void testReorderedDrawsDiverge() {
  const Trace a = makeReferenceTrace();
  Trace b = makeReferenceTrace();
  // Swap the pipeline/depth binds — same set, different order. A reorder must
  // FAIL the comparison (proves the harness is order-sensitive, not a set
  // compare).
  std::swap(b[0], b[1]);
  const ParityResult result = compareEncodeTraces(a, b);
  check(!result.equal, "reordering two binds is detected as divergence");
  checkEq(result.divergenceIndex, std::size_t{0},
          "reorder divergence index points at the first swapped entry");
  checkEq(result.reason, std::string("entry-mismatch"),
          "reorder reported as an element mismatch");
}

void testMutatedOperandDiverges() {
  const Trace a = makeReferenceTrace();
  Trace b = makeReferenceTrace();
  b[3].a = 128;  // Same op/handle/slot, different vertex-buffer offset.
  const ParityResult result = compareEncodeTraces(a, b);
  check(!result.equal, "a mutated operand is detected as divergence");
  checkEq(result.divergenceIndex, std::size_t{3},
          "mutated-operand divergence index points at the changed entry");
  checkEq(result.reason, std::string("entry-mismatch"),
          "mutated operand reported as an element mismatch");
}

// NEGATIVE-CONTROL self-test (task brief): a deliberately-diverging fake
// backend trace (drops/reorders a draw) MUST make the comparison FAIL. The two
// tests above already exercise drop and reorder; this consolidates them into
// the explicit "harness detects a diverging backend" claim so a future change
// that accidentally made compareEncodeTraces order-insensitive or
// length-insensitive trips here.
void testHarnessDetectsDivergingBackend() {
  const Trace traditional = makeReferenceTrace();

  // A FrameGraph that reordered two draws.
  Trace reordered = traditional;
  std::swap(reordered[4], reordered[5]);
  check(!compareEncodeTraces(traditional, reordered).equal,
        "reordering backend is rejected");

  // A FrameGraph that dropped a draw.
  Trace dropped = traditional;
  dropped.erase(dropped.begin() + 5);
  check(!compareEncodeTraces(traditional, dropped).equal,
        "dropping backend is rejected");

  // Sanity: an honest FrameGraph that reproduces the trace passes.
  const Trace honest = traditional;
  check(compareEncodeTraces(traditional, honest).equal,
        "byte-identical backend passes");
}

// ---------------------------------------------------------------------------
// Device-free structural backend equivalence (does NOT call encodeChunk).
//
// At L0 both backends forward onChunkReady verbatim to encoders::encodeChunk,
// so the only device-free claim we can make about the live objects is that they
// are both constructible, report their mode, and are usable polymorphically
// through the common IRenderBackend& seam. The actual byte-identical replay is
// device-gated (see file header) and lands in the conformance leg.
// ---------------------------------------------------------------------------

void testBothBackendsReportModeAndArePolymorphic() {
  TraditionalBackend traditional;
  FrameGraphBackend framegraph;

  checkEq(static_cast<int>(traditional.mode()),
          static_cast<int>(BackendMode::Traditional),
          "TraditionalBackend reports Traditional mode");
  checkEq(static_cast<int>(framegraph.mode()),
          static_cast<int>(BackendMode::FrameGraph),
          "FrameGraphBackend reports FrameGraph mode");

  // Both must be drivable through the same interface reference so the parity
  // replay (L1) can hold them behind one IRenderBackend* without an adapter.
  IRenderBackend& a = traditional;
  IRenderBackend& b = framegraph;
  check(a.mode() != b.mode(),
        "the two backends are distinguishable through IRenderBackend&");
  // Lifecycle hooks inherit no-op defaults at L0; exercising them must be safe
  // and is device-free.
  a.onDeviceCreated();
  a.onFrameBegin(0);
  a.onFrameEnd();
  a.onDeviceDestroyed();
  b.onDeviceCreated();
  b.onFrameBegin(0);
  b.onFrameEnd();
  b.onDeviceDestroyed();
}

}  // namespace

int main() {
  try {
    // Comparison-core tests (device-free, run here).
    testIdenticalTracesAreEqual();
    testEmptyTracesAreEqual();
    testDroppedDrawDiverges();
    testExtraDrawDiverges();
    testReorderedDrawsDiverge();
    testMutatedOperandDiverges();
    testHarnessDetectsDivergingBackend();
    // Structural backend equivalence (device-free, run here).
    testBothBackendsReportModeAndArePolymorphic();
  } catch (const TestFailure& failure) {
    std::cerr << "parity_harness_spec failed: " << failure.what() << '\n';
    return 1;
  } catch (const std::exception& ex) {
    std::cerr << "parity_harness_spec unexpected exception: " << ex.what()
              << '\n';
    return 1;
  }
  std::cout << "parity_harness_spec passed\n";
  return 0;
}
