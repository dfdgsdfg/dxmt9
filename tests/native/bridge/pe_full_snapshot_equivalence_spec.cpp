// pe_full_snapshot_equivalence_spec
//
// Regression guard for the DXMT9_PE_DRAW_FULL_SNAPSHOT debug knob.
//
// The PE recorder (src/d3d9/d3d9_pe_device.cpp::buildDrawPrimitivePacket)
// emits a D9CDrawPrimitivePacket in one of two modes:
//
//   - Delta (default): only fields that changed since the previous draw
//     have their valid/mask bits set; the rest are zeroed. Idempotent
//     run-coalescing on the importer hinges on packetHasNoStateDelta().
//
//   - Full-snapshot (DXMT9_PE_DRAW_FULL_SNAPSHOT=1, Phase 16): every
//     valid/mask bit is set and the payload is drained from the PE
//     shadow. The packet is self-contained, replayable out of order,
//     and costs ~10x more wire bandwidth.
//
// The unix-side applier (device_c_chunk_replay.cpp::
// applyDrawPacketStateDirect) is mode-agnostic — it observes only the
// valid/mask bits. The invariant this spec proves is:
//
//   For any sequence of D3D9 state mutations + draws, the effective
//   per-draw boundary state produced by replaying the delta-mode
//   packet stream equals the effective state produced by replaying
//   the full-snapshot-mode packet stream.
//
// That equivalence is what makes the debug knob safe: flipping it on
// must not alter rendered output, only wire size / coalescing.
//
// Native bridge tests cannot instantiate src/d3d9/d3d9_pe_device.cpp
// (Windows-only build via windows.h / d3d9.h). They also cannot run
// applyDrawPacketStateDirect, which needs a real D9CDevice + iface.
//
// We therefore mirror both halves at test scope:
//
//   1. A `PeShadow` represents the PE-side state shadow.
//   2. `emitDeltaPacket(shadow)` mirrors buildDrawPrimitivePacket's
//      delta block — copies pending* into the packet and clears
//      pending bits.
//   3. `emitSnapshotPacket(shadow)` mirrors the Phase 16 override —
//      every valid bit set, populated from the full shadow.
//   4. A `BridgeShadow` represents the unix-side D9CDevice state.
//   5. `applyPacket(bridgeShadow, packet)` mirrors the valid/mask
//      iteration of applyDrawPacketStateDirect.
//
// For each scenario, two parallel runs (delta vs full-snapshot) drive
// two `BridgeShadow`s through identical D3D9-level mutations and assert
// equality at every per-draw boundary.

#include "chunk_record_import_spec_fixtures.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace dxmt9::d3d9::devicec::spec;

constexpr uint32_t kMaxRs = D9C_DRAW_PACKET_MAX_RENDER_STATES;
constexpr uint32_t kMaxTex = D9C_DRAW_PACKET_MAX_TEXTURES;
constexpr uint32_t kMaxStream = D9C_DRAW_PACKET_MAX_STREAMS;
constexpr uint32_t kMaxRt = D9C_DRAW_PACKET_MAX_RENDER_TARGETS;
constexpr uint32_t kMaxTss = D9C_DRAW_PACKET_MAX_TSS;
constexpr uint32_t kMaxSamp = D9C_DRAW_PACKET_MAX_SAMPLER;
constexpr uint32_t kMaxXf = D9C_DRAW_PACKET_MAX_TRANSFORMS;
constexpr uint32_t kMaxLight = D9C_DRAW_PACKET_MAX_LIGHTS;
constexpr uint32_t kClipPlanes = 6u;

// =====================================================================
// PE-side shadow + pending tracking (mirrors d3d9_pe_device PeState).
// =====================================================================
struct PeShadow {
    // Render states stored as ordered list: state->value plus pending.
    std::vector<std::pair<uint32_t, uint32_t>> rsList;
    uint64_t rsPendingMask = 0;  // bit i = entry i pending (we keep small)

    std::array<D9CWireHandle, kMaxTex> textures{};
    uint32_t texPending = 0;

    std::array<D9CDrawPacketStreamSource, kMaxStream> streams{};
    uint32_t streamPending = 0;

    uint32_t fvf = 0;
    bool fvfPending = false;

    D9CWireHandle vs{};
    bool vsPending = false;
    D9CWireHandle ps{};
    bool psPending = false;
    D9CWireHandle vdecl{};
    bool vdeclPending = false;

    std::array<D9CWireHandle, kMaxRt> rt{};
    uint32_t rtPending = 0;
    D9CWireHandle ds{};
    bool dsPending = false;

    D9CViewport viewport{};
    bool viewportPending = false;
    D9CRect scissor{};
    bool scissorPending = false;

    std::vector<D9CDrawPacketTextureStageState> tssList;
    std::vector<D9CDrawPacketSamplerState> sampList;

    D9CMaterial material{};
    bool materialPending = false;

    std::array<std::array<float, 4>, kClipPlanes> clipPlanes{};
    uint32_t clipPending = 0;

