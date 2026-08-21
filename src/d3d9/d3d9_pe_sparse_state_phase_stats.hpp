#pragma once

// DXMT9_PE_STATS_DECIMATION sub-phase accumulators for buildSparseState()
// (src/d3d9/d3d9_pe_producer.cpp).
//
// buildSparseState is the callee timed by the existing `draw_packet`
// decimated scope (peDrawPacketDecimatedStats_, phase offset 2, armed in
// D9CDevice::buildSparseStateForRecord in d3d9_pe_device.cpp). That scope
// says the callee costs ~0.23ms/present on GT2 but not which of its section
// walks owns that cost. These six accumulators split it into sequential
// phases that sum to the function body by construction.
//
// Same function-local-static-in-a-shared-header shape as
// peConstSetterDecimatedStats() in d3d9_pe_const_shadow.hpp: buildSparseState
// lives in d3d9_pe_producer.cpp (native-buildable, no windows.h/d3d9.h) while
// the aggregate log line is emitted from D9CDevice::logPeStatsDecimation() in
// d3d9_pe_device.cpp (Windows-only TU). A shared inline accessor function
// gives both translation units the same single process-wide instance via the
// ODR guarantee for inline functions, with no device-class member and no new
// cross-TU storage to define.
//
// These are PHASE timers (DxmtPeDecimatedPhaseTimer in
// d3d9_pe_decimated_scope.hpp), not independently-armed scopes: they only run
// when the caller's `draw_packet` scope was already sampled for this call, so
// they are directly comparable to each other and to the parent total, and pay
// only one clock pair each rather than a whole independently-sampled
// instrument. That is why none of them need a distinct phaseOffset -- the
// phase-offset collision hazard documented at the top of
// d3d9_pe_stats_decimation.hpp applies to scopes that decide FOR THEMSELVES
// whether to sample; these never do.
#include "d3d9_pe_stats_decimation.hpp"

// Phase 1: render states.
inline PeDecimatedScopeStats& peSparsePhaseRenderStatesDecimatedStats() {
  static PeDecimatedScopeStats stats{};
  return stats;
}

// Phase 2: textures + streams.
inline PeDecimatedScopeStats& peSparsePhaseTexturesStreamsDecimatedStats() {
  static PeDecimatedScopeStats stats{};
  return stats;
}

// Phase 3: shaders + vertex input + index buffer.
inline PeDecimatedScopeStats&
peSparsePhaseShadersVertexIndexDecimatedStats() {
  static PeDecimatedScopeStats stats{};
  return stats;
}

// Phase 4: attachments (render targets, depth/stencil) + scalar sections
// (viewport, scissor, material).
inline PeDecimatedScopeStats&
peSparsePhaseAttachmentsScalarsDecimatedStats() {
  static PeDecimatedScopeStats stats{};
  return stats;
}

// Phase 5: clip planes + TSS/sampler/transform tables + lights.
inline PeDecimatedScopeStats& peSparsePhaseClipTssLightsDecimatedStats() {
  static PeDecimatedScopeStats stats{};
  return stats;
}

// Phase 6: constants (the DXMT9_PE_INLINE_CONST_DELTA drain).
inline PeDecimatedScopeStats& peSparsePhaseConstantsDecimatedStats() {
  static PeDecimatedScopeStats stats{};
  return stats;
}

// Remainder: everything the six named phases above do not cover -- the
// unusable-ref validation pass, UP payload assignment, and the per-record-type
// draw header fill. Kept separate instead of folded into phase 6 so the named
// phases stay a clean one-section-class-per-phase partition; this bucket is
// expected to be small and is here only so the six named phases plus this one
// sum to the whole timed body with no gap at the measured level.
inline PeDecimatedScopeStats& peSparsePhaseRemainderDecimatedStats() {
  static PeDecimatedScopeStats stats{};
  return stats;
}
