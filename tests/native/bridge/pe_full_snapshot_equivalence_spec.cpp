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
// This runs the REAL producer -- buildSparseState plus addChunkContextSections,
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
#include "d3d9_pe_tape_bootstrap.hpp"
#include "device_c_chunk_replay.hpp"

#include <algorithm>
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
#include <vector>

// The real builder retains bound objects. Model that ownership with counted
// native fixtures, as the canonical producer differential does; byte-address
// stand-ins alone are suitable only for the non-appending producer tests.
struct CaptureRefCounter { std::uint32_t refs = 1u; };
struct D9CSurface : CaptureRefCounter {};
struct D9CTexture : CaptureRefCounter {};
struct D9CBuffer : CaptureRefCounter {};
struct D9CShader : CaptureRefCounter {};
struct D9CVertexDecl : CaptureRefCounter {};
struct D9CQuery : CaptureRefCounter {};

extern "C" void dxmt9c_surface_addref(D9CSurface* value) { ++value->refs; }
extern "C" std::uint32_t dxmt9c_surface_release(D9CSurface* value) { return --value->refs; }
extern "C" void dxmt9c_texture_addref(D9CTexture* value) { ++value->refs; }
extern "C" std::uint32_t dxmt9c_texture_release(D9CTexture* value) { return --value->refs; }
extern "C" void dxmt9c_buffer_addref(D9CBuffer* value) { ++value->refs; }
extern "C" std::uint32_t dxmt9c_buffer_release(D9CBuffer* value) { return --value->refs; }
extern "C" void dxmt9c_shader_addref(D9CShader* value) { ++value->refs; }
extern "C" std::uint32_t dxmt9c_shader_release(D9CShader* value) { return --value->refs; }
extern "C" void dxmt9c_vdecl_addref(D9CVertexDecl* value) { ++value->refs; }
extern "C" std::uint32_t dxmt9c_vdecl_release(D9CVertexDecl* value) { return --value->refs; }
extern "C" void dxmt9c_query_addref(D9CQuery* value) { ++value->refs; }
extern "C" std::uint32_t dxmt9c_query_release(D9CQuery* value) { return --value->refs; }

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
// aliasing onto a right one. Counted fixtures also support the real builder's
// retain/release ownership while no graphics provider is needed.
D9CTexture texObjects[D9C_DRAW_PACKET_MAX_TEXTURES]{};
D9CBuffer vbObjects[D9C_DRAW_PACKET_MAX_STREAMS]{};
D9CBuffer ibObject{};
D9CShader vsObject{};
D9CShader psObject{};
D9CVertexDecl vdeclObject{};
D9CSurface rtObjects[D9C_DRAW_PACKET_MAX_RENDER_TARGETS]{};
D9CSurface dsObject{};

template <typename Ref>
Ref ref(void* object, std::uint32_t kind, std::uint32_t objectId) {
  Ref r{};
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
    v.textures[i] = ref<pe::TextureRef>(
        &texObjects[i], D9C_CHUNK_HANDLE_KIND_TEXTURE, id++);
  }
  for (std::uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_STREAMS; ++i) {
    v.streams[i].buffer =
        ref<pe::BufferRef>(&vbObjects[i], D9C_CHUNK_HANDLE_KIND_BUFFER, id++);
    v.streams[i].offset = 16u * i;
    v.streams[i].stride = 4u * (i + 1u);
  }
  v.indexBuffer = ref<pe::BufferRef>(
      &ibObject, D9C_CHUNK_HANDLE_KIND_BUFFER, id++);
  v.vs = ref<pe::ShaderRef>(&vsObject, D9C_CHUNK_HANDLE_KIND_SHADER, id++);
  v.ps = ref<pe::ShaderRef>(&psObject, D9C_CHUNK_HANDLE_KIND_SHADER, id++);
  v.vdecl = ref<pe::DeclarationRef>(
      &vdeclObject, D9C_CHUNK_HANDLE_KIND_VERTEX_DECL, id++);
  for (std::uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_RENDER_TARGETS; ++i) {
    v.renderTargets[i] =
        ref<pe::SurfaceRef>(
            &rtObjects[i], D9C_CHUNK_HANDLE_KIND_SURFACE, id++);
    v.rtExplicitMask[i] = true;
  }
  v.depthStencil = ref<pe::SurfaceRef>(
      &dsObject, D9C_CHUNK_HANDLE_KIND_SURFACE, id++);
  v.fvf = 0x142u;
  return v;
}

