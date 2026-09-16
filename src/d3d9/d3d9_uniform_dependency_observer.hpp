#pragma once

#include "dxmt9/core_snapshots.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace dxmt9::d3d9 {

template <typename T>
bool uniformBitsEqual(const T& a, const T& b) noexcept {
  return std::bit_cast<std::array<std::byte, sizeof(T)>>(a) ==
         std::bit_cast<std::array<std::byte, sizeof(T)>>(b);
}

inline bool uniformMaterialEqual(const core::Material& a,
                                 const core::Material& b) noexcept {
  return uniformBitsEqual(a.emissive, b.emissive) &&
         uniformBitsEqual(a.ambient, b.ambient) &&
         uniformBitsEqual(a.diffuse, b.diffuse) &&
         uniformBitsEqual(a.specular, b.specular) &&
         uniformBitsEqual(a.power, b.power);
}

inline bool uniformLightEqual(const core::Light& a,
                              const core::Light& b) noexcept {
  // Ignore padding after Light::enabled; preserve signed zero and NaN bits.
  return a.type == b.type && a.enabled == b.enabled &&
      uniformBitsEqual(a.diffuse, b.diffuse) &&
      uniformBitsEqual(a.specular, b.specular) &&
      uniformBitsEqual(a.ambient, b.ambient) &&
      uniformBitsEqual(a.position, b.position) &&
      uniformBitsEqual(a.direction, b.direction) &&
      uniformBitsEqual(a.range, b.range) &&
      uniformBitsEqual(a.falloff, b.falloff) &&
      uniformBitsEqual(a.attenuation0, b.attenuation0) &&
      uniformBitsEqual(a.attenuation1, b.attenuation1) &&
      uniformBitsEqual(a.attenuation2, b.attenuation2) &&
      uniformBitsEqual(a.theta, b.theta) && uniformBitsEqual(a.phi, b.phi);
}

inline std::array<bool, 5> compareUniformNonconstantValues(
    const core::DrawUniformFixedPayload& before,
    const core::DrawUniformPayload& after) noexcept {
  bool lights = true;
  for (std::size_t i = 0; i < before.lights.size(); ++i)
    lights &= uniformLightEqual(before.lights[i], after.lights[i]);
  return {
      uniformBitsEqual(before.worldViewProj, after.worldViewProj) &&
          uniformBitsEqual(before.ffpView, after.ffpView) &&
          uniformBitsEqual(before.ffpWorldView, after.ffpWorldView) &&
          uniformBitsEqual(before.ffpNormalMatrix, after.ffpNormalMatrix) &&
          uniformBitsEqual(before.ffpBlendWorldViewProj, after.ffpBlendWorldViewProj) &&
          uniformBitsEqual(before.ffpBlendWorldView, after.ffpBlendWorldView) &&
          uniformBitsEqual(before.ffpBlendNormalMatrix, after.ffpBlendNormalMatrix),
      uniformMaterialEqual(before.material, after.material), lights,
      uniformBitsEqual(before.textureTransforms, after.textureTransforms),
      before.clipPlaneMask == after.clipPlaneMask &&
          uniformBitsEqual(before.clipPlanes, after.clipPlanes),
  };
}

inline bool uniformDependencyProbeEnabled() noexcept {
  static const bool enabled = [] {
    const char* value = std::getenv("DXMT9_PERF_UNIFORM_DEPENDENCY_PROBE");
    return value && value[0] != '\0' && std::string_view(value) != "0";
  }();
  return enabled;
}

class UniformDependencyProbe {
 public:
  ~UniformDependencyProbe() {
    constexpr std::array names{"matrices", "material", "lights", "texture", "clip"};
    for (std::size_t i = 0; i < names.size(); ++i) {
      std::fprintf(stderr,
          "[dxmt9-uniform-dependency] category=%s builds=%llu prior=%llu equal=%llu\n",
          names[i], static_cast<unsigned long long>(builds_.load()),
          static_cast<unsigned long long>(prior_.load()),
          static_cast<unsigned long long>(equal_[i].load()));
    }
  }

  void record(bool valid, const core::DrawUniformFixedPayload& before,
              const core::DrawUniformPayload& after) noexcept {
    builds_.fetch_add(1, std::memory_order_relaxed);
    if (!valid) return;
    prior_.fetch_add(1, std::memory_order_relaxed);
    const auto same = compareUniformNonconstantValues(before, after);
    for (std::size_t i = 0; i < same.size(); ++i)
      if (same[i]) equal_[i].fetch_add(1, std::memory_order_relaxed);
  }

 private:
  std::atomic<std::uint64_t> builds_{0}, prior_{0};
  std::array<std::atomic<std::uint64_t>, 5> equal_{};
};

// Only entered behind the opt-in gate. This cold frame owns the prior values;
// disabled replay constructs neither a projection nor observer storage.
template <typename Rebuild>
[[gnu::noinline]] void observeUniformNonconstantRebuild(
    const core::DrawUniformPayload& payload, bool valid, Rebuild&& rebuild) {
  const auto before = core::makeDrawUniformFixedPayload(payload);
  std::forward<Rebuild>(rebuild)();
  static UniformDependencyProbe probe;
  probe.record(valid, before, payload);
}

} // namespace dxmt9::d3d9
