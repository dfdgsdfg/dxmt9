#include "d3d9_pe_chunk_v2_builder.hpp"
#include "device_c_chunk_v2_registry.hpp"
#include "device_c_chunk_v2_replay.hpp"
#include "device_c_replay_offload.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::atomic<bool> gCountAllocations{false};
std::atomic<std::size_t> gAllocationCount{0u};

void noteAllocation() noexcept {
  if (gCountAllocations.load(std::memory_order_relaxed))
    gAllocationCount.fetch_add(1u, std::memory_order_relaxed);
}

void* allocate(std::size_t size) {
  noteAllocation();
  if (void* value = std::malloc(size ? size : 1u)) return value;
  throw std::bad_alloc();
}

void* allocateAligned(std::size_t size, std::size_t alignment) {
  noteAllocation();
  void* value = nullptr;
  if (posix_memalign(&value, alignment, size ? size : alignment) != 0)
    throw std::bad_alloc();
  return value;
}

}  // namespace

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
  return allocateAligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return allocateAligned(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
void operator delete(void* value, std::align_val_t) noexcept {
  std::free(value);
}
void operator delete[](void* value, std::align_val_t) noexcept {
  std::free(value);
}
void operator delete(void* value, std::size_t, std::align_val_t) noexcept {
  std::free(value);
}
void operator delete[](void* value, std::size_t, std::align_val_t) noexcept {
  std::free(value);
}

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
using dxmt9::d3d9::ResolvedChunkV2View;
using dxmt9::d3d9::SparseDrawCallV2;
using dxmt9::d3d9::V2ChunkEnvelope;
using dxmt9::d3d9::V2ValidationScratch;
using dxmt9::d3d9::WireObjectRegistry;
using dxmt9::d3d9::pe::CommandChunkV2Builder;
using dxmt9::d3d9::pe::PeWireObjectRef;
using dxmt9::d3d9::pe::SparseBindingV2Input;
using dxmt9::d3d9::pe::SparseStateV2Input;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) throw TestFailure(std::string(message));
}

class CountingSink final : public dxmt9::d3d9::SparseReplaySinkV2 {
 public:
  std::uint32_t drawCount = 0u;
  std::uint32_t textureCount = 0u;

  std::int32_t setRenderStates(
      std::span<const D9CCommandChunkWireRenderStateV2>) override {
    return 0;
  }
  std::int32_t setTexture(std::uint32_t, void*) override {
    ++textureCount;
    return 0;
  }
  std::int32_t setStream(
      const D9CCommandChunkWireStreamBindingV2&, void*) override {
    return 0;
  }
  std::int32_t setShader(std::uint32_t, void*) override { return 0; }
  std::int32_t setVertexInput(std::uint32_t, std::uint32_t, void*) override {
    return 0;
  }
  std::int32_t setIndexBuffer(void*) override { return 0; }
  std::int32_t setRenderTarget(std::uint32_t, void*) override { return 0; }
  std::int32_t setDepthStencil(void*) override { return 0; }
  std::int32_t setViewport(const D9CViewport&) override { return 0; }
  std::int32_t setScissor(const D9CRect&) override { return 0; }
  std::int32_t setMaterial(const D9CMaterial&) override { return 0; }
  std::int32_t setClipPlane(
      const D9CCommandChunkWireClipPlaneV2&) override {
    return 0;
  }
  std::int32_t setTextureStageStates(
      std::span<const D9CDrawPacketTextureStageState>) override {
    return 0;
  }
  std::int32_t setSamplerStates(
      std::span<const D9CDrawPacketSamplerState>) override {
    return 0;
  }
  std::int32_t setTransforms(
      std::span<const D9CDrawPacketTransform>) override {
    return 0;
  }
  std::int32_t setLights(
      std::span<const D9CCommandChunkWireLightV2>) override {
    return 0;
  }
  std::int32_t setLightEnables(
      std::span<const D9CCommandChunkWireLightEnableV2>) override {
    return 0;
  }
  std::int32_t setConstants(
      std::uint16_t, const D9CCommandChunkWireConstantRangeV2&,
      std::span<const std::byte>) override {
    return 0;
  }
  std::int32_t finishApplyState(std::uint32_t) override { return 0; }
  std::int32_t draw(const SparseDrawCallV2&) override {
    ++drawCount;
    return 0;
  }
};

struct Fixture {
  WireObjectRegistry registry;
  D9CTexture texture;
  PeWireObjectRef textureRef;
  CommandChunkV2Builder builder;
  V2ValidationScratch validationScratch;
  std::array<void*, 1u> resolved{};
  CountingSink sink;
  std::array<D9CCommandChunkWireRenderStateV2, 1u> renderStates{{
      {.state = 7u, .value = 11u},
  }};
  std::array<SparseBindingV2Input<
                 D9CCommandChunkWireTextureBindingV2>,
             1u>
      textures{};

  Fixture() {
    textureRef = PeWireObjectRef{
        .identity = registry.insert(D9C_CHUNK_HANDLE_KIND_TEXTURE, &texture),
        .object = &texture,
    };
    textures[0] = {
        .wire = {.slot = 0u, .valid = 1u},
        .object = textureRef,
    };
  }

  ~Fixture() {
    builder.reset();
    registry.erase(textureRef.identity, &texture);
  }
};

