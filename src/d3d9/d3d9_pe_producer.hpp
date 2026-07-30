#pragma once

// The PE draw-packet / sparse-state producer.
//
// buildDrawPacketFromViews is d3d9_pe_device.cpp's former
// buildDrawPrimitivePacket, rehosted here against explicit POD inputs so it
// compiles and runs on a native host. Its body is unchanged: member accesses
// became parameter accesses, and the COM-to-wire translation the device used to
// do inline now happens when the device fills PeBindingView.
//
// It is a pure read of {shadow, bindings}: no instrumentation, no side effects,
// and it does NOT clear pending bits -- callers do that on success. The
// DXMT9_PE_STATS_DECIMATION `draw_packet` scope stays on the device side, in
// the forwarder, because that scope historically covered the COM-to-wire
// binding translation too. Timing only this function would quietly shrink the
// measured figure while the real work moved outside it.
//
// Being side-effect-free also means the differential test can call it without
// controlling for instrumentation state.
//
// Why this is a separate translation unit: d3d9_pe_device.cpp includes
// windows.h and d3d9.h, so no meson test can compile it. Moving the producer
// out is what lets a native differential test call the real function instead of
// mirroring it, which is the whole gate for the later rewrite. See
// docs/superpowers/specs/2026-07-29-pe-legacy-record-removal-design.md §4.

#include "d3d9_pe_producer_views.hpp"
#include "d3d9_pe_state_shadow.hpp"
#include "dxmt9/device_c.h"
#include "util/config/config.hpp"

#include <cstdint>

namespace dxmt9::d3d9::pe {

// DXMT9_PE_DRAW_FULL_SNAPSHOT: emit a self-contained packet with every valid
// bit set, drained from the full shadow, instead of a delta. Costs ~10x wire
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
// Fills SparseStateV2Input directly from the shadows and the binding view, with
// no fat packet in between.
//
// NOTE: no PeChunkContext. Destination-chunk re-emission is a draw-site step
// applied after this function -- see the header comment in
// d3d9_pe_producer_views.hpp.
// `constants` is currently UNREAD: this producer emits no constant sections, so
// callers must flush them as standalone SET_CONST records first. It is in the
// signature because the draw sites can skip that flush under
// DXMT9_PE_INLINE_CONST_DELTA=1 and fold the ranges into the record instead --
// whoever migrates them owns that decision. Non-const for the same reason.
bool buildSparseStateV2(const PeHotStateShadow& shadow,
                        PeConstShadowBlock& constants,
                        const PeBindingView& bindings,
                        const PeDrawPayloads& payloads,
                        const PeDrawParams& params,
                        bool forceFullSnapshot,
                        bool inlineConstDelta,
                        PeSparseScratch& scratch,
                        D9CCommandChunkWireDrawHeaderV2& header,
                        SparseStateV2Input& out) noexcept;

// Applies destination-chunk re-emission on top of a buildSparseStateV2 result.
// Draw sites only: the barrier path never calls this, matching production.
//
// It REBUILDS the stream and index spans rather than appending to them. The
// emitted set is the union of "dirty" and "bound but not retained by this
// chunk", and V2 sections must be in strictly ascending slot order
// (appendSparseRecordV2's orderedSlot); merging two independently-ordered
// subsets is easy to get wrong, so one ascending pass over the union is the
// whole point.
//
// Must be called INSIDE the append emitter, after any CapacityPre flush has
// resealed the chunk -- the retention answers are about the DESTINATION chunk.
// `bindings.streams` must be authoritative for every slot, not just pending
// ones; see populateBindingView's allStreams parameter.
// forceFullSnapshot must be the SAME value the paired buildSparseStateV2 call
// received. Under snapshot that call emits all 16 stream sections, including null
// unbinds, and this function must then leave them alone: legacy's
// populateDrawPacketStreamDependencies only ever ADDED mask bits, so an all-ones
// snapshot mask survived the dependency checkpoint untouched. Rebuilding the span
// here would drop every bound-but-retained-and-clean slot and every null unbind,
// breaking the self-contained-record contract DXMT9_PE_DRAW_FULL_SNAPSHOT exists
// to provide.
bool addChunkContextSections(const PeChunkContext& chunk,
                             const PeHotStateShadow& shadow,
                             const PeBindingView& bindings,
                             const PeDrawParams& params,
                             bool forceFullSnapshot,
                             PeSparseScratch& scratch,
                             SparseStateV2Input& out) noexcept;

bool buildDrawPacketFromViews(const PeHotStateShadow& shadow,
                const PeBindingView& bindings,
                std::uint32_t primitiveType,
                std::uint32_t startVertex,
                std::uint32_t primitiveCount,
                bool forceFullSnapshot,
                D9CDrawPrimitivePacket& packet) noexcept;

}  // namespace dxmt9::d3d9::pe
