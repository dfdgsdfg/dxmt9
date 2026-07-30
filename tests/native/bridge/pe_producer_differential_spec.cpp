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
#include "d3d9_pe_producer_views.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
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
  pe::PeSparseScratch scratch{};
  pe::SparseStateV2Input state{};
  D9CCommandChunkWireDrawHeaderV2 header{};
  if (!pe::buildSparseStateV2(fixture.shadow, constants, fixture.bindings,
                              fixture.payloads, fixture.params,
                              fixture.forceFullSnapshot, scratch, header,
                              state)) {
    return LaneResult{};
  }
  const bool ok = pe::appendApplyStateV2(builder, header.flags, state);
  return finishLane(builder, ok);
}

void requireLanesAgree(const Fixture& f) {
  const LaneResult legacy = runLegacyLane(f);
  const LaneResult direct = runDirectLane(f);
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
  check(std::memcmp(legacy.bytes.data(), direct.bytes.data(),
                    legacy.bytes.size()) == 0,
        f.name + ": chunk bytes must be identical");
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
  f.shadow.pendingTransforms.set(0u, identityTransformMatrix());
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

// --- fixed-seed randomized corpus -----------------------------------------

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
    f.shadow.pendingVdecl = (rng() & 1u) != 0u;
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
    randomizedApplyStateSequences();
  } catch (const TestFailure& failure) {
    std::cerr << "pe_producer_differential_spec FAILED: " << failure.what()
              << "\n";
    return 1;
  }
  std::cout << "pe_producer_differential_spec OK\n";
  return 0;
}
