// pe_producer_differential_spec
//
// Runs the REAL old and new producers over one corpus and requires that they
// agree on the emitted V2 chunk bytes and on the builder side effects that
// bytes do not capture: record/handle/payload counts, retained object count,
// and the return value.
//
// Both lanes call functions from src/. Nothing here reimplements production
// logic -- that is the whole reason the producer was rehosted into a natively
// buildable translation unit. The pre-existing
// pe_full_snapshot_equivalence_spec mirrors buildDrawPrimitivePacket at test
// scope because it had no other choice; a mirror proves the mirror consistent
// with itself and says nothing about the code that ships. See
// docs/superpowers/specs/2026-07-29-pe-legacy-record-removal-design.md §6.
//
// APPLY_STATE shapes only. Chunk-context and draw-header fixtures arrive with
// the code paths that consume them, so a fixture never sits red across a task
// it does not belong to.

#include "d3d9_pe_chunk_v2_builder.hpp"
#include "d3d9_pe_producer.hpp"
#include "d3d9_pe_const_shadow.hpp"
#include "d3d9_pe_draw_packet.hpp"
#include "d3d9_pe_producer_views.hpp"
#include "d3d9_pe_wire_handle.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// --- C-ABI object stubs the V2 builder's retainer links against -------------

struct RefCounter {
  std::uint32_t refs = 1u;
};

struct D9CSurface : RefCounter {};
struct D9CTexture : RefCounter {};
struct D9CBuffer : RefCounter {};
struct D9CShader : RefCounter {};
struct D9CVertexDecl : RefCounter {};
struct D9CQuery : RefCounter {};

template <typename T>
void addRef(T* value) {
  ++value->refs;
}
template <typename T>
std::uint32_t release(T* value) {
  return --value->refs;
}

extern "C" void dxmt9c_surface_addref(D9CSurface* v) { addRef(v); }
extern "C" std::uint32_t dxmt9c_surface_release(D9CSurface* v) { return release(v); }
extern "C" void dxmt9c_texture_addref(D9CTexture* v) { addRef(v); }
extern "C" std::uint32_t dxmt9c_texture_release(D9CTexture* v) { return release(v); }
extern "C" void dxmt9c_buffer_addref(D9CBuffer* v) { addRef(v); }
extern "C" std::uint32_t dxmt9c_buffer_release(D9CBuffer* v) { return release(v); }
extern "C" void dxmt9c_shader_addref(D9CShader* v) { addRef(v); }
extern "C" std::uint32_t dxmt9c_shader_release(D9CShader* v) { return release(v); }
extern "C" void dxmt9c_vdecl_addref(D9CVertexDecl* v) { addRef(v); }
extern "C" std::uint32_t dxmt9c_vdecl_release(D9CVertexDecl* v) { return release(v); }
extern "C" void dxmt9c_query_addref(D9CQuery* v) { addRef(v); }
extern "C" std::uint32_t dxmt9c_query_release(D9CQuery* v) { return release(v); }

namespace {

namespace pe = dxmt9::d3d9::pe;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure(message);
  }
}

// --- object identity -------------------------------------------------------
//
// cacheWireObjectRef publishes into the same cache the legacy shim resolves
// through (lookupCachedWireObjectRef), so a ref built this way is visible to
// BOTH lanes. A bare PeWireObjectRef that skipped publication would make every
// legacy-lane fixture with a bound object fail on handle resolution, which
// looks like disagreement but is really an unprepared fixture.

std::uint64_t nextObjectId = 1u;

// One STABLE identity per object, minted on first request and reused after.
//
// This mirrors production: a PE wrapper caches its identity once at
// construction and wireObject() hands out the same ref forever. Minting a
// fresh identity per call instead makes the two lanes disagree for a reason
// that has nothing to do with the producer -- the legacy lane resolves handles
// by object POINTER through the cache and so collapses N identities for one
// object down to the last one published, while the direct lane carries each
// distinct ref and the builder cannot dedupe them. That shows up as a handle
// count mismatch with identical payload bytes.
template <typename Object>
pe::PeWireObjectRef publishedRef(Object* object, std::uint32_t kind) {
  static std::vector<std::pair<Object*, pe::PeWireObjectRef>> cache;
  for (const auto& entry : cache) {
    if (entry.first == object) {
      return entry.second;
    }
  }
  pe::PeWireObjectRef ref{};
  const auto id = ++nextObjectId;
  const auto getter = [kind, id](Object*, D9CWireObjectIdentity* identity) {
    *identity = D9CWireObjectIdentity{
        .kind = kind,
        .generation = 3u,
        .objectId = 0x900000000ull + id,
    };
    return 0;
  };
  check(pe::cacheWireObjectRef(object, kind, getter, ref),
        "publishedRef must cache and publish the identity");
  cache.emplace_back(object, ref);
  return ref;
}

// Same shape, never published: the legacy lane cannot resolve it.
template <typename Object>
pe::PeWireObjectRef unpublishedRef(Object* object, std::uint32_t kind) {
  return pe::PeWireObjectRef{
      .identity = D9CWireObjectIdentity{.kind = kind,
                                        .generation = 3u,
                                        .objectId = 0xdead0000ull},
      .object = object,
  };
}

// Shared stub objects. Distinct addresses matter: the cache is keyed on the
// object pointer.
D9CTexture tex0, tex1;
D9CBuffer vb0, vb1;
D9CShader vsObj, psObj;
D9CSurface rt0, ds0;
D9CVertexDecl vdecl0;
D9CBuffer ib0, ib1;

// --- fixtures --------------------------------------------------------------

struct Fixture {
  std::string name;
  PeHotStateShadow shadow{};
  PeConstShadowBlock constants{};
  pe::PeBindingView bindings{};
  pe::PeChunkContext chunk{};  // unused until the draw-site task
  pe::PeDrawPayloads payloads{};
  pe::PeDrawParams params{};
  bool forceFullSnapshot = false;
  bool inlineConstDelta = false;
};

struct LaneResult {
  bool ok = false;
  std::vector<std::byte> bytes;
  std::size_t recordCount = 0;
  std::size_t handleCount = 0;
  std::size_t payloadBytes = 0;
  std::size_t retainedObjectCount = 0;
};

