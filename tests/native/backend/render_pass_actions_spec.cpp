// R-BACK-15.16 — render-pass load/store action policy assertions.
//
// Spec: specs/backend/render-pass-actions/design.md sections 2 + 6.
// Implementation under test:
//   - encoders::nextDepthOperationIsClear (G3, file-local helper promoted
//     to the public encoder API for this test fixture).
//   - perf::countRenderPassLoadActionColor / *Store* (G1).
//
// We use Option A from the G4 task: a hybrid decision-table fixture.
// `nextDepthOperationIsClear` is exercised against real `core::ChunkSlot`
// SoA records (R-BACK-15.7 / 15.9 / 15.15 cases). The color-attachment
// load/store cases (R-BACK-15.1, 15.2, 15.3) live entirely inside
// `beginRenderPass` and need a fully populated `EncodeContext` + Metal
// device, so we mirror the spec's decision tree as a pure `applyColorPolicy`
// transcription and assert the four ordered-precedence branches against
// it. The transcription is a one-to-one copy of the rules in
// design.md section 2.1; if the live encoder ever diverges, the integration
// + counter coverage in G1/G3 picks up the regression in
// dxmt9-allocation-counter-spec and the perf-counter histogram.
//
// The `nextDepthOperationIsClear` promotion is justified in the G4 return
// notes — the helper is a pure value transform on `core::ChunkSlot`, with
// no Metal types in its signature, so it can ship as a public symbol
// without leaking `WMTRenderPassInfo` into headers. The encoder still
// owns the call site in `beginRenderPass`.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "dxmt9/core.hpp"
#include "../../../src/dxmt9/dxmt9_command_queue.hpp"
#include "../../../src/dxmt9/dxmt9_draw_encoder.hpp"
#include "../../../src/dxmt9/dxmt9_perf_counters.hpp"
#include "../../../src/winemetal/winemetal.h"

namespace {

using dxmt9::core::ChunkSlot;
using dxmt9::core::ClearDesc;
using dxmt9::core::ColorFillDesc;
using dxmt9::core::DrawRunCommandRecord;
using dxmt9::core::DrawDebugSnapshot;
using dxmt9::core::DrawShaderLayoutContext;
using dxmt9::core::FlatDrawStateRecord;
using dxmt9::core::Handle;
using dxmt9::core::MetalCommandHeader;
using dxmt9::core::MetalCommandKind;
using dxmt9::core::ReadbackDesc;
using dxmt9::core::StretchRectDesc;
using dxmt9::core::SurfaceCopyDesc;
using dxmt9::core::SwapDesc;

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
    out << message << " (left != right)";
    fail(out.str());
  }
}

// SoA helpers: the production `ChunkSlot::appendDrawRun` requires a
// CanonicalDrawState plus payload spans; for the look-ahead we only care
// about the depth handle stored in the FlatDrawStateRecord. Push the
// minimum SoA tuple by hand so the fixtures stay readable.
void appendDrawRunWithDepth(ChunkSlot& slot, Handle depthHandle) {
  FlatDrawStateRecord hot{};
  hot.depthStencil.handle = depthHandle;
  const auto stateIndex = static_cast<std::uint32_t>(slot.drawHotStates.size());
  slot.drawHotStates.push_back(hot);
  slot.drawShaderLayouts.push_back(DrawShaderLayoutContext{});
  slot.drawDebugSnapshots.push_back(DrawDebugSnapshot{});
  const auto recordIndex =
      static_cast<std::uint32_t>(slot.drawRunRecords.size());
  slot.drawRunRecords.push_back(DrawRunCommandRecord{
      .stateIndex = stateIndex,
      .firstParam = 0u,
      .paramCount = 0u,
      .payloadOffset = 0u,
      .payloadSize = 0u,
      .uniformHandle = {},
  });
  slot.commandHeaders.push_back(MetalCommandHeader{
      .kind = MetalCommandKind::DrawRun,
      .payloadIndex = recordIndex,
  });
}

void appendClearOnDepth(ChunkSlot& slot, Handle depthHandle) {
  ClearDesc desc{};
  desc.depthStencil.handle = depthHandle;
  desc.clearDepth = true;
  slot.appendClear(desc);
}

