// pe_producer_differential_spec
//
// Runs the REAL old and new producers over one corpus and requires that they
// agree on the emitted canonical chunk bytes and on the builder side effects that
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

#include "d3d9_pe_chunk_builder.hpp"
#include "d3d9_pe_producer.hpp"
#include "d3d9_pe_const_shadow.hpp"
#include "d3d9_pe_producer_views.hpp"
#include "d3d9_pe_sparse_iteration.hpp"
#include "d3d9_pe_semantic_owner.hpp"
#include "d3d9_pe_wire_handle.hpp"

#include <array>
#include <fstream>
#include <map>
#include <set>
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

// --- C-ABI object stubs the canonical builder's retainer links against -------------

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

struct TestRecorderDomain {
  std::recursive_mutex mutex;
  dxmt9::core::ThreadOwnershipToken owner;
  std::uint64_t epoch = 1u;
};

TestRecorderDomain& testRecorderDomain() {
  // The domain must not be a function-template static: each distinct lambda
  // type would otherwise become a different recorder on reacquisition.
  static TestRecorderDomain domain;
  return domain;
}

template <typename Fn>
bool withRecorderAccess(Fn&& fn) {
  auto& domain = testRecorderDomain();
  pe::RecorderLockGuard guard(domain.mutex, false);
  return guard.withCapability(domain.owner, domain.epoch, std::forward<Fn>(fn));
}

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
// Mirror the kind-qualified refs cached by production child wrappers.

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
template <typename Object> struct LocalRefFor;
template <> struct LocalRefFor<D9CTexture> { using type = pe::TextureRef; };
template <> struct LocalRefFor<D9CBuffer> { using type = pe::BufferRef; };
template <> struct LocalRefFor<D9CShader> { using type = pe::ShaderRef; };
template <> struct LocalRefFor<D9CSurface> { using type = pe::SurfaceRef; };
template <> struct LocalRefFor<D9CVertexDecl> {
  using type = pe::DeclarationRef;
};

template <typename Object>
using LocalRefForT = typename LocalRefFor<Object>::type;

