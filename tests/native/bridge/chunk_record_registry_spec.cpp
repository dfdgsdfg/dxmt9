#include "device_c_chunk_registry.hpp"
#include "device_c_common.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <thread>

extern "C" int32_t dxmt9p_device_negotiate_command_chunk(
    D9CDevice*, D9CCommandChunkNegotiation*);

namespace census_allocation_probe {
thread_local bool enabled = false;
thread_local std::size_t failAfter = 0;
}

void* operator new(std::size_t size) {
  if (census_allocation_probe::enabled) {
    if (!census_allocation_probe::failAfter) {
      census_allocation_probe::enabled = false;
      throw std::bad_alloc();
    }
    --census_allocation_probe::failAfter;
  }
  if (auto* value = std::malloc(size ? size : 1u)) return value;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { ::operator delete(value); }
void operator delete(void* value, std::size_t) noexcept { ::operator delete(value); }
void operator delete[](void* value, std::size_t) noexcept { ::operator delete(value); }

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

static_assert(dxmt9::d3d9::WireObjectRegistry::kRetainCallbackNonReentrant);
static_assert(std::is_nothrow_invocable_r_v<void,
                                            dxmt9::d3d9::WireObjectRegistry::RetainFn,
                                            std::uint32_t, void*>);

struct FakeObject {
  std::atomic<std::uint32_t> retains{0u};
};

void retainFake(std::uint32_t, void* object) noexcept {
  static_cast<FakeObject*>(object)->retains.fetch_add(
      1u, std::memory_order_relaxed);
}

struct ReentrantRetainProbe {
  dxmt9::d3d9::WireObjectRegistry* registry = nullptr;
  D9CWireObjectIdentity identity{};
  bool insertRejected = false;
  bool eraseRejected = false;
  bool containsRejected = false;
  bool resolveRejected = false;
  bool activeCountRejected = false;
  bool censusRejected = false;
};

ReentrantRetainProbe* gReentrantRetainProbe = nullptr;

void retainWithRejectedRegistryReentry(std::uint32_t kind,
                                       void* object) noexcept {
  auto& probe = *gReentrantRetainProbe;
  probe.insertRejected =
      probe.registry->insert(kind, object).objectId == 0u;
  probe.eraseRejected = !probe.registry->erase(probe.identity, object);
  probe.containsRejected =
      !probe.registry->contains(probe.identity, object);
  const std::array entries = {
      dxmt9::d3d9::wireHandleEntry(probe.identity)};
  std::array<void*, 1> resolved{};
  probe.resolveRejected = !probe.registry->resolveAndRetain(
      entries, resolved, retainFake);
  probe.activeCountRejected = probe.registry->activeCount() == 0u;
  std::vector<dxmt9::d3d9::WireObjectRegistry::RetainedEntry> census;
  probe.censusRejected = !probe.registry->snapshotAndRetainKind(kind, 8u, census, retainFake);
  retainFake(kind, object);
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
      dxmt9::d3d9::wireHandleEntry(textureIdentity),
      dxmt9::d3d9::wireHandleEntry(bufferIdentity),
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

  auto wrongKind = dxmt9::d3d9::wireHandleEntry(bufferIdentity);
  wrongKind.kind = D9C_CHUNK_HANDLE_KIND_TEXTURE;
  const std::array entries = {
      dxmt9::d3d9::wireHandleEntry(textureIdentity), wrongKind};
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
  const auto entry = dxmt9::d3d9::wireHandleEntry(identity);
  const std::array entries = {entry, entry};
  std::array<void*, 2> resolved{};

  check(registry.resolveAndRetain(entries, resolved, retainFake),
        "duplicate identity entries are valid across record slices");
  check(resolved[0] == &texture && resolved[1] == &texture,
        "duplicate identity resolves consistently");
  check(texture.retains == 2u,
        "each admitted handle-table entry owns one retain");
}

void testRetainCallbackReentryFailsClosed() {
  dxmt9::d3d9::WireObjectRegistry registry;
  FakeObject texture;
  const auto identity =
      registry.insert(D9C_CHUNK_HANDLE_KIND_TEXTURE, &texture);
  const std::array entries = {
      dxmt9::d3d9::wireHandleEntry(identity)};
  std::array<void*, 1> resolved{};
  ReentrantRetainProbe probe{
      .registry = &registry,
      .identity = identity,
  };
  gReentrantRetainProbe = &probe;
  const bool retained = registry.resolveAndRetain(
      entries, resolved, retainWithRejectedRegistryReentry);
  gReentrantRetainProbe = nullptr;

  check(retained && resolved[0] == &texture && texture.retains == 1u,
        "outer resolve retains exactly once after rejected callback re-entry");
  check(probe.insertRejected && probe.eraseRejected &&
            probe.containsRejected && probe.resolveRejected &&
            probe.activeCountRejected && probe.censusRejected,
        "every registry method fails closed during a retain callback");
  check(registry.contains(identity, &texture) && registry.activeCount() == 1u,
        "rejected callback re-entry leaves the registry intact");
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
  const auto aboveTwentyFourBits =
      dxmt9::d3d9::WireObjectRegistry::advanceGeneration(0x00ffffffu);
  check(aboveTwentyFourBits.generation == 0x01000000u &&
            !aboveTwentyFourBits.retired,
        "registry generation advances across the retired 24-bit boundary");
  const auto wrapped = dxmt9::d3d9::WireObjectRegistry::advanceGeneration(
      std::numeric_limits<std::uint32_t>::max());
  check(wrapped.generation == std::numeric_limits<std::uint32_t>::max() &&
            wrapped.retired,
        "maximum uint32 generation retires slot without wrapping");
}

void testBoundedCensus() {
  using Registry = dxmt9::d3d9::WireObjectRegistry;
  Registry registry;
  FakeObject first, second, texture;
  const auto old = registry.insert(D9C_CHUNK_HANDLE_KIND_SURFACE, &first);
  registry.insert(D9C_CHUNK_HANDLE_KIND_SURFACE, &second);
  registry.insert(D9C_CHUNK_HANDLE_KIND_TEXTURE, &texture);
  std::vector<Registry::RetainedEntry> rows;
  bool allocationSucceeded = false;
  for (std::size_t allocation = 0; allocation < 8u; ++allocation) {
    census_allocation_probe::failAfter = allocation;
    census_allocation_probe::enabled = true;
    try {
      allocationSucceeded = registry.snapshotAndRetainKind(
          D9C_CHUNK_HANDLE_KIND_SURFACE, 2u, rows, retainFake);
    } catch (const std::bad_alloc&) {}
    census_allocation_probe::enabled = false;
    if (allocationSucceeded) break;
    check(rows.empty() && first.retains == 0u && second.retains == 0u,
        "failure at every census allocation acquires zero references");
  }
  check(allocationSucceeded, "all census allocation points exercised");
  rows.clear(); first.retains = 0; second.retains = 0;
  check(!registry.snapshotAndRetainKind(D9C_CHUNK_HANDLE_KIND_SURFACE, 1u, rows, retainFake) &&
      rows.empty() && first.retains == 0u && second.retains == 0u,
      "over-capacity census retains no partial prefix");
  check(registry.erase(old, &first), "census test retires old generation");
  const auto current = registry.insert(D9C_CHUNK_HANDLE_KIND_SURFACE, &first);
  check(registry.snapshotAndRetainKind(D9C_CHUNK_HANDLE_KIND_SURFACE, 2u, rows, retainFake) &&
      rows.size() == 2u && rows[0].identity.objectId < rows[1].identity.objectId &&
      rows[0].identity.generation == current.generation && current.generation != old.generation &&
      first.retains == 1u && second.retains == 1u && texture.retains == 0u,
      "bounded census retains only live selected-kind generation-qualified rows in identity order");
  check(!registry.snapshotAndRetainKind(D9C_CHUNK_HANDLE_KIND_SURFACE, 2u, rows, retainFake) &&
      first.retains == 1u && second.retains == 1u, "nonempty owner cannot acquire twice");
  rows.clear();
  check(!registry.snapshotAndRetainKind(UINT32_MAX, 2u, rows, retainFake) &&
      !registry.snapshotAndRetainKind(0u, 2u, rows, nullptr), "invalid census inputs reject");
}

void testPerDeviceVersionNegotiation() {
  D9CDevice legacyDevice(nullptr);
  D9CCommandChunkNegotiation legacy{
      .peSupportedVersions = D9C_COMMAND_CHUNK_CAP_CURRENT,
      .pePreferredVersion = D9C_COMMAND_CHUNK_VERSION,
  };
  check(dxmt9p_device_negotiate_command_chunk(&legacyDevice, &legacy) == 0 &&
            legacy.selectedVersion == D9C_COMMAND_CHUNK_VERSION &&
            legacy.selectedTransport ==
                D9C_COMMAND_CHUNK_TRANSPORT_CONTIGUOUS &&
            legacyDevice.commandChunkTransport ==
                D9C_COMMAND_CHUNK_TRANSPORT_CONTIGUOUS,
        "zeroed legacy transport slots negotiate contiguous compatibility");

  D9CDevice deviceStorage(nullptr);
  auto* device = &deviceStorage;
  static_assert(D9C_COMMAND_CHUNK_DEFAULT_WIRE_VERSION ==
                D9C_COMMAND_CHUNK_VERSION);
  D9CCommandChunkNegotiation automatic{
      .peSupportedVersions = D9C_COMMAND_CHUNK_CAP_CURRENT,
      .pePreferredVersion = D9C_COMMAND_CHUNK_DEFAULT_WIRE_VERSION,
      .peSupportedTransports = D9C_COMMAND_CHUNK_TRANSPORT_CONTIGUOUS |
                               D9C_COMMAND_CHUNK_TRANSPORT_SEGMENTED_V1,
      .pePreferredTransport = D9C_COMMAND_CHUNK_TRANSPORT_SEGMENTED_V1,
  };
  check(dxmt9p_device_negotiate_command_chunk(device, &automatic) == 0 &&
            automatic.unixSupportedVersions ==
                D9C_COMMAND_CHUNK_CAP_CURRENT &&
            automatic.selectedVersion == D9C_COMMAND_CHUNK_VERSION &&
            automatic.selectedTransport ==
                D9C_COMMAND_CHUNK_TRANSPORT_SEGMENTED_V1 &&
            device->commandChunkNegotiated,
        "canonical peers negotiate wire version 2 and segmented transport");

  auto repeated = automatic;
  repeated.unixSupportedVersions = 0u;
  repeated.selectedVersion = 0u;
  repeated.unixSupportedTransports = 0u;
  repeated.selectedTransport = 0u;
  check(dxmt9p_device_negotiate_command_chunk(device, &repeated) == 0 &&
            repeated.selectedVersion == D9C_COMMAND_CHUNK_VERSION &&
            repeated.selectedTransport ==
                D9C_COMMAND_CHUNK_TRANSPORT_SEGMENTED_V1,
        "identical negotiation is idempotent");

  D9CCommandChunkNegotiation versionOne{
      .peSupportedVersions = 0x00000001u,
      .pePreferredVersion = 1u,
  };
  check(dxmt9p_device_negotiate_command_chunk(device, &versionOne) < 0 &&
            versionOne.unixSupportedVersions ==
                D9C_COMMAND_CHUNK_CAP_CURRENT &&
            versionOne.selectedVersion == 0u,
        "numeric wire version 1 is rejected");

  auto changedTransport = automatic;
  changedTransport.pePreferredTransport =
      D9C_COMMAND_CHUNK_TRANSPORT_CONTIGUOUS;
  changedTransport.selectedVersion = 0u;
  changedTransport.selectedTransport = 0u;
  check(dxmt9p_device_negotiate_command_chunk(device, &changedTransport) < 0 &&
            changedTransport.selectedVersion == 0u &&
            changedTransport.selectedTransport == 0u &&
            device->commandChunkTransport ==
                D9C_COMMAND_CHUNK_TRANSPORT_SEGMENTED_V1,
        "a second negotiation cannot mutate the latched transport");

  D9CCommandChunkNegotiation unsupported{
      .peSupportedVersions = 0x00000004u,
      .pePreferredVersion = 3u,
  };
  check(dxmt9p_device_negotiate_command_chunk(device, &unsupported) < 0 &&
            unsupported.selectedVersion == 0u,
        "unsupported numeric wire version rejects without fallback");
}

}  // namespace

int main() {
  try {
    testInsertResolveEraseReuse();
    testTransactionalFailureAndWrongKind();
    testDuplicateIdentityRetainsEachEntry();
    testRetainCallbackReentryFailsClosed();
    testCrossDeviceIdentityAndConcurrentErase();
    testGenerationWrapRetiresSlot();
    testBoundedCensus();
    testPerDeviceVersionNegotiation();
  } catch (const TestFailure& error) {
    std::cerr << "chunk_record_registry_spec failed: " << error.what()
              << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "chunk_record_registry_spec passed\n";
  return EXIT_SUCCESS;
}
