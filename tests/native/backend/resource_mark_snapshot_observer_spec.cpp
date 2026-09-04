#include "../../../src/d3d9/d3d9_snapshot_miss_observer.hpp"
#include "../../../src/dxmt9/dxmt9_resource_mark_observer.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool value, std::string_view message) {
  if (!value) {
    throw TestFailure(std::string(message));
  }
}

using dxmt9::core::ChunkHandleKind;
using dxmt9::core::FlatDrawStateKey;
using dxmt9::d3d9::SnapshotMissUniformGenerations;
using dxmt9::resources::ResourceMarkObservationKey;
using dxmt9::resources::ResourceMarkObservationPhase;
using dxmt9::resources::ResourceMarkOverlapLedger;

ResourceMarkObservationKey key(
    ChunkHandleKind kind = ChunkHandleKind::Buffer,
    std::uint64_t handle = 9,
    std::uint64_t bufferBacking = 0) {
  return {.kind = kind, .handle = handle, .bufferBacking = bufferBacking};
}

void resourceMarkLedgerTracksCoverageByWaterline() {
  auto delayedIngress = std::make_unique<ResourceMarkOverlapLedger>();
  check(delayedIngress
            ->observe(ResourceMarkObservationPhase::PublishScan,
                     key(ChunkHandleKind::Surface, 77), 90)
            .noIngress,
        "publish can arrive before its ingress observation");
  check(delayedIngress
            ->observe(ResourceMarkObservationPhase::Ingress,
                     key(ChunkHandleKind::Surface, 77), 100)
            .recorded,
        "later ingress advances the resource waterline");
  check(delayedIngress
            ->observe(ResourceMarkObservationPhase::PublishScan,
                     key(ChunkHandleKind::Surface, 77), 90)
            .covered,
        "later ingress covers an earlier publish even when observed later");

  auto ledger = std::make_unique<ResourceMarkOverlapLedger>();
  check(ledger->observe(ResourceMarkObservationPhase::Ingress, key(), 100)
            .newIdentity,
        "first ingress identity is new");
  check(ledger->observe(ResourceMarkObservationPhase::Ingress, key(), 100)
            .samePhaseDuplicate,
        "same ingress sequence is a duplicate");

  const auto coveredEarlier = ledger->observe(
      ResourceMarkObservationPhase::PublishScan, key(), 90);
  check(coveredEarlier.covered && !coveredEarlier.stale,
        "later ingress covers an earlier publish");
  const auto coveredEqual = ledger->observe(
      ResourceMarkObservationPhase::PublishScan, key(), 100);
  check(coveredEqual.covered && !coveredEqual.samePhaseDuplicate,
        "ingress at the publish waterline is covered");
  const auto repeatedEqual = ledger->observe(
      ResourceMarkObservationPhase::PublishScan, key(), 100);
  check(repeatedEqual.covered && repeatedEqual.samePhaseDuplicate,
        "repeated publish sequence is a duplicate");

  const auto stale = ledger->observe(ResourceMarkObservationPhase::PublishScan,
                                     key(), 101);
  check(stale.stale && !stale.covered,
        "older ingress does not cover a newer publish");

  const auto noIngress = ledger->observe(
      ResourceMarkObservationPhase::PublishScan,
      key(ChunkHandleKind::Texture, 9), 1);
  check(noIngress.noIngress && !noIngress.covered,
        "publish without ingress is not claimed covered");

  const auto snapshot = ledger->snapshot();
  check(snapshot.ingressEntries == 2 && snapshot.publishEntries == 5,
        "phase entry totals are exact");
  check(snapshot.uniqueIdentities == 2 && snapshot.ingressDuplicates == 1 &&
            snapshot.publishDuplicates == 1 && snapshot.publishCovered == 3 &&
            snapshot.publishStale == 1 && snapshot.publishNoIngress == 1,
        "coverage classifications conserve");

  auto backingLedger = std::make_unique<ResourceMarkOverlapLedger>();
  check(backingLedger
            ->observe(ResourceMarkObservationPhase::Ingress,
                     key(ChunkHandleKind::Buffer, 55), 12)
            .recorded,
        "logical buffer ingress is observed");
  check(backingLedger
            ->observe(ResourceMarkObservationPhase::PublishScan,
                     key(ChunkHandleKind::Buffer, 55, 0xabc), 12)
            .noIngress,
        "logical ingress cannot prove a captured backing stamp");
  check(backingLedger
            ->observe(ResourceMarkObservationPhase::Ingress,
                     key(ChunkHandleKind::Buffer, 55, 0xabc), 12)
            .recorded &&
            backingLedger
                ->observe(ResourceMarkObservationPhase::PublishScan,
                         key(ChunkHandleKind::Buffer, 55, 0xabc), 12)
                .covered,
        "matching captured backing ingress proves publish coverage");
}

