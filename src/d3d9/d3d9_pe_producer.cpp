#include "d3d9_pe_producer.hpp"

#include "d3d9_pe_draw_packet.hpp"
#include "d3d9_pe_wire_handle.hpp"
#include "dxmt9/assert.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <cstring>

namespace dxmt9::d3d9::pe {

// Fills SparseStateV2Input straight from the shadows and the binding view.
//
// Section content and ORDER must match what the legacy shim's
// populateLegacySparseState produced from the fat packet, because the
// differential compares emitted chunk bytes. Every walk therefore goes slot 0
// upward, which is also what appendSparseRecordV2's orderedSlot() requires.
bool buildSparseStateV2(const PeHotStateShadow& shadow,
                        PeConstShadowBlock& constants,
                        const PeBindingView& bindings,
                        const PeDrawPayloads& payloads,
                        const PeDrawParams& params,
                        bool forceFullSnapshot,
                        PeSparseScratch& scratch,
                        D9CCommandChunkWireDrawHeaderV2& header,
                        SparseStateV2Input& out) noexcept {
  // Snapshot mode drains the whole shadow instead of the pending set, so the
  // record is self-contained and replayable out of order. Same gate the fat-
  // packet producer used.
  const bool snapshot = forceFullSnapshot || dxmt9PeFullSnapshotEnabled();
  out = SparseStateV2Input{};

  // --- render states -------------------------------------------------------
  const auto& renderStateTable =
      snapshot ? shadow.renderStateShadow : shadow.pendingRenderStates;
  if (renderStateTable.size() > scratch.renderStates.size()) {
    return false;  // over cap: seal the chunk rather than truncate
  }
  std::size_t renderStateCount = 0;
  renderStateTable.forEach([&](std::uint32_t state, std::uint32_t value) {
    scratch.renderStates[renderStateCount++] =
        D9CCommandChunkWireRenderStateV2{.state = state, .value = value};
  });
  out.renderStates = std::span(scratch.renderStates).first(renderStateCount);

  // --- textures ------------------------------------------------------------
  std::size_t textureCount = 0;
  for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_TEXTURES; ++slot) {
    if (!snapshot && (shadow.pendingTextureMask & (1u << slot)) == 0u) {
      continue;
    }
    auto& entry = scratch.textures[textureCount++];
    entry.wire = D9CCommandChunkWireTextureBindingV2{};
    entry.wire.slot = slot;
    entry.wire.valid = 1u;
    entry.object = bindings.textures[slot];
  }
  out.textures = std::span(scratch.textures).first(textureCount);