// Counts and retention are read BEFORE seal(); the blob is copied out because
// SealedCommandChunkV2::blob is a span into the builder, which is lane-local.
LaneResult finishLane(pe::CommandChunkV2Builder& builder, bool ok) {
  LaneResult result;
  result.ok = ok;
  if (!ok) {
    return result;
  }
  result.recordCount = builder.recordCount();
  result.handleCount = builder.handleCount();
  result.payloadBytes = builder.payloadBytes();
  result.retainedObjectCount = builder.retainedObjectCount();
  const auto sealed = builder.seal();
  result.bytes.assign(sealed.blob.begin(), sealed.blob.end());
  return result;
}

// Production reuses ONE PeSparseScratch device member across every build, so a
// section that fails to reset an entry inherits the previous build's value. A
// fresh local scratch per lane hides that entirely, so this is a shared static
// -- deliberately mirroring the device.
pe::PeSparseScratch& sharedScratch() {
  static pe::PeSparseScratch scratch{};
  return scratch;
}

// PeStreamSources as currentDrawStreamSources() builds it: wire handle plus
// offset and stride per slot. A translation of the view, not a reimplementation
// of any decision.
pe::PeStreamSources streamSourcesFrom(const pe::PeBindingView& bindings) {
  pe::PeStreamSources sources{};
  for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_STREAMS; ++slot) {
    sources[slot].buffer = toWireHandle(bindings.streams[slot].buffer.object);
    sources[slot].offset = bindings.streams[slot].offset;
    sources[slot].stride = bindings.streams[slot].stride;
  }
  return sources;
}

bool isUpRecord(std::uint32_t type) {
  return type == D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP ||
         type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP;
}

// The real legacy UP path. Two things differ from the buffer-backed draws and
// both are load-bearing:
//   - No chunk-dependency checkpoint. UP draws carry their geometry inline, so
//     neither populateDrawPacketStreamDependencies nor
//     populateDrawPacketIndexDependency ran at these call sites, and the shim
//     leaves its `indexed` pointer null for UP records -- so an indexed-UP
//     record gets NO index-buffer section however dirty the IB shadow is.
//   - The payload rides after the record: index bytes then vertex bytes, at the
//     offsets the header declares.
// inlineConstDelta is not modelled: production's UP sites always flush pending
// constants first and never fold, and the UP const-delta base depends on the
// payload offsets, so a folded UP fixture would test a shape production cannot
// emit.
LaneResult runLegacyUpLane(const Fixture& fixture) {
  pe::CommandChunkV2Builder builder;
  D9CDrawPrimitivePacket packet{};
  if (!pe::buildDrawPacketFromViews(
          fixture.shadow, fixture.bindings, fixture.params.primitiveType,
          /*startVertex=*/0u, fixture.params.primitiveCount,
          fixture.forceFullSnapshot, packet)) {
    return LaneResult{};
  }
  const auto indexBytes =
      static_cast<std::uint32_t>(fixture.payloads.upIndex.size());
  const auto vertexBytes =
      static_cast<std::uint32_t>(fixture.payloads.upVertex.size());
  std::vector<std::byte> recordBytes;
  if (fixture.params.recordType == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP) {
    D9CCommandRecordDrawIndexedPrimitiveUP record{};
    record.header.type = fixture.params.recordType;
    record.packet.state = packet;
    record.packet.minVertex = fixture.params.minVertex;
    record.packet.numVertices = fixture.params.numVertices;
    record.packet.primitiveCount = fixture.params.primitiveCount;
    record.packet.indexFormat = fixture.params.indexFormat;
    record.packet.stride = fixture.params.stride;
    record.packet.indexDataOffset = sizeof(record);
    record.packet.indexDataSize = indexBytes;
    record.packet.vertexDataOffset = record.packet.indexDataOffset + indexBytes;
    record.packet.vertexDataSize = vertexBytes;
    record.header.size = static_cast<std::uint32_t>(sizeof(record)) +
                         indexBytes + vertexBytes;
    recordBytes.resize(record.header.size);
    std::memcpy(recordBytes.data(), &record, sizeof(record));
    if (indexBytes != 0u) {
      std::memcpy(recordBytes.data() + record.packet.indexDataOffset,
                  fixture.payloads.upIndex.data(), indexBytes);
    }
    if (vertexBytes != 0u) {
      std::memcpy(recordBytes.data() + record.packet.vertexDataOffset,
                  fixture.payloads.upVertex.data(), vertexBytes);
    }
  } else {
    D9CCommandRecordDrawPrimitiveUP record{};
    record.header.type = fixture.params.recordType;
    record.packet.state = packet;
    record.packet.primitiveCount = fixture.params.primitiveCount;
    record.packet.stride = fixture.params.stride;
    record.packet.vertexDataOffset = sizeof(record);
    record.packet.vertexDataSize = vertexBytes;
    record.header.size =
        static_cast<std::uint32_t>(sizeof(record)) + vertexBytes;
    recordBytes.resize(record.header.size);
    std::memcpy(recordBytes.data(), &record, sizeof(record));
    if (vertexBytes != 0u) {
      std::memcpy(recordBytes.data() + record.packet.vertexDataOffset,
                  fixture.payloads.upVertex.data(), vertexBytes);
    }
  }
  const bool ok = pe::appendLegacyCommandRecordAsV2(builder, recordBytes);
  return finishLane(builder, ok);
}

// The direct UP lane: state build plus payload spans, and NO chunk-context step,
// mirroring what the migrated UP call sites do.
LaneResult runDirectUpLane(const Fixture& fixture) {
  pe::CommandChunkV2Builder builder;
  PeConstShadowBlock constants = fixture.constants;
  pe::PeSparseScratch& scratch = sharedScratch();
  pe::SparseStateV2Input state{};
  D9CCommandChunkWireDrawHeaderV2 header{};
  if (!pe::buildSparseStateV2(fixture.shadow, constants, fixture.bindings,
                              fixture.payloads, fixture.params,
                              fixture.forceFullSnapshot,
                              /*inlineConstDelta=*/false, scratch, header,
                              state)) {
    return LaneResult{};
  }
  const bool ok = pe::appendSparseRecordV2(
      builder, fixture.params.recordType, header, state);
  return finishLane(builder, ok);
}

bool isIndexedRecord(std::uint32_t type) {
  return type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE ||
         type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP;
}