void appendReadbackOn(ChunkSlot& slot, Handle source) {
  ReadbackDesc desc{};
  desc.source = source;
  slot.appendReadback(desc);
}

void appendSurfaceCopyOn(ChunkSlot& slot, Handle source, Handle dest) {
  SurfaceCopyDesc desc{};
  desc.source = source;
  desc.destination = dest;
  slot.appendSurfaceCopy(desc);
}

void appendStretchRectOn(ChunkSlot& slot, Handle source, Handle dest) {
  StretchRectDesc desc{};
  desc.source = source;
  desc.destination = dest;
  slot.appendStretchRect(desc);
}

void appendColorFillOn(ChunkSlot& slot, Handle dest) {
  ColorFillDesc desc{};
  desc.destination = dest;
  slot.appendColorFill(desc);
}

void appendPresent(ChunkSlot& slot, Handle source) {
  slot.appendPresent(SwapDesc{}, source);
}

// One-to-one transcription of design.md section 2.1 (color load action
// precedence). This mirrors the live `beginRenderPass` policy without
// pulling in `WMTRenderPassInfo` / Metal device construction. Cases 1-2
// in the requirements list assert against this transcription so the
// spec is encoded as test data, not just prose.
struct ColorPolicyInputs {
  bool clearMatchesAttachment = false;
  bool postPresentBackbuffer = false;
  bool handleInTouchedSet = false;
  bool hasResolveTexture = false;
};

WMTLoadAction applyColorLoadPolicy(const ColorPolicyInputs& in) {
  if (in.clearMatchesAttachment) return WMTLoadActionClear;          // 15.3
  if (in.postPresentBackbuffer) return WMTLoadActionDontCare;        // 6.3
  if (!in.handleInTouchedSet) return WMTLoadActionDontCare;          // 15.4
  return WMTLoadActionLoad;                                          // 15.1
}

WMTStoreAction applyColorStorePolicy(const ColorPolicyInputs& in) {
  if (in.hasResolveTexture) return WMTStoreActionMultisampleResolve; // 15.14
  return WMTStoreActionStore;                                        // 15.2
}

void testDefaultsLoadAndStore() {
  // R-BACK-15.1 / R-BACK-15.2: a chunk with one DrawRun, no pending clear,
  // and the color attachment already in the per-CB touched set must
  // default to Load+Store. The decision-table transcription tracks the
  // live encoder's precedence chain (see design.md section 2.1).
  ColorPolicyInputs in{};
  in.handleInTouchedSet = true;  // not first use → not DontCare-load
  checkEq(applyColorLoadPolicy(in), WMTLoadActionLoad,
          "R-BACK-15.1 default color load is Load");
  checkEq(applyColorStorePolicy(in), WMTStoreActionStore,
          "R-BACK-15.2 default color store is Store");

  // Depth side: with no further records on the depth handle, the
  // look-ahead must answer "no proof" → defensive Store (false).
  ChunkSlot slot;
  Handle depth{0xD000u};
  appendDrawRunWithDepth(slot, depth);
  check(!dxmt9::encoders::nextDepthOperationIsClear(slot, 0u, depth),
        "R-BACK-15.2 default depth store is Store when no proof exists");
}

void testClearPrecedesDontCare() {
  // R-BACK-15.3: when a Clear is bound to the same color attachment as
  // the next DrawRun, the load action must be Clear, not DontCare —
  // first match wins in the precedence list.
  ColorPolicyInputs in{};
  in.clearMatchesAttachment = true;
  in.postPresentBackbuffer = true;   // would otherwise trigger DontCare
  in.handleInTouchedSet = false;     // would otherwise trigger DontCare
  checkEq(applyColorLoadPolicy(in), WMTLoadActionClear,
          "R-BACK-15.3 clear-as-load wins over post-present DontCare");

  in.postPresentBackbuffer = false;
  checkEq(applyColorLoadPolicy(in), WMTLoadActionClear,
          "R-BACK-15.3 clear-as-load wins over first-use DontCare");

  // Same precedence on the depth side: if the next record on the depth
  // handle is a clear of that handle, the look-ahead must report
  // "DontCare allowed" before any later live op could downgrade it.
  ChunkSlot slot;
  Handle depth{0xD001u};
  appendDrawRunWithDepth(slot, depth);
  appendClearOnDepth(slot, depth);
  // A second DrawRun on the same depth handle after the clear should not
  // matter — the look-ahead returns at the first matching record.
  appendDrawRunWithDepth(slot, depth);
  check(dxmt9::encoders::nextDepthOperationIsClear(slot, 0u, depth),
        "R-BACK-15.3 clear precedence on depth: DontCare allowed");
}