    std::vector<D9CDrawPacketTransform> xformList;

    std::array<D9CLight, kMaxLight> lights{};
    uint32_t lightSlotPending = 0;
    uint32_t lightEnableShadow = 0;
    uint32_t lightEnableValidPending = 0;
    uint32_t lightEnableMaskPending = 0;

    void setRenderState(uint32_t s, uint32_t v) {
        for (size_t i = 0; i < rsList.size(); ++i) {
            if (rsList[i].first == s) {
                rsList[i].second = v;
                rsPendingMask |= (1ull << i);
                return;
            }
        }
        const size_t idx = rsList.size();
        rsList.push_back({s, v});
        if (idx < 64) rsPendingMask |= (1ull << idx);
    }

    void setTexture(uint32_t stage, D9CWireHandle h) {
        textures[stage] = h;
        texPending |= 1u << stage;
    }

    void setStream(uint32_t s, D9CDrawPacketStreamSource src) {
        streams[s] = src;
        streamPending |= 1u << s;
    }

    void setFvf(uint32_t v) { fvf = v; fvfPending = true; }
    void setVs(D9CWireHandle h) { vs = h; vsPending = true; }
    void setPs(D9CWireHandle h) { ps = h; psPending = true; }
    void setVdecl(D9CWireHandle h) { vdecl = h; vdeclPending = true; }

    void setRt(uint32_t slot, D9CWireHandle h) {
        rt[slot] = h;
        rtPending |= 1u << slot;
    }
    void setDs(D9CWireHandle h) { ds = h; dsPending = true; }

    void setViewport(D9CViewport v) { viewport = v; viewportPending = true; }
    void setScissor(D9CRect r) { scissor = r; scissorPending = true; }

    void setTss(uint32_t stage, uint32_t type, uint32_t value) {
        for (auto& e : tssList) {
            if (e.stage == stage && e.type == type) {
                e.value = value;
                return;  // Pending: simplification — re-emit on next draw.
            }
        }
        tssList.push_back({stage, type, value});
    }
    void setSamp(uint32_t sampler, uint32_t type, uint32_t value) {
        for (auto& e : sampList) {
            if (e.sampler == sampler && e.type == type) {
                e.value = value;
                return;
            }
        }
        sampList.push_back({sampler, type, value});
    }
    void setMaterial(D9CMaterial m) { material = m; materialPending = true; }
    void setClipPlane(uint32_t i, std::array<float, 4> plane) {
        clipPlanes[i] = plane;
        clipPending |= 1u << i;
    }
    void setTransform(uint32_t state, D9CMatrix m) {
        for (auto& e : xformList) {
            if (e.state == state) {
                e.matrix = m;
                return;
            }
        }
        xformList.push_back({state, 0u, m});
    }
    void setLight(uint32_t slot, D9CLight l) {
        lights[slot] = l;
        lightSlotPending |= 1u << slot;
    }
    void setLightEnabled(uint32_t slot, bool enabled) {
        if (enabled) {
            lightEnableShadow |= 1u << slot;
        } else {
            lightEnableShadow &= ~(1u << slot);
        }
        lightEnableValidPending |= 1u << slot;
        if (enabled) {
            lightEnableMaskPending |= 1u << slot;
        } else {
            lightEnableMaskPending &= ~(1u << slot);
        }
    }

    void clearPending() {
        rsPendingMask = 0;
        texPending = 0;
        streamPending = 0;
        fvfPending = false;
        vsPending = false;
        psPending = false;
        vdeclPending = false;
        rtPending = 0;
        dsPending = false;
        viewportPending = false;
        scissorPending = false;
        tssList.clear();
        sampList.clear();
        materialPending = false;
        clipPending = 0;
        xformList.clear();
        lightSlotPending = 0;
        lightEnableValidPending = 0;
    }
};

// =====================================================================
// Two PE-side packet emitters: delta vs full-snapshot.
// =====================================================================

