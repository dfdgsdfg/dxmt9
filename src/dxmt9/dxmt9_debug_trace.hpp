#pragma once

// Env-gated debug knobs shared by encoder translation units. Previously
// file-local to backend_metal.mm's anonymous namespace; exposed here so
// dxmt9_draw_encoder.mm can reach them without duplication.

#include "dxmt9/core.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

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

struct RenderEncoderSelector {
  bool enabled = false;
  u64 seqId = 0;
  u64 encoderIndex = 0;
};

struct RenderEncoderSelectorList {
  static constexpr std::size_t MaxSelectors = 32;

  bool enabled = false;
  std::array<RenderEncoderSelector, MaxSelectors> selectors = {};
  std::size_t count = 0;
};

enum class IndexedTriangleClassFilter : std::uint8_t {
  Any,
  OpaqueDepthWrite,
  NonOpaque,
  DepthRead,
  AlphaBlend,
  Scissor,
  Textured,
  Large4096,
};

struct IndexedTriangleClassFilterList {
  static constexpr std::size_t MaxFilters = 8;

  std::array<IndexedTriangleClassFilter, MaxFilters> filters = {};
  std::size_t count = 0;
};

struct ScissorRectOverride {
  bool enabled = false;
  core::Rect rect{};
};

IndexedTriangleClassFilter makeIndexedTriangleClassFilter(
    std::string_view spec) noexcept;
IndexedTriangleClassFilterList makeIndexedTriangleClassFilterList(
    std::string_view spec) noexcept;
ScissorRectOverride makeScissorRectOverride(std::string_view spec) noexcept;

DrawSeqRange makeDrawSeqRange(std::optional<u64> min, std::optional<u64> max) noexcept;
bool drawSeqRangeEnabled(DrawSeqRange range) noexcept;
bool shouldSkipDrawSeq(u64 seqId, DrawSeqRange range) noexcept;

DrawOrdinalRange makeDrawOrdinalRange(std::optional<u64> min, std::optional<u64> max) noexcept;
bool drawOrdinalRangeEnabled(DrawOrdinalRange range) noexcept;
bool shouldSkipDrawOrdinal(u64 ordinal, DrawOrdinalRange range) noexcept;

RenderEncoderSelector makeRenderEncoderSelector(std::string_view spec) noexcept;
RenderEncoderSelectorList makeRenderEncoderSelectorList(std::string_view spec) noexcept;
bool renderEncoderSelectorMatches(RenderEncoderSelector selector,
                                  u64 seqId,
                                  u64 encoderIndex) noexcept;
bool renderEncoderSelectorListMatches(const RenderEncoderSelectorList& selectors,
                                      u64 seqId,
                                      u64 encoderIndex) noexcept;

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

// Disable fog programmatic emulation. Env: DXMT_DISABLE_FOG.
bool disableFog();

// Force fragment texture samples to white. Env: DXMT_FORCE_TEXTURE_WHITE.
bool forceTextureWhite();

// Diagnostic render-state A/B: force color blending off while preserving
// color-write masks. Env: DXMT9_PROBE_DISABLE_ALPHA_BLEND.
bool probeDisableAlphaBlend();

// Diagnostic render-state A/B: keep depth testing but force depth writes off.
// Env: DXMT9_PROBE_DISABLE_DEPTH_WRITE.
bool probeDisableDepthWrite();

// Diagnostic render-state A/B: keep depth enable/write state but force the
// depth compare function to Always. Env: DXMT9_PROBE_DEPTH_FUNC_ALWAYS.
bool probeDepthFuncAlways();

// When set, force indexed draws to be expanded into flat vertex lists.
// Env: DXMT_FORCE_EXPAND_INDEXED.
bool forceExpandIndexed();

// Disable the compatibility heuristic that auto-expands selected indexed
// draws. Env: DXMT_DISABLE_AUTO_EXPAND_INDEXED.
bool disableAutoExpandIndexed();

// Use Metal's native baseVertex for indexed draws and keep the shader-side
// vertexBaseIndex at zero. Env: DXMT9_USE_NATIVE_METAL_BASE_VERTEX.
bool useNativeMetalBaseVertex();

// Experimental optimization: for order-independent screen-blend indexed
// triangle lists, submit a transient index buffer with primitive order reversed.
// Env: DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER.
bool optimizeScreenBlendIndexOrder();

// Optional class filter for DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER. Accepted
// values: any, opaque-depth-write, nonopaque, depth-read, alpha-blend, scissor,
// textured, and large4096.
// Env: DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER_CLASS.
IndexedTriangleClassFilter optimizeScreenBlendIndexOrderClassFilter();

// Optional AND class-list filter for DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER.
// Values match DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER_CLASS and may be
// separated by comma, semicolon, space, '+', or '&'.
// Env: DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER_CLASSES.
IndexedTriangleClassFilterList optimizeScreenBlendIndexOrderClassFilters();

// Optional selector for DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER. Format is
// "<seq>/<encoder>", for example "60/4".
// Env: DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER_ROW.
RenderEncoderSelector optimizeScreenBlendIndexOrderRow();

// Optional selector list for DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER. Format is
// a comma/semicolon/space separated list of "<seq>/<encoder>".
// Env: DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER_ROWS.
RenderEncoderSelectorList optimizeScreenBlendIndexOrderRows();

