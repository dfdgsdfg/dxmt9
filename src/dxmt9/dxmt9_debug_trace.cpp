#include "dxmt9_debug_trace.hpp"
#include "../util/config/config.hpp"

#include <algorithm>
#include <cstdlib>

namespace dxmt9::debug {

bool forceVisibleDraw() {
  static const bool v = util::getenvFlag("DXMT_DEBUG_FORCE_VISIBLE");
  return v;
}

bool skipAllDraws() {
  static const bool v = util::getenvFlag("DXMT_SKIP_ALL_DRAWS");
  return v;
}

bool disableScissor() {
  static const bool v = util::getenvFlag("DXMT_DISABLE_SCISSOR");
  return v;
}

bool disableAlphaTest() {
  static const bool v = util::getenvFlag("DXMT_DISABLE_ALPHA_TEST");
  return v;
}

bool forceExpandIndexed() {
  static const bool v = util::getenvFlag("DXMT_FORCE_EXPAND_INDEXED");
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

bool shouldTraceEncode(const core::DrawDesc& draw, u64 seqId) {
  const u64 seq = traceEncodeSeq();
  if (seq != 0ull && seqId == seq) {
    return true;
  }
  const u64 wanted = traceTextureHandle();
  if (wanted != 0ull && draw.textures[0].handle && draw.textures[0].handle.value == wanted) {
    return true;
  }
  const u64 ffWanted = fixedFunctionTraceTextureHandle();
  return ffWanted != 0ull && draw.textures[0].handle && draw.textures[0].handle.value == ffWanted;
}

bool shouldTraceTexture(core::Handle handle) {
  const u64 wanted = traceTextureHandle();
  return wanted != 0ull && handle.value == wanted;
}

}  // namespace dxmt9::debug