  // --- streams -------------------------------------------------------------
  std::size_t streamCount = 0;
  for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_STREAMS; ++slot) {
    if (!snapshot && (shadow.pendingStreamMask & (1u << slot)) == 0u) {
      continue;
    }
    auto& entry = scratch.streams[streamCount++];
    entry.wire = D9CCommandChunkWireStreamBindingV2{};
    entry.wire.slot = slot;
    entry.wire.valid = 1u;
    entry.wire.offset = bindings.streams[slot].offset;
    entry.wire.stride = bindings.streams[slot].stride;
    entry.wire.frequency = 0u;
    entry.object = bindings.streams[slot].buffer;
  }
  out.streams = std::span(scratch.streams).first(streamCount);

  // --- shaders -------------------------------------------------------------
  std::size_t shaderCount = 0;
  const auto appendShader = [&](std::uint32_t stage, bool valid,
                                const PeWireObjectRef& ref) {
    if (!valid) {
      return;
    }
    auto& entry = scratch.shaders[shaderCount++];
    entry.wire = D9CCommandChunkWireShaderBindingV2{};
    entry.wire.stage = stage;
    entry.wire.valid = 1u;
    entry.object = ref;
  };
  appendShader(D9C_COMMAND_CHUNK_V2_SHADER_STAGE_VERTEX,
               snapshot || shadow.pendingVs, bindings.vs);
  appendShader(D9C_COMMAND_CHUNK_V2_SHADER_STAGE_PIXEL,
               snapshot || shadow.pendingPs, bindings.ps);
  out.shaders = std::span(scratch.shaders).first(shaderCount);

  // --- vertex input --------------------------------------------------------
  // One entry, never two: declaration wins when both are dirty and `value`
  // carries the FVF either way.
  std::size_t vertexInputCount = 0;
  if (snapshot || shadow.pendingVdecl || shadow.pendingFvf) {
    auto& entry = scratch.vertexInputs[vertexInputCount++];
    entry.wire = D9CCommandChunkWireVertexInputV2{};
    entry.wire.valid = 1u;
    entry.wire.value = bindings.fvf;
    // In snapshot mode the fat-packet producer set BOTH vdeclValid and
    // fvfValid, and the shim's declaration-wins rule then selected the
    // declaration. Keep that outcome.
    if (snapshot || shadow.pendingVdecl) {
      entry.wire.kind = D9C_COMMAND_CHUNK_V2_VERTEX_INPUT_DECLARATION;
      entry.object = bindings.vdecl;
    } else {
      entry.wire.kind = D9C_COMMAND_CHUNK_V2_VERTEX_INPUT_FVF;
    }
  }
  out.vertexInputs = std::span(scratch.vertexInputs).first(vertexInputCount);

  // --- index buffer --------------------------------------------------------
  // Only indexed draws carry one. APPLY_STATE never does, matching the shim,
  // which emitted this section only when `indexed && indexed->ibValid`.
  std::size_t indexBufferCount = 0;
  const bool indexedDraw =
      params.recordType == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE ||
      params.recordType == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP;
  if (indexedDraw && shadow.pendingIb) {
    auto& entry = scratch.indexBuffers[indexBufferCount++];
    entry.wire = D9CCommandChunkWireIndexBindingV2{};
    entry.wire.valid = 1u;
    entry.object = bindings.indexBuffer;
  }
  out.indexBuffers = std::span(scratch.indexBuffers).first(indexBufferCount);

  // --- attachments ---------------------------------------------------------
  std::size_t renderTargetCount = 0;
  for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_RENDER_TARGETS;
       ++slot) {
    // populateDrawPacketAttachmentSnapshot's rule: a slot is present when it
    // is explicitly set or holds a non-null surface.
    const bool present =
        snapshot ? (bindings.rtExplicitMask[slot] ||
                    bindings.renderTargets[slot].object != nullptr)
                 : (shadow.pendingRtMask & (1u << slot)) != 0u;
    if (!present) {
      continue;
    }
    auto& entry = scratch.renderTargets[renderTargetCount++];
    entry.wire = D9CCommandChunkWireRenderTargetBindingV2{};
    entry.wire.slot = slot;
    entry.wire.valid = 1u;
    entry.object = bindings.renderTargets[slot];
  }
  out.renderTargets = std::span(scratch.renderTargets).first(renderTargetCount);

  std::size_t depthStencilCount = 0;
  if (snapshot || shadow.pendingDs) {
    auto& entry = scratch.depthStencils[depthStencilCount++];
    entry.wire = D9CCommandChunkWireDepthStencilBindingV2{};
    entry.wire.valid = 1u;
    entry.object = bindings.depthStencil;
  }
  out.depthStencils = std::span(scratch.depthStencils).first(depthStencilCount);

  // --- scalar sections -----------------------------------------------------
  if (snapshot || shadow.pendingViewport) {
    scratch.viewports[0] = shadow.viewportShadow;
    out.viewports = scratch.viewports;
  }
  if (snapshot || shadow.pendingScissor) {
    scratch.scissors[0] = shadow.scissorShadow;
    out.scissors = scratch.scissors;
  }
  if (snapshot || shadow.pendingMaterial) {
    scratch.materials[0] = shadow.materialShadow;
    out.materials = scratch.materials;
  }

  // --- clip planes ---------------------------------------------------------
  std::size_t clipPlaneCount = 0;
  for (std::uint32_t slot = 0; slot < 6u; ++slot) {
    // Snapshot emits every plane (the packet set clipPlaneMask = 0x3F).
    if (!snapshot && (shadow.pendingClipPlaneMask & (1u << slot)) == 0u) {
      continue;
    }
    auto& entry = scratch.clipPlanes[clipPlaneCount++];
    entry.slot = slot;
    std::memcpy(entry.values, &shadow.clipPlaneShadow[slot * 4u],
                sizeof(entry.values));
  }
  out.clipPlanes = std::span(scratch.clipPlanes).first(clipPlaneCount);

  // --- TSS / sampler / transform tables ------------------------------------
  const auto& tssTable = snapshot ? shadow.tssShadow : shadow.pendingTss;
  const auto& samplerTable =
      snapshot ? shadow.samplerStateShadow : shadow.pendingSamplerStates;
  const auto& transformTable =
      snapshot ? shadow.transformShadow : shadow.pendingTransforms;
  if (tssTable.size() > scratch.textureStageStates.size() ||
      samplerTable.size() > scratch.samplerStates.size() ||
      transformTable.size() > scratch.transforms.size()) {
    return false;
  }
  std::size_t tssCount = 0;
  tssTable.forEach(
      [&](std::uint32_t stage, std::uint32_t type, std::uint32_t value) {
        scratch.textureStageStates[tssCount++] =
            D9CDrawPacketTextureStageState{stage, type, value};
      });
  out.textureStageStates =
      std::span(scratch.textureStageStates).first(tssCount);

  std::size_t samplerCount = 0;
  samplerTable.forEach(
      [&](std::uint32_t sampler, std::uint32_t type, std::uint32_t value) {
        scratch.samplerStates[samplerCount++] =
            D9CDrawPacketSamplerState{sampler, type, value};
      });
  out.samplerStates = std::span(scratch.samplerStates).first(samplerCount);

  std::size_t transformCount = 0;
  transformTable.forEach(
      [&](std::uint32_t state, const D9CMatrix& matrix) {
        auto& entry = scratch.transforms[transformCount++];
        entry.state = state;
        entry.reserved = 0u;
        entry.matrix = matrix;
      });
  out.transforms = std::span(scratch.transforms).first(transformCount);

  // --- lights --------------------------------------------------------------
  // One pass over slots emitting into both arrays, matching the shim: a slot
  // can contribute a light, an enable, or both.
  std::size_t lightCount = 0;
  std::size_t lightEnableCount = 0;
  for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_LIGHTS; ++slot) {
    if (snapshot || (shadow.pendingLightSlotMask & (1u << slot)) != 0u) {
      auto& entry = scratch.lights[lightCount++];
      entry.slot = slot;
      entry.light = shadow.lightShadow[slot];
    }
    if (snapshot ||
        (shadow.pendingLightEnableValidMask & (1u << slot)) != 0u) {
      auto& entry = scratch.lightEnables[lightEnableCount++];
      entry.slot = slot;
      // Snapshot reads the shadow; delta reads the pending value mask.
      const std::uint32_t source = snapshot ? shadow.lightEnableShadow
                                            : shadow.pendingLightEnableMask;
      entry.enabled = (source & (1u << slot)) != 0u;
    }
  }
  out.lights = std::span(scratch.lights).first(lightCount);
  out.lightEnables = std::span(scratch.lightEnables).first(lightEnableCount);

  // --- constants -----------------------------------------------------------
  // Only the inline-delta path folds constants into the record. On the default
  // path (DXMT9_PE_INLINE_CONST_DELTA unset) flushPendingConsts has already
  // emitted standalone SET_CONST records, so draining here would duplicate
  // them and change the wire stream.
  (void)constants;

  // --- payloads and draw header -------------------------------------------
  // Every ref that reached a section must carry a usable identity. Without
  // this, a caller that fills only `object` produces sections that
  // CommandChunkV2Builder::appendHandle silently rejects -- it calls
  // PeWireObjectRef::valid(), fails the record through failActiveRecord() with
  // NO log line, and the app sees a bare D3DERR_INVALIDCALL from whatever API
  // call happened to trigger the barrier. That is exactly how the APPLY_STATE
  // migration first surfaced: "IDirect3DDevice9::Clear failed: Invalid call",
  // with the experiment harness still reporting status=pass because it does not
  // gate on the rendered image. In debug builds this turns that into an
  // immediate, located abort.
  // DXMT_ASSERT compiles to ((void)0) in release, so both the parameter and
  // the loop variable would otherwise read as unused there.
  const auto assertUsableRefs = [](auto sections,
                                   [[maybe_unused]] std::uint32_t kind) {
    for ([[maybe_unused]] const auto& entry : sections) {
      DXMT_ASSERT(entry.object.object == nullptr ||
                  entry.object.valid(kind));
    }
  };
  assertUsableRefs(out.textures, D9C_CHUNK_HANDLE_KIND_TEXTURE);
  assertUsableRefs(out.streams, D9C_CHUNK_HANDLE_KIND_BUFFER);
  assertUsableRefs(out.shaders, D9C_CHUNK_HANDLE_KIND_SHADER);
  assertUsableRefs(out.indexBuffers, D9C_CHUNK_HANDLE_KIND_BUFFER);
  assertUsableRefs(out.renderTargets, D9C_CHUNK_HANDLE_KIND_SURFACE);
  assertUsableRefs(out.depthStencils, D9C_CHUNK_HANDLE_KIND_SURFACE);
  for ([[maybe_unused]] const auto& entry : out.vertexInputs) {
    DXMT_ASSERT(entry.wire.kind != D9C_COMMAND_CHUNK_V2_VERTEX_INPUT_DECLARATION ||
                entry.object.object == nullptr ||
                entry.object.valid(D9C_CHUNK_HANDLE_KIND_VERTEX_DECL));
  }

  out.upIndexData = payloads.upIndex;
  out.upVertexData = payloads.upVertex;

  header = D9CCommandChunkWireDrawHeaderV2{};
  header.primitiveType = params.primitiveType;
  header.baseVertex = params.baseVertex;
  header.minVertex = params.minVertex;
  header.numVertices = params.numVertices;
  header.startVertex = params.startVertex;
  header.startIndex = params.startIndex;
  header.primitiveCount = params.primitiveCount;
  header.stride = params.stride;
  header.indexFormat = params.indexFormat;
  // The shim set FULL_SNAPSHOT from all-ones texture and stream masks, which
  // means it also fires for a delta that happens to touch every slot. Keep the
  // heuristic exactly, or the emitted flags differ.
  constexpr auto allTextures = (1u << D9C_DRAW_PACKET_MAX_TEXTURES) - 1u;
  constexpr auto allStreams = (1u << D9C_DRAW_PACKET_MAX_STREAMS) - 1u;
  if (snapshot || (shadow.pendingTextureMask == allTextures &&
                   shadow.pendingStreamMask == allStreams)) {
    header.flags |= D9C_COMMAND_CHUNK_V2_DRAW_FLAG_FULL_SNAPSHOT;
  }
  return true;
}

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
