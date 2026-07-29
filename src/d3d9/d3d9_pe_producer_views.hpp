#pragma once

// POD inputs for the PE sparse-state producer.
//
// The producer is pure over (a) bindings, (b) the constant shadow, and
// (c) draw payloads. Destination-chunk state (d) is NOT a producer input:
// production applies it in the draw call sites' writer lambdas, after the
// producer runs, and never for APPLY_STATE
// (d3d9_pe_device.cpp:9349/9363/9422/9437 versus the barrier path at :10000).
// It is therefore an input to addChunkContextSections instead. See
// docs/superpowers/specs/2026-07-29-pe-legacy-record-removal-design.md §3.
//
// PeChunkContext is passed as data rather than reached for through
// CommandChunkV2Builder so a native differential test can drive it.

#include "d3d9_pe_chunk_v2_builder.hpp"
#include "dxmt9/device_c.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace dxmt9::d3d9::pe {

// The wire form of the device's streamSrc_ / streamOff_ / streamStr_ triple.
struct PeStreamBinding {
  PeWireObjectRef buffer{};
  std::uint32_t offset = 0u;
  std::uint32_t stride = 0u;
};

// (a) COM-derived bindings, already translated by the device. The producer
// forwards `object` to CommandChunkV2Builder::appendHandle, which owns
// retention; it never dereferences it.
struct PeBindingView {
  std::array<PeWireObjectRef, D9C_DRAW_PACKET_MAX_TEXTURES> textures{};
  std::array<PeStreamBinding, D9C_DRAW_PACKET_MAX_STREAMS> streams{};
  PeWireObjectRef vs{};
  PeWireObjectRef ps{};
  PeWireObjectRef vdecl{};
  PeWireObjectRef indexBuffer{};
  PeWireObjectRef depthStencil{};
  std::array<PeWireObjectRef, D9C_DRAW_PACKET_MAX_RENDER_TARGETS>
      renderTargets{};
  std::uint32_t rtExplicitMask = 0u;
  std::uint32_t fvf = 0u;
};

// (d) Destination-chunk history, consumed by addChunkContextSections only.
// Mirrors what d3d9_pe_device.cpp:9462-9484 computes today.
//
// submittedIndexBufferWire holds the POINTER-valued wire, as
// d9cWireHandleValue(toWireHandle(rawIBuf(indexBuf_))) does at :9401-9406 --
// not an objectId. Comparing the wrong one makes the predicate always differ,
// so every indexed draw re-emits its index binding.
struct PeChunkContext {
  std::uint32_t retainedStreamMask = 0u;
  bool indexBufferKnown = false;
  std::uint64_t submittedIndexBufferWire = 0u;
};

// (c) Draw payloads. Borrowed for the producer call and the append that
// consumes its output. Never stored.
struct PeDrawPayloads {
  std::span<const std::byte> upIndex{};
  std::span<const std::byte> upVertex{};
};

// Per-draw scalars. APPLY_STATE passes this default-constructed and the
// producer leaves the draw header alone. baseVertex is signed to match
// D9CCommandChunkWireDrawHeaderV2::baseVertex (int32_t); a negative
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

// Device-owned, reused output storage. The SparseStateV2Input spans the
// producer fills point into these arrays, so the scratch must outlive the
// append that consumes them. Capacities match the V2 section caps exactly;
// the producer returns false rather than truncating.
//
// Single-element arrays are used for the scalar sections (viewport, scissor,
// material, index buffer, depth stencil) so every category is addressed the
// same way and SparseStateV2Input's spans can point at them uniformly.
struct PeSparseScratch {
  std::array<D9CCommandChunkWireRenderStateV2,
             D9C_DRAW_PACKET_MAX_RENDER_STATES> renderStates{};
  std::array<SparseBindingV2Input<D9CCommandChunkWireTextureBindingV2>,
             D9C_DRAW_PACKET_MAX_TEXTURES> textures{};
  std::array<SparseBindingV2Input<D9CCommandChunkWireStreamBindingV2>,
             D9C_DRAW_PACKET_MAX_STREAMS> streams{};
  // Two shader stages (vertex, pixel) and two vertex-input kinds (FVF,
  // declaration); neither has a D9C_DRAW_PACKET_MAX_* cap of its own.
  std::array<SparseBindingV2Input<D9CCommandChunkWireShaderBindingV2>, 2>
      shaders{};
  std::array<SparseBindingV2Input<D9CCommandChunkWireVertexInputV2>, 2>
      vertexInputs{};
  std::array<SparseBindingV2Input<D9CCommandChunkWireIndexBindingV2>, 1>
      indexBuffers{};
  std::array<SparseBindingV2Input<D9CCommandChunkWireRenderTargetBindingV2>,
             D9C_DRAW_PACKET_MAX_RENDER_TARGETS> renderTargets{};
  std::array<SparseBindingV2Input<D9CCommandChunkWireDepthStencilBindingV2>, 1>
      depthStencils{};
  std::array<D9CViewport, 1> viewports{};
  std::array<D9CRect, 1> scissors{};
  std::array<D9CMaterial, 1> materials{};
  // D3D9 exposes six clip planes; the packet carries clipPlanes[6 * 4] floats.
  std::array<D9CCommandChunkWireClipPlaneV2, 6> clipPlanes{};
  std::array<D9CDrawPacketTextureStageState, D9C_DRAW_PACKET_MAX_TSS>
      textureStageStates{};
  std::array<D9CDrawPacketSamplerState, D9C_DRAW_PACKET_MAX_SAMPLER>
      samplerStates{};
  std::array<D9CDrawPacketTransform, D9C_DRAW_PACKET_MAX_TRANSFORMS>
      transforms{};
  std::array<D9CCommandChunkWireLightV2, D9C_DRAW_PACKET_MAX_LIGHTS> lights{};
  std::array<D9CCommandChunkWireLightEnableV2, D9C_DRAW_PACKET_MAX_LIGHTS>
      lightEnables{};
};

}  // namespace dxmt9::d3d9::pe
