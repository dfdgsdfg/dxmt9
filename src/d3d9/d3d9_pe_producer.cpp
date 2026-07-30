#include "d3d9_pe_producer.hpp"

#include "d3d9_pe_wire_handle.hpp"
#include "dxmt9/assert.hpp"
#include "util/log/log.hpp"

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
                        bool inlineConstDelta,
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
    // Reset BOTH halves. PeSparseScratch is a reused device member, so the FVF
    // branch below must not inherit a vdecl ref left by an earlier
    // DECLARATION build: vertexInputPrepare rejects an FVF entry whose object
    // is non-null (`return !object.object;`) and fails the whole record through
    // failActiveRecord() with no log line.
    entry = SparseBindingV2Input<D9CCommandChunkWireVertexInputV2>{};
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
  // DRAW_INDEXED_PRIMITIVE only -- deliberately NOT the _UP variant. An
  // indexed-UP draw carries its indices inline in the record payload and binds
  // no index buffer, and the shim proves it: appendLegacySparseRecord sets its
  // `indexed` packet pointer only for DRAW_INDEXED_PRIMITIVE, leaving it null
  // for _UP, so populateLegacySparseState's `indexed && indexed->ibValid` gate
  // never fired and no index-buffer section was produced. Including _UP here
  // emits a section production never emitted, plus a retained handle the record
  // does not need. Pinned by the differential's "indexed UP draw ignores a dirty
  // index buffer" fixture, which caught this before the UP call sites were
  // wired: 3 handles / 408 bytes against legacy's 2 / 368.
  const bool indexedDraw =
      params.recordType == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
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
  // Under DXMT9_PE_INLINE_CONST_DELTA the draw sites deliberately do NOT call
  // flushPendingConsts, and the dirty ranges ride inside the draw record
  // instead. The legacy shape folded them into the fat packet's
  // constDeltaSections; V2 expresses the same thing natively as constant-range
  // sections, so this drains straight into them.
  //
  // DRAINING MUTATES: each range is cleared once emitted, exactly as
  // foldConstShadowIntoDeltaSection did. That is why `constants` is non-const,
  // and it means a caller must not build a record it then throws away -- the
  // dirty ranges are gone. Off the inline path the shadows are already clean
  // (the caller flushed them as standalone records) so this is a no-op.
  //
  // registerBytes points into the shadow's own storage, which is device-owned
  // and outlives the append that consumes the span.
  if (inlineConstDelta) {
    struct ConstRange {
      ConstShadow* shadow;
      SparseConstantRangeV2Input* out;
      std::size_t elemSize;
    };
    const ConstRange ranges[D9C_DRAW_PACKET_CONST_DELTA_COUNT] = {
        {&constants.vsConstF, &out.vsFloatConstants, 16u},
        {&constants.vsConstI, &out.vsIntConstants, 16u},
        {&constants.vsConstB, &out.vsBoolConstants, 4u},
        {&constants.psConstF, &out.psFloatConstants, 16u},
        {&constants.psConstI, &out.psIntConstants, 16u},
        {&constants.psConstB, &out.psBoolConstants, 4u},
    };
    for (std::uint32_t kind = 0u; kind < D9C_DRAW_PACKET_CONST_DELTA_COUNT;
         ++kind) {
      auto& shadowRange = *ranges[kind].shadow;
      if (!shadowRange.dirty()) {
        continue;
      }
      const std::uint32_t start = shadowRange.dirtyStart;
      const std::uint32_t count = shadowRange.dirtyEnd - shadowRange.dirtyStart;
      if (!d9c_draw_packet_const_delta_section_range_valid(kind, start, count)) {
        return false;
      }
      const auto offset = static_cast<std::size_t>(start) * ranges[kind].elemSize;
      const auto bytes = static_cast<std::size_t>(count) * ranges[kind].elemSize;
      if (shadowRange.values.size() < offset + bytes) {
        return false;
      }
      *ranges[kind].out = SparseConstantRangeV2Input{
          .startRegister = start,
          .registerCount = count,
          .registerBytes = std::span<const std::byte>(
              reinterpret_cast<const std::byte*>(shadowRange.values.data()) +
                  offset,
              bytes),
      };
      shadowRange.clear();
    }
  }

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
  // Release-safe, not just DXMT_ASSERT: every lane that runs under Wine is a
  // release build (b_ndebug=if-release), so an assert-only guard would compile
  // out of precisely the configuration where this bug class manifests --
  // populateBindingView lives in the TU no native test can compile. Logged once
  // per process so a hot path cannot flood the log.
  const auto reportUnusableRef = [](const char* section) {
    static bool reported = false;
    if (reported) {
      return;
    }
    reported = true;
    dxmt9::util::logf(dxmt9::util::LogLevel::Error, "dxmt9-pe",
                      "buildSparseStateV2: %s section carries a bound object "
                      "with an unusable wire identity; appendHandle will reject "
                      "the record and the caller will see a bare "
                      "D3DERR_INVALIDCALL. The binding view was filled without "
                      "identity.",
                      section);
  };
  const auto checkUsableRefs = [&](auto sections, std::uint32_t kind,
                                   const char* section) {
    for (const auto& entry : sections) {
      if (entry.object.object != nullptr && !entry.object.valid(kind)) {
        reportUnusableRef(section);
        DXMT_ASSERT(false);
      }
    }
  };
  checkUsableRefs(out.textures, D9C_CHUNK_HANDLE_KIND_TEXTURE, "texture");
  checkUsableRefs(out.streams, D9C_CHUNK_HANDLE_KIND_BUFFER, "stream");
  checkUsableRefs(out.shaders, D9C_CHUNK_HANDLE_KIND_SHADER, "shader");
  checkUsableRefs(out.indexBuffers, D9C_CHUNK_HANDLE_KIND_BUFFER, "index buffer");
  checkUsableRefs(out.renderTargets, D9C_CHUNK_HANDLE_KIND_SURFACE, "render target");
  checkUsableRefs(out.depthStencils, D9C_CHUNK_HANDLE_KIND_SURFACE, "depth stencil");
  for (const auto& entry : out.vertexInputs) {
    if (entry.wire.kind == D9C_COMMAND_CHUNK_V2_VERTEX_INPUT_DECLARATION &&
        entry.object.object != nullptr &&
        !entry.object.valid(D9C_CHUNK_HANDLE_KIND_VERTEX_DECL)) {
      reportUnusableRef("vertex declaration");
      DXMT_ASSERT(false);
    }
  }

  out.upIndexData = payloads.upIndex;
  out.upVertexData = payloads.upVertex;

  // The draw header carries a DIFFERENT SUBSET per record type, and the fields
  // a type does not use must stay zero or the emitted bytes differ. This
  // mirrors appendLegacySparseRecord's per-case fill exactly:
  //
  //   DRAW_PRIMITIVE            primitiveType, startVertex, primitiveCount
  //   DRAW_INDEXED_PRIMITIVE    primitiveType, baseVertex, minVertex,
  //                             numVertices, startIndex, primitiveCount
  //   DRAW_PRIMITIVE_UP         primitiveType, primitiveCount, stride
  //   DRAW_INDEXED_PRIMITIVE_UP primitiveType, minVertex, numVertices,
  //                             primitiveCount, stride, indexFormat
  //
  // Note what is NOT shared: stride belongs to the UP variants only (their
  // vertex data is inline), startVertex to the non-indexed non-UP case only,
  // startIndex to the indexed non-UP case only, and indexFormat to the indexed
  // UP case only. Setting all of them unconditionally is what a first cut does,
  // and the differential catches it as a one-word byte difference.
  header = D9CCommandChunkWireDrawHeaderV2{};
  header.primitiveType = params.primitiveType;
  switch (params.recordType) {
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
      header.startVertex = params.startVertex;
      header.primitiveCount = params.primitiveCount;
      break;
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
      header.baseVertex = params.baseVertex;
      header.minVertex = params.minVertex;
      header.numVertices = params.numVertices;
      header.startIndex = params.startIndex;
      header.primitiveCount = params.primitiveCount;
      break;
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
      header.primitiveCount = params.primitiveCount;
      header.stride = params.stride;
      break;
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP:
      header.minVertex = params.minVertex;
      header.numVertices = params.numVertices;
      header.primitiveCount = params.primitiveCount;
      header.stride = params.stride;
      header.indexFormat = params.indexFormat;
      break;
    default:
      // APPLY_STATE and the unset type carry no draw parameters;
      // appendApplyStateV2 takes only the flags.
      break;
  }
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

