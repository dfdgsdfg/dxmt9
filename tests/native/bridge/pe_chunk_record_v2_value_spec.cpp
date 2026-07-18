#include "d3d9_pe_chunk_v2_builder.hpp"
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

}  // namespace

int main() {
  try {
    testCachedIdentityBuilderAndSeal();
    testInvalidIdentityAndExplicitRollback();
  } catch (const TestFailure& error) {
    std::cerr << "pe_chunk_record_v2_value_spec failed: " << error.what()
              << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "pe_chunk_record_v2_value_spec passed\n";
  return EXIT_SUCCESS;
}