// Split indexed triangle-list draws larger than this primitive count into
// multiple Metal drawIndexed calls. Zero disables it.
// Env: DXMT9_SPLIT_LARGE_INDEXED_DRAWS.
std::uint32_t splitLargeIndexedDrawPrimitiveLimit();

// Optional class filter for DXMT9_SPLIT_LARGE_INDEXED_DRAWS. Accepted values:
// any, opaque-depth-write, nonopaque, depth-read, alpha-blend, scissor,
// textured, and large4096.
// Env: DXMT9_SPLIT_LARGE_INDEXED_DRAWS_CLASS.
IndexedTriangleClassFilter splitLargeIndexedDrawClassFilter();

// Optional AND class-list filter for DXMT9_SPLIT_LARGE_INDEXED_DRAWS. Accepted
// values match DXMT9_SPLIT_LARGE_INDEXED_DRAWS_CLASS and may be separated by
// comma, semicolon, space, '+', or '&'. Example: "large4096,alpha-blend".
// Env: DXMT9_SPLIT_LARGE_INDEXED_DRAWS_CLASSES.
IndexedTriangleClassFilterList splitLargeIndexedDrawClassFilters();

// Optional selector for DXMT9_SPLIT_LARGE_INDEXED_DRAWS. Format is
// "<seq>/<encoder>", for example "60/3".
// Env: DXMT9_SPLIT_LARGE_INDEXED_DRAWS_ROW.
RenderEncoderSelector splitLargeIndexedDrawRow();

// Optional selector list for DXMT9_SPLIT_LARGE_INDEXED_DRAWS. Format is a
// comma/semicolon/space separated list of "<seq>/<encoder>", for example
// "60/1,60/3".
// Env: DXMT9_SPLIT_LARGE_INDEXED_DRAWS_ROWS.
RenderEncoderSelectorList splitLargeIndexedDrawRows();

// Diagnostic-only: keep indexed draws and render state intact, but submit a
// transient index buffer with triangle-list primitive order reversed.
// Env: DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES.
bool probeReverseIndexedTriangles();

// Diagnostic-only: same as DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES, but applies
// only to opaque depth-writing triangle-list draws.
// Env: DXMT9_PROBE_REVERSE_OPAQUE_INDEXED_TRIANGLES.
bool probeReverseOpaqueIndexedTriangles();

// Diagnostic-only: same as DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES, but applies
// only to triangle-list draws outside the opaque depth-writing subset. This
// isolates blended/depth-write-off/visibility-sensitive rows after the opaque
// classifier rejects the production-safe subset.
// Env: DXMT9_PROBE_REVERSE_NONOPAQUE_INDEXED_TRIANGLES.
bool probeReverseNonOpaqueIndexedTriangles();

// Optional class filter for reverse-indexed-triangle probes. Accepted values:
// any, opaque-depth-write, nonopaque, depth-read, alpha-blend, scissor,
// textured, and large4096.
// Env: DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_CLASS.
IndexedTriangleClassFilter probeReverseIndexedTrianglesClassFilter();

// Optional AND class-list filter for reverse-indexed-triangle probes. Accepted
// values match DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_CLASS and may be separated
// by comma, semicolon, space, '+', or '&'. Example: "large4096,scissor".
// Env: DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_CLASSES.
IndexedTriangleClassFilterList probeReverseIndexedTrianglesClassFilters();

// Optional selector for reverse-indexed-triangle probes. Format is
// "<seq>/<encoder>", for example "60/3".
// Env: DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_ROW.
RenderEncoderSelector probeReverseIndexedTrianglesRow();

// Optional selector list for reverse-indexed-triangle probes. Format is a
// comma/semicolon/space separated list of "<seq>/<encoder>", for example
// "60/0,60/1,60/3,60/4".
// Env: DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_ROWS.
RenderEncoderSelectorList probeReverseIndexedTrianglesRows();

// Diagnostic-only: preserve scissor enablement but override the scissor
// rectangle for selected indexed triangle-list draws. This changes rendering
// and is only intended to classify tile-coverage/backend-storage effects.
// Env: DXMT9_PROBE_SCISSOR_RECT, format "left,top,right,bottom".
ScissorRectOverride probeScissorRectOverride();

// Optional class filter for DXMT9_PROBE_SCISSOR_RECT. Accepted values match
// DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_CLASS.
// Env: DXMT9_PROBE_SCISSOR_RECT_CLASS.
IndexedTriangleClassFilter probeScissorRectClassFilter();

// Optional AND class-list filter for DXMT9_PROBE_SCISSOR_RECT.
// Env: DXMT9_PROBE_SCISSOR_RECT_CLASSES.
IndexedTriangleClassFilterList probeScissorRectClassFilters();

// Optional selector for DXMT9_PROBE_SCISSOR_RECT. Format is "<seq>/<encoder>".
// Env: DXMT9_PROBE_SCISSOR_RECT_ROW.
RenderEncoderSelector probeScissorRectRow();

// Optional selector list for DXMT9_PROBE_SCISSOR_RECT.
// Env: DXMT9_PROBE_SCISSOR_RECT_ROWS.
RenderEncoderSelectorList probeScissorRectRows();

// Diagnostic-only: scan accessible index-buffer bytes and report the sum of
// per-draw unique index references in encoder breakdown logs.
// Env: DXMT9_MEASURE_INDEX_REUSE.
bool measureIndexReuse();

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
