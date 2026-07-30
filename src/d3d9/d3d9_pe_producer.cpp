#include "d3d9_pe_producer.hpp"

#include "d3d9_pe_draw_packet.hpp"
#include "d3d9_pe_wire_handle.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dxmt9::d3d9::pe {

bool buildDrawPacketFromViews(const PeHotStateShadow& shadow,
                              const PeBindingView& bindings,
                              std::uint32_t primitiveType,
                              std::uint32_t startVertex,
                              std::uint32_t primitiveCount,
                              bool forceFullSnapshot,
                              D9CDrawPrimitivePacket& packet) noexcept {
    // populateDrawPacketAttachment{Delta,Snapshot} take PeRtWireHandles
    // (std::array<D9CWireHandle, 4>); the view carries PeWireObjectRef, so
    // build the wire form once here. This is the only shape conversion the
    // rehost needed -- everything else was a member-to-parameter rename.
    PeRtWireHandles rtWire{};
    for (std::size_t slot = 0; slot < rtWire.size(); ++slot) {
        rtWire[slot] = toWireHandle(bindings.renderTargets[slot].object);
    }
    if (shadow.pendingRenderStates.size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
        return false;
    }

    packet = D9CDrawPrimitivePacket{};
    shadow.pendingRenderStates.forEach([&](uint32_t state, uint32_t value) {
        auto& entry = packet.renderStates[packet.renderStateCount++];
        entry.state = state;
        entry.value = value;
    });

    packet.textureMask = shadow.pendingTextureMask;
    for (std::uint32_t stage = 0; stage < D9C_DRAW_PACKET_MAX_TEXTURES; ++stage) {
        if ((shadow.pendingTextureMask & (1u << stage)) != 0) {
            packet.textures[stage] = toWireHandle(bindings.textures[stage].object);
        }
    }

    packet.streamSourceMask = shadow.pendingStreamMask;
    for (std::uint32_t stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
        if ((shadow.pendingStreamMask & (1u << stream)) == 0) {
            continue;
        }
        auto& source = packet.streamSources[stream];
        source.buffer = toWireHandle(bindings.streams[stream].buffer.object);
        source.offset = bindings.streams[stream].offset;
        source.stride = bindings.streams[stream].stride;
    }

    packet.fvfValid = shadow.pendingFvf ? 1u : 0u;
    packet.fvf = bindings.fvf;
    // Phase 12: shader-handle delta. Server-side applyDrawPacketState
    // dispatches dxmt9c_device_set_vertex_shader / set_pixel_shader
    // when valid=1, mirroring the renderState/texture/stream pattern.
    packet.vsValid = shadow.pendingVs ? 1u : 0u;
    packet.vsHandle = toWireHandle(bindings.vs.object);
    packet.psValid = shadow.pendingPs ? 1u : 0u;
    packet.psHandle = toWireHandle(bindings.ps.object);
    packet.vdeclValid = shadow.pendingVdecl ? 1u : 0u;
    packet.vdeclHandle = toWireHandle(bindings.vdecl.object);
    // RT / DS delta — emit a handle for every pending bit. A set bit
    // with a zero wire handle is a deliberate detach.
    dxmt9::d3d9::pe::populateDrawPacketAttachmentDelta(
        packet, shadow.pendingRtMask, rtWire,
        shadow.pendingDs, toWireHandle(bindings.depthStencil.object));
    packet.viewportValid = shadow.pendingViewport ? 1u : 0u;
    packet.viewport = shadow.viewportShadow;
    packet.scissorValid = shadow.pendingScissor ? 1u : 0u;
    packet.scissor = shadow.scissorShadow;
    // Phase 12: drain TSS / SamplerState pending tables into packet
    // delta arrays. The cap check inside Set* already flushes the
    // chunk if a single Set would push beyond the per-packet limit;
    // here we just emit what's pending.
    if (shadow.pendingTss.size() > D9C_DRAW_PACKET_MAX_TSS ||
        shadow.pendingSamplerStates.size() > D9C_DRAW_PACKET_MAX_SAMPLER) {
        return false;
    }
    packet.tssCount = static_cast<uint32_t>(shadow.pendingTss.size());
    uint32_t tssIdx = 0;
    shadow.pendingTss.forEach([&](uint32_t stage, uint32_t state, uint32_t value) {
        packet.tss[tssIdx].stage = stage;
        packet.tss[tssIdx].type = state;
        packet.tss[tssIdx].value = value;
        ++tssIdx;
    });
    packet.samplerStateCount = static_cast<uint32_t>(shadow.pendingSamplerStates.size());
    uint32_t ssIdx = 0;
    shadow.pendingSamplerStates.forEach([&](uint32_t sampler, uint32_t state, uint32_t value) {
        packet.samplerStates[ssIdx].sampler = sampler;
        packet.samplerStates[ssIdx].type = state;
        packet.samplerStates[ssIdx].value = value;
        ++ssIdx;
    });
    // Phase 12: material + clip-plane deltas. Material rides as a
    // single struct + valid flag; clip planes ride as a 6-bit mask
    // + flat 6×4 float array (only set bits' slots are
    // semantically meaningful, but the array is fixed-size so the
    // packet layout stays simple).
    packet.materialValid = shadow.pendingMaterial ? 1u : 0u;
    packet.material = shadow.materialShadow;
    packet.clipPlaneMask = shadow.pendingClipPlaneMask;
    std::memcpy(packet.clipPlanes, shadow.clipPlaneShadow, sizeof(packet.clipPlanes));
    // Phase 12: Transform delta — drain pending transform table
    // (per-frame typically a handful: View, Projection, a few
    // World/Texture transforms). Cap check: > MAX_TRANSFORMS forces
    // chunk seal upstream.
    if (shadow.pendingTransforms.size() > D9C_DRAW_PACKET_MAX_TRANSFORMS) {
        return false;
    }
    packet.transformCount = static_cast<uint32_t>(shadow.pendingTransforms.size());
    uint32_t txIdx = 0;
    shadow.pendingTransforms.forEach([&](uint32_t state, const D9CMatrix& matrix) {
        packet.transforms[txIdx].state = state;
        packet.transforms[txIdx].reserved = 0;
        packet.transforms[txIdx].matrix = matrix;
        ++txIdx;
    });
    // Phase 12: Light + LightEnable deltas. Light slot mask carries
    // the per-slot full D9CLight payload (set bit ⇒ lights[slot] is
    // semantically meaningful). LightEnable delta is two parallel
    // masks: ValidMask says "this slot has a fresh enable" and
    // LightEnableMask carries the new value.
    packet.lightSlotMask = shadow.pendingLightSlotMask;
    for (uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_LIGHTS; ++slot) {
        if ((shadow.pendingLightSlotMask & (1u << slot)) != 0) {
            packet.lights[slot] = shadow.lightShadow[slot];
        }
    }
    packet.lightEnableValidMask = shadow.pendingLightEnableValidMask;
    packet.lightEnableMask = shadow.pendingLightEnableMask;
    // Phase 16: full-snapshot mode — override every delta field with
    // the complete shadow snapshot. The importer applies whatever
    // valid bits are set, so flipping every bit + populating from
    // the existing PE shadow gives a self-contained packet without
    // requiring any importer changes. We respect the per-array caps;
    // a shadow that overflows (e.g. > 64 distinct render states)
    // returns false to force the chunk to seal.
    //
    // Triggered exclusively by DXMT9_PE_DRAW_FULL_SNAPSHOT=1 (see
    // dxmt9PeFullSnapshotEnabled() above for the env-flag contract
    // and equivalence guarantee). Branch is delta-vs-snapshot only —
    // both produce a D9CDrawPrimitivePacket with the same wire layout
    // (no schema change), and the unix-side applier in
    // device_c_chunk_replay.cpp::applyDrawPacketStateDirect() applies
    // either packet by the same valid/mask iteration.
    if (forceFullSnapshot || dxmt9PeFullSnapshotEnabled()) {
        // Render states: drain the entire shadow table.
        if (shadow.renderStateShadow.size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            return false;
        }
        packet.renderStateCount = 0;
        shadow.renderStateShadow.forEach([&](uint32_t state, uint32_t value) {
            auto& entry = packet.renderStates[packet.renderStateCount++];
            entry.state = state;
            entry.value = value;
        });
        // A self-contained snapshot must also encode null unbinds. If a
        // prior server state has a texture/stream in a slot that is null
        // in this PE shadow, omitting the bit would preserve stale state.
        packet.textureMask = (1u << D9C_DRAW_PACKET_MAX_TEXTURES) - 1u;
        for (std::uint32_t stage = 0; stage < D9C_DRAW_PACKET_MAX_TEXTURES; ++stage) {
            packet.textures[stage] = toWireHandle(bindings.textures[stage].object);
        }
        packet.streamSourceMask = (1u << D9C_DRAW_PACKET_MAX_STREAMS) - 1u;
        for (std::uint32_t stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
            auto& s = packet.streamSources[stream];
            s.buffer = toWireHandle(bindings.streams[stream].buffer.object);
            s.offset = bindings.streams[stream].offset;
            s.stride = bindings.streams[stream].stride;
        }
        dxmt9::d3d9::pe::populateDrawPacketAttachmentSnapshot(
            packet, rtWire, bindings.rtExplicitMask, true,
            toWireHandle(bindings.depthStencil.object));
        // Scalar valid bits: emit shadow contents unconditionally.
        packet.fvfValid = 1u;
        packet.fvf = bindings.fvf;
        packet.vsValid = 1u;
        packet.vsHandle = toWireHandle(bindings.vs.object);
        packet.psValid = 1u;
        packet.psHandle = toWireHandle(bindings.ps.object);
        packet.vdeclValid = 1u;
        packet.vdeclHandle = toWireHandle(bindings.vdecl.object);
        packet.viewportValid = 1u;
        packet.viewport = shadow.viewportShadow;
        packet.scissorValid = 1u;
        packet.scissor = shadow.scissorShadow;
        // TSS / SamplerState — drain shadow tables fully.
        if (shadow.tssShadow.size() > D9C_DRAW_PACKET_MAX_TSS ||
            shadow.samplerStateShadow.size() > D9C_DRAW_PACKET_MAX_SAMPLER ||
            shadow.transformShadow.size() > D9C_DRAW_PACKET_MAX_TRANSFORMS) {
            return false;
        }
        packet.tssCount = 0;
        shadow.tssShadow.forEach([&](uint32_t stage, uint32_t state, uint32_t value) {
            auto& e = packet.tss[packet.tssCount++];
            e.stage = stage;
            e.type = state;
            e.value = value;
        });
        packet.samplerStateCount = 0;
        shadow.samplerStateShadow.forEach([&](uint32_t sampler, uint32_t state, uint32_t value) {
            auto& e = packet.samplerStates[packet.samplerStateCount++];
            e.sampler = sampler;
            e.type = state;
            e.value = value;
        });
        packet.materialValid = 1u;
        packet.material = shadow.materialShadow;
        // Clip planes: emit every slot with mask = 0x3F (all 6).
        packet.clipPlaneMask = 0x3Fu;
        std::memcpy(packet.clipPlanes, shadow.clipPlaneShadow,
                    sizeof(packet.clipPlanes));
        // Transforms: drain shadow.
        packet.transformCount = 0;
        shadow.transformShadow.forEach([&](uint32_t state, const D9CMatrix& matrix) {
            auto& t = packet.transforms[packet.transformCount++];
            t.state = state;
            t.reserved = 0;
            t.matrix = matrix;
        });
        // Lights: emit every slot.
        packet.lightSlotMask = (1u << D9C_DRAW_PACKET_MAX_LIGHTS) - 1u;
        for (uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_LIGHTS; ++i) {
            packet.lights[i] = shadow.lightShadow[i];
        }
        packet.lightEnableValidMask = (1u << D9C_DRAW_PACKET_MAX_LIGHTS) - 1u;
        packet.lightEnableMask = shadow.lightEnableShadow;
    }
    packet.primitiveType = primitiveType;
    packet.startVertex = startVertex;
    packet.primitiveCount = primitiveCount;
    return true;
}
}  // namespace dxmt9::d3d9::pe