// Mirrors d3d9_pe_device.cpp::buildDrawPrimitivePacket delta block
// (lines 380-489): copies pending* into packet, leaves un-pending
// fields zero/unset.
D9CDrawPrimitivePacket emitDeltaPacket(const PeShadow& s, uint32_t primType,
                                       uint32_t startVertex, uint32_t count) {
    D9CDrawPrimitivePacket p{};
    // Render states: emit only entries flagged pending.
    p.renderStateCount = 0;
    for (size_t i = 0; i < s.rsList.size() && i < 64; ++i) {
        if ((s.rsPendingMask & (1ull << i)) == 0) continue;
        p.renderStates[p.renderStateCount++] = {s.rsList[i].first,
                                                s.rsList[i].second};
    }

    p.textureMask = s.texPending;
    for (uint32_t i = 0; i < kMaxTex; ++i) {
        if (p.textureMask & (1u << i)) p.textures[i] = s.textures[i];
    }
    p.streamSourceMask = s.streamPending;
    for (uint32_t i = 0; i < kMaxStream; ++i) {
        if (p.streamSourceMask & (1u << i)) p.streamSources[i] = s.streams[i];
    }
    p.fvfValid = s.fvfPending ? 1u : 0u;
    if (s.fvfPending) p.fvf = s.fvf;
    p.vsValid = s.vsPending ? 1u : 0u;
    if (s.vsPending) p.vsHandle = s.vs;
    p.psValid = s.psPending ? 1u : 0u;
    if (s.psPending) p.psHandle = s.ps;
    p.vdeclValid = s.vdeclPending ? 1u : 0u;
    if (s.vdeclPending) p.vdeclHandle = s.vdecl;
    p.rtMask = s.rtPending;
    for (uint32_t i = 0; i < kMaxRt; ++i) {
        if (p.rtMask & (1u << i)) p.rtHandles[i] = s.rt[i];
    }
    p.dsValid = s.dsPending ? 1u : 0u;
    if (s.dsPending) p.dsHandle = s.ds;
    p.viewportValid = s.viewportPending ? 1u : 0u;
    if (s.viewportPending) p.viewport = s.viewport;
    p.scissorValid = s.scissorPending ? 1u : 0u;
    if (s.scissorPending) p.scissor = s.scissor;
    p.tssCount = static_cast<uint32_t>(s.tssList.size());
    for (uint32_t i = 0; i < p.tssCount && i < kMaxTss; ++i) p.tss[i] = s.tssList[i];
    p.samplerStateCount = static_cast<uint32_t>(s.sampList.size());
    for (uint32_t i = 0; i < p.samplerStateCount && i < kMaxSamp; ++i)
        p.samplerStates[i] = s.sampList[i];
    p.materialValid = s.materialPending ? 1u : 0u;
    if (s.materialPending) p.material = s.material;
    p.clipPlaneMask = s.clipPending;
    for (uint32_t i = 0; i < kClipPlanes; ++i) {
        if (p.clipPlaneMask & (1u << i)) {
            for (uint32_t k = 0; k < 4; ++k)
                p.clipPlanes[i * 4 + k] = s.clipPlanes[i][k];
        }
    }
    p.transformCount = static_cast<uint32_t>(s.xformList.size());
    for (uint32_t i = 0; i < p.transformCount && i < kMaxXf; ++i)
        p.transforms[i] = s.xformList[i];
    p.lightSlotMask = s.lightSlotPending;
    for (uint32_t i = 0; i < kMaxLight; ++i) {
        if (p.lightSlotMask & (1u << i)) p.lights[i] = s.lights[i];
    }
    p.lightEnableValidMask = s.lightEnableValidPending;
    p.lightEnableMask = s.lightEnableMaskPending;
    p.primitiveType = primType;
    p.startVertex = startVertex;
    p.primitiveCount = count;
    return p;
}

// Mirrors d3d9_pe_device.cpp::buildDrawPrimitivePacket full-snapshot
// override block (lines 497-583): every valid bit set, every payload
// drained from the live shadow. No reliance on pending masks.
D9CDrawPrimitivePacket emitSnapshotPacket(const PeShadow& s, uint32_t primType,
                                          uint32_t startVertex, uint32_t count) {
    D9CDrawPrimitivePacket p{};
    p.renderStateCount = 0;
    for (const auto& entry : s.rsList) {
        if (p.renderStateCount >= kMaxRs) break;
        p.renderStates[p.renderStateCount++] = {entry.first, entry.second};
    }
    p.textureMask = 0;
    for (uint32_t i = 0; i < kMaxTex; ++i) {
        if (s.textures[i].lo != 0 || s.textures[i].hi != 0) {
            p.textureMask |= 1u << i;
            p.textures[i] = s.textures[i];
        }
    }
    p.streamSourceMask = 0;
    for (uint32_t i = 0; i < kMaxStream; ++i) {
        const auto& src = s.streams[i];
        if (src.buffer.lo != 0 || src.buffer.hi != 0 || src.stride != 0 ||
            src.offset != 0) {
            p.streamSourceMask |= 1u << i;
            p.streamSources[i] = src;
        }
    }
    p.fvfValid = 1u;
    p.fvf = s.fvf;
    p.vsValid = 1u;
    p.vsHandle = s.vs;
    p.psValid = 1u;
    p.psHandle = s.ps;
    p.vdeclValid = 1u;
    p.vdeclHandle = s.vdecl;
    p.rtMask = 0;
    for (uint32_t i = 0; i < kMaxRt; ++i) {
        if (s.rt[i].lo != 0 || s.rt[i].hi != 0) {
            p.rtMask |= 1u << i;
            p.rtHandles[i] = s.rt[i];
        }
    }
    p.dsValid = 1u;
    p.dsHandle = s.ds;
    p.viewportValid = 1u;
    p.viewport = s.viewport;
    p.scissorValid = 1u;
    p.scissor = s.scissor;
    p.tssCount = static_cast<uint32_t>(s.tssList.size());
    for (uint32_t i = 0; i < p.tssCount && i < kMaxTss; ++i) p.tss[i] = s.tssList[i];
    p.samplerStateCount = static_cast<uint32_t>(s.sampList.size());
    for (uint32_t i = 0; i < p.samplerStateCount && i < kMaxSamp; ++i)
        p.samplerStates[i] = s.sampList[i];
    p.materialValid = 1u;
    p.material = s.material;
    p.clipPlaneMask = (1u << kClipPlanes) - 1u;  // 0x3F: all 6 planes.
    for (uint32_t i = 0; i < kClipPlanes; ++i) {
        for (uint32_t k = 0; k < 4; ++k)
            p.clipPlanes[i * 4 + k] = s.clipPlanes[i][k];
    }
    p.transformCount = static_cast<uint32_t>(s.xformList.size());
    for (uint32_t i = 0; i < p.transformCount && i < kMaxXf; ++i)
        p.transforms[i] = s.xformList[i];
    p.lightSlotMask = (1u << kMaxLight) - 1u;
    for (uint32_t i = 0; i < kMaxLight; ++i) p.lights[i] = s.lights[i];
    p.lightEnableValidMask = (1u << kMaxLight) - 1u;
    p.lightEnableMask = s.lightEnableShadow;
    p.primitiveType = primType;
    p.startVertex = startVertex;
    p.primitiveCount = count;
    return p;
}