// The real legacy DRAW path: fat packet, then the two chunk-dependency helpers
// the call sites run inside their writer lambda, then serialize and let the shim
// re-encode. Both helpers are the production free functions from
// d3d9_pe_draw_packet.hpp -- nothing here reimplements their decisions.
LaneResult runLegacyDrawLane(const Fixture& fixture) {
  pe::CommandChunkV2Builder builder;
  D9CDrawIndexedPrimitivePacket indexed{};
  if (!pe::buildDrawPacketFromViews(
          fixture.shadow, fixture.bindings, fixture.params.primitiveType,
          fixture.params.startVertex, fixture.params.primitiveCount,
          fixture.forceFullSnapshot, indexed.state)) {
    return LaneResult{};
  }
  // The inline-const fold, as the draw sites run it, into the fat packet.
  std::vector<std::uint8_t> constPayload(64u * 1024u);
  std::uint32_t constPayloadBytes = 0u;
  if (fixture.inlineConstDelta) {
    PeConstShadowBlock constants = fixture.constants;
    ConstShadow* shadows[6] = {&constants.vsConstF, &constants.vsConstI,
                               &constants.vsConstB, &constants.psConstF,
                               &constants.psConstI, &constants.psConstB};
    const std::size_t elemSizes[6] = {16u, 16u, 4u, 16u, 16u, 4u};
    for (std::uint32_t kind = 0; kind < 6u; ++kind) {
      if (!foldConstShadowIntoDeltaSection(
              *shadows[kind], kind, elemSizes[kind],
              indexed.state.constDeltaSections[kind], constPayload.data(),
              constPayload.size(), constPayloadBytes)) {
        return LaneResult{};
      }
    }
  }
  indexed.baseVertex = fixture.params.baseVertex;
  indexed.minVertex = fixture.params.minVertex;
  indexed.numVertices = fixture.params.numVertices;
  indexed.startIndex = fixture.params.startIndex;
  indexed.primitiveCount = fixture.params.primitiveCount;

  const bool indexedDraw = isIndexedRecord(fixture.params.recordType);
  const auto sources = streamSourcesFrom(fixture.bindings);
  if (indexedDraw) {
    indexed.ibHandle = toWireHandle(fixture.bindings.indexBuffer.object);
    const std::uint64_t ibWire = d9cWireHandleValue(indexed.ibHandle);
    indexed.ibValid = (fixture.shadow.pendingIb ||
                       !fixture.chunk.indexBufferKnown ||
                       fixture.chunk.submittedIndexBufferWire != ibWire)
                          ? 1u
                          : 0u;
  }
  // The two append-time dependency checkpoints, production code.
  pe::populateDrawPacketStreamDependencies(indexed.state, sources,
                                           fixture.chunk.retainedStreamMask);
  if (indexedDraw) {
    pe::populateDrawPacketIndexDependency(indexed,
                                          fixture.chunk.indexBufferRetained);
  }

  std::vector<std::byte> recordBytes;
  if (indexedDraw) {
    D9CCommandRecordDrawIndexedPrimitive record{};
    record.header.type = fixture.params.recordType;
    record.packet = indexed;
    record.header.size = static_cast<std::uint32_t>(sizeof(record) +
                                                    constPayloadBytes);
    recordBytes.resize(record.header.size);
    std::memcpy(recordBytes.data(), &record, sizeof(record));
    if (constPayloadBytes != 0u) {
      std::memcpy(recordBytes.data() + sizeof(record), constPayload.data(),
                  constPayloadBytes);
    }
  } else {
    D9CCommandRecordDrawPrimitive record{};
    record.header.type = fixture.params.recordType;
    record.packet = indexed.state;
    record.packet.primitiveCount = fixture.params.primitiveCount;
    record.header.size = static_cast<std::uint32_t>(sizeof(record) +
                                                    constPayloadBytes);
    recordBytes.resize(record.header.size);
    std::memcpy(recordBytes.data(), &record, sizeof(record));
    if (constPayloadBytes != 0u) {
      std::memcpy(recordBytes.data() + sizeof(record), constPayload.data(),
                  constPayloadBytes);
    }
  }
  const bool ok = pe::appendLegacyCommandRecordAsV2(builder, recordBytes);
  return finishLane(builder, ok);
}

LaneResult runDirectDrawLane(const Fixture& fixture) {
  pe::CommandChunkV2Builder builder;
  PeConstShadowBlock constants = fixture.constants;
  pe::PeSparseScratch& scratch = sharedScratch();
  pe::SparseStateV2Input state{};
  D9CCommandChunkWireDrawHeaderV2 header{};
  if (!pe::buildSparseStateV2(fixture.shadow, constants, fixture.bindings,
                              fixture.payloads, fixture.params,
                              fixture.forceFullSnapshot,
                              fixture.inlineConstDelta, scratch, header,
                              state)) {
    return LaneResult{};
  }
  if (!pe::addChunkContextSections(fixture.chunk, fixture.shadow,
                                   fixture.bindings, fixture.params, scratch,
                                   state)) {
    return LaneResult{};
  }
  const bool ok =
      pe::appendSparseRecordV2(builder, fixture.params.recordType, header,
                               state);
  return finishLane(builder, ok);
}

// The real legacy path for APPLY_STATE: build the fat packet, serialize it as
// a legacy record, and let the shim re-parse and re-encode it. chunkBarrierFlush
// applies no chunk-context step and no constant folding, so neither appears
// here.
LaneResult runLegacyLane(const Fixture& fixture) {
  pe::CommandChunkV2Builder builder;
  D9CDrawPrimitivePacket packet;
  if (!pe::buildDrawPacketFromViews(
          fixture.shadow, fixture.bindings, fixture.params.primitiveType,
          fixture.params.startVertex, fixture.params.primitiveCount,
          fixture.forceFullSnapshot, packet)) {
    return LaneResult{};
  }
  D9CCommandRecordApplyState record{};
  record.header.type = D9C_COMMAND_RECORD_APPLY_STATE;
  record.header.size = sizeof(record);
  record.packet = packet;
  std::vector<std::byte> recordBytes(sizeof(record));
  std::memcpy(recordBytes.data(), &record, sizeof(record));
  const bool ok = pe::appendLegacyCommandRecordAsV2(builder, recordBytes);
  return finishLane(builder, ok);
}

LaneResult runDirectLane(const Fixture& fixture) {
  pe::CommandChunkV2Builder builder;
  PeConstShadowBlock constants = fixture.constants;  // lanes must not share
  pe::PeSparseScratch& scratch = sharedScratch();
  pe::SparseStateV2Input state{};
  D9CCommandChunkWireDrawHeaderV2 header{};
  if (!pe::buildSparseStateV2(fixture.shadow, constants, fixture.bindings,
                              fixture.payloads, fixture.params,
                              fixture.forceFullSnapshot,
                              fixture.inlineConstDelta, scratch, header,
                              state)) {
    return LaneResult{};
  }
  const bool ok = pe::appendApplyStateV2(builder, header.flags, state);
  return finishLane(builder, ok);
}