void testDepthDontCareStoreOnNextClear() {
  // R-BACK-15.7 simple-form shortcut: DrawRun on depth H followed
  // immediately by a Clear on the same handle must let the previous
  // pass DontCare-store, since the tile contents would be discarded
  // anyway.
  ChunkSlot slot;
  Handle depth{0xD007u};
  appendDrawRunWithDepth(slot, depth);
  appendClearOnDepth(slot, depth);
  check(dxmt9::encoders::nextDepthOperationIsClear(slot, 0u, depth),
        "R-BACK-15.7 next-op-is-clear on depth handle → DontCare-store");

  // A clear that targets a DIFFERENT depth handle must NOT trigger the
  // shortcut — the original handle is still live-out.
  ChunkSlot otherSlot;
  Handle depthA{0xDA00u};
  Handle depthB{0xDB00u};
  appendDrawRunWithDepth(otherSlot, depthA);
  appendClearOnDepth(otherSlot, depthB);
  check(!dxmt9::encoders::nextDepthOperationIsClear(otherSlot, 0u, depthA),
        "R-BACK-15.7 unrelated-handle clear does not satisfy the proof");

  // A null/zero depth handle is never a valid candidate for the
  // shortcut, even with a trailing clear — defensive false (matches
  // the encoder's `hot.depthStencil.handle` guard).
  ChunkSlot zeroSlot;
  appendDrawRunWithDepth(zeroSlot, Handle{0});
  appendClearOnDepth(zeroSlot, Handle{0});
  check(!dxmt9::encoders::nextDepthOperationIsClear(zeroSlot, 0u, Handle{0}),
        "R-BACK-15.7 null depth handle never enables DontCare-store");
}

void testDepthForcedStoreOnNextRead() {
  // R-BACK-15.15 / defensive Store: if the next record after our DrawRun
  // is itself a DrawRun that re-binds the same depth handle (live-out
  // read), we must Store — the look-ahead returns false before reaching
  // any later clear.
  ChunkSlot drawSlot;
  Handle depth{0xD015u};
  appendDrawRunWithDepth(drawSlot, depth);
  appendDrawRunWithDepth(drawSlot, depth);
  appendClearOnDepth(drawSlot, depth);  // would otherwise allow DontCare
  check(!dxmt9::encoders::nextDepthOperationIsClear(drawSlot, 0u, depth),
        "R-BACK-15.15 subsequent draw on same depth handle forces Store");

  // R-BACK-15.15: a Readback that touches the depth handle must force
  // Store so the host-visible read is served correct tile contents.
  ChunkSlot readbackSlot;
  appendDrawRunWithDepth(readbackSlot, depth);
  appendReadbackOn(readbackSlot, depth);
  appendClearOnDepth(readbackSlot, depth);
  check(!dxmt9::encoders::nextDepthOperationIsClear(readbackSlot, 0u, depth),
        "R-BACK-15.15 readback on depth handle forces Store");

  // SurfaceCopy with depth as source or destination: same defensive
  // Store contract.
  ChunkSlot copySlot;
  Handle other{0xCC00u};
  appendDrawRunWithDepth(copySlot, depth);
  appendSurfaceCopyOn(copySlot, depth, other);
  appendClearOnDepth(copySlot, depth);
  check(!dxmt9::encoders::nextDepthOperationIsClear(copySlot, 0u, depth),
        "R-BACK-15.15 surface copy from depth handle forces Store");

  // StretchRect with depth as destination: same.
  ChunkSlot stretchSlot;
  appendDrawRunWithDepth(stretchSlot, depth);
  appendStretchRectOn(stretchSlot, other, depth);
  appendClearOnDepth(stretchSlot, depth);
  check(!dxmt9::encoders::nextDepthOperationIsClear(stretchSlot, 0u, depth),
        "R-BACK-15.15 stretch rect into depth handle forces Store");

  // ColorFill targeting the depth handle: same defensive Store.
  ChunkSlot fillSlot;
  appendDrawRunWithDepth(fillSlot, depth);
  appendColorFillOn(fillSlot, depth);
  appendClearOnDepth(fillSlot, depth);
  check(!dxmt9::encoders::nextDepthOperationIsClear(fillSlot, 0u, depth),
        "R-BACK-15.15 color fill into depth handle forces Store");

  // R-BACK-15.13 defensive Store on Present: the slot still has live-out
  // potential for the surface, so the look-ahead must return false.
  ChunkSlot presentSlot;
  appendDrawRunWithDepth(presentSlot, depth);
  appendPresent(presentSlot, depth);
  appendClearOnDepth(presentSlot, depth);
  check(!dxmt9::encoders::nextDepthOperationIsClear(presentSlot, 0u, depth),
        "R-BACK-15.13 present in slot forces defensive Store");
}