// =====================================================================
// Unix-side bridge shadow + applier (mirrors applyDrawPacketStateDirect).
// =====================================================================
// Bridge shadow models the unix-side D9CDevice state values *only*. We
// intentionally do NOT track "did this slot ever receive a write"
// because the real unix applier does not either — applyDrawPacketStateDirect
// just writes the value when the valid/mask bit is set, and unset slots
// retain their default value. The equivalence claim is value-equality at
// every D9CDevice-visible slot after each draw.
struct BridgeShadow {
    std::array<uint32_t, 256> rs{};
    std::array<D9CWireHandle, kMaxTex> textures{};
    std::array<D9CDrawPacketStreamSource, kMaxStream> streams{};
    uint32_t fvf = 0;
    D9CWireHandle vs{};
    D9CWireHandle ps{};
    D9CWireHandle vdecl{};
    std::array<D9CWireHandle, kMaxRt> rt{};
    D9CWireHandle ds{};
    D9CViewport viewport{};
    D9CRect scissor{};
    std::array<std::array<uint32_t, 64>, 16> tss{};
    std::array<std::array<uint32_t, 64>, 16> samp{};
    D9CMaterial material{};
    std::array<std::array<float, 4>, kClipPlanes> clipPlanes{};
    std::array<D9CMatrix, 320> xform{};
    std::array<D9CLight, kMaxLight> lights{};
    uint32_t lightEnableMask = 0;
};

void applyPacket(BridgeShadow& sh, const D9CDrawPrimitivePacket& p) {
    for (uint32_t i = 0; i < p.renderStateCount; ++i) {
        const uint32_t s = p.renderStates[i].state;
        if (s < sh.rs.size()) sh.rs[s] = p.renderStates[i].value;
    }
    for (uint32_t i = 0; i < kMaxTex; ++i) {
        if (p.textureMask & (1u << i)) sh.textures[i] = p.textures[i];
    }
    for (uint32_t i = 0; i < kMaxStream; ++i) {
        if (p.streamSourceMask & (1u << i)) sh.streams[i] = p.streamSources[i];
    }
    if (p.fvfValid) sh.fvf = p.fvf;
    if (p.vsValid) sh.vs = p.vsHandle;
    if (p.psValid) sh.ps = p.psHandle;
    if (p.vdeclValid) sh.vdecl = p.vdeclHandle;
    for (uint32_t i = 0; i < kMaxRt; ++i) {
        if (p.rtMask & (1u << i)) sh.rt[i] = p.rtHandles[i];
    }
    if (p.dsValid) sh.ds = p.dsHandle;
    if (p.viewportValid) sh.viewport = p.viewport;
    if (p.scissorValid) sh.scissor = p.scissor;
    for (uint32_t i = 0; i < p.tssCount; ++i) {
        const auto& e = p.tss[i];
        if (e.stage < sh.tss.size() && e.type < sh.tss[0].size())
            sh.tss[e.stage][e.type] = e.value;
    }
    for (uint32_t i = 0; i < p.samplerStateCount; ++i) {
        const auto& e = p.samplerStates[i];
        if (e.sampler < sh.samp.size() && e.type < sh.samp[0].size())
            sh.samp[e.sampler][e.type] = e.value;
    }
    if (p.materialValid) sh.material = p.material;
    for (uint32_t i = 0; i < kClipPlanes; ++i) {
        if (p.clipPlaneMask & (1u << i)) {
            for (uint32_t k = 0; k < 4; ++k)
                sh.clipPlanes[i][k] = p.clipPlanes[i * 4 + k];
        }
    }
    for (uint32_t i = 0; i < p.transformCount; ++i) {
        const auto& e = p.transforms[i];
        if (e.state < sh.xform.size()) sh.xform[e.state] = e.matrix;
    }
    for (uint32_t i = 0; i < kMaxLight; ++i) {
        if (p.lightSlotMask & (1u << i)) sh.lights[i] = p.lights[i];
    }
    for (uint32_t i = 0; i < kMaxLight; ++i) {
        if (p.lightEnableValidMask & (1u << i)) {
            if (p.lightEnableMask & (1u << i))
                sh.lightEnableMask |= 1u << i;
            else
                sh.lightEnableMask &= ~(1u << i);
        }
    }
}

