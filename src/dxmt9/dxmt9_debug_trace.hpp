#pragma once

// Env-gated debug knobs shared by encoder translation units. Previously
// file-local to backend_metal.mm's anonymous namespace; exposed here so
// dxmt9_draw_encoder.mm can reach them without duplication.

#include "dxmt9/core.hpp"

#include <cstdint>

namespace dxmt9::debug {

using u64 = std::uint64_t;

// Force blend-disable + writeMask=0xf to make all draws visible, for
// rendering-bisect work. Env: DXMT_DEBUG_FORCE_VISIBLE.
bool forceVisibleDraw();

// Skip recording any draw command. Env: DXMT_SKIP_ALL_DRAWS.
bool skipAllDraws();

// Disable scissor rect — useful when debugging clipping issues.
// Env: DXMT_DISABLE_SCISSOR.
bool disableScissor();

// Disable alpha-test programmatic emulation. Env: DXMT_DISABLE_ALPHA_TEST.
bool disableAlphaTest();

// When set, force indexed draws to be expanded into flat vertex lists.
// Env: DXMT_FORCE_EXPAND_INDEXED.
bool forceExpandIndexed();

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

// Returns true if `draw` should be traced (by seqId / tex0 match).
bool shouldTraceEncode(const core::DrawDesc& draw, u64 seqId);

// Returns true if `handle` matches traceTextureHandle.
bool shouldTraceTexture(core::Handle handle);

}  // namespace dxmt9::debug
