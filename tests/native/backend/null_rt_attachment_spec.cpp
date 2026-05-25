// R-FORMAT-12 — D3DFMT_NULL depth-only (colorless) render-pass attachment
// decisions.
//
// Spec: specs/d3d9/formats/{requirements,design}.md "NULL render target"
// note + R-FORMAT-12. Implementation under test:
//   - encoders::renderPassAdmitsRt0      (begin-pass admission for a NULL RT0)
//   - encoders::colorAttachmentIncluded  (per-slot color-attachment filter)
// Both are pure value transforms extracted from beginRenderPass
// (src/dxmt9/dxmt9_draw_encoder.mm) so the depth-only-pass policy is
// unit-testable without a Metal device / ObjC++ encoder. The live encoder
// calls these at the begin guard and inside the color-attachment loop; this
// spec drives them directly, modeled on the applyColorLoadPolicy
// transcription pattern in render_pass_actions_spec.cpp.
//
// The decision being asserted: when render-target slot 0 is a NULL render
// target (Format::NullRt — a surface record with NO backend color texture),
// the render pass still opens (admission true) but the per-attachment loop
// OMITS the color attachment (colorAttachmentIncluded false), leaving the
// bound depth/stencil attachment as the effective target.

#include <cstdlib>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "../../../src/dxmt9/dxmt9_draw_encoder.hpp"

