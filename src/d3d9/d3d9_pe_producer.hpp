#pragma once

// The PE draw-packet / sparse-state producer.
//
// buildDrawPacketFromViews is d3d9_pe_device.cpp's former
// buildDrawPrimitivePacket, rehosted here against explicit POD inputs so it
// compiles and runs on a native host. Its body is unchanged: member accesses
// became parameter accesses, and the COM-to-wire translation the device used to
// do inline now happens when the device fills PeBindingView.
//
// It is a pure read of {shadow, bindings} apart from `stats`, which the
// DXMT9_PE_STATS_DECIMATION instrumentation accumulates into. It does NOT clear
// pending bits -- callers do that on success.
//
// Why this is a separate translation unit: d3d9_pe_device.cpp includes
// windows.h and d3d9.h, so no meson test can compile it. Moving the producer
// out is what lets a native differential test call the real function instead of
// mirroring it, which is the whole gate for the later rewrite. See
// docs/superpowers/specs/2026-07-29-pe-legacy-record-removal-design.md §4.

#include "d3d9_pe_decimated_scope.hpp"
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
bool buildDrawPacketFromViews(const PeHotStateShadow& shadow,
                              const PeBindingView& bindings,
                              std::uint32_t primitiveType,
                              std::uint32_t startVertex,
                              std::uint32_t primitiveCount,
                              bool forceFullSnapshot,
                              PeDecimatedScopeStats& stats,
                              D9CDrawPrimitivePacket& packet) noexcept;

}  // namespace dxmt9::d3d9::pe
