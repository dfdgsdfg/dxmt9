#include "dxmt9_pso_cache_diagnostics.hpp"

#include "dxmt9_perf_counters.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <new>

namespace dxmt9::pipeline::diagnostics {
namespace {

std::size_t tableCapacity(std::size_t requested) noexcept {
  const auto bounded = std::max<std::size_t>(requested, 2u);
  std::size_t capacity = 1u;
  while (capacity < bounded && capacity <= (static_cast<std::size_t>(-1) / 2u)) {
    capacity *= 2u;
  }
  return capacity;
}

std::size_t mix(std::uint64_t value) noexcept {
  value ^= value >> 30u;
  value *= 0xbf58476d1ce4e5b9ull;
  value ^= value >> 27u;
  value *= 0x94d049bb133111ebull;
  value ^= value >> 31u;
  return static_cast<std::size_t>(value);
}

std::array<std::uint64_t, PsoCacheKeyCardinality::kAxisCount> axisValues(
    const PsoCacheKeyAxes& axes) noexcept {
  return {
      axes.sourceTupleHash,
      axes.backendIdentityHash,
      axes.vertexSourceHash,
      axes.fragmentSourceHash,
      axes.tileSourceHash,
      axes.vsoutShapeHash,
      axes.textureMask,
      axes.textureTypesHash,
      axes.sampledDepthShapeHash,
      axes.fetch4ShapeHash,
      axes.x8ShapeHash,
      axes.sampleCount,
      axes.colorFormatShapeHash,
      axes.blendShapeHash,
      axes.depthStencilShapeHash,
      axes.modeBits,
  };
}

std::mutex gMutex;
std::unique_ptr<PsoCacheKeyCardinality> gState;

PsoCacheKeyCardinality* state() noexcept {
  if (gState) {
    return gState.get();
  }
  try {
    gState = std::make_unique<PsoCacheKeyCardinality>();
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
  return gState.get();
}

}  // namespace

PsoCacheKeyCardinality::BoundedSet::BoundedSet(
    std::size_t capacity)
    : values(tableCapacity(capacity), 0u),
      occupied(tableCapacity(capacity), 0u),
      mask(tableCapacity(capacity) - 1u) {}

PsoCacheKeyCardinality::BoundedSet::InsertResult
PsoCacheKeyCardinality::BoundedSet::insert(
    std::uint64_t value) noexcept {
  if (full) {
    return InsertResult::Full;
  }
  const auto start = mix(value) & mask;
  for (std::size_t probe = 0; probe <= mask; ++probe) {
    const auto index = (start + probe) & mask;
    if (!occupied[index]) {
      occupied[index] = 1u;
      values[index] = value;
      return InsertResult::Inserted;
    }
    if (values[index] == value) {
      return InsertResult::Existing;
    }
  }
  full = true;
  return InsertResult::Full;
}

PsoCacheKeyCardinality::FanoutMap::FanoutMap(
    std::size_t capacity)
    : keys(tableCapacity(capacity), 0u),
      counts(tableCapacity(capacity), 0u),
      occupied(tableCapacity(capacity), 0u),
      mask(tableCapacity(capacity) - 1u) {}

bool PsoCacheKeyCardinality::FanoutMap::increment(
    std::uint64_t value, std::uint64_t* maximum) noexcept {
  if (full) {
    return false;
  }
  const auto start = mix(value) & mask;
  for (std::size_t probe = 0; probe <= mask; ++probe) {
    const auto index = (start + probe) & mask;
    if (!occupied[index]) {
      occupied[index] = 1u;
      keys[index] = value;
      counts[index] = 1u;
      *maximum = 1u;
      return true;
    }
    if (keys[index] == value) {
      if (counts[index] != UINT32_MAX) {
        ++counts[index];
      }
      *maximum = counts[index];
      return true;
    }
  }
  full = true;
  return false;
}

PsoCacheKeyCardinality::PsoCacheKeyCardinality(
    std::size_t capacity)
    : axes_{BoundedSet(capacity), BoundedSet(capacity), BoundedSet(capacity),
            BoundedSet(capacity), BoundedSet(capacity), BoundedSet(capacity),
            BoundedSet(capacity), BoundedSet(capacity), BoundedSet(capacity),
            BoundedSet(capacity), BoundedSet(capacity), BoundedSet(capacity),
            BoundedSet(capacity), BoundedSet(capacity), BoundedSet(capacity),
            BoundedSet(capacity)},
      fanout_(capacity) {}

PsoCacheKeyCardinality::Observation PsoCacheKeyCardinality::observe(
    const PsoCacheKeyAxes& axes) noexcept {
  Observation observation;
  const auto values = axisValues(axes);
  for (std::size_t i = 0; i < values.size(); ++i) {
    const auto result = axes_[i].insert(values[i]);
    observation.newAxes[i] =
        result == BoundedSet::InsertResult::Inserted;
    observation.overflowedAxes[i] =
        result == BoundedSet::InsertResult::Full;
  }
  return observation;
}

std::uint64_t PsoCacheKeyCardinality::observeFinalFanout(
    std::uint64_t sourceTupleHash) noexcept {
  std::uint64_t maximum = 0;
  return fanout_.increment(sourceTupleHash, &maximum) ? maximum : 0u;
}

void observePsoCacheKeyAxes(const PsoCacheKeyAxes& axes) noexcept {
  if (!perf::psoCacheDiagnosticsEnabled()) {
    return;
  }
  std::lock_guard lock(gMutex);
  auto* cardinality = state();
  if (!cardinality) {
    return;
  }
  const auto observation = cardinality->observe(axes);
  for (std::size_t i = 0; i < observation.newAxes.size(); ++i) {
    if (observation.overflowedAxes[i]) {
      perf::recordPsoCacheDiagnosticTrackerOverflow();
    }
    if (observation.newAxes[i]) {
      perf::recordPsoCacheDistinctKeyAxis(
          static_cast<perf::PsoCacheKeyAxis>(i));
    }
  }
}

void observePsoCacheFinalFanout(std::uint64_t sourceTupleHash) noexcept {
  if (!perf::psoCacheDiagnosticsEnabled()) {
    return;
  }
  std::lock_guard lock(gMutex);
  auto* cardinality = state();
  const auto fanout = cardinality
                          ? cardinality->observeFinalFanout(sourceTupleHash)
                          : 0u;
  if (fanout == 0u) {
    perf::recordPsoCacheDiagnosticTrackerOverflow();
    return;
  }
  // The perf sink retains the maximum over all source tuples.
  perf::recordPsoCacheFinalFanout(fanout);
}

}  // namespace dxmt9::pipeline::diagnostics
