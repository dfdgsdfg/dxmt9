// P0-3 — D3DRS_STENCILREF / D3DRS_TWOSIDEDSTENCILMODE pass-through coverage.
//
// Before the fix, `dxmt9_draw_encoder.mm:1341` invoked
// `recordedSetDepthStencilState(ctx, encoder, depthState)` with no
// explicit stencilRef, so the default-argument zero silently applied
// and Metal saw stencil ref = 0 regardless of what D3D9 set.
//
// This spec locks two things:
//   1. The pure value transform `state::computeStencilRef` reads the
//      low byte of `RS_STENCIL_REF` (D3D9 slot 57).
//   2. The encoder-driven path actually carries that byte to
//      `setDepthStencilState`. We drive it via the encoder's recorder
//      seam (`EncodeDrawRecorder::setDepthStencilState`) with Metal
//      calls suppressed.
//
// D3D9 has no D3DRS_CCW_STENCILREF (see
// `~/workspaces/wine/include/d3d9types.h:1029-1033` — the CCW family
// ends at D3DRS_CCW_STENCILFUNC=189). Wine's
// `wined3d_device_apply_stencil_ref` applies the same `state->stencil_ref`
// to both faces regardless of D3DRS_TWOSIDEDSTENCILMODE; the Metal
// equivalent is `setStencilReferenceValue` which programs front and
// back together. The two-sided case below verifies the same ref byte
// is applied when D3DRS_TWOSIDEDSTENCILMODE is enabled — i.e. that
// the implementation does not accidentally zero out the back face.

#include "../../../src/dxmt9/dxmt9_draw_encoder.hpp"
#include "../../../src/dxmt9/dxmt9_draw_state.hpp"
#include "../../../src/dxmt9/dxmt9_pipeline_cache.hpp"
#include "../../../src/dxmt9/dxmt9_resource_pool.hpp"
#include "../../../src/dxmt9/dxmt9_ring_arena.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using u32 = std::uint32_t;
using dxmt9::core::PrimitiveType;
using dxmt9::encoders::EncodeDrawRecorder;
using dxmt9::encoders::PreUploadedDrawData;

struct RecordedDepthBind {
  obj_handle_t depthHandle = 0;
  std::uint8_t stencilRef = 0;
};

struct Capture {
  std::vector<RecordedDepthBind> depthBinds;
};

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw TestFailure(std::move(message));
}

