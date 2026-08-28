#include "../../../src/dxmt9/dxmt9_pso_cache_diagnostics.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using dxmt9::pipeline::diagnostics::PsoCacheKeyAxes;
using dxmt9::pipeline::diagnostics::PsoCacheKeyCardinality;
using dxmt9::pipeline::diagnostics::PsoBackendIdentityAxes;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

PsoCacheKeyAxes axes(std::uint64_t seed) {
  return PsoCacheKeyAxes{
      .sourceTupleHash = seed + 1u,
      .backendIdentityHash = seed + 2u,
      .vertexSourceHash = seed + 3u,
      .fragmentSourceHash = seed + 4u,
      .tileSourceHash = seed + 5u,
      .vsoutShapeHash = seed + 6u,
      .textureMask = seed + 7u,
      .textureTypesHash = seed + 8u,
      .sampledDepthShapeHash = seed + 9u,
      .fetch4ShapeHash = seed + 10u,
      .x8ShapeHash = seed + 11u,
      .sampleCount = seed + 12u,
      .colorFormatShapeHash = seed + 13u,
      .blendShapeHash = seed + 14u,
      .depthStencilShapeHash = seed + 15u,
      .modeBits = seed + 16u,
  };
}

PsoBackendIdentityAxes backendAxes(std::uint64_t seed) {
  return PsoBackendIdentityAxes{
      .vertexShaderIdentityHash = seed + 1u,
      .pixelShaderIdentityHash = seed + 2u,
      .clipPlaneMask = seed + 3u,
      .vertexLayoutHash = seed + 4u,
      .vertexElementLayoutHash = seed + 5u,
      .stream0Offset = seed + 6u,
      .extraStreamOffsetsHash = seed + 7u,
      .stream0Stride = seed + 8u,
      .extraStreamStridesHash = seed + 9u,
      .fvf = seed + 10u,
      .depthFormat = seed + 11u,
      .stencilFormat = seed + 12u,
  };
}

void testAxisCardinalityIsDistinctAndBounded() {
  PsoCacheKeyCardinality cardinality(8u);
  const auto first = cardinality.observe(axes(100u));
  for (bool inserted : first.newAxes) {
    check(inserted, "first observation admits every diagnostic axis");
  }

  const auto duplicate = cardinality.observe(axes(100u));
  for (bool inserted : duplicate.newAxes) {
    check(!inserted, "duplicate observation is not counted twice");
  }

  auto changed = axes(100u);
  changed.blendShapeHash = 900u;
  const auto delta = cardinality.observe(changed);
  check(!delta.newAxes[0], "unchanged source tuple remains canonical");
  check(delta.newAxes[13], "changed blend shape is a new axis value");
  check(!delta.newAxes[12], "unchanged color shape remains canonical");

  // A requested capacity of two is a genuinely bounded two-entry table. The
  // third value is intentionally under-counted instead of growing storage.
  PsoCacheKeyCardinality tiny(2u);
  check(tiny.observe(axes(1u)).newAxes[0], "bounded table admits first value");
  check(tiny.observe(axes(2u)).newAxes[0], "bounded table admits second value");
  const auto overflow = tiny.observe(axes(3u));
  check(!overflow.newAxes[0],
        "bounded table rejects values after saturation");
  check(overflow.overflowedAxes[0],
        "bounded table exposes saturation instead of hiding it");
  const auto latchedOverflow = tiny.observe(axes(4u));
  check(!latchedOverflow.newAxes[0] && latchedOverflow.overflowedAxes[0],
        "saturation remains fail-closed without reopening the table");
}

void testFanoutCountsPerSourceTuple() {
  PsoCacheKeyCardinality cardinality(8u);
  check(cardinality.observeFinalFanout(77u) == 1u,
        "first final key has fanout one");
  check(cardinality.observeFinalFanout(77u) == 2u,
        "same source tuple increments fanout");
  check(cardinality.observeFinalFanout(88u) == 1u,
        "different source tuple starts independent fanout");
  check(cardinality.observeFinalFanout(77u) == 3u,
        "fanout remains monotonic for the original tuple");

  PsoCacheKeyCardinality tiny(2u);
  check(tiny.observeFinalFanout(1u) == 1u, "fanout table admits first tuple");
  check(tiny.observeFinalFanout(2u) == 1u, "fanout table admits second tuple");
  check(tiny.observeFinalFanout(3u) == 0u, "fanout saturation is visible");
  check(tiny.observeFinalFanout(1u) == 0u,
        "saturated fanout table remains conservatively fail-closed");
}

void testBackendIdentityAxesAreIndependentAndBounded() {
  PsoCacheKeyCardinality cardinality(4u);
  const auto first = cardinality.observeBackendIdentity(backendAxes(10u));
  for (bool inserted : first.newAxes) {
    check(inserted, "first backend identity admits every component axis");
  }

  const auto duplicate = cardinality.observeBackendIdentity(backendAxes(10u));
  for (bool inserted : duplicate.newAxes) {
    check(!inserted, "duplicate backend component is not counted twice");
  }

  auto changed = backendAxes(10u);
  changed.pixelShaderIdentityHash = 900u;
  const auto delta = cardinality.observeBackendIdentity(changed);
  check(!delta.newAxes[0], "unchanged vertex identity remains canonical");
  check(delta.newAxes[1], "pixel identity is independently observable");
  check(!delta.newAxes[3], "unchanged layout remains canonical");

  changed = backendAxes(10u);
  changed.stream0Offset = 901u;
  const auto streamDelta = cardinality.observeBackendIdentity(changed);
  check(streamDelta.newAxes[5],
        "runtime stream-zero offset is independently observable");
  check(!streamDelta.newAxes[4],
        "stream offset does not alter declaration-element identity");
  check(!streamDelta.newAxes[7],
        "stream offset does not alter stream-zero stride identity");

  PsoCacheKeyCardinality tiny(2u);
  check(tiny.observeBackendIdentity(backendAxes(1u)).newAxes[0],
        "bounded backend table admits first value");
  check(tiny.observeBackendIdentity(backendAxes(2u)).newAxes[0],
        "bounded backend table admits second value");
  const auto overflow = tiny.observeBackendIdentity(backendAxes(3u));
  check(!overflow.newAxes[0] && overflow.overflowedAxes[0],
        "backend identity saturation remains visible and fail-closed");
}

}  // namespace

int main() {
  try {
    testAxisCardinalityIsDistinctAndBounded();
    testFanoutCountsPerSourceTuple();
    testBackendIdentityAxesAreIndependentAndBounded();
  } catch (const TestFailure& failure) {
    std::cerr << "pso_cache_diagnostics_spec failed: " << failure.what()
              << '\n';
    return 1;
  } catch (const std::exception& exception) {
    std::cerr << "pso_cache_diagnostics_spec unexpected exception: "
              << exception.what() << '\n';
    return 1;
  }
  std::cout << "pso_cache_diagnostics_spec passed\n";
  return 0;
}
