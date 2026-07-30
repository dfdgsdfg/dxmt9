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
#include "d3d9_pe_producer_views.hpp"
#include "d3d9_pe_wire_handle.hpp"

#include <array>
#include <fstream>
#include <map>
#include <sstream>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

bool isUpRecord(std::uint32_t type) {
  return type == D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP ||
         type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP;
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
                                   fixture.bindings, fixture.params,
                                   fixture.forceFullSnapshot, scratch, state)) {
    return LaneResult{};
  }
  const bool ok =
      pe::appendSparseRecordV2(builder, fixture.params.recordType, header,
                               state);
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

// --- goldens ---------------------------------------------------------------
//
// The legacy lane is going away with the shim (Task 10 stage C). Its value is a
// property -- "the sparse producer emits what the legacy format did" -- and a
// property cannot survive the deletion of one of its two sides. So each fixture's
// direct-lane output is also pinned to a golden, and the goldens are captured
// while BOTH lanes still run and agree. That certification is what carries the
// old-vs-new property forward: after stage C these goldens are no longer
// "current behaviour, snapshotted", they are "the legacy-equivalent bytes, as
// verified on the commit that captured them".
//
// Regenerate deliberately with DXMT9_UPDATE_PE_PRODUCER_GOLDEN=1. Doing that
// after the legacy lane is gone re-baselines against whatever the producer does
// today and silently discards the certification -- so once stage C lands, a
// golden diff is a finding, not a chore.
struct Golden {
  std::size_t bytes = 0;
  std::size_t records = 0;
  std::size_t handles = 0;
  std::size_t payload = 0;
  std::size_t retained = 0;
  std::uint64_t hash = 0;
  bool ok = false;
};

// FNV-1a 64. Not a security hash -- it only has to make a changed record byte
// change the digest, and it keeps the golden file diffable and dependency-free.
std::uint64_t hashBytes(const std::vector<std::byte>& bytes) {
  std::uint64_t h = 0xcbf29ce484222325ull;
  for (const std::byte b : bytes) {
    h ^= static_cast<std::uint64_t>(b);
    h *= 0x100000001b3ull;
  }
  return h;
}

Golden goldenOf(const LaneResult& r) {
  return Golden{r.bytes.size(), r.recordCount,  r.handleCount,
                r.payloadBytes, r.retainedObjectCount,
                hashBytes(r.bytes), r.ok};
}

const char* goldenPath() { return DXMT9_PE_GOLDEN_PATH; }

bool updatingGoldens() {
  const char* v = std::getenv("DXMT9_UPDATE_PE_PRODUCER_GOLDEN");
  return v && v[0] != '\0' && std::strcmp(v, "0") != 0;
}

std::map<std::string, Golden>& goldens() {
  static std::map<std::string, Golden> table;
  return table;
}

void loadGoldens() {
  std::ifstream in(goldenPath());
  if (!in) {
    return;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    // name is last so it may contain spaces; the six numerics precede it.
    std::istringstream ls(line);
    Golden g{};
    int okFlag = 0;
    if (!(ls >> okFlag >> g.bytes >> g.records >> g.handles >> g.payload >>
          g.retained >> std::hex >> g.hash >> std::dec)) {
      continue;
    }
    g.ok = okFlag != 0;
    std::string name;
    std::getline(ls, name);
    if (!name.empty() && name[0] == ' ') {
      name.erase(0, 1);
    }
    goldens()[name] = g;
  }
}

std::vector<std::pair<std::string, Golden>>& capturedGoldens() {
  static std::vector<std::pair<std::string, Golden>> captured;
  return captured;
}

void writeGoldens() {
  std::ofstream out(goldenPath(), std::ios::trunc);
  if (!out) {
    throw TestFailure(std::string("cannot write goldens to ") + goldenPath());
  }
  out << "# pe_producer_differential goldens: the sparse producer's emitted "
         "chunk per fixture.\n"
      << "# Captured while the legacy lane still ran and agreed, so these are "
         "the\n"
      << "# legacy-equivalent bytes -- see the Golden comment in the spec "
         "before\n"
      << "# regenerating. Columns: ok bytes records handles payload retained "
         "hash name\n";
  for (const auto& [name, g] : capturedGoldens()) {
    out << (g.ok ? 1 : 0) << ' ' << g.bytes << ' ' << g.records << ' '
        << g.handles << ' ' << g.payload << ' ' << g.retained << ' '
        << std::hex << g.hash << std::dec << ' ' << name << '\n';
  }
}

