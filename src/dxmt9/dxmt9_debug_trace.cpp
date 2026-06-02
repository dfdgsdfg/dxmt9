#include "dxmt9_debug_trace.hpp"
#include "../util/config/config.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>

namespace dxmt9::debug {

namespace {

bool isSpace(char ch) noexcept {
  return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

bool isListSeparator(char ch) noexcept {
  return ch == ',' || ch == ';' || isSpace(ch);
}

bool isClassListSeparator(char ch) noexcept {
  return isListSeparator(ch) || ch == '+' || ch == '&';
}

void skipSpaces(std::string_view spec, std::size_t& pos) noexcept {
  while (pos < spec.size() && isSpace(spec[pos])) {
    ++pos;
  }
}

void skipListSeparators(std::string_view spec, std::size_t& pos) noexcept {
  while (pos < spec.size() && isListSeparator(spec[pos])) {
    ++pos;
  }
}

void skipClassListSeparators(std::string_view spec, std::size_t& pos) noexcept {
  while (pos < spec.size() && isClassListSeparator(spec[pos])) {
    ++pos;
  }
}

bool parseU64(std::string_view spec, std::size_t& pos, u64& value) noexcept {
  skipSpaces(spec, pos);
  if (pos >= spec.size() || spec[pos] < '0' || spec[pos] > '9') {
    return false;
  }

  u64 parsed = 0;
  while (pos < spec.size() && spec[pos] >= '0' && spec[pos] <= '9') {
    const auto digit = static_cast<u64>(spec[pos] - '0');
    if (parsed > (std::numeric_limits<u64>::max() - digit) / 10u) {
      return false;
    }
    parsed = parsed * 10u + digit;
    ++pos;
  }

  value = parsed;
  return true;
}

bool parseRenderEncoderSelectorAt(std::string_view spec,
                                  std::size_t& pos,
                                  bool allowCommaPairSeparator,
                                  RenderEncoderSelector& selector) noexcept {
  u64 seq = 0;
  u64 encoder = 0;
  if (!parseU64(spec, pos, seq)) {
    return false;
  }

  skipSpaces(spec, pos);
  if (pos >= spec.size()) {
    return false;
  }

  const char separator = spec[pos];
  if (separator != '/' && separator != ':' &&
      !(allowCommaPairSeparator && separator == ',')) {
    return false;
  }
  ++pos;

  if (!parseU64(spec, pos, encoder)) {
    return false;
  }

  selector = RenderEncoderSelector{
      .enabled = true,
      .seqId = seq,
      .encoderIndex = encoder,
  };
  return true;
}

char normalizedTokenChar(char ch) noexcept {
  if (ch == '_') {
    return '-';
  }
  if (ch >= 'A' && ch <= 'Z') {
    return static_cast<char>(ch - 'A' + 'a');
  }
  return ch;
}

std::string_view trimToken(std::string_view token) noexcept {
  while (!token.empty() && isSpace(token.front())) {
    token.remove_prefix(1);
  }
  while (!token.empty() && isSpace(token.back())) {
    token.remove_suffix(1);
  }
  return token;
}

bool tokenEquals(std::string_view token, std::string_view expected) noexcept {
  token = trimToken(token);
  if (token.size() != expected.size()) {
    return false;
  }
  for (std::size_t i = 0; i < token.size(); ++i) {
    if (normalizedTokenChar(token[i]) != expected[i]) {
      return false;
    }
  }
  return true;
}

IndexedTriangleClassFilter parseIndexedTriangleClassFilter(
    std::string_view spec) noexcept {
  if (spec.empty() || tokenEquals(spec, "any")) {
    return IndexedTriangleClassFilter::Any;
  }
  if (tokenEquals(spec, "opaque") ||
      tokenEquals(spec, "opaque-depth-write")) {
    return IndexedTriangleClassFilter::OpaqueDepthWrite;
  }
  if (tokenEquals(spec, "nonopaque") ||
      tokenEquals(spec, "non-opaque")) {
    return IndexedTriangleClassFilter::NonOpaque;
  }
  if (tokenEquals(spec, "depth-read")) {
    return IndexedTriangleClassFilter::DepthRead;
  }
  if (tokenEquals(spec, "alpha-blend") ||
      tokenEquals(spec, "blend")) {
    return IndexedTriangleClassFilter::AlphaBlend;
  }
  if (tokenEquals(spec, "scissor")) {
    return IndexedTriangleClassFilter::Scissor;
  }
  if (tokenEquals(spec, "textured") ||
      tokenEquals(spec, "texture")) {
    return IndexedTriangleClassFilter::Textured;
  }
  if (tokenEquals(spec, "large4096") ||
      tokenEquals(spec, "large-4096")) {
    return IndexedTriangleClassFilter::Large4096;
  }
  return IndexedTriangleClassFilter::Any;
}

IndexedTriangleClassFilterList parseIndexedTriangleClassFilterList(
    std::string_view spec) noexcept {
  IndexedTriangleClassFilterList result = {};
  std::size_t pos = 0;

  while (pos < spec.size() &&
         result.count < IndexedTriangleClassFilterList::MaxFilters) {
    skipClassListSeparators(spec, pos);
    if (pos >= spec.size()) {
      break;
    }

    const std::size_t tokenBegin = pos;
    while (pos < spec.size() && !isClassListSeparator(spec[pos])) {
      ++pos;
    }
    const auto filter =
        parseIndexedTriangleClassFilter(spec.substr(tokenBegin, pos - tokenBegin));
    if (filter != IndexedTriangleClassFilter::Any) {
      result.filters[result.count++] = filter;
    }
  }

  return result;
}

}  // namespace

RenderEncoderSelector makeRenderEncoderSelector(std::string_view spec) noexcept {
  if (spec.empty()) {
    return {};
  }

  std::size_t pos = 0;
  RenderEncoderSelector selector = {};
  if (!parseRenderEncoderSelectorAt(spec, pos, true, selector)) {
    return {};
  }
  return selector;
}

RenderEncoderSelectorList makeRenderEncoderSelectorList(std::string_view spec) noexcept {
  RenderEncoderSelectorList result = {};
  std::size_t pos = 0;

  while (pos < spec.size() && result.count < RenderEncoderSelectorList::MaxSelectors) {
    skipListSeparators(spec, pos);
    if (pos >= spec.size()) {
      break;
    }

    RenderEncoderSelector selector = {};
    if (parseRenderEncoderSelectorAt(spec, pos, false, selector)) {
      result.selectors[result.count++] = selector;
      continue;
    }

    while (pos < spec.size() && !isListSeparator(spec[pos])) {
      ++pos;
    }
  }

  result.enabled = result.count != 0;
  return result;
}

IndexedTriangleClassFilter makeIndexedTriangleClassFilter(
    std::string_view spec) noexcept {
  return parseIndexedTriangleClassFilter(spec);
}

IndexedTriangleClassFilterList makeIndexedTriangleClassFilterList(
    std::string_view spec) noexcept {
  return parseIndexedTriangleClassFilterList(spec);
}

bool renderEncoderSelectorMatches(RenderEncoderSelector selector,
                                  u64 seqId,
                                  u64 encoderIndex) noexcept {
  return selector.enabled &&
         selector.seqId == seqId &&
         selector.encoderIndex == encoderIndex;
}

bool renderEncoderSelectorListMatches(const RenderEncoderSelectorList& selectors,
                                      u64 seqId,
                                      u64 encoderIndex) noexcept {
  if (!selectors.enabled) {
    return false;
  }
  for (std::size_t i = 0; i < selectors.count; ++i) {
    if (renderEncoderSelectorMatches(selectors.selectors[i], seqId, encoderIndex)) {
      return true;
    }
  }
  return false;
}

RenderEncoderSelector parseRenderEncoderSelector(const char* envName) {
  return makeRenderEncoderSelector(util::getenvString(envName));
}

DrawSeqRange makeDrawSeqRange(std::optional<u64> min, std::optional<u64> max) noexcept {
  return DrawSeqRange{
      .hasMin = min.has_value(),
      .hasMax = max.has_value(),
      .min = min.value_or(0),
      .max = max.value_or(0),
  };
}

bool drawSeqRangeEnabled(DrawSeqRange range) noexcept {
  return range.hasMin || range.hasMax;
}

bool shouldSkipDrawSeq(u64 seqId, DrawSeqRange range) noexcept {
  if (range.hasMin && seqId < range.min) {
    return true;
  }
  if (range.hasMax && seqId > range.max) {
    return true;
  }
  return false;
}

DrawOrdinalRange makeDrawOrdinalRange(std::optional<u64> min, std::optional<u64> max) noexcept {
  return DrawOrdinalRange{
      .hasMin = min.has_value(),
      .hasMax = max.has_value(),
      .min = min.value_or(0),
      .max = max.value_or(0),
  };
}

bool drawOrdinalRangeEnabled(DrawOrdinalRange range) noexcept {
  return range.hasMin || range.hasMax;
}

bool shouldSkipDrawOrdinal(u64 ordinal, DrawOrdinalRange range) noexcept {
  if (range.hasMin && ordinal < range.min) {
    return true;
  }
  if (range.hasMax && ordinal > range.max) {
    return true;
  }
  return false;
}

bool forceVisibleDraw() {
  static const bool v = util::getenvFlag("DXMT_DEBUG_FORCE_VISIBLE");
  return v;
}

bool forceFullscreenVertexShader() {
  static const bool v = util::getenvFlag("DXMT_DEBUG_FORCE_FULLSCREEN_VERTEX");
  return v;
}

bool forceFragmentShaderColor() {
  static const bool v = util::getenvFlag("DXMT_DEBUG_FORCE_FRAGMENT_COLOR");
  return v;
}

bool skipAllDraws() {
  static const bool v = util::getenvFlag("DXMT_SKIP_ALL_DRAWS");
  return v;
}

bool shouldSkipDrawSeq(u64 seqId) {
  static const DrawSeqRange range = makeDrawSeqRange(
      util::getenvU64Auto("DXMT9_DRAW_SEQ_MIN"),
      util::getenvU64Auto("DXMT9_DRAW_SEQ_MAX"));
  return shouldSkipDrawSeq(seqId, range);
}

u64 nextDrawOrdinal() noexcept {
  static std::atomic<u64> ordinal{0};
  return ordinal.fetch_add(1, std::memory_order_relaxed) + 1u;
}

bool shouldSkipDrawOrdinal(u64 ordinal) {
  static const DrawOrdinalRange range = makeDrawOrdinalRange(
      util::getenvU64Auto("DXMT9_DRAW_ORDINAL_MIN"),
      util::getenvU64Auto("DXMT9_DRAW_ORDINAL_MAX"));
  return shouldSkipDrawOrdinal(ordinal, range);
}

bool disableScissor() {
  static const bool v = util::getenvFlag("DXMT_DISABLE_SCISSOR");
  return v;
}

bool disableCull() {
  static const bool v = util::getenvFlag("DXMT_DISABLE_CULL");
  return v;
}

bool frontFaceCounterClockwise() {
  static const bool v = util::getenvFlag("DXMT_DEBUG_FRONT_FACE_CCW");
  return v;
}

bool flipTranslatedVertexY() {
  static const bool v = util::getenvFlag("DXMT_DEBUG_FLIP_VERTEX_Y");
  return v;
}

bool forcePixelVFlip() {
  static const bool v = util::getenvFlag("DXMT_DEBUG_FORCE_PIXEL_V_FLIP");
  return v;
}

bool disableAlphaTest() {
  static const bool v = util::getenvFlag("DXMT_DISABLE_ALPHA_TEST");
  return v;
}

bool disableFog() {
  static const bool v = util::getenvFlag("DXMT_DISABLE_FOG");
  return v;
}

bool forceTextureWhite() {
  static const bool v = util::getenvFlag("DXMT_FORCE_TEXTURE_WHITE");
  return v;
}

bool probeDisableAlphaBlend() {
  static const bool v = util::getenvFlag("DXMT9_PROBE_DISABLE_ALPHA_BLEND");
  return v;
}

bool probeDisableDepthWrite() {
  static const bool v = util::getenvFlag("DXMT9_PROBE_DISABLE_DEPTH_WRITE");
  return v;
}

bool probeDepthFuncAlways() {
  static const bool v = util::getenvFlag("DXMT9_PROBE_DEPTH_FUNC_ALWAYS");
  return v;
}

bool forceExpandIndexed() {
  static const bool v = util::getenvFlag("DXMT_FORCE_EXPAND_INDEXED");
  return v;
}

bool disableAutoExpandIndexed() {
  static const bool v = util::getenvFlag("DXMT_DISABLE_AUTO_EXPAND_INDEXED");
  return v;
}

bool useNativeMetalBaseVertex() {
  static const bool v = util::getenvFlag("DXMT9_USE_NATIVE_METAL_BASE_VERTEX");
  return v;
}

bool optimizeScreenBlendIndexOrder() {
  static const bool v =
      util::getenvFlag("DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER");
  return v;
}

IndexedTriangleClassFilter optimizeScreenBlendIndexOrderClassFilter() {
  static const IndexedTriangleClassFilter filter =
      makeIndexedTriangleClassFilter(
          util::getenvString("DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER_CLASS"));
  return filter;
}

IndexedTriangleClassFilterList optimizeScreenBlendIndexOrderClassFilters() {
  static const IndexedTriangleClassFilterList filters =
      makeIndexedTriangleClassFilterList(
          util::getenvString("DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER_CLASSES"));
  return filters;
}

RenderEncoderSelector optimizeScreenBlendIndexOrderRow() {
  static const RenderEncoderSelector selector =
      parseRenderEncoderSelector("DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER_ROW");
  return selector;
}

RenderEncoderSelectorList optimizeScreenBlendIndexOrderRows() {
  static const RenderEncoderSelectorList selectors =
      makeRenderEncoderSelectorList(
          util::getenvString("DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER_ROWS"));
  return selectors;
}

std::uint32_t splitLargeIndexedDrawPrimitiveLimit() {
  static const std::uint32_t limit = [] {
    const auto value = util::getenvU64Auto("DXMT9_SPLIT_LARGE_INDEXED_DRAWS");
    if (!value.has_value()) {
      return 0u;
    }
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(*value, std::numeric_limits<std::uint32_t>::max()));
  }();
  return limit;
}

IndexedTriangleClassFilter splitLargeIndexedDrawClassFilter() {
  static const IndexedTriangleClassFilter filter =
      makeIndexedTriangleClassFilter(
          util::getenvString("DXMT9_SPLIT_LARGE_INDEXED_DRAWS_CLASS"));
  return filter;
}

IndexedTriangleClassFilterList splitLargeIndexedDrawClassFilters() {
  static const IndexedTriangleClassFilterList filters =
      makeIndexedTriangleClassFilterList(
          util::getenvString("DXMT9_SPLIT_LARGE_INDEXED_DRAWS_CLASSES"));
  return filters;
}

RenderEncoderSelector splitLargeIndexedDrawRow() {
  static const RenderEncoderSelector selector =
      parseRenderEncoderSelector("DXMT9_SPLIT_LARGE_INDEXED_DRAWS_ROW");
  return selector;
}

RenderEncoderSelectorList splitLargeIndexedDrawRows() {
  static const RenderEncoderSelectorList selectors =
      makeRenderEncoderSelectorList(
          util::getenvString("DXMT9_SPLIT_LARGE_INDEXED_DRAWS_ROWS"));
  return selectors;
}

bool probeReverseIndexedTriangles() {
  static const bool v = util::getenvFlag("DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES");
  return v;
}

bool probeReverseOpaqueIndexedTriangles() {
  static const bool v = util::getenvFlag("DXMT9_PROBE_REVERSE_OPAQUE_INDEXED_TRIANGLES");
  return v;
}

bool probeReverseNonOpaqueIndexedTriangles() {
  static const bool v =
      util::getenvFlag("DXMT9_PROBE_REVERSE_NONOPAQUE_INDEXED_TRIANGLES");
  return v;
}

IndexedTriangleClassFilter probeReverseIndexedTrianglesClassFilter() {
  static const IndexedTriangleClassFilter filter =
      makeIndexedTriangleClassFilter(
          util::getenvString("DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_CLASS"));
  return filter;
}

IndexedTriangleClassFilterList probeReverseIndexedTrianglesClassFilters() {
  static const IndexedTriangleClassFilterList filters =
      makeIndexedTriangleClassFilterList(
          util::getenvString("DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_CLASSES"));
  return filters;
}

RenderEncoderSelector probeReverseIndexedTrianglesRow() {
  static const RenderEncoderSelector selector =
      parseRenderEncoderSelector("DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_ROW");
  return selector;
}

RenderEncoderSelectorList probeReverseIndexedTrianglesRows() {
  static const RenderEncoderSelectorList selectors =
      makeRenderEncoderSelectorList(
          util::getenvString("DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_ROWS"));
  return selectors;
}

bool measureIndexReuse() {
  static const bool v = util::getenvFlag("DXMT9_MEASURE_INDEX_REUSE");
  return v;
}

int fixedFunctionTraceBudget() {
  static const int budget = [] {
    const auto env = util::getenvString("DXMT_TRACE_FVF");
    if (env.empty()) {
      return 0;
    }
    return std::max(0, std::atoi(env.c_str()));
  }();
  return budget;
}

u64 fixedFunctionTraceTextureHandle() {
  static const u64 value = [] {
    const char* env = std::getenv("DXMT_TRACE_FVF_TEX0");
    if (!env || env[0] == '\0') {
      return 0ull;
    }
    return static_cast<u64>(std::strtoull(env, nullptr, 0));
  }();
  return value;
}

u64 traceEncodeSeq() {
  static const u64 value = [] {
    const char* env = std::getenv("DXMT_TRACE_ENCODE_SEQ");
    if (!env || env[0] == '\0') {
      return 0ull;
    }
    return static_cast<u64>(std::strtoull(env, nullptr, 0));
  }();
  return value;
}

u64 traceTextureHandle() {
  static const u64 handle = [] {
    const char* env = std::getenv("DXMT_TRACE_TEXTURE_HANDLE");
    if (!env || env[0] == '\0') {
      return 0ull;
    }
    return static_cast<u64>(std::strtoull(env, nullptr, 0));
  }();
  return handle;
}

u64 skippedTextureHandle() {
  static const u64 value = [] {
    const char* env = std::getenv("DXMT_SKIP_TEXTURE_HANDLE");
    if (!env || env[0] == '\0') {
      return 0ull;
    }
    char* end = nullptr;
    const auto parsed = std::strtoull(env, &end, 0);
    if (end == env) {
      return 0ull;
    }
    return static_cast<u64>(parsed);
  }();
  return value;
}

bool shouldTraceEncode(const core::FlatDrawStateRecord& hot, u64 seqId) {
  const u64 seq = traceEncodeSeq();
  if (seq != 0ull && seqId == seq) {
    return true;
  }
  const u64 wanted = traceTextureHandle();
  if (wanted != 0ull && hot.textures[0] && hot.textures[0].value == wanted) {
    return true;
  }
  const u64 ffWanted = fixedFunctionTraceTextureHandle();
  return ffWanted != 0ull && hot.textures[0] && hot.textures[0].value == ffWanted;
}

bool shouldTraceTexture(core::Handle handle) {
  const u64 wanted = traceTextureHandle();
  return wanted != 0ull && handle.value == wanted;
}

}  // namespace dxmt9::debug