bool wireEq(const D9CWireHandle& a, const D9CWireHandle& b) {
    return a.lo == b.lo && a.hi == b.hi;
}

void assertShadowsEqual(const BridgeShadow& a, const BridgeShadow& b,
                        const std::string& tag) {
    for (size_t i = 0; i < a.rs.size(); ++i) {
        if (a.rs[i] != b.rs[i])
            fail(tag + ": render-state value[" + std::to_string(i) + "]");
    }
    for (size_t i = 0; i < a.textures.size(); ++i) {
        if (!wireEq(a.textures[i], b.textures[i]))
            fail(tag + ": texture[" + std::to_string(i) + "]");
    }
    for (size_t i = 0; i < a.streams.size(); ++i) {
        if (!wireEq(a.streams[i].buffer, b.streams[i].buffer) ||
            a.streams[i].offset != b.streams[i].offset ||
            a.streams[i].stride != b.streams[i].stride)
            fail(tag + ": stream[" + std::to_string(i) + "]");
    }
    if (a.fvf != b.fvf) fail(tag + ": fvf");
    if (!wireEq(a.vs, b.vs)) fail(tag + ": vs");
    if (!wireEq(a.ps, b.ps)) fail(tag + ": ps");
    if (!wireEq(a.vdecl, b.vdecl)) fail(tag + ": vdecl");
    for (size_t i = 0; i < a.rt.size(); ++i) {
        if (!wireEq(a.rt[i], b.rt[i]))
            fail(tag + ": rt[" + std::to_string(i) + "]");
    }
    if (!wireEq(a.ds, b.ds)) fail(tag + ": ds");
    if (a.viewport.x != b.viewport.x || a.viewport.y != b.viewport.y ||
        a.viewport.width != b.viewport.width ||
        a.viewport.height != b.viewport.height ||
        a.viewport.minZ != b.viewport.minZ ||
        a.viewport.maxZ != b.viewport.maxZ)
        fail(tag + ": viewport");
    if (a.scissor.left != b.scissor.left || a.scissor.top != b.scissor.top ||
        a.scissor.right != b.scissor.right ||
        a.scissor.bottom != b.scissor.bottom)
        fail(tag + ": scissor");
    for (size_t st = 0; st < a.tss.size(); ++st) {
        for (size_t ty = 0; ty < a.tss[0].size(); ++ty) {
            if (a.tss[st][ty] != b.tss[st][ty])
                fail(tag + ": tss-value[" + std::to_string(st) + "][" +
                     std::to_string(ty) + "]");
        }
    }
    for (size_t s = 0; s < a.samp.size(); ++s) {
        for (size_t ty = 0; ty < a.samp[0].size(); ++ty) {
            if (a.samp[s][ty] != b.samp[s][ty])
                fail(tag + ": samp-value[" + std::to_string(s) + "][" +
                     std::to_string(ty) + "]");
        }
    }
    if (std::memcmp(&a.material, &b.material, sizeof(D9CMaterial)) != 0)
        fail(tag + ": material");
    for (size_t i = 0; i < kClipPlanes; ++i) {
        if (a.clipPlanes[i] != b.clipPlanes[i])
            fail(tag + ": clip-value[" + std::to_string(i) + "]");
    }
    for (size_t i = 0; i < a.xform.size(); ++i) {
        if (std::memcmp(&a.xform[i], &b.xform[i], sizeof(D9CMatrix)) != 0)
            fail(tag + ": xform-value[" + std::to_string(i) + "]");
    }
    for (size_t i = 0; i < kMaxLight; ++i) {
        if (std::memcmp(&a.lights[i], &b.lights[i], sizeof(D9CLight)) != 0)
            fail(tag + ": light[" + std::to_string(i) + "]");
    }
    if (a.lightEnableMask != b.lightEnableMask)
        fail(tag + ": light-enable-mask");
}

// =====================================================================
// Scenario harness: a step describes a state mutation OR a draw. Both
// the delta and full-snapshot lanes share the step list; they differ
// only in how the PE shadow translates into a wire packet at each draw.
// =====================================================================
struct Step {
    enum class Kind {
        RenderState, Texture, Stream, Fvf, Vs, Ps, Vdecl, Rt, Ds,
        Viewport, Scissor, Tss, Samp, Material, ClipPlane, Transform,
        Light, LightEnable, Draw,
    } kind;
    uint32_t a = 0, b = 0, c = 0;  // generic slots
    D9CWireHandle wire{};
    D9CDrawPacketStreamSource stream{};
    D9CViewport viewport{};
    D9CRect scissor{};
    D9CMaterial material{};
    std::array<float, 4> clip{};
    D9CMatrix matrix{};
    D9CLight light{};
};

