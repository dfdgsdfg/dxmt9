#include "dxmt9_debug_trace.hpp"
#include "../util/config/config.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>

namespace dxmt9::debug {

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

bool probeDisableAlphaBlend() {
  static const bool v = util::getenvFlag("DXMT9_PROBE_DISABLE_ALPHA_BLEND");
  return v;
}

bool probeDisableDepthWrite() {
  static const bool v = util::getenvFlag("DXMT9_PROBE_DISABLE_DEPTH_WRITE");
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
