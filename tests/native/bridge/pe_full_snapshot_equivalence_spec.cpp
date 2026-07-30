// pe_full_snapshot_equivalence_spec
//
// Regression guard for the DXMT9_PE_DRAW_FULL_SNAPSHOT debug knob. Rewritten in
// Task 10 stage C; what follows is why, because the change trades one kind of
// evidence for another and that should not be silent.
//
// WHAT THIS USED TO BE
//
// The previous version mirrored BOTH sides of the pipeline at test scope: its own
// copy of the PE recorder (PeShadow / applyStepToShadow) producing fat packets,
// and its own copy of the unix applier (BridgeShadow / applyPacket, mirroring
// applyDrawPacketStateDirect) consuming them. It asserted that delta-mode and
// snapshot-mode streams left the two mirrored shadows equal, across draws and
// seven kinds of barrier record.
//
// Task 10 deleted the fat packet, so it was mirroring a producer that no longer
// exists, and it read unix-side symbols (packetHasNoStateDelta) that stage D
// removes. It also could never have caught the one real defect this area has
// produced: addChunkContextSections replacing the snapshot's 16 stream sections
// with a delta-shaped subset (fixed in c3e18446). Two mirrors agree with each
// other regardless of what the real code does.
//
// WHAT IT IS NOW
//
// This runs the REAL producer -- buildSparseStateV2 plus addChunkContextSections,
// the exact pair the draw call sites use -- twice over identical inputs, once per
// mode, and asserts the two structural properties that are what make the knob
// safe:
//
//   1. SELF-CONTAINED. In snapshot mode every category with state present gets a
//      section, null unbinds included. That is the knob's whole purpose: a record
//      replayable in isolation and out of order.
//
//   2. DELTA IS A SUBSET. Every section entry the delta-mode record emits appears
//      identically in the snapshot-mode record. Together with (1) that is what
//      makes the modes interchangeable -- the snapshot carries everything the
//      delta carries plus the unchanged remainder, and re-asserting unchanged
//      state is idempotent on the applier.
//
// WHAT WAS LOST, STATED PLAINLY
//
//   - Barrier records (Clear / StretchRect / ColorFill / UpdateTexture /
//     UpdateSurface / Readback / Present) are no longer covered. That coverage was
//     close to vacuous for this knob: barrier records carry no draw-state packet,
//     so the mode cannot change them.
//   - Sequence coverage is a GENUINE loss. Nothing here checks "record N given
//     records 1..N-1". That is the same blind spot H4 records for the
//     differential, and closing it needs a harness that replays through the real
//     importer -- which neither spec has ever had.
//
// A per-record structural property against the real producer is better evidence
// than a sequence property against two mirrors. It is not a superset of it.

#include "d3d9_pe_producer.hpp"
#include "d3d9_pe_producer_views.hpp"
#include "d3d9_pe_state_shadow.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace pe = dxmt9::d3d9::pe;

namespace {

struct TestFailure : std::runtime_error {
  explicit TestFailure(std::string message)
      : std::runtime_error(std::move(message)) {}
};

void check(bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure(message);
  }
}

// Distinct addresses per slot, so a wrong slot shows up as a mismatch instead of
// aliasing onto a right one. The D9C wrapper types are opaque forward
// declarations and the producer only ever stores the pointer -- it never
// dereferences it -- so any distinct storage is a faithful stand-in.
unsigned char texObjects[D9C_DRAW_PACKET_MAX_TEXTURES]{};
unsigned char vbObjects[D9C_DRAW_PACKET_MAX_STREAMS]{};
unsigned char ibObject{};
unsigned char vsObject{};
unsigned char psObject{};
unsigned char vdeclObject{};
unsigned char rtObjects[D9C_DRAW_PACKET_MAX_RENDER_TARGETS]{};
unsigned char dsObject{};

