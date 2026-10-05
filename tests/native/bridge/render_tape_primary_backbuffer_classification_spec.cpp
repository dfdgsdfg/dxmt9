#include "device_c_render_tape_primary_backbuffer_classification.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using dxmt9::d3d9::renderTapeClassifyPrimaryBackbuffer;

void check(bool value, std::string_view message) {
  if (!value) throw std::runtime_error(std::string(message));
}

struct FakeBacking {
  std::uint32_t width = 1280u;
  std::uint32_t height = 720u;
  std::uint32_t format = 22u;
  std::array<std::uint8_t, 4u> sampleBytes{1u, 2u, 3u, 4u};
  bool alive = true;

  bool valid() const noexcept { return alive; }
};

struct FakeDevice {
  const FakeBacking* primary = nullptr;
  bool terminal = false;
};

struct FakeFacade {
  D9CWireObjectIdentity identity{};
  const FakeDevice* device = nullptr;
  const FakeBacking* backing = nullptr;
};

// Supplies the same retained observation fields as the production entry.
// Registry AddRef mechanics are covered by chunk_record_registry_spec; this
// fake-backend spec binds the physical classification and Reset policy itself.
bool classify(const D9CWireObjectIdentity& requested,
              const FakeDevice* device, const FakeFacade& retained,
              std::uint32_t& out) noexcept {
  return renderTapeClassifyPrimaryBackbuffer(
      requested, retained.identity, device, retained.device, retained.backing,
      device ? device->primary : nullptr, device && device->terminal, out);
}

constexpr D9CWireObjectIdentity kRt0{
    D9C_CHUNK_HANDLE_KIND_SURFACE, 1u, 4294967297ull};
constexpr D9CWireObjectIdentity kPresent{
    D9C_CHUNK_HANDLE_KIND_SURFACE, 4u, 4294968987ull};

void testDistinctFacadesShareTheActualPrimaryBacking() {
  FakeBacking primary;
  const FakeDevice device{&primary};
  const FakeFacade rt0{kRt0, &device, &primary};
  const FakeFacade present{kPresent, &device, &primary};
  std::uint32_t out = 99u;
  check(classify(kRt0, &device, rt0, out) && out == 1u,
        "fresh RT0 facade identifies the actual primary backing");
  out = 99u;
  check(classify(kPresent, &device, present, out) && out == 1u,
        "distinct Present facade independently identifies the same backing");
  check(rt0.identity.objectId != present.identity.objectId &&
            rt0.identity.generation != present.identity.generation,
        "physical classification does not merge logical facade identities");

  out = 1u;
  check(!classify(kRt0, &device, present, out) && out == 0u,
        "shared backing does not authorize resolving another facade identity");
}

void testEqualShapeAndBytesDoNotCreateAnAlias() {
  FakeBacking primary;
  FakeBacking sameShape = primary;
  const FakeDevice device{&primary};
  const FakeFacade nonprimary{kRt0, &device, &sameShape};
  check(primary.width == sameShape.width && primary.height == sameShape.height &&
            primary.format == sameShape.format &&
            primary.sampleBytes == sameShape.sampleBytes && &primary != &sameShape,
        "fixture has indistinguishable shape/content but distinct backing");
  std::uint32_t out = std::numeric_limits<std::uint32_t>::max();
  check(classify(kRt0, &device, nonprimary, out) && out == 0u,
        "valid same-shaped separate backing is known nonprimary");
}

