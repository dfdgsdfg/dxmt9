#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dxmt9::pipeline::diagnostics {

// All values are canonical, pre-hashed axes from the authoritative final
// pipeline key. The observer intentionally does not depend on ShaderVariantKey
// or pipeline-cache internals, so it can remain a cold diagnostic boundary.
struct PsoCacheKeyAxes {
  std::uint64_t sourceTupleHash = 0;
  std::uint64_t backendIdentityHash = 0;
  std::uint64_t vertexSourceHash = 0;
  std::uint64_t fragmentSourceHash = 0;
  std::uint64_t tileSourceHash = 0;
  std::uint64_t vsoutShapeHash = 0;
  std::uint64_t textureMask = 0;
  std::uint64_t textureTypesHash = 0;
  std::uint64_t sampledDepthShapeHash = 0;
  std::uint64_t fetch4ShapeHash = 0;
  std::uint64_t x8ShapeHash = 0;
  std::uint64_t sampleCount = 0;
  std::uint64_t colorFormatShapeHash = 0;
  std::uint64_t blendShapeHash = 0;
  std::uint64_t depthStencilShapeHash = 0;
  std::uint64_t modeBits = 0;
};

// Inputs to the opaque pre-source `ShaderVariantKey::hash` composition. These
// are observed at key construction only when the cold diagnostic gate is on;
// they never become fields of the authoritative key or alter equality.
struct PsoBackendIdentityAxes {
  std::uint64_t vertexShaderIdentityHash = 0;
  std::uint64_t pixelShaderIdentityHash = 0;
  std::uint64_t clipPlaneMask = 0;
  std::uint64_t vertexLayoutHash = 0;
  std::uint64_t vertexElementLayoutHash = 0;
  std::uint64_t stream0Offset = 0;
  std::uint64_t extraStreamOffsetsHash = 0;
  std::uint64_t stream0Stride = 0;
  std::uint64_t extraStreamStridesHash = 0;
  std::uint64_t fvf = 0;
  std::uint64_t depthFormat = 0;
  std::uint64_t stencilFormat = 0;
};

class PsoCacheKeyCardinality {
 public:
  static constexpr std::size_t kAxisCount = 16;
  static constexpr std::size_t kBackendAxisCount = 12;
  static constexpr std::size_t kDefaultCapacity = 4096;

  struct Observation {
    std::array<bool, kAxisCount> newAxes{};
    std::array<bool, kAxisCount> overflowedAxes{};
  };

  struct BackendObservation {
    std::array<bool, kBackendAxisCount> newAxes{};
    std::array<bool, kBackendAxisCount> overflowedAxes{};
  };

  explicit PsoCacheKeyCardinality(
      std::size_t capacity = kDefaultCapacity);

  // Returns which axes were newly admitted. Once a bounded set is full, its
  // corresponding bit remains false; this is an intentional conservative
  // under-count rather than an unbounded diagnostic allocation.
  Observation observe(const PsoCacheKeyAxes& axes) noexcept;
  BackendObservation observeBackendIdentity(
      const PsoBackendIdentityAxes& axes) noexcept;

  // Returns the current fanout, or zero if the bounded map is saturated.
  std::uint64_t observeFinalFanout(std::uint64_t sourceTupleHash) noexcept;

 private:
  struct BoundedSet {
    enum class InsertResult : std::uint8_t {
      Inserted,
      Existing,
      Full,
    };
    explicit BoundedSet(std::size_t capacity);
    InsertResult insert(std::uint64_t value) noexcept;

    std::vector<std::uint64_t> values;
    std::vector<std::uint8_t> occupied;
    std::size_t mask = 0;
    bool full = false;
  };

  struct FanoutMap {
    explicit FanoutMap(std::size_t capacity);
    bool increment(std::uint64_t value, std::uint64_t* maximum) noexcept;

    std::vector<std::uint64_t> keys;
    std::vector<std::uint32_t> counts;
    std::vector<std::uint8_t> occupied;
    std::size_t mask = 0;
    bool full = false;
  };

  std::array<BoundedSet, kAxisCount> axes_;
  std::array<BoundedSet, kBackendAxisCount> backendAxes_;
  FanoutMap fanout_;
};

// Process-global adapter used by the production pipeline-cache call sites.
// It creates the bounded state only after both perf gates are enabled.
void observePsoCacheKeyAxes(const PsoCacheKeyAxes& axes) noexcept;
void observePsoBackendIdentityAxes(
    const PsoBackendIdentityAxes& axes) noexcept;
void observePsoCacheFinalFanout(std::uint64_t sourceTupleHash) noexcept;

}  // namespace dxmt9::pipeline::diagnostics