pe::PeWireObjectRef ref(void* object, std::uint32_t kind,
                        std::uint32_t objectId) {
  pe::PeWireObjectRef r{};
  r.object = object;
  r.identity.kind = kind;
  r.identity.generation = 1u;
  r.identity.objectId = objectId;
  return r;
}

// Everything bound, so "self-contained" has something to contain.
pe::PeBindingView fullyBoundView() {
  pe::PeBindingView v{};
  std::uint32_t id = 1u;
  for (std::uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_TEXTURES; ++i) {
    v.textures[i] = ref(&texObjects[i], D9C_CHUNK_HANDLE_KIND_TEXTURE, id++);
  }
  for (std::uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_STREAMS; ++i) {
    v.streams[i].buffer =
        ref(&vbObjects[i], D9C_CHUNK_HANDLE_KIND_BUFFER, id++);
    v.streams[i].offset = 16u * i;
    v.streams[i].stride = 4u * (i + 1u);
  }
  v.indexBuffer = ref(&ibObject, D9C_CHUNK_HANDLE_KIND_BUFFER, id++);
  v.vs = ref(&vsObject, D9C_CHUNK_HANDLE_KIND_SHADER, id++);
  v.ps = ref(&psObject, D9C_CHUNK_HANDLE_KIND_SHADER, id++);
  v.vdecl = ref(&vdeclObject, D9C_CHUNK_HANDLE_KIND_VERTEX_DECL, id++);
  for (std::uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_RENDER_TARGETS; ++i) {
    v.renderTargets[i] =
        ref(&rtObjects[i], D9C_CHUNK_HANDLE_KIND_SURFACE, id++);
    v.rtExplicitMask[i] = true;
  }
  v.depthStencil = ref(&dsObject, D9C_CHUNK_HANDLE_KIND_SURFACE, id++);
  v.fvf = 0x142u;
  return v;
}

// One build of the real producer pair. Scratch is per-mode here because the two
// results must coexist to be compared; production reuses a single scratch, and
// the differential covers that reuse.
struct Built {
  pe::PeSparseScratch scratch{};
  pe::SparseStateV2Input state{};
  D9CCommandChunkWireDrawHeaderV2 header{};
  bool ok = false;
};

void build(Built& out, const PeHotStateShadow& shadow,
           const pe::PeBindingView& bindings, const pe::PeDrawParams& params,
           const pe::PeChunkContext& chunk, bool snapshot) {
  PeConstShadowBlock constants{};
  pe::PeDrawPayloads payloads{};
  out.ok = pe::buildSparseStateV2(shadow, constants, bindings, payloads, params,
                                  snapshot, /*inlineConstDelta=*/false,
                                  out.scratch, out.header, out.state);
  if (!out.ok) {
    return;
  }
  if (params.recordType != D9C_COMMAND_RECORD_APPLY_STATE) {
    out.ok = pe::addChunkContextSections(chunk, shadow, bindings, params,
                                         snapshot, out.scratch, out.state);
  }
}

// Property 2, per category. Element types are PODs default-initialized in both
// lanes, so padding compares equal and memcmp is the right comparison.
template <typename T>
void requireSubset(std::span<const T> delta, std::span<const T> snap,
                   std::string_view category, const std::string& name) {
  for (const T& d : delta) {
    bool found = false;
    for (const T& s : snap) {
      if (std::memcmp(&d, &s, sizeof(T)) == 0) {
        found = true;
        break;
      }
    }
    check(found,
          name + ": a " + std::string(category) +
              " entry emitted in delta mode is missing from the snapshot "
              "record, so the two modes are not interchangeable");
  }
  check(snap.size() >= delta.size(),
        name + ": the " + std::string(category) +
            " snapshot section is smaller than the delta section");
}

