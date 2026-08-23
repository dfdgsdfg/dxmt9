#pragma once

// POD inputs for the PE sparse-state producer.
//
// The producer is pure over (a) bindings, (b) the constant shadow, and
// (c) draw payloads. Destination-chunk state (d) is NOT a producer input:
// production applies it inside the draw call sites' writer lambdas, after the
// producer runs, and never on the barrier path. Compare the four callers of
// d3d9_pe_device.cpp's populatePendingChunkDrawStreamDependencies against
// chunkBarrierFlush, which builds an APPLY_STATE packet with no chunk-context
// step at all. Chunk context is therefore an input to
// addChunkContextSections instead. See
// docs/superpowers/specs/2026-07-29-pe-legacy-record-removal-design.md §3.
//
// References here name symbols rather than line numbers on purpose: this
// migration moves large blocks of d3d9_pe_device.cpp, so line citations go
// stale within a task or two.
//
// PeChunkContext is passed as data rather than reached for through
// CommandChunkBuilder so a native differential test can drive it.

#include "d3d9_pe_chunk_builder.hpp"
#include "d3d9_pe_state_shadow.hpp"
#include "device_c_chunk_schema.hpp"
#include "dxmt9/device_c.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>

namespace dxmt9::d3d9::pe {

// Per-render-target "was this slot explicitly set" flags. A bool array rather
// than a bitmask, matching the device's currentRtExplicitMask(). This lived in
// d3d9_pe_draw_packet.hpp until Task 10 deleted that header: it was the only
// declaration there that outlived the fat packet, and it belongs next to the
// PeBindingView field that holds it.
using PeRtExplicitMask =
    std::array<bool, D9C_DRAW_PACKET_MAX_RENDER_TARGETS>;

// The wire form of the device's streamSrc_ / streamOff_ / streamStr_ triple.
struct PeStreamBinding {
  BufferRef buffer{};
  std::uint32_t offset = 0u;
  std::uint32_t stride = 0u;
};

// (a) COM-derived bindings, already translated by the device. The producer
// forwards `object` to CommandChunkBuilder::appendHandle, which owns
// retention; it never dereferences it.
struct PeBindingView {
  std::array<TextureRef, D9C_DRAW_PACKET_MAX_TEXTURES> textures{};
  std::array<PeStreamBinding, D9C_DRAW_PACKET_MAX_STREAMS> streams{};
  ShaderRef vs{};
  ShaderRef ps{};
  DeclarationRef vdecl{};
  BufferRef indexBuffer{};
  SurfaceRef depthStencil{};
  std::array<SurfaceRef, D9C_DRAW_PACKET_MAX_RENDER_TARGETS>
      renderTargets{};
  // Per-slot bool array, NOT a bitmask: this mirrors the device's
  // currentRtExplicitMask(), whose type is PeRtExplicitMask
  // (std::array<bool, 4>), and populateDrawPacketAttachmentSnapshot binds it
  // by const reference.
  PeRtExplicitMask rtExplicitMask{};
  std::uint32_t fvf = 0u;
};

// (d) Destination-chunk history, consumed by addChunkContextSections only.
// Mirrors what d3d9_pe_device.cpp's populatePendingChunkDrawStreamDependencies
// and populatePendingChunkDrawIndexDependency compute today.
//
// submittedIndexBufferWire holds the POINTER-valued wire, matching what the
// device stores in submittedIndexBufferWireValue_ via
// d9cWireHandleValue(toWireHandle(rawIBuf(indexBuf_))) -- not an objectId.
// Comparing the wrong one makes the predicate always differ, so every indexed
// draw re-emits its index binding.
struct PeChunkContext {
  std::uint32_t retainedStreamMask = 0u;
  bool indexBufferKnown = false;
  std::uint64_t submittedIndexBufferWire = 0u;
  // Whether the DESTINATION chunk already references the currently bound index
  // buffer. This is a second, independent reason to emit the index section:
  // populateDrawPacketIndexDependency sets ibValid when the handle is non-null
  // and the chunk has not retained it, regardless of pendingIb / known /
  // changed. Omitting it lets a fresh chunk's first indexed draw with an
  // unchanged IB emit no section, so that chunk never retains the buffer.
  bool indexBufferRetained = false;
};

// (c) Draw payloads. Borrowed for the producer call and the append that
// consumes its output. Never stored.
struct PeDrawPayloads {
  std::span<const std::byte> upIndex{};
  std::span<const std::byte> upVertex{};
};

// Per-draw scalars. APPLY_STATE passes this default-constructed and the
// producer leaves the draw header alone. baseVertex is signed to match
// D9CCommandChunkWireDrawHeader::baseVertex (int32_t); a negative
// BaseVertexIndex is legal in D3D9.
struct PeDrawParams {
  std::uint32_t recordType = 0u;
  std::uint32_t primitiveType = 0u;
  std::int32_t baseVertex = 0;
  std::uint32_t minVertex = 0u;
  std::uint32_t numVertices = 0u;
  std::uint32_t startVertex = 0u;
  std::uint32_t startIndex = 0u;
  std::uint32_t primitiveCount = 0u;
  std::uint32_t stride = 0u;
  std::uint32_t indexFormat = 0u;
};

// Device-owned, reused output storage. The SparseStateInput spans the
// producer fills point into these arrays, so the scratch must outlive the
// append that consumes them. The producer returns false rather than
// truncating.
//
// Every capacity is asserted against the canonical schema's own maxCount below, not
// against a hand-copied D9C_DRAW_PACKET_MAX_* value. Oversizing a category is
// not a harmless slack: appendSparseRecord rejects a span longer than
// maxCount through validSectionCount(), so the draw would be dropped at
// runtime. Undersizing truncates. Both are caught at compile time now.
//
// Single-element arrays are used for the scalar sections (viewport, scissor,
// material, index buffer, depth stencil, vertex input) so every category is
// addressed the same way and SparseStateInput's spans can point at them
// uniformly.
struct PeSparseScratch {
  std::array<D9CCommandChunkWireRenderState,
             D9C_DRAW_PACKET_MAX_RENDER_STATES> renderStates{};
  std::array<SparseBindingInput<D9CCommandChunkWireTextureBinding>,
             D9C_DRAW_PACKET_MAX_TEXTURES> textures{};
  std::array<SparseBindingInput<D9CCommandChunkWireStreamBinding>,
             D9C_DRAW_PACKET_MAX_STREAMS> streams{};
  // Two shader stages (vertex, pixel).
  std::array<SparseBindingInput<D9CCommandChunkWireShaderBinding>, 2>
      shaders{};
  // ONE vertex input, not two. The section is SectionRuleSingle with
  // maxCount 1: FVF and vertex declaration are the two values of the entry's
  // `kind` field, not two entries. Declaration wins when both are dirty, and
  // `value` carries the FVF either way -- see buildSparseState's vertex-input
  // block in d3d9_pe_producer.cpp.
  std::array<SparseBindingInput<D9CCommandChunkWireVertexInput>, 1>
      vertexInputs{};
  std::array<SparseBindingInput<D9CCommandChunkWireIndexBinding>, 1>
      indexBuffers{};
  std::array<SparseBindingInput<D9CCommandChunkWireRenderTargetBinding>,
             D9C_DRAW_PACKET_MAX_RENDER_TARGETS> renderTargets{};
  std::array<SparseBindingInput<D9CCommandChunkWireDepthStencilBinding>, 1>
      depthStencils{};
  std::array<D9CViewport, 1> viewports{};
  std::array<D9CRect, 1> scissors{};
  std::array<D9CMaterial, 1> materials{};
  // D3D9 exposes six clip planes; the packet carries clipPlanes[6 * 4] floats.
  std::array<D9CCommandChunkWireClipPlane, 6> clipPlanes{};
  std::array<D9CDrawPacketTextureStageState, D9C_DRAW_PACKET_MAX_TSS>
      textureStageStates{};
  std::array<D9CDrawPacketSamplerState, D9C_DRAW_PACKET_MAX_SAMPLER>
      samplerStates{};
  std::array<D9CDrawPacketTransform, D9C_DRAW_PACKET_MAX_TRANSFORMS>
      transforms{};
  std::array<D9CCommandChunkWireLight, D9C_DRAW_PACKET_MAX_LIGHTS> lights{};
  std::array<D9CCommandChunkWireLightEnable, D9C_DRAW_PACKET_MAX_LIGHTS>
      lightEnables{};
};

// Tie every scratch capacity to the canonical schema rather than to a copied macro,
// so a schema change breaks the build here instead of dropping draws at
// runtime. sectionRule is constexpr, so this costs nothing.
namespace detail {

constexpr std::uint32_t sectionMaxCount(std::uint16_t kind) {
  const auto* rule = sectionRule(kind);
  return rule != nullptr ? rule->maxCount : 0u;
}

#define DXMT9_ASSERT_SCRATCH_CAP(member, sectionKind)                        \
  static_assert(std::tuple_size_v<decltype(PeSparseScratch::member)> ==      \
                    sectionMaxCount(sectionKind),                            \
                "PeSparseScratch::" #member                                  \
                " must equal the canonical schema maxCount for " #sectionKind)

DXMT9_ASSERT_SCRATCH_CAP(renderStates, D9C_COMMAND_CHUNK_SECTION_RENDER_STATE);
DXMT9_ASSERT_SCRATCH_CAP(textures, D9C_COMMAND_CHUNK_SECTION_TEXTURE);
DXMT9_ASSERT_SCRATCH_CAP(streams, D9C_COMMAND_CHUNK_SECTION_STREAM);
DXMT9_ASSERT_SCRATCH_CAP(shaders, D9C_COMMAND_CHUNK_SECTION_SHADER);
DXMT9_ASSERT_SCRATCH_CAP(vertexInputs, D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT);
DXMT9_ASSERT_SCRATCH_CAP(indexBuffers, D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER);
DXMT9_ASSERT_SCRATCH_CAP(renderTargets, D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET);
DXMT9_ASSERT_SCRATCH_CAP(depthStencils, D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL);
DXMT9_ASSERT_SCRATCH_CAP(viewports, D9C_COMMAND_CHUNK_SECTION_VIEWPORT);
DXMT9_ASSERT_SCRATCH_CAP(scissors, D9C_COMMAND_CHUNK_SECTION_SCISSOR);
DXMT9_ASSERT_SCRATCH_CAP(materials, D9C_COMMAND_CHUNK_SECTION_MATERIAL);
DXMT9_ASSERT_SCRATCH_CAP(clipPlanes, D9C_COMMAND_CHUNK_SECTION_CLIP_PLANE);
DXMT9_ASSERT_SCRATCH_CAP(textureStageStates,
                         D9C_COMMAND_CHUNK_SECTION_TEXTURE_STAGE_STATE);
DXMT9_ASSERT_SCRATCH_CAP(samplerStates, D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE);
DXMT9_ASSERT_SCRATCH_CAP(transforms, D9C_COMMAND_CHUNK_SECTION_TRANSFORM);
DXMT9_ASSERT_SCRATCH_CAP(lights, D9C_COMMAND_CHUNK_SECTION_LIGHT);
DXMT9_ASSERT_SCRATCH_CAP(lightEnables, D9C_COMMAND_CHUNK_SECTION_LIGHT_ENABLE);

#undef DXMT9_ASSERT_SCRATCH_CAP

// PeBindingView::textures indexes the PE shadow's texture slots, so the two
// must agree; today both are 20 but nothing else ties them together.
static_assert(std::tuple_size_v<decltype(PeBindingView::textures)> ==
                  kPeTextureSlots,
              "PeBindingView::textures must cover every PE texture slot");

}  // namespace detail

}  // namespace dxmt9::d3d9::pe