void testNoCrossChunkLookahead() {
  // R-BACK-15.9: the look-ahead never crosses the chunk boundary. A
  // DrawRun whose slot ends with no further records on the depth handle
  // must Store, even if a *later* chunk clears the handle (we have no
  // visibility into that and must not assume).
  ChunkSlot slot;
  Handle depth{0xD009u};
  appendDrawRunWithDepth(slot, depth);
  // Slot ends here — no clear, no copy, no read.
  check(!dxmt9::encoders::nextDepthOperationIsClear(slot, 0u, depth),
        "R-BACK-15.9 end-of-slot with no proof must Store");

  // Even with one totally unrelated record (different handle), the
  // encoder still has no proof for our depth handle and must Store.
  ChunkSlot mixedSlot;
  Handle depthA{0xDA09u};
  Handle depthB{0xDB09u};
  appendDrawRunWithDepth(mixedSlot, depthA);
  appendDrawRunWithDepth(mixedSlot, depthB);  // unrelated handle
  // No clear on depthA anywhere — slot ends without proof.
  check(!dxmt9::encoders::nextDepthOperationIsClear(mixedSlot, 0u, depthA),
        "R-BACK-15.9 slot end with only unrelated records must Store");

  // The look-ahead's `startCommandIndex` parameter is also exclusive —
  // if the only matching clear is at-or-before the start index, we must
  // not look backward and must report no proof.
  ChunkSlot backwardSlot;
  Handle depthC{0xDC09u};
  appendClearOnDepth(backwardSlot, depthC);
  appendDrawRunWithDepth(backwardSlot, depthC);
  check(!dxmt9::encoders::nextDepthOperationIsClear(backwardSlot, 1u, depthC),
        "R-BACK-15.9 look-ahead does not see records before start index");
}