void requireDeltaIsSubsetOfSnapshot(const Built& delta, const Built& snap,
                                    const std::string& name) {
  requireSubset(delta.state.renderStates, snap.state.renderStates,
                "render state", name);
  requireSubset(delta.state.textures, snap.state.textures, "texture", name);
  requireSubset(delta.state.streams, snap.state.streams, "stream", name);
  requireSubset(delta.state.shaders, snap.state.shaders, "shader", name);
  requireSubset(delta.state.vertexInputs, snap.state.vertexInputs,
                "vertex input", name);
  requireSubset(delta.state.indexBuffers, snap.state.indexBuffers,
                "index buffer", name);
  requireSubset(delta.state.renderTargets, snap.state.renderTargets,
                "render target", name);
  requireSubset(delta.state.depthStencils, snap.state.depthStencils,
                "depth stencil", name);
  requireSubset(delta.state.viewports, snap.state.viewports, "viewport", name);
  requireSubset(delta.state.scissors, snap.state.scissors, "scissor", name);
  requireSubset(delta.state.materials, snap.state.materials, "material", name);
  requireSubset(delta.state.clipPlanes, snap.state.clipPlanes, "clip plane",
                name);
  requireSubset(delta.state.textureStageStates, snap.state.textureStageStates,
                "texture stage state", name);
  requireSubset(delta.state.samplerStates, snap.state.samplerStates,
                "sampler state", name);
  requireSubset(delta.state.transforms, snap.state.transforms, "transform",
                name);
  requireSubset(delta.state.lights, snap.state.lights, "light", name);
  requireSubset(delta.state.lightEnables, snap.state.lightEnables,
                "light enable", name);
}

// Property 1. Counted for the categories whose "present" definition is
// unambiguous from the bindings. The stream count is the one c3e18446 restored --
// it is what the chunk-context rebuild used to destroy.
void requireSelfContained(const Built& snap, const std::string& name) {
  check(snap.state.streams.size() == D9C_DRAW_PACKET_MAX_STREAMS,
        name + ": a snapshot record must carry every stream slot (got " +
            std::to_string(snap.state.streams.size()) + " of " +
            std::to_string(D9C_DRAW_PACKET_MAX_STREAMS) +
            "), which is exactly what the chunk-context stream rebuild used to "
            "drop");
  check(snap.state.textures.size() == D9C_DRAW_PACKET_MAX_TEXTURES,
        name + ": a snapshot record must carry every texture slot");
  check(snap.state.renderTargets.size() == D9C_DRAW_PACKET_MAX_RENDER_TARGETS,
        name + ": a snapshot record must carry every render-target slot");
  check(snap.state.depthStencils.size() == 1u,
        name + ": a snapshot record must carry the depth-stencil binding");
  check(snap.state.shaders.size() == 2u,
        name + ": a snapshot record must carry both shader stages");
  check(snap.state.vertexInputs.size() == 1u,
        name + ": a snapshot record must carry the vertex input");
  check((snap.header.flags & D9C_COMMAND_CHUNK_V2_DRAW_FLAG_FULL_SNAPSHOT) !=
            0u,
        name + ": a snapshot record must set FULL_SNAPSHOT in its draw header");
}

void runScenario(const std::string& name, std::uint32_t recordType,
                 const PeHotStateShadow& shadow,
                 const pe::PeChunkContext& chunk) {
  const pe::PeBindingView bindings = fullyBoundView();
  pe::PeDrawParams params{};
  params.recordType = recordType;
  params.primitiveType = 4u;  // D3DPT_TRIANGLELIST
  params.primitiveCount = 6u;
  params.stride = 32u;
  params.numVertices = 64u;

  // Static so the two large scratches do not sit on the stack.
  static Built delta, snap;
  delta = Built{};
  snap = Built{};
  build(delta, shadow, bindings, params, chunk, /*snapshot=*/false);
  build(snap, shadow, bindings, params, chunk, /*snapshot=*/true);
  check(delta.ok, name + ": the delta-mode build must succeed");
  check(snap.ok, name + ": the snapshot-mode build must succeed");

  requireSelfContained(snap, name);
  requireDeltaIsSubsetOfSnapshot(delta, snap, name);
}