namespace {

using dxmt9::encoders::ColorlessRenderPassRt0;
using dxmt9::encoders::colorAttachmentIncluded;
using dxmt9::encoders::renderPassAdmitsRt0;

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

// --- Begin-pass admission (renderPassAdmitsRt0) ---------------------------

void testNullRt0AdmitsDepthOnlyPass() {
  // R-FORMAT-12 core case: RT0 is a NULL render target — a surface record
  // exists but it owns no backend color texture by design. The pass must
  // still open so the bound depth/stencil becomes the effective target.
  ColorlessRenderPassRt0 nullRt{};
  nullRt.surfaceExists = true;
  nullRt.hasTexture = false;
  nullRt.isNullRt = true;
  check(renderPassAdmitsRt0(nullRt),
        "R-FORMAT-12 NULL RT0 (no texture) admits a depth-only render pass");
}

void testNormalRt0Admits() {
  // A normal color RT0 with a backing texture admits the pass (the common
  // path — unchanged by the NULL behavior).
  ColorlessRenderPassRt0 normalRt{};
  normalRt.surfaceExists = true;
  normalRt.hasTexture = true;
  normalRt.isNullRt = false;
  check(renderPassAdmitsRt0(normalRt),
        "normal color RT0 with a texture admits the render pass");
}

void testMissingRt0Aborts() {
  // RT0 surface record genuinely absent (pool.findSurface returned null):
  // there is no target at all, so the pass must NOT open.
  ColorlessRenderPassRt0 missing{};
  missing.surfaceExists = false;
  missing.hasTexture = false;
  missing.isNullRt = false;
  check(!renderPassAdmitsRt0(missing),
        "absent RT0 surface aborts the render pass");

  // Defensive: even if stale flags claim a texture / NULL marker, a
  // non-existent surface can never admit the pass — the surfaceExists gate
  // dominates (matches the `!primarySurface || ...` short-circuit).
  ColorlessRenderPassRt0 staleFlags{};
  staleFlags.surfaceExists = false;
  staleFlags.hasTexture = true;
  staleFlags.isNullRt = true;
  check(!renderPassAdmitsRt0(staleFlags),
        "absent RT0 surface aborts even with stale texture/NULL flags");
}

void testNormalRt0WithoutTextureAborts() {
  // A NORMAL (non-NULL) color RT0 whose texture failed to allocate must
  // abort the pass — only a NULL render target legitimately has no color
  // texture. This is the case the `!hasTexture && !isNullRt` guard rejects.
  ColorlessRenderPassRt0 brokenColorRt{};
  brokenColorRt.surfaceExists = true;
  brokenColorRt.hasTexture = false;
  brokenColorRt.isNullRt = false;
  check(!renderPassAdmitsRt0(brokenColorRt),
        "normal color RT0 with no allocated texture aborts the render pass");
}

void testNullRtFlagWithTextureStillAdmits() {
  // Degenerate ordering check: a record flagged both NULL and (somehow)
  // texture-bearing still admits — admission only requires a surface plus
  // (a texture OR the NULL marker). This locks the OR, not an XOR.
  ColorlessRenderPassRt0 both{};
  both.surfaceExists = true;
  both.hasTexture = true;
  both.isNullRt = true;
  check(renderPassAdmitsRt0(both),
        "RT0 with texture and NULL marker still admits (admission is an OR)");
}

// --- Per-attachment color inclusion (colorAttachmentIncluded) -------------

void testNullColorAttachmentOmitted() {
  // R-FORMAT-12 omission contract: a NULL render target's slot has a surface
  // record but no backend texture, so the per-attachment loop SKIPS it — no
  // color attachment is contributed. This is the heart of "colorless".
  check(!colorAttachmentIncluded(/*surfaceExists=*/true, /*hasTexture=*/false),
        "R-FORMAT-12 NULL RT color slot (surface, no texture) is omitted");
}

void testNormalColorAttachmentIncluded() {
  // A normal color RT slot (surface + texture) is included.
  check(colorAttachmentIncluded(/*surfaceExists=*/true, /*hasTexture=*/true),
        "normal color slot (surface + texture) contributes a color attachment");
}

void testEmptyColorSlotOmitted() {
  // An unbound color slot (no surface record at all) is omitted — same
  // outcome as the NULL RT, different cause. Matches `!surface ||
  // !surface->texture`.
  check(!colorAttachmentIncluded(/*surfaceExists=*/false, /*hasTexture=*/false),
        "unbound color slot (no surface) is omitted");

  // A surface-absent slot whose stale flag claims a texture must still be
  // omitted — the surface-existence gate dominates the AND.
  check(!colorAttachmentIncluded(/*surfaceExists=*/false, /*hasTexture=*/true),
        "absent surface omits the slot even with a stale texture flag");
}

void testColorlessPassShape() {
  // End-to-end value-level shape of a depth-only pass: RT0 is NULL (admits),
  // and EVERY color slot that is NULL/empty is omitted. Together these mean
  // the pass opens with zero color attachments — the depth/stencil binding
  // (handled separately in beginRenderPass, unchanged by R-FORMAT-12) is the
  // effective target. This ties the two transforms into the single contract
  // R-FORMAT-12 promises.
  ColorlessRenderPassRt0 nullRt{};
  nullRt.surfaceExists = true;
  nullRt.hasTexture = false;
  nullRt.isNullRt = true;
  check(renderPassAdmitsRt0(nullRt), "colorless pass opens on a NULL RT0");

  // Slot 0 is the NULL RT — omitted.
  check(!colorAttachmentIncluded(true, false),
        "colorless pass omits the NULL RT0 color attachment");
  // Remaining slots unbound — omitted.
  check(!colorAttachmentIncluded(false, false),
        "colorless pass omits unbound color slots");
  // Net: no color attachment is contributed by the loop.
}

}  // namespace

int main() {
  try {
    testNullRt0AdmitsDepthOnlyPass();
    testNormalRt0Admits();
    testMissingRt0Aborts();
    testNormalRt0WithoutTextureAborts();
    testNullRtFlagWithTextureStillAdmits();
    testNullColorAttachmentOmitted();
    testNormalColorAttachmentIncluded();
    testEmptyColorSlotOmitted();
    testColorlessPassShape();
  } catch (const TestFailure& e) {
    std::cerr << "null_rt_attachment_spec failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& e) {
    std::cerr << "null_rt_attachment_spec unexpected exception: " << e.what()
              << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "null_rt_attachment_spec passed\n";
  return EXIT_SUCCESS;
}