namespace {

// Fail loudly on an unstamped recordType instead of silently deciding "not
// indexed". D9C_COMMAND_RECORD_* starts at 1, so 0 is never a real record.
// This is not hypothetical: the first wiring of the draw call sites reached
// addChunkContextSections with recordType == 0, because the device forwarder
// stamped it onto a by-value copy of params. The stream/index rebuild then WIPED
// the index section buildSparseStateV2 had already emitted for pendingIb, and
// since SetIndices records nothing standalone in chunk mode, every indexed draw
// replayed against a stale index buffer -- GT1 rendered sliver triangles with
// garbled HUD digits while the harness still reported status=pass. The offline
// differential passed byte-equality throughout, because its fixtures stamp
// params themselves and so never reproduce the device's threading of it.
// Refusing the record (the caller turns this into D3DERR_INVALIDCALL) beats
// rendering garbage.
//
// Release-safe rather than DXMT_ASSERT-only: every lane that runs under Wine is
// a release build (b_ndebug=if-release), which is exactly the configuration
// where this manifested. And deliberately no DXMT_ASSERT alongside it: the
// native build is debugoptimized, so an assert here is LIVE and would abort the
// very test that pins this guard.
bool validRecordType(const PeDrawParams& params, const char* where) noexcept {
  if (params.recordType != 0u) {
    return true;
  }
  static bool reported = false;
  if (!reported) {
    reported = true;
    dxmt9::util::logf(dxmt9::util::LogLevel::Error, "dxmt9-pe",
                      "%s: params.recordType is 0. The caller did not stamp it, "
                      "so the index-buffer section would be suppressed and every "
                      "indexed draw would replay against a stale index buffer.",
                      where);
  }
  return false;
}

}  // namespace

