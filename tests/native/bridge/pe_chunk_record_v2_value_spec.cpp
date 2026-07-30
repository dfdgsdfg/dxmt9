#include "d3d9_pe_chunk_v2_builder.hpp"
#include "d3d9_pe_const_shadow.hpp"
#include "device_c_chunk_v2_validate.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

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

extern "C" void dxmt9c_surface_addref(D9CSurface* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_surface_release(D9CSurface* value) {
  return release(value);
}
extern "C" void dxmt9c_texture_addref(D9CTexture* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_texture_release(D9CTexture* value) {
  return release(value);
}
extern "C" void dxmt9c_buffer_addref(D9CBuffer* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_buffer_release(D9CBuffer* value) {
  return release(value);
}
extern "C" void dxmt9c_shader_addref(D9CShader* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_shader_release(D9CShader* value) {
  return release(value);
}
extern "C" void dxmt9c_vdecl_addref(D9CVertexDecl* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_vdecl_release(D9CVertexDecl* value) {
  return release(value);
}
extern "C" void dxmt9c_query_addref(D9CQuery* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_query_release(D9CQuery* value) {
  return release(value);
}

namespace {

using dxmt9::d3d9::ImportedChunkV2View;
using dxmt9::d3d9::V2ChunkEnvelope;
using dxmt9::d3d9::pe::CommandChunkV2Builder;
using dxmt9::d3d9::pe::PeWireObjectRef;
using dxmt9::d3d9::pe::SparseBindingV2Input;
using dxmt9::d3d9::pe::SparseStateV2Input;
using dxmt9::d3d9::pe::cacheWireObjectRef;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

std::uint32_t getterCalls = 0u;

int32_t getTextureIdentity(D9CTexture* texture,
                           D9CWireObjectIdentity* identity) {
  ++getterCalls;
  if (!texture || !identity) {
    return -1;
  }
  const auto ordinal = static_cast<std::uint64_t>(getterCalls);
  *identity = D9CWireObjectIdentity{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
      .generation = 7u,
      .objectId = 0x700000000ull + ordinal,
  };
  return 0;
}

bool containsPointerBytes(std::span<const std::byte> blob,
                          const void* pointer) {
  const auto value = reinterpret_cast<std::uintptr_t>(pointer);
  std::array<std::byte, sizeof(value)> bytes{};
  std::memcpy(bytes.data(), &value, sizeof(value));
  return std::search(blob.begin(), blob.end(), bytes.begin(), bytes.end()) !=
         blob.end();
}

PeWireObjectRef wireRef(void* object, std::uint32_t kind,
                        std::uint64_t objectId) {
  return PeWireObjectRef{
      .identity = D9CWireObjectIdentity{
          .kind = kind,
          .generation = 11u,
          .objectId = objectId,
      },
      .object = object,
  };
}

D9CWireHandle wireHandle(std::uint64_t value) {
  return D9CWireHandle{
      .lo = static_cast<std::uint32_t>(value),
      .hi = static_cast<std::uint32_t>(value >> 32u),
  };
}

void testCachedIdentityBuilderAndSeal() {
  getterCalls = 0u;
  D9CTexture first;
  D9CTexture second;
  D9CTexture rollbackOnly;
  PeWireObjectRef firstRef;
  PeWireObjectRef secondRef;
  PeWireObjectRef rollbackRef;
  check(cacheWireObjectRef(&first, D9C_CHUNK_HANDLE_KIND_TEXTURE,
                           getTextureIdentity, firstRef) &&
            cacheWireObjectRef(&second, D9C_CHUNK_HANDLE_KIND_TEXTURE,
                               getTextureIdentity, secondRef) &&
            cacheWireObjectRef(&rollbackOnly,
                               D9C_CHUNK_HANDLE_KIND_TEXTURE,
                               getTextureIdentity, rollbackRef),
        "child construction caches each typed identity once");
  check(getterCalls == 3u, "identity getter count equals wrapper count");

  CommandChunkV2Builder builder;
  std::uint32_t firstIndex = 0u;
  std::uint32_t secondIndex = 0u;
  std::uint32_t duplicateIndex = 0u;
  check(builder.beginRecord(D9C_COMMAND_RECORD_UPDATE_TEXTURE) &&
            builder.appendHandle(firstRef, D9C_CHUNK_HANDLE_KIND_TEXTURE,
                                 firstIndex) &&
            builder.appendHandle(secondRef, D9C_CHUNK_HANDLE_KIND_TEXTURE,
                                 secondIndex) &&
            builder.appendHandle(firstRef, D9C_CHUNK_HANDLE_KIND_TEXTURE,
                                 duplicateIndex),
        "first record appends cached object references");
  check(firstIndex == 0u && secondIndex == 1u && duplicateIndex == firstIndex &&
            builder.handleCount() == 2u,
        "record-local handles are deduplicated into absolute indices");
  const D9CCommandChunkWireUpdateTextureV2 firstUpdate{
      .srcHandleIndex = firstIndex,
      .dstHandleIndex = secondIndex,
  };
  check(builder.appendPayloadValue(firstUpdate) && builder.commitRecord(),
        "first fixed record commits");
  check(first.refs == 2u && second.refs == 2u,
        "chunk retainer AddRefs each unique wrapper once");

  std::uint32_t repeatedFirst = 0u;
  std::uint32_t repeatedSecond = 0u;
  check(builder.beginRecord(D9C_COMMAND_RECORD_UPDATE_TEXTURE) &&
            builder.appendHandle(firstRef, D9C_CHUNK_HANDLE_KIND_TEXTURE,
                                 repeatedFirst) &&
            builder.appendHandle(secondRef, D9C_CHUNK_HANDLE_KIND_TEXTURE,
                                 repeatedSecond),
        "later record may repeat the same identities");
  const D9CCommandChunkWireUpdateTextureV2 secondUpdate{
      .srcHandleIndex = repeatedFirst,
      .dstHandleIndex = repeatedSecond,
  };
  check(builder.appendPayloadValue(secondUpdate) && builder.commitRecord(),
        "second fixed record commits");
  check(repeatedFirst == 2u && repeatedSecond == 3u &&
            builder.handleCount() == 4u,
        "later record receives a distinct canonical handle slice");
  check(first.refs == 2u && second.refs == 2u,
        "chunk-wide retainer eliminates duplicate AddRefs across records");
  check(getterCalls == 3u,
        "record and draw appends never call identity getters");

  const auto recordCount = builder.recordCount();
  const auto handleCount = builder.handleCount();
  const auto payloadBytes = builder.payloadBytes();
  std::uint32_t rollbackIndex = 0u;
  check(builder.beginRecord(D9C_COMMAND_RECORD_UPDATE_TEXTURE) &&
            builder.appendHandle(rollbackRef,
                                 D9C_CHUNK_HANDLE_KIND_TEXTURE,
                                 rollbackIndex),
        "failed record acquires a checkpoint suffix");
  const std::uint32_t tooSmall = rollbackIndex;
  check(builder.appendPayloadValue(tooSmall) && !builder.commitRecord(),
        "undersized record fails commit and rolls back");
  check(builder.recordCount() == recordCount &&
            builder.handleCount() == handleCount &&
            builder.payloadBytes() == payloadBytes &&
            rollbackOnly.refs == 1u,
        "rollback restores every arena and releases only its suffix");

  const auto sealed = builder.seal();
  check(sealed.valid() && sealed.recordCount == 2u &&
            sealed.handleCount == 4u && builder.sealed(),
        "builder seals one immutable table/table/arena blob");
  check(!containsPointerBytes(sealed.blob, &first) &&
            !containsPointerBytes(sealed.blob, &second),
        "sealed V2 bytes contain no PE or D9C wrapper address");

  ImportedChunkV2View imported;
  const auto validation = dxmt9::d3d9::validateCommandChunkV2(
      sealed.blob,
      V2ChunkEnvelope{
          .version = D9C_COMMAND_CHUNK_VERSION_V2,
          .recordCount = sealed.recordCount,
          .handleCount = sealed.handleCount,
      },
      &imported);
  check(validation.valid() && imported.records.size() == 2u &&
            imported.handles[0].objectId == firstRef.identity.objectId &&
            imported.handles[2].objectId == firstRef.identity.objectId,
        "sealed builder output passes transactional unix validation");
  check(!builder.beginRecord(D9C_COMMAND_RECORD_UPDATE_TEXTURE),
        "sealed blob rejects mutation until reset");

  builder.reset();
  check(first.refs == 1u && second.refs == 1u &&
            builder.recordCount() == 0u && builder.handleCount() == 0u &&
            !builder.sealed(),
        "reset releases retained wrappers while preserving builder capacity");
}

void testInvalidIdentityAndExplicitRollback() {
  D9CTexture texture;
  PeWireObjectRef invalid{
      .identity = D9CWireObjectIdentity{
          .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
          .generation = 0u,
          .objectId = 1u,
      },
      .object = &texture,
  };
  CommandChunkV2Builder builder;
  std::uint32_t index = 0u;
  check(builder.beginRecord(D9C_COMMAND_RECORD_UPDATE_TEXTURE) &&
            !builder.appendHandle(invalid, D9C_CHUNK_HANDLE_KIND_TEXTURE,
                                  index) &&
            !builder.recordActive() && builder.handleCount() == 0u &&
            texture.refs == 1u,
        "invalid cached identity fails atomically");

  PeWireObjectRef valid;
  check(cacheWireObjectRef(&texture, D9C_CHUNK_HANDLE_KIND_TEXTURE,
                           getTextureIdentity, valid),
        "valid identity can be cached after a failed record");
  check(builder.beginRecord(D9C_COMMAND_RECORD_UPDATE_TEXTURE) &&
            builder.appendHandle(valid, D9C_CHUNK_HANDLE_KIND_TEXTURE,
                                 index),
        "explicit rollback fixture acquires one wrapper");
  builder.rollbackRecord();
  check(texture.refs == 1u && builder.handleCount() == 0u &&
            builder.payloadBytes() == 0u,
        "explicit rollback releases checkpoint ownership");
}

void testNonDrawProducerMatrix() {
  D9CTexture srcTexture;
  D9CTexture dstTexture;
  D9CSurface srcSurface;
  D9CSurface dstSurface;
  D9CQuery query;
  const auto srcTextureRef = wireRef(
      &srcTexture, D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x810000001ull);
  const auto dstTextureRef = wireRef(
      &dstTexture, D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x810000002ull);
  const auto srcSurfaceRef = wireRef(
      &srcSurface, D9C_CHUNK_HANDLE_KIND_SURFACE, 0x810000003ull);
  const auto dstSurfaceRef = wireRef(
      &dstSurface, D9C_CHUNK_HANDLE_KIND_SURFACE, 0x810000004ull);
  const auto queryRef =
      wireRef(&query, D9C_CHUNK_HANDLE_KIND_QUERY, 0x810000005ull);

  CommandChunkV2Builder builder;
  std::array<std::byte, 16> oneRegister{};
  const std::array constantTypes = {
      D9C_COMMAND_RECORD_SET_VS_CONST_F,
      D9C_COMMAND_RECORD_SET_VS_CONST_I,
      D9C_COMMAND_RECORD_SET_VS_CONST_B,
      D9C_COMMAND_RECORD_SET_PS_CONST_F,
      D9C_COMMAND_RECORD_SET_PS_CONST_I,
      D9C_COMMAND_RECORD_SET_PS_CONST_B,
  };
  for (const auto type : constantTypes) {
    const auto bytes =
        type == D9C_COMMAND_RECORD_SET_VS_CONST_B ||
                type == D9C_COMMAND_RECORD_SET_PS_CONST_B
            ? std::span<const std::byte>(oneRegister).first(4u)
            : std::span<const std::byte>(oneRegister);
    check(dxmt9::d3d9::pe::appendSetConstantsV2(
              builder, type, 0u, 1u, bytes),
          "each standalone constant opcode has a V2 producer");
  }

  const std::array rects = {
      D9CRect{0, 0, 32, 32},
      D9CRect{32, 32, 64, 64},
  };
  D9CCommandChunkWireClearV2 clear{
      .flags = 1u,
      .colorARGB = 0xff102030u,
      .z = 0.5f,
      .stencil = 3u,
  };
  check(dxmt9::d3d9::pe::appendClearV2(builder, clear, rects),
        "Clear V2 producer appends typed rect tail");

  D9CCommandChunkWirePresentV2 present{
      .hwnd = 0x1234u,
      .flags = 2u,
      .hasSrc = 1u,
      .hasDst = 0u,
      .reserved0 = 99u,
      .src = D9CRect{0, 0, 640, 480},
  };
  check(dxmt9::d3d9::pe::appendPresentV2(builder, present),
        "Present V2 producer canonicalizes reserved bytes");

  D9CCommandChunkWireStretchRectV2 stretch{
      .hasSrcRect = 1u,
      .hasDstRect = 1u,
      .filter = 2u,
      .srcRect = D9CRect{0, 0, 16, 16},
      .dstRect = D9CRect{4, 4, 20, 20},
  };
  check(dxmt9::d3d9::pe::appendStretchRectV2(
            builder, stretch, srcSurfaceRef, dstSurfaceRef),
        "StretchRect V2 producer uses surface indices");

  D9CCommandChunkWireColorFillV2 colorFill{
      .colorARGB = 0xffaabbccu,
      .hasRect = 1u,
      .rect = D9CRect{1, 2, 3, 4},
  };
  check(dxmt9::d3d9::pe::appendColorFillV2(
            builder, colorFill, dstSurfaceRef) &&
            dxmt9::d3d9::pe::appendUpdateTextureV2(
                builder, srcTextureRef, dstTextureRef),
        "ColorFill and UpdateTexture V2 producers append");

  D9CCommandChunkWireUpdateSurfaceV2 updateSurface{
      .hasSrcRect = 1u,
      .hasDstPoint = 1u,
      .srcRect = D9CRect{0, 0, 8, 8},
      .dstPoint = D9CRect{8, 9, 0, 0},
  };
  check(dxmt9::d3d9::pe::appendUpdateSurfaceV2(
            builder, updateSurface, srcSurfaceRef, dstSurfaceRef) &&
            dxmt9::d3d9::pe::appendQueryIssueV2(builder, 1u, queryRef) &&
            dxmt9::d3d9::pe::appendReadbackV2(
                builder, srcSurfaceRef, dstSurfaceRef) &&
            dxmt9::d3d9::pe::appendReszDepthResolveV2(
                builder, srcSurfaceRef, dstTextureRef),
        "remaining fixed non-draw V2 producers append");

  const auto sealed = builder.seal();
  check(sealed.valid() && sealed.recordCount == 15u,
        "non-draw producer matrix seals all fixed opcodes");
  ImportedChunkV2View imported;
  const auto validation = dxmt9::d3d9::validateCommandChunkV2(
      sealed.blob,
      V2ChunkEnvelope{
          .version = D9C_COMMAND_CHUNK_VERSION_V2,
          .recordCount = sealed.recordCount,
          .handleCount = sealed.handleCount,
      },
      &imported);
  check(validation.valid() &&
            imported.records.back().type ==
                D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE,
        "complete fixed non-draw producer output passes unix preflight");
  check(srcTexture.refs == 2u && dstTexture.refs == 2u &&
            srcSurface.refs == 2u && dstSurface.refs == 2u &&
            query.refs == 2u,
        "non-draw matrix retains each wrapper once per chunk");
  builder.reset();
  check(srcTexture.refs == 1u && dstTexture.refs == 1u &&
            srcSurface.refs == 1u && dstSurface.refs == 1u &&
            query.refs == 1u,
        "non-draw matrix releases all cached wrapper ownership");
}

void testSparseDrawAndApplyProducerMatrix() {
  D9CTexture texture;
  D9CBuffer buffer;
  D9CShader shader;
  D9CVertexDecl vertexDecl;
  D9CSurface surface;
  const auto textureRef = wireRef(
      &texture, D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x820000001ull);
  const auto bufferRef = wireRef(
      &buffer, D9C_CHUNK_HANDLE_KIND_BUFFER, 0x820000002ull);
  const auto shaderRef = wireRef(
      &shader, D9C_CHUNK_HANDLE_KIND_SHADER, 0x820000003ull);
  const auto vertexDeclRef = wireRef(
      &vertexDecl, D9C_CHUNK_HANDLE_KIND_VERTEX_DECL, 0x820000004ull);
  const auto surfaceRef = wireRef(
      &surface, D9C_CHUNK_HANDLE_KIND_SURFACE, 0x820000005ull);

  std::array<SparseBindingV2Input<D9CCommandChunkWireTextureBindingV2>,
             D9C_DRAW_PACKET_MAX_TEXTURES>
      textures{};
  for (std::uint32_t slot = 0u; slot < textures.size(); ++slot) {
    textures[slot].wire.slot = slot;
    textures[slot].wire.valid = 1u;
  }
  textures[0].object = textureRef;

  std::array<SparseBindingV2Input<D9CCommandChunkWireStreamBindingV2>,
             D9C_DRAW_PACKET_MAX_STREAMS>
      streams{};
  for (std::uint32_t slot = 0u; slot < streams.size(); ++slot) {
    streams[slot].wire.slot = slot;
    streams[slot].wire.valid = 1u;
    streams[slot].wire.frequency = 1u;
  }
  streams[0].wire.stride = 16u;
  streams[0].object = bufferRef;

  const std::array shaders = {
      SparseBindingV2Input<D9CCommandChunkWireShaderBindingV2>{
          .wire = {.stage = D9C_COMMAND_CHUNK_V2_SHADER_STAGE_VERTEX,
                   .valid = 1u},
          .object = shaderRef,
      },
      SparseBindingV2Input<D9CCommandChunkWireShaderBindingV2>{
          .wire = {.stage = D9C_COMMAND_CHUNK_V2_SHADER_STAGE_PIXEL,
                   .valid = 1u},
          .object = shaderRef,
      },
  };
  const std::array vertexInputs = {
      SparseBindingV2Input<D9CCommandChunkWireVertexInputV2>{
          .wire = {
              .valid = 1u,
              .kind = D9C_COMMAND_CHUNK_V2_VERTEX_INPUT_DECLARATION,
          },
          .object = vertexDeclRef,
      },
  };
  const std::array indexBuffers = {
      SparseBindingV2Input<D9CCommandChunkWireIndexBindingV2>{
          .wire = {.valid = 1u},
          .object = bufferRef,
      },
  };
  const std::array renderTargets = {
      SparseBindingV2Input<D9CCommandChunkWireRenderTargetBindingV2>{
          .wire = {.slot = 0u, .valid = 1u},
          .object = surfaceRef,
      },
  };
  const std::array depthStencils = {
      SparseBindingV2Input<D9CCommandChunkWireDepthStencilBindingV2>{
          .wire = {.valid = 1u},
          .object = surfaceRef,
      },
  };
  const std::array renderStates = {
      D9CCommandChunkWireRenderStateV2{.state = 7u, .value = 1u},
  };
  const std::array viewports = {
      D9CViewport{.width = 640u, .height = 480u, .maxZ = 1.0f},
  };
  const std::array scissors = {
      D9CRect{0, 0, 640, 480},
  };
  const std::array materials = {D9CMaterial{}};
  const std::array clipPlanes = {
      D9CCommandChunkWireClipPlaneV2{.slot = 0u,
                                     .values = {0.0f, 1.0f, 0.0f, 0.0f}},
  };
  const std::array textureStageStates = {
      D9CDrawPacketTextureStageState{.stage = 0u, .type = 1u, .value = 2u},
  };
  const std::array samplerStates = {
      D9CDrawPacketSamplerState{.sampler = 0u, .type = 1u, .value = 2u},
  };
  const std::array transforms = {
      D9CDrawPacketTransform{.state = 2u},
  };
  const std::array lights = {
      D9CCommandChunkWireLightV2{.slot = 0u},
  };
  const std::array lightEnables = {
      D9CCommandChunkWireLightEnableV2{.slot = 0u, .enabled = 1u},
  };
  const std::array<std::byte, 16> wideRegister{};
  const std::array<std::byte, 4> boolRegister{};

  SparseStateV2Input fullState{
      .renderStates = renderStates,
      .textures = textures,
      .streams = streams,
      .shaders = shaders,
      .vertexInputs = vertexInputs,
      .indexBuffers = indexBuffers,
      .renderTargets = renderTargets,
      .depthStencils = depthStencils,
      .viewports = viewports,
      .scissors = scissors,
      .materials = materials,
      .clipPlanes = clipPlanes,
      .textureStageStates = textureStageStates,
      .samplerStates = samplerStates,
      .transforms = transforms,
      .lights = lights,
      .lightEnables = lightEnables,
      .vsFloatConstants = {0u, 1u, wideRegister},
      .vsIntConstants = {0u, 1u, wideRegister},
      .vsBoolConstants = {0u, 1u, boolRegister},
      .psFloatConstants = {0u, 1u, wideRegister},
      .psIntConstants = {0u, 1u, wideRegister},
      .psBoolConstants = {0u, 1u, boolRegister},
  };

  CommandChunkV2Builder builder;
  D9CCommandChunkWireDrawHeaderV2 direct{
      .flags = D9C_COMMAND_CHUNK_V2_DRAW_FLAG_FULL_SNAPSHOT,
      .primitiveType = 4u,
      .startVertex = 2u,
      .primitiveCount = 1u,
  };
  check(dxmt9::d3d9::pe::appendSparseRecordV2(
            builder, D9C_COMMAND_RECORD_DRAW_PRIMITIVE, direct, fullState),
        "full-snapshot DrawPrimitive V2 producer emits every state section");

  D9CCommandChunkWireDrawHeaderV2 indexed{
      .primitiveType = 4u,
      .baseVertex = -1,
      .numVertices = 3u,
      .primitiveCount = 1u,
  };
  check(dxmt9::d3d9::pe::appendSparseRecordV2(
            builder, D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE, indexed, {}),
        "DrawIndexedPrimitive V2 producer accepts an empty delta");

  const std::array<std::byte, 12> vertices{};
  D9CCommandChunkWireDrawHeaderV2 up{
      .primitiveType = 4u,
      .primitiveCount = 1u,
      .stride = 4u,
  };
  SparseStateV2Input upState{.upVertexData = vertices};
  check(dxmt9::d3d9::pe::appendSparseRecordV2(
            builder, D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP, up, upState),
        "DrawPrimitiveUP V2 producer emits its exact vertex byte range");

  const std::array<std::byte, 6> indices{};
  D9CCommandChunkWireDrawHeaderV2 indexedUp{
      .primitiveType = 4u,
      .numVertices = 3u,
      .primitiveCount = 1u,
      .stride = 4u,
      .indexFormat = 101u,
  };
  SparseStateV2Input indexedUpState{
      .upIndexData = indices,
      .upVertexData = vertices,
  };
  check(dxmt9::d3d9::pe::appendSparseRecordV2(
            builder, D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
            indexedUp, indexedUpState) &&
            dxmt9::d3d9::pe::appendApplyStateV2(
                builder, 0u,
                SparseStateV2Input{.renderStates = renderStates}),
        "DrawIndexedPrimitiveUP and APPLY_STATE V2 producers append");

  const auto sealed = builder.seal();
  ImportedChunkV2View imported;
  const auto validation = dxmt9::d3d9::validateCommandChunkV2(
      sealed.blob,
      V2ChunkEnvelope{
          .version = D9C_COMMAND_CHUNK_VERSION_V2,
          .recordCount = sealed.recordCount,
          .handleCount = sealed.handleCount,
      },
      &imported);
  check(validation.valid() && sealed.recordCount == 5u &&
            imported.record(0u).sections.size() ==
                D9C_COMMAND_CHUNK_V2_SECTION_UP_INDEX_DATA - 1u &&
            imported.records.back().type == D9C_COMMAND_RECORD_APPLY_STATE,
        "all sparse producers seal a canonical pointer-free V2 chunk");
  check(texture.refs == 2u && buffer.refs == 2u && shader.refs == 2u &&
            vertexDecl.refs == 2u && surface.refs == 2u,
        "draw handle table retains each cached wrapper once per chunk");
  builder.reset();
  check(texture.refs == 1u && buffer.refs == 1u && shader.refs == 1u &&
            vertexDecl.refs == 1u && surface.refs == 1u,
        "draw builder reset releases every retained wrapper");

  std::array duplicateSlots = {
      SparseBindingV2Input<D9CCommandChunkWireTextureBindingV2>{
          .wire = {.slot = 0u, .valid = 1u},
      },
      SparseBindingV2Input<D9CCommandChunkWireTextureBindingV2>{
          .wire = {.slot = 0u, .valid = 1u},
      },
  };
  check(!dxmt9::d3d9::pe::appendApplyStateV2(
            builder, 0u, SparseStateV2Input{.textures = duplicateSlots}) &&
            !builder.recordActive() && builder.recordCount() == 0u,
        "noncanonical sparse input rolls back transactionally");
}

// testPeStateStagingFeedsV2Producer lived here. It tested only the fat-packet
// staging helpers (populateDrawPacketAttachmentDelta / ...StreamDependencies /
// ...IndexDependency), which Task 10 deleted along with the format they staged.
// The properties it asserted did not disappear with it: "retained streams stay
// absent from later staged draws" and "the first indexed draw checkpoints its
// index dependency" are now addChunkContextSections' contract, covered by
// pe_producer_differential_spec's stream-retention and six index-buffer fixtures,
// which exercise the real production path rather than a staging helper.

void testConstShadowFeedsV2ConstantSections() {
  const std::array<float, 8> values{
      1.0f, -2.0f, 3.0f, -4.0f, 5.0f, -6.0f, 7.0f, -8.0f};
  ConstShadow shadow;
  touchConstShadow(shadow, 4u, 2u, values.data(), sizeof(float) * 4u);
  check(shadow.dirty() && shadow.dirtyStart == 4u && shadow.dirtyEnd == 6u,
        "constant shadow tracks the merged dirty range");

  D9CDrawPacketConstDeltaSection section{};
  std::array<std::uint8_t, 64> payload{};
  std::uint32_t payloadBytes = 0u;
  check(foldConstShadowIntoDeltaSection(
            shadow, D9C_DRAW_PACKET_CONST_DELTA_VS_F, sizeof(float) * 4u,
            section, payload.data(), payload.size(), payloadBytes) &&
            section.valid == 1u && section.startRegister == 4u &&
            section.registerCount == 2u && payloadBytes == sizeof(values) &&
            std::memcmp(payload.data(), values.data(), sizeof(values)) == 0,
        "constant staging produces the exact V2 section range and bytes");
  check(!shadow.dirty(), "successful fold clears the staged dirty range");

  CommandChunkV2Builder builder;
  SparseStateV2Input state{
      .vsFloatConstants = {
          section.startRegister,
          section.registerCount,
          std::as_bytes(std::span(values)),
      },
  };
  D9CCommandChunkWireDrawHeaderV2 draw{
      .primitiveType = 4u,
      .primitiveCount = 1u,
  };
  check(dxmt9::d3d9::pe::appendSparseRecordV2(
            builder, D9C_COMMAND_RECORD_DRAW_PRIMITIVE, draw, state),
        "staged constant range appends through the direct V2 producer");
}

}  // namespace

int main() {
  try {
    testCachedIdentityBuilderAndSeal();
    testInvalidIdentityAndExplicitRollback();
    testNonDrawProducerMatrix();
    testSparseDrawAndApplyProducerMatrix();
    testConstShadowFeedsV2ConstantSections();
  } catch (const TestFailure& error) {
    std::cerr << "pe_chunk_record_v2_value_spec failed: " << error.what()
              << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "pe_chunk_record_v2_value_spec passed\n";
  return EXIT_SUCCESS;
}