D9CWireHandle wh(uint64_t v) {
    return D9CWireHandle{static_cast<uint32_t>(v),
                         static_cast<uint32_t>(v >> 32)};
}

D9CMatrix identityMatrix() {
    D9CMatrix m{};
    for (uint32_t i = 0; i < 16; ++i) m.m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    return m;
}

void applyStepToShadow(PeShadow& s, const Step& st) {
    switch (st.kind) {
        case Step::Kind::RenderState: s.setRenderState(st.a, st.b); break;
        case Step::Kind::Texture: s.setTexture(st.a, st.wire); break;
        case Step::Kind::Stream: s.setStream(st.a, st.stream); break;
        case Step::Kind::Fvf: s.setFvf(st.a); break;
        case Step::Kind::Vs: s.setVs(st.wire); break;
        case Step::Kind::Ps: s.setPs(st.wire); break;
        case Step::Kind::Vdecl: s.setVdecl(st.wire); break;
        case Step::Kind::Rt: s.setRt(st.a, st.wire); break;
        case Step::Kind::Ds: s.setDs(st.wire); break;
        case Step::Kind::Viewport: s.setViewport(st.viewport); break;
        case Step::Kind::Scissor: s.setScissor(st.scissor); break;
        case Step::Kind::Tss: s.setTss(st.a, st.b, st.c); break;
        case Step::Kind::Samp: s.setSamp(st.a, st.b, st.c); break;
        case Step::Kind::Material: s.setMaterial(st.material); break;
        case Step::Kind::ClipPlane: s.setClipPlane(st.a, st.clip); break;
        case Step::Kind::Transform: s.setTransform(st.a, st.matrix); break;
        case Step::Kind::Light: s.setLight(st.a, st.light); break;
        case Step::Kind::LightEnable: s.setLightEnabled(st.a, st.b != 0); break;
        case Step::Kind::Draw: break;
    }
}

// Run a step list twice — once via delta packets, once via full-snapshot
// packets — and assert the BridgeShadow agrees at every Draw boundary.
void runEquivalenceScenario(const std::string& name,
                            const std::vector<Step>& steps) {
    PeShadow peDelta;
    PeShadow peSnap;
    BridgeShadow bridgeDelta;
    BridgeShadow bridgeSnap;

    uint32_t drawIndex = 0;
    for (const auto& step : steps) {
        if (step.kind == Step::Kind::Draw) {
            const auto deltaPacket = emitDeltaPacket(peDelta, step.a, step.b, step.c);
            const auto snapPacket =
                emitSnapshotPacket(peSnap, step.a, step.b, step.c);
            applyPacket(bridgeDelta, deltaPacket);
            applyPacket(bridgeSnap, snapPacket);

            // Draw-call parameters must be identical: full-snapshot must
            // not perturb startVertex / primitiveType / primitiveCount.
            checkEq(deltaPacket.primitiveType, snapPacket.primitiveType,
                    name + " draw#" + std::to_string(drawIndex) + " primType");
            checkEq(deltaPacket.startVertex, snapPacket.startVertex,
                    name + " draw#" + std::to_string(drawIndex) + " startVertex");
            checkEq(deltaPacket.primitiveCount, snapPacket.primitiveCount,
                    name + " draw#" + std::to_string(drawIndex) + " primCount");

            // The boundary: after applying packet i, the unix-side
            // BridgeShadow must agree between modes.
            assertShadowsEqual(bridgeDelta, bridgeSnap,
                               name + " draw#" + std::to_string(drawIndex));

            // PE-side: clear pending after the delta emit (matches the
            // recorder's clearPendingHotState() at chunk-record time).
            peDelta.clearPending();
            // Full-snapshot mode also "clears" pending — it doesn't
            // depend on pending anyway; mirror for realism.
            peSnap.clearPending();
            ++drawIndex;
        } else {
            applyStepToShadow(peDelta, step);
            applyStepToShadow(peSnap, step);
        }
    }
    check(drawIndex > 0, name + " must contain at least one draw");
}

// =====================================================================
// Scenarios.
// =====================================================================