void requireLanesAgree(const Fixture& f) {
  const bool draw = f.params.recordType != D9C_COMMAND_RECORD_APPLY_STATE &&
                    f.params.recordType != 0u;
  const bool up = isUpRecord(f.params.recordType);
  const LaneResult legacy = up ? runLegacyUpLane(f)
                               : (draw ? runLegacyDrawLane(f)
                                       : runLegacyLane(f));
  const LaneResult direct = up ? runDirectUpLane(f)
                               : (draw ? runDirectDrawLane(f)
                                       : runDirectLane(f));
  check(legacy.ok == direct.ok, f.name + ": lanes must agree on success");
  if (!legacy.ok) {
    return;  // both failed; failure reasons may legitimately differ
  }
  const auto shape = [](const LaneResult& r) {
    return "bytes=" + std::to_string(r.bytes.size()) +
           " records=" + std::to_string(r.recordCount) +
           " handles=" + std::to_string(r.handleCount) +
           " payload=" + std::to_string(r.payloadBytes) +
           " retained=" + std::to_string(r.retainedObjectCount);
  };
  check(legacy.bytes.size() == direct.bytes.size(),
        f.name + ": chunk byte length must match\n    legacy: " +
            shape(legacy) + "\n    direct: " + shape(direct));
  if (std::memcmp(legacy.bytes.data(), direct.bytes.data(),
                  legacy.bytes.size()) != 0) {
    std::size_t at = 0;
    while (at < legacy.bytes.size() && legacy.bytes[at] == direct.bytes[at]) {
      ++at;
    }
    std::string dump = f.name + ": chunk bytes must be identical; first differ at byte " +
                       std::to_string(at) + "\n    legacy:";
    for (std::size_t k = at; k < legacy.bytes.size() && k < at + 24; ++k) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), " %02x",
                    static_cast<unsigned>(std::to_integer<std::uint8_t>(legacy.bytes[k])));
      dump += buf;
    }
    dump += "\n    direct:";
    for (std::size_t k = at; k < direct.bytes.size() && k < at + 24; ++k) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), " %02x",
                    static_cast<unsigned>(std::to_integer<std::uint8_t>(direct.bytes[k])));
      dump += buf;
    }
    check(false, dump);
  }
  check(legacy.recordCount == direct.recordCount,
        f.name + ": record count must match");
  check(legacy.handleCount == direct.handleCount,
        f.name + ": handle count must match");
  check(legacy.payloadBytes == direct.payloadBytes,
        f.name + ": payload bytes must match");
  check(legacy.retainedObjectCount == direct.retainedObjectCount,
        f.name + ": retained object count must match");
}

// --- deterministic corpus --------------------------------------------------

void emptyDelta() {
  Fixture f;
  f.name = "empty delta";
  requireLanesAgree(f);
}

void singleRenderStateDirty() {
  Fixture f;
  f.name = "one render state dirty";
  f.shadow.pendingRenderStates.set(7u, 1u);
  requireLanesAgree(f);
}

void scalarCategoriesDirty() {
  Fixture f;
  f.name = "viewport, scissor and material dirty";
  f.shadow.pendingViewport = true;
  f.shadow.pendingScissor = true;
  f.shadow.pendingMaterial = true;
  f.shadow.viewportShadow = D9CViewport{0u, 0u, 640u, 480u, 0.0f, 1.0f};
  f.shadow.scissorShadow = D9CRect{1, 2, 3, 4};
  requireLanesAgree(f);
}

void everyCategoryDirty() {
  Fixture f;
  f.name = "every category dirty";
  f.shadow.pendingRenderStates.set(7u, 1u);
  f.shadow.pendingTextureMask = 0x1u;
  f.shadow.pendingStreamMask = 0x1u;
  f.shadow.pendingVs = true;
  f.shadow.pendingPs = true;
  f.shadow.pendingVdecl = true;
  f.shadow.pendingViewport = true;
  f.shadow.pendingScissor = true;
  f.shadow.pendingMaterial = true;
  f.shadow.pendingClipPlaneMask = 0x3u;
  f.shadow.pendingLightSlotMask = 0x1u;
  f.shadow.pendingLightEnableValidMask = 0x1u;
  f.shadow.pendingLightEnableMask = 0x1u;
  f.shadow.pendingTss.set(0u, 1u, 42u);
  f.shadow.pendingSamplerStates.set(0u, 1u, 7u);
  f.shadow.pendingTransforms.set(kD3dTsView, identityTransformMatrix());
  f.bindings.textures[0] = publishedRef(&tex0, D9C_CHUNK_HANDLE_KIND_TEXTURE);
  f.bindings.streams[0].buffer =
      publishedRef(&vb0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  f.bindings.streams[0].offset = 64u;
  f.bindings.streams[0].stride = 32u;
  f.bindings.vs = publishedRef(&vsObj, D9C_CHUNK_HANDLE_KIND_SHADER);
  f.bindings.ps = publishedRef(&psObj, D9C_CHUNK_HANDLE_KIND_SHADER);
  f.bindings.fvf = 0x142u;
  requireLanesAgree(f);
}

void attachmentsDirty() {
  Fixture f;
  f.name = "render target and depth stencil dirty";
  f.shadow.pendingRtMask = 0x1u;
  f.shadow.pendingDs = true;
  f.bindings.renderTargets[0] =
      publishedRef(&rt0, D9C_CHUNK_HANDLE_KIND_SURFACE);
  f.bindings.depthStencil = publishedRef(&ds0, D9C_CHUNK_HANDLE_KIND_SURFACE);
  requireLanesAgree(f);
}

void renderStatesAtCap() {
  Fixture f;
  f.name = "render states at the section cap";
  for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_RENDER_STATES;
       ++slot) {
    f.shadow.pendingRenderStates.set(slot, slot + 1u);
  }
  requireLanesAgree(f);
}