template <typename Object>
LocalRefForT<Object> publishedRef(Object* object, std::uint32_t kind) {
  static std::vector<std::pair<Object*, LocalRefForT<Object>>> cache;
  for (const auto& entry : cache) {
    if (entry.first == object) {
      return entry.second;
    }
  }
  LocalRefForT<Object> ref{};
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
LocalRefForT<Object> unpublishedRef(Object* object, std::uint32_t kind) {
  LocalRefForT<Object> ref{};
  ref.identity = D9CWireObjectIdentity{.kind = kind,
                                       .generation = 3u,
                                       .objectId = 0xdead0000ull};
  ref.object = object;
  return ref;
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

  // The pinned differential corpus is the legacy-wire compatibility lane:
  // its stream records intentionally carry the historical omitted-frequency
  // value.  Production PeBindingView defaults to one; canonical-frequency
  // coverage below opts in to explicit indexed/instance values.
  Fixture() { bindings.streamFrequencies.fill(0u); }
};

struct LaneResult {
  bool ok = false;
  std::vector<std::byte> bytes;
  std::size_t recordCount = 0;
  std::size_t handleCount = 0;
  std::size_t payloadBytes = 0;
  std::size_t retainedObjectCount = 0;
};

Fixture baseDraw(const char* name, std::uint32_t recordType);

// Counts and retention are read BEFORE seal(); the blob is copied out because
// SealedCommandChunk::blob is a span into the builder, which is lane-local.
LaneResult finishLane(pe::CommandChunkBuilder& builder, bool ok) {
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
  pe::CommandChunkBuilder builder;
  PeConstShadowBlock constants = fixture.constants;
  pe::PeSparseScratch& scratch = sharedScratch();
  pe::SparseStateInput state{};
  D9CCommandChunkWireDrawHeader header{};
  if (!pe::buildSparseState(fixture.shadow, constants, fixture.bindings,
                              fixture.payloads, fixture.params,
                              fixture.forceFullSnapshot,
                              /*inlineConstDelta=*/false, scratch, header,
                              state)) {
    return LaneResult{};
  }
  const bool ok = pe::appendSparseRecord(
      builder, fixture.params.recordType, header, state);
  return finishLane(builder, ok);
}

LaneResult runDirectDrawLane(const Fixture& fixture) {
  pe::CommandChunkBuilder builder;
  PeConstShadowBlock constants = fixture.constants;
  pe::PeSparseScratch& scratch = sharedScratch();
  pe::SparseStateInput state{};
  D9CCommandChunkWireDrawHeader header{};
  if (!pe::buildSparseState(fixture.shadow, constants, fixture.bindings,
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
      pe::appendSparseRecord(builder, fixture.params.recordType, header,
                               state);
  return finishLane(builder, ok);
}

LaneResult runDirectLane(const Fixture& fixture) {
  pe::CommandChunkBuilder builder;
  PeConstShadowBlock constants = fixture.constants;  // lanes must not share
  pe::PeSparseScratch& scratch = sharedScratch();
  pe::SparseStateInput state{};
  D9CCommandChunkWireDrawHeader header{};
  if (!pe::buildSparseState(fixture.shadow, constants, fixture.bindings,
                              fixture.payloads, fixture.params,
                              fixture.forceFullSnapshot,
                              fixture.inlineConstDelta, scratch, header,
                              state)) {
    return LaneResult{};
  }
  const bool ok = pe::appendApplyState(builder, header.flags, state);
  return finishLane(builder, ok);
}

// New two-pass production lane. Pass 1 owns only compact selection metadata;
// pass 2 visits the source witnesses and writes final builder payload rows.
LaneResult runPlanLane(const Fixture& fixture) {
  pe::CommandChunkBuilder builder;
  PeConstShadowBlock constants = fixture.constants;
  pe::PeDrawParams params = fixture.params;
  if (params.recordType == 0u) {
    params.recordType = D9C_COMMAND_RECORD_APPLY_STATE;
  }
  pe::SparseStatePlan plan{};
  bool ok = withRecorderAccess([&](const auto& access) noexcept {
    if (!pe::buildSparseStatePlan(
            access, fixture.shadow, constants, fixture.bindings,
            fixture.payloads, params, fixture.forceFullSnapshot,
            fixture.inlineConstDelta, plan)) {
      return false;
    }
    const bool destinationDependent =
        params.recordType == D9C_COMMAND_RECORD_DRAW_PRIMITIVE ||
        params.recordType == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
    if (destinationDependent &&
        !pe::finalizeSparseStatePlanChunkContext(access, fixture.chunk, plan)) {
      return false;
    }
    const auto* const source = plan.source(access);
    return source && (params.recordType == D9C_COMMAND_RECORD_APPLY_STATE
        ? pe::appendApplyStatePlan(access, builder, source->drawFlags, plan)
        : pe::appendSparseStatePlan(access, builder, params.recordType, plan));
  });
  if (!ok) return LaneResult{};
  return finishLane(builder, ok);
}

LaneResult runOwnerPlanLane(const Fixture& fixture) {
  Fixture source = fixture;
  const auto canonicalResult = runPlanLane(fixture);
  std::recursive_mutex mutex;
  dxmt9::core::ThreadOwnershipToken ownerThread;
  std::uint64_t epoch = 1u;
  pe::RecorderLockGuard guard(mutex, false);
  pe::SparseStatePlan plan;
  pe::PeDrawParams params = source.params;
  if (params.recordType == 0u)
    params.recordType = D9C_COMMAND_RECORD_APPLY_STATE;
  bool built = false;
  const bool accessed = guard.withCapability(
      ownerThread, epoch, [&](const auto& access) noexcept {
    built = pe::buildSparseStatePlan(
        access, source.shadow, source.constants, source.bindings,
        source.payloads, params, false, source.inlineConstDelta, plan);
    if (built &&
        (params.recordType == D9C_COMMAND_RECORD_DRAW_PRIMITIVE ||
         params.recordType == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE)) {
      built = pe::finalizeSparseStatePlanChunkContext(access, source.chunk, plan);
    }
    return built;
  });
  if (!accessed || !built) {
    return {};
  }
  pe::PeSemanticBatchOwner<> owner;
  const auto producer = params.recordType == D9C_COMMAND_RECORD_APPLY_STATE
      ? pe::PeSemanticProducerKind::ApplyState
      : (params.recordType == D9C_COMMAND_RECORD_DRAW_PRIMITIVE
             ? pe::PeSemanticProducerKind::DrawPrimitive
             : pe::PeSemanticProducerKind::DrawIndexedPrimitive);
  LaneResult result;
  const auto settlement = pe::settleRecorderAppend({
      .phase = pe::AppendSettlement::Prepared,
      .appendSucceeded = true,
  });
  result.ok = guard.withCapability(
      ownerThread, epoch, [&](const auto& access) noexcept {
        return owner.tryAppendSparseStatePlan(
            access, plan, producer, params.recordType,
            D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, 1u, 1u,
            [&]() noexcept {
              const auto* const checkedSource = plan.source(access);
              return checkedSource && pe::acceptSparseStatePlan(
                  access, source.shadow, source.constants, plan, settlement,
                  nullptr, 1u, checkedSource->pendingTicket);
            });
      });
  if (!result.ok) return result;
  std::size_t wireBytes = 0u;
  if (!owner.emissionMetrics(result.handleCount, result.payloadBytes,
                             wireBytes)) return {};
  result.recordCount = owner.size();
  result.retainedObjectCount = owner.retainedCount();
  pe::PeSemanticExactFixedEmission exact;
  if (!owner.emitExactFixed(exact) || !exact.valid()) return {};
  result.bytes.assign(exact.wire.begin(), exact.wire.end());
  check(result.bytes == canonicalResult.bytes,
        source.name + ": production owner plan differs from plan builder");
  return result;
}

// The compact owner has one qualified identity/mapping authority for all
// binding occurrences. Keep this matrix small, but make each row reach the
// real SparseStatePlan -> PeSemanticBatchOwner path and compare it with the
// independent CommandChunkBuilder producer. Each row uses a fresh owner and
// plan; stateful A/B/A reuse is covered separately below.
struct BindingRefCounts {
  std::array<std::uint32_t, 10u> values{};
};

BindingRefCounts bindingRefCounts() noexcept {
  return {{tex0.refs, tex1.refs, vb0.refs, vb1.refs, ib0.refs, ib1.refs,
            vsObj.refs, psObj.refs, rt0.refs, ds0.refs}};
}

void requireBindingRefCounts(const BindingRefCounts& expected,
                             const std::string& caseName) {
  check(bindingRefCounts().values == expected.values,
        caseName + ": compact/generic lanes must release every acquired pin");
}

void productionOwnerBindingAliasDifferential() {
  // Shape 0 is a null/unbind APPLY_STATE (there is no destination-dependent
  // stream/index qualification); the remaining shapes use an indexed draw so
  // stream, index, shader, render-target, and depth rows all pass through the
  // destination finalizer as well as owner admission.
  for (std::uint32_t shape = 0u; shape < 4u; ++shape) {
    const bool indexedDraw = shape != 0u;
    Fixture f = baseDraw(
        "owner binding aliases", indexedDraw
            ? D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE
            : D9C_COMMAND_RECORD_APPLY_STATE);
    f.name = "owner binding aliases independent shape " +
             std::to_string(shape);
    f.shadow.writer().pendingTextureMask() = 0x7u;
    f.shadow.writer().pendingStreamMask() = 0x3u;
    f.shadow.writer().pendingVs() = true;
    f.shadow.writer().pendingPs() = true;
    f.shadow.writer().pendingRtMask() = 0x3u;
    f.shadow.writer().pendingDs() = true;
    f.shadow.writer().pendingIb() = indexedDraw;
    f.chunk.retainedStreamMask = 0u;
    f.chunk.indexBufferKnown = false;
    f.chunk.indexBufferRetained = false;

    const auto textureA = publishedRef(&tex0, D9C_CHUNK_HANDLE_KIND_TEXTURE);
    const auto textureB = publishedRef(&tex1, D9C_CHUNK_HANDLE_KIND_TEXTURE);
    const auto bufferA = publishedRef(&vb0, D9C_CHUNK_HANDLE_KIND_BUFFER);
    const auto bufferB = publishedRef(&vb1, D9C_CHUNK_HANDLE_KIND_BUFFER);
    const auto indexA = publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
    const auto indexB = publishedRef(&ib1, D9C_CHUNK_HANDLE_KIND_BUFFER);
    const auto shaderA = publishedRef(&vsObj, D9C_CHUNK_HANDLE_KIND_SHADER);
    const auto shaderB = publishedRef(&psObj, D9C_CHUNK_HANDLE_KIND_SHADER);
    const auto surfaceA = publishedRef(&rt0, D9C_CHUNK_HANDLE_KIND_SURFACE);
    const auto surfaceB = publishedRef(&ds0, D9C_CHUNK_HANDLE_KIND_SURFACE);

    switch (shape) {
    case 0u:
      // The pending rows remain present and must encode null bindings, not
      // be silently dropped by the compact occurrence mapper.
      f.bindings = {};
      break;
    case 1u:
      // One identity is repeated across each family and across the indexed
      // stream/index roles; this is the warm/duplicate alias shape.
      f.bindings.textures[0] = textureA;
      f.bindings.textures[1] = textureA;
      f.bindings.streams[0].buffer = bufferA;
      f.bindings.streams[1].buffer = bufferA;
      f.bindings.indexBuffer = bufferA;
      f.bindings.vs = shaderA;
      f.bindings.ps = shaderA;
      f.bindings.renderTargets[0] = surfaceA;
      f.bindings.renderTargets[1] = surfaceA;
      f.bindings.depthStencil = surfaceA;
      break;
    case 2u:
      // Distinct identities plus repeated aliases exercise exact wire bytes,
      // deduplication, and retained-object multiplicity.
      f.bindings.textures[0] = textureA;
      f.bindings.textures[1] = textureB;
      f.bindings.textures[2] = textureA;
      f.bindings.streams[0].buffer = bufferA;
      f.bindings.streams[1].buffer = bufferB;
      f.bindings.indexBuffer = indexA;
      f.bindings.vs = shaderA;
      f.bindings.ps = shaderB;
      f.bindings.renderTargets[0] = surfaceA;
      f.bindings.renderTargets[1] = surfaceB;
      f.bindings.depthStencil = surfaceA;
      break;
    default:
      // Mixed null/duplicate occurrences keep all seven family ranges live.
      // Reset the complete view so baseDraw's defaults cannot accidentally
      // turn an intended null occurrence into a real binding.
      f.bindings = {};
      f.bindings.textures[1] = textureA;
      f.bindings.textures[2] = textureA;
      f.bindings.streams[0].buffer = bufferA;
      f.bindings.indexBuffer = indexB;
      f.bindings.ps = shaderA;
      f.bindings.renderTargets[1] = surfaceB;
      f.bindings.depthStencil = surfaceB;
      break;
    }

    if (shape == 1u) {
      const auto& stream = f.bindings.streams[0].buffer;
      const auto& index = f.bindings.indexBuffer;
      check(stream.object == index.object &&
                stream.identity.kind == index.identity.kind &&
                stream.identity.generation == index.identity.generation &&
                stream.identity.objectId == index.identity.objectId,
            f.name + ": shape1 must alias one buffer across stream/index");
    }

    const auto before = bindingRefCounts();
    const auto generic = indexedDraw ? runDirectDrawLane(f) : runDirectLane(f);
    requireBindingRefCounts(
        before, f.name + ": independent builder refcount balance");
    const auto owner = runOwnerPlanLane(f);
    check(generic.ok && owner.ok && generic.bytes == owner.bytes &&
              generic.recordCount == owner.recordCount &&
              generic.handleCount == owner.handleCount &&
              generic.payloadBytes == owner.payloadBytes &&
              generic.retainedObjectCount == owner.retainedObjectCount,
          f.name +
              ": SparseStatePlan owner must match independent generic bytes, "
              "wire counts, and retained identity count");
    requireBindingRefCounts(
        before, f.name + ": owner rollback/release refcount balance");
  }
}

// Unlike the independent shape matrix above, this trace keeps one owner and
// one non-movable SparseStatePlan across A -> B -> A. A and B are different
// live wrapper objects with stable, published identities; A appears again as
// the same live identity, so this never fabricates a generation change on a
// still-live object. Each owner commit uses the real PendingDelta settlement,
// and the accumulated exact wire is compared with one generic builder.
void productionOwnerBindingAliasReusedPlanABA() {
  std::array<Fixture, 3u> steps;
  const auto configure = [](Fixture& f, const char* name, bool b) {
    f = baseDraw(name, D9C_COMMAND_RECORD_APPLY_STATE);
    f.bindings = {};
    f.shadow.writer().pendingTextureMask() = 0x3u;
    f.shadow.writer().pendingStreamMask() = 0x3u;
    f.shadow.writer().pendingVs() = true;
    f.shadow.writer().pendingPs() = true;
    f.shadow.writer().pendingRtMask() = 0x3u;
    f.shadow.writer().pendingDs() = true;
    f.bindings.textures[0] = publishedRef(
        b ? &tex1 : &tex0, D9C_CHUNK_HANDLE_KIND_TEXTURE);
    f.bindings.textures[1] = f.bindings.textures[0];
    f.bindings.streams[0].buffer = publishedRef(
        b ? &vb1 : &vb0, D9C_CHUNK_HANDLE_KIND_BUFFER);
    f.bindings.streams[1].buffer = f.bindings.streams[0].buffer;
    f.bindings.vs = publishedRef(
        b ? &psObj : &vsObj, D9C_CHUNK_HANDLE_KIND_SHADER);
    f.bindings.ps = f.bindings.vs;
    f.bindings.renderTargets[0] = publishedRef(
        b ? &ds0 : &rt0, D9C_CHUNK_HANDLE_KIND_SURFACE);
    f.bindings.renderTargets[1] = f.bindings.renderTargets[0];
    f.bindings.depthStencil = f.bindings.renderTargets[0];
  };
  configure(steps[0], "shared owner A", false);
  configure(steps[1], "shared owner B", true);
  configure(steps[2], "shared owner A again", false);

  const auto before = bindingRefCounts();
  {
    pe::CommandChunkBuilder genericBuilder;
    pe::PeSemanticBatchOwner<> owner;
    std::recursive_mutex mutex;
    dxmt9::core::ThreadOwnershipToken ownerThread;
    std::uint64_t epoch = 311u;
    pe::RecorderLockGuard guard(mutex, false);
    pe::SparseStatePlan plan;

    for (std::size_t stepIndex = 0u; stepIndex < steps.size(); ++stepIndex) {
      auto& step = steps[stepIndex];
      PeConstShadowBlock genericConstants = step.constants;
      pe::PeSparseScratch genericScratch{};
      pe::SparseStateInput genericState{};
      D9CCommandChunkWireDrawHeader genericHeader{};
      check(pe::buildSparseState(
                step.shadow, genericConstants, step.bindings, step.payloads,
                step.params, false, false, genericScratch, genericHeader,
                genericState) &&
                pe::appendApplyState(genericBuilder, genericHeader.flags,
                                     genericState),
            step.name + ": generic A/B/A builder append succeeds");

      const auto sourceOrdinal = static_cast<std::uint64_t>(stepIndex + 1u);
      const auto settlement = pe::settleRecorderAppend({
          .phase = pe::AppendSettlement::Prepared,
          .appendSucceeded = true,
      });
      check(guard.withCapability(
                ownerThread, epoch, [&](const auto& access) noexcept {
                  if (!pe::buildSparseStatePlan(
                          access, step.shadow, step.constants, step.bindings,
                          step.payloads, step.params, false, false, plan)) {
                    return false;
                  }
                  return owner.tryAppendSparseStatePlan(
                      access, plan, pe::PeSemanticProducerKind::ApplyState,
                      step.params.recordType,
                      D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, sourceOrdinal,
                      sourceOrdinal, [&]() noexcept {
                        const auto* const checkedSource = plan.source(access);
                        return checkedSource && pe::acceptSparseStatePlan(
                            access, step.shadow, step.constants, plan,
                            settlement, nullptr, sourceOrdinal,
                            checkedSource->pendingTicket);
                      });
                }),
            step.name + ": reused production plan settles successfully");
    }

    const auto generic = finishLane(genericBuilder, true);
    std::size_t ownerHandles = 0u;
    std::size_t ownerPayload = 0u;
    std::size_t ownerWireBytes = 0u;
    check(owner.emissionMetrics(ownerHandles, ownerPayload, ownerWireBytes),
          "shared owner A/B/A metrics remain exact");
    pe::PeSemanticExactFixedEmission exact;
    check(owner.emitExactFixed(exact) && exact.valid() &&
              generic.bytes.size() == exact.wireBytes &&
              generic.bytes.size() == ownerWireBytes &&
              std::equal(generic.bytes.begin(), generic.bytes.end(),
                         exact.wire.begin()) &&
              generic.recordCount == owner.size() &&
              generic.handleCount == ownerHandles &&
              generic.payloadBytes == ownerPayload &&
              generic.retainedObjectCount == owner.retainedCount(),
          "shared owner A/B/A exact wire and ownership match generic oracle");
  }
  requireBindingRefCounts(before,
                          "shared owner A/B/A final release balance");
}

// A generic control envelope historically counts foreign sparse fields even
// though its fixed wire payload does not emit draw sections. Compact quota
// specialization must not accidentally replace that count with an empty
// fixed-record layout.
void genericControlForeignSparseQuotaPreserved() {
  pe::PeSemanticBatchOwner<8u, 8u, 4096u, 8u, 2u> owner;
  const std::array<D9CCommandChunkWireRenderState, 3u> states{};
  pe::PeSemanticRecordInput input{
      .producer = pe::PeSemanticProducerKind::Clear,
      .recordType = D9C_COMMAND_RECORD_CLEAR,
      .sourceOrdinal = 1u,
      .recordOrdinal = 1u,
  };
  input.sparse.renderStates = states;
  pe::PeSemanticPreparedRecord inspection;
  check(owner.prepareAdmission(input, inspection) ==
            pe::PeSemanticAdmissionOutcome::Capacity &&
            inspection.plan.sparseCounts[0] == states.size(),
        "generic control inspection counts foreign sparse quota");
  check(!owner.admit(input) && owner.size() == 0u &&
            owner.lastAdmissionFailure() == decltype(owner)::AdmissionFailure::Capacity,
        "generic control production preserves foreign sparse capacity rejection");
  input.sparse.renderStates = std::span(states).first(2u);
  check(owner.admit(input) && owner.size() == 1u,
        "generic control production accepts the exact foreign sparse quota");
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
// today and silently discards the certification -- so a golden diff is a finding,
// not a chore.
//
// Update mode therefore FAILS the run on purpose. It writes the file and returns
// nonzero, because a run that asserted nothing must never report success: a
// leaked env var in CI or a dev shell would otherwise re-baseline the
// certification and print OK. tests/meson.build additionally pins the variable
// to "0" in the test environment, so reaching update mode requires running this
// binary directly and deliberately.
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

// Names actually checked this run. A renamed fixture fails loudly (its golden is
// missing), but a DELETED one would leave its row in the file forever with
// nothing noticing -- so the run also asserts it consumed every row it loaded.
std::set<std::string>& consumedGoldens() {
  static std::set<std::string> consumed;
  return consumed;
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
  consumedGoldens().insert(name);
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
  const LaneResult planned = runPlanLane(f);
  const auto directShape = goldenOf(direct);
  const auto planShape = goldenOf(planned);
  std::size_t firstDifference = 0u;
  while (firstDifference < direct.bytes.size() &&
         firstDifference < planned.bytes.size() &&
         direct.bytes[firstDifference] == planned.bytes[firstDifference]) {
    ++firstDifference;
  }
  check(directShape.ok == planShape.ok &&
            directShape.bytes == planShape.bytes &&
            directShape.records == planShape.records &&
            directShape.handles == planShape.handles &&
            directShape.payload == planShape.payload &&
            directShape.retained == planShape.retained &&
            directShape.hash == planShape.hash,
        f.name + ": compact plan output differs from compatibility producer " +
            "direct(bytes=" + std::to_string(directShape.bytes) +
            ", payload=" + std::to_string(directShape.payload) +
            ", handles=" + std::to_string(directShape.handles) +
            ", hash=" + std::to_string(directShape.hash) + ") plan(bytes=" +
            std::to_string(planShape.bytes) + ", payload=" +
            std::to_string(planShape.payload) + ", handles=" +
            std::to_string(planShape.handles) + ", hash=" +
            std::to_string(planShape.hash) + ", firstDifference=" +
            std::to_string(firstDifference) + ", directByte=" +
            (firstDifference < direct.bytes.size()
                 ? std::to_string(std::to_integer<unsigned>(
                       direct.bytes[firstDifference]))
                 : std::string("end")) + ", planByte=" +
            (firstDifference < planned.bytes.size()
                 ? std::to_string(std::to_integer<unsigned>(
                       planned.bytes[firstDifference]))
                 : std::string("end")) + ")");
  requireMatchesGolden(f.name, direct);
}

void emptyDelta() {
  Fixture f;
  f.name = "empty delta";
  requirePinnedOutput(f);
}

void sparseSetBitIterationOrderAndShortCircuit() {
  // The production callback is deliberately tiny, but this exhaustive
  // bounded check pins the two observable properties needed by the compact
  // producer: no inactive work and the same ascending first-failure order.
  constexpr std::array<std::uint32_t, 5u> domains = {0u, 4u, 8u, 16u, 20u};
  for (const auto domain : domains) {
    const auto limit = 1u << domain;
    for (std::uint32_t mask = 0u; mask < limit; ++mask) {
      std::vector<std::uint32_t> visited;
      check(pe::forEachSetBitAscending(
                mask, [&](std::uint32_t slot) noexcept {
                  visited.push_back(slot);
                  return true;
                }),
            "set-bit iteration accepts every bounded mask");
      std::vector<std::uint32_t> expected;
      for (std::uint32_t slot = 0u; slot < domain; ++slot)
        if ((mask & (1u << slot)) != 0u) expected.push_back(slot);
      check(visited == expected,
            "set-bit iteration preserves canonical ascending slot order");

      std::size_t calls = 0u;
      const bool completed = pe::forEachSetBitAscending(
          mask, [&](std::uint32_t) noexcept { return ++calls < 2u; });
      check(completed == (expected.size() <= 1u),
            "set-bit iteration short-circuits at the first failing slot");
      check(calls == (expected.size() < 2u ? expected.size() : 2u),
            "set-bit iteration does no work after first failure");
    }
  }
}

void singleRenderStateDirty() {
  Fixture f;
  f.name = "one render state dirty";
  f.shadow.writer().pendingRenderStatesTyped().set(renderStateSlotKey(7u), 1u);
  requirePinnedOutput(f);
}

void scalarCategoriesDirty() {
  Fixture f;
  f.name = "viewport, scissor and material dirty";
  f.shadow.writer().pendingViewport() = true;
  f.shadow.writer().pendingScissor() = true;
  f.shadow.writer().pendingMaterial() = true;
  f.shadow.writer().viewportShadow() = D9CViewport{0u, 0u, 640u, 480u, 0.0f, 1.0f};
  f.shadow.writer().scissorShadow() = D9CRect{1, 2, 3, 4};
  requirePinnedOutput(f);
}

void compactPendingMaskAndEmptyTableDifferential() {
  // Keep the reference deliberately independent of the production reducer:
  // it models the old per-slot consumer and therefore also documents the
  // required preservation of bits outside each typed domain.
  const auto reference = [](std::uint32_t pending,
                            std::uint32_t represented,
                            std::uint32_t slotCount) noexcept {
    for (std::uint32_t slot = 0u; slot < slotCount; ++slot) {
      if ((represented & (1u << slot)) != 0u) {
        pending &= ~(1u << slot);
      }
    }
    return pending;
  };
  constexpr std::array<std::uint32_t, 5> slotCounts = {4u, 6u, 8u, 16u, 20u};
  constexpr std::array<std::uint32_t, 4> highBits = {
      0u, 0x80000000u, 0x60000000u, 0xf0000000u};
  std::size_t maskCases = 0u;
  for (const auto slotCount : slotCounts) {
    for (std::uint32_t pendingLow = 0u; pendingLow < 256u; ++pendingLow) {
      for (std::uint32_t representedLow = 0u; representedLow < 256u;
           ++representedLow) {
        for (const auto pendingHigh : highBits) {
          for (const auto representedHigh : highBits) {
            const auto pending = pendingLow | pendingHigh;
            const auto represented = representedLow | representedHigh;
            const auto expected = reference(pending, represented, slotCount);
            switch (slotCount) {
            case 4u:
              check(consumeRepresentedMask<4u>(pending, represented) ==
                        expected,
                    "4-slot bulk mask reducer differs from per-slot reference");
              break;
            case 6u:
              check(consumeRepresentedMask<6u>(pending, represented) ==
                        expected,
                    "6-slot bulk mask reducer differs from per-slot reference");
              break;
            case 8u:
              check(consumeRepresentedMask<8u>(pending, represented) ==
                        expected,
                    "8-slot bulk mask reducer differs from per-slot reference");
              break;
            case 16u:
              check(consumeRepresentedMask<16u>(pending, represented) ==
                        expected,
                    "16-slot bulk mask reducer differs from per-slot reference");
              break;
            case 20u:
              check(consumeRepresentedMask<20u>(pending, represented) ==
                        expected,
                    "20-slot bulk mask reducer differs from per-slot reference");
              break;
            default:
              check(false, "unexpected typed mask domain");
            }
            ++maskCases;
          }
        }
      }
    }
  }
  check(maskCases == 5u * 256u * 256u * 16u,
        "bounded mask differential visited every planned case");

  // The Cartesian low-byte sweep above is not exhaustive over 32-bit words.
  // Check every bit's four (pending, represented) valuations as well, so the
  // upper valid texture/stream slots and each exact domain edge are covered.
  const auto bitwiseDomain = [&]<std::uint32_t Slots>() {
    for (std::uint32_t bit = 0u; bit < 32u; ++bit) {
      for (std::uint32_t p = 0u; p < 2u; ++p) {
        for (std::uint32_t r = 0u; r < 2u; ++r) {
          check(consumeRepresentedMask<Slots>(p << bit, r << bit) ==
                    reference(p << bit, r << bit, Slots),
                "every bit and typed domain edge refines per-slot clearing");
        }
      }
    }
  };
  bitwiseDomain.template operator()<4u>();
  bitwiseDomain.template operator()<6u>();
  bitwiseDomain.template operator()<8u>();
  bitwiseDomain.template operator()<16u>();
  bitwiseDomain.template operator()<20u>();
  bitwiseDomain.template operator()<0u>();
  bitwiseDomain.template operator()<32u>();

  // Pin the actual category wiring as well as the reducer: a light-enable
  // update consumes both masks, while all consumers preserve unrelated bits.
  PeHotStateShadow masks;
  for (std::uint32_t bit = 0u; bit < 32u; ++bit) {
    const std::uint32_t represented = 1u << bit;
    auto writer = masks.writer();
    writer.pendingTextureMask() = 0xffffffffu;
    writer.pendingStreamMask() = 0xffffffffu;
    writer.pendingRtMask() = 0xffffffffu;
    writer.pendingClipPlaneMask() = 0xffffffffu;
    writer.pendingLightSlotMask() = 0xffffffffu;
    writer.pendingLightEnableValidMask() = 0xffffffffu;
    writer.pendingLightEnableMask() = 0xaaaaaaaau;
    auto consumer = masks.consume();
    consumer.acceptTextureMask(represented);
    consumer.acceptStreamMask(represented);
    consumer.acceptRenderTargetMask(represented);
    consumer.acceptClipPlaneMask(represented);
    consumer.acceptLightMask(represented);
    consumer.acceptLightEnableMask(represented);
    check(masks.pendingTextureMask() == reference(
              0xffffffffu, represented, D9C_DRAW_PACKET_MAX_TEXTURES) &&
              masks.pendingStreamMask() == reference(
                  0xffffffffu, represented, D9C_DRAW_PACKET_MAX_STREAMS) &&
              masks.pendingRtMask() == reference(
                  0xffffffffu, represented, D9C_DRAW_PACKET_MAX_RENDER_TARGETS) &&
              masks.pendingClipPlaneMask() == reference(
                  0xffffffffu, represented, 6u) &&
              masks.pendingLightSlotMask() == reference(
                  0xffffffffu, represented, D9C_DRAW_PACKET_MAX_LIGHTS) &&
              masks.pendingLightEnableValidMask() == reference(
                  0xffffffffu, represented, D9C_DRAW_PACKET_MAX_LIGHTS) &&
              masks.pendingLightEnableMask() == reference(
                  0xaaaaaaaau, represented, D9C_DRAW_PACKET_MAX_LIGHTS),
          "production consumers bind the correct category and slot domain");
  }

  // The same consumer handles empty, sparse, and full typed tables.  The
  // count guards are intentionally exercised through the production-facing
  // consumer rather than by a test-only clear path.
  PeHotStateShadow empty;
  auto emptyConsumer = empty.consume();
  emptyConsumer.acceptAllRenderStates();
  emptyConsumer.acceptAllTextureStageStates();
  emptyConsumer.acceptAllSamplerStates();
  emptyConsumer.acceptAllTransforms();
  check(!empty.hasPendingHotState(),
        "empty compact settlement leaves all typed tables empty");

  PeHotStateShadow sparse;
  auto sparseWriter = sparse.writer();
  sparseWriter.pendingRenderStatesTyped().set(renderStateSlotKey(7u), 7u);
  sparseWriter.pendingTssTyped().set(textureStageIndexKey(2u),
                                      textureStageStateTypeKey(3u), 23u);
  SamplerIndex sparseSampler{};
  SamplerStateType sparseSamplerType{};
  check(samplerIndexKey(1u, sparseSampler) &&
            samplerStateTypeKey(2u, sparseSamplerType),
        "sparse compact sampler key is valid");
  sparseWriter.pendingSamplerStatesTyped().set(sparseSampler,
                                               sparseSamplerType, 12u);
  sparseWriter.pendingTransformsTyped().set(transformStateKey(kD3dTsView),
                                            identityTransformMatrix());
  auto sparseConsumer = sparse.consume();
  sparseConsumer.acceptAllRenderStates();
  sparseConsumer.acceptAllTextureStageStates();
  sparseConsumer.acceptAllSamplerStates();
  sparseConsumer.acceptAllTransforms();
  check(!sparse.hasPendingHotState(),
        "sparse compact settlement clears each represented typed table");

  PeHotStateShadow full;
  auto fullWriter = full.writer();
  for (std::uint32_t state = 0u; state < kPeRenderStateSlots; ++state) {
    fullWriter.pendingRenderStatesTyped().set(renderStateSlotKey(state), state);
  }
  for (std::uint32_t stage = 0u; stage < kPeTextureStageSlots; ++stage) {
    for (std::uint32_t type = 0u; type < kPeTextureStageStateSlots; ++type) {
      fullWriter.pendingTssTyped().set(textureStageIndexKey(stage),
                                       textureStageStateTypeKey(type),
                                       stage * 64u + type);
    }
  }
  for (std::uint32_t sampler = 0u; sampler < kPeSamplerSlots; ++sampler) {
    for (std::uint32_t type = 0u; type < kPeSamplerStateSlots; ++type) {
      SamplerIndex samplerKey{};
      SamplerStateType typeKey{};
      check(samplerIndexKey(samplerForSlot(SamplerIndex::fromRaw(sampler)),
                            samplerKey) &&
                samplerStateTypeKey(type, typeKey),
            "full compact sampler key is valid");
      fullWriter.pendingSamplerStatesTyped().set(samplerKey, typeKey,
                                                 sampler * 16u + type);
    }
  }
  fullWriter.pendingTransformsTyped().set(transformStateKey(kD3dTsView),
                                          identityTransformMatrix());
  fullWriter.pendingTransformsTyped().set(
      transformStateKey(kD3dTsProjection), identityTransformMatrix());
  for (std::uint32_t texture = kD3dTsTexture0;
       texture <= kD3dTsTexture7; ++texture) {
    fullWriter.pendingTransformsTyped().set(transformStateKey(texture),
                                            identityTransformMatrix());
  }
  for (std::uint32_t world = kD3dTsWorld;
       world < kD3dTsWorld + kPeTransformWorldSlots; ++world) {
    fullWriter.pendingTransformsTyped().set(transformStateKey(world),
                                            identityTransformMatrix());
  }
  auto fullConsumer = full.consume();
  fullConsumer.acceptAllRenderStates();
  fullConsumer.acceptAllTextureStageStates();
  fullConsumer.acceptAllSamplerStates();
  fullConsumer.acceptAllTransforms();
  check(!full.hasPendingHotState(),
        "full compact settlement clears every typed table");
}

void everyCategoryDirty() {
  Fixture f;
  f.name = "every category dirty";
  f.shadow.writer().pendingRenderStatesTyped().set(renderStateSlotKey(7u), 1u);
  f.shadow.writer().pendingTextureMask() = 0x1u;
  f.shadow.writer().pendingStreamMask() = 0x1u;
  f.shadow.writer().pendingVs() = true;
  f.shadow.writer().pendingPs() = true;
  f.shadow.writer().pendingVdecl() = true;
  f.shadow.writer().pendingViewport() = true;
  f.shadow.writer().pendingScissor() = true;
  f.shadow.writer().pendingMaterial() = true;
  f.shadow.writer().pendingClipPlaneMask() = 0x3u;
  f.shadow.writer().pendingLightSlotMask() = 0x1u;
  f.shadow.writer().pendingLightEnableValidMask() = 0x1u;
  f.shadow.writer().pendingLightEnableMask() = 0x1u;
  f.shadow.writer().pendingTssTyped().set(textureStageIndexKey(0u),
                                 textureStageStateTypeKey(1u), 42u);
  SamplerIndex sampler{};
  SamplerStateType samplerType{};
  check(samplerIndexKey(0u, sampler) && samplerStateTypeKey(1u, samplerType),
        "fixture sampler key must be valid");
  f.shadow.writer().pendingSamplerStatesTyped().set(sampler, samplerType, 7u);
  f.shadow.writer().pendingTransformsTyped().set(transformStateKey(kD3dTsView),
                                        identityTransformMatrix());
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
  f.shadow.writer().pendingRtMask() = 0x1u;
  f.shadow.writer().pendingDs() = true;
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
    f.shadow.writer().pendingRenderStatesTyped().set(renderStateSlotKey(slot),
                                            slot + 1u);
  }
  requirePinnedOutput(f);
}

void renderStatesOverCapFails() {
  Fixture f;
  f.name = "render states over cap";
  // The pending table holds kPeRenderStateSlots (256) slots while the canonical
  // section cap is D9C_DRAW_PACKET_MAX_RENDER_STATES (64), so 65 distinct sets
  // really do over-fill rather than being silently dropped by the table.
  for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_RENDER_STATES + 1u;
       ++slot) {
    f.shadow.writer().pendingRenderStatesTyped().set(renderStateSlotKey(slot),
                                            slot + 1u);
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
  f.shadow.writer().pendingTextureMask() = 0x1u;
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
  f.shadow.writer().renderStateShadowTyped().set(renderStateSlotKey(3u), 9u);
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
  f.shadow.writer().pendingTextureMask() = (1u << D9C_DRAW_PACKET_MAX_TEXTURES) - 1u;
  f.shadow.writer().pendingStreamMask() = (1u << D9C_DRAW_PACKET_MAX_STREAMS) - 1u;
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
    decl.shadow.writer().pendingVdecl() = true;
    decl.bindings.vdecl =
        publishedRef(&vdecl0, D9C_CHUNK_HANDLE_KIND_VERTEX_DECL);
    requirePinnedOutput(decl);
  }
  Fixture f;
  f.name = "FVF only, after a declaration build (stale-scratch guard)";
  f.shadow.writer().pendingFvf() = true;
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
  f.shadow.writer().renderStateShadowTyped().set(renderStateSlotKey(3u), 9u);
  f.shadow.writer().tssShadowTyped().set(textureStageIndexKey(0u),
                                textureStageStateTypeKey(1u), 11u);
  f.shadow.writer().tssShadowTyped().set(textureStageIndexKey(2u),
                                textureStageStateTypeKey(4u), 22u);
  SamplerIndex sampler{};
  SamplerStateType samplerType{};
  check(samplerIndexKey(1u, sampler) && samplerStateTypeKey(2u, samplerType),
        "fixture sampler key must be valid");
  f.shadow.writer().samplerStateShadowTyped().set(sampler, samplerType, 33u);
  // FixedTransformTable::set takes a STATE, not a slot: 0 and 1 are not valid
  // D3DTRANSFORMSTATETYPE values, so slotForState rejects them and the set is a
  // silent no-op. Use the real mirrored constants (VIEW=2, PROJECTION=3).
  f.shadow.writer().transformShadowTyped().set(transformStateKey(kD3dTsView),
                                      identityTransformMatrix());
  f.shadow.writer().transformShadowTyped().set(transformStateKey(kD3dTsProjection),
                                      identityTransformMatrix());
  // lightEnableShadow is the snapshot source; pendingLightEnableMask is the
  // delta source. Make them DIFFER so reading the wrong one is visible.
  f.shadow.writer().lightEnableShadow() = 0x5u;
  f.shadow.writer().pendingLightEnableMask() = 0xAu;
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
  f.shadow.writer().pendingTextureMask() = 0xFu;
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
  f.shadow.writer().pendingStreamMask() = 0x1u;
  f.bindings.streams[0].buffer = publishedRef(&vb0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  f.bindings.streams[0].offset = 64u;
  f.bindings.streams[0].stride = 32u;
  f.bindings.vs = publishedRef(&vsObj, D9C_CHUNK_HANDLE_KIND_SHADER);
  f.bindings.ps = publishedRef(&psObj, D9C_CHUNK_HANDLE_KIND_SHADER);
  return f;
}

void canonicalStreamFrequenciesReachEveryProducerLane() {
  constexpr std::uint32_t indexedData = 0x40000000u;
  constexpr std::uint32_t instanceData = 0x80000000u;
  Fixture fixture = baseDraw("canonical stream frequencies",
                             D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  static D9CBuffer streamOne;
  fixture.shadow.writer().pendingStreamMask() = 0x3u;
  fixture.bindings.streams[1].buffer =
      publishedRef(&streamOne, D9C_CHUNK_HANDLE_KIND_BUFFER);
  fixture.bindings.streams[1].offset = 128u;
  fixture.bindings.streams[1].stride = 48u;
  fixture.bindings.streamFrequencies[0] = indexedData | 7u;
  fixture.bindings.streamFrequencies[1] = instanceData | 3u;

  auto checkState = [&](bool fullSnapshot) {
    PeConstShadowBlock constants = fixture.constants;
    pe::PeSparseScratch scratch{};
    pe::SparseStateInput state{};
    D9CCommandChunkWireDrawHeader header{};
    check(pe::buildSparseState(
              fixture.shadow, constants, fixture.bindings, fixture.payloads,
              fixture.params, fullSnapshot, false, scratch, header, state),
          "canonical stream frequency state builds");
    for (const auto& stream : state.streams) {
      check(stream.wire.frequency ==
                fixture.bindings.streamFrequencies[stream.wire.slot],
            "canonical stream frequency reaches sparse wire state");
    }
    const auto before = fixture.bindings.streamFrequencies;
    fixture.bindings.streams[0].offset += 16u;
    fixture.bindings.streams[0].stride += 4u;
    PeConstShadowBlock reboundConstants = fixture.constants;
    pe::PeSparseScratch reboundScratch{};
    pe::SparseStateInput reboundState{};
    D9CCommandChunkWireDrawHeader reboundHeader{};
    check(pe::buildSparseState(
              fixture.shadow, reboundConstants, fixture.bindings,
              fixture.payloads, fixture.params, fullSnapshot, false,
              reboundScratch, reboundHeader, reboundState),
          "tuple rebind preserves producer build");
    check(fixture.bindings.streamFrequencies == before,
          "stream tuple rebind leaves canonical frequencies unchanged");
  };

  checkState(false);
  fixture.forceFullSnapshot = true;
  checkState(true);

  fixture.forceFullSnapshot = false;
  const LaneResult direct = runDirectDrawLane(fixture);
  const LaneResult compact = runPlanLane(fixture);
  const LaneResult semantic = runOwnerPlanLane(fixture);
  check(direct.ok && compact.ok && semantic.ok,
        "frequency fixture reaches direct, compact, and semantic-owner lanes");
  check(direct.bytes == compact.bytes && direct.bytes == semantic.bytes,
        "frequency fixture emits identical compact and semantic-owner bytes");
}

void nonIndexedDraw() {
  Fixture f = baseDraw("non-indexed draw", D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  f.params.startVertex = 32u;
  requirePinnedOutput(f);
}

void indexedDrawWithBaseVertex() {
  Fixture f = baseDraw("indexed draw, base vertex and index range",
                       D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE);
  check(f.bindings.streamFrequencies[0] == 0u,
        "legacy indexed fixture must retain explicit zero frequency");
  f.params.baseVertex = 64;
  f.params.minVertex = 8u;
  f.params.numVertices = 256u;
  f.params.startIndex = 12u;
  f.shadow.writer().pendingIb() = true;
  f.bindings.indexBuffer = publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  requirePinnedOutput(f);
}

void indexedDrawNegativeBaseVertex() {
  Fixture f = baseDraw("indexed draw, negative base vertex",
                       D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE);
  f.params.baseVertex = -32;  // int32_t on the wire; must not wrap
  f.shadow.writer().pendingIb() = true;
  f.bindings.indexBuffer = publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  requirePinnedOutput(f);
}

// H2: a stream that is BOUND but neither dirty nor retained must be re-emitted,
// or the destination chunk never retains the buffer.
void streamBoundNotDirtyNotRetained() {
  Fixture f = baseDraw("stream bound, not dirty, not retained: must re-emit",
                       D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  f.shadow.writer().pendingStreamMask() = 0u;   // not dirty
  f.chunk.retainedStreamMask = 0u;   // not retained
  requirePinnedOutput(f);
}

void streamBoundNotDirtyButRetained() {
  Fixture f = baseDraw("stream bound, not dirty, already retained: no re-emit",
                       D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  f.shadow.writer().pendingStreamMask() = 0u;
  f.chunk.retainedStreamMask = 0x1u;
  requirePinnedOutput(f);
}

void streamDirtyAndRetained() {
  Fixture f = baseDraw("stream dirty and retained: dirty still wins",
                       D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  f.shadow.writer().pendingStreamMask() = 0x1u;
  f.chunk.retainedStreamMask = 0x1u;
  requirePinnedOutput(f);
}

void multipleStreamsMixedRetention() {
  Fixture f = baseDraw("four streams, mixed dirty/retained",
                       D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  static D9CBuffer slotVb[4];
  f.shadow.writer().pendingStreamMask() = 0x5u;   // slots 0 and 2 dirty
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
  f.shadow.writer().pendingIb() = false;
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
  f.shadow.writer().pendingIb() = false;
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
  f.shadow.writer().pendingIb() = false;
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
  f.shadow.writer().pendingIb() = false;
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
  f.shadow.writer().pendingIb() = false;
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
  f.shadow.writer().pendingIb() = true;
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
  f.shadow.writer().pendingIb() = true;
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

void productionOwnerPlanAllCategoryDifferential() {
  Fixture f = baseDraw("production owner compact plan all-category",
                       D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE);
  f.inlineConstDelta = true;
  f.params.baseVertex = -3;
  f.params.minVertex = 2u;
  f.params.numVertices = 33u;
  f.params.startIndex = 5u;
  f.shadow.writer().pendingRenderStatesTyped().set(
      renderStateSlotKey(7u), 0x12345678u);
  f.shadow.writer().pendingTextureMask() = 0x7u;
  f.shadow.writer().pendingStreamMask() = 0x3u;
  f.shadow.writer().pendingVs() = true;
  f.shadow.writer().pendingPs() = true;
  f.shadow.writer().pendingVdecl() = true;
  f.shadow.writer().pendingIb() = true;
  f.shadow.writer().pendingRtMask() = 0x3u;
  f.shadow.writer().pendingDs() = true;
  f.shadow.writer().pendingViewport() = true;
  f.shadow.writer().pendingScissor() = true;
  f.shadow.writer().pendingMaterial() = true;
  f.shadow.writer().pendingClipPlaneMask() = 0x3u;
  f.shadow.writer().pendingLightSlotMask() = 0x3u;
  f.shadow.writer().pendingLightEnableValidMask() = 0x3u;
  f.shadow.writer().pendingLightEnableMask() = 0x1u;
  f.shadow.writer().pendingTssTyped().set(
      textureStageIndexKey(1u), textureStageStateTypeKey(2u), 0x77u);
  SamplerIndex sampler{};
  SamplerStateType samplerType{};
  check(samplerIndexKey(1u, sampler) &&
            samplerStateTypeKey(2u, samplerType),
        "all-category sampler key is valid");
  f.shadow.writer().pendingSamplerStatesTyped().set(
      sampler, samplerType, 0x88u);
  f.shadow.writer().pendingTransformsTyped().set(
      transformStateKey(kD3dTsView), identityTransformMatrix());
  f.shadow.writer().viewportShadow() =
      D9CViewport{3u, 4u, 800u, 600u, 0.1f, 0.9f};
  f.shadow.writer().scissorShadow() = D9CRect{5, 6, 700, 500};
  f.shadow.writer().materialShadow().diffuse =
      D9CColorRGBA{0.2f, 0.4f, 0.6f, 0.8f};
  f.shadow.writer().materialShadow().power = 17.0f;
  f.shadow.writer().clipPlaneShadow()[0] = 1.0f;
  f.shadow.writer().clipPlaneShadow()[5] = 2.0f;
  f.shadow.writer().lightShadow()[0].type = 1u;
  f.shadow.writer().lightShadow()[0].diffuse =
      D9CColorRGBA{0.1f, 0.3f, 0.5f, 0.7f};
  f.shadow.writer().lightShadow()[1].range = 123.0f;
  f.bindings.textures[0] =
      publishedRef(&tex0, D9C_CHUNK_HANDLE_KIND_TEXTURE);
  f.bindings.textures[1] = f.bindings.textures[0];
  f.bindings.textures[2] = {};
  f.bindings.streams[0].buffer =
      publishedRef(&vb0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  f.bindings.streams[1].buffer = f.bindings.streams[0].buffer;
  f.bindings.streams[1].offset = 128u;
  f.bindings.streams[1].stride = 16u;
  f.bindings.vs = publishedRef(&vsObj, D9C_CHUNK_HANDLE_KIND_SHADER);
  f.bindings.ps = publishedRef(&psObj, D9C_CHUNK_HANDLE_KIND_SHADER);
  f.bindings.vdecl =
      publishedRef(&vdecl0, D9C_CHUNK_HANDLE_KIND_VERTEX_DECL);
  f.bindings.indexBuffer =
      publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  f.bindings.renderTargets[0] =
      publishedRef(&rt0, D9C_CHUNK_HANDLE_KIND_SURFACE);
  f.bindings.renderTargets[1] = f.bindings.renderTargets[0];
  f.bindings.depthStencil =
      publishedRef(&ds0, D9C_CHUNK_HANDLE_KIND_SURFACE);

  struct ConstantSpec {
    ConstShadow* shadow;
    std::size_t elementBytes;
    std::uint32_t registers;
  };
  const ConstantSpec constants[] = {
      {&f.constants.vsConstF, 16u, 256u},
      {&f.constants.vsConstI, 16u, 16u},
      {&f.constants.vsConstB, 4u, 16u},
      {&f.constants.psConstF, 16u, 224u},
      {&f.constants.psConstI, 16u, 16u},
      {&f.constants.psConstB, 4u, 16u},
  };
  std::uint32_t seed = 3u;
  for (const auto& constant : constants) {
    constant.shadow->values.assign(
        constant.registers * constant.elementBytes,
        static_cast<std::uint8_t>(seed));
    constant.shadow->dirtyElems.assign(constant.registers, 0u);
    constant.shadow->dirtyStart = seed % 2u;
    constant.shadow->dirtyEnd = constant.shadow->dirtyStart + 1u;
    ++seed;
  }
  const auto direct = runDirectDrawLane(f);
  const auto planned = runPlanLane(f);
  const auto owned = runOwnerPlanLane(f);
  check(direct.ok && planned.ok && owned.ok &&
            direct.bytes == planned.bytes && planned.bytes == owned.bytes &&
            direct.recordCount == owned.recordCount &&
            direct.handleCount == owned.handleCount &&
            direct.payloadBytes == owned.payloadBytes &&
            direct.retainedObjectCount == owned.retainedObjectCount,
        "production owner compact plan equals direct and builder goldens");
}

// R-CORE-REC-7.61: the producer-issued constant-range witness is one bit for the
// whole six-family set: an inline plan with no selected range must not pay the
// settlement loop, while an inline plan with any selected range must validate
// and clear exactly the represented family rows. Exercise every bounded
// presence mask through the real plan/finalize/owner/accept path. The direct
// and plan-builder lanes remain the byte references; this test only observes
// the owner transition and its source facts.
struct InlineConstantFamilySpec {
  ConstShadow PeConstShadowBlock::*shadow;
  std::size_t elementBytes;
};

constexpr std::array<InlineConstantFamilySpec, 6u>
    kInlineConstantFamilySpecs = {{
        {&PeConstShadowBlock::vsConstF, 16u},
        {&PeConstShadowBlock::vsConstI, 16u},
        {&PeConstShadowBlock::vsConstB, 4u},
        {&PeConstShadowBlock::psConstF, 16u},
        {&PeConstShadowBlock::psConstI, 16u},
        {&PeConstShadowBlock::psConstB, 4u},
    }};

ConstShadow& inlineConstantFamily(PeConstShadowBlock& constants,
                                  std::size_t family) noexcept {
  return constants.*kInlineConstantFamilySpecs[family].shadow;
}

const pe::SparseConstantRangeInput& inlineConstantSourceFamily(
    const pe::SparseStatePlan::SourceFacts& source,
    std::size_t family) noexcept {
  switch (family) {
  case 0u: return source.vsFloatConstants;
  case 1u: return source.vsIntConstants;
  case 2u: return source.vsBoolConstants;
  case 3u: return source.psFloatConstants;
  case 4u: return source.psIntConstants;
  case 5u: return source.psBoolConstants;
  }
  return source.vsFloatConstants;
}

Fixture inlineConstantMaskFixture(bool inlineConstDelta,
                                  std::uint32_t mask) {
  Fixture f = baseDraw("inline constant range mask",
                       D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  f.inlineConstDelta = inlineConstDelta;
  f.shadow.writer().pendingRenderStatesTyped().set(
      renderStateSlotKey(7u), 0x7000u + mask);
  for (std::size_t family = 0u;
       family < kInlineConstantFamilySpecs.size(); ++family) {
    if ((mask & (1u << family)) == 0u) continue;
    auto& shadow = inlineConstantFamily(f.constants, family);
    const auto elementBytes = kInlineConstantFamilySpecs[family].elementBytes;
    // Keep two elements materialized so stale-range rejection can move the
    // live dirty interval from [0,1) to [1,2) without invalidating the
    // producer's borrowed source span.
    shadow.values.assign(elementBytes * 2u,
                         static_cast<std::uint8_t>(0x20u + family));
    shadow.dirtyElems = {1u, 0u};
    shadow.dirtyStart = 0u;
    shadow.dirtyEnd = 1u;
  }
  return f;
}

void productionOwnerInlineConstantRangeMaskMatrix() {
  std::recursive_mutex mutex;
  dxmt9::core::ThreadOwnershipToken ownerThread;
  std::uint64_t epoch = 31u;
  pe::RecorderLockGuard guard(mutex, false);
  std::size_t cases = 0u;
  for (const bool inlineConstDelta : {false, true}) {
    for (std::uint32_t mask = 0u; mask < 64u; ++mask) {
      Fixture f = inlineConstantMaskFixture(inlineConstDelta, mask);
      const auto direct = runDirectDrawLane(f);
      const auto planned = runPlanLane(f);
      check(direct.ok && planned.ok && direct.bytes == planned.bytes,
            "six-family mask reference lanes agree");

      pe::SparseStatePlan plan;
      bool hasRanges = false;
      std::array<bool, 6u> sourcePresence{};
      check(guard.withCapability(
                ownerThread, epoch, [&](const auto& access) noexcept {
                  if (!pe::buildSparseStatePlan(
                          access, f.shadow, f.constants, f.bindings,
                          f.payloads, f.params, false, inlineConstDelta,
                          plan) ||
                      !pe::finalizeSparseStatePlanChunkContext(
                          access, f.chunk, plan)) {
                    return false;
                  }
                  const auto* source = plan.source(access);
                  if (!source) return false;
                  hasRanges = source->hasConstantRanges;
                  for (std::size_t family = 0u; family < 6u; ++family) {
                    sourcePresence[family] =
                        inlineConstantSourceFamily(*source, family).present();
                  }
                  return true;
                }),
            "six-family mask plan prepares and finalizes");
      const bool expectedHasRanges = inlineConstDelta && mask != 0u;
      check(hasRanges == expectedHasRanges,
            "hasConstantRanges follows inline mode and selected mask");
      for (std::size_t family = 0u; family < 6u; ++family) {
        check(sourcePresence[family] ==
                  (inlineConstDelta && (mask & (1u << family)) != 0u),
              "source range presence follows represented family");
      }

      pe::PeScalarSemanticTokenLedger tokens;
      const auto render = renderStateSlotKey(7u);
      check(tokens.record(pe::ScalarSemanticCategory::RenderState,
                          rawSlot(render)),
            "mask matrix records its render-state token");
      pe::PeSemanticBatchOwner<> owner;
      const auto accepted = pe::settleRecorderAppend({
          .phase = pe::AppendSettlement::Prepared,
          .appendSucceeded = true,
      });
      check(guard.withCapability(
                ownerThread, epoch, [&](const auto& access) noexcept {
                  return owner.tryAppendSparseStatePlan(
                      access, plan,
                      pe::PeSemanticProducerKind::DrawPrimitive,
                      f.params.recordType,
                      D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, 1u, 1u,
                      [&]() noexcept {
                        const auto* source = plan.source(access);
                        return source && pe::acceptSparseStatePlan(
                            access, f.shadow, f.constants, plan, accepted,
                            &tokens, 1u, source->pendingTicket);
                      });
                }),
            "six-family mask owner accepts its prepared plan");

      pe::PeSemanticExactFixedEmission exact;
      check(owner.size() == 1u && owner.emitExactFixed(exact) &&
                exact.valid() &&
                std::vector<std::byte>(exact.wire.begin(), exact.wire.end()) ==
                    direct.bytes,
            "six-family mask owner bytes match direct reference");
      check(tokens.empty() && f.shadow.pendingRenderStatesTyped().empty(),
            "six-family mask settles scalar PendingDelta and token exactly once");
      for (std::size_t family = 0u; family < 6u; ++family) {
        const auto& shadow = inlineConstantFamily(f.constants, family);
        const bool expectedDirty = !inlineConstDelta &&
            (mask & (1u << family)) != 0u;
        check(shadow.dirty() == expectedDirty,
              "represented constant family clears while unrepresented stays dirty");
        if (!shadow.dirtyElems.empty()) {
          check(shadow.dirtyElems[0] == (expectedDirty ? 1u : 0u) &&
                    shadow.dirtyElems[1] == 0u,
                "mask matrix preserves exact per-element dirty membership");
        }
      }
      ++cases;
    }
  }
  check(cases == 128u, "six-family mask matrix covers 64 masks in both modes");
}

void productionOwnerInlineConstantPlanReuseAndRangeRetries() {
  std::recursive_mutex mutex;
  dxmt9::core::ThreadOwnershipToken ownerThread;
  std::uint64_t epoch = 37u;
  pe::RecorderLockGuard guard(mutex, false);
  pe::SparseStatePlan plan;

  const auto append = [&](Fixture& f, bool expectedHasRanges) {
    bool observedHasRanges = false;
    check(guard.withCapability(
              ownerThread, epoch, [&](const auto& access) noexcept {
                if (!pe::buildSparseStatePlan(
                        access, f.shadow, f.constants, f.bindings,
                        f.payloads, f.params, false, f.inlineConstDelta,
                        plan) ||
                    !pe::finalizeSparseStatePlanChunkContext(
                        access, f.chunk, plan)) {
                  return false;
                }
                const auto* source = plan.source(access);
                if (!source) return false;
                observedHasRanges = source->hasConstantRanges;
                return true;
              }),
          "reused plan prepares each mode");
    check(observedHasRanges == expectedHasRanges,
          "reused plan resets producer constant-range witness");
    const auto direct = runDirectDrawLane(f);
    pe::PeSemanticBatchOwner<> owner;
    const auto accepted = pe::settleRecorderAppend({
        .phase = pe::AppendSettlement::Prepared,
        .appendSucceeded = true,
    });
    check(guard.withCapability(
              ownerThread, epoch, [&](const auto& access) noexcept {
                return owner.tryAppendSparseStatePlan(
                    access, plan,
                    pe::PeSemanticProducerKind::DrawPrimitive,
                    f.params.recordType,
                    D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, 1u, 1u,
                    [&]() noexcept {
                      const auto* source = plan.source(access);
                      return source && pe::acceptSparseStatePlan(
                          access, f.shadow, f.constants, plan, accepted,
                          nullptr, 1u, source->pendingTicket);
                    });
              }),
          "reused plan owner append succeeds");
    pe::PeSemanticExactFixedEmission exact;
    check(owner.emitExactFixed(exact) && exact.valid() &&
              std::vector<std::byte>(exact.wire.begin(), exact.wire.end()) ==
                  direct.bytes,
          "reused plan bytes match reference");
  };

  Fixture first = inlineConstantMaskFixture(true, 0x15u);
  append(first, true);
  for (std::size_t family = 0u; family < 6u; ++family) {
    const auto& shadow = inlineConstantFamily(first.constants, family);
    check(!shadow.dirty(),
          "first reused plan settles only represented families");
  }
  Fixture second = inlineConstantMaskFixture(false, 0x2au);
  append(second, false);
  for (std::size_t family = 0u; family < 6u; ++family) {
    check(inlineConstantFamily(second.constants, family).dirty() ==
              ((0x2au & (1u << family)) != 0u),
          "non-inline reused plan leaves dirty constant families pending");
  }
  Fixture third = inlineConstantMaskFixture(true, 0x2au);
  append(third, true);
  for (std::size_t family = 0u; family < 6u; ++family) {
    check(!inlineConstantFamily(third.constants, family).dirty(),
          "second inline reused plan settles all represented families");
  }

  for (std::size_t family = 0u; family < 6u; ++family) {
    for (const bool clearRange : {false, true}) {
      Fixture f = inlineConstantMaskFixture(true, 1u << family);
      check(guard.withCapability(
                ownerThread, epoch, [&](const auto& access) noexcept {
                  return pe::buildSparseStatePlan(
                             access, f.shadow, f.constants, f.bindings,
                             f.payloads, f.params, false, true, plan) &&
                      pe::finalizeSparseStatePlanChunkContext(
                          access, f.chunk, plan);
                }),
            "represented family rejection plan prepares");
      auto& shadow = inlineConstantFamily(f.constants, family);
      if (clearRange) {
        shadow.clear();
      } else {
        shadow.dirtyElems = {0u, 1u};
        shadow.dirtyStart = 1u;
        shadow.dirtyEnd = 2u;
      }
      pe::PeSemanticBatchOwner<> failedOwner;
      const auto accepted = pe::settleRecorderAppend({
          .phase = pe::AppendSettlement::Prepared,
          .appendSucceeded = true,
      });
      check(!guard.withCapability(
                ownerThread, epoch, [&](const auto& access) noexcept {
                  return failedOwner.tryAppendSparseStatePlan(
                      access, plan,
                      pe::PeSemanticProducerKind::DrawPrimitive,
                      f.params.recordType,
                      D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, 1u, 1u,
                      [&]() noexcept {
                        const auto* source = plan.source(access);
                        return source && pe::acceptSparseStatePlan(
                            access, f.shadow, f.constants, plan, accepted,
                            nullptr, 1u, source->pendingTicket);
                      });
                }) &&
                failedOwner.size() == 0u &&
                f.shadow.pendingRenderStatesTyped().size() == 1u,
            "stale or cleared represented family rejects atomically");

      // Repair the current source and retry through the same plan object.
      shadow.dirtyElems = {1u, 0u};
      shadow.dirtyStart = 0u;
      shadow.dirtyEnd = 1u;
      check(guard.withCapability(
                ownerThread, epoch, [&](const auto& access) noexcept {
                  return pe::buildSparseStatePlan(
                             access, f.shadow, f.constants, f.bindings,
                             f.payloads, f.params, false, true, plan) &&
                      pe::finalizeSparseStatePlanChunkContext(
                          access, f.chunk, plan);
                }) &&
                guard.withCapability(
                    ownerThread, epoch, [&](const auto& access) noexcept {
                      return failedOwner.tryAppendSparseStatePlan(
                          access, plan,
                          pe::PeSemanticProducerKind::DrawPrimitive,
                          f.params.recordType,
                          D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, 1u, 1u,
                          [&]() noexcept {
                            const auto* source = plan.source(access);
                            return source && pe::acceptSparseStatePlan(
                                access, f.shadow, f.constants, plan, accepted,
                                nullptr, 1u, source->pendingTicket);
                          });
                    }),
            "repaired represented family retries successfully");
      check(failedOwner.size() == 1u && !shadow.dirty() &&
                f.shadow.pendingRenderStatesTyped().empty(),
            "repaired family settles exactly once");
    }
  }

  // A token mismatch is a separate source witness failure: the constant
  // range is current, but the scalar authority was recorded for another key.
  Fixture tokenFixture = inlineConstantMaskFixture(true, 1u);
  check(guard.withCapability(
            ownerThread, epoch, [&](const auto& access) noexcept {
              return pe::buildSparseStatePlan(
                         access, tokenFixture.shadow, tokenFixture.constants,
                         tokenFixture.bindings, tokenFixture.payloads,
                         tokenFixture.params, false, true, plan) &&
                  pe::finalizeSparseStatePlanChunkContext(
                      access, tokenFixture.chunk, plan);
            }),
        "token mismatch plan prepares");
  pe::PeScalarSemanticTokenLedger tokens;
  check(tokens.record(pe::ScalarSemanticCategory::RenderState,
                      rawSlot(renderStateSlotKey(8u))),
        "token mismatch records wrong scalar key");
  pe::PeSemanticBatchOwner<> tokenOwner;
  const auto accepted = pe::settleRecorderAppend({
      .phase = pe::AppendSettlement::Prepared,
      .appendSucceeded = true,
  });
  const auto wrongTicket = PendingDeltaTicket{
      plan.pendingTicket().generation + 1u};
  check(!guard.withCapability(
            ownerThread, epoch, [&](const auto& access) noexcept {
              return tokenOwner.tryAppendSparseStatePlan(
                  access, plan, pe::PeSemanticProducerKind::DrawPrimitive,
                  tokenFixture.params.recordType,
                  D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, 1u, 1u,
                  [&]() noexcept {
                    return pe::acceptSparseStatePlan(
                        access, tokenFixture.shadow, tokenFixture.constants,
                        plan, accepted, nullptr, 1u, wrongTicket);
                  });
            }) &&
            tokenOwner.size() == 0u && tokenOwner.retainedCount() == 0u &&
            tokenFixture.shadow.pendingRenderStatesTyped().size() == 1u &&
            tokenFixture.constants.vsConstF.dirty() && !tokens.empty(),
        "current plan with foreign transaction ticket rejects before settlement");
  check(!guard.withCapability(
              ownerThread, epoch, [&](const auto& access) noexcept {
                return tokenOwner.tryAppendSparseStatePlan(
                    access, plan,
                    pe::PeSemanticProducerKind::DrawPrimitive,
                    tokenFixture.params.recordType,
                    D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, 1u, 1u,
                    [&]() noexcept {
                      const auto* source = plan.source(access);
                      return source && pe::acceptSparseStatePlan(
                          access, tokenFixture.shadow, tokenFixture.constants,
                          plan, accepted, &tokens, 1u,
                          source->pendingTicket);
                    });
              }) &&
              tokenOwner.size() == 0u &&
              tokenFixture.shadow.pendingRenderStatesTyped().size() == 1u &&
              tokenFixture.constants.vsConstF.dirty(),
          "token validation failure preserves all witnesses");
  tokens.clear();
  check(tokens.record(pe::ScalarSemanticCategory::RenderState,
                      rawSlot(renderStateSlotKey(7u))) &&
            guard.withCapability(
                ownerThread, epoch, [&](const auto& access) noexcept {
                  return tokenOwner.tryAppendSparseStatePlan(
                      access, plan,
                      pe::PeSemanticProducerKind::DrawPrimitive,
                      tokenFixture.params.recordType,
                      D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, 1u, 1u,
                      [&]() noexcept {
                        const auto* source = plan.source(access);
                        return source && pe::acceptSparseStatePlan(
                            access, tokenFixture.shadow,
                            tokenFixture.constants, plan, accepted, &tokens,
                            1u, source->pendingTicket);
                      });
                }) &&
            tokenOwner.size() == 1u && tokens.empty() &&
            !tokenFixture.constants.vsConstF.dirty(),
        "corrected token retries and settles exactly once");
}

// The compact owner prepares mask-derived category counts once and must reuse
// those qualified quotas through canonical emission. Exercise the empty,
// sparse, and maximum bounded masks, then rebuild one non-movable plan A-B-A
// while checking the accumulated bytes against the independent producer. The
// final case uses the same production path with a deliberately small sparse
// budget to pin that CapacityPre rejects before acquiring any physical pin.
void productionOwnerCompactQuotaReuseAndCapacityPins() {
  const auto makeFixture = [](const char* name, std::uint32_t textures,
                              std::uint32_t streams, std::uint8_t shaders,
                              std::uint32_t renderTargets,
                              std::uint32_t clipPlanes, std::uint32_t lights,
                              std::uint32_t lightEnables) {
    Fixture f = baseDraw(name, D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE);
    f.bindings = {};
    f.shadow.writer().pendingTextureMask() = textures;
    f.shadow.writer().pendingStreamMask() = streams;
    f.shadow.writer().pendingVs() = (shaders & 1u) != 0u;
    f.shadow.writer().pendingPs() = (shaders & 2u) != 0u;
    f.shadow.writer().pendingIb() = (streams | renderTargets) != 0u;
    f.shadow.writer().pendingRtMask() = renderTargets;
    f.shadow.writer().pendingDs() = renderTargets != 0u;
    f.shadow.writer().pendingClipPlaneMask() = clipPlanes;
    f.shadow.writer().pendingLightSlotMask() = lights;
    f.shadow.writer().pendingLightEnableValidMask() = lightEnables;
    f.shadow.writer().pendingLightEnableMask() = lightEnables;
    for (std::uint32_t slot = 0u; slot < D9C_DRAW_PACKET_MAX_TEXTURES;
         ++slot) {
      if ((textures & (1u << slot)) != 0u)
        f.bindings.textures[slot] =
            publishedRef(&tex0, D9C_CHUNK_HANDLE_KIND_TEXTURE);
    }
    for (std::uint32_t slot = 0u; slot < D9C_DRAW_PACKET_MAX_STREAMS;
         ++slot) {
      if ((streams & (1u << slot)) != 0u) {
        f.bindings.streams[slot].buffer =
            publishedRef(&vb0, D9C_CHUNK_HANDLE_KIND_BUFFER);
        f.bindings.streams[slot].offset = 8u * slot;
        f.bindings.streams[slot].stride = 16u + slot;
      }
    }
    if ((shaders & 1u) != 0u)
      f.bindings.vs = publishedRef(&vsObj, D9C_CHUNK_HANDLE_KIND_SHADER);
    if ((shaders & 2u) != 0u)
      f.bindings.ps = publishedRef(&psObj, D9C_CHUNK_HANDLE_KIND_SHADER);
    for (std::uint32_t slot = 0u;
         slot < D9C_DRAW_PACKET_MAX_RENDER_TARGETS; ++slot) {
      if ((renderTargets & (1u << slot)) != 0u)
        f.bindings.renderTargets[slot] =
            publishedRef(&rt0, D9C_CHUNK_HANDLE_KIND_SURFACE);
    }
    if (f.shadow.pendingDs())
      f.bindings.depthStencil =
          publishedRef(&ds0, D9C_CHUNK_HANDLE_KIND_SURFACE);
    if (f.shadow.pendingIb())
      f.bindings.indexBuffer =
          publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
    f.chunk.indexBufferKnown = !f.shadow.pendingIb();
    f.chunk.indexBufferRetained = !f.shadow.pendingIb();
    return f;
  };
  constexpr std::uint32_t allTextures =
      (1u << D9C_DRAW_PACKET_MAX_TEXTURES) - 1u;
  constexpr std::uint32_t allStreams =
      (1u << D9C_DRAW_PACKET_MAX_STREAMS) - 1u;
  constexpr std::uint32_t allRenderTargets =
      (1u << D9C_DRAW_PACKET_MAX_RENDER_TARGETS) - 1u;
  constexpr std::uint32_t allLights = (1u << D9C_DRAW_PACKET_MAX_LIGHTS) - 1u;
  auto empty = makeFixture("compact quota empty masks", 0u, 0u, 0u, 0u, 0u,
                           0u, 0u);
  auto full = makeFixture("compact quota full masks", allTextures, allStreams,
                          3u, allRenderTargets, 0x3fu, allLights, allLights);
  auto sparse = makeFixture("compact quota sparse masks", 0x55555u, 0xaaaaU,
                            3u, 0x9u, 0x15u, 0xa5u, 0x5au);
  const auto before = bindingRefCounts();

  // Keep each bounded shape's one-record canonical equivalence explicit. The
  // sparse shape is also the reusable A side of the accumulated A-B-A trace.
  for (const Fixture* fixture : {&empty, &sparse, &full}) {
    const auto direct = runDirectDrawLane(*fixture);
    const auto owner = runOwnerPlanLane(*fixture);
    check(direct.ok && owner.ok && direct.bytes == owner.bytes &&
              direct.recordCount == owner.recordCount &&
              direct.handleCount == owner.handleCount &&
              direct.payloadBytes == owner.payloadBytes,
          std::string(fixture->name) +
              ": compact quota shape preserves canonical bytes");
    requireBindingRefCounts(
        before, std::string(fixture->name) + ": one-record pins balance");
  }

  // Keep A mutable: settlement consumes its PendingDelta, and the same
  // non-movable plan source is retried after a failed settlement below.
  std::array<Fixture, 3u> steps = {sparse, full, sparse};
  {
    pe::CommandChunkBuilder genericBuilder;
    pe::PeSemanticBatchOwner<> owner;
    std::recursive_mutex mutex;
    dxmt9::core::ThreadOwnershipToken ownerThread;
    std::uint64_t epoch = 401u;
    pe::RecorderLockGuard guard(mutex, false);
    pe::SparseStatePlan plan;
    for (std::size_t stepIndex = 0u; stepIndex < steps.size(); ++stepIndex) {
      auto& fixture = steps[stepIndex];
      PeConstShadowBlock genericConstants = fixture.constants;
      pe::PeSparseScratch scratch{};
      pe::SparseStateInput state{};
      D9CCommandChunkWireDrawHeader header{};
      check(pe::buildSparseState(
                fixture.shadow, genericConstants, fixture.bindings,
                fixture.payloads, fixture.params, false, false, scratch, header,
                state) &&
                pe::addChunkContextSections(
                    fixture.chunk, fixture.shadow, fixture.bindings,
                    fixture.params, scratch, state) &&
                pe::appendSparseRecord(
                    genericBuilder, fixture.params.recordType, header, state),
            fixture.name + ": generic A-B-A append succeeds");

      const auto sourceOrdinal = static_cast<std::uint64_t>(stepIndex + 1u);
      check(guard.withCapability(
                ownerThread, epoch, [&](const auto& access) noexcept {
                  if (!pe::buildSparseStatePlan(
                          access, fixture.shadow, fixture.constants,
                          fixture.bindings, fixture.payloads, fixture.params,
                          false, false, plan) ||
                      !pe::finalizeSparseStatePlanChunkContext(
                          access, fixture.chunk, plan))
                    return false;
                  const auto* source = plan.source(access);
                  if (source == nullptr) return false;

                  // A failed settlement must roll back all owner state and
                  // physical pins so the exact same bytes can be retried.
                  if (stepIndex == 0u) {
                    const auto beforeFailure = bindingRefCounts();
                    const auto failed = pe::settleRecorderAppend({
                        .phase = pe::AppendSettlement::Prepared,
                        .appendSucceeded = false,
                    });
                    const bool failedAppend = owner.tryAppendSparseStatePlan(
                        access, plan,
                        pe::PeSemanticProducerKind::DrawIndexedPrimitive,
                        fixture.params.recordType,
                        D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, sourceOrdinal,
                        sourceOrdinal, [&]() noexcept {
                          const auto* checkedSource = plan.source(access);
                          return checkedSource && pe::acceptSparseStatePlan(
                              access, fixture.shadow, fixture.constants, plan,
                              failed, nullptr, sourceOrdinal,
                              checkedSource->pendingTicket);
                        });
                    if (failedAppend || owner.size() != 0u ||
                        owner.retainedCount() != 0u ||
                        bindingRefCounts().values != beforeFailure.values)
                      return false;
                  }

                  const auto accepted = pe::settleRecorderAppend({
                      .phase = pe::AppendSettlement::Prepared,
                      .appendSucceeded = true,
                  });
                  return owner.tryAppendSparseStatePlan(
                      access, plan,
                      pe::PeSemanticProducerKind::DrawIndexedPrimitive,
                      fixture.params.recordType,
                      D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, sourceOrdinal,
                      sourceOrdinal, [&]() noexcept {
                        const auto* checkedSource = plan.source(access);
                        return checkedSource && pe::acceptSparseStatePlan(
                            access, fixture.shadow, fixture.constants, plan,
                            accepted, nullptr, sourceOrdinal,
                            checkedSource->pendingTicket);
                      });
                }),
            fixture.name + ": reused production plan appends");
    }
    const auto generic = finishLane(genericBuilder, true);
    std::size_t ownerHandles = 0u;
    std::size_t ownerPayload = 0u;
    std::size_t ownerWireBytes = 0u;
    check(owner.emissionMetrics(ownerHandles, ownerPayload, ownerWireBytes),
          "compact quota A-B-A metrics remain exact");
    pe::PeSemanticExactFixedEmission exact;
    check(owner.emitExactFixed(exact) && exact.valid() &&
              generic.bytes.size() == exact.wireBytes &&
              generic.bytes.size() == ownerWireBytes &&
              std::equal(generic.bytes.begin(), generic.bytes.end(),
                         exact.wire.begin()) &&
              generic.recordCount == owner.size() &&
              generic.handleCount == ownerHandles &&
              generic.payloadBytes == ownerPayload &&
              generic.retainedObjectCount == owner.retainedCount(),
          "compact quota A-B-A reuses prepared counts with exact bytes");
  }
  requireBindingRefCounts(before, "compact quota A-B-A pins balance");

  // CapacityPre runs before compact binding acquisition. A full mask is large
  // enough to exceed this owner in sparse quota while the generous remaining
  // dimensions make this specifically a sparse-capacity fixture, not a
  // generic record/payload/pin overflow.
  constexpr std::size_t sparseLimit = 1u;
  constexpr std::size_t rectLimit = 1024u;
  using SparseCapacityOwner =
      pe::PeSemanticBatchOwner<8u, 256u, 1u << 20u, rectLimit, sparseLimit>;
  SparseCapacityOwner constrained;
  std::recursive_mutex capacityMutex;
  dxmt9::core::ThreadOwnershipToken capacityOwnerThread;
  std::uint64_t capacityEpoch = 501u;
  pe::RecorderLockGuard capacityGuard(capacityMutex, false);
  pe::SparseStatePlan constrainedPlan;
  check(capacityGuard.withCapability(
            capacityOwnerThread, capacityEpoch,
            [&](const auto& access) noexcept {
              return pe::buildSparseStatePlan(
                         access, full.shadow, full.constants, full.bindings,
                         full.payloads, full.params, false, false,
                         constrainedPlan) &&
                     pe::finalizeSparseStatePlanChunkContext(
                         access, full.chunk, constrainedPlan);
            }),
        "full compact quota capacity fixture prepares");
  pe::PeSemanticProductionCompactPreparedRecord prepared;
  const bool sparseOnlyCapacity = capacityGuard.withCapability(
      capacityOwnerThread, capacityEpoch, [&](const auto& access) noexcept {
        const pe::PeSemanticSparsePlanRecordInput input{
            .access = &access,
            .sparsePlan = &constrainedPlan,
            .producer = pe::PeSemanticProducerKind::DrawIndexedPrimitive,
            .recordType = full.params.recordType,
            .recordFlags = D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
            .sourceOrdinal = 1u,
            .recordOrdinal = 1u,
        };
        const auto outcome = constrained.prepareAdmission(input, prepared);
        std::size_t pinDelta = 0u;
        for (const auto delta : prepared.retentionDeltas) pinDelta += delta;
        return outcome == pe::PeSemanticAdmissionOutcome::Capacity &&
               prepared.sparseLayout.valid &&
               prepared.sparseLayout.quotaDeltas[1u] > sparseLimit &&
               prepared.plan.payloadBytes > 0u &&
               prepared.plan.payloadBytes <
                   D9C_COMMAND_CHUNK_MAX_TOTAL_WIRE_BYTES &&
               prepared.plan.semanticBytes <=
                   SparseCapacityOwner::maxSemanticBytes &&
               prepared.plan.rectCount <= rectLimit &&
               pinDelta <= SparseCapacityOwner::maxPins;
      });
  check(sparseOnlyCapacity,
        "compact sparse capacity fixture rejects only its sparse quota");
  bool settlementCalled = false;
  const bool capacityRejected = capacityGuard.withCapability(
      capacityOwnerThread, capacityEpoch, [&](const auto& access) noexcept {
        return !constrained.tryAppendSparseStatePlan(
                   access, constrainedPlan,
                   pe::PeSemanticProducerKind::DrawIndexedPrimitive,
                   full.params.recordType,
                   D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, 1u, 1u,
                   [&]() noexcept {
                     settlementCalled = true;
                     return true;
                   }) &&
               constrained.size() == 0u && constrained.retainedCount() == 0u;
      });
  check(capacityRejected && !settlementCalled,
        "compact sparse capacity failure rejects before callbacks or pins");
  requireBindingRefCounts(before,
                          "compact sparse capacity failure pins balance");
}

void productionOwnerSelectionDifferentialAcrossFamilies() {
  // Keep this matrix intentionally small and exhaustive over the bounded
  // source choices that affect a compact plan: three producer families and
  // every pair of three-bit texture/stream masks (192 cells). Other categories
  // vary representatively, not over their full Cartesian product. The builder is
  // still the byte oracle; the prepared layout exposes the owner's exact
  // sparse quotas so a matching final blob alone cannot hide a count drift.
  constexpr std::array<std::uint32_t, 3> recordTypes = {
      D9C_COMMAND_RECORD_APPLY_STATE,
      D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
      D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
  };
  static D9CTexture textures[D9C_DRAW_PACKET_MAX_TEXTURES];
  static D9CBuffer streams[D9C_DRAW_PACKET_MAX_STREAMS];
  static D9CSurface renderTargets[D9C_DRAW_PACKET_MAX_RENDER_TARGETS];

  for (std::size_t family = 0u; family < recordTypes.size(); ++family) {
    for (std::uint32_t selection = 0u; selection < 64u; ++selection) {
      const std::size_t caseIndex = selection % 4u;
      Fixture f = baseDraw("owner source selection matrix",
                           recordTypes[family]);
      const auto textureMask = selection & 7u;
      const auto streamMask = selection >> 3u;
      f.shadow.writer().pendingTextureMask() = textureMask;
      f.shadow.writer().pendingStreamMask() = streamMask;
      f.shadow.writer().pendingVs() = (caseIndex & 1u) != 0u;
      f.shadow.writer().pendingPs() = (family == 2u);
      f.shadow.writer().pendingVdecl() = (caseIndex == 3u);
      f.shadow.writer().pendingFvf() = (caseIndex == 2u);
      f.shadow.writer().pendingRtMask() =
          static_cast<std::uint32_t>((caseIndex + family) & 0x3u);
      f.shadow.writer().pendingDs() = (caseIndex == 3u);
      f.shadow.writer().pendingViewport() = (caseIndex & 1u) != 0u;
      f.shadow.writer().pendingScissor() = (family == 1u);
      f.shadow.writer().pendingMaterial() = (caseIndex == 2u);
      f.shadow.writer().pendingClipPlaneMask() =
          static_cast<std::uint32_t>(caseIndex & 0x3u);
      f.shadow.writer().pendingLightSlotMask() =
          static_cast<std::uint32_t>(family == 2u ? 0x3u : caseIndex & 0x1u);
      f.shadow.writer().pendingLightEnableValidMask() =
          static_cast<std::uint32_t>(caseIndex & 0x3u);
      f.shadow.writer().pendingLightEnableMask() = 0x1u;
      f.shadow.writer().pendingTssTyped().set(
          textureStageIndexKey(static_cast<std::uint32_t>(caseIndex)),
          textureStageStateTypeKey(static_cast<std::uint32_t>(family)),
          0x100u + static_cast<std::uint32_t>(caseIndex));
      SamplerIndex sampler{};
      SamplerStateType samplerType{};
      check(samplerIndexKey(static_cast<std::uint32_t>(family), sampler) &&
                samplerStateTypeKey(static_cast<std::uint32_t>(caseIndex),
                                    samplerType),
            "owner selection matrix keys are valid");
      f.shadow.writer().pendingSamplerStatesTyped().set(
          sampler, samplerType, 0x200u + static_cast<std::uint32_t>(family));
      f.shadow.writer().pendingTransformsTyped().set(
          transformStateKey(kD3dTsView), identityTransformMatrix());

      for (std::uint32_t slot = 0u;
           slot < D9C_DRAW_PACKET_MAX_TEXTURES; ++slot) {
        if ((textureMask & (1u << slot)) != 0u) {
          f.bindings.textures[slot] =
              publishedRef(&textures[slot], D9C_CHUNK_HANDLE_KIND_TEXTURE);
        }
      }
      for (std::uint32_t slot = 0u;
           slot < D9C_DRAW_PACKET_MAX_STREAMS; ++slot) {
        if ((streamMask & (1u << slot)) != 0u) {
          f.bindings.streams[slot].buffer =
              publishedRef(&streams[slot], D9C_CHUNK_HANDLE_KIND_BUFFER);
          f.bindings.streams[slot].offset = 16u * slot;
          f.bindings.streams[slot].stride = 16u + slot;
        }
      }
      for (std::uint32_t slot = 0u;
           slot < D9C_DRAW_PACKET_MAX_RENDER_TARGETS; ++slot) {
        if ((f.shadow.pendingRtMask() & (1u << slot)) != 0u) {
          f.bindings.renderTargets[slot] = publishedRef(
              &renderTargets[slot], D9C_CHUNK_HANDLE_KIND_SURFACE);
        }
      }
      if (f.shadow.pendingVdecl()) {
        f.bindings.vdecl =
            publishedRef(&vdecl0, D9C_CHUNK_HANDLE_KIND_VERTEX_DECL);
      }
      if (recordTypes[family] == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE) {
        f.shadow.writer().pendingIb() = (caseIndex != 0u);
        if (f.shadow.pendingIb()) {
          f.bindings.indexBuffer =
              publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
        }
      }
      f.chunk.retainedStreamMask =
          (caseIndex == 1u) ? streamMask : 0u;
      f.chunk.indexBufferKnown = caseIndex == 3u;
      f.chunk.indexBufferRetained = caseIndex == 3u;
      if (f.bindings.indexBuffer.object != nullptr) {
        f.chunk.submittedIndexBufferWire = d9cWireHandleValue(
            toWireHandle(f.bindings.indexBuffer.object));
      }

      const auto direct = recordTypes[family] == D9C_COMMAND_RECORD_APPLY_STATE
          ? runDirectLane(f)
          : runDirectDrawLane(f);
      const auto owner = runOwnerPlanLane(f);
      check(direct.ok && owner.ok && direct.bytes == owner.bytes &&
                direct.recordCount == owner.recordCount &&
                direct.handleCount == owner.handleCount &&
                direct.payloadBytes == owner.payloadBytes &&
                direct.retainedObjectCount == owner.retainedObjectCount,
            "owner source selection matches independent builder bytes and totals");

      std::recursive_mutex mutex;
      dxmt9::core::ThreadOwnershipToken ownerThread;
      std::uint64_t epoch = 101u + family * 64u + selection;
      pe::RecorderLockGuard guard(mutex, false);
      PeConstShadowBlock constants = f.constants;
      pe::SparseStatePlan plan;
      bool prepared = false;
      const bool accessed = guard.withCapability(
          ownerThread, epoch, [&](const auto& access) noexcept {
            prepared = pe::buildSparseStatePlan(
                access, f.shadow, constants, f.bindings, f.payloads, f.params,
                false, false, plan);
            if (prepared &&
                recordTypes[family] != D9C_COMMAND_RECORD_APPLY_STATE) {
              prepared = pe::finalizeSparseStatePlanChunkContext(
                  access, f.chunk, plan);
            }
            return prepared;
          });
      check(accessed && prepared,
            "owner source selection plan prepares");
      pe::PeSemanticPreparedSparseLayout layout{};
      const bool layoutPrepared = guard.withCapability(
          ownerThread, epoch, [&](const auto& access) noexcept {
            const auto* const source = plan.source(access);
            return source &&
                   pe::detail::preparePeSemanticSparsePlanLayout(
                       *source, plan.destination(), layout);
          });
      check(layoutPrepared && layout.valid,
            "owner source selection quotas prepare");
      pe::PeSparseScratch scratch{};
      PeHotStateShadow independentShadow = f.shadow;
      PeConstShadowBlock independentConstants = f.constants;
      pe::SparseStateInput independentState{};
      D9CCommandChunkWireDrawHeader independentHeader{};
      check(pe::buildSparseState(
                independentShadow, independentConstants, f.bindings,
                f.payloads, f.params, false, false, scratch,
                independentHeader, independentState),
            "independent source selection builds");
      if (recordTypes[family] != D9C_COMMAND_RECORD_APPLY_STATE) {
        check(pe::addChunkContextSections(
                  f.chunk, f.shadow, f.bindings, f.params, scratch,
                  independentState),
              "independent source selection destination builds");
      }
      const std::array<std::size_t, 17> expectedQuotas = {
          independentState.renderStates.size(),
          independentState.textures.size(),
          independentState.streams.size(),
          independentState.shaders.size(),
          independentState.vertexInputs.size(),
          independentState.indexBuffers.size(),
          independentState.renderTargets.size(),
          independentState.depthStencils.size(),
          independentState.viewports.size(),
          independentState.scissors.size(),
          independentState.materials.size(),
          independentState.clipPlanes.size(),
          independentState.textureStageStates.size(),
          independentState.samplerStates.size(),
          independentState.transforms.size(),
          independentState.lights.size(),
          independentState.lightEnables.size(),
      };
      for (std::size_t quota = 0u; quota < expectedQuotas.size(); ++quota) {
        check(layout.quotaDeltas[quota] == expectedQuotas[quota],
              "owner source selection preserves exact sparse quota");
      }
    }
  }
}

void productionOwnerExplicitSnapshotRoutePreserved() {
  Fixture f = baseDraw("production owner explicit snapshot route",
                       D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE);
  f.forceFullSnapshot = true;
  f.bindings.indexBuffer =
      publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  const auto direct = runDirectDrawLane(f);

  PeConstShadowBlock planConstants = f.constants;
  pe::SparseStatePlan plan;
  const bool compactPrepared = withRecorderAccess(
      [&](const auto& access) noexcept {
        return pe::buildSparseStatePlan(
                   access, f.shadow, planConstants, f.bindings, f.payloads,
                   f.params, true, false, plan) &&
               pe::finalizeSparseStatePlanChunkContext(access, f.chunk, plan);
      });
  check(compactPrepared,
        "explicit snapshot compact witness prepares for route rejection");
  pe::PeSemanticBatchOwner<> owner;
  const bool compactOwnerRejected = withRecorderAccess(
      [&](const auto& access) noexcept {
        return !owner.tryAppendSparseStatePlan(
                   access, plan,
                   pe::PeSemanticProducerKind::DrawIndexedPrimitive,
                   f.params.recordType, D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, 1u,
                   1u, []() noexcept { return true; }) && owner.size() == 0u;
      });
  check(compactOwnerRejected,
        "production compact owner rejects the explicit full-snapshot route");

  PeConstShadowBlock explicitConstants = f.constants;
  pe::SparseStateInput sparse{};
  D9CCommandChunkWireDrawHeader draw{};
  check(pe::buildSparseState(
            f.shadow, explicitConstants, f.bindings, f.payloads, f.params,
            true, false, sharedScratch(), draw, sparse) &&
            pe::addChunkContextSections(
                f.chunk, f.shadow, f.bindings, f.params, sharedScratch(),
                sparse),
        "explicit snapshot retains the generic value-owned preparation route");
  pe::PeSemanticRecordInput input{};
  input.producer = pe::PeSemanticProducerKind::DrawIndexedPrimitive;
  input.recordType = f.params.recordType;
  input.sourceOrdinal = 1u;
  input.recordOrdinal = 1u;
  input.draw = draw;
  input.sparse = sparse;
  check(owner.tryAppendOwnedRecord(
            input, []() noexcept { return true; }),
        "generic owner still accepts the explicit full-snapshot envelope");
  pe::PeSemanticExactFixedEmission exact;
  check(direct.ok && owner.emitExactFixed(exact) && exact.valid() &&
            std::vector<std::byte>(exact.wire.begin(), exact.wire.end()) ==
                direct.bytes,
        "explicit generic owner bytes equal the independent direct golden");
}

void inlineConstAppendFailurePreservesDirtyRange() {
  Fixture f = baseDraw("inline const append failure retry",
                       D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  auto& range = f.constants.vsConstF;
  range.values.assign(16u, 0x5au);
  range.dirtyElems.assign(1u, 1u);
  range.dirtyStart = 0u;
  range.dirtyEnd = 1u;

  PeConstShadowBlock constants = f.constants;
  pe::PeSparseScratch scratch{};
  pe::SparseStateInput state{};
  D9CCommandChunkWireDrawHeader header{};
  check(pe::buildSparseState(f.shadow, constants, f.bindings, f.payloads,
                             f.params, false, true, scratch, header, state),
        "inline constant preparation must succeed");
  check(constants.vsConstF.dirty(),
        "preparation must not consume the dirty constant range");

  pe::CommandChunkBuilder builder;
  check(builder.beginRecord(D9C_COMMAND_RECORD_CLEAR),
        "failure injection occupies the builder's active record");
  check(!pe::appendSparseRecord(builder, f.params.recordType, header, state),
        "active-record injection must reject the draw append");
  check(constants.vsConstF.dirty() &&
            constants.vsConstF.dirtyStart == 0u &&
            constants.vsConstF.dirtyEnd == 1u,
        "failed append must preserve exact constant retry input");

  builder.rollbackRecord();
  check(pe::appendSparseRecord(builder, f.params.recordType, header, state),
        "retry must accept the same prepared range exactly once");
  check(pe::acceptInlineConstantDelta(
            constants, state,
            pe::settleRecorderAppend({
                .phase = pe::AppendSettlement::Prepared,
                .appendSucceeded = true,
            })),
        "accepted record must settle its prepared constant range");
  check(!constants.vsConstF.dirty(),
        "accepted retry consumes the dirty constant range");
}

void sparseSettlementNormalAndOversizedAreExact() {
  Fixture f = baseDraw("settlement-only fixture",
                       D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  f.shadow.writer().pendingRenderStatesTyped().set(renderStateSlotKey(2u), 20u);
  f.shadow.writer().pendingRenderStatesTyped().set(renderStateSlotKey(7u), 70u);
  f.shadow.writer().pendingTextureMask() = 1u << 3u;
  f.shadow.writer().pendingViewport() = true;
  f.inlineConstDelta = true;
  std::uint32_t constant[4] = {1u, 2u, 3u, 4u};
  touchConstShadow(f.constants.vsConstF, 5u, 1u, constant,
                   sizeof(constant));

  PeConstShadowBlock constants = f.constants;
  PeHotStateShadow shadow = f.shadow;
  pe::PeSparseScratch scratch{};
  pe::SparseStateInput state{};
  D9CCommandChunkWireDrawHeader header{};
  check(pe::buildSparseState(shadow, constants, f.bindings, f.payloads,
                             f.params, false, true, scratch, header, state),
        "normal settlement fixture prepares");
  const auto failed = pe::settleRecorderAppend({
      .phase = pe::AppendSettlement::Prepared,
      .appendSucceeded = false,
  });
  check(!pe::acceptPreparedSparseState(shadow, constants, state, failed) &&
            shadow.writer().pendingRenderStatesTyped().size() == 2u &&
            shadow.pendingTextureMask() == (1u << 3u) &&
            shadow.pendingViewport() && constants.vsConstF.dirty(),
        "failed normal settlement retains every represented owner");

  const auto accepted = pe::settleRecorderAppend({
      .phase = pe::AppendSettlement::Prepared,
      .appendSucceeded = true,
  });
  check(pe::acceptPreparedSparseState(shadow, constants, state, accepted) &&
            shadow.writer().pendingRenderStatesTyped().empty() &&
            shadow.pendingTextureMask() == 0u && !shadow.pendingViewport() &&
            !constants.vsConstF.dirty(),
        "accepted normal settlement consumes exactly represented owners");
  check(!pe::acceptPreparedSparseState(
            shadow, constants, state,
            pe::settleRecorderAppend({
                .phase = accepted.next(),
                .appendSucceeded = true,
            })),
        "normal sparse projection cannot settle twice");

  PeHotStateShadow oversized{};
  auto pending = oversized.writer().pendingRenderStatesTyped();
  constexpr std::uint32_t total =
      D9C_DRAW_PACKET_MAX_RENDER_STATES + 1u;
  for (std::uint32_t i = 0u; i < total; ++i) {
    pending.set(renderStateSlotKey(i), i + 100u);
  }
  std::array<D9CCommandChunkWireRenderState,
             D9C_DRAW_PACKET_MAX_RENDER_STATES> rows{};
  const std::size_t count = oversized.prepareRenderStateBatch(rows);
  pe::SparseStateInput oversizedState{};
  oversizedState.renderStates = std::span(rows).first(count);
  PeConstShadowBlock emptyConstants{};
  check(pe::acceptPreparedSparseState(
            oversized, emptyConstants, oversizedState, accepted) &&
            pending.size() == 1u &&
            pending.contains(renderStateSlotKey(total - 1u)),
        "oversized settlement uses the same exact represented-set acceptance");
}

void sparsePlanFailureRetryAndSourceWitness() {
  Fixture f = baseDraw("plan settlement-only fixture",
                       D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  PeHotStateShadow shadow{};
  shadow.writer().pendingRenderStatesTyped().set(renderStateSlotKey(7u), 70u);
  shadow.writer().pendingTextureMask() = 1u << 2u;
  PeConstShadowBlock constants{};
  pe::SparseStatePlan plan{};
  check(withRecorderAccess([&](const auto& access) noexcept {
          return pe::buildSparseStatePlan(access, shadow, constants,
                                          f.bindings, {}, f.params, false,
                                          false, plan);
        }),
        "compact plan preparation succeeds");
  bool sourceIdentityMatches = false;
  check(withRecorderAccess([&](const auto& access) noexcept {
          return plan.withSettlementSources(
              access,
              [&](const PeHotStateShadow& borrowedShadow,
                  PeConstShadowBlock& borrowedConstants,
                  const pe::PeBindingView& borrowedBindings) noexcept {
                sourceIdentityMatches = &borrowedShadow == &shadow &&
                    &borrowedConstants == &constants &&
                    &borrowedBindings == &f.bindings;
              });
        }) && sourceIdentityMatches &&
            sizeof(pe::SparseStatePlan) < 512u,
        "compact plan carries bounded borrowed source witnesses only");

  pe::CommandChunkBuilder builder;
  check(!withRecorderAccess([&](const auto& access) noexcept {
          return pe::appendSparseStatePlan(access, builder, f.params.recordType,
                                           plan);
        }),
        "draw plan cannot materialize before destination chunk finalization");
  check(shadow.pendingRenderStatesTyped().size() == 1u &&
            shadow.pendingTextureMask() == (1u << 2u),
        "pre-materialization rejection preserves PendingDelta");
  check(withRecorderAccess([&](const auto& access) noexcept {
          return pe::finalizeSparseStatePlanChunkContext(access, f.chunk,
                                                         plan);
        }),
        "chunk-context finalization succeeds after CapacityPre");

  check(builder.beginRecord(D9C_COMMAND_RECORD_CLEAR),
        "active-record injection occupies the builder");
  check(!withRecorderAccess([&](const auto& access) noexcept {
          return pe::appendSparseStatePlan(access, builder, f.params.recordType,
                                           plan);
        }),
        "builder failure rejects direct final-payload materialization");
  const auto failed = pe::settleRecorderAppend({
      .phase = pe::AppendSettlement::Prepared,
      .appendSucceeded = false,
  });
  check(!withRecorderAccess([&](const auto& access) noexcept {
          return pe::acceptSparseStatePlan(access, shadow, constants, plan,
                                           failed);
        }) &&
            shadow.pendingRenderStatesTyped().size() == 1u &&
            shadow.pendingTextureMask() == (1u << 2u),
        "failed plan append preserves exact retry witnesses");

  builder.rollbackRecord();
  check(withRecorderAccess([&](const auto& access) noexcept {
          return pe::appendSparseStatePlan(access, builder, f.params.recordType,
                                           plan);
        }),
        "same compact plan retries successfully");
  const auto accepted = pe::settleRecorderAppend({
      .phase = pe::AppendSettlement::Prepared,
      .appendSucceeded = true,
  });
  PeHotStateShadow wrongSource = shadow;
  check(!withRecorderAccess([&](const auto& access) noexcept {
          return pe::acceptSparseStatePlan(access, wrongSource, constants,
                                           plan, accepted);
        }) &&
            shadow.pendingRenderStatesTyped().size() == 1u,
        "accept rejects a source binding different from the emitted witness");
  check(withRecorderAccess([&](const auto& access) noexcept {
          return pe::acceptSparseStatePlan(access, shadow, constants, plan,
                                           accepted);
        }) &&
            shadow.pendingRenderStatesTyped().empty() &&
            shadow.pendingTextureMask() == 0u,
        "durable plan consumes exactly its emitted PendingDelta");

  // Indexed compact settlement must collect the source receipt while the
  // admission capability is live, before settlement consumes the plan's
  // borrowed PendingDelta/dependency witness.
  Fixture indexed = baseDraw("indexed receipt before compact settlement",
                             D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE);
  indexed.bindings.indexBuffer =
      publishedRef(&ib1, D9C_CHUNK_HANDLE_KIND_BUFFER);
  indexed.shadow.writer().pendingIb() = true;
  std::recursive_mutex receiptMutex;
  dxmt9::core::ThreadOwnershipToken receiptOwnerThread;
  std::uint64_t receiptEpoch = 1u;
  pe::RecorderLockGuard receiptGuard(receiptMutex, false);
  pe::SparseStatePlan receiptPlan;
  check(receiptGuard.withCapability(
            receiptOwnerThread, receiptEpoch,
            [&](const auto& access) noexcept {
              return pe::buildSparseStatePlan(
                         access, indexed.shadow, indexed.constants,
                         indexed.bindings, indexed.payloads, indexed.params,
                         false, false, receiptPlan) &&
                     pe::finalizeSparseStatePlanChunkContext(
                         access, indexed.chunk, receiptPlan);
            }) &&
            receiptPlan.destination().indexBuffer,
        "indexed compact receipt plan selects its index dependency");
  pe::PeSemanticBatchOwner<> receiptBatchOwner;
  const auto receiptSettlement = pe::settleRecorderAppend({
      .phase = pe::AppendSettlement::Prepared,
      .appendSucceeded = true,
  });
  std::uint64_t receiptWire = 0u;
  bool receiptBound = false;
  check(receiptGuard.withCapability(
            receiptOwnerThread, receiptEpoch,
            [&](const auto& access) noexcept {
              const bool receiptReady = receiptPlan.withBindingSource(
                  access,
                  [&](const pe::PeBindingView& bindings) noexcept {
                    receiptWire = d9cWireHandleValue(
                        toWireHandle(bindings.indexBuffer.object));
                    receiptBound = bindings.indexBuffer.object != nullptr;
                  });
              return receiptReady && receiptBatchOwner.tryAppendSparseStatePlan(
                                         access, receiptPlan,
                                         pe::PeSemanticProducerKind::DrawIndexedPrimitive,
                                         indexed.params.recordType,
                                         D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
                                         1u, 1u, [&]() noexcept {
                                           return pe::acceptSparseStatePlan(
                                               access, indexed.shadow,
                                               indexed.constants, receiptPlan,
                                               receiptSettlement, nullptr, 1u,
                                               receiptPlan.pendingTicket());
                                         });
            }) &&
            receiptBound &&
            receiptWire ==
                d9cWireHandleValue(toWireHandle(indexed.bindings.indexBuffer.object)),
        "indexed receipt is captured before compact settlement");
}

void productionOwnerPlanRollbackRebaseAndStaleAuthority() {
  Fixture f = baseDraw("owner plan rollback/rebase fixture",
                       D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE);
  f.bindings.indexBuffer =
      publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  f.bindings.streams[1].buffer =
      publishedRef(&vb1, D9C_CHUNK_HANDLE_KIND_BUFFER);
  f.bindings.streams[1].offset = 96u;
  f.bindings.streams[1].stride = 24u;

  pe::SparseStatePlan plan;
  pe::SparseStatePlan unpublished;
  pe::PeSemanticBatchOwner<> unpublishedOwner;
  check(withRecorderAccess([&](const auto& access) noexcept {
          return !unpublishedOwner.tryAppendSparseStatePlan(
                     access, unpublished,
                     pe::PeSemanticProducerKind::DrawIndexedPrimitive,
                     f.params.recordType, D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
                     1u, 1u, []() noexcept { return true; }) &&
                 unpublishedOwner.size() == 0u;
        }),
        "owner rejects an unpublished plan without a forgeable validity flag");
  check(withRecorderAccess([&](const auto& access) noexcept {
          return pe::buildSparseStatePlan(access, f.shadow, f.constants,
                                          f.bindings, {}, f.params, false,
                                          false, plan);
        }),
        "production owner plan fixture prepares");
  pe::PeChunkContext firstDestination{};
  check(withRecorderAccess([&](const auto& access) noexcept {
          return pe::finalizeSparseStatePlanChunkContext(
                     access, firstDestination, plan) &&
            (plan.destination().streamMask & (1u << 1u)) != 0u &&
                plan.destination().indexBuffer;
        }),
        "first destination adds missing stream and index dependencies");
  pe::PeChunkContext rebasedDestination{};
  rebasedDestination.retainedStreamMask = (1u << 0u) | (1u << 1u);
  rebasedDestination.indexBufferKnown = true;
  rebasedDestination.submittedIndexBufferWire =
      d9cWireHandleValue(toWireHandle(f.bindings.indexBuffer.object));
  rebasedDestination.indexBufferRetained = true;
  check(withRecorderAccess([&](const auto& access) noexcept {
          const auto* const source = plan.source(access);
          return source && pe::finalizeSparseStatePlanChunkContext(
                     access, rebasedDestination, plan) &&
            plan.destination().streamMask == source->selectedStreamMask &&
            !plan.destination().indexBuffer && !source->selectedIndexBuffer;
        }),
        "destination rebase discards prior chunk-only dependency decisions");

  pe::PeSemanticBatchOwner<> owner;
  check(!withRecorderAccess([&](const auto& access) noexcept {
          return owner.tryAppendSparseStatePlan(
                     access, plan,
                     pe::PeSemanticProducerKind::DrawIndexedPrimitive,
                     f.params.recordType, D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
                     1u, 1u, []() noexcept { return false; });
        }) &&
            owner.size() == 0u && owner.retainedCount() == 0u,
        "failed owner settlement rolls back record, pins, and ledgers");
  check(withRecorderAccess([&](const auto& access) noexcept {
          return owner.tryAppendSparseStatePlan(
                     access, plan,
                     pe::PeSemanticProducerKind::DrawIndexedPrimitive,
                     f.params.recordType, D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
                     1u, 1u, []() noexcept { return true; });
        }) && owner.size() == 1u,
        "same plan retries through the sole owner transaction");

  PeHotStateShadow staleShadow{};
  staleShadow.writer().pendingRenderStatesTyped().set(
      renderStateSlotKey(3u), 9u);
  PeConstShadowBlock staleConstants{};
  pe::SparseStatePlan stalePlan;
  pe::PeDrawParams apply{};
  apply.recordType = D9C_COMMAND_RECORD_APPLY_STATE;
  check(withRecorderAccess([&](const auto& access) noexcept {
          return pe::buildSparseStatePlan(access, staleShadow, staleConstants,
                                          f.bindings, {}, apply, false, false,
                                          stalePlan);
        }),
        "stale-ticket plan prepares");
  staleShadow.writer().pendingRenderStatesTyped().set(
      renderStateSlotKey(4u), 10u);
  pe::PeSemanticBatchOwner<> staleOwner;
  check(!withRecorderAccess([&](const auto& access) noexcept {
          return staleOwner.tryAppendSparseStatePlan(
                     access, stalePlan, pe::PeSemanticProducerKind::ApplyState,
                     apply.recordType, D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, 1u,
                     1u, []() noexcept { return true; });
        }) && staleOwner.size() == 0u,
        "stale PendingDelta ticket rejects before owner mutation");
  check(staleShadow.pendingRenderStatesTyped().size() == 2u &&
            staleShadow.pendingRenderStatesTyped().contains(
                renderStateSlotKey(3u)) &&
            staleShadow.pendingRenderStatesTyped().contains(
                renderStateSlotKey(4u)),
        "stale ticket rejection preserves both old and newer pending keys");
  std::recursive_mutex otherMutex;
  dxmt9::core::ThreadOwnershipToken otherOwnerThread;
  std::uint64_t otherEpoch = testRecorderDomain().epoch;
  pe::RecorderLockGuard otherGuard(otherMutex, false);
  const bool wrongOwner = otherGuard.withCapability(
      otherOwnerThread, otherEpoch, [&](const auto& otherAccess) noexcept {
        return withRecorderAccess([&](const auto& access) noexcept {
          return plan.validFor(access) && !plan.validFor(otherAccess) &&
                 !owner.tryAppendSparseStatePlan(
                     otherAccess, plan,
                     pe::PeSemanticProducerKind::DrawIndexedPrimitive,
                     f.params.recordType, D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
                     2u, 2u, []() noexcept { return true; });
        });
      });
  check(wrongOwner && owner.size() == 1u,
        "same numeric epoch from another recorder cannot authorize a plan");
  ++testRecorderDomain().epoch;
  check(!withRecorderAccess([&](const auto& access) noexcept {
          return owner.tryAppendSparseStatePlan(
              access, plan, pe::PeSemanticProducerKind::DrawIndexedPrimitive,
              f.params.recordType, D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, 2u, 2u,
              []() noexcept { return true; });
        }),
        "fresh capability cannot authorize a plan from the previous epoch");
}

void productionOwnerRejectsMutatedSourcesAtomically() {
  // A source borrow is authority, not a byte/epoch hash. The owner still
  // validates independently mutable dependencies at admission: a changed
  // constant range and a kind-qualified binding collision must reject without
  // consuming the newer PendingDelta/constant state.
  Fixture constantFixture = baseDraw("owner source mutation constants",
                                     D9C_COMMAND_RECORD_APPLY_STATE);
  constantFixture.inlineConstDelta = true;
  constantFixture.shadow.writer().pendingRenderStatesTyped().set(
      renderStateSlotKey(4u), 44u);
  constantFixture.constants.vsConstF.values.assign(32u, 0x11u);
  constantFixture.constants.vsConstF.dirtyElems.assign(2u, 1u);
  constantFixture.constants.vsConstF.dirtyStart = 0u;
  constantFixture.constants.vsConstF.dirtyEnd = 1u;

  std::recursive_mutex mutex;
  dxmt9::core::ThreadOwnershipToken ownerThread;
  std::uint64_t epoch = 211u;
  pe::RecorderLockGuard guard(mutex, false);
  PeConstShadowBlock constants = constantFixture.constants;
  pe::SparseStatePlan staleConstants;
  check(guard.withCapability(ownerThread, epoch, [&](const auto& access) noexcept {
          return pe::buildSparseStatePlan(
              access, constantFixture.shadow, constants,
              constantFixture.bindings, constantFixture.payloads,
              constantFixture.params, false, true, staleConstants);
        }),
        "constant mutation plan prepares");
  const auto originalTicket = staleConstants.pendingTicket();
  constants.vsConstF.dirtyStart = 1u;
  constants.vsConstF.dirtyEnd = 2u;
  constants.vsConstF.values[16u] = 0x77u;
  pe::PeSemanticBatchOwner<> constantOwner;
  const auto accepted = pe::settleRecorderAppend({
      .phase = pe::AppendSettlement::Prepared,
      .appendSucceeded = true,
  });
  check(!guard.withCapability(ownerThread, epoch, [&](const auto& access) noexcept {
                return constantOwner.tryAppendSparseStatePlan(
                    access, staleConstants,
                    pe::PeSemanticProducerKind::ApplyState,
                    constantFixture.params.recordType,
                    D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, 1u, 1u,
                    [&]() noexcept {
                      return pe::acceptSparseStatePlan(
                          access, constantFixture.shadow, constants,
                          staleConstants, accepted, nullptr, 1u, originalTicket);
                    });
              }) &&
            constantOwner.size() == 0u &&
            constantFixture.shadow.pendingRenderStatesTyped().size() == 1u &&
            constants.vsConstF.dirty() && constants.vsConstF.dirtyStart == 1u &&
            constants.vsConstF.dirtyEnd == 2u,
        "changed constant dirty range rejects without consuming newer state");

  // Rebuild against the newer range and prove the retry settles exactly the
  // current source, including the unchanged pending ticket.
  pe::SparseStatePlan currentConstants;
  check(guard.withCapability(ownerThread, epoch, [&](const auto& access) noexcept {
          return pe::buildSparseStatePlan(
              access, constantFixture.shadow, constants,
              constantFixture.bindings, constantFixture.payloads,
              constantFixture.params, false, true, currentConstants);
        }),
        "new constant range plan prepares");
  check(guard.withCapability(ownerThread, epoch, [&](const auto& access) noexcept {
                return constantOwner.tryAppendSparseStatePlan(
                    access, currentConstants,
                    pe::PeSemanticProducerKind::ApplyState,
                    constantFixture.params.recordType,
                    D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, 2u, 2u,
                    [&]() noexcept {
                      return pe::acceptSparseStatePlan(
                          access, constantFixture.shadow, constants,
                          currentConstants, accepted, nullptr, 2u,
                          currentConstants.pendingTicket());
                    });
              }) &&
            constantOwner.size() == 1u &&
            constantFixture.shadow.pendingRenderStatesTyped().empty() &&
            !constants.vsConstF.dirty(),
        "rebuilt plan consumes only the newer constant range");

  auto bindingMutation = [&](bool generationMismatch) noexcept {
    Fixture f = baseDraw("owner source mutation binding",
                         D9C_COMMAND_RECORD_APPLY_STATE);
    f.shadow.writer().pendingTextureMask() = 0x3u;
    f.bindings.textures[0] =
        publishedRef(&tex0, D9C_CHUNK_HANDLE_KIND_TEXTURE);
    f.bindings.textures[1] =
        publishedRef(&tex1, D9C_CHUNK_HANDLE_KIND_TEXTURE);
    PeConstShadowBlock localConstants = f.constants;
    pe::SparseStatePlan plan;
    if (!guard.withCapability(ownerThread, epoch, [&](const auto& access) noexcept {
          return pe::buildSparseStatePlan(access, f.shadow, localConstants,
                                          f.bindings, f.payloads, f.params,
                                          false, false, plan);
        })) {
      return false;
    }
    if (generationMismatch) {
      f.bindings.textures[1].object = f.bindings.textures[0].object;
      f.bindings.textures[1].identity = f.bindings.textures[0].identity;
      ++f.bindings.textures[1].identity.generation;
    } else {
      f.bindings.textures[1].identity = f.bindings.textures[0].identity;
    }
    pe::PeSemanticBatchOwner<> owner;
    return !guard.withCapability(ownerThread, epoch, [&](const auto& access) noexcept {
               return owner.tryAppendSparseStatePlan(
                   access, plan, pe::PeSemanticProducerKind::ApplyState,
                   f.params.recordType, D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, 1u,
                   1u, [&]() noexcept {
                     return pe::acceptSparseStatePlan(
                         access, f.shadow, localConstants, plan, accepted,
                         nullptr, 1u, plan.pendingTicket());
                   });
             }) &&
        owner.size() == 0u &&
        f.shadow.pendingTextureMask() == 0x3u;
  };
  check(bindingMutation(false),
        "owner rejects same-generation object identity mismatch");
  check(bindingMutation(true),
        "owner rejects same-object generation identity mismatch");
}

void compactDestinationResolversAreExhaustiveAndLazy() {
  static D9CBuffer resolverBuffers[3];
  pe::PeBindingView bindings{};
  for (std::uint32_t slot = 0u; slot < 3u; ++slot) {
    bindings.streams[slot].buffer =
        publishedRef(&resolverBuffers[slot], D9C_CHUNK_HANDLE_KIND_BUFFER);
  }

  // Every small selected/bound/retained mask is covered by the same pure
  // production resolver used by the compact destination path. Selected slots
  // never invoke retention, and unbound slots never invoke it either.
  for (std::uint32_t selected = 0u; selected < 8u; ++selected) {
    for (std::uint32_t bound = 0u; bound < 8u; ++bound) {
      for (std::uint32_t retainedMask = 0u; retainedMask < 8u;
           ++retainedMask) {
        for (std::uint32_t slot = 0u; slot < 3u; ++slot) {
          if ((bound & (1u << slot)) == 0u) {
            bindings.streams[slot].buffer = {};
          } else {
            bindings.streams[slot].buffer = publishedRef(
                &resolverBuffers[slot], D9C_CHUNK_HANDLE_KIND_BUFFER);
          }
        }
        std::size_t calls = 0u;
        const auto retained =
            [retainedMask, &calls](std::uint32_t slot,
                                   const pe::BufferRef&) noexcept {
              ++calls;
              return (retainedMask & (1u << slot)) != 0u;
            };
        const std::uint32_t actual = pe::resolveSparseStreamMask(
            selected, bindings, retained);
        const std::uint32_t expected =
            selected | (bound & ~selected & ~retainedMask);
        check(actual == expected,
              "compact stream resolver preserves selected/bound/retained masks");
        check(calls == static_cast<std::size_t>(std::popcount(bound & ~selected)),
              "compact stream resolver queries only bound unselected slots");
      }
    }
  }

  const auto index = publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  for (bool indexed : {false, true}) {
    for (bool selected : {false, true}) {
      for (bool known : {false, true}) {
        for (bool changed : {false, true}) {
          for (bool bound : {false, true}) {
            for (bool retainedValue : {false, true}) {
              std::size_t calls = 0u;
              const auto retained =
                  [retainedValue, &calls](const pe::BufferRef&) noexcept {
                    ++calls;
                    return retainedValue;
                  };
              const bool actual = pe::resolveSparseIndexBuffer(
                  indexed, selected, known, changed ? 2u : 1u, 1u,
                  bound ? index : pe::BufferRef{}, retained);
              const bool forced = selected || !known || changed;
              const bool expected = indexed &&
                  (forced || (bound && !retainedValue));
              check(actual == expected,
                    "compact index resolver preserves all emission reasons");
              const std::size_t expectedCalls =
                  indexed && !forced && bound ? 1u : 0u;
              check(calls == expectedCalls,
                    "compact index resolver queries retention only when needed");
            }
          }
        }
      }
    }
  }

  // The typed plan overload re-resolves on every destination, so an A/B/A
  // retention change cannot reuse a stale destination answer.
  Fixture f = baseDraw("typed compact destination A/B/A",
                       D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE);
  f.shadow.writer().pendingStreamMask() = 0u;
  f.bindings.streams[0].buffer = {};
  f.bindings.streams[1].buffer =
      publishedRef(&resolverBuffers[0], D9C_CHUNK_HANDLE_KIND_BUFFER);
  f.bindings.indexBuffer = index;
  f.shadow.writer().pendingIb() = false;
  std::recursive_mutex mutex;
  dxmt9::core::ThreadOwnershipToken ownerThread;
  std::uint64_t epoch = 11u;
  pe::RecorderLockGuard guard(mutex, false);
  pe::SparseStatePlan plan;
  check(guard.withCapability(ownerThread, epoch, [&](const auto& access) noexcept {
          return pe::buildSparseStatePlan(access, f.shadow, f.constants,
                                          f.bindings, {}, f.params, false,
                                          false, plan);
        }),
        "typed compact destination plan prepares");
  pe::PeChunkContext chunk{};
  chunk.indexBufferKnown = true;
  chunk.submittedIndexBufferWire =
      d9cWireHandleValue(toWireHandle(f.bindings.indexBuffer.object));
  bool retainAll = false;
  std::size_t calls = 0u;
  const auto retained =
      [&retainAll, &calls](std::uint32_t,
                           const pe::BufferRef&) noexcept {
        ++calls;
        return retainAll;
      };
  check(guard.withCapability(ownerThread, epoch, [&](const auto& access) noexcept {
          return pe::finalizeSparseStatePlanChunkContext(access, chunk, plan,
                                                         retained);
        }) &&
            plan.destination().streamMask == (1u << 1u) &&
                plan.destination().indexBuffer,
        "typed compact resolver resolves destination A");
  retainAll = true;
  check(guard.withCapability(ownerThread, epoch, [&](const auto& access) noexcept {
          return pe::finalizeSparseStatePlanChunkContext(access, chunk, plan,
                                                         retained);
        }) &&
            plan.destination().streamMask == 0u &&
                !plan.destination().indexBuffer,
        "typed compact resolver resolves destination B");
  retainAll = false;
  check(guard.withCapability(ownerThread, epoch, [&](const auto& access) noexcept {
          return pe::finalizeSparseStatePlanChunkContext(access, chunk, plan,
                                                         retained);
        }) &&
            plan.destination().streamMask == (1u << 1u) &&
                plan.destination().indexBuffer && calls == 6u,
        "typed compact resolver re-resolves destination A after B");
}

void compactObserverOnOffBytesAndPendingDifferential() {
  Fixture f;
  f.name = "compact observer on/off differential";
  const auto render = renderStateSlotKey(7u);
  const auto stage = textureStageIndexKey(0u);
  const auto type = textureStageStateTypeKey(1u);
  SamplerIndex sampler{};
  SamplerStateType samplerType{};
  check(samplerIndexKey(0u, sampler) && samplerStateTypeKey(1u, samplerType),
        "compact observer fixture keys are valid");
  f.shadow.writer().pendingRenderStatesTyped().set(render, 0x12u);
  f.shadow.writer().pendingTssTyped().set(stage, type, 0x34u);
  f.shadow.writer().pendingSamplerStatesTyped().set(sampler, samplerType,
                                                    0x56u);
  f.params.recordType = D9C_COMMAND_RECORD_APPLY_STATE;

  const auto unobserved = runOwnerPlanLane(f);

  std::recursive_mutex mutex;
  dxmt9::core::ThreadOwnershipToken ownerThread;
  std::uint64_t epoch = 13u;
  pe::RecorderLockGuard guard(mutex, false);
  pe::SparseStatePlan plan;
  check(guard.withCapability(ownerThread, epoch, [&](const auto& access) noexcept {
          return pe::buildSparseStatePlan(access, f.shadow, f.constants,
                                          f.bindings, {}, f.params, false,
                                          false, plan);
        }),
        "compact observer plan prepares");
  pe::PeScalarSemanticTokenLedger tokens;
  check(tokens.record(pe::ScalarSemanticCategory::RenderState, rawSlot(render)) &&
            tokens.record(pe::ScalarSemanticCategory::TextureStageState,
                          rawSlot(stage), rawSlot(type)) &&
            tokens.record(pe::ScalarSemanticCategory::SamplerState,
                          rawSlot(sampler), rawSlot(samplerType)),
        "compact observer fixture records all scalar tokens");
  pe::PeSemanticBatchOwner<> owner;
  const auto accepted = pe::settleRecorderAppend({
      .phase = pe::AppendSettlement::Prepared,
      .appendSucceeded = true,
  });
  check(guard.withCapability(ownerThread, epoch, [&](const auto& access) noexcept {
          return owner.tryAppendSparseStatePlan(
              access, plan, pe::PeSemanticProducerKind::ApplyState,
              f.params.recordType, D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, 1u, 1u,
              [&]() noexcept {
                return pe::acceptSparseStatePlan(
                    access, f.shadow, f.constants, plan, accepted, &tokens, 1u,
                    plan.pendingTicket());
              });
        }),
        "compact observer owner accepts the same plan");
  pe::PeSemanticExactFixedEmission exact;
  check(owner.emitExactFixed(exact) && exact.valid() &&
            std::vector<std::byte>(exact.wire.begin(), exact.wire.end()) ==
                unobserved.bytes && tokens.empty() &&
            f.shadow.pendingRenderStatesTyped().empty() &&
            f.shadow.pendingTssTyped().empty() &&
            f.shadow.pendingSamplerStatesTyped().empty(),
        "observer-on bytes and PendingDelta match observer-off settlement");
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
  f.shadow.writer().pendingVs() = true;
  f.shadow.writer().pendingPs() = true;
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
  f.shadow.writer().pendingRenderStatesTyped().set(renderStateSlotKey(7u), 3u);
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
  f.shadow.writer().pendingIb() = true;
  f.bindings.indexBuffer = publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  // Every chunk-context reason to emit is true as well, so nothing but the
  // record type can suppress the section.
  f.chunk.indexBufferKnown = false;
  f.chunk.indexBufferRetained = false;
  requirePinnedOutput(f);
}

// A device owns one SparseStatePlan scratch slot and rebuilds it for every
// record. Keep this plan alive across a deliberately rich indexed-UP draw,
// then rebuild it as an empty APPLY_STATE record: the second
// packet must not carry any constant range, UP span, mask, or destination-chunk
// decision from the first one.  The first packet uses all six constant
// families so an omitted reset of any optional range is observable.
void sparsePlanReuseClearsRichSourcesAndDestination() {
  Fixture first = baseUpDraw("reused plan rich indexed-UP source",
                             D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP);
  first.params.numVertices = 4u;
  first.params.indexFormat = 101u; // D3DFMT_INDEX16: 12 indices in 24 bytes.
  first.inlineConstDelta = true;
  first.payloads.upIndex = std::span<const std::byte>(upIndexBytes);
  first.payloads.upVertex = std::span<const std::byte>(upVertexBytes);
  first.shadow.writer().pendingRenderStatesTyped().set(
      renderStateSlotKey(4u), 0x44u);
  first.shadow.writer().pendingTextureMask() = 0x3u;
  first.shadow.writer().pendingStreamMask() = 0x3u;
  first.shadow.writer().pendingVdecl() = true;
  first.shadow.writer().pendingFvf() = true;
  first.shadow.writer().pendingIb() = true;
  first.shadow.writer().pendingRtMask() = 0x1u;
  first.shadow.writer().pendingDs() = true;
  first.shadow.writer().pendingViewport() = true;
  first.shadow.writer().pendingScissor() = true;
  first.shadow.writer().pendingMaterial() = true;
  first.shadow.writer().pendingClipPlaneMask() = 0x1u;
  first.shadow.writer().pendingLightSlotMask() = 0x1u;
  first.shadow.writer().pendingLightEnableValidMask() = 0x1u;
  first.shadow.writer().lightShadow()[0].type = 1u;
  first.bindings.textures[0] =
      publishedRef(&tex0, D9C_CHUNK_HANDLE_KIND_TEXTURE);
  first.bindings.textures[1] =
      publishedRef(&tex1, D9C_CHUNK_HANDLE_KIND_TEXTURE);
  first.bindings.streams[0].buffer =
      publishedRef(&vb0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  first.bindings.streams[1].buffer =
      publishedRef(&vb1, D9C_CHUNK_HANDLE_KIND_BUFFER);
  first.bindings.vdecl =
      publishedRef(&vdecl0, D9C_CHUNK_HANDLE_KIND_VERTEX_DECL);
  first.bindings.indexBuffer =
      publishedRef(&ib0, D9C_CHUNK_HANDLE_KIND_BUFFER);
  first.bindings.renderTargets[0] =
      publishedRef(&rt0, D9C_CHUNK_HANDLE_KIND_SURFACE);
  first.bindings.depthStencil =
      publishedRef(&ds0, D9C_CHUNK_HANDLE_KIND_SURFACE);

  struct ConstantSpec {
    ConstShadow* shadow;
    std::size_t elementBytes;
    std::uint32_t registers;
  };
  const ConstantSpec ranges[6] = {
      {&first.constants.vsConstF, 16u, 256u},
      {&first.constants.vsConstI, 16u, 16u},
      {&first.constants.vsConstB, 4u, 16u},
      {&first.constants.psConstF, 16u, 224u},
      {&first.constants.psConstI, 16u, 16u},
      {&first.constants.psConstB, 4u, 16u},
  };
  std::uint32_t seed = 1u;
  for (const auto& range : ranges) {
    range.shadow->values.assign(range.registers * range.elementBytes,
                                static_cast<std::uint8_t>(seed));
    range.shadow->dirtyElems.assign(range.registers, 0u);
    range.shadow->dirtyStart = seed % 3u;
    range.shadow->dirtyEnd = range.shadow->dirtyStart + 2u;
    ++seed;
  }

  std::recursive_mutex mutex;
  dxmt9::core::ThreadOwnershipToken owner;
  std::uint64_t epoch = 71u;
  pe::RecorderLockGuard guard(mutex, false);
  pe::SparseStatePlan plan{};
  pe::CommandChunkBuilder firstBuilder;
  check(guard.withCapability(owner, epoch, [&](const auto& access) noexcept {
          if (!pe::buildSparseStatePlan(
                  access, first.shadow, first.constants, first.bindings,
                  first.payloads, first.params, false,
                  first.inlineConstDelta, plan)) {
            return false;
          }
          const auto* const source = plan.source(access);
          check(source && source->vsFloatConstants.present() &&
                 source->vsIntConstants.present() &&
                 source->vsBoolConstants.present() &&
                 source->psFloatConstants.present() &&
                 source->psIntConstants.present() &&
                 source->psBoolConstants.present() &&
                 source->payloads.upIndex.data() == first.payloads.upIndex.data() &&
                 source->payloads.upVertex.data() ==
                     first.payloads.upVertex.data() &&
                 source->textureMask == 0x3u &&
                 source->selectedStreamMask == 0x3u &&
                 plan.destination().streamMask == 0x3u &&
                 !plan.destination().indexBuffer &&
                 plan.destination().chunkContextFinalized,
                "rich reused plan publishes all source and destination fields");
          return pe::appendSparseStatePlan(
              access, firstBuilder, first.params.recordType, plan);
        }),
        "rich UP plan prepares and emits through the reused plan");
  const auto reusedFirst = finishLane(firstBuilder, true);
  const auto freshFirst = runPlanLane(first);
  const auto genericFirst = runDirectDrawLane(first);
  check(reusedFirst.ok && freshFirst.ok && genericFirst.ok &&
            reusedFirst.bytes == freshFirst.bytes &&
            reusedFirst.bytes == genericFirst.bytes &&
            reusedFirst.recordCount == freshFirst.recordCount &&
            reusedFirst.handleCount == freshFirst.handleCount &&
            reusedFirst.payloadBytes == freshFirst.payloadBytes,
        "rich UP reuse matches fresh compact and independent generic bytes");

  Fixture second;
  second.name = "reused plan empty APPLY_STATE source";
  second.params.recordType = D9C_COMMAND_RECORD_APPLY_STATE;
  second.inlineConstDelta = false;
  pe::CommandChunkBuilder secondBuilder;
  check(guard.withCapability(owner, epoch, [&](const auto& access) noexcept {
          if (!pe::buildSparseStatePlan(
                  access, second.shadow, second.constants, second.bindings,
                  second.payloads, second.params, false,
                  second.inlineConstDelta, plan)) {
            return false;
          }
          const auto* const source = plan.source(access);
          const bool noConstants =
              source && !source->vsFloatConstants.present() &&
              !source->vsIntConstants.present() &&
              !source->vsBoolConstants.present() &&
              !source->psFloatConstants.present() &&
              !source->psIntConstants.present() &&
              !source->psBoolConstants.present();
          const bool noSources =
              source && source->renderStateCount == 0u &&
              source->textureStageStateCount == 0u &&
              source->samplerStateCount == 0u && source->transformCount == 0u &&
              source->textureMask == 0u && source->selectedStreamMask == 0u &&
              source->drawFlags == 0u && source->renderTargetMask == 0u &&
              source->clipPlaneMask == 0u && source->lightMask == 0u &&
              source->lightEnableMask == 0u && source->shaderMask == 0u &&
              !source->vertexInput && !source->selectedIndexBuffer &&
              !source->depthStencil && !source->viewport &&
              !source->scissor && !source->material &&
              source->payloads.upIndex.empty() &&
              source->payloads.upVertex.empty();
          const auto& destination = plan.destination();
          return noConstants && noSources && destination.streamMask == 0u &&
                 !destination.indexBuffer && destination.chunkContextFinalized &&
                 pe::appendApplyStatePlan(access, secondBuilder,
                                          source->drawFlags, plan);
        }),
        "reused plan clears constants, payloads, masks, and destination facts");
  const auto reusedSecond = finishLane(secondBuilder, true);
  const auto freshSecond = runPlanLane(second);
  const auto genericSecond = runDirectLane(second);
  check(reusedSecond.ok && freshSecond.ok && genericSecond.ok &&
            reusedSecond.bytes == freshSecond.bytes &&
            reusedSecond.bytes == genericSecond.bytes &&
            reusedSecond.recordCount == genericSecond.recordCount &&
            reusedSecond.handleCount == genericSecond.handleCount &&
            reusedSecond.payloadBytes == genericSecond.payloadBytes,
        "empty APPLY_STATE retry matches compact and generic producers");
}

// The six-family case above proves presence-to-absence.  This bounded A-B-A
// sequence additionally proves that changing families cannot leave a prior
// range authoritative when the same plan is rebuilt a third time.
void sparsePlanConstantFamilyABA() {
  Fixture a;
  a.name = "constant family A, VS float";
  a.params.recordType = D9C_COMMAND_RECORD_APPLY_STATE;
  a.inlineConstDelta = true;
  a.constants.vsConstF.values.assign(64u, 0x11u);
  a.constants.vsConstF.dirtyElems.assign(4u, 0u);
  a.constants.vsConstF.dirtyStart = 1u;
  a.constants.vsConstF.dirtyEnd = 3u;

  Fixture b;
  b.name = "constant family B, PS bool";
  b.params.recordType = D9C_COMMAND_RECORD_APPLY_STATE;
  b.inlineConstDelta = true;
  b.constants.psConstB.values.assign(16u, 0x22u);
  b.constants.psConstB.dirtyElems.assign(4u, 0u);
  b.constants.psConstB.dirtyStart = 2u;
  b.constants.psConstB.dirtyEnd = 4u;

  Fixture aAgain;
  aAgain.name = "constant family A again, VS float";
  aAgain.params.recordType = D9C_COMMAND_RECORD_APPLY_STATE;
  aAgain.inlineConstDelta = true;
  aAgain.constants.vsConstF.values.assign(64u, 0x33u);
  aAgain.constants.vsConstF.dirtyElems.assign(4u, 0u);
  aAgain.constants.vsConstF.dirtyStart = 0u;
  aAgain.constants.vsConstF.dirtyEnd = 1u;

  auto genericInline = [](const Fixture& fixture) {
    pe::CommandChunkBuilder builder;
    PeConstShadowBlock constants = fixture.constants;
    pe::PeSparseScratch scratch{};
    pe::SparseStateInput state{};
    D9CCommandChunkWireDrawHeader header{};
    if (!pe::buildSparseState(fixture.shadow, constants, fixture.bindings,
                              fixture.payloads, fixture.params, false,
                              fixture.inlineConstDelta, scratch, header,
                              state)) {
      return LaneResult{};
    }
    return finishLane(builder,
                     pe::appendApplyState(builder, header.flags, state));
  };

  const std::array<Fixture*, 3> cases = {&a, &b, &aAgain};
  std::recursive_mutex mutex;
  dxmt9::core::ThreadOwnershipToken owner;
  std::uint64_t epoch = 83u;
  pe::RecorderLockGuard guard(mutex, false);
  pe::SparseStatePlan plan{};
  for (Fixture* fixture : cases) {
    pe::CommandChunkBuilder builder;
    check(guard.withCapability(owner, epoch, [&](const auto& access) noexcept {
            if (!pe::buildSparseStatePlan(
                    access, fixture->shadow,
                    fixture->constants,
                    fixture->bindings, fixture->payloads, fixture->params, false,
                    fixture->inlineConstDelta, plan)) {
              return false;
            }
            const auto* const source = plan.source(access);
            const bool expectA = fixture != &b;
            return source && source->vsFloatConstants.present() == expectA &&
                   source->psBoolConstants.present() == (fixture == &b) &&
                   !source->vsIntConstants.present() &&
                   !source->vsBoolConstants.present() &&
                   !source->psFloatConstants.present() &&
                   !source->psIntConstants.present() &&
                   source->payloads.upIndex.empty() &&
                   source->payloads.upVertex.empty() &&
                   pe::appendApplyStatePlan(access, builder,
                                            source->drawFlags, plan);
          }),
          "A-B-A constant family plan prepares and emits");
    const auto reused = finishLane(builder, true);
    const auto fresh = runPlanLane(*fixture);
    const auto generic = genericInline(*fixture);
    check(reused.ok && fresh.ok && generic.ok &&
              reused.bytes == fresh.bytes && reused.bytes == generic.bytes &&
              reused.handleCount == generic.handleCount &&
              reused.payloadBytes == generic.payloadBytes,
          fixture->name + ": A-B-A bytes match compact and generic paths");
  }
}

void sparsePlanFailedPreparationCleanRetry() {
  // Fail late enough that the plan has already selected several optional
  // masks and entered constant preparation.  A failure at the initial
  // section-cap check would not exercise reset of those partially prepared
  // fields.
  Fixture failed = baseDraw("failed compact plan preparation",
                            D9C_COMMAND_RECORD_APPLY_STATE);
  failed.inlineConstDelta = true;
  failed.shadow.writer().pendingRenderStatesTyped().set(
      renderStateSlotKey(7u), 0x507u);
  failed.shadow.writer().pendingTextureMask() =
      (1u << D9C_DRAW_PACKET_MAX_TEXTURES) - 1u;
  failed.shadow.writer().pendingStreamMask() =
      (1u << D9C_DRAW_PACKET_MAX_STREAMS) - 1u;
  failed.shadow.writer().pendingVdecl() = true;
  failed.bindings.textures[0] =
      publishedRef(&tex0, D9C_CHUNK_HANDLE_KIND_TEXTURE);
  failed.bindings.vdecl =
      publishedRef(&vdecl0, D9C_CHUNK_HANDLE_KIND_VERTEX_DECL);
  failed.constants.vsConstF.values.assign(16u, 0x44u);
  failed.constants.vsConstF.dirtyElems.assign(256u, 0u);
  failed.constants.vsConstF.dirtyStart = 256u;
  failed.constants.vsConstF.dirtyEnd = 257u;
  pe::SparseStatePlan plan{};
  std::recursive_mutex mutex;
  dxmt9::core::ThreadOwnershipToken owner;
  std::uint64_t epoch = 97u;
  pe::RecorderLockGuard guard(mutex, false);
  bool failedSourceInaccessible = false;
  std::uint32_t failedDrawFlags = 0u;
  const bool failedBuild = guard.withCapability(
      owner, epoch, [&](const auto& access) noexcept {
        const bool built = pe::buildSparseStatePlan(
            access, failed.shadow, failed.constants, failed.bindings,
            failed.payloads, failed.params, false, failed.inlineConstDelta,
            plan);
        failedSourceInaccessible = plan.source(access) == nullptr;
        failedDrawFlags = plan.drawFlags();
        return built;
      });
  check(!failedBuild && failedSourceInaccessible && failedDrawFlags == 0u &&
            !plan.prepared() && !plan.pendingTicket().valid() &&
            !plan.destination().chunkContextFinalized,
        "partial preparation revokes the plan without publication");

  Fixture clean;
  clean.name = "clean retry after failed plan preparation";
  clean.params.recordType = D9C_COMMAND_RECORD_APPLY_STATE;
  clean.shadow.writer().pendingRenderStatesTyped().set(
      renderStateSlotKey(2u), 0x202u);
  pe::CommandChunkBuilder builder;
  check(guard.withCapability(owner, epoch, [&](const auto& access) noexcept {
          if (!pe::buildSparseStatePlan(
                  access, clean.shadow, clean.constants, clean.bindings,
                  clean.payloads, clean.params, false, false, plan)) {
            return false;
          }
          const auto* const source = plan.source(access);
          return source && source->renderStateCount == 1u &&
                 source->textureMask == 0u && source->selectedStreamMask == 0u &&
                 source->shaderMask == 0u && !source->vertexInput &&
                 source->drawFlags == 0u && plan.destination().streamMask == 0u &&
                 !plan.destination().indexBuffer &&
                 plan.destination().chunkContextFinalized &&
                 pe::appendApplyStatePlan(access, builder,
                                          source->drawFlags, plan);
        }),
        "same plan cleanly retries after failed preparation");
  const auto retry = finishLane(builder, true);
  const auto fresh = runPlanLane(clean);
  const auto generic = runDirectLane(clean);
  check(retry.ok && fresh.ok && generic.ok && retry.bytes == fresh.bytes &&
            retry.bytes == generic.bytes &&
            retry.recordCount == generic.recordCount &&
            retry.handleCount == generic.handleCount &&
            retry.payloadBytes == generic.payloadBytes,
        "clean retry matches fresh compact and generic canonical bytes");
}

void sparsePlanMaskSnapshotFlagParity() {
  constexpr auto allTextures =
      (1u << D9C_DRAW_PACKET_MAX_TEXTURES) - 1u;
  constexpr auto allStreams = (1u << D9C_DRAW_PACKET_MAX_STREAMS) - 1u;
  const std::array<std::uint32_t, 3u> maskChoices = {0u, 1u, allTextures};
  const std::array<std::uint32_t, 3u> streamChoices = {0u, 1u, allStreams};
  const std::array<std::uint32_t, 3u> recordTypes = {
      D9C_COMMAND_RECORD_APPLY_STATE, D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
      D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE};

  std::recursive_mutex mutex;
  dxmt9::core::ThreadOwnershipToken owner;
  std::uint64_t epoch = 103u;
  pe::RecorderLockGuard guard(mutex, false);
  pe::SparseStatePlan plan{};
  const bool envFullSnapshot = pe::dxmt9PeFullSnapshotEnabled();
  for (const auto recordType : recordTypes) {
    for (const auto textureMask : maskChoices) {
      for (const auto streamMask : streamChoices) {
        for (const bool forceFullSnapshot : {false, true}) {
          for (const bool inlineConstDelta : {false, true}) {
            Fixture fixture = baseDraw(
                "mask matrix", recordType);
            fixture.shadow.writer().pendingTextureMask() = textureMask;
            fixture.shadow.writer().pendingStreamMask() = streamMask;
            fixture.forceFullSnapshot = forceFullSnapshot;
            fixture.inlineConstDelta = inlineConstDelta;
            const bool expectedFullSnapshot =
                forceFullSnapshot || envFullSnapshot;
            const auto expectedTextureMask =
                expectedFullSnapshot ? allTextures : textureMask;
            const auto expectedStreamMask =
                expectedFullSnapshot ? allStreams : streamMask;
            const bool expectedSnapshotFlag =
                expectedFullSnapshot ||
                (expectedTextureMask == allTextures &&
                 expectedStreamMask == allStreams);
            const std::string caseName =
                "mask matrix record=" + std::to_string(recordType) +
                " texture=" + std::to_string(textureMask) +
                " stream=" + std::to_string(streamMask) +
                " force=" + std::to_string(forceFullSnapshot) +
                " inline=" + std::to_string(inlineConstDelta);
            check(guard.withCapability(
                      owner, epoch, [&](const auto& access) noexcept {
                        if (!pe::buildSparseStatePlan(
                                access, fixture.shadow, fixture.constants,
                                fixture.bindings, fixture.payloads,
                                fixture.params, forceFullSnapshot,
                                inlineConstDelta, plan)) {
                          return false;
                        }
                        const auto* const source = plan.source(access);
                        const bool snapshotFlag =
                            source &&
                            (source->drawFlags &
                             D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT) != 0u;
                        return source &&
                               source->fullSnapshot == expectedFullSnapshot &&
                               source->textureMask == expectedTextureMask &&
                               source->selectedStreamMask ==
                                   expectedStreamMask &&
                               snapshotFlag == expectedSnapshotFlag;
                      }),
                  caseName + ": selected masks and snapshot flag match");
          }
        }
      }
    }
  }
}

void sparsePlanFinalizerRejectsRetainedEpochInvalidation() {
  Fixture f = baseDraw("retained callback invalidates finalizer epoch",
                       D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE);
  f.shadow.writer().pendingStreamMask() = 0u;
  f.shadow.writer().pendingIb() = false;
  f.chunk.retainedStreamMask = 0u;
  f.chunk.indexBufferKnown = true;
  f.chunk.indexBufferRetained = true;

  std::recursive_mutex mutex;
  dxmt9::core::ThreadOwnershipToken owner;
  std::uint64_t epoch = 109u;
  pe::RecorderLockGuard guard(mutex, false);
  pe::SparseStatePlan plan{};
  bool callbackCalled = false;
  std::size_t callbackCount = 0u;
  check(guard.withCapability(owner, epoch, [&](const auto& access) noexcept {
          if (!pe::buildSparseStatePlan(
                  access, f.shadow, f.constants, f.bindings, f.payloads,
                  f.params, false, false, plan)) {
            return false;
          }
          const bool finalized = pe::finalizeSparseStatePlanChunkContext(
              access, f.chunk, plan,
              [&](std::uint32_t slot, const pe::BufferRef& buffer) noexcept {
                callbackCalled = slot == 0u && buffer.object != nullptr;
                ++callbackCount;
                ++epoch;
                return true;
              });
          return !finalized && !plan.validFor(access);
        }),
        "retained callback epoch invalidation rejects finalization");
  check(callbackCalled && callbackCount == 1u,
        "finalizer reached the custom retained callback before revalidation");
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
// context step runs) in snapshot mode (so buildSparseState emitted all 16
// stream sections including null unbinds). Legacy's
// populateDrawPacketStreamDependencies only ever ADDED mask bits, so an all-ones
// snapshot mask survived it untouched.
void snapshotDrawKeepsEveryStreamSection() {
  Fixture f = baseDraw("draw under snapshot keeps every stream section",
                       D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  f.forceFullSnapshot = true;
  // Bound, retained by the destination chunk, and NOT dirty: the exact slot the
  // context step's rebuild would drop.
  f.shadow.writer().pendingStreamMask() = 0u;
  f.chunk.retainedStreamMask = 0x1u;
  requirePinnedOutput(f);
}

void snapshotIndexedDrawKeepsEveryStreamSection() {
  Fixture f = baseDraw("indexed draw under snapshot keeps every stream section",
                       D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE);
  f.forceFullSnapshot = true;
  f.params.minVertex = 4u;
  f.params.numVertices = 64u;
  f.shadow.writer().pendingStreamMask() = 0u;
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
      const auto slot = rng() % D9C_DRAW_PACKET_MAX_RENDER_STATES;
      f.shadow.writer().pendingRenderStatesTyped().set(renderStateSlotKey(slot), rng());
    }
    f.shadow.writer().pendingTextureMask() =
        rng() & ((1u << D9C_DRAW_PACKET_MAX_TEXTURES) - 1u);
    f.shadow.writer().pendingStreamMask() =
        rng() & ((1u << D9C_DRAW_PACKET_MAX_STREAMS) - 1u);
    f.shadow.writer().pendingVs() = (rng() & 1u) != 0u;
    f.shadow.writer().pendingPs() = (rng() & 1u) != 0u;
    // Independently: either, both, or neither -- this is what exercises the
    // FVF wire kind and the declaration-wins rule.
    f.shadow.writer().pendingVdecl() = (rng() & 1u) != 0u;
    f.shadow.writer().pendingFvf() = (rng() & 1u) != 0u;
    f.bindings.vdecl = publishedRef(&vdecl0, D9C_CHUNK_HANDLE_KIND_VERTEX_DECL);
    f.shadow.writer().lightEnableShadow() = rng() & 0xFFu;
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
    f.shadow.writer().pendingIb() = (rng() & 1u) != 0u;
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
    f.shadow.writer().pendingViewport() = (rng() & 1u) != 0u;
    f.shadow.writer().pendingScissor() = (rng() & 1u) != 0u;
    f.shadow.writer().pendingMaterial() = (rng() & 1u) != 0u;
    f.shadow.writer().pendingRtMask() = rng() & 0xFu;
    f.shadow.writer().pendingDs() = (rng() & 1u) != 0u;
    f.shadow.writer().pendingClipPlaneMask() = rng() & 0x3Fu;
    f.shadow.writer().pendingLightSlotMask() = rng() & 0xFFu;
    f.shadow.writer().pendingLightEnableValidMask() = rng() & 0xFFu;
    f.shadow.writer().pendingLightEnableMask() = rng() & 0xFFu;
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
    sparseSetBitIterationOrderAndShortCircuit();
    emptyDelta();
    singleRenderStateDirty();
    scalarCategoriesDirty();
    compactPendingMaskAndEmptyTableDifferential();
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
    productionOwnerPlanAllCategoryDifferential();
    productionOwnerInlineConstantRangeMaskMatrix();
    productionOwnerInlineConstantPlanReuseAndRangeRetries();
    productionOwnerCompactQuotaReuseAndCapacityPins();
    productionOwnerSelectionDifferentialAcrossFamilies();
    productionOwnerBindingAliasDifferential();
    productionOwnerBindingAliasReusedPlanABA();
    genericControlForeignSparseQuotaPreserved();
    productionOwnerExplicitSnapshotRoutePreserved();
    inlineConstAppendFailurePreservesDirtyRange();
    sparseSettlementNormalAndOversizedAreExact();
    sparsePlanFailureRetryAndSourceWitness();
    productionOwnerPlanRollbackRebaseAndStaleAuthority();
    productionOwnerRejectsMutatedSourcesAtomically();
    compactDestinationResolversAreExhaustiveAndLazy();
    compactObserverOnOffBytesAndPendingDifferential();
    upDrawCarriesVertexPayload();
    upDrawWithDirtyState();
    upIndexedDrawCarriesBothPayloads();
    upIndexedDrawIgnoresDirtyIndexBuffer();
    sparsePlanReuseClearsRichSourcesAndDestination();
    sparsePlanConstantFamilyABA();
    sparsePlanFailedPreparationCleanRetry();
    sparsePlanMaskSnapshotFlagParity();
    sparsePlanFinalizerRejectsRetainedEpochInvalidation();
    upDrawUnderFullSnapshot();
    upIndexedDrawUnderFullSnapshot();
    snapshotDrawKeepsEveryStreamSection();
    snapshotIndexedDrawKeepsEveryStreamSection();
    randomizedRecordSequences();
    canonicalStreamFrequenciesReachEveryProducerLane();
    if (updatingGoldens()) {
      writeGoldens();
      std::cerr << "pe_producer_differential_spec: wrote "
                << capturedGoldens().size() << " goldens to " << goldenPath()
                << "\nThis run VERIFIED NOTHING -- update mode is not a test "
                   "run. Review the golden diff, then re-run without "
                   "DXMT9_UPDATE_PE_PRODUCER_GOLDEN.\n";
      return 2;
    }
    // Every loaded golden must have been checked. Catches a fixture deleted
    // without its row, which would otherwise rot in the file unnoticed.
    for (const auto& [name, unused] : goldens()) {
      (void)unused;
      check(consumedGoldens().count(name) != 0,
            name + ": golden row has no fixture. A fixture was removed or "
                   "renamed without updating the golden file.");
    }
  } catch (const TestFailure& failure) {
    std::cerr << "pe_producer_differential_spec FAILED: " << failure.what()
              << "\n";
    return 1;
  }
  std::cout << "pe_producer_differential_spec OK\n";
  return 0;
}
