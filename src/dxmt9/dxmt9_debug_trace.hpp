#pragma once

// Env-gated debug knobs shared by encoder translation units. Previously
// file-local to backend_metal.mm's anonymous namespace; exposed here so
// the encoder translation units can reach them without duplication.

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

struct DrawOrdinalList {
  static constexpr std::size_t MaxOrdinals = 64;

  bool enabled = false;
  std::array<u64, MaxOrdinals> ordinals = {};
  std::size_t count = 0;
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
  NoAlphaBlend,
  ScreenBlend,
  StandardAlphaBlend,
  AdditiveAlphaBlend,
  Scissor,
  NoScissor,
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

enum class CullModeOverride : std::uint8_t {
  Disabled,
  None,
  Front,
  Back,
};

IndexedTriangleClassFilter makeIndexedTriangleClassFilter(
    std::string_view spec) noexcept;
IndexedTriangleClassFilterList makeIndexedTriangleClassFilterList(
    std::string_view spec) noexcept;
ScissorRectOverride makeScissorRectOverride(std::string_view spec) noexcept;
CullModeOverride makeCullModeOverride(std::string_view spec) noexcept;

DrawSeqRange makeDrawSeqRange(std::optional<u64> min, std::optional<u64> max) noexcept;
bool drawSeqRangeEnabled(DrawSeqRange range) noexcept;
bool shouldSkipDrawSeq(u64 seqId, DrawSeqRange range) noexcept;

DrawOrdinalRange makeDrawOrdinalRange(std::optional<u64> min, std::optional<u64> max) noexcept;
bool drawOrdinalRangeEnabled(DrawOrdinalRange range) noexcept;
bool shouldSkipDrawOrdinal(u64 ordinal, DrawOrdinalRange range) noexcept;
DrawOrdinalList makeDrawOrdinalList(std::string_view spec) noexcept;
bool drawOrdinalListContains(const DrawOrdinalList& list, u64 ordinal) noexcept;

RenderEncoderSelector makeRenderEncoderSelector(std::string_view spec) noexcept;
RenderEncoderSelectorList makeRenderEncoderSelectorList(std::string_view spec) noexcept;
bool renderEncoderSelectorMatches(RenderEncoderSelector selector,
                                  u64 seqId,
                                  u64 encoderIndex) noexcept;
bool renderEncoderSelectorListMatches(const RenderEncoderSelectorList& selectors,
                                      u64 seqId,
                                      u64 encoderIndex) noexcept;

// Row selectors are optional filters: no selector means every encoder is
// eligible, including diagnostic paths that do not collect encoder identity.
inline bool renderEncoderSelectionMatches(
    RenderEncoderSelector selector,
    const RenderEncoderSelectorList& selectors,
    bool hasEncoderIdentity,
    u64 seqId,
    u64 encoderIndex) noexcept {
  if (!selector.enabled && !selectors.enabled) {
    return true;
  }
  if (!hasEncoderIdentity) {
    return false;
  }
  return renderEncoderSelectorMatches(selector, seqId, encoderIndex) ||
         renderEncoderSelectorListMatches(selectors, seqId, encoderIndex);
}

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

// Diagnostic shader-source A/B: force fragment texture samples to white only
// for selected indexed triangle-list draws. Env:
// DXMT9_PROBE_FORCE_TEXTURE_WHITE.
bool probeForceTextureWhite();

// Optional class filter for DXMT9_PROBE_FORCE_TEXTURE_WHITE. Accepted values
// match DXMT9_SPLIT_LARGE_INDEXED_DRAWS_CLASS.
// Env: DXMT9_PROBE_FORCE_TEXTURE_WHITE_CLASS.
IndexedTriangleClassFilter probeForceTextureWhiteClassFilter();

// Optional AND class-list filter for DXMT9_PROBE_FORCE_TEXTURE_WHITE.
// Env: DXMT9_PROBE_FORCE_TEXTURE_WHITE_CLASSES.
IndexedTriangleClassFilterList probeForceTextureWhiteClassFilters();

// Optional selector for DXMT9_PROBE_FORCE_TEXTURE_WHITE. Format is
// "<seq>/<encoder>".
// Env: DXMT9_PROBE_FORCE_TEXTURE_WHITE_ROW.
RenderEncoderSelector probeForceTextureWhiteRow();

// Optional selector list for DXMT9_PROBE_FORCE_TEXTURE_WHITE.
// Env: DXMT9_PROBE_FORCE_TEXTURE_WHITE_ROWS.
RenderEncoderSelectorList probeForceTextureWhiteRows();

// Optional texture0 descriptor filter for DXMT9_PROBE_FORCE_TEXTURE_WHITE.
// Values are ANDed in handle/width/height/format order.
// Env: DXMT9_PROBE_FORCE_TEXTURE_WHITE_TEXTURE0(_WIDTH/_HEIGHT/_FORMAT).
std::optional<u64> probeForceTextureWhiteTexture0Handle();
std::optional<u64> probeForceTextureWhiteTexture0Width();
std::optional<u64> probeForceTextureWhiteTexture0Height();
std::optional<u64> probeForceTextureWhiteTexture0Format();

// Optional per-process draw ordinal filters for DXMT9_PROBE_FORCE_TEXTURE_WHITE.
// Unlike encoder_draw_index, draw ordinals are assigned before probe matching
// and can isolate individual effect draws inside a render encoder.
// Env: DXMT9_PROBE_FORCE_TEXTURE_WHITE_DRAW_ORDINAL_MIN/MAX and
// DXMT9_PROBE_FORCE_TEXTURE_WHITE_DRAW_ORDINALS.
DrawOrdinalRange probeForceTextureWhiteDrawOrdinalRange();
DrawOrdinalList probeForceTextureWhiteDrawOrdinalList();

// Optional command index filters for DXMT9_PROBE_FORCE_TEXTURE_WHITE.
// command_index is the draw command slot inside the currently replayed chunk
// and is more stable than global draw ordinal for frame-capture probes.
// Env: DXMT9_PROBE_FORCE_TEXTURE_WHITE_COMMAND_INDEX_MIN/MAX and
// DXMT9_PROBE_FORCE_TEXTURE_WHITE_COMMAND_INDEXES.
DrawOrdinalRange probeForceTextureWhiteCommandIndexRange();
DrawOrdinalList probeForceTextureWhiteCommandIndexList();

// Optional command-local subdraw filters for DXMT9_PROBE_FORCE_TEXTURE_WHITE.
// command_draw_index is the draw-run subdraw index reported by
// dxmt9-effect-draw/geometry. It is stable when a replayed command expands
// into multiple indexed draws with the same command_index.
// Env: DXMT9_PROBE_FORCE_TEXTURE_WHITE_COMMAND_DRAW_INDEX_MIN/MAX and
// DXMT9_PROBE_FORCE_TEXTURE_WHITE_COMMAND_DRAW_INDEXES.
DrawOrdinalRange probeForceTextureWhiteCommandDrawIndexRange();
DrawOrdinalList probeForceTextureWhiteCommandDrawIndexList();

// Diagnostic render-state A/B: force color blending off while preserving
// color-write masks. Env: DXMT9_PROBE_DISABLE_ALPHA_BLEND.
bool probeDisableAlphaBlend();

// Optional class filter for DXMT9_PROBE_DISABLE_ALPHA_BLEND. Accepted values
// match DXMT9_SPLIT_LARGE_INDEXED_DRAWS_CLASS.
// Env: DXMT9_PROBE_DISABLE_ALPHA_BLEND_CLASS.
IndexedTriangleClassFilter probeDisableAlphaBlendClassFilter();

// Optional AND class-list filter for DXMT9_PROBE_DISABLE_ALPHA_BLEND.
// Env: DXMT9_PROBE_DISABLE_ALPHA_BLEND_CLASSES.
IndexedTriangleClassFilterList probeDisableAlphaBlendClassFilters();

// Optional selector for DXMT9_PROBE_DISABLE_ALPHA_BLEND. Format is
// "<seq>/<encoder>".
// Env: DXMT9_PROBE_DISABLE_ALPHA_BLEND_ROW.
RenderEncoderSelector probeDisableAlphaBlendRow();

// Optional selector list for DXMT9_PROBE_DISABLE_ALPHA_BLEND.
// Env: DXMT9_PROBE_DISABLE_ALPHA_BLEND_ROWS.
RenderEncoderSelectorList probeDisableAlphaBlendRows();

// Optional texture0 descriptor filter for DXMT9_PROBE_DISABLE_ALPHA_BLEND.
// Values are ANDed in handle/width/height/format order.
// Env: DXMT9_PROBE_DISABLE_ALPHA_BLEND_TEXTURE0(_WIDTH/_HEIGHT/_FORMAT).
std::optional<u64> probeDisableAlphaBlendTexture0Handle();
std::optional<u64> probeDisableAlphaBlendTexture0Width();
std::optional<u64> probeDisableAlphaBlendTexture0Height();
std::optional<u64> probeDisableAlphaBlendTexture0Format();

// Diagnostic render-state A/B: keep depth testing but force depth writes off.
// Env: DXMT9_PROBE_DISABLE_DEPTH_WRITE.
bool probeDisableDepthWrite();

// Optional class filter for DXMT9_PROBE_DISABLE_DEPTH_WRITE. Accepted values
// match DXMT9_SPLIT_LARGE_INDEXED_DRAWS_CLASS.
// Env: DXMT9_PROBE_DISABLE_DEPTH_WRITE_CLASS.
IndexedTriangleClassFilter probeDisableDepthWriteClassFilter();

// Optional AND class-list filter for DXMT9_PROBE_DISABLE_DEPTH_WRITE.
// Env: DXMT9_PROBE_DISABLE_DEPTH_WRITE_CLASSES.
IndexedTriangleClassFilterList probeDisableDepthWriteClassFilters();

// Optional selector for DXMT9_PROBE_DISABLE_DEPTH_WRITE. Format is
// "<seq>/<encoder>".
// Env: DXMT9_PROBE_DISABLE_DEPTH_WRITE_ROW.
RenderEncoderSelector probeDisableDepthWriteRow();

// Optional selector list for DXMT9_PROBE_DISABLE_DEPTH_WRITE.
// Env: DXMT9_PROBE_DISABLE_DEPTH_WRITE_ROWS.
RenderEncoderSelectorList probeDisableDepthWriteRows();

// Diagnostic render-state A/B: keep depth enable/write state but force the
// depth compare function to Always. Env: DXMT9_PROBE_DEPTH_FUNC_ALWAYS.
bool probeDepthFuncAlways();

// Optional class filter for DXMT9_PROBE_DEPTH_FUNC_ALWAYS. Accepted values
// match DXMT9_SPLIT_LARGE_INDEXED_DRAWS_CLASS.
// Env: DXMT9_PROBE_DEPTH_FUNC_ALWAYS_CLASS.
IndexedTriangleClassFilter probeDepthFuncAlwaysClassFilter();

// Optional AND class-list filter for DXMT9_PROBE_DEPTH_FUNC_ALWAYS.
// Env: DXMT9_PROBE_DEPTH_FUNC_ALWAYS_CLASSES.
IndexedTriangleClassFilterList probeDepthFuncAlwaysClassFilters();

// Optional selector for DXMT9_PROBE_DEPTH_FUNC_ALWAYS. Format is
// "<seq>/<encoder>".
// Env: DXMT9_PROBE_DEPTH_FUNC_ALWAYS_ROW.
RenderEncoderSelector probeDepthFuncAlwaysRow();

// Optional selector list for DXMT9_PROBE_DEPTH_FUNC_ALWAYS.
// Env: DXMT9_PROBE_DEPTH_FUNC_ALWAYS_ROWS.
RenderEncoderSelectorList probeDepthFuncAlwaysRows();

// Optional texture0 descriptor filter for DXMT9_PROBE_DEPTH_FUNC_ALWAYS.
// Values are ANDed in handle/width/height/format order.
// Env: DXMT9_PROBE_DEPTH_FUNC_ALWAYS_TEXTURE0(_WIDTH/_HEIGHT/_FORMAT).
std::optional<u64> probeDepthFuncAlwaysTexture0Handle();
std::optional<u64> probeDepthFuncAlwaysTexture0Width();
std::optional<u64> probeDepthFuncAlwaysTexture0Height();
std::optional<u64> probeDepthFuncAlwaysTexture0Format();

// Diagnostic backend-shape A/B: for selected depth-only indexed triangle-list
// draws, request a render PSO with no fragment function. The encoder still
// applies strict depth/color/alpha/stencil/clip gates before routing.
// Env: DXMT9_PROBE_FRAGMENTLESS_DEPTH_ONLY.
bool probeFragmentlessDepthOnly();

// Diagnostic sub-mode for DXMT9_PROBE_FRAGMENTLESS_DEPTH_ONLY. When enabled,
// keep the ordinary pair-local VSOut layout instead of forcing position-only,
// isolating fragment-function removal from VSOut-shape changes.
// Env: DXMT9_PROBE_FRAGMENTLESS_DEPTH_ONLY_KEEP_VSOUT.
bool probeFragmentlessDepthOnlyKeepVSOut();

// Optional class filter for DXMT9_PROBE_FRAGMENTLESS_DEPTH_ONLY. Accepted
// values match DXMT9_SPLIT_LARGE_INDEXED_DRAWS_CLASS.
// Env: DXMT9_PROBE_FRAGMENTLESS_DEPTH_ONLY_CLASS.
IndexedTriangleClassFilter probeFragmentlessDepthOnlyClassFilter();

// Optional AND class-list filter for DXMT9_PROBE_FRAGMENTLESS_DEPTH_ONLY.
// Env: DXMT9_PROBE_FRAGMENTLESS_DEPTH_ONLY_CLASSES.
IndexedTriangleClassFilterList probeFragmentlessDepthOnlyClassFilters();

// Optional selector for DXMT9_PROBE_FRAGMENTLESS_DEPTH_ONLY. Format is
// "<seq>/<encoder>".
// Env: DXMT9_PROBE_FRAGMENTLESS_DEPTH_ONLY_ROW.
RenderEncoderSelector probeFragmentlessDepthOnlyRow();

// Optional selector list for DXMT9_PROBE_FRAGMENTLESS_DEPTH_ONLY.
// Env: DXMT9_PROBE_FRAGMENTLESS_DEPTH_ONLY_ROWS.
RenderEncoderSelectorList probeFragmentlessDepthOnlyRows();

// Diagnostic-only: force the Metal cull mode for selected indexed
// triangle-list draws while preserving the rest of the render state.
// Env: DXMT9_PROBE_FORCE_CULL_MODE=none|front|back.
CullModeOverride probeForceCullMode();

// Optional class filter for DXMT9_PROBE_FORCE_CULL_MODE. Accepted values match
// DXMT9_SPLIT_LARGE_INDEXED_DRAWS_CLASS.
// Env: DXMT9_PROBE_FORCE_CULL_MODE_CLASS.
IndexedTriangleClassFilter probeForceCullModeClassFilter();

// Optional AND class-list filter for DXMT9_PROBE_FORCE_CULL_MODE.
// Env: DXMT9_PROBE_FORCE_CULL_MODE_CLASSES.
IndexedTriangleClassFilterList probeForceCullModeClassFilters();

// Optional selector for DXMT9_PROBE_FORCE_CULL_MODE. Format is "<seq>/<encoder>".
// Env: DXMT9_PROBE_FORCE_CULL_MODE_ROW.
RenderEncoderSelector probeForceCullModeRow();

// Optional selector list for DXMT9_PROBE_FORCE_CULL_MODE.
// Env: DXMT9_PROBE_FORCE_CULL_MODE_ROWS.
RenderEncoderSelectorList probeForceCullModeRows();

// When set, force indexed draws to be expanded into flat vertex lists.
// Env: DXMT_FORCE_EXPAND_INDEXED.
bool forceExpandIndexed();

// Diagnostic-only: force selected indexed triangle-list draws to be expanded
// into flat vertex lists. Uses row/class filters plus the shared indexed
// triangle encoder draw range. Env: DXMT9_PROBE_FORCE_EXPAND_INDEXED.
bool probeForceExpandIndexed();

// Optional class filter for DXMT9_PROBE_FORCE_EXPAND_INDEXED. Accepted values
// match DXMT9_SPLIT_LARGE_INDEXED_DRAWS_CLASS.
// Env: DXMT9_PROBE_FORCE_EXPAND_INDEXED_CLASS.
IndexedTriangleClassFilter probeForceExpandIndexedClassFilter();

// Optional AND class-list filter for DXMT9_PROBE_FORCE_EXPAND_INDEXED.
// Env: DXMT9_PROBE_FORCE_EXPAND_INDEXED_CLASSES.
IndexedTriangleClassFilterList probeForceExpandIndexedClassFilters();

// Optional selector for DXMT9_PROBE_FORCE_EXPAND_INDEXED. Format is
// "<seq>/<encoder>".
// Env: DXMT9_PROBE_FORCE_EXPAND_INDEXED_ROW.
RenderEncoderSelector probeForceExpandIndexedRow();

// Optional selector list for DXMT9_PROBE_FORCE_EXPAND_INDEXED.
// Env: DXMT9_PROBE_FORCE_EXPAND_INDEXED_ROWS.
RenderEncoderSelectorList probeForceExpandIndexedRows();

// Diagnostic-only: stage selected indexed draw stream/IB source buffers through
// encoder-local transient slabs to reduce Metal buffer-handle churn while
// preserving draw order and index bytes. Env: DXMT9_PROBE_STAGE_STREAM_IB.
bool probeStageStreamIb();

// Optional selector for DXMT9_PROBE_STAGE_STREAM_IB. Format is "<seq>/<encoder>".
// Env: DXMT9_PROBE_STAGE_STREAM_IB_ROW.
RenderEncoderSelector probeStageStreamIbRow();

// Optional selector list for DXMT9_PROBE_STAGE_STREAM_IB.
// Env: DXMT9_PROBE_STAGE_STREAM_IB_ROWS.
RenderEncoderSelectorList probeStageStreamIbRows();

// Disable the compatibility heuristic that auto-expands selected indexed
// draws. Env: DXMT_DISABLE_AUTO_EXPAND_INDEXED.
bool disableAutoExpandIndexed();

// Use Metal's native baseVertex for indexed draws and keep the shader-side
// vertexBaseIndex at zero. Env: DXMT9_USE_NATIVE_METAL_BASE_VERTEX.
bool useNativeMetalBaseVertex();

// Diagnostic-only: for strict screen-blend indexed triangle lists, submit a
// transient index buffer with primitive order reversed. Screen-blend output is
// destination-dependent; same-input translated-FS probes have shown bit-exact
// differences, so this is not production-safe.
// Env: DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER.
bool optimizeScreenBlendIndexOrder();

// Optional class filter for DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER. Accepted
// values: any, opaque-depth-write, nonopaque, depth-read, alpha-blend,
// no-alpha-blend, screen-blend, standard-alpha, additive-alpha, scissor,
// no-scissor, textured, and large4096.
// Env: DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER_CLASS.
IndexedTriangleClassFilter optimizeScreenBlendIndexOrderClassFilter();

// Optional AND class-list filter for DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER.
// Values match DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER_CLASS and may be
// separated by comma, semicolon, space, '+', or '&'.
// Env: DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER_CLASSES.
IndexedTriangleClassFilterList optimizeScreenBlendIndexOrderClassFilters();

// Optional minimum original stream0 byte-span filter for
// DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER. Zero disables it.
// Env: DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER_STREAM0_SPAN_MIN.
u64 optimizeScreenBlendIndexOrderStream0SpanMin();

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

// Split indexed triangle-list draws into multiple contiguous Metal drawIndexed
// calls when the chunk's stream0 byte span would exceed this limit. Zero
// disables stream0-span splitting. Uses the same row/class filters as
// DXMT9_SPLIT_LARGE_INDEXED_DRAWS.
// Env: DXMT9_SPLIT_LARGE_INDEXED_DRAWS_STREAM0_SPAN_MAX.
u64 splitLargeIndexedDrawStream0SpanMax();

// Optional cap for split-large-indexed diagnostics. If a source draw would
// produce more chunks than this value, it is left unsplit. Zero disables the
// cap.
// Env: DXMT9_SPLIT_LARGE_INDEXED_DRAWS_MAX_CHUNKS_PER_DRAW.
std::uint32_t splitLargeIndexedDrawMaxChunksPerDraw();

// Optional class filter for DXMT9_SPLIT_LARGE_INDEXED_DRAWS. Accepted values:
// any, opaque-depth-write, nonopaque, depth-read, alpha-blend, no-alpha-blend,
// screen-blend, standard-alpha, additive-alpha, scissor, no-scissor, textured,
// and large4096.
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

// Diagnostic-only: keep indexed draws and render state intact, but submit a
// transient index buffer with triangle-list primitive order sorted by triangle
// min/max index. Uses the reverse-indexed-triangles row/class/span filters.
// Env: DXMT9_PROBE_SORT_INDEXED_TRIANGLES_BY_MIN_INDEX.
bool probeSortIndexedTrianglesByMinIndex();

// Diagnostic-only: keep indexed draws and render state intact, but submit a
// transient index buffer with triangle-list primitive order greedily reordered
// around a small post-transform vertex cache. Uses the reverse-indexed-
// triangles row/class/span filters.
// Env: DXMT9_PROBE_OPTIMIZE_INDEXED_TRIANGLES_VERTEX_CACHE.
bool probeOptimizeIndexedTrianglesVertexCache();

// Opt-in optimization: submit an LRU32 cache-aware reordered index buffer for
// opaque depth-writing triangle-list draws when the stable source index buffer
// candidate clears DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE_MIN_GAIN_PCT.
// Unlike the probe flag below, this is not scoped by seq/enc row filters and
// never bypasses the opaque-depth-write safety gate.
// Env: DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE.
bool optimizeOpaqueDepthIndexCache();

// Experimental scope extension for the opaque-depth index-cache path. In
// addition to the production predicate, admit reverse-depth comparisons and
// source-replacement blending (ONE/ZERO/ADD). The submitted render state is
// unchanged; only triangle order may change.
// Env: DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE_EXTENDED_SCOPE.
bool optimizeOpaqueDepthIndexCacheExtendedScope();

// Experimental draw-boundary coalescing. Consecutive compatible indexed
// triangle-list DrawParams whose source-IB ranges are contiguous are submitted
// as one Metal draw without copying or changing index order.
// Env: DXMT9_OPTIMIZE_COMPATIBLE_INDEXED_DRAW_MERGE.
bool optimizeCompatibleIndexedDrawMerge();

// Minimum whole-percent LRU32 miss reduction required by
// DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE. Defaults to 10.
// Env: DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE_MIN_GAIN_PCT.
std::uint32_t optimizeOpaqueDepthIndexCacheMinGainPct();

// Profiling-only opt-in: submit an LRU32 cache-aware reordered index buffer
// for strict screen-blend triangle-list draws. This uses the strict
// screen-blend predicate, is not row-scoped, and does not bypass alpha-test,
// separate-alpha, stencil, clip-plane, or depth-write safety gates. It is not
// production-safe yet: same-input translated-FS probes have shown small
// destination-dependent output differences.
// Env: DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_CACHE.
bool optimizeScreenBlendIndexCache();

// Minimum whole-percent LRU32 miss reduction required by
// DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_CACHE. Defaults to 10.
// Env: DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_CACHE_MIN_GAIN_PCT.
std::uint32_t optimizeScreenBlendIndexCacheMinGainPct();

// Diagnostic-only: submit the same LRU32 cache-aware candidate measured by
// DXMT9_MEASURE_INDEX_CACHE_OPT_CANDIDATE, but only when the candidate reduces
// LRU32 misses by at least
// DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE_MIN_GAIN_PCT. Uses the reverse-
// indexed-triangles row/class/span filters, opaque-depth-write safety gate,
// and a source-IB keyed cached reordered index buffer instead of per-draw
// transient reorder uploads.
// Env: DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE.
bool probeApplyIndexCacheOptCandidate();

// Diagnostic-only: allow DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE to bypass
// the opaque-depth-write safety gate. This can mutate depth-read/blended rows
// where primitive order may affect final color writers; use only for targeted
// probes with same-input semantic image validation.
// Env: DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE_UNSAFE_NONOPAQUE.
bool probeApplyIndexCacheOptCandidateUnsafeNonOpaque();

// Minimum whole-percent LRU32 miss reduction required by
// DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE. Defaults to 10.
// Env: DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE_MIN_GAIN_PCT.
std::uint32_t probeApplyIndexCacheOptCandidateMinGainPct();

// Optional class filter for reverse-indexed-triangle probes. Accepted values:
// any, opaque-depth-write, nonopaque, depth-read, alpha-blend, no-alpha-blend,
// screen-blend, standard-alpha, additive-alpha, scissor, no-scissor, textured,
// and large4096.
// Env: DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_CLASS.
IndexedTriangleClassFilter probeReverseIndexedTrianglesClassFilter();

// Optional AND class-list filter for reverse-indexed-triangle probes. Accepted
// values match DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_CLASS and may be separated
// by comma, semicolon, space, '+', or '&'. Example: "large4096,scissor".
// Env: DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_CLASSES.
IndexedTriangleClassFilterList probeReverseIndexedTrianglesClassFilters();

// Optional minimum original stream0 byte-span filter for reverse-indexed-
// triangle probes. Zero disables it.
// Env: DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_STREAM0_SPAN_MIN.
u64 probeReverseIndexedTrianglesStream0SpanMin();

// Optional encoder-local draw-index range for indexed triangle primitive/
// locality probes. This is evaluated after row filters against the
// per-render-encoder draw index reported in perf probe draw samples.
// Env: DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_MIN/MAX.
DrawOrdinalRange probeIndexedTriangleEncoderDrawRange();

// Env: DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_EXCLUDE.
DrawOrdinalList probeIndexedTriangleEncoderDrawExcludeList();

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

// Diagnostic-only: when index-reuse measurement is enabled, build a
// cache-aware LRU32 reordered index candidate without submitting it and report
// original-vs-candidate cache-miss estimates in encoder breakdown logs.
// Env: DXMT9_MEASURE_INDEX_CACHE_OPT_CANDIDATE.
bool measureIndexCacheOptCandidate();

// True when indexed triangle-list diagnostics or opt-in reordering paths are
// enabled. The draw encoder uses this to keep the default indexed path from
// preparing diagnostic byte spans and row/class filters when every such knob is
// off.
bool indexedTriangleDiagnosticsEnabled();

// Diagnostic-only: directory for dumping replayable indexed triangle geometry
// payloads. Uses the reverse-indexed-triangle row/class/span filters and the
// indexed triangle encoder draw range. Env: DXMT9_DUMP_INDEXED_GEOMETRY_DIR.
std::string_view indexedGeometryDumpDir();

// Maximum number of indexed draw geometry payloads to dump. Defaults to a
// small cap when DXMT9_DUMP_INDEXED_GEOMETRY_DIR is set.
// Env: DXMT9_DUMP_INDEXED_GEOMETRY_MAX_DRAWS.
std::uint32_t indexedGeometryDumpMaxDraws();

// Optional shader-hash filters for indexed geometry payload dumping. Values
// accept decimal or 0x-prefixed hashes from 3dmark05-perf-indexed-probe-draws.csv.
// Env: DXMT9_DUMP_INDEXED_GEOMETRY_VS / DXMT9_DUMP_INDEXED_GEOMETRY_PS.
std::optional<u64> indexedGeometryDumpVertexShaderHash();
std::optional<u64> indexedGeometryDumpPixelShaderHash();

// Optional texture0 handle filter for indexed geometry payload dumping. Values
// accept decimal or 0x-prefixed handles from probe draw CSV/log output.
// Env: DXMT9_DUMP_INDEXED_GEOMETRY_TEXTURE0.
std::optional<u64> indexedGeometryDumpTexture0Handle();

// Optional texture0 descriptor filters for indexed geometry payload dumping.
// These are stable when run-local object handles shift with resource creation
// order. Env: DXMT9_DUMP_INDEXED_GEOMETRY_TEXTURE0_WIDTH/HEIGHT/FORMAT.
std::optional<u64> indexedGeometryDumpTexture0Width();
std::optional<u64> indexedGeometryDumpTexture0Height();
std::optional<u64> indexedGeometryDumpTexture0Format();

// Diagnostic-only: include real per-draw uniform payloads beside indexed
// geometry dumps. Env: DXMT9_DUMP_INDEXED_GEOMETRY_CBUFS.
bool indexedGeometryDumpCbufs();

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