void requireMatchesGolden(const std::string& name, const LaneResult& direct) {
  const Golden actual = goldenOf(direct);
  if (updatingGoldens()) {
    capturedGoldens().emplace_back(name, actual);
    return;
  }
  const auto it = goldens().find(name);
  check(it != goldens().end(),
        name + ": no golden. Fixtures must be pinned; regenerate with "
               "DXMT9_UPDATE_PE_PRODUCER_GOLDEN=1 and review the diff.");
  const Golden& want = it->second;
  const auto shape = [](const Golden& g) {
    std::ostringstream os;
    os << "ok=" << (g.ok ? 1 : 0) << " bytes=" << g.bytes
       << " records=" << g.records << " handles=" << g.handles
       << " payload=" << g.payload << " retained=" << g.retained << " hash="
       << std::hex << g.hash;
    return os.str();
  };
  check(want.ok == actual.ok && want.bytes == actual.bytes &&
            want.records == actual.records && want.handles == actual.handles &&
            want.payload == actual.payload &&
            want.retained == actual.retained && want.hash == actual.hash,
        name + ": producer output does not match its golden\n    golden: " +
            shape(want) + "\n    actual: " + shape(actual));
}

// The legacy lane is gone with the shim (Task 10 stage C), so this no longer
// compares two producers -- it pins one against goldens that a passing
// differential certified as legacy-equivalent at capture time (`043d101a`).
// The name stays: the corpus, the fixtures, and the property they encode are the
// same, and renaming it would detach the goldens from the history that gives them
// their meaning.
void requirePinnedOutput(const Fixture& f) {
  const bool draw = f.params.recordType != D9C_COMMAND_RECORD_APPLY_STATE &&
                    f.params.recordType != 0u;
  const bool up = isUpRecord(f.params.recordType);
  const LaneResult direct = up ? runDirectUpLane(f)
                               : (draw ? runDirectDrawLane(f)
                                       : runDirectLane(f));
  requireMatchesGolden(f.name, direct);
}

void emptyDelta() {
  Fixture f;
  f.name = "empty delta";
  requirePinnedOutput(f);
}

void singleRenderStateDirty() {
  Fixture f;
  f.name = "one render state dirty";
  f.shadow.pendingRenderStates.set(7u, 1u);
  requirePinnedOutput(f);
}

void scalarCategoriesDirty() {
  Fixture f;
  f.name = "viewport, scissor and material dirty";
  f.shadow.pendingViewport = true;
  f.shadow.pendingScissor = true;
  f.shadow.pendingMaterial = true;
  f.shadow.viewportShadow = D9CViewport{0u, 0u, 640u, 480u, 0.0f, 1.0f};
  f.shadow.scissorShadow = D9CRect{1, 2, 3, 4};
  requirePinnedOutput(f);
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
  requirePinnedOutput(f);
}

void attachmentsDirty() {
  Fixture f;
  f.name = "render target and depth stencil dirty";
  f.shadow.pendingRtMask = 0x1u;
  f.shadow.pendingDs = true;
  f.bindings.renderTargets[0] =
      publishedRef(&rt0, D9C_CHUNK_HANDLE_KIND_SURFACE);
  f.bindings.depthStencil = publishedRef(&ds0, D9C_CHUNK_HANDLE_KIND_SURFACE);
  requirePinnedOutput(f);
}

void renderStatesAtCap() {
  Fixture f;
  f.name = "render states at the section cap";
  for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_RENDER_STATES;
       ++slot) {
    f.shadow.pendingRenderStates.set(slot, slot + 1u);
  }
  requirePinnedOutput(f);
}

void renderStatesOverCapFails() {
  Fixture f;
  f.name = "render states over cap";
  // The pending table holds kPeRenderStateSlots (256) slots while the V2
  // section cap is D9C_DRAW_PACKET_MAX_RENDER_STATES (64), so 65 distinct sets
  // really do over-fill rather than being silently dropped by the table.
  for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_RENDER_STATES + 1u;
       ++slot) {
    f.shadow.pendingRenderStates.set(slot, slot + 1u);
  }
  // Over-cap must be refused, never silently truncated -- a truncated section
  // would drop render states the app set.
  check(!runDirectLane(f).ok, "over-cap must fail");
}

// This used to assert an asymmetry: the legacy lane resolved packet handles
// through lookupCachedWireObjectRef and failed on an unpublished ref, while the
// direct lane holds the wrapper and can succeed. With the legacy lane gone, only
// the surviving half is assertable -- so pin that half explicitly rather than
// deleting the case and losing the record that the producer tolerates a bound
// but unpublished texture.
void unpublishedHandleStillEncodes() {
  Fixture f;
  f.name = "texture bound but never published";
  f.shadow.pendingTextureMask = 0x1u;
  f.bindings.textures[0] =
      unpublishedRef(&tex1, D9C_CHUNK_HANDLE_KIND_TEXTURE);
  // Deliberately NOT goldened. The legacy lane failed on this input by design, so
  // it never produced bytes here and there is no legacy-equivalent sequence to
  // certify. A golden captured now would be indistinguishable from the 291
  // certified ones while carrying none of their authority, which is worse than no
  // golden at all. Assert the property that survives: it encodes.
  check(runDirectLane(f).ok,
        "the producer holds the wrapper ref and must still encode an "
        "unpublished handle");
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
  requirePinnedOutput(f);
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
  requirePinnedOutput(f);
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
    requirePinnedOutput(decl);
  }
  Fixture f;
  f.name = "FVF only, after a declaration build (stale-scratch guard)";
  f.shadow.pendingFvf = true;
  f.bindings.fvf = 0x1C4u;
  requirePinnedOutput(f);
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
  requirePinnedOutput(f);
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
  requirePinnedOutput(f);
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
  requirePinnedOutput(f);
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
  requirePinnedOutput(f);
}