bool appendRepresentativeCorpus(Fixture& fixture) {
  const SparseStateV2Input singleState{
      .renderStates = fixture.renderStates,
      .textures = fixture.textures,
  };
  const D9CCommandChunkWireDrawHeaderV2 first{
      .primitiveType = 4u,
      .startVertex = 3u,
      .primitiveCount = 1u,
  };
  const D9CCommandChunkWireDrawHeaderV2 second{
      .primitiveType = 4u,
      .startVertex = 6u,
      .primitiveCount = 1u,
  };
  return dxmt9::d3d9::pe::appendSparseRecordV2(
             fixture.builder, D9C_COMMAND_RECORD_DRAW_PRIMITIVE, first,
             singleState) &&
         dxmt9::d3d9::pe::appendSparseRecordV2(
             fixture.builder, D9C_COMMAND_RECORD_DRAW_PRIMITIVE, second, {});
}

bool runRepresentativeCorpus(Fixture& fixture) {
  fixture.builder.reset();
  if (!appendRepresentativeCorpus(fixture)) return false;
  const auto sealed = fixture.builder.seal();
  if (!sealed.valid()) return false;
  const V2ChunkEnvelope envelope{
      .version = D9C_COMMAND_CHUNK_VERSION_V2,
      .recordCount = sealed.recordCount,
      .handleCount = sealed.handleCount,
  };
  ImportedChunkV2View imported;
  if (!dxmt9::d3d9::validateCommandChunkV2(
           sealed.blob, envelope, &imported,
           fixture.validationScratch).valid() ||
      imported.handles.size() != fixture.resolved.size()) {
    return false;
  }
  const auto retain = +[](std::uint32_t, void* object) noexcept {
    addRef(static_cast<D9CTexture*>(object));
  };
  if (!fixture.registry.resolveAndRetain(imported.handles, fixture.resolved,
                                         retain)) {
    return false;
  }
  const ResolvedChunkV2View resolved{
      .wire = imported,
      .objects = fixture.resolved,
  };
  bool replayed = true;
  for (std::size_t i = 0u; i < imported.records.size(); ++i)
    replayed &= dxmt9::d3d9::replaySparseRecordV2(
                    resolved.record(i), fixture.sink) == 0;
  release(static_cast<D9CTexture*>(fixture.resolved[0]));
  fixture.resolved[0] = nullptr;
  fixture.builder.reset();
  return replayed;
}

std::size_t sealedSize(Fixture& fixture, const SparseStateV2Input& state) {
  fixture.builder.reset();
  const D9CCommandChunkWireDrawHeaderV2 draw{
      .primitiveType = 4u,
      .primitiveCount = 1u,
  };
  if (!dxmt9::d3d9::pe::appendSparseRecordV2(
          fixture.builder, D9C_COMMAND_RECORD_DRAW_PRIMITIVE, draw, state)) {
    return 0u;
  }
  return fixture.builder.seal().blob.size();
}

void testWarmRecordImportReplayAllocations() {
  Fixture fixture;

  dxmt9::d3d9::ReplayOffloadQueue queue(2u, 1u << 20);
  dxmt9::d3d9::RawCommandChunk warmChunk;
  warmChunk.recordBytes = 1u;
  check(queue.push(std::move(warmChunk)), "offload queue warm push succeeds");
  dxmt9::d3d9::RawCommandChunk popped;
  check(queue.pop(popped), "offload queue warm pop succeeds");
  queue.markReplayDone();

  check(runRepresentativeCorpus(fixture),
        "representative V2 corpus warms every measured capacity");
  const auto drawsBefore = fixture.sink.drawCount;
  const auto texturesBefore = fixture.sink.textureCount;

  gAllocationCount.store(0u, std::memory_order_relaxed);
  gCountAllocations.store(true, std::memory_order_release);
  bool repeated = true;
  for (std::uint32_t i = 0u; i < 256u; ++i)
    repeated &= runRepresentativeCorpus(fixture);
  gCountAllocations.store(false, std::memory_order_release);

  check(repeated, "bounded warm V2 corpus records, imports, and replays");
  check(gAllocationCount.load(std::memory_order_relaxed) == 0u,
        "warm V2 record/import/replay performs zero system allocations");
  check(fixture.sink.drawCount - drawsBefore == 512u &&
            fixture.sink.textureCount - texturesBefore == 256u &&
            fixture.texture.refs == 1u,
        "warm corpus replays every draw and balances both ownership layers");
}

void testSparseDrawWireSizeReduction() {
  Fixture fixture;
  const SparseStateV2Input singleState{
      .renderStates = fixture.renderStates,
      .textures = fixture.textures,
  };
  const auto singleStateBytes = sealedSize(fixture, singleState);
  const auto noStateBytes = sealedSize(fixture, {});
  check(singleStateBytes != 0u && noStateBytes != 0u &&
            noStateBytes < singleStateBytes,
        "sparse V2 state adds bytes only when a section is present");
}

}  // namespace

int main() {
  try {
    testWarmRecordImportReplayAllocations();
    testSparseDrawWireSizeReduction();
  } catch (const TestFailure& error) {
    gCountAllocations.store(false, std::memory_order_relaxed);
    std::cerr << "chunk_record_v2_allocation_spec failed: " << error.what()
              << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "chunk_record_v2_allocation_spec passed\n";
  return EXIT_SUCCESS;
}