void renderStatesOverCapFailBothLanes() {
  Fixture f;
  f.name = "render states over cap";
  // The pending table holds kPeRenderStateSlots (256) slots while the V2
  // section cap is D9C_DRAW_PACKET_MAX_RENDER_STATES (64), so 65 distinct sets
  // really do over-fill rather than being silently dropped by the table.
  for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_RENDER_STATES + 1u;
       ++slot) {
    f.shadow.pendingRenderStates.set(slot, slot + 1u);
  }
  check(!runLegacyLane(f).ok, "over-cap must fail on the legacy lane");
  check(!runDirectLane(f).ok, "over-cap must fail on the direct lane");
}

void unpublishedHandleFailsLegacyLane() {
  Fixture f;
  f.name = "texture bound but never published";
  f.shadow.pendingTextureMask = 0x1u;
  f.bindings.textures[0] =
      unpublishedRef(&tex1, D9C_CHUNK_HANDLE_KIND_TEXTURE);
  // The legacy lane resolves packet handles through lookupCachedWireObjectRef
  // and must fail. The direct lane holds the wrapper ref and can legitimately
  // succeed, so this asserts the observed asymmetry rather than assuming the
  // lanes agree: only conditions BOTH lanes can see are required to match.
  check(!runLegacyLane(f).ok,
        "an unpublished handle must fail the legacy lane's cache lookup");
}

void fullSnapshotMode() {
  Fixture f;
  f.name = "forced full snapshot";
  f.forceFullSnapshot = true;
  f.shadow.renderStateShadow.set(3u, 9u);
  for (std::uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_TEXTURES; ++i) {
    f.bindings.textures[i] =
        publishedRef(&tex0, D9C_CHUNK_HANDLE_KIND_TEXTURE);
  }
  for (std::uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_STREAMS; ++i) {
    f.bindings.streams[i].buffer =
        publishedRef(&vb1, D9C_CHUNK_HANDLE_KIND_BUFFER);
  }
  f.bindings.vs = publishedRef(&vsObj, D9C_CHUNK_HANDLE_KIND_SHADER);
  f.bindings.ps = publishedRef(&psObj, D9C_CHUNK_HANDLE_KIND_SHADER);
  for (std::uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_RENDER_TARGETS; ++i) {
    f.bindings.renderTargets[i] =
        publishedRef(&rt0, D9C_CHUNK_HANDLE_KIND_SURFACE);
  }
  f.bindings.depthStencil = publishedRef(&ds0, D9C_CHUNK_HANDLE_KIND_SURFACE);
  requireLanesAgree(f);
}

