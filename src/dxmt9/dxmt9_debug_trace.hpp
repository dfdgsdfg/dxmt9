#pragma once

// Env-gated debug knobs shared by encoder translation units. Previously
// file-local to backend_metal.mm's anonymous namespace; exposed here so
// dxmt9_draw_encoder.mm can reach them without duplication.

#include "dxmt9/core.hpp"

#include <cstdint>
#include <optional>

namespace dxmt9::debug {

using u64 = std::uint64_t;

struct DrawSeqRange {
  bool hasMin = false;
  bool hasMax = false;
  u64 min = 0;
  u64 max = 0;
};

struct DrawOrdinalRange {
  bool hasMin = false;
  bool hasMax = false;
  u64 min = 0;
  u64 max = 0;
};

DrawSeqRange makeDrawSeqRange(std::optional<u64> min, std::optional<u64> max) noexcept;
bool drawSeqRangeEnabled(DrawSeqRange range) noexcept;
bool shouldSkipDrawSeq(u64 seqId, DrawSeqRange range) noexcept;

DrawOrdinalRange makeDrawOrdinalRange(std::optional<u64> min, std::optional<u64> max) noexcept;
bool drawOrdinalRangeEnabled(DrawOrdinalRange range) noexcept;
bool shouldSkipDrawOrdinal(u64 ordinal, DrawOrdinalRange range) noexcept;

// Force blend-disable + writeMask=0xf to make all draws visible, for
// rendering-bisect work. Env: DXMT_DEBUG_FORCE_VISIBLE.
bool forceVisibleDraw();

// Force translated vertex shaders to output a fullscreen triangle. This
// separates vertex fetch/uniform issues from render-pass/pixel issues.
// Env: DXMT_DEBUG_FORCE_FULLSCREEN_VERTEX.
bool forceFullscreenVertexShader();

// Force translated fragment shaders to return magenta. This separates
// pixel shader/texture sampling issues from pipeline/raster/present issues.
// Env: DXMT_DEBUG_FORCE_FRAGMENT_COLOR.
bool forceFragmentShaderColor();

// Skip recording any draw command. Env: DXMT_SKIP_ALL_DRAWS.
bool skipAllDraws();

// Skip draws outside the inclusive seq-id range from
// DXMT9_DRAW_SEQ_MIN / DXMT9_DRAW_SEQ_MAX.
bool shouldSkipDrawSeq(u64 seqId);

// Monotonic draw ordinal after any seq-id filter has been applied. This is
// useful for bisection inside a large submission chunk.
u64 nextDrawOrdinal() noexcept;

// Skip draws outside the inclusive per-process draw ordinal range from
// DXMT9_DRAW_ORDINAL_MIN / DXMT9_DRAW_ORDINAL_MAX.
bool shouldSkipDrawOrdinal(u64 ordinal);

// Disable scissor rect — useful when debugging clipping issues.
// Env: DXMT_DISABLE_SCISSOR.
bool disableScissor();

// Disable culling — useful when debugging winding/front-face issues.
// Env: DXMT_DISABLE_CULL.
bool disableCull();

// Force Metal front-facing winding to counter-clockwise for cull debugging.
// Env: DXMT_DEBUG_FRONT_FACE_CCW.
bool frontFaceCounterClockwise();

// Flip translated vertex shader clip-space Y for D3D/Metal coordinate bisect.
// Env: DXMT_DEBUG_FLIP_VERTEX_Y.
bool flipTranslatedVertexY();

// Force translated pixel shaders to flip texture sample V for regression
// bisects. Normal D3D texture coordinates are not globally flipped.
// Env: DXMT_DEBUG_FORCE_PIXEL_V_FLIP.
bool forcePixelVFlip();

// Disable alpha-test programmatic emulation. Env: DXMT_DISABLE_ALPHA_TEST.
bool disableAlphaTest();

// When set, force indexed draws to be expanded into flat vertex lists.
// Env: DXMT_FORCE_EXPAND_INDEXED.
bool forceExpandIndexed();

// Disable the compatibility heuristic that auto-expands selected indexed
// draws. Env: DXMT_DISABLE_AUTO_EXPAND_INDEXED.
bool disableAutoExpandIndexed();

// Use Metal's native baseVertex for indexed draws and keep the shader-side
// vertexBaseIndex at zero. Env: DXMT9_USE_NATIVE_METAL_BASE_VERTEX.
bool useNativeMetalBaseVertex();

// Split indexed triangle-list draws larger than this primitive count into
// multiple Metal drawIndexed calls. Zero disables it.
// Env: DXMT9_SPLIT_LARGE_INDEXED_DRAWS.
std::uint32_t splitLargeIndexedDrawPrimitiveLimit();

// Trace budget for the FFP path (emits N trace lines then stops).
// Env: DXMT_TRACE_FVF.
int fixedFunctionTraceBudget();

// Specific texture handle that should trigger an FFP trace on every draw
// that uses it. Env: DXMT_TRACE_FVF_TEX0.
u64 fixedFunctionTraceTextureHandle();

// Seq id that should be traced regardless of other filters.
// Env: DXMT_TRACE_ENCODE_SEQ.
u64 traceEncodeSeq();

// Texture handle to emit bind/sample traces for. Env: DXMT_TRACE_TEXTURE_HANDLE.
u64 traceTextureHandle();

// Texture handle to skip: draws that use it are dropped entirely.
// Env: DXMT_SKIP_TEXTURE_HANDLE.
u64 skippedTextureHandle();

bool shouldTraceEncode(const core::FlatDrawStateRecord& hot, u64 seqId);

// Returns true if `handle` matches traceTextureHandle.
bool shouldTraceTexture(core::Handle handle);

}  // namespace dxmt9::debug