// One build of the real producer pair. Scratch is per-mode here because the two
// results must coexist to be compared; production reuses a single scratch, and
// the differential covers that reuse.
struct Built {
  pe::PeSparseScratch scratch{};
  pe::SparseStateInput state{};
  D9CCommandChunkWireDrawHeader header{};
  bool ok = false;
};

void build(Built& out, const PeHotStateShadow& shadow,
           const pe::PeBindingView& bindings, const pe::PeDrawParams& params,
           const pe::PeChunkContext& chunk, bool snapshot) {
  PeConstShadowBlock constants{};
  pe::PeDrawPayloads payloads{};
  out.ok = pe::buildSparseState(shadow, constants, bindings, payloads, params,
                                  snapshot, /*inlineConstDelta=*/false,
                                  out.scratch, out.header, out.state);
  if (!out.ok) {
    return;
  }
  if (params.recordType != D9C_COMMAND_RECORD_APPLY_STATE) {
    out.ok = pe::addChunkContextSections(chunk, shadow, bindings, params,
                                         out.scratch, out.state);
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
  check((snap.header.flags & D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT) !=
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
  s.writer().pendingRenderStatesTyped().set(renderStateSlotKey(state), value);
  s.writer().renderStateShadowTyped().set(renderStateSlotKey(state), value);
}

PeHotStateShadow nothingDirty() { return PeHotStateShadow{}; }

PeHotStateShadow someDirty() {
  PeHotStateShadow s{};
  s.writer().pendingStreamMask() = 0x5u;
  s.writer().pendingTextureMask() = 0x3u;
  s.writer().pendingVs() = true;
  s.writer().pendingVdecl() = true;
  s.writer().pendingIb() = true;
  s.writer().pendingViewport() = true;
  setRenderState(s, 7u, 3u);
  s.writer().pendingRtMask() = 0x1u;
  return s;
}

PeHotStateShadow everythingDirty() {
  PeHotStateShadow s{};
  s.writer().pendingStreamMask() = (1u << D9C_DRAW_PACKET_MAX_STREAMS) - 1u;
  s.writer().pendingTextureMask() = (1u << D9C_DRAW_PACKET_MAX_TEXTURES) - 1u;
  s.writer().pendingVs() = true;
  s.writer().pendingPs() = true;
  s.writer().pendingVdecl() = true;
  s.writer().pendingFvf() = true;
  s.writer().pendingIb() = true;
  s.writer().pendingViewport() = true;
  s.writer().pendingScissor() = true;
  s.writer().pendingMaterial() = true;
  s.writer().pendingDs() = true;
  s.writer().pendingRtMask() = (1u << D9C_DRAW_PACKET_MAX_RENDER_TARGETS) - 1u;
  s.writer().pendingClipPlaneMask() = 0x3Fu;
  for (std::uint32_t i = 0; i < 8u; ++i) {
    setRenderState(s, i, i + 1u);
  }
  s.writer().lightEnableShadow() = 0xFFu;
  s.writer().pendingLightEnableMask() = 0xFFu;
  s.writer().pendingLightEnableValidMask() = 0xFFu;
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

void renderTapeSnapshotCarriesCompleteConstantsWithoutDrainingDirtyState() {
  PeConstShadowBlock constants{};
  const auto initialize = [](ConstShadow& shadow, std::size_t elementBytes,
                             std::uint32_t registers) {
    shadow.values.resize(elementBytes * registers);
    for (std::size_t i = 0u; i < shadow.values.size(); ++i) {
      shadow.values[i] = static_cast<std::uint8_t>(i + registers);
    }
    shadow.dirtyStart = 1u;
    shadow.dirtyEnd = registers;
  };
  initialize(constants.vsConstF, 16u, 4u);
  initialize(constants.vsConstI, 16u, 3u);
  initialize(constants.vsConstB, 4u, 5u);
  initialize(constants.psConstF, 16u, 6u);
  initialize(constants.psConstI, 16u, 2u);
  initialize(constants.psConstB, 4u, 7u);

  static pe::PeSparseScratch scratch{};
  pe::SparseStateInput state{};
  D9CCommandChunkWireDrawHeader header{};
  check(pe::buildFullSnapshotState(PeHotStateShadow{}, constants,
                                   fullyBoundView(), scratch, header, state),
        "Render Tape full snapshot build must succeed");
  const std::array ranges{
      std::pair{state.vsFloatConstants.registerCount, 4u},
      std::pair{state.vsIntConstants.registerCount, 3u},
      std::pair{state.vsBoolConstants.registerCount, 5u},
      std::pair{state.psFloatConstants.registerCount, 6u},
      std::pair{state.psIntConstants.registerCount, 2u},
      std::pair{state.psBoolConstants.registerCount, 7u},
  };
  check(std::all_of(ranges.begin(), ranges.end(), [](const auto& range) {
          return range.first == range.second;
        }),
        "Render Tape full snapshot must carry every constant register");
  check(constants.vsConstF.dirtyStart == 1u &&
            constants.vsConstF.dirtyEnd == 4u &&
            constants.psConstB.dirtyStart == 1u &&
            constants.psConstB.dirtyEnd == 7u,
        "Render Tape full snapshot must not drain live dirty constant ranges");
  check((header.flags & D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT) != 0u,
        "Render Tape APPLY_STATE snapshot must set FULL_SNAPSHOT");
}

// Public D3D9 render-state ordinals, not the 256-slot shadow's unused holes.
constexpr std::array legalRenderStates{
    7u, 8u, 9u, 14u, 15u, 16u, 19u, 20u, 22u, 23u, 24u, 25u, 26u, 27u,
    28u, 29u, 34u, 35u, 36u, 37u, 38u, 48u, 52u, 53u, 54u, 55u, 56u, 57u,
    58u, 59u, 60u, 128u, 129u, 130u, 131u, 132u, 133u, 134u, 135u, 136u,
    137u, 139u, 140u, 141u, 142u, 143u, 145u, 146u, 147u, 148u, 151u, 152u,
    154u, 155u, 156u, 157u, 158u, 159u, 160u, 161u, 162u, 163u, 165u, 166u,
    167u, 168u, 170u, 171u, 172u, 173u, 174u, 175u, 176u, 178u, 179u, 180u,
    181u, 182u, 183u, 184u, 185u, 186u, 187u, 188u, 189u, 190u, 191u, 192u,
    193u, 194u, 195u, 198u, 199u, 200u, 201u, 202u, 203u, 204u, 205u, 206u,
    207u, 208u, 209u};
constexpr std::array legalTextureStageTypes{
    1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 22u, 23u, 24u,
    26u, 27u, 28u, 32u};

template <typename T>
bool equalBits(const T& left, const T& right) {
  return std::memcmp(&left, &right, sizeof(T)) == 0;
}

std::array<ConstShadow*, 6> constantFamilies(PeConstShadowBlock& constants) {
  return {&constants.vsConstF, &constants.vsConstI, &constants.vsConstB,
          &constants.psConstF, &constants.psConstI, &constants.psConstB};
}

void initializeCaptureConstants(PeConstShadowBlock& constants) {
  const auto families = constantFamilies(constants);
  constexpr std::array elementBytes{16u, 16u, 4u, 16u, 16u, 4u};
  constexpr std::array registerCounts{256u, 16u, 16u, 224u, 16u, 16u};
  for (std::size_t family = 0u; family < families.size(); ++family) {
    auto& range = *families[family];
    range.values.resize(elementBytes[family] * registerCounts[family]);
    for (std::size_t byte = 0u; byte < range.values.size(); ++byte)
      range.values[byte] = static_cast<std::uint8_t>(17u * family + byte);
    range.dirtyElems.assign(registerCounts[family], 0u);
    range.dirtyStart = 2u;
    range.dirtyEnd = 4u;
    range.dirtyElems[2] = range.dirtyElems[3] = 1u;
  }
}

void requireConstantsUnchanged(PeConstShadowBlock& actual,
                               PeConstShadowBlock& before) {
  const auto left = constantFamilies(actual);
  const auto right = constantFamilies(before);
  for (std::size_t family = 0u; family < left.size(); ++family) {
    check(left[family]->values == right[family]->values &&
              left[family]->dirtyElems == right[family]->dirtyElems &&
              left[family]->dirtyStart == right[family]->dirtyStart &&
              left[family]->dirtyEnd == right[family]->dirtyEnd,
          "capture must preserve every constant byte and dirty cursor");
  }
}

PeHotStateShadow captureShadow(std::uint32_t renderCount,
                              std::uint32_t tssCount,
                              std::uint32_t samplerCount,
                              std::uint32_t transformCount) {
  PeHotStateShadow shadow{};
  auto write = shadow.transition();
  for (std::uint32_t i = 0u; i < renderCount; ++i)
    write.setRenderState(renderStateSlotKey(legalRenderStates[i]), 0x10000u + i);
  for (std::uint32_t i = 0u; i < tssCount; ++i)
    write.setTextureStageState(
        textureStageIndexKey(i / legalTextureStageTypes.size()),
        textureStageStateTypeKey(legalTextureStageTypes[i % legalTextureStageTypes.size()]),
        0x20000u + i);
  for (std::uint32_t i = 0u; i < samplerCount; ++i)
    write.setSamplerState(SamplerIndex::fromRaw(i / 13u),
                          SamplerStateType::fromRaw(1u + i % 13u), 0x30000u + i);
  for (std::uint32_t i = 0u; i < transformCount; ++i) {
    D9CMatrix value{};
    for (std::uint32_t component = 0u; component < 16u; ++component)
      value.m[component] = static_cast<float>(i * 16u + component) + 0.5f;
    write.setTransform(transformStateKey(FixedTransformTable::stateForSlot(i)), value);
  }
  write.setViewport(D9CViewport{11u, 12u, 123u, 234u, 0.25f, 0.75f});
  write.setScissor(D9CRect{13, 14, 135, 246});
  write.setMaterial(D9CMaterial{
      {0.1f, 0.2f, 0.3f, 0.4f}, {0.5f, 0.6f, 0.7f, 0.8f},
      {0.9f, 1.0f, 1.1f, 1.2f}, {1.3f, 1.4f, 1.5f, 1.6f}, 17.5f});
  for (std::uint32_t slot = 0u; slot < 6u; ++slot) {
    const float plane[4]{static_cast<float>(slot) + 1.25f, 2.5f, 3.75f, -4.0f};
    write.setClipPlane(slot, plane);
  }
  for (std::uint32_t slot = 0u; slot < D9C_DRAW_PACKET_MAX_LIGHTS; ++slot) {
    const float offset = static_cast<float>(slot);
    write.setLight(slot, D9CLight{
        1u + slot % 3u,
        {offset + 0.1f, 0.2f, 0.3f, 0.4f}, {0.5f, offset + 0.6f, 0.7f, 0.8f},
        {0.9f, 1.0f, offset + 1.1f, 1.2f}, {offset + 1.3f, 1.4f, 1.5f},
        {1.6f, offset + 1.7f, 1.8f}, 19.0f, 2.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f});
    write.setLightEnable(slot, slot % 2u == 0u);
  }
  // Clean historical entries must not vanish just because pending is empty.
  shadow.writer().pendingRenderStatesTyped().clear();
  shadow.writer().pendingTssTyped().clear();
  shadow.writer().pendingSamplerStatesTyped().clear();
  shadow.writer().pendingTransformsTyped().clear();
  return shadow;
}

struct CaptureReplaySink final : dxmt9::d3d9::SparseReplaySink {
  PeHotStateShadow shadow{};
  pe::PeBindingView bindings{};
  PeConstShadowBlock constants{};
  std::array<std::size_t, 4> scalarWrites{};
  std::size_t applies = 0u;

  std::int32_t setRenderStates(std::span<const D9CCommandChunkWireRenderState> rows) override {
    scalarWrites[0] += rows.size();
    for (const auto& row : rows) shadow.transition().setRenderState(renderStateSlotKey(row.state), row.value);
    return 0;
  }
  std::int32_t setTexture(std::uint32_t slot, void* object) override { bindings.textures[slot].object = object; return 0; }
  std::int32_t setStream(const D9CCommandChunkWireStreamBinding& row, void* object) override {
    bindings.streams[row.slot].buffer.object = object;
    bindings.streams[row.slot].offset = row.offset;
    bindings.streams[row.slot].stride = row.stride;
    bindings.streamFrequencies[row.slot] = row.frequency;
    return 0;
  }
  std::int32_t setShader(std::uint32_t stage, void* object) override {
    (stage == D9C_COMMAND_CHUNK_SHADER_STAGE_VERTEX ? bindings.vs : bindings.ps).object = object;
    return 0;
  }
  std::int32_t setVertexInput(std::uint32_t kind, std::uint32_t value, void* object) override {
    if (kind != D9C_COMMAND_CHUNK_VERTEX_INPUT_DECLARATION) return -1;
    bindings.vdecl.object = object; bindings.fvf = value; return 0;
  }
  std::int32_t setIndexBuffer(void* object) override { bindings.indexBuffer.object = object; return 0; }
  std::int32_t setRenderTarget(std::uint32_t slot, void* object) override {
    bindings.renderTargets[slot].object = object; bindings.rtExplicitMask[slot] = true; return 0;
  }
  std::int32_t setDepthStencil(void* object) override { bindings.depthStencil.object = object; return 0; }
  std::int32_t setViewport(const D9CViewport& value) override { shadow.transition().setViewport(value); return 0; }
  std::int32_t setScissor(const D9CRect& value) override { shadow.transition().setScissor(value); return 0; }
  std::int32_t setMaterial(const D9CMaterial& value) override { shadow.transition().setMaterial(value); return 0; }
  std::int32_t setClipPlane(const D9CCommandChunkWireClipPlane& row) override { shadow.transition().setClipPlane(row.slot, row.values); return 0; }
  std::int32_t setTextureStageStates(std::span<const D9CDrawPacketTextureStageState> rows) override {
    scalarWrites[1] += rows.size();
    for (const auto& row : rows) shadow.transition().setTextureStageState(textureStageIndexKey(row.stage), textureStageStateTypeKey(row.type), row.value);
    return 0;
  }
  std::int32_t setSamplerStates(std::span<const D9CDrawPacketSamplerState> rows) override {
    scalarWrites[2] += rows.size();
    for (const auto& row : rows) shadow.transition().setSamplerState(SamplerIndex::fromRaw(row.sampler), SamplerStateType::fromRaw(row.type), row.value);
    return 0;
  }
  std::int32_t setTransforms(std::span<const D9CDrawPacketTransform> rows) override {
    scalarWrites[3] += rows.size();
    for (const auto& row : rows) shadow.transition().setTransform(transformStateKey(row.state), row.matrix);
    return 0;
  }
  std::int32_t setLights(std::span<const D9CCommandChunkWireLight> rows) override {
    for (const auto& row : rows) shadow.transition().setLight(row.slot, row.light);
    return 0;
  }
  std::int32_t setLightEnables(std::span<const D9CCommandChunkWireLightEnable> rows) override {
    for (const auto& row : rows) shadow.transition().setLightEnable(row.slot, row.enabled != 0u);
    return 0;
  }
  std::int32_t setConstants(std::uint16_t kind, const D9CCommandChunkWireConstantRange& range,
                           std::span<const std::byte> bytes) override {
    const auto family = static_cast<std::uint32_t>(kind) -
                        D9C_COMMAND_CHUNK_SECTION_VS_CONST_F;
    if (family >= 6u || range.startRegister != 0u) return -1;
    auto& target = constantFamilies(constants)[family]->values;
    target.resize(bytes.size());
    std::memcpy(target.data(), bytes.data(), bytes.size());
    return 0;
  }
  std::int32_t finishApplyState(std::uint32_t flags) override {
    if ((applies == 0u) != ((flags & D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT) != 0u)) return -1;
    ++applies; return 0;
  }
  std::int32_t draw(const dxmt9::d3d9::SparseDrawCall&) override { return -1; }
};

void requireCaptureStateEqual(const PeHotStateShadow& expected,
                              const pe::PeBindingView& bindings,
                              PeConstShadowBlock& constants,
                              CaptureReplaySink& actual) {
  check(actual.scalarWrites == std::array<std::size_t, 4>{
      expected.renderStateShadowTyped().size(), expected.tssShadowTyped().size(),
      expected.samplerStateShadowTyped().size(), expected.transformShadowTyped().size()},
      "every full-shadow keyed entry must be emitted exactly once");
  expected.renderStateShadowTyped().forEach([&](RenderStateSlot key, std::uint32_t value) {
    std::uint32_t got = 0u;
    check(actual.shadow.renderStateShadowTyped().get(key, got) && got == value,
          "render-state page must preserve exact key/value");
  });
  expected.tssShadowTyped().forEach([&](TextureStageIndex stage, TextureStageStateType type, std::uint32_t value) {
    std::uint32_t got = 0u;
    check(actual.shadow.tssShadowTyped().get(stage, type, got) && got == value,
          "TSS page must preserve exact stage/type/value");
  });
  expected.samplerStateShadowTyped().forEach([&](SamplerIndex slot, SamplerStateType type, std::uint32_t value) {
    std::uint32_t got = 0u;
    check(actual.shadow.samplerStateShadowTyped().get(slot, type, got) && got == value,
          "sampler page must preserve exact slot/type/value");
  });
  expected.transformShadowTyped().forEach([&](TransformState key, const D9CMatrix& value) {
    D9CMatrix got{};
    check(actual.shadow.transformShadowTyped().get(key, got) && equalBits(got, value),
          "transform page must preserve exact matrix bits");
  });
  check(equalBits(expected.viewportShadow(), actual.shadow.viewportShadow()) &&
            equalBits(expected.scissorShadow(), actual.shadow.scissorShadow()) &&
            equalBits(expected.materialShadow(), actual.shadow.materialShadow()) &&
            std::memcmp(expected.clipPlaneShadow(), actual.shadow.clipPlaneShadow(), 6u * 4u * sizeof(float)) == 0 &&
            std::memcmp(expected.lightShadow(), actual.shadow.lightShadow(), D9C_DRAW_PACKET_MAX_LIGHTS * sizeof(D9CLight)) == 0 &&
            expected.lightEnableShadow() == actual.shadow.lightEnableShadow(),
        "anchor must preserve all unique scalar/clip/light sentinels");
  for (std::size_t slot = 0u; slot < bindings.textures.size(); ++slot)
    check(actual.bindings.textures[slot].object == bindings.textures[slot].object,
          "anchor must preserve every texture binding and explicit null");
  for (std::size_t slot = 0u; slot < bindings.streams.size(); ++slot)
    check(actual.bindings.streams[slot].buffer.object == bindings.streams[slot].buffer.object &&
              actual.bindings.streams[slot].offset == bindings.streams[slot].offset &&
              actual.bindings.streams[slot].stride == bindings.streams[slot].stride &&
              actual.bindings.streamFrequencies[slot] == bindings.streamFrequencies[slot],
          "anchor must preserve stream tuple/frequency/null independently");
  check(actual.bindings.vs.object == bindings.vs.object && actual.bindings.ps.object == bindings.ps.object &&
            actual.bindings.vdecl.object == bindings.vdecl.object && actual.bindings.fvf == bindings.fvf &&
            actual.bindings.indexBuffer.object == bindings.indexBuffer.object &&
            actual.bindings.depthStencil.object == bindings.depthStencil.object,
        "anchor must preserve shader/declaration/FVF/index/depth binding or null");
  for (std::size_t slot = 0u; slot < bindings.renderTargets.size(); ++slot)
    check(actual.bindings.renderTargets[slot].object == bindings.renderTargets[slot].object,
          "anchor must preserve explicit render-target binding/null");
  const auto left = constantFamilies(constants);
  const auto right = constantFamilies(actual.constants);
  for (std::size_t family = 0u; family < left.size(); ++family)
    check(left[family]->values == right[family]->values,
          "anchor must preserve all six complete constant vectors");
}

void* resolveCaptureHandle(const D9CCommandChunkWireHandleEntry& handle,
                           const pe::PeBindingView& bindings) {
  void* object = nullptr;
  const auto consider = [&](const pe::PeWireObjectRef& ref) {
    if (ref.object && ref.identity.kind == handle.kind &&
        ref.identity.generation == handle.generation && ref.identity.objectId == handle.objectId)
      object = ref.object;
  };
  for (const auto& ref : bindings.textures) consider(ref);
  for (const auto& stream : bindings.streams) consider(stream.buffer);
  for (const auto& ref : bindings.renderTargets) consider(ref);
  consider(bindings.vs); consider(bindings.ps); consider(bindings.vdecl);
  consider(bindings.indexBuffer); consider(bindings.depthStencil);
  check(object != nullptr, "every emitted capture identity must resolve exactly");
  return object;
}

void runCapturePaginationCase(std::array<std::uint32_t, 4> counts, bool nullBindings) {
  const auto shadow = captureShadow(counts[0], counts[1], counts[2], counts[3]);
  std::array<std::byte, sizeof(shadow)> shadowBefore{};
  std::memcpy(shadowBefore.data(), &shadow, sizeof(shadow));
  auto bindings = fullyBoundView();
  for (std::uint32_t slot = 0u; slot < bindings.streams.size(); ++slot)
    bindings.streamFrequencies[slot] = 0x40000000u | (slot + 2u);
  if (nullBindings) {
    bindings.textures.back() = {}; bindings.streams.back().buffer = {};
    bindings.ps = {}; bindings.vdecl = {}; bindings.indexBuffer = {};
    bindings.depthStencil = {}; bindings.renderTargets.back() = {};
  }
  PeConstShadowBlock constants{};
  initializeCaptureConstants(constants);
  auto constantsBefore = constants;
  pe::CommandChunkBuilder builder({.records = pe::kRenderTapeBootstrapMaxRecords,
                                  .handles = 256u, .payloadBytes = 1024u * 1024u,
                                  .sealedBytes = 1024u * 1024u});
  check(pe::appendRenderTapeBootstrapState(builder, shadow, constants, bindings) ==
            pe::RenderTapeBootstrapBuildStatus::Valid,
        "cold capture pagination must succeed without raising section caps");
  const auto sealed = builder.seal();
  check(sealed.valid(), "paged capture must seal one canonical chunk");
  dxmt9::d3d9::ImportedChunkView chunk{};
  check(dxmt9::d3d9::validateCommandChunk(sealed.blob,
            {.recordCount = sealed.recordCount, .handleCount = sealed.handleCount}, &chunk).valid(),
        "every anchor/page must pass the real canonical validator");
  std::vector<void*> objects;
  for (const auto& handle : chunk.handles) objects.push_back(resolveCaptureHandle(handle, bindings));
  const dxmt9::d3d9::ResolvedChunkView resolved{chunk, objects};
  CaptureReplaySink replay{};
  // Seed stale non-null bindings so null tests prove real unbinding, not
  // merely agreement with a freshly zeroed test sink.
  const auto stale = reinterpret_cast<void*>(std::uintptr_t{0xdead});
  for (auto& ref : replay.bindings.textures) ref.object = stale;
  for (auto& stream : replay.bindings.streams) stream.buffer.object = stale;
  for (auto& ref : replay.bindings.renderTargets) ref.object = stale;
  replay.bindings.vs.object = replay.bindings.ps.object = stale;
  replay.bindings.vdecl.object = replay.bindings.indexBuffer.object = stale;
  replay.bindings.depthStencil.object = stale;
  for (std::size_t record = 0u; record < chunk.records.size(); ++record) {
    const auto wire = chunk.record(record);
    check(wire.header.type == D9C_COMMAND_RECORD_APPLY_STATE && !wire.sections.empty(),
          "bootstrap anchor and every continuation must be nonempty state-only records");
    check(dxmt9::d3d9::replaySparseRecord(resolved.record(record), replay) == 0,
          "paged state must accumulate through real sparse record replay");
  }
  requireCaptureStateEqual(shadow, bindings, constants, replay);
  requireConstantsUnchanged(constants, constantsBefore);
  check(std::memcmp(&shadow, shadowBefore.data(), sizeof(shadow)) == 0,
        "capture must not mutate any live full/pending shadow byte or ticket");
  check(chunk.records.size() <= pe::kRenderTapeBootstrapMaxRecords,
        "complete legal domains fit the conservative record bound");
}

void renderTapePaginationPreservesExactState() {
  static_assert(pe::kRenderTapeBootstrapMaxRecords == 35u);
  static_assert(pe::kRenderTapeBootstrapMaxRecords <= 64u);
  static_assert(std::tuple_size_v<decltype(pe::PeSparseScratch{}.samplerStates)> == 64u);
  for (const auto counts : {
           std::array<std::uint32_t, 4>{0u, 0u, 0u, 0u},
           std::array<std::uint32_t, 4>{1u, 1u, 1u, 1u},
           std::array<std::uint32_t, 4>{64u, 64u, 64u, 16u},
           std::array<std::uint32_t, 4>{65u, 65u, 65u, 17u},
           std::array<std::uint32_t, 4>{43u, 12u, 83u, 3u},
           std::array<std::uint32_t, 4>{static_cast<std::uint32_t>(legalRenderStates.size()),
               kPeTextureStageSlots * static_cast<std::uint32_t>(legalTextureStageTypes.size()),
               kPeSamplerSlots * 13u, kPeTransformSlots}}) {
    runCapturePaginationCase(counts, false);
    runCapturePaginationCase(counts, true);
  }
}

void renderTapePaginationRejectsBeforePublishing() {
  const auto shadow = captureShadow(43u, 12u, 83u, 3u);
  std::array<std::byte, sizeof(shadow)> shadowBefore{};
  std::memcpy(shadowBefore.data(), &shadow, sizeof(shadow));
  const auto bindings = fullyBoundView();
  PeConstShadowBlock constants{};
  initializeCaptureConstants(constants);
  auto constantsBefore = constants;
  // Warm-capacity constructor values are reserve hints, not hard limits.
  // The real exact-layout builder enforces both dimensions deterministically.
  for (const auto plan : {
           pe::planExactCommandChunkLayout(1u, 256u, 1024u * 1024u),
           pe::planExactCommandChunkLayout(pe::kRenderTapeBootstrapMaxRecords, 256u, 1u)}) {
    pe::CommandChunkBuilder builder(plan);
    check(pe::appendRenderTapeBootstrapState(builder, shadow, constants, bindings) ==
              pe::RenderTapeBootstrapBuildStatus::AppendRejected,
          "bounded builder rejection must remain a capture append failure");
    check(builder.recordCount() == (plan.recordCount == 1u ? 1u : 0u),
          "failed temporary builder may contain only its accepted prefix");
    requireConstantsUnchanged(constants, constantsBefore);
    check(std::memcmp(&shadow, shadowBefore.data(), sizeof(shadow)) == 0,
          "append failure must preserve every live shadow byte/ticket");
    // The builder is deliberately discarded; a prefix is never a checkpoint.
  }

  constexpr std::array elementBytes{16u, 16u, 4u, 16u, 16u, 4u};
  for (std::size_t family = 0u; family < 6u; ++family) {
    for (const auto excessBytes : {1u, elementBytes[family]}) {
      auto malformed = constants;
      constantFamilies(malformed)[family]->values.resize(
          constantFamilies(malformed)[family]->values.size() + excessBytes);
      auto before = malformed;
      pe::CommandChunkBuilder builder({.records = pe::kRenderTapeBootstrapMaxRecords,
          .handles = 256u, .payloadBytes = 1024u * 1024u,
          .sealedBytes = 1024u * 1024u});
      check(pe::appendRenderTapeBootstrapState(builder, shadow, malformed, bindings) ==
                pe::RenderTapeBootstrapBuildStatus::SnapshotRejected &&
                builder.recordCount() == 0u,
            "every malformed/over-cap constant family must reject before the anchor");
      requireConstantsUnchanged(malformed, before);
    }
  }
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
    renderTapeSnapshotCarriesCompleteConstantsWithoutDrainingDirtyState();
    renderTapePaginationPreservesExactState();
    renderTapePaginationRejectsBeforePublishing();
  } catch (const TestFailure& failure) {
    std::cerr << "pe_full_snapshot_equivalence_spec FAILED: " << failure.what()
              << "\n";
    return 1;
  }
  std::cout << "pe_full_snapshot_equivalence_spec passed\n";
  return 0;
}