// The FULL_SNAPSHOT draw flag is derived inside the shim being deleted, from
// all-ones texture and stream masks, which means it also fires in delta mode
// whenever both masks happen to be full. Only an all-dirty delta exercises
// that, and the new producer has to own the heuristic explicitly.
void allSlotsDirtyTriggersSnapshotFlagHeuristic() {
  Fixture f;
  f.name = "all texture and stream slots dirty (snapshot flag heuristic)";
  f.shadow.pendingTextureMask = (1u << D9C_DRAW_PACKET_MAX_TEXTURES) - 1u;
  f.shadow.pendingStreamMask = (1u << D9C_DRAW_PACKET_MAX_STREAMS) - 1u;
  for (std::uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_TEXTURES; ++i) {
    f.bindings.textures[i] =
        publishedRef(&tex1, D9C_CHUNK_HANDLE_KIND_TEXTURE);
  }
  for (std::uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_STREAMS; ++i) {
    f.bindings.streams[i].buffer =
        publishedRef(&vb0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  }
  requireLanesAgree(f);
}


// --- gaps the Tasks 5-7 review found in this corpus ------------------------

// FVF-only. Nothing exercised the FVF wire kind before, so a producer emitting
// DECLARATION here went unnoticed. Runs AFTER a declaration build on purpose:
// the scratch is shared, so a stale vdecl ref left in the reused entry makes
// vertexInputPrepare reject the record (`return !object.object;`).
void fvfOnlyAfterDeclaration() {
  {
    Fixture decl;
    decl.name = "vertex declaration bound (primes the shared scratch)";
    decl.shadow.pendingVdecl = true;
    decl.bindings.vdecl =
        publishedRef(&vdecl0, D9C_CHUNK_HANDLE_KIND_VERTEX_DECL);
    requireLanesAgree(decl);
  }
  Fixture f;
  f.name = "FVF only, after a declaration build (stale-scratch guard)";
  f.shadow.pendingFvf = true;
  f.bindings.fvf = 0x1C4u;
  requireLanesAgree(f);
}

// Snapshot mode selects the FULL shadow for the TSS / sampler / transform
// tables. Only renderStateShadow was populated before, so dropping the snapshot
// source on the other three was undetectable.
void snapshotDrainsEveryTable() {
  Fixture f;
  f.name = "snapshot drains tss, sampler, transform and light shadows";
  f.forceFullSnapshot = true;
  f.shadow.renderStateShadow.set(3u, 9u);
  f.shadow.tssShadow.set(0u, 1u, 11u);
  f.shadow.tssShadow.set(2u, 4u, 22u);
  f.shadow.samplerStateShadow.set(1u, 2u, 33u);
  // FixedTransformTable::set takes a STATE, not a slot: 0 and 1 are not valid
  // D3DTRANSFORMSTATETYPE values, so slotForState rejects them and the set is a
  // silent no-op. Use the real mirrored constants (VIEW=2, PROJECTION=3).
  f.shadow.transformShadow.set(kD3dTsView, identityTransformMatrix());
  f.shadow.transformShadow.set(kD3dTsProjection, identityTransformMatrix());
  // lightEnableShadow is the snapshot source; pendingLightEnableMask is the
  // delta source. Make them DIFFER so reading the wrong one is visible.
  f.shadow.lightEnableShadow = 0x5u;
  f.shadow.pendingLightEnableMask = 0xAu;
  f.bindings.vdecl = publishedRef(&vdecl0, D9C_CHUNK_HANDLE_KIND_VERTEX_DECL);
  for (std::uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_TEXTURES; ++i) {
    f.bindings.textures[i] =
        publishedRef(&tex0, D9C_CHUNK_HANDLE_KIND_TEXTURE);
  }
  for (std::uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_STREAMS; ++i) {
    f.bindings.streams[i].buffer =
        publishedRef(&vb1, D9C_CHUNK_HANDLE_KIND_BUFFER);
  }
  f.bindings.vs = publishedRef(&vsObj, D9C_CHUNK_HANDLE_KIND_SHADER);
  f.bindings.ps = publishedRef(&psObj, D9C_CHUNK_HANDLE_KIND_SHADER);
  for (std::uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_RENDER_TARGETS; ++i) {
    f.bindings.renderTargets[i] =
        publishedRef(&rt0, D9C_CHUNK_HANDLE_KIND_SURFACE);
  }
  f.bindings.depthStencil = publishedRef(&ds0, D9C_CHUNK_HANDLE_KIND_SURFACE);
  requireLanesAgree(f);
}

// Distinct objects per slot, so the builder cannot dedupe them to one handle
// and the per-slot handle mapping is actually exercised.
void distinctObjectPerTextureSlot() {
  Fixture f;
  f.name = "four texture slots with four distinct objects";
  static D9CTexture slotTex[4];
  f.shadow.pendingTextureMask = 0xFu;
  for (std::uint32_t i = 0; i < 4u; ++i) {
    f.bindings.textures[i] =
        publishedRef(&slotTex[i], D9C_CHUNK_HANDLE_KIND_TEXTURE);
  }
  requireLanesAgree(f);
}


// --- draw records, chunk context, and inline constants ---------------------

Fixture baseDraw(const char* name, std::uint32_t recordType) {
  Fixture f;
  f.name = name;
  f.params.recordType = recordType;
  f.params.primitiveType = 4u;  // D3DPT_TRIANGLELIST
  f.params.primitiveCount = 12u;
  f.params.stride = 32u;
  f.shadow.pendingStreamMask = 0x1u;
  f.bindings.streams[0].buffer = publishedRef(&vb0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  f.bindings.streams[0].offset = 64u;
  f.bindings.streams[0].stride = 32u;
  f.bindings.vs = publishedRef(&vsObj, D9C_CHUNK_HANDLE_KIND_SHADER);
  f.bindings.ps = publishedRef(&psObj, D9C_CHUNK_HANDLE_KIND_SHADER);
  return f;
}

void nonIndexedDraw() {
  Fixture f = baseDraw("non-indexed draw", D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  f.params.startVertex = 32u;
  requireLanesAgree(f);
}

void indexedDrawWithBaseVertex() {
  Fixture f = baseDraw("indexed draw, base vertex and index range",
                       D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE);
  f.params.baseVertex = 64;
  f.params.minVertex = 8u;
  f.params.numVertices = 256u;
  f.params.startIndex = 12u;
  f.shadow.pendingIb = true;
  f.bindings.indexBuffer = publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  requireLanesAgree(f);
}

void indexedDrawNegativeBaseVertex() {
  Fixture f = baseDraw("indexed draw, negative base vertex",
                       D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE);
  f.params.baseVertex = -32;  // int32_t on the wire; must not wrap
  f.shadow.pendingIb = true;
  f.bindings.indexBuffer = publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  requireLanesAgree(f);
}

// H2: a stream that is BOUND but neither dirty nor retained must be re-emitted,
// or the destination chunk never retains the buffer.
void streamBoundNotDirtyNotRetained() {
  Fixture f = baseDraw("stream bound, not dirty, not retained: must re-emit",
                       D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  f.shadow.pendingStreamMask = 0u;   // not dirty
  f.chunk.retainedStreamMask = 0u;   // not retained
  requireLanesAgree(f);
}

void streamBoundNotDirtyButRetained() {
  Fixture f = baseDraw("stream bound, not dirty, already retained: no re-emit",
                       D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  f.shadow.pendingStreamMask = 0u;
  f.chunk.retainedStreamMask = 0x1u;
  requireLanesAgree(f);
}

void streamDirtyAndRetained() {
  Fixture f = baseDraw("stream dirty and retained: dirty still wins",
                       D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  f.shadow.pendingStreamMask = 0x1u;
  f.chunk.retainedStreamMask = 0x1u;
  requireLanesAgree(f);
}

void multipleStreamsMixedRetention() {
  Fixture f = baseDraw("four streams, mixed dirty/retained",
                       D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  static D9CBuffer slotVb[4];
  f.shadow.pendingStreamMask = 0x5u;   // slots 0 and 2 dirty
  f.chunk.retainedStreamMask = 0x3u;   // slots 0 and 1 retained
  for (std::uint32_t i = 0; i < 4u; ++i) {
    f.bindings.streams[i].buffer =
        publishedRef(&slotVb[i], D9C_CHUNK_HANDLE_KIND_BUFFER);
    f.bindings.streams[i].offset = 16u * i;
    f.bindings.streams[i].stride = 32u;
  }
  requireLanesAgree(f);
}

// H1: the retention leg. Known, unchanged, but NOT retained by this chunk --
// the section must still be emitted or the chunk never retains the buffer.
void indexBufferKnownUnchangedNotRetained() {
  Fixture f = baseDraw("IB known and unchanged but not retained: must re-emit",
                       D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE);
  f.bindings.indexBuffer = publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  f.shadow.pendingIb = false;
  f.chunk.indexBufferKnown = true;
  f.chunk.submittedIndexBufferWire =
      d9cWireHandleValue(toWireHandle(f.bindings.indexBuffer.object));
  f.chunk.indexBufferRetained = false;
  requireLanesAgree(f);
}

void indexBufferKnownUnchangedAndRetained() {
  Fixture f = baseDraw("IB known, unchanged and retained: no re-emit",
                       D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE);
  f.bindings.indexBuffer = publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  f.shadow.pendingIb = false;
  f.chunk.indexBufferKnown = true;
  f.chunk.submittedIndexBufferWire =
      d9cWireHandleValue(toWireHandle(f.bindings.indexBuffer.object));
  f.chunk.indexBufferRetained = true;
  requireLanesAgree(f);
}

void indexBufferChanged() {
  Fixture f = baseDraw("IB known but changed: must re-emit",
                       D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE);
  f.bindings.indexBuffer = publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  f.shadow.pendingIb = false;
  f.chunk.indexBufferKnown = true;
  f.chunk.submittedIndexBufferWire =
      d9cWireHandleValue(toWireHandle(&ib1));  // a different buffer
  f.chunk.indexBufferRetained = true;
  requireLanesAgree(f);
}

void indexBufferUnknown() {
  Fixture f = baseDraw("IB not yet known to the chunk: must emit",
                       D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE);
  f.bindings.indexBuffer = publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  f.shadow.pendingIb = false;
  f.chunk.indexBufferKnown = false;
  requireLanesAgree(f);
}

// The four emit legs overlap, so a fixture that leaves several true cannot tell
// which one fired: dropping the not-known leg survived the corpus until this
// case isolated it. Here pendingIb is false and the buffer IS retained, so
// not-known is the ONLY reason to emit.
void indexBufferUnknownButRetained() {
  Fixture f = baseDraw("IB not known but already retained: not-known leg alone",
                       D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE);
  f.bindings.indexBuffer = publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  f.shadow.pendingIb = false;
  f.chunk.indexBufferKnown = false;
  f.chunk.submittedIndexBufferWire =
      d9cWireHandleValue(toWireHandle(f.bindings.indexBuffer.object));
  f.chunk.indexBufferRetained = true;
  requireLanesAgree(f);
}

// Mirror image: pendingIb alone. Known, unchanged, retained -- only the dirty
// bit justifies the section.
void indexBufferPendingOnly() {
  Fixture f = baseDraw("IB dirty only: pendingIb leg alone",
                       D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE);
  f.bindings.indexBuffer = publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  f.shadow.pendingIb = true;
  f.chunk.indexBufferKnown = true;
  f.chunk.submittedIndexBufferWire =
      d9cWireHandleValue(toWireHandle(f.bindings.indexBuffer.object));
  f.chunk.indexBufferRetained = true;
  requireLanesAgree(f);
}

// H3: inline-const delta. Each range dirty with a distinct start/count so a
// producer that swaps two kinds, or mis-slices, is visible.
void inlineConstDeltaAllSixRanges() {
  Fixture f = baseDraw("inline const delta, all six ranges dirty",
                       D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE);
  f.inlineConstDelta = true;
  f.shadow.pendingIb = true;
  f.bindings.indexBuffer = publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);

  struct Spec { ConstShadow* shadow; std::size_t elemSize; std::uint32_t regs; };
  const Spec specs[6] = {
      {&f.constants.vsConstF, 16u, 256u}, {&f.constants.vsConstI, 16u, 16u},
      {&f.constants.vsConstB, 4u, 16u},   {&f.constants.psConstF, 16u, 224u},
      {&f.constants.psConstI, 16u, 16u},  {&f.constants.psConstB, 4u, 16u},
  };
  std::uint32_t seed = 1u;
  for (const auto& spec : specs) {
    spec.shadow->values.assign(spec.regs * spec.elemSize, 0u);
    spec.shadow->dirtyElems.assign(spec.regs, 0u);
    for (std::size_t i = 0; i < spec.shadow->values.size(); ++i) {
      spec.shadow->values[i] = static_cast<std::uint8_t>((i * seed) & 0xFFu);
    }
    // Distinct dirty window per kind.
    spec.shadow->dirtyStart = seed % 3u;
    spec.shadow->dirtyEnd = spec.shadow->dirtyStart + 2u + (seed % 2u);
    ++seed;
  }
  requireLanesAgree(f);
}

void inlineConstDeltaSingleRange() {
  Fixture f = baseDraw("inline const delta, only vs float dirty",
                       D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  f.inlineConstDelta = true;
  auto& sh = f.constants.vsConstF;
  sh.values.assign(256u * 16u, 0u);
  sh.dirtyElems.assign(256u, 0u);
  for (std::size_t i = 0; i < sh.values.size(); ++i) {
    sh.values[i] = static_cast<std::uint8_t>(i & 0xFFu);
  }
  sh.dirtyStart = 5u;
  sh.dirtyEnd = 9u;
  requireLanesAgree(f);
}

// --- fixed-seed randomized corpus -----------------------------------------

// --- UP draw records -------------------------------------------------------
//
// UP draws carry geometry inline instead of binding app buffers. The legacy call
// sites therefore ran no chunk-dependency checkpoint, and the shim leaves its
// `indexed` pointer null for both UP record types -- so an indexed-UP record
// carries NO index-buffer section no matter how dirty the IB shadow is. These
// fixtures pin that, and the pendingIb one is the case that would otherwise let
// the producer emit a section production never emitted.

alignas(16) std::array<std::byte, 96> upVertexBytes{};
alignas(16) std::array<std::byte, 24> upIndexBytes{};

Fixture baseUpDraw(const char* name, std::uint32_t recordType) {
  Fixture f;
  f.name = name;
  f.params.recordType = recordType;
  f.params.primitiveType = 4u;  // D3DPT_TRIANGLELIST
  f.params.primitiveCount = 4u;
  f.params.stride = 24u;
  for (std::size_t i = 0; i < upVertexBytes.size(); ++i) {
    upVertexBytes[i] = static_cast<std::byte>(0x40u + (i & 0x3fu));
  }
  for (std::size_t i = 0; i < upIndexBytes.size(); ++i) {
    upIndexBytes[i] = static_cast<std::byte>(i);
  }
  f.payloads.upVertex = std::span<const std::byte>(upVertexBytes);
  // UP draws bind no vertex buffer, so the shadow carries no dirty stream.
  f.bindings.vs = publishedRef(&vsObj, D9C_CHUNK_HANDLE_KIND_SHADER);
  f.bindings.ps = publishedRef(&psObj, D9C_CHUNK_HANDLE_KIND_SHADER);
  f.shadow.pendingVs = true;
  f.shadow.pendingPs = true;
  return f;
}

void upDrawCarriesVertexPayload() {
  Fixture f = baseUpDraw("UP draw, inline vertex payload",
                         D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP);
  requireLanesAgree(f);
}

void upDrawWithDirtyState() {
  Fixture f = baseUpDraw("UP draw, inline payload plus dirty render state",
                         D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP);
  f.shadow.pendingRenderStates.set(7u, 3u);
  requireLanesAgree(f);
}

void upIndexedDrawCarriesBothPayloads() {
  Fixture f = baseUpDraw("indexed UP draw, inline index and vertex payloads",
                         D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP);
  f.params.minVertex = 2u;
  f.params.numVertices = 4u;
  f.params.indexFormat = 101u;  // D3DFMT_INDEX16
  f.payloads.upIndex = std::span<const std::byte>(upIndexBytes);
  requireLanesAgree(f);
}

// The pin that matters: a dirty IB shadow must NOT produce an index-buffer
// section on an indexed-UP record, because the shim never produced one.
void upIndexedDrawIgnoresDirtyIndexBuffer() {
  Fixture f = baseUpDraw("indexed UP draw ignores a dirty index buffer",
                         D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP);
  f.params.minVertex = 2u;
  f.params.numVertices = 4u;
  f.params.indexFormat = 101u;
  f.payloads.upIndex = std::span<const std::byte>(upIndexBytes);
  f.shadow.pendingIb = true;
  f.bindings.indexBuffer = publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  // Every chunk-context reason to emit is true as well, so nothing but the
  // record type can suppress the section.
  f.chunk.indexBufferKnown = false;
  f.chunk.indexBufferRetained = false;
  requireLanesAgree(f);
}

void upDrawUnderFullSnapshot() {
  Fixture f = baseUpDraw("UP draw under forceFullSnapshot",
                         D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP);
  f.forceFullSnapshot = true;
  requireLanesAgree(f);
}

void upIndexedDrawUnderFullSnapshot() {
  Fixture f = baseUpDraw("indexed UP draw under forceFullSnapshot",
                         D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP);
  f.params.minVertex = 2u;
  f.params.numVertices = 4u;
  f.params.indexFormat = 101u;
  f.payloads.upIndex = std::span<const std::byte>(upIndexBytes);
  f.forceFullSnapshot = true;
  requireLanesAgree(f);
}

void randomizedApplyStateSequences() {
  std::mt19937 rng(0xD9C0DEu);  // pinned: CI must be deterministic
  for (int iteration = 0; iteration < 256; ++iteration) {
    Fixture f;
    f.name = "randomized applystate " + std::to_string(iteration);
    const std::uint32_t stateCount =
        rng() % D9C_DRAW_PACKET_MAX_RENDER_STATES;
    for (std::uint32_t k = 0; k < stateCount; ++k) {
      f.shadow.pendingRenderStates.set(
          rng() % D9C_DRAW_PACKET_MAX_RENDER_STATES, rng());
    }
    f.shadow.pendingTextureMask =
        rng() & ((1u << D9C_DRAW_PACKET_MAX_TEXTURES) - 1u);
    f.shadow.pendingStreamMask =
        rng() & ((1u << D9C_DRAW_PACKET_MAX_STREAMS) - 1u);
    f.shadow.pendingVs = (rng() & 1u) != 0u;
    f.shadow.pendingPs = (rng() & 1u) != 0u;
    // Independently: either, both, or neither -- this is what exercises the
    // FVF wire kind and the declaration-wins rule.
    f.shadow.pendingVdecl = (rng() & 1u) != 0u;
    f.shadow.pendingFvf = (rng() & 1u) != 0u;
    f.bindings.vdecl = publishedRef(&vdecl0, D9C_CHUNK_HANDLE_KIND_VERTEX_DECL);
    f.shadow.lightEnableShadow = rng() & 0xFFu;
    f.params.recordType = D9C_COMMAND_RECORD_APPLY_STATE;
    f.shadow.pendingViewport = (rng() & 1u) != 0u;
    f.shadow.pendingScissor = (rng() & 1u) != 0u;
    f.shadow.pendingMaterial = (rng() & 1u) != 0u;
    f.shadow.pendingRtMask = rng() & 0xFu;
    f.shadow.pendingDs = (rng() & 1u) != 0u;
    f.shadow.pendingClipPlaneMask = rng() & 0x3Fu;
    f.shadow.pendingLightSlotMask = rng() & 0xFFu;
    f.shadow.pendingLightEnableValidMask = rng() & 0xFFu;
    f.shadow.pendingLightEnableMask = rng() & 0xFFu;
    for (std::uint32_t k = 0; k < D9C_DRAW_PACKET_MAX_TEXTURES; ++k) {
      f.bindings.textures[k] =
          publishedRef(&tex0, D9C_CHUNK_HANDLE_KIND_TEXTURE);
    }
    for (std::uint32_t k = 0; k < D9C_DRAW_PACKET_MAX_STREAMS; ++k) {
      f.bindings.streams[k].buffer =
          publishedRef(&vb0, D9C_CHUNK_HANDLE_KIND_BUFFER);
      f.bindings.streams[k].offset = rng() % 1024u;
      f.bindings.streams[k].stride = 4u * (1u + rng() % 16u);
    }
    f.bindings.vs = publishedRef(&vsObj, D9C_CHUNK_HANDLE_KIND_SHADER);
    f.bindings.ps = publishedRef(&psObj, D9C_CHUNK_HANDLE_KIND_SHADER);
    for (std::uint32_t k = 0; k < D9C_DRAW_PACKET_MAX_RENDER_TARGETS; ++k) {
      f.bindings.renderTargets[k] =
          publishedRef(&rt0, D9C_CHUNK_HANDLE_KIND_SURFACE);
    }
    f.bindings.depthStencil = publishedRef(&ds0, D9C_CHUNK_HANDLE_KIND_SURFACE);
    f.bindings.fvf = rng();
    requireLanesAgree(f);
  }
}

}  // namespace

int main() {
  try {
    emptyDelta();
    singleRenderStateDirty();
    scalarCategoriesDirty();
    everyCategoryDirty();
    attachmentsDirty();
    renderStatesAtCap();
    renderStatesOverCapFailBothLanes();
    unpublishedHandleFailsLegacyLane();
    fullSnapshotMode();
    allSlotsDirtyTriggersSnapshotFlagHeuristic();
    fvfOnlyAfterDeclaration();
    snapshotDrainsEveryTable();
    distinctObjectPerTextureSlot();
    nonIndexedDraw();
    indexedDrawWithBaseVertex();
    indexedDrawNegativeBaseVertex();
    streamBoundNotDirtyNotRetained();
    streamBoundNotDirtyButRetained();
    streamDirtyAndRetained();
    multipleStreamsMixedRetention();
    indexBufferKnownUnchangedNotRetained();
    indexBufferKnownUnchangedAndRetained();
    indexBufferChanged();
    indexBufferUnknown();
    indexBufferUnknownButRetained();
    indexBufferPendingOnly();
    inlineConstDeltaAllSixRanges();
    inlineConstDeltaSingleRange();
    upDrawCarriesVertexPayload();
    upDrawWithDirtyState();
    upIndexedDrawCarriesBothPayloads();
    upIndexedDrawIgnoresDirtyIndexBuffer();
    upDrawUnderFullSnapshot();
    upIndexedDrawUnderFullSnapshot();
    randomizedApplyStateSequences();
  } catch (const TestFailure& failure) {
    std::cerr << "pe_producer_differential_spec FAILED: " << failure.what()
              << "\n";
    return 1;
  }
  std::cout << "pe_producer_differential_spec OK\n";
  return 0;
}