// A real SetRenderState writes BOTH the pending set and the full shadow
// (d3d9_pe_device.cpp:12292). Delta mode reads the former and snapshot mode the
// latter, so a fixture that touches only one describes a device state that cannot
// occur -- and then the two modes legitimately disagree. Same duality applies to
// tss, sampler states, transforms, and lightEnable. Setting only the pending side
// is the trap this helper exists to avoid.
void setRenderState(PeHotStateShadow& s, std::uint32_t state,
                    std::uint32_t value) {
  s.pendingRenderStates.set(state, value);
  s.renderStateShadow.set(state, value);
}

PeHotStateShadow nothingDirty() { return PeHotStateShadow{}; }

PeHotStateShadow someDirty() {
  PeHotStateShadow s{};
  s.pendingStreamMask = 0x5u;
  s.pendingTextureMask = 0x3u;
  s.pendingVs = true;
  s.pendingVdecl = true;
  s.pendingIb = true;
  s.pendingViewport = true;
  setRenderState(s, 7u, 3u);
  s.pendingRtMask = 0x1u;
  return s;
}

PeHotStateShadow everythingDirty() {
  PeHotStateShadow s{};
  s.pendingStreamMask = (1u << D9C_DRAW_PACKET_MAX_STREAMS) - 1u;
  s.pendingTextureMask = (1u << D9C_DRAW_PACKET_MAX_TEXTURES) - 1u;
  s.pendingVs = true;
  s.pendingPs = true;
  s.pendingVdecl = true;
  s.pendingFvf = true;
  s.pendingIb = true;
  s.pendingViewport = true;
  s.pendingScissor = true;
  s.pendingMaterial = true;
  s.pendingDs = true;
  s.pendingRtMask = (1u << D9C_DRAW_PACKET_MAX_RENDER_TARGETS) - 1u;
  s.pendingClipPlaneMask = 0x3Fu;
  for (std::uint32_t i = 0; i < 8u; ++i) {
    setRenderState(s, i, i + 1u);
  }
  s.lightEnableShadow = 0xFFu;
  s.pendingLightEnableMask = 0xFFu;
  s.pendingLightEnableValidMask = 0xFFu;
  return s;
}

pe::PeChunkContext nothingRetained() { return pe::PeChunkContext{}; }

// The case that actually broke: nothing dirty and everything already retained,
// so the delta-shaped rebuild produced an EMPTY stream set and applied it to the
// snapshot record.
pe::PeChunkContext everythingRetained() {
  pe::PeChunkContext c{};
  c.retainedStreamMask = (1u << D9C_DRAW_PACKET_MAX_STREAMS) - 1u;
  c.indexBufferKnown = true;
  c.indexBufferRetained = true;
  return c;
}

}  // namespace

int main() {
  try {
    const std::array<std::pair<const char*, std::uint32_t>, 3> recordTypes{{
        {"draw", D9C_COMMAND_RECORD_DRAW_PRIMITIVE},
        {"indexed draw", D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE},
        {"applystate", D9C_COMMAND_RECORD_APPLY_STATE},
    }};
    const std::array<std::pair<const char*, PeHotStateShadow>, 3> shadows{{
        {"nothing dirty", nothingDirty()},
        {"some dirty", someDirty()},
        {"everything dirty", everythingDirty()},
    }};
    const std::array<std::pair<const char*, pe::PeChunkContext>, 2> chunks{{
        {"nothing retained", nothingRetained()},
        {"everything retained", everythingRetained()},
    }};
    for (const auto& [typeName, recordType] : recordTypes) {
      for (const auto& [shadowName, shadow] : shadows) {
        for (const auto& [chunkName, chunk] : chunks) {
          runScenario(std::string(typeName) + ", " + shadowName + ", " +
                          chunkName,
                      recordType, shadow, chunk);
        }
      }
    }
  } catch (const TestFailure& failure) {
    std::cerr << "pe_full_snapshot_equivalence_spec FAILED: " << failure.what()
              << "\n";
    return 1;
  }
  std::cout << "pe_full_snapshot_equivalence_spec passed\n";
  return 0;
}
