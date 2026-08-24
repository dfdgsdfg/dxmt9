#pragma once

// The PE draw-packet / sparse-state producer.
//
// buildSparseState is the PE recorder's sole state producer: it reads the
// hot-state shadow, the constant shadow, the binding view and the draw payloads,
// and fills a SparseStateInput plus the record's draw header. addChunkContextSections
// then adds the sections that depend on what the DESTINATION chunk already
// retains, which is why it is separate -- that input is not available until
// after the capacity precheck has sealed the previous chunk.
//
// buildSparseState is a pure read of its inputs, including constant dirty
// ranges when asked to fold them inline. It does NOT clear pending state;
// callers settle the exact accepted record afterwards. The
// DXMT9_PE_STATS_DECIMATION `draw_packet` scope stays on the device side, in the
// forwarder, because that scope historically covered the COM-to-wire binding
// translation too. Timing only this function would quietly shrink the measured
// figure while the real work moved outside it.
//
// Why this is a separate translation unit: d3d9_pe_device.cpp includes
// windows.h and d3d9.h, so no meson test can compile it. Moving the producer out
// is what lets native tests call the real function instead of mirroring it --
// which is how the golden corpus in pe_producer_differential_spec can be trusted.
// See docs/superpowers/specs/2026-07-29-pe-legacy-record-removal-design.md §4.

#include "d3d9_pe_producer_views.hpp"
#include "d3d9_pe_semantic_tokens.hpp"
#include "d3d9_pe_state_shadow.hpp"
#include "dxmt9/device_c.h"
#include "util/config/config.hpp"

#include <cstdint>

namespace dxmt9::d3d9::pe {

// DXMT9_PE_DRAW_FULL_SNAPSHOT: emit a self-contained packet with every valid
// bit set, read from the full shadow, instead of a delta. Costs ~10x wire
// bandwidth and defeats importer run-coalescing; debug / stress /
// out-of-order-replay only.
//
// Equivalence guarantee: applying a delta-mode packet sequence versus the
// matching full-snapshot sequence yields identical effective state. The
// regression guard is tests/native/bridge/pe_full_snapshot_equivalence_spec.cpp.
inline bool dxmt9PeFullSnapshotEnabled() {
  static const bool enabled =
    dxmt9::util::getenvFlag("DXMT9_PE_DRAW_FULL_SNAPSHOT");
  return enabled;
}

// `primitiveType` is the D3DPRIMITIVETYPE value widened to uint32_t; this TU
// cannot see the D3D9 enum. Callers cast.
// Fills SparseStateInput directly from the shadows and the binding view, with
// no fat packet in between.
//
// NOTE: no PeChunkContext. Destination-chunk re-emission is a draw-site step
// applied after this function -- see the header comment in
// d3d9_pe_producer_views.hpp.
// `constants` is read when DXMT9_PE_INLINE_CONST_DELTA=1 so draw sites can fold
// dirty ranges into the record instead of flushing standalone SET_CONST
// records. Preparation is non-consuming; acceptInlineConstantDelta settles the
// exact ranges only after the builder accepts their record.
bool buildSparseState(const PeHotStateShadow& shadow,
                        PeConstShadowBlock& constants,
                        const PeBindingView& bindings,
                        const PeDrawPayloads& payloads,
                        const PeDrawParams& params,
                        bool forceFullSnapshot,
                        bool inlineConstDelta,
                        PeSparseScratch& scratch,
                        D9CCommandChunkWireDrawHeader& header,
                        SparseStateInput& out) noexcept;

// Settles only the constant ranges prepared by buildSparseState after the
// builder has accepted their record. A failed append must skip this call.
bool acceptInlineConstantDelta(PeConstShadowBlock& constants,
                               const SparseStateInput& state,
                               const AppendPlan& plan) noexcept;

// Settles exactly the pending keys/ranges represented by `state`.  The same
// AppendPlan vocabulary is shared by draw, barrier, oversized-category,
// tail, and inline-constant append sites; prepare/fail/discard never consume.
bool acceptPreparedSparseState(PeHotStateShadow& shadow,
                               PeConstShadowBlock& constants,
                               const SparseStateInput& state,
                               const AppendPlan& plan,
                               PeScalarSemanticTokenLedger* tokens = nullptr,
                               std::uint64_t recordOrdinal = 0u) noexcept;

// Builds the value-owned checkpoint form used by Render Tape. Unlike the
// normal draw path this preserves the complete constant shadow rather than
// consuming only dirty ranges; the caller may then append the result to a
// temporary APPLY_STATE builder without touching the live command chunk.
bool buildFullSnapshotState(
    const PeHotStateShadow& shadow, PeConstShadowBlock& constants,
    const PeBindingView& bindings, PeSparseScratch& scratch,
    D9CCommandChunkWireDrawHeader& header, SparseStateInput& out) noexcept;

// Applies destination-chunk re-emission on top of a buildSparseState result.
// Draw sites only: the barrier path never calls this, matching production.
//
// It REBUILDS the stream and index spans rather than appending to them. The
// emitted set is the union of "dirty" and "bound but not retained by this
// chunk", and canonical sections must be in strictly ascending slot order
// (appendSparseRecord's orderedSlot); merging two independently-ordered
// subsets is easy to get wrong, so one ascending pass over the union is the
// whole point.
//
// Must be called INSIDE the append emitter, after any CapacityPre flush has
// resealed the chunk -- the retention answers are about the DESTINATION chunk.
// `bindings.streams` must be authoritative for every slot, not just pending
// ones; see populateBindingView's allStreams parameter.
// The paired buildSparseState call records the effective disposition in
// `out.fullSnapshot`. Under snapshot that call emits all 16 stream sections,
// including null unbinds, and this function must then leave them alone: legacy's
// populateDrawPacketStreamDependencies only ever ADDED mask bits, so an all-ones
// snapshot mask survived the dependency checkpoint untouched. Rebuilding the span
// here would drop every bound-but-retained-and-clean slot and every null unbind,
// breaking the self-contained-record contract DXMT9_PE_DRAW_FULL_SNAPSHOT exists
// to provide.
bool addChunkContextSections(const PeChunkContext& chunk,
                             const PeHotStateShadow& shadow,
                             const PeBindingView& bindings,
                             const PeDrawParams& params,
                             PeSparseScratch& scratch,
                             SparseStateInput& out) noexcept;


}  // namespace dxmt9::d3d9::pe