void indexedDrawNegativeBaseVertex() {
  Fixture f = baseDraw("indexed draw, negative base vertex",
                       D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE);
  f.params.baseVertex = -32;  // int32_t on the wire; must not wrap
  f.shadow.pendingIb = true;
  f.bindings.indexBuffer = publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  requirePinnedOutput(f);
}

// H2: a stream that is BOUND but neither dirty nor retained must be re-emitted,
// or the destination chunk never retains the buffer.
void streamBoundNotDirtyNotRetained() {
  Fixture f = baseDraw("stream bound, not dirty, not retained: must re-emit",
                       D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  f.shadow.pendingStreamMask = 0u;   // not dirty
  f.chunk.retainedStreamMask = 0u;   // not retained
  requirePinnedOutput(f);
}

void streamBoundNotDirtyButRetained() {
  Fixture f = baseDraw("stream bound, not dirty, already retained: no re-emit",
                       D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  f.shadow.pendingStreamMask = 0u;
  f.chunk.retainedStreamMask = 0x1u;
  requirePinnedOutput(f);
}

void streamDirtyAndRetained() {
  Fixture f = baseDraw("stream dirty and retained: dirty still wins",
                       D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  f.shadow.pendingStreamMask = 0x1u;
  f.chunk.retainedStreamMask = 0x1u;
  requirePinnedOutput(f);
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
  requirePinnedOutput(f);
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
  requirePinnedOutput(f);
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
  requirePinnedOutput(f);
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
  requirePinnedOutput(f);
}

void indexBufferUnknown() {
  Fixture f = baseDraw("IB not yet known to the chunk: must emit",
                       D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE);
  f.bindings.indexBuffer = publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  f.shadow.pendingIb = false;
  f.chunk.indexBufferKnown = false;
  requirePinnedOutput(f);
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
  requirePinnedOutput(f);
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
  requirePinnedOutput(f);
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
  requirePinnedOutput(f);
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
  requirePinnedOutput(f);
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
  requirePinnedOutput(f);
}

void upDrawWithDirtyState() {
  Fixture f = baseUpDraw("UP draw, inline payload plus dirty render state",
                         D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP);
  f.shadow.pendingRenderStates.set(7u, 3u);
  requirePinnedOutput(f);
}

void upIndexedDrawCarriesBothPayloads() {
  Fixture f = baseUpDraw("indexed UP draw, inline index and vertex payloads",
                         D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP);
  f.params.minVertex = 2u;
  f.params.numVertices = 4u;
  f.params.indexFormat = 101u;  // D3DFMT_INDEX16
  f.payloads.upIndex = std::span<const std::byte>(upIndexBytes);
  requirePinnedOutput(f);
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
  requirePinnedOutput(f);
}

void upDrawUnderFullSnapshot() {
  Fixture f = baseUpDraw("UP draw under forceFullSnapshot",
                         D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP);
  f.forceFullSnapshot = true;
  requirePinnedOutput(f);
}

void upIndexedDrawUnderFullSnapshot() {
  Fixture f = baseUpDraw("indexed UP draw under forceFullSnapshot",
                         D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP);
  f.params.minVertex = 2u;
  f.params.numVertices = 4u;
  f.params.indexFormat = 101u;
  f.payloads.upIndex = std::span<const std::byte>(upIndexBytes);
  f.forceFullSnapshot = true;
  requirePinnedOutput(f);
}

// --- draw records under forceFullSnapshot ----------------------------------
//
// The composition no named fixture covered before: a non-UP draw (so the chunk
// context step runs) in snapshot mode (so buildSparseStateV2 emitted all 16
// stream sections including null unbinds). Legacy's
// populateDrawPacketStreamDependencies only ever ADDED mask bits, so an all-ones
// snapshot mask survived it untouched.
void snapshotDrawKeepsEveryStreamSection() {
  Fixture f = baseDraw("draw under snapshot keeps every stream section",
                       D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  f.forceFullSnapshot = true;
  // Bound, retained by the destination chunk, and NOT dirty: the exact slot the
  // context step's rebuild would drop.
  f.shadow.pendingStreamMask = 0u;
  f.chunk.retainedStreamMask = 0x1u;
  requirePinnedOutput(f);
}

void snapshotIndexedDrawKeepsEveryStreamSection() {
  Fixture f = baseDraw("indexed draw under snapshot keeps every stream section",
                       D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE);
  f.forceFullSnapshot = true;
  f.params.minVertex = 4u;
  f.params.numVertices = 64u;
  f.shadow.pendingStreamMask = 0u;
  f.chunk.retainedStreamMask = 0x1u;
  f.bindings.indexBuffer = publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  f.chunk.indexBufferKnown = false;
  requirePinnedOutput(f);
}

// Randomized corpus over EVERY migrated record type, not just APPLY_STATE.
// The record type, the chunk context, and forceFullSnapshot are all drawn from
// the rng, because the defects this migration actually produced lived in the
// cross-product -- an unstamped record type, and a snapshot draw whose chunk
// context rebuilt the stream set -- and no single-axis fixture reaches those.
void randomizedRecordSequences() {
  static const std::uint32_t kRecordTypes[] = {
      D9C_COMMAND_RECORD_APPLY_STATE,
      D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
      D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
      D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
      D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
  };
  std::mt19937 rng(0xD9C0DEu);  // pinned: CI must be deterministic
  for (int iteration = 0; iteration < 256; ++iteration) {
    Fixture f;
    const std::uint32_t recordType =
        kRecordTypes[rng() % (sizeof(kRecordTypes) / sizeof(kRecordTypes[0]))];
    f.name = "randomized record " + std::to_string(iteration) + " type " +
             std::to_string(recordType);
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
    f.params.recordType = recordType;
    f.forceFullSnapshot = (rng() & 1u) != 0u;
    // Draw scalars. Kept small and valid so primitiveVertexCount and the byte
    // math stay in range; the point of the loop is state/context breadth.
    f.params.primitiveType = 4u;  // D3DPT_TRIANGLELIST
    f.params.primitiveCount = 1u + rng() % 8u;
    f.params.startVertex = rng() % 64u;
    f.params.baseVertex = static_cast<std::int32_t>(rng() % 128u) - 64;
    f.params.minVertex = rng() % 16u;
    f.params.numVertices = 1u + rng() % 64u;
    f.params.startIndex = rng() % 32u;
    f.params.stride = 4u * (1u + rng() % 8u);
    f.params.indexFormat = (rng() & 1u) ? 101u : 102u;  // INDEX16 / INDEX32
    // Chunk context: every leg of the retention predicates, independently.
    f.chunk.retainedStreamMask =
        rng() & ((1u << D9C_DRAW_PACKET_MAX_STREAMS) - 1u);
    f.chunk.indexBufferKnown = (rng() & 1u) != 0u;
    f.chunk.indexBufferRetained = (rng() & 1u) != 0u;
    // Half the time claim the SAME index buffer the bindings hold, so the
    // "known and unchanged" leg actually fires rather than always differing.
    f.chunk.submittedIndexBufferWire =
        (rng() & 1u) ? d9cWireHandleValue(toWireHandle(&ib0)) : (rng() | 1u);
    f.shadow.pendingIb = (rng() & 1u) != 0u;
    f.bindings.indexBuffer = publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
    if (isUpRecord(recordType)) {
      // UP records carry geometry inline. Vary the payload sizes, including
      // empty, and keep them consistent for both lanes.
      f.payloads.upVertex = std::span<const std::byte>(
          upVertexBytes.data(), (rng() % (upVertexBytes.size() + 1u)) & ~3u);
      if (recordType == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP) {
        f.payloads.upIndex = std::span<const std::byte>(
            upIndexBytes.data(), (rng() % (upIndexBytes.size() + 1u)) & ~3u);
      }
    }
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
    requirePinnedOutput(f);
  }
}

}  // namespace

int main() {
  try {
    loadGoldens();
    emptyDelta();
    singleRenderStateDirty();
    scalarCategoriesDirty();
    everyCategoryDirty();
    attachmentsDirty();
    renderStatesAtCap();
    renderStatesOverCapFails();
    unpublishedHandleStillEncodes();
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
    snapshotDrawKeepsEveryStreamSection();
    snapshotIndexedDrawKeepsEveryStreamSection();
    randomizedRecordSequences();
    if (updatingGoldens()) {
      writeGoldens();
      std::cout << "pe_producer_differential_spec: wrote "
                << capturedGoldens().size() << " goldens to " << goldenPath()
                << "\n";
      return 0;
    }
  } catch (const TestFailure& failure) {
    std::cerr << "pe_producer_differential_spec FAILED: " << failure.what()
              << "\n";
    return 1;
  }
  std::cout << "pe_producer_differential_spec OK\n";
  return 0;
}