void testExactIdentityAndDeviceFailuresClearTheOutput() {
  FakeBacking primary;
  const FakeDevice device{&primary};
  const FakeDevice foreign{&primary};
  const FakeFacade retained{kRt0, &device, &primary};
  const std::array invalidRequests{
      D9CWireObjectIdentity{D9C_CHUNK_HANDLE_KIND_TEXTURE,
                            kRt0.generation, kRt0.objectId},
      D9CWireObjectIdentity{kRt0.kind, 0u, kRt0.objectId},
      D9CWireObjectIdentity{kRt0.kind, kRt0.generation, 0u},
      D9CWireObjectIdentity{kRt0.kind, kRt0.generation + 1u, kRt0.objectId},
      D9CWireObjectIdentity{kRt0.kind, kRt0.generation, kPresent.objectId},
  };
  for (const auto& request : invalidRequests) {
    std::uint32_t out = 1u;
    check(!classify(request, &device, retained, out) && out == 0u,
          "invalid kind/generation/objectId request cannot expose a primary bit");
  }
  auto recycled = retained;
  ++recycled.identity.generation;
  std::uint32_t out = 1u;
  check(!classify(kRt0, &device, recycled, out) && out == 0u,
        "stale generation cannot classify a reused registry slot");
  out = 1u;
  check(!classify(kRt0, &foreign, retained, out) && out == 0u,
        "foreign device fails even with the identical primary backing pointer");
  out = 1u;
  check(!classify(kRt0, nullptr, retained, out) && out == 0u,
        "missing requested device clears output");
  auto ownerless = retained;
  ownerless.device = nullptr;
  out = 1u;
  check(!classify(kRt0, &device, ownerless, out) && out == 0u,
        "missing facade device clears output");
}

void testMissingInvalidAndTerminalMetadataFailsClosed() {
  FakeBacking primary;
  FakeDevice device{&primary};
  FakeFacade retained{kRt0, &device, &primary};
  std::uint32_t out = 1u;
  device.terminal = true;
  check(!classify(kRt0, &device, retained, out) && out == 0u,
        "terminal replay never returns a classification");
  device.terminal = false;
  device.primary = nullptr;
  out = 1u;
  check(!classify(kRt0, &device, retained, out) && out == 0u,
        "missing current primary is failure, not known nonprimary");
  device.primary = &primary;
  retained.backing = nullptr;
  out = 1u;
  check(!classify(kRt0, &device, retained, out) && out == 0u,
        "missing facade backing fails closed");
  retained.backing = &primary;
  primary.alive = false;
  out = 1u;
  check(!classify(kRt0, &device, retained, out) && out == 0u,
        "matching but invalid backing fails closed");
  FakeBacking liveNonprimary;
  retained.backing = &liveNonprimary;
  out = 1u;
  check(!classify(kRt0, &device, retained, out) && out == 0u,
        "invalid current primary is failure despite a live nonprimary facade");
  primary.alive = true;
  liveNonprimary.alive = false;
  out = 1u;
  check(!classify(kRt0, &device, retained, out) && out == 0u,
        "invalid retained backing is failure despite a live primary");
}

void testPrimaryReplacementIsObservedWithoutAClassificationCache() {
  FakeBacking oldPrimary;
  FakeBacking replacement = oldPrimary;
  FakeDevice device{&oldPrimary};
  const FakeFacade oldFacade{kRt0, &device, &oldPrimary};
  const FakeFacade newFacade{kPresent, &device, &replacement};
  std::uint32_t out = 99u;
  check(classify(kRt0, &device, oldFacade, out) && out == 1u,
        "old facade is primary before replacement");
  device.primary = &replacement;
  out = 1u;
  check(classify(kRt0, &device, oldFacade, out) && out == 0u,
        "retained old backing cannot alias a same-shaped replacement");
  out = 99u;
  check(classify(kPresent, &device, newFacade, out) && out == 1u,
        "new facade matches the current replacement");
  oldPrimary.alive = false;
  out = 1u;
  check(!classify(kRt0, &device, oldFacade, out) && out == 0u,
        "Reset-invalidated retained old backing fails closed");
}

} // namespace

int main() {
  try {
    testDistinctFacadesShareTheActualPrimaryBacking();
    testEqualShapeAndBytesDoNotCreateAnAlias();
    testExactIdentityAndDeviceFailuresClearTheOutput();
    testMissingInvalidAndTerminalMetadataFailsClosed();
    testPrimaryReplacementIsObservedWithoutAClassificationCache();
    std::cout << "Render Tape primary backbuffer classification spec passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "Render Tape primary backbuffer classification spec failed: "
              << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