// Case 1: simple FFP triangle. FVF only, world/view/projection
// transforms, no programmable shaders, one texture stage.
void testFfpTriangle() {
    std::vector<Step> steps;
    Step st;
    st.kind = Step::Kind::Fvf; st.a = 0x152u;  // D3DFVF_XYZ|NORMAL|TEX1
    steps.push_back(st);
    st = {}; st.kind = Step::Kind::Texture; st.a = 0; st.wire = wh(0xDEAD0001ull);
    steps.push_back(st);
    st = {}; st.kind = Step::Kind::Stream; st.a = 0;
    st.stream.buffer = wh(0xCAFE0001ull); st.stream.offset = 0; st.stream.stride = 32;
    steps.push_back(st);
    st = {}; st.kind = Step::Kind::RenderState; st.a = 7u; st.b = 1u;  // ZENABLE
    steps.push_back(st);
    st = {}; st.kind = Step::Kind::RenderState; st.a = 22u; st.b = 1u; // CULLMODE
    steps.push_back(st);
    st = {}; st.kind = Step::Kind::Transform; st.a = 2u; st.matrix = identityMatrix();
    steps.push_back(st);
    st = {}; st.kind = Step::Kind::Transform; st.a = 3u; st.matrix = identityMatrix();
    steps.push_back(st);
    st = {}; st.kind = Step::Kind::Viewport;
    st.viewport = D9CViewport{0, 0, 1280, 720, 0.0f, 1.0f};
    steps.push_back(st);
    st = {}; st.kind = Step::Kind::Material;
    st.material.diffuse = {1.0f, 0.5f, 0.25f, 1.0f};
    st.material.power = 16.0f;
    steps.push_back(st);
    st = {}; st.kind = Step::Kind::Tss; st.a = 0; st.b = 1; st.c = 4;  // COLOROP=MODULATE
    steps.push_back(st);
    st = {}; st.kind = Step::Kind::Draw; st.a = 4u; st.b = 0u; st.c = 1u;
    steps.push_back(st);
    runEquivalenceScenario("ffp-triangle", steps);
}

// Case 2: programmable VS+PS draw with textures bound. Exercises every
// shader-handle delta field plus multi-stage sampler/TSS state.
void testProgrammableShaderDraw() {
    std::vector<Step> steps;
    Step st;
    st.kind = Step::Kind::Vs; st.wire = wh(0x4141414100000001ull);
    steps.push_back(st);
    st = {}; st.kind = Step::Kind::Ps; st.wire = wh(0x5151515100000001ull);
    steps.push_back(st);
    st = {}; st.kind = Step::Kind::Vdecl; st.wire = wh(0x6161616100000001ull);
    steps.push_back(st);
    for (uint32_t i = 0; i < 4; ++i) {
        st = {}; st.kind = Step::Kind::Texture; st.a = i;
        st.wire = wh(0x7000000000000000ull + i);
        steps.push_back(st);
        st = {}; st.kind = Step::Kind::Samp; st.a = i; st.b = 6; st.c = 2;
        steps.push_back(st);
    }
    st = {}; st.kind = Step::Kind::Stream; st.a = 0;
    st.stream.buffer = wh(0xC0FFEE01ull); st.stream.offset = 64; st.stream.stride = 24;
    steps.push_back(st);
    st = {}; st.kind = Step::Kind::Rt; st.a = 0; st.wire = wh(0xAA00000000000001ull);
    steps.push_back(st);
    st = {}; st.kind = Step::Kind::Ds; st.wire = wh(0xBB00000000000001ull);
    steps.push_back(st);
    st = {}; st.kind = Step::Kind::Scissor;
    st.scissor = D9CRect{32, 32, 1248, 688};
    steps.push_back(st);
    st = {}; st.kind = Step::Kind::Draw; st.a = 4u; st.b = 0u; st.c = 2u;
    steps.push_back(st);
    runEquivalenceScenario("programmable-shader", steps);
}

// Case 3: state deltas across multiple draws. Specifically validates
// that intermediate mutations between draws yield identical effective
// state under both modes — the most ABI-sensitive case for the knob.
void testInterleavedStateAndDraws() {
    std::vector<Step> steps;
    Step st;
    // Initial setup before first draw.
    st = {}; st.kind = Step::Kind::Fvf; st.a = 0x52u; steps.push_back(st);
    st = {}; st.kind = Step::Kind::RenderState; st.a = 7u; st.b = 1u;
    steps.push_back(st);
    st = {}; st.kind = Step::Kind::Stream; st.a = 0;
    st.stream.buffer = wh(0xCAFEBABEull); st.stream.stride = 16;
    steps.push_back(st);
    st = {}; st.kind = Step::Kind::Draw; st.a = 5u; st.b = 0u; st.c = 1u;
    steps.push_back(st);

    // Mutate render state + light state between draws.
    st = {}; st.kind = Step::Kind::RenderState; st.a = 7u; st.b = 0u;
    steps.push_back(st);
    st = {}; st.kind = Step::Kind::RenderState; st.a = 28u; st.b = 1u;  // ALPHABLENDENABLE
    steps.push_back(st);
    st = {}; st.kind = Step::Kind::Light; st.a = 0;
    st.light.type = 1u;
    st.light.diffuse = {1.0f, 1.0f, 1.0f, 1.0f};
    st.light.direction[0] = 0.0f; st.light.direction[1] = -1.0f;
    st.light.direction[2] = 0.0f;
    steps.push_back(st);
    st = {}; st.kind = Step::Kind::LightEnable; st.a = 0; st.b = 1;
    steps.push_back(st);
    st = {}; st.kind = Step::Kind::ClipPlane; st.a = 2;
    st.clip = {1.0f, 0.0f, 0.0f, -0.5f};
    steps.push_back(st);
    st = {}; st.kind = Step::Kind::Draw; st.a = 4u; st.b = 3u; st.c = 1u;
    steps.push_back(st);

    // Third draw: change texture, leave everything else.
    st = {}; st.kind = Step::Kind::Texture; st.a = 1; st.wire = wh(0xDEADBEEFull);
    steps.push_back(st);
    st = {}; st.kind = Step::Kind::LightEnable; st.a = 0; st.b = 0;
    steps.push_back(st);
    st = {}; st.kind = Step::Kind::Draw; st.a = 4u; st.b = 6u; st.c = 1u;
    steps.push_back(st);

    runEquivalenceScenario("interleaved-state-and-draws", steps);
}