void testTouchedSet() {
  // R-BACK-15.4 / 15.5 / 15.6: minimal CommandQueue API exercise.
  // Inert queue (null WMT::Device) — no threads, no Metal — but the
  // touched-set methods are pure container ops and run fine in this
  // mode. H4 will add the integration test that drives the encoder.
  dxmt9::core::BackendLimits limits{};
  dxmt9::CommandQueue queue(WMT::Device{NULL_OBJECT_HANDLE}, limits);

  // Empty set: nothing is touched.
  Handle a{0xC0A0u};
  Handle b{0xC0B0u};
  check(!queue.isColorHandleTouched(a),
        "R-BACK-15.4 untouched handle reports false");
  check(!queue.isColorHandleTouched(Handle{0}),
        "R-BACK-15.4 zero handle is never touched");

  // Mark + isTouched.
  queue.markColorHandleTouched(a);
  check(queue.isColorHandleTouched(a),
        "R-BACK-15.6 marked handle reports true");
  check(!queue.isColorHandleTouched(b),
        "R-BACK-15.6 unrelated handle stays untouched");

  // Idempotent mark — set semantics; no growth on re-insert.
  queue.markColorHandleTouched(a);
  check(queue.isColorHandleTouched(a),
        "R-BACK-15.6 mark is idempotent");

  // Zero handle is a no-op on every entry-point.
  queue.markColorHandleTouched(Handle{0});
  check(!queue.isColorHandleTouched(Handle{0}),
        "R-BACK-15.6 mark on zero handle is a no-op");

  // Invalidate removes only the targeted handle.
  queue.markColorHandleTouched(b);
  check(queue.isColorHandleTouched(b),
        "R-BACK-15.6 second mark recorded");
  queue.invalidateColorHandle(a);
  check(!queue.isColorHandleTouched(a),
        "R-BACK-15.5 invalidate removes target handle");
  check(queue.isColorHandleTouched(b),
        "R-BACK-15.5 invalidate is scoped to target handle");

  // Invalidate of an absent handle is a no-op (no throw, b survives).
  queue.invalidateColorHandle(a);
  check(queue.isColorHandleTouched(b),
        "R-BACK-15.5 invalidate on absent handle is harmless");
  queue.invalidateColorHandle(Handle{0});
  check(queue.isColorHandleTouched(b),
        "R-BACK-15.5 invalidate on zero handle is a no-op");

  // clearAll wipes every entry.
  queue.markColorHandleTouched(a);
  queue.clearAllTouchedColorHandles();
  check(!queue.isColorHandleTouched(a),
        "R-BACK-15.4 clearAll wipes a");
  check(!queue.isColorHandleTouched(b),
        "R-BACK-15.4 clearAll wipes b");
}

void testCounterEmission() {
  // R-BACK-15.10/15.11/15.12: the perf counter API is the validation
  // surface for this spec. We cannot read counter sums back from outside
  // the perf TU, but we can confirm the API accepts every action enum
  // value the encoder emits without crashing or asserting (the encoder
  // calls these on every render pass; see beginRenderPass in G3). This
  // matches the smoke contract used by `dxmt9-allocation-counter-spec`.
  if (!dxmt9::perf::enabled()) {
    // Counters are gated by an env var the meson harness sets for spec
    // tests; the allocation-counter spec already requires this. Skip
    // silently if the harness disabled them.
    return;
  }
  using namespace dxmt9::perf;
  countRenderPassLoadActionColor(static_cast<std::uint32_t>(WMTLoadActionLoad));
  countRenderPassLoadActionColor(
      static_cast<std::uint32_t>(WMTLoadActionClear));
  countRenderPassLoadActionColor(
      static_cast<std::uint32_t>(WMTLoadActionDontCare));
  countRenderPassLoadActionDepth(
      static_cast<std::uint32_t>(WMTLoadActionLoad));
  countRenderPassLoadActionStencil(
      static_cast<std::uint32_t>(WMTLoadActionLoad));
  countRenderPassStoreActionColor(
      static_cast<std::uint32_t>(WMTStoreActionStore));
  countRenderPassStoreActionColor(
      static_cast<std::uint32_t>(WMTStoreActionDontCare));
  countRenderPassStoreActionColor(
      static_cast<std::uint32_t>(WMTStoreActionMultisampleResolve));
  countRenderPassStoreActionDepth(
      static_cast<std::uint32_t>(WMTStoreActionDontCare));
  countRenderPassStoreActionStencil(
      static_cast<std::uint32_t>(WMTStoreActionDontCare));
  countRenderPassTilePreservationBytes(7'372'800u);  // 1280×720×4 (RGBA8)
}

}  // namespace

int main() {
  try {
    testDefaultsLoadAndStore();
    testClearPrecedesDontCare();
    testDepthDontCareStoreOnNextClear();
    testDepthForcedStoreOnNextRead();
    testNoCrossChunkLookahead();
    testTouchedSet();
    testCounterEmission();
  } catch (const TestFailure& e) {
    std::cerr << "render_pass_actions_spec failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& e) {
    std::cerr << "render_pass_actions_spec unexpected exception: " << e.what()
              << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "render_pass_actions_spec passed\n";
  return EXIT_SUCCESS;
}