void check(bool value, std::string_view message) {
  if (!value) {
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

void recordSetDepthStencilState(void* userdata,
                                WMT::DepthStencilState depthStencil,
                                std::uint8_t stencilRef) {
  auto* capture = static_cast<Capture*>(userdata);
  capture->depthBinds.push_back(
      RecordedDepthBind{depthStencil.handle, stencilRef});
}

// Recorder where every callback except setDepthStencilState is a no-op
// — we only care about the depth-state bind for this spec.
EncodeDrawRecorder makeRecorder(Capture& capture) {
  EncodeDrawRecorder recorder{};
  recorder.userdata = &capture;
  recorder.suppressMetalCalls = true;
  recorder.suppressBaseStateLookup = true;
  // Non-zero so the encoder treats the pipeline / DSSO / sampler binds as
  // satisfied via the recorder seam.
  recorder.renderPipelineState.handle =
      static_cast<obj_handle_t>(0xD00000D000000001ull);
  recorder.depthStencilState.handle =
      static_cast<obj_handle_t>(0xD00000D000000002ull);
  recorder.fragmentSamplerState.handle =
      static_cast<obj_handle_t>(0xD00000D000000003ull);
  recorder.setDepthStencilState = recordSetDepthStencilState;
  // All other callbacks intentionally nullptr — the recorder seam
  // tolerates nullptrs and we suppress Metal calls, so nothing else
  // fires.
  return recorder;
}

dxmt9::core::CanonicalDrawState makeStateWithStencilRef(
    std::uint32_t stencilRef, bool twoSided) {
  dxmt9::core::CanonicalDrawState state{};
  state.hot.streamOffsets[0] = 0;
  state.hot.streamStrides[0] = 20u;
  state.shaderLayout.vertexDecl.streams[0].stride = 20u;
  state.shaderLayout.vertexShader.kind =
      dxmt9::core::ShaderRef::Kind::Bytecode;
  state.shaderLayout.pixelShader.kind =
      dxmt9::core::ShaderRef::Kind::Bytecode;

  // FlatStateSet invariant: entries[0..count) sorted ascending by state.
  // RS_STENCIL_REF = 57. When twoSided, also set RS_STENCIL_ENABLE (52)
  // and the kRsTwoSidedStencilMode (185). The two-sided slot ID is the
  // raw D3DRS value — dxmt9 does not define a named constant for it
  // (only the CCW per-face ops live in core_constants.hpp).
  std::size_t idx = 0;
  if (twoSided) {
    state.hot.renderStates.entries[idx++] = dxmt9::core::FlatStateEntry{
        dxmt9::core::RS_STENCIL_ENABLE, 1u};
  }
  state.hot.renderStates.entries[idx++] = dxmt9::core::FlatStateEntry{
      dxmt9::core::RS_STENCIL_REF, stencilRef};
  if (twoSided) {
    // D3DRS_TWOSIDEDSTENCILMODE = 185. Wine's
    // `wined3d_device_apply_stencil_ref` applies the same stencil_ref
    // to both faces in both single- and two-sided modes, so the
    // dxmt9 lowering's correctness condition is: stencilRef byte is
    // unchanged when the two-sided bit flips.
    state.hot.renderStates.entries[idx++] = dxmt9::core::FlatStateEntry{
        185u, 1u};
  }
  state.hot.renderStates.count = static_cast<u32>(idx);
  return state;
}

void testComputeStencilRefReadsLowByte() {
  auto state = makeStateWithStencilRef(0x42u, /*twoSided=*/false);
  const auto ref = dxmt9::state::computeStencilRef(state.view());
  checkEq(static_cast<unsigned>(ref), 0x42u,
          "computeStencilRef extracts D3DRS_STENCILREF byte");
}

void testComputeStencilRefTruncatesDword() {
  // D3DRS_STENCILREF is a DWORD; Metal stores 8 bits. Wine
  // `wined3d_device_apply_stencil_ref` calls Vulkan with the same
  // unsigned int, but Metal's `setStencilReferenceValue:` takes a
  // uint32_t that the hardware masks to compare-mask bit width. dxmt9
  // truncates at the wire because WMT exposes a uint8_t; verify the
  // truncation is the low byte (not 0, not the high byte).
  auto state = makeStateWithStencilRef(0xdeadbeu, /*twoSided=*/false);
  const auto ref = dxmt9::state::computeStencilRef(state.view());
  checkEq(static_cast<unsigned>(ref), 0xbeu,
          "computeStencilRef truncates to low byte");
}

void testComputeStencilRefDefaultsToZero() {
  dxmt9::core::CanonicalDrawState empty{};
  const auto ref = dxmt9::state::computeStencilRef(empty.view());
  checkEq(static_cast<unsigned>(ref), 0u,
          "computeStencilRef returns 0 when RS_STENCIL_REF is unset");
}

void testComputeStencilRefTwoSidedAppliesToBothFaces() {
  // D3D9 has no D3DRS_CCW_STENCILREF. Same byte applies to both faces
  // regardless of D3DRS_TWOSIDEDSTENCILMODE. The pure transform should
  // therefore return the same value when twoSided is on.
  auto state = makeStateWithStencilRef(0x73u, /*twoSided=*/true);
  const auto ref = dxmt9::state::computeStencilRef(state.view());
  checkEq(static_cast<unsigned>(ref), 0x73u,
          "computeStencilRef ignores TWOSIDEDSTENCILMODE — same ref both faces");
}

// Encoder-driven check: the encoder's depth-state bind on the recorder
// seam carries the same byte the pure transform produces.
struct Harness {
  dxmt9::core::BackendLimits limits{};
  dxmt9::resources::Pool pool{};
  dxmt9::pipeline::Cache cache{};
  dxmt9::scratch::FrameAllocators allocators{};
  dxmt9::CommandQueue queue;

  Harness() : queue(WMT::Device{}, limits) {}
};

dxmt9::encoders::EncodeContext makeContext(Harness& harness,
                                           EncodeDrawRecorder& recorder) {
  return dxmt9::encoders::EncodeContext{
      WMT::Reference<WMT::Device>{},
      harness.limits,
      harness.pool,
      harness.cache,
      harness.allocators,
      nullptr,
      nullptr,
      harness.queue,
      dxmt9::uniform::DirtyState{},
      &recorder,
  };
}

void runDrawWithStencilRef(std::uint32_t stencilRef, bool twoSided,
                           std::uint8_t expectedRef,
                           std::string_view label) {
  Harness harness;
  Capture capture;
  auto recorder = makeRecorder(capture);
  auto state = makeStateWithStencilRef(stencilRef, twoSided);

  dxmt9::core::DrawParam param{};
  param.primitiveType = PrimitiveType::TriangleList;
  param.primitiveCount = 1u;
  param.indexed = false;

  PreUploadedDrawData preUploaded{};
  auto ctx = makeContext(harness, recorder);
  WMT::CommandBuffer commandBuffer{};
  WMT::RenderCommandEncoder encoder{};
  encoder.handle = static_cast<obj_handle_t>(0xE006u);
  dxmt9::uniform::DirtyState cleanDirty{};

  const bool encoded = dxmt9::encoders::encodeDraw(
      ctx,
      commandBuffer,
      encoder,
      state.view(),
      /*seq=*/77u,
      /*skipBaseStateBind=*/false,
      &preUploaded,
      &param,
      /*arena=*/{},
      &cleanDirty,
      /*tileFfpMode=*/false,
      /*argbufHybridMode=*/false);

  check(encoded, "encodeDraw emits a draw");
  check(!capture.depthBinds.empty(),
        "encoder issued at least one depth-state bind");
  const auto& bind = capture.depthBinds.front();
  checkEq(static_cast<unsigned>(bind.stencilRef),
          static_cast<unsigned>(expectedRef), label);
}

void testEncoderForwardsStencilRefSingleSided() {
  runDrawWithStencilRef(0x42u, /*twoSided=*/false, 0x42u,
                        "encoder forwards RS_STENCIL_REF byte to depth bind");
}

void testEncoderForwardsStencilRefTwoSided() {
  // Regression guard: if a future change accidentally zeroed the back-face
  // ref when twoSided is on, this would catch it because WMT has a single
  // setter that programs both faces with the same byte.
  runDrawWithStencilRef(0x91u, /*twoSided=*/true, 0x91u,
                        "encoder forwards RS_STENCIL_REF byte when "
                        "TWOSIDEDSTENCILMODE is enabled");
}

}  // namespace

int main() {
  try {
    testComputeStencilRefReadsLowByte();
    testComputeStencilRefTruncatesDword();
    testComputeStencilRefDefaultsToZero();
    testComputeStencilRefTwoSidedAppliesToBothFaces();
    testEncoderForwardsStencilRefSingleSided();
    testEncoderForwardsStencilRefTwoSided();
  } catch (const TestFailure& e) {
    std::cerr << "stencil_ref_spec failed: " << e.what() << '\n';
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "stencil_ref_spec unexpected exception: " << e.what() << '\n';
    return 1;
  }
  std::cout << "stencil_ref_spec passed\n";
  return 0;
}