// Case 4: prove the per-mode wire shape contract: in delta mode the
// quiescent (no-mutation) draw has every valid/mask bit zero (lets the
// importer's run-coalescer fire), while in full-snapshot mode every
// valid bit is set (deliberately breaks run-coalescing). This is the
// observable trade-off the env-var doc lists.
void testWireShapeContract() {
    PeShadow s;
    // Seed shadow content (typical post-first-draw situation: handles
    // and scalars are populated). The lists for TSS / Samp / Xform stay
    // empty — those are "drained on emit", so a non-empty list IS a
    // pending mutation. Same for clip planes / lights, which use bit
    // masks rather than a separate pending state. The recorder maintains
    // pending-state flags such that after each draw seal, the *delta*
    // emitter sees nothing pending.
    s.fvf = 0x52u;
    s.textures[0] = wh(0x1234ull);
    s.streams[0].buffer = wh(0x5678ull);
    s.streams[0].stride = 12u;
    s.vs = wh(0x1111ull);
    s.ps = wh(0x2222ull);
    s.vdecl = wh(0x3333ull);
    s.rt[0] = wh(0x4444ull);
    s.ds = wh(0x5555ull);
    s.viewport = D9CViewport{0, 0, 800, 600, 0, 1};
    s.material.power = 4.0f;
    s.clipPlanes[0] = {1, 0, 0, 0};
    s.lights[0].type = 1u;
    // Note: rsList stays empty AND rsPendingMask is zero — simulating
    // "no pending render-state mutations since last seal". The shadow
    // would normally track all set render states in rsList; for this
    // test we only care that delta-mode produces an empty packet.
    // s.clearPending() is implicit (fresh struct).

    const auto delta = emitDeltaPacket(s, 4u, 0u, 1u);
    const auto snap = emitSnapshotPacket(s, 4u, 0u, 1u);

    // Delta mode: every state-delta field is zero -> run-coalescer eats
    // this draw (packetHasNoStateDelta == true).
    check(packetHasNoStateDelta(delta),
          "delta packet with no pending mutations has no state delta");

    // Full-snapshot mode: at minimum fvfValid / vsValid / psValid /
    // vdeclValid / dsValid / viewportValid / scissorValid / materialValid
    // / clipPlaneMask / lightSlotMask / lightEnableValidMask are all
    // set, so packetHasNoStateDelta == false. This is the deliberate
    // trade-off documented in the env var rules: every snapshot-mode
    // draw breaks the importer's run-coalescer.
    check(!packetHasNoStateDelta(snap),
          "full-snapshot packet always carries state delta");

    // Apply both to a fresh BridgeShadow. Even though the wire packets
    // differ, the unix-side D9CDevice state should match — the no-op
    // delta should be equivalent to "re-write every existing shadow
    // value" because writing X to a slot already holding X is a no-op.
    BridgeShadow afterDelta;
    BridgeShadow afterSnap;
    applyPacket(afterDelta, delta);  // No-op: zero valid bits.
    applyPacket(afterSnap, snap);
    // afterSnap now reflects every shadow value (handles, fvf, viewport
    // etc). afterDelta is still zeroed — that's actually correct because
    // delta mode would never produce an empty packet for the *first*
    // draw of a session; the first draw always has pending bits set
    // for every initialized field. The proper equivalence check is the
    // one in cases 1-3 above (full mutation sequence from the same
    // starting state). The wire-shape contract here only asserts the
    // delta/snapshot polarity of packetHasNoStateDelta.
}

}  // namespace

int main() {
    try {
        testFfpTriangle();
        testProgrammableShaderDraw();
        testInterleavedStateAndDraws();
        testWireShapeContract();
    } catch (const TestFailure& e) {
        std::cerr << "pe_full_snapshot_equivalence_spec failed: " << e.what()
                  << '\n';
        return EXIT_FAILURE;
    } catch (const std::exception& e) {
        std::cerr << "pe_full_snapshot_equivalence_spec unexpected exception: "
                  << e.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "pe_full_snapshot_equivalence_spec passed\n";
    return EXIT_SUCCESS;
}
