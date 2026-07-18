#include "device_c_chunk_v2_registry.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

struct FakeObject {
  std::atomic<std::uint32_t> retains{0u};
};

void retainFake(std::uint32_t, void* object) noexcept {
  static_cast<FakeObject*>(object)->retains.fetch_add(
      1u, std::memory_order_relaxed);
}

void testInsertResolveEraseReuse() {
  dxmt9::d3d9::WireObjectRegistry registry;
  FakeObject texture;
  FakeObject buffer;
  FakeObject replacement;

  const auto textureIdentity =
      registry.insert(D9C_CHUNK_HANDLE_KIND_TEXTURE, &texture);
  const auto bufferIdentity =
      registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, &buffer);
  check(textureIdentity.objectId != 0u && textureIdentity.generation == 1u,
        "texture receives a nonzero generation-1 identity");
  check(bufferIdentity.objectId != textureIdentity.objectId,
        "live registry objects have distinct IDs");
  check(registry.activeCount() == 2u, "registry tracks active object count");

  const std::array entries = {
      dxmt9::d3d9::wireHandleEntryV2(textureIdentity),
      dxmt9::d3d9::wireHandleEntryV2(bufferIdentity),
  };
  std::array<void*, 2> resolved{};
  check(registry.resolveAndRetain(entries, resolved, retainFake),
        "valid identity batch resolves");
  check(resolved[0] == &texture && resolved[1] == &buffer,
        "resolved batch preserves entry order");
  check(texture.retains == 1u && buffer.retains == 1u,
        "valid batch retains every resolved object");

  check(registry.erase(textureIdentity, &texture),
        "registered texture can be erased");
  check(!registry.contains(textureIdentity),
        "erased identity becomes stale immediately");
  const auto replacementIdentity =
      registry.insert(D9C_CHUNK_HANDLE_KIND_TEXTURE, &replacement);
  check(replacementIdentity.objectId == textureIdentity.objectId,
        "freed slot ID is reused");
  check(replacementIdentity.generation == textureIdentity.generation + 1u,
        "slot generation advances before reuse");
  check(!registry.erase(textureIdentity, &replacement),
        "stale generation cannot erase replacement");
}

void testTransactionalFailureAndWrongKind() {
  dxmt9::d3d9::WireObjectRegistry registry;
  FakeObject texture;
  FakeObject buffer;
  const auto textureIdentity =
      registry.insert(D9C_CHUNK_HANDLE_KIND_TEXTURE, &texture);
  const auto bufferIdentity =
      registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, &buffer);

  auto wrongKind = dxmt9::d3d9::wireHandleEntryV2(bufferIdentity);
  wrongKind.kind = D9C_CHUNK_HANDLE_KIND_TEXTURE;
  const std::array entries = {
      dxmt9::d3d9::wireHandleEntryV2(textureIdentity), wrongKind};
  std::array<void*, 2> resolved{
      reinterpret_cast<void*>(std::uintptr_t{1u}),
      reinterpret_cast<void*>(std::uintptr_t{2u}),
  };
  check(!registry.resolveAndRetain(entries, resolved, retainFake),
        "wrong-kind batch rejects");
  check(texture.retains == 0u && buffer.retains == 0u,
        "failed batch retains no prefix");
  check(resolved[0] == reinterpret_cast<void*>(std::uintptr_t{1u}) &&
            resolved[1] == reinterpret_cast<void*>(std::uintptr_t{2u}),
        "failed batch leaves caller scratch unchanged");
}

void testDuplicateIdentityRetainsEachEntry() {
  dxmt9::d3d9::WireObjectRegistry registry;
  FakeObject texture;
  const auto identity =
      registry.insert(D9C_CHUNK_HANDLE_KIND_TEXTURE, &texture);
  const auto entry = dxmt9::d3d9::wireHandleEntryV2(identity);
  const std::array entries = {entry, entry};
  std::array<void*, 2> resolved{};

  check(registry.resolveAndRetain(entries, resolved, retainFake),
        "duplicate identity entries are valid across record slices");
  check(resolved[0] == &texture && resolved[1] == &texture,
        "duplicate identity resolves consistently");
  check(texture.retains == 2u,
        "each admitted handle-table entry owns one retain");
}

void testCrossDeviceIdentityAndConcurrentErase() {
  dxmt9::d3d9::WireObjectRegistry first;
  dxmt9::d3d9::WireObjectRegistry second;
  FakeObject firstObject;
  FakeObject secondObject;
  const auto firstIdentity =
      first.insert(D9C_CHUNK_HANDLE_KIND_TEXTURE, &firstObject);
  const auto secondIdentity =
      second.insert(D9C_CHUNK_HANDLE_KIND_TEXTURE, &secondObject);
  check(firstIdentity.objectId != secondIdentity.objectId,
        "device-local registries use distinct object-ID namespaces");
  check(!second.contains(firstIdentity),
        "identity from another registry cannot resolve");

  std::atomic<bool> start{false};
  std::thread reader([&] {
    while (!start.load(std::memory_order_acquire)) {
    }
    for (std::uint32_t i = 0; i < 10000u; ++i) {
      (void)first.contains(firstIdentity, &firstObject);
    }
  });
  start.store(true, std::memory_order_release);
  check(first.erase(firstIdentity, &firstObject),
        "erase synchronizes with concurrent lookup");
  reader.join();
  check(!first.contains(firstIdentity),
        "concurrent lookup cannot keep stale identity live");
}

void testGenerationWrapRetiresSlot() {
  const auto normal =
      dxmt9::d3d9::WireObjectRegistry::advanceGeneration(41u);
  check(normal.generation == 42u && !normal.retired,
        "ordinary release advances generation");
  const auto wrapped = dxmt9::d3d9::WireObjectRegistry::advanceGeneration(
      std::numeric_limits<std::uint32_t>::max());
  check(wrapped.retired, "generation wrap retires slot");
}

}  // namespace

int main() {
  try {
    testInsertResolveEraseReuse();
    testTransactionalFailureAndWrongKind();
    testDuplicateIdentityRetainsEachEntry();
    testCrossDeviceIdentityAndConcurrentErase();
    testGenerationWrapRetiresSlot();
  } catch (const TestFailure& error) {
    std::cerr << "chunk_record_v2_registry_spec failed: " << error.what()
              << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "chunk_record_v2_registry_spec passed\n";
  return EXIT_SUCCESS;
}