void resourceMarkLedgerProbesCollisionsWithoutFalseCoverage() {
  auto ledger = std::make_unique<ResourceMarkOverlapLedger>();
  const auto first = key(ChunkHandleKind::Buffer, 100);
  const auto firstIndex = ResourceMarkOverlapLedger::hashIndexForTest(first);
  ResourceMarkObservationKey second = first;
  for (std::uint64_t handle = 101; handle != 0; ++handle) {
    second.handle = handle;
    if (ResourceMarkOverlapLedger::hashIndexForTest(second) == firstIndex) {
      break;
    }
  }
  check(second.handle != first.handle &&
            ResourceMarkOverlapLedger::hashIndexForTest(second) == firstIndex,
        "test found a hash collision");
  check(ledger->observe(ResourceMarkObservationPhase::Ingress, first, 10)
            .newIdentity,
        "first colliding identity is recorded");
  const auto inserted = ledger->observe(ResourceMarkObservationPhase::Ingress,
                                        second, 20);
  check(inserted.newIdentity && inserted.collisionProbes != 0,
        "open addressing records the collision");
  check(ledger->observe(ResourceMarkObservationPhase::PublishScan, first, 10)
            .covered,
        "collision does not alter first identity coverage");
  check(ledger->observe(ResourceMarkObservationPhase::PublishScan, second, 20)
            .covered,
        "collision does not alter second identity coverage");
  check(ledger->snapshot().collisionProbes >= inserted.collisionProbes,
        "collision probe count is observable");
}

void resourceMarkLedgerReportsFullTable() {
  auto ledger = std::make_unique<ResourceMarkOverlapLedger>();
  for (std::size_t i = 0; i < ResourceMarkOverlapLedger::kCapacity; ++i) {
    check(ledger->observe(ResourceMarkObservationPhase::Ingress,
                          key(ChunkHandleKind::Buffer, 1 + i), 1 + i)
              .newIdentity,
          "capacity entries remain unique");
  }
  const auto overflow = ledger->observe(
      ResourceMarkObservationPhase::PublishScan,
      key(ChunkHandleKind::Buffer,
          1 + ResourceMarkOverlapLedger::kCapacity),
      999999);
  check(overflow.overflow && !overflow.covered,
        "full table reports overflow without claiming coverage");
  check(ledger->snapshot().overflow == 1, "table overflow is observable");
}

void snapshotMissObserverSeparatesSemanticDimensions() {
  dxmt9::core::DrawShaderLayoutContext layout;
  FlatDrawStateKey keyValue;
  const SnapshotMissUniformGenerations generations{
      .aggregate = 2, .vertexConstants = 3, .pixelConstants = 4};
  const auto compare = [&](const auto& previousLayout, const auto& currentLayout,
                           const auto& previousKey, const auto& currentKey,
                           auto previousGenerations, auto currentGenerations,
                           auto previousPayload, auto currentPayload) {
    return dxmt9::d3d9::compareSnapshotMissSemantics(
        previousLayout, currentLayout, previousKey, currentKey,
        previousGenerations, currentGenerations, previousPayload, currentPayload);
  };
  check(compare(layout, layout, keyValue, keyValue, generations, generations, 7, 7)
            .sameSemantic(),
        "unchanged snapshot is same semantic");
  auto changedResource = keyValue;
  changedResource.textures[0] = dxmt9::core::Handle{42};
  const auto resource = compare(layout, layout, keyValue, changedResource,
                                generations, generations, 7, 7);
  check(resource.resourceIdentityChanged && !resource.shaderLayoutChanged &&
            !resource.sameSemantic(),
        "texture handle change is resource-only");
  auto changedGeneration = generations;
  ++changedGeneration.vertexConstants;
  const auto generation = compare(layout, layout, keyValue, keyValue,
                                  generations, changedGeneration, 7, 7);
  check(generation.uniformGenerationChanged && !generation.uniformPayloadChanged &&
            generation.sameSemantic(),
        "constant generation churn is separate from semantic payload");
  const auto payload = compare(layout, layout, keyValue, keyValue, generations,
                               generations, 7, 8);
  check(payload.uniformPayloadChanged && !payload.resourceIdentityChanged &&
            !payload.sameSemantic(),
        "uniform payload change is separate from resources");
  auto changedLayout = layout;
  changedLayout.clipPlaneMask = 1;
  const auto shader = compare(layout, changedLayout, keyValue, keyValue,
                              generations, generations, 7, 7);
  check(shader.shaderLayoutChanged && !shader.resourceIdentityChanged &&
            !shader.sameSemantic(),
        "shader layout change is separate from resources");
}
}  // namespace

int main() {
  try {
    resourceMarkLedgerTracksCoverageByWaterline();
    resourceMarkLedgerProbesCollisionsWithoutFalseCoverage();
    resourceMarkLedgerReportsFullTable();
    snapshotMissObserverSeparatesSemanticDimensions();
  } catch (const TestFailure& failure) {
    std::cerr << "resource_mark_snapshot_observer_spec: " << failure.what()
              << '\n';
    return 1;
  }
  std::cout << "resource_mark_snapshot_observer_spec: ok\n";
  return 0;
}