bool addChunkContextSections(const PeChunkContext& chunk,
                             const PeHotStateShadow& shadow,
                             const PeBindingView& bindings,
                             const PeDrawParams& params,
                             bool forceFullSnapshot,
                             PeSparseScratch& scratch,
                             SparseStateV2Input& out) noexcept {
  if (!validRecordType(params, "addChunkContextSections")) {
    return false;
  }
  // Snapshot mode: buildSparseStateV2 already emitted all 16 stream sections,
  // null unbinds included, and legacy's add-only mask semantics left an all-ones
  // mask untouched here. The rebuild below is a REPLACEMENT, so running it under
  // snapshot silently drops every bound-but-retained-and-clean slot and every
  // null unbind -- pinned by the differential's "draw under snapshot keeps every
  // stream section" fixture (legacy 2396 bytes / 3 handles against a rebuilt
  // 1916 / 2). The emitted set is already a superset of anything retention could
  // add, so there is nothing to do.
  const bool snapshot = forceFullSnapshot || dxmt9PeFullSnapshotEnabled();
  // Streams: dirty, OR bound and not yet retained by this chunk. One ascending
  // pass so the section order is correct by construction.
  std::size_t streamCount = 0;
  for (std::uint32_t slot = 0; !snapshot && slot < D9C_DRAW_PACKET_MAX_STREAMS;
       ++slot) {
    const bool dirty = (shadow.pendingStreamMask & (1u << slot)) != 0u;
    const bool bound = bindings.streams[slot].buffer.object != nullptr;
    const bool retained = (chunk.retainedStreamMask & (1u << slot)) != 0u;
    if (!dirty && !(bound && !retained)) {
      continue;
    }
    auto& entry = scratch.streams[streamCount++];
    entry = SparseBindingV2Input<D9CCommandChunkWireStreamBindingV2>{};
    entry.wire.slot = slot;
    entry.wire.valid = 1u;
    entry.wire.offset = bindings.streams[slot].offset;
    entry.wire.stride = bindings.streams[slot].stride;
    entry.wire.frequency = 0u;
    entry.object = bindings.streams[slot].buffer;
  }
  if (!snapshot) {
    out.streams = std::span(scratch.streams).first(streamCount);
  }

  // Index buffer: INDEXED RECORDS ONLY. A non-indexed draw has no index
  // binding at all -- the legacy packet's ibValid field lives on
  // D9CDrawIndexedPrimitivePacket and the call sites ran
  // populateDrawPacketIndexDependency only for indexed draws. Emitting one here
  // for a non-indexed draw adds a section production never produced.
  //
  // Three independent reasons, matching production's two sites. The third
  // clause is the retention leg; without it the chunk can end up replaying a
  // draw against a buffer it never retained.
  // DRAW_INDEXED_PRIMITIVE only -- deliberately NOT the _UP variant. An
  // indexed-UP draw carries its indices inline in the record payload and binds
  // no index buffer, and the shim proves it: appendLegacySparseRecord sets its
  // `indexed` packet pointer only for DRAW_INDEXED_PRIMITIVE, leaving it null
  // for _UP, so populateLegacySparseState's `indexed && indexed->ibValid` gate
  // never fired and no index-buffer section was produced. Including _UP here
  // emits a section production never emitted, plus a retained handle the record
  // does not need. Pinned by the differential's "indexed UP draw ignores a dirty
  // index buffer" fixture, which caught this before the UP call sites were
  // wired: 3 handles / 408 bytes against legacy's 2 / 368.
  const bool indexedDraw =
      params.recordType == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
  const std::uint64_t ibWire =
      d9cWireHandleValue(toWireHandle(bindings.indexBuffer.object));
  const bool ibBound = bindings.indexBuffer.object != nullptr;
  const bool emitIndex =
      indexedDraw &&
      (shadow.pendingIb || !chunk.indexBufferKnown ||
       chunk.submittedIndexBufferWire != ibWire ||
       (!chunk.indexBufferRetained && ibBound));
  std::size_t indexCount = 0;
  if (emitIndex) {
    auto& entry = scratch.indexBuffers[indexCount++];
    entry = SparseBindingV2Input<D9CCommandChunkWireIndexBindingV2>{};
    entry.wire.valid = 1u;
    entry.object = bindings.indexBuffer;
  }
  out.indexBuffers = std::span(scratch.indexBuffers).first(indexCount);
  return true;
}

}  // namespace dxmt9::d3d9::pe
